#include "audio_engine.h"
#include "buffer_plan.h"
#include "ks_device_access.h"

#include <windows.h>
#include <winioctl.h>
#include <avrt.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr DWORD kWorkerHandshakeMs = 3000;
constexpr unsigned kMaximumChannelsPerEndpoint = 64;
constexpr unsigned kMaximumRingFrames = 32768;

enum class WorkerCommand : LONG { none, start, stop, shutdown };
enum class SampleEncoding { float32, pcm32, pcm24, pcm16 };

std::string narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) return "audio pin";
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), size, nullptr, nullptr);
  return result;
}

std::string hresult_text(HRESULT hr) {
  char message[32]{};
  sprintf_s(message, "0x%08lX", static_cast<unsigned long>(hr));
  return message;
}

float decode_sample(const BYTE* data, SampleEncoding encoding) noexcept {
  switch (encoding) {
    case SampleEncoding::float32: {
      float value{};
      std::memcpy(&value, data, sizeof(value));
      return value;
    }
    case SampleEncoding::pcm32: {
      int32_t value{};
      std::memcpy(&value, data, sizeof(value));
      return static_cast<float>(static_cast<double>(value) / 2147483648.0);
    }
    case SampleEncoding::pcm24: {
      int32_t value = static_cast<int32_t>(data[0]) |
                      (static_cast<int32_t>(data[1]) << 8) |
                      (static_cast<int32_t>(data[2]) << 16);
      if (value & 0x00800000) value |= static_cast<int32_t>(0xff000000);
      return static_cast<float>(static_cast<double>(value) / 8388608.0);
    }
    case SampleEncoding::pcm16: {
      int16_t value{};
      std::memcpy(&value, data, sizeof(value));
      return static_cast<float>(value / 32768.0);
    }
  }
  return 0.0f;
}

void encode_sample(BYTE* data, SampleEncoding encoding, float sample) noexcept {
  const double value = std::clamp(static_cast<double>(sample), -1.0, 1.0);
  switch (encoding) {
    case SampleEncoding::float32:
      std::memcpy(data, &sample, sizeof(sample));
      break;
    case SampleEncoding::pcm32: {
      const auto quantized = static_cast<int32_t>(std::clamp<long long>(
          std::llround(value * 2147483648.0), -2147483648LL, 2147483647LL));
      std::memcpy(data, &quantized, sizeof(quantized));
      break;
    }
    case SampleEncoding::pcm24: {
      const auto quantized = static_cast<int32_t>(std::clamp<long long>(
          std::llround(value * 8388608.0), -8388608LL, 8388607LL));
      data[0] = static_cast<BYTE>(quantized & 0xff);
      data[1] = static_cast<BYTE>((quantized >> 8) & 0xff);
      data[2] = static_cast<BYTE>((quantized >> 16) & 0xff);
      break;
    }
    case SampleEncoding::pcm16: {
      const auto quantized = static_cast<int16_t>(std::clamp<long>(
          std::lround(value * 32768.0), -32768L, 32767L));
      std::memcpy(data, &quantized, sizeof(quantized));
      break;
    }
  }
}

unsigned next_power_of_two(unsigned value) {
  unsigned result = 1;
  while (result < value && result < kMaximumRingFrames) result <<= 1;
  return result;
}

class AudioRing {
 public:
  AudioRing(unsigned channels, unsigned capacity_frames)
      : channels_(channels), capacity_(next_power_of_two(capacity_frames)),
        mask_(capacity_ - 1), samples_(static_cast<size_t>(capacity_) * channels, 0.0f),
        last_samples_(channels, 0.0f) {}

  uint64_t fill_frames() const noexcept {
    const auto write = write_frame_.load(std::memory_order_acquire);
    const auto read = read_frame_.load(std::memory_order_acquire);
    return write >= read ? std::min<uint64_t>(write - read, capacity_) : 0;
  }

  // Called only while the ASIO producer and this stream are stopped.
  void reset() noexcept {
    read_position_ = 0.0;
    splice_remaining_ = 0;
    has_read_samples_ = false;
    std::fill(last_samples_.begin(), last_samples_.end(), 0.0f);
    read_frame_.store(0, std::memory_order_relaxed);
    write_frame_.store(0, std::memory_order_relaxed);
  }

  // The consumer alone moves the read cursor. A callback or device stall
  // must not leave hundreds of milliseconds queued behind the live stream.
  uint64_t trim_backlog(unsigned limit, unsigned target) noexcept {
    const auto write = write_frame_.load(std::memory_order_acquire);
    const auto read = read_frame_.load(std::memory_order_relaxed);
    if (write <= read || write - read <= limit) return 0;
    const uint64_t keep = std::min<uint64_t>(target, write - read);
    const uint64_t next = write - keep;
    const uint64_t skipped = next - read;
    const double fraction = read_position_ - std::floor(read_position_);
    read_position_ = static_cast<double>(next) + fraction;
    splice_remaining_ = has_read_samples_ ? 32 : 0;
    // Keep the old read cursor published until read_resampled finishes its
    // short fade; the producer cannot overwrite retained history meanwhile.
    return skipped;
  }

  unsigned push(const float* source, unsigned frames) noexcept {
    if (!source || !frames) return 0;
    const uint64_t write = write_frame_.load(std::memory_order_relaxed);
    const uint64_t read = read_frame_.load(std::memory_order_acquire);
    const auto used = write >= read ? write - read : capacity_;
    const auto accepted = static_cast<unsigned>(std::min<uint64_t>(frames,
        used >= capacity_ ? 0 : capacity_ - used));
    for (unsigned frame = 0; frame < accepted; ++frame) {
      const size_t target = static_cast<size_t>((write + frame) & mask_) * channels_;
      std::memcpy(samples_.data() + target,
                  source + static_cast<size_t>(frame) * channels_,
                  static_cast<size_t>(channels_) * sizeof(float));
    }
    write_frame_.store(write + accepted, std::memory_order_release);
    return accepted;
  }

  void push_silence(unsigned frames) noexcept {
    if (!frames) return;
    const uint64_t write = write_frame_.load(std::memory_order_relaxed);
    const uint64_t read = read_frame_.load(std::memory_order_acquire);
    const auto used = write >= read ? write - read : capacity_;
    const auto accepted = static_cast<unsigned>(std::min<uint64_t>(frames,
        used >= capacity_ ? 0 : capacity_ - used));
    for (unsigned frame = 0; frame < accepted; ++frame) {
      const size_t target = static_cast<size_t>((write + frame) & mask_) * channels_;
      std::fill_n(samples_.data() + target, channels_, 0.0f);
    }
    write_frame_.store(write + accepted, std::memory_order_release);
  }

  unsigned read_resampled(float* destination, unsigned frames, unsigned target_fill,
                      double base_ratio = 1.0) noexcept {
    if (!destination || !frames) return 0;
    const uint64_t write = write_frame_.load(std::memory_order_acquire);
    const uint64_t read = static_cast<uint64_t>(read_position_);
    const uint64_t fill = write >= read ? std::min<uint64_t>(write - read, capacity_) : 0;
    const double error = static_cast<double>(fill) - target_fill;
    const double servo = std::clamp(error * 0.00000075, -0.0015, 0.0015);
    const double ratio = base_ratio * (1.0 + servo);
    unsigned missing = 0;
    for (unsigned frame = 0; frame < frames; ++frame) {
      const double position = read_position_ + static_cast<double>(frame) * ratio;
      const uint64_t first_frame = static_cast<uint64_t>(position);
      const float fraction = static_cast<float>(position - first_frame);
      const bool first_ready = first_frame < write;
      if (!first_ready) ++missing;
      const bool second_ready = first_frame + 1 < write;
      const size_t first = static_cast<size_t>(first_frame & mask_) * channels_;
      const size_t second = static_cast<size_t>((first_frame + 1) & mask_) * channels_;
      for (unsigned channel = 0; channel < channels_; ++channel) {
        const float a = first_ready ? samples_[first + channel] : 0.0f;
        const float b = second_ready ? samples_[second + channel] : a;
        float value = a + (b - a) * fraction;
        if (splice_remaining_) {
          const float fade = (33.0f - splice_remaining_) / 32.0f;
          value = last_samples_[channel] + (value - last_samples_[channel]) * fade;
        }
        destination[static_cast<size_t>(frame) * channels_ + channel] = value;
        if (!splice_remaining_ || splice_remaining_ == 1) last_samples_[channel] = value;
      }
      if (splice_remaining_) --splice_remaining_;
    }
    const double desired = read_position_ + static_cast<double>(frames) * ratio;
    const uint64_t consumed = std::min<uint64_t>(static_cast<uint64_t>(desired), write);
    // Keep the fractional phase across blocks. Rounding it away repeats or
    // skips a sample at every block boundary and defeats clock correction.
    read_position_ = std::min(desired, static_cast<double>(write));
    read_frame_.store(consumed, std::memory_order_release);
    has_read_samples_ = true;
    return missing;
  }

 private:
  const unsigned channels_;
  const unsigned capacity_;
  const unsigned mask_;
  std::vector<float> samples_;
  std::vector<float> last_samples_;
  alignas(64) std::atomic<uint64_t> write_frame_{0};
  alignas(64) std::atomic<uint64_t> read_frame_{0};
  double read_position_{};
  unsigned splice_remaining_{};
  bool has_read_samples_{};
};

struct FormatCandidate {
  SampleEncoding encoding;
  unsigned bits;
  bool floating_point;
};

constexpr std::array<FormatCandidate, 4> kFormatCandidates{{
    {SampleEncoding::float32, 32, true},
    {SampleEncoding::pcm32, 32, false},
    {SampleEncoding::pcm24, 24, false},
    {SampleEncoding::pcm16, 16, false}}};

std::vector<unsigned> ordered_rates(const AudioEndpoint& endpoint, unsigned requested) {
  auto rates = endpoint.native_sample_rates;
  if (rates.empty()) rates.push_back(requested);
  if (std::find(rates.begin(), rates.end(), requested) == rates.end() &&
      requested >= endpoint.minimum_sample_rate && requested <= endpoint.maximum_sample_rate) {
    rates.push_back(requested);
  }
  std::stable_sort(rates.begin(), rates.end(), [requested](unsigned a, unsigned b) {
    const auto da = a > requested ? a - requested : requested - a;
    const auto db = b > requested ? b - requested : requested - b;
    return da < db;
  });
  rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
  return rates;
}

std::vector<unsigned> channel_candidates(unsigned maximum) {
  maximum = std::clamp(maximum, 1u, kMaximumChannelsPerEndpoint);
  std::vector<unsigned> channels{maximum};
  constexpr std::array<unsigned, 10> common_counts{32, 24, 16, 12, 10, 8, 6, 4, 2, 1};
  for (const auto count : common_counts) {
    if (count <= maximum && std::find(channels.begin(), channels.end(), count) == channels.end()) {
      channels.push_back(count);
    }
  }
  return channels;
}

std::vector<BYTE> make_connection(unsigned pin_id, const GUID& interface_set,
                                  ULONG interface_id, const GUID& medium_set,
                                  ULONG medium_id, unsigned channels, unsigned rate,
                                  const FormatCandidate& candidate, bool extensible) {
  const size_t wave_size = extensible ? sizeof(WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEX);
  std::vector<BYTE> bytes(sizeof(KSPIN_CONNECT) + sizeof(KSDATAFORMAT) + wave_size, 0);
  auto* connect = reinterpret_cast<KSPIN_CONNECT*>(bytes.data());
  connect->Interface.Set = interface_set;
  connect->Interface.Id = interface_id;
  connect->Medium.Set = medium_set;
  connect->Medium.Id = medium_id;
  connect->PinId = pin_id;
  connect->PinToHandle = nullptr;
  connect->Priority.PriorityClass = KSPRIORITY_NORMAL;
  connect->Priority.PrioritySubClass = 1;

  auto* data_format = reinterpret_cast<KSDATAFORMAT*>(bytes.data() + sizeof(KSPIN_CONNECT));
  const size_t format_size = sizeof(KSDATAFORMAT) + wave_size;
  data_format->FormatSize = static_cast<ULONG>(format_size);
  data_format->Flags = 0;
  data_format->SampleSize = channels * candidate.bits / 8;
  data_format->Reserved = 0;
  data_format->MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
  data_format->SubFormat = candidate.floating_point ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                                                    : KSDATAFORMAT_SUBTYPE_PCM;
  data_format->Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

  auto* wave = reinterpret_cast<WAVEFORMATEX*>(data_format + 1);
  wave->wFormatTag = extensible ? WAVE_FORMAT_EXTENSIBLE
      : (candidate.floating_point ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM);
  wave->nChannels = static_cast<WORD>(channels);
  wave->nSamplesPerSec = rate;
  wave->wBitsPerSample = static_cast<WORD>(candidate.bits);
  wave->nBlockAlign = static_cast<WORD>(channels * candidate.bits / 8);
  wave->nAvgBytesPerSec = rate * wave->nBlockAlign;
  wave->cbSize = extensible ? sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) : 0;
  if (extensible) {
    auto* extended = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wave);
    extended->Samples.wValidBitsPerSample = static_cast<WORD>(candidate.bits);
    extended->dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER
        : channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                        : KSAUDIO_SPEAKER_DIRECTOUT;
    extended->SubFormat = candidate.floating_point ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                                                   : KSDATAFORMAT_SUBTYPE_PCM;
  }
  return bytes;
}

HRESULT set_pin_state(HANDLE pin, KSSTATE state) noexcept {
  KSPROPERTY property{};
  property.Set = KSPROPSETID_Connection;
  property.Id = KSPROPERTY_CONNECTION_STATE;
  property.Flags = KSPROPERTY_TYPE_SET;
  ULONG returned = 0;
  if (!DeviceIoControl(pin, IOCTL_KS_PROPERTY, &property, sizeof(property),
                       &state, sizeof(state), &returned, nullptr)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  return S_OK;
}

} // namespace

class EndpointAudioStream::Impl {
 public:
  Impl(AudioEndpoint endpoint, unsigned sample_rate, unsigned buffer_frames)
      : endpoint_(std::move(endpoint)), sample_rate_(sample_rate), buffer_frames_(buffer_frames),
        asio_sample_rate_(sample_rate), asio_buffer_frames_(buffer_frames) {
    command_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    acknowledge_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }

  ~Impl() { shutdown(); }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  bool initialize(std::string& error) {
    if (!command_event_ || !acknowledge_event_ || !ready_event_) {
      error = "Could not create the Kernel Streaming worker events.";
      return false;
    }
    HRESULT hr = pin_reservation_.acquire(endpoint_.id);
    if (SUCCEEDED(hr)) hr = open_pin();
    if (FAILED(hr)) {
      last_error_.store(hr, std::memory_order_release);
      error = ks_device_access::is_busy(hr)
          ? "Device is in use: " + narrow(endpoint_.name) + ". Release it and try again."
          : "Could not create a KS pin for " + narrow(endpoint_.name) +
              " at a supported rate, channel count, and PCM format (" + hresult_text(hr) + ").";
      return false;
    }
    thread_ = std::thread([this] { run(); });
    return true;
  }

  bool wait_initialized(std::string& error) {
    if (WaitForSingleObject(ready_event_, kWorkerHandshakeMs) != WAIT_OBJECT_0) {
      error = "Timed out starting the KS worker for " + narrow(endpoint_.name) + ".";
      return false;
    }
    const HRESULT hr = init_result_.load(std::memory_order_acquire);
    if (FAILED(hr)) {
      error = "Could not initialize the KS worker for " + narrow(endpoint_.name) +
              " (" + hresult_text(hr) + ").";
      return false;
    }
    return true;
  }

  bool request_start() noexcept {
    return post_command(WorkerCommand::start);
  }
  bool wait_started(unsigned timeout_ms, std::string& error) noexcept {
    if (WaitForSingleObject(acknowledge_event_, timeout_ms) != WAIT_OBJECT_0) {
      error = "Timed out starting " + narrow(endpoint_.name) + ".";
      return false;
    }
    const HRESULT hr = command_result_.load(std::memory_order_acquire);
    if (FAILED(hr)) {
      error = "Could not start KS pin " + narrow(endpoint_.name) + " (" + hresult_text(hr) + ").";
      return false;
    }
    return true;
  }

  void request_stop() noexcept {
    post_command(WorkerCommand::stop);
    // Every streaming wait includes command_event_. The worker stops the pin
    // and drains its own pending IRPs before acknowledging the command.
  }

  void wait_stopped(unsigned timeout_ms) noexcept {
    if (thread_.joinable() && !exited_.load(std::memory_order_acquire)) {
      WaitForSingleObject(acknowledge_event_, timeout_ms);
    }
  }

  void stop() noexcept {
    request_stop();
    wait_stopped(kWorkerHandshakeMs);
  }

  void shutdown() noexcept {
    if (thread_.joinable()) {
      if (!exited_.load(std::memory_order_acquire)) {
        request_stop();
        post_command(WorkerCommand::shutdown);
        WaitForSingleObject(acknowledge_event_, kWorkerHandshakeMs);
      }
      thread_.join();
    }
    unregister_wave_rt(pin_handle_);
    close_handle(pin_handle_);
    for (auto& packet : render_packets_) close_handle(packet.io.hEvent);
    for (auto& packet : capture_packets_) close_handle(packet.io.hEvent);
    close_handle(command_event_);
    close_handle(acknowledge_event_);
    close_handle(ready_event_);
  }

  bool is_input() const noexcept { return endpoint_.direction == Direction::input; }
  unsigned channels() const noexcept { return channels_; }
  const std::wstring& id() const noexcept { return endpoint_.id; }
  const std::wstring& name() const noexcept { return endpoint_.name; }

  void read_input(float* destination, unsigned frames) noexcept {
    if (!ring_) {
      if (destination) std::fill_n(destination, static_cast<size_t>(frames) * channels_, 0.0f);
      return;
    }
    const auto asio_rate = asio_sample_rate_.load(std::memory_order_relaxed);
    const double ratio = asio_rate ? static_cast<double>(device_sample_rate_) / asio_rate : 1.0;
    const auto plan = bridge_buffer_plan(stream_frames_, frames * ratio, device_sample_rate_);
    discarded_frames_.fetch_add(ring_->trim_backlog(plan.limit, plan.recovery), std::memory_order_relaxed);
    const auto available = ring_->fill_frames();
    if (!input_primed_ && available < plan.prime) {
      if (destination) std::fill_n(destination, static_cast<size_t>(frames) * channels_, 0.0f);
      if (input_has_started_) missing_frames_.fetch_add(frames, std::memory_order_relaxed);
      return;
    }
    input_primed_ = true;
    input_has_started_ = true;
    const auto missing = ring_->read_resampled(destination, frames, plan.target, ratio);
    rendered_frames_.fetch_add(frames - missing, std::memory_order_relaxed);
    missing_frames_.fetch_add(missing, std::memory_order_relaxed);
    if (missing) {
      capture_underruns_.fetch_add(1, std::memory_order_relaxed);
      input_primed_ = false;
    }
  }

  void push_output(const float* source, unsigned frames) noexcept {
    if (ring_) {
      const auto accepted = ring_->push(source, frames);
      received_frames_.fetch_add(accepted, std::memory_order_relaxed);
      dropped_frames_.fetch_add(frames - accepted, std::memory_order_relaxed);
    }
  }
  void set_asio_sample_rate(unsigned rate) noexcept {
    asio_sample_rate_.store(rate, std::memory_order_relaxed);
  }
  void set_asio_buffer_frames(unsigned frames) noexcept {
    asio_buffer_frames_.store(frames, std::memory_order_relaxed);
  }
  unsigned stream_latency_frames() const noexcept {
    const unsigned rate = asio_sample_rate_.load(std::memory_order_relaxed);
    const unsigned device_rate = std::max(1u, device_sample_rate_);
    const auto device_frames = stream_latency_frames_.load(std::memory_order_relaxed);
    return static_cast<unsigned>(std::min<unsigned long long>(
        (static_cast<unsigned long long>(device_frames) * rate + device_rate - 1) / device_rate,
        384000));
  }
  bool healthy() const noexcept {
    return running_.load(std::memory_order_acquire) &&
           SUCCEEDED(last_error_.load(std::memory_order_acquire));
  }
  unsigned actual_sample_rate() const noexcept {
    return device_sample_rate_;
  }
  unsigned actual_buffer_frames() const noexcept {
    return stream_frames_;
  }
  unsigned stream_buffer_latency_frames() const noexcept {
    const unsigned rate = asio_sample_rate_.load(std::memory_order_relaxed);
    return static_cast<unsigned>(std::min<unsigned long long>(
        (static_cast<unsigned long long>(stream_frames_) * rate +
         std::max(1u, device_sample_rate_) - 1) / std::max(1u, device_sample_rate_), 384000));
  }
  unsigned processed_packets() const noexcept { return processed_packets_.load(std::memory_order_relaxed); }
  unsigned render_underruns() const noexcept { return render_underruns_.load(std::memory_order_relaxed); }
  long last_status() const noexcept { return last_error_.load(std::memory_order_acquire); }
  bool wave_rt_mode() const noexcept { return rt_event_ != nullptr; }
  StreamStatistics statistics() const noexcept {
    return {actual_sample_rate(), actual_buffer_frames(), channels_, processed_packets(),
        is_input() ? capture_underruns_.load(std::memory_order_relaxed) : render_underruns(),
        ring_ ? static_cast<unsigned>(ring_->fill_frames()) : 0,
        (is_input() ? 1u : 0u) | (wave_rt_mode() ? 2u : 0u) | (healthy() ? 4u : 0u), last_status(),
        received_frames_.load(std::memory_order_relaxed), dropped_frames_.load(std::memory_order_relaxed),
        rendered_frames_.load(std::memory_order_relaxed), missing_frames_.load(std::memory_order_relaxed),
        discarded_frames_.load(std::memory_order_relaxed)};
  }

 private:
  static void close_handle(HANDLE& handle) noexcept {
    if (handle && handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); handle = nullptr; }
  }

  bool post_command(WorkerCommand command) noexcept {
    if (!command_event_ || !acknowledge_event_ || !thread_.joinable() ||
        exited_.load(std::memory_order_acquire)) return false;
    ResetEvent(acknowledge_event_);
    command_.store(static_cast<LONG>(command), std::memory_order_release);
    return SetEvent(command_event_) != FALSE;
  }

  HRESULT open_pin() {
    HRESULT last_error = E_FAIL;
    std::vector<KsPinCandidate> fallback_candidate;
    const std::vector<KsPinCandidate>* candidates = &endpoint_.ks_pin_candidates;
    if (candidates->empty()) {
      KsPinCandidate pin;
      pin.filter_path = endpoint_.ks_filter_path;
      pin.pin_id = endpoint_.ks_pin_id;
      pin.interface_set = endpoint_.ks_interface_set;
      pin.interface_id = endpoint_.ks_interface_id;
      pin.medium_set = endpoint_.ks_medium_set;
      pin.medium_id = endpoint_.ks_medium_id;
      pin.channels = endpoint_.channels;
      pin.sample_rates = endpoint_.native_sample_rates;
      fallback_candidate.push_back(std::move(pin));
      candidates = &fallback_candidate;
    }
    const DWORD access = is_input() ? GENERIC_READ : GENERIC_WRITE;

    for (const auto& candidate : *candidates) {
      HANDLE filter = CreateFileW(candidate.filter_path.c_str(), GENERIC_READ | GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (filter == INVALID_HANDLE_VALUE) {
        last_error = HRESULT_FROM_WIN32(GetLastError());
        if (ks_device_access::is_busy(last_error)) return last_error;
        continue;
      }
      const HRESULT availability = ks_device_access::check_pin(filter, candidate.pin_id);
      if (FAILED(availability)) {
        CloseHandle(filter);
        if (ks_device_access::is_busy(availability)) return availability;
        last_error = availability;
        continue;
      }
      auto rates = candidate.sample_rates.empty()
                       ? ordered_rates(endpoint_, sample_rate_) : candidate.sample_rates;
      std::stable_sort(rates.begin(), rates.end(), [&](unsigned left, unsigned right) {
        const auto left_delta = left > sample_rate_ ? left - sample_rate_ : sample_rate_ - left;
        const auto right_delta = right > sample_rate_ ? right - sample_rate_ : sample_rate_ - right;
        return left_delta < right_delta;
      });
      const auto channels = channel_candidates(candidate.channels ? candidate.channels : endpoint_.channels);
      for (const unsigned count : channels) {
        for (const unsigned rate : rates) {
          for (const auto& format : kFormatCandidates) {
            for (const bool extensible : {true, false}) {
              if (!extensible && count > 2) continue;
              auto connection = make_connection(candidate.pin_id,
                  candidate.interface_set, candidate.interface_id,
                  candidate.medium_set, candidate.medium_id,
                  count, rate, format, extensible);
              HANDLE pin = INVALID_HANDLE_VALUE;
              const DWORD status = KsCreatePin(filter,
                  reinterpret_cast<PKSPIN_CONNECT>(connection.data()), access, &pin);
              if (status == ERROR_SUCCESS && pin != INVALID_HANDLE_VALUE) {
                channels_ = count;
                device_sample_rate_ = rate;
                encoding_ = format.encoding;
                bits_per_sample_ = format.bits;
                block_align_ = count * format.bits / 8;
                const unsigned requested_stream_frames = std::clamp<unsigned>(static_cast<unsigned>(
                    (static_cast<unsigned long long>(buffer_frames_) * rate + sample_rate_ - 1) /
                    std::max(1u, sample_rate_)), 16u, 8192u);
                stream_frames_ = requested_stream_frames;
                bool wave_rt_requested = false;
                if (initialize_wave_rt(pin, wave_rt_requested)) {
                  pin_handle_ = pin;
                  CloseHandle(filter);
                  return S_OK;
                }
                if (!wave_rt_requested) {
                  // An ordinary KS pin needs no close/reopen cycle when the
                  // support query says WaveRT mapping is unavailable.
                  pin_handle_ = pin;
                  const bool initialized = initialize_transfer_buffers();
                  if (!initialized) close_handle(pin_handle_);
                  CloseHandle(filter);
                  return initialized ? S_OK : E_OUTOFMEMORY;
                }
                unregister_wave_rt(pin);
                CloseHandle(pin);
                stream_frames_ = requested_stream_frames;

                // Some filters expose KS pins but do not implement WaveRT. Try
                // the standard stream-I/O path with a fresh pin instance.
                pin = INVALID_HANDLE_VALUE;
                const DWORD retry = KsCreatePin(filter,
                    reinterpret_cast<PKSPIN_CONNECT>(connection.data()), access, &pin);
                if (retry == ERROR_SUCCESS && pin != INVALID_HANDLE_VALUE) {
                  pin_handle_ = pin;
                  if (initialize_transfer_buffers()) {
                    CloseHandle(filter);
                    return S_OK;
                  }
                  close_handle(pin_handle_);
                  CloseHandle(filter);
                  return E_OUTOFMEMORY;
                }
                last_error = HRESULT_FROM_WIN32(retry);
                if (ks_device_access::is_busy(last_error)) {
                  CloseHandle(filter);
                  return last_error;
                }
                continue;
              }
              last_error = HRESULT_FROM_WIN32(status);
              if (ks_device_access::is_busy(last_error)) {
                CloseHandle(filter);
                return last_error;
              }
            }
          }
        }
      }
      CloseHandle(filter);
    }
    return last_error;
  }

  bool initialize_transfer_buffers() noexcept {
    try {
      conversion_buffer_.resize(static_cast<size_t>(stream_frames_) * channels_);
      ring_ = std::make_unique<AudioRing>(channels_,
          std::max(16384u, next_power_of_two(buffer_frames_ * 8)));
      if (!is_input() && !rt_event_) {
        for (auto& packet : render_packets_) {
          packet.data.resize(static_cast<size_t>(stream_frames_) * block_align_);
          if (!packet.io.hEvent) packet.io.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
          if (!packet.io.hEvent) return false;
        }
      }
      if (is_input() && !rt_event_) {
        for (auto& packet : capture_packets_) {
          packet.data.resize(static_cast<size_t>(stream_frames_) * block_align_);
          if (!packet.io.hEvent) packet.io.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
          if (!packet.io.hEvent) return false;
        }
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  bool initialize_wave_rt(HANDLE pin, bool& mapping_requested) noexcept {
    // Query support before issuing a mapping request to an arbitrary KS driver.
    KSPROPERTY support{};
    support.Set = KSPROPSETID_RtAudio;
    support.Id = KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION;
    support.Flags = KSPROPERTY_TYPE_BASICSUPPORT;
    ULONG access{};
    ULONG support_bytes{};
    if (!DeviceIoControl(pin, IOCTL_KS_PROPERTY, &support, sizeof(support),
                         &access, sizeof(access), &support_bytes, nullptr) ||
        support_bytes < sizeof(access) || !(access & KSPROPERTY_TYPE_GET)) return false;
    KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION request{};
    request.Property.Set = KSPROPSETID_RtAudio;
    request.Property.Id = KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION;
    request.Property.Flags = KSPROPERTY_TYPE_GET;
    request.BaseAddress = nullptr;
    request.RequestedBufferSize = static_cast<ULONG>(
        static_cast<size_t>(stream_frames_) * block_align_ * 2);
    request.NotificationCount = 2;
    KSRTAUDIO_BUFFER buffer{};
    ULONG returned = 0;
    mapping_requested = true;
    if (!DeviceIoControl(pin, IOCTL_KS_PROPERTY, &request, sizeof(request),
                         &buffer, sizeof(buffer), &returned, nullptr) ||
        !buffer.BufferAddress || buffer.ActualBufferSize < block_align_ * 2) {
      return false;
    }
    const unsigned notification_frames = buffer.ActualBufferSize / block_align_ / 2;
    if (!notification_frames || notification_frames > kMaximumRingFrames ||
        buffer.ActualBufferSize % (block_align_ * 2) != 0) return false;

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY registration{};
    registration.Property.Set = KSPROPSETID_RtAudio;
    registration.Property.Id = KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT;
    registration.Property.Flags = KSPROPERTY_TYPE_SET;
    registration.NotificationEvent = event;
    returned = 0;
    if (!DeviceIoControl(pin, IOCTL_KS_PROPERTY, &registration, sizeof(registration),
                         nullptr, 0, &returned, nullptr)) {
      CloseHandle(event);
      return false;
    }

    rt_event_ = event;
    rt_buffer_address_ = static_cast<BYTE*>(buffer.BufferAddress);
    rt_buffer_size_ = buffer.ActualBufferSize;
    rt_frames_per_notification_ = notification_frames;
    rt_call_memory_barrier_ = buffer.CallMemoryBarrier != FALSE;
    rt_notification_registered_ = true;
    stream_frames_ = notification_frames;
    // With two WaveRT notifications per cycle, the first event is raised at
    // the half-buffer point. For render, the first half has just been consumed
    // and is now the safe half to refill; capture reads that same first half.
    // Starting render at segment 1 writes into the half the device is playing,
    // leaving the next half silent and causing persistent playback underruns.
    rt_next_segment_ = 0u;
    if (!is_input()) {
      std::memset(rt_buffer_address_, 0, rt_buffer_size_);
      if (rt_call_memory_barrier_) MemoryBarrier();
    }

    KSPROPERTY latency_property{};
    latency_property.Set = KSPROPSETID_RtAudio;
    latency_property.Id = KSPROPERTY_RTAUDIO_HWLATENCY;
    latency_property.Flags = KSPROPERTY_TYPE_GET;
    KSRTAUDIO_HWLATENCY hardware_latency{};
    returned = 0;
    if (DeviceIoControl(pin, IOCTL_KS_PROPERTY, &latency_property, sizeof(latency_property),
                        &hardware_latency, sizeof(hardware_latency), &returned, nullptr)) {
      const unsigned long long delay_100ns =
          static_cast<unsigned long long>(hardware_latency.ChipsetDelay) +
          hardware_latency.CodecDelay;
      const unsigned long long fifo_frames = hardware_latency.FifoSize / block_align_;
      const unsigned long long delay_frames = delay_100ns * device_sample_rate_ / 10'000'000ULL;
      stream_latency_frames_.store(static_cast<unsigned>(std::min<unsigned long long>(
          fifo_frames + delay_frames, 384000)), std::memory_order_relaxed);
    }

    return initialize_transfer_buffers();
  }

  void unregister_wave_rt(HANDLE pin) noexcept {
    if (rt_notification_registered_ && pin && pin != INVALID_HANDLE_VALUE && rt_event_) {
      KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY registration{};
      registration.Property.Set = KSPROPSETID_RtAudio;
      registration.Property.Id = KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT;
      registration.Property.Flags = KSPROPERTY_TYPE_SET;
      registration.NotificationEvent = rt_event_;
      ULONG returned = 0;
      DeviceIoControl(pin, IOCTL_KS_PROPERTY, &registration, sizeof(registration),
                      nullptr, 0, &returned, nullptr);
      rt_notification_registered_ = false;
    }
    close_handle(rt_event_);
    rt_buffer_address_ = nullptr;
    rt_buffer_size_ = 0;
    rt_frames_per_notification_ = 0;
  }

  HRESULT transition_to(KSSTATE target) noexcept {
    KSSTATE current = pin_state_.load(std::memory_order_acquire);
    while (current != target) {
      KSSTATE next = current;
      if (current == KSSTATE_STOP) next = KSSTATE_ACQUIRE;
      else if (current == KSSTATE_ACQUIRE) next = target == KSSTATE_RUN ? KSSTATE_PAUSE : KSSTATE_STOP;
      else if (current == KSSTATE_PAUSE) next = target == KSSTATE_RUN ? KSSTATE_RUN : KSSTATE_ACQUIRE;
      else if (current == KSSTATE_RUN) next = KSSTATE_PAUSE;
      const HRESULT hr = set_pin_state(pin_handle_, next);
      if (FAILED(hr)) return hr;
      pin_state_.store(next, std::memory_order_release);
      current = next;
    }
    running_.store(target == KSSTATE_RUN, std::memory_order_release);
    return S_OK;
  }

  HRESULT queue_capture_packet(unsigned index) noexcept {
    auto& packet = capture_packets_[index];
    auto& header = packet.header;
    header = {};
    header.Size = sizeof(header);
    header.FrameExtent = static_cast<ULONG>(packet.data.size());
    header.DataUsed = 0;
    header.Data = packet.data.data();
    header.PresentationTime.Numerator = 1;
    header.PresentationTime.Denominator = 1;
    ULONG returned = 0;
    const HANDLE event = packet.io.hEvent;
    packet.io = {};
    packet.io.hEvent = event;
    ResetEvent(event);
    if (DeviceIoControl(pin_handle_, IOCTL_KS_READ_STREAM, nullptr, 0,
                        &header, sizeof(header), &returned, &packet.io)) {
      SetEvent(event);
    } else if (const DWORD status = GetLastError(); status != ERROR_IO_PENDING) {
      return HRESULT_FROM_WIN32(status);
    }
    packet.in_flight = true;
    return S_OK;
  }

  void accept_capture(unsigned frames) noexcept {
    const auto accepted = ring_->push(conversion_buffer_.data(), frames);
    received_frames_.fetch_add(frames, std::memory_order_relaxed);
    dropped_frames_.fetch_add(frames - accepted, std::memory_order_relaxed);
  }

  HRESULT transfer_capture() noexcept {
    if (!capture_queued_) {
      for (unsigned index = 0; index < 2; ++index) {
        const HRESULT hr = queue_capture_packet(index);
        if (FAILED(hr)) return hr;
      }
      capture_queued_ = true;
    }
    auto& packet = capture_packets_[capture_next_packet_];
    HANDLE waits[]{command_event_, packet.io.hEvent};
    const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0) {
      SetEvent(command_event_);
      return HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
    }
    if (wait != WAIT_OBJECT_0 + 1) return HRESULT_FROM_WIN32(GetLastError());
    DWORD returned = 0;
    const BOOL completed = GetOverlappedResult(pin_handle_, &packet.io, &returned, FALSE);
    const DWORD status = completed ? ERROR_SUCCESS : GetLastError();
    packet.in_flight = false;
    if (!completed) return HRESULT_FROM_WIN32(status);
    // A third buffer keeps two reads queued while the completed buffer is
    // decoded. Never resubmit storage that still contains unread samples.
    const unsigned packet_count = static_cast<unsigned>(capture_packets_.size());
    const HRESULT hr = queue_capture_packet((capture_next_packet_ + 2) % packet_count);
    if (FAILED(hr)) return hr;
    if (packet.header.DataUsed > packet.header.FrameExtent || packet.header.DataUsed % block_align_) {
      return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    const unsigned frames = std::min<unsigned>(stream_frames_, packet.header.DataUsed / block_align_);
    const size_t samples = static_cast<size_t>(frames) * channels_;
    const unsigned bytes_per_sample = bits_per_sample_ / 8;
    for (size_t i = 0; i < samples; ++i) {
      conversion_buffer_[i] = decode_sample(packet.data.data() + i * bytes_per_sample, encoding_);
    }
    if (frames) accept_capture(frames);
    capture_next_packet_ = (capture_next_packet_ + 1) % packet_count;
    return S_OK;
  }

  void prepare_render(unsigned frames) noexcept {
    const double ratio = static_cast<double>(asio_sample_rate_.load(std::memory_order_relaxed)) /
                         std::max(1u, device_sample_rate_);
    const auto plan = bridge_buffer_plan(asio_buffer_frames_.load(std::memory_order_relaxed),
        frames * ratio, asio_sample_rate_.load(std::memory_order_relaxed));
    discarded_frames_.fetch_add(ring_->trim_backlog(plan.limit, plan.recovery), std::memory_order_relaxed);
    const auto available = ring_->fill_frames();
    if (available < plan.required) {
      if (output_primed_) render_underruns_.fetch_add(1, std::memory_order_relaxed);
      output_primed_ = false;
    }
    if (!output_primed_) {
      if (available < plan.prime) {
        std::fill_n(conversion_buffer_.data(), static_cast<size_t>(frames) * channels_, 0.0f);
        return;
      }
      output_primed_ = true;
    }
    ring_->read_resampled(conversion_buffer_.data(), frames, plan.target, ratio);
    rendered_frames_.fetch_add(frames, std::memory_order_relaxed);
  }

  HRESULT queue_render_packet(unsigned index) noexcept {
    auto& packet = render_packets_[index];
    prepare_render(stream_frames_);
    const size_t samples = static_cast<size_t>(stream_frames_) * channels_;
    const unsigned bytes_per_sample = bits_per_sample_ / 8;
    for (size_t i = 0; i < samples; ++i) {
      encode_sample(packet.data.data() + i * bytes_per_sample, encoding_, conversion_buffer_[i]);
    }
    auto& header = packet.header;
    header = {};
    header.Size = sizeof(header);
    header.FrameExtent = static_cast<ULONG>(packet.data.size());
    header.DataUsed = header.FrameExtent;
    header.Data = packet.data.data();
    header.PresentationTime.Numerator = 1;
    header.PresentationTime.Denominator = 1;
    ULONG returned = 0;
    const HANDLE event = packet.io.hEvent;
    packet.io = {};
    packet.io.hEvent = event;
    ResetEvent(event);
    // Both KS stream IOCTLs take their KSSTREAM_HEADER array in the output
    // buffer (METHOD_NEITHER), including render/write requests.
    if (DeviceIoControl(pin_handle_, IOCTL_KS_WRITE_STREAM, nullptr, 0,
                        &header, sizeof(header), &returned, &packet.io)) {
      SetEvent(event);
    } else if (const DWORD status = GetLastError(); status != ERROR_IO_PENDING) {
      return HRESULT_FROM_WIN32(status);
    }
    packet.in_flight = true;
    return S_OK;
  }

  void cancel_render_packets() noexcept {
    for (auto& packet : render_packets_) {
      if (!packet.in_flight) continue;
      CancelIoEx(pin_handle_, &packet.io);
      DWORD returned = 0;
      // Keep headers and sample storage alive until cancellation completes.
      GetOverlappedResult(pin_handle_, &packet.io, &returned, TRUE);
      packet.in_flight = false;
    }
    render_queued_ = false;
    render_next_packet_ = 0;
  }

  void cancel_capture_packets() noexcept {
    for (auto& packet : capture_packets_) {
      if (!packet.in_flight) continue;
      CancelIoEx(pin_handle_, &packet.io);
      DWORD returned = 0;
      GetOverlappedResult(pin_handle_, &packet.io, &returned, TRUE);
      packet.in_flight = false;
    }
    capture_queued_ = false;
    capture_next_packet_ = 0;
  }

  HRESULT stop_pin() noexcept {
    // STOP lets drivers flush queued stream IRPs before we wait for their
    // cancellation. Their headers and buffers remain alive throughout.
    const HRESULT hr = transition_to(KSSTATE_STOP);
    cancel_render_packets();
    cancel_capture_packets();
    return hr;
  }

  HRESULT transfer_render() noexcept {
    if (!render_queued_) {
      for (unsigned index = 0; index < render_packets_.size(); ++index) {
        const HRESULT hr = queue_render_packet(index);
        if (FAILED(hr)) return hr;
      }
      render_queued_ = true;
    }
    auto& packet = render_packets_[render_next_packet_];
    HANDLE waits[]{command_event_, packet.io.hEvent};
    const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0) {
      // This auto-reset event was consumed by the wait. The stopped branch
      // still needs to receive and acknowledge the pending command.
      SetEvent(command_event_);
      return HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
    }
    if (wait != WAIT_OBJECT_0 + 1) return HRESULT_FROM_WIN32(GetLastError());
    DWORD returned = 0;
    const BOOL completed = GetOverlappedResult(pin_handle_, &packet.io, &returned, FALSE);
    packet.in_flight = false;
    if (!completed) return HRESULT_FROM_WIN32(GetLastError());
    // The other packet remains queued while this packet is converted/refilled.
    const HRESULT hr = queue_render_packet(render_next_packet_);
    render_next_packet_ ^= 1u;
    return hr;
  }

  HRESULT transfer_wave_rt() noexcept {
    if (!rt_buffer_address_ || !rt_event_ || !rt_frames_per_notification_) return E_HANDLE;
    const size_t segment_bytes = static_cast<size_t>(rt_frames_per_notification_) * block_align_;
    if (!is_input()) {
      // Auto-reset notifications may coalesce when the worker is delayed.
      // Refill the half behind the actual play cursor rather than assuming
      // that every half-buffer notification has been observed.
      KSPROPERTY property{};
      property.Set = KSPROPSETID_Audio;
      property.Id = KSPROPERTY_AUDIO_POSITION;
      property.Flags = KSPROPERTY_TYPE_GET;
      KSAUDIO_POSITION position{};
      ULONG returned = 0;
      if (DeviceIoControl(pin_handle_, IOCTL_KS_PROPERTY, &property, sizeof(property),
                          &position, sizeof(position), &returned, nullptr) &&
          returned >= sizeof(position)) {
        const auto playing_segment = (position.PlayOffset % rt_buffer_size_) / segment_bytes;
        rt_next_segment_ = static_cast<unsigned>(playing_segment) ^ 1u;
      }
    }
    const size_t offset = static_cast<size_t>(rt_next_segment_) * segment_bytes;
    if (offset + segment_bytes > rt_buffer_size_) return E_INVALIDARG;
    BYTE* segment = rt_buffer_address_ + offset;
    if (rt_call_memory_barrier_) MemoryBarrier();

    if (is_input()) {
      const size_t samples = static_cast<size_t>(rt_frames_per_notification_) * channels_;
      const unsigned bytes_per_sample = bits_per_sample_ / 8;
      for (size_t i = 0; i < samples; ++i) {
        conversion_buffer_[i] = decode_sample(segment + i * bytes_per_sample, encoding_);
      }
      accept_capture(rt_frames_per_notification_);
    } else {
      prepare_render(rt_frames_per_notification_);
      const size_t samples = static_cast<size_t>(rt_frames_per_notification_) * channels_;
      const unsigned bytes_per_sample = bits_per_sample_ / 8;
      for (size_t i = 0; i < samples; ++i) {
        encode_sample(segment + i * bytes_per_sample, encoding_, conversion_buffer_[i]);
      }
      if (rt_call_memory_barrier_) MemoryBarrier();
    }
    rt_next_segment_ ^= 1u;
    processed_packets_.fetch_add(1, std::memory_order_relaxed);
    return S_OK;
  }

  void run() noexcept {
    init_result_.store(S_OK, std::memory_order_release);
    SetEvent(ready_event_);
    DWORD mmcss_index = 0;
    HANDLE mmcss = nullptr;
    bool done = false;
    while (!done) {
      if (!running_.load(std::memory_order_acquire)) {
        const DWORD wait = WaitForSingleObject(command_event_, INFINITE);
        if (wait != WAIT_OBJECT_0) break;
        const auto command = static_cast<WorkerCommand>(command_.exchange(
            static_cast<LONG>(WorkerCommand::none), std::memory_order_acq_rel));
        HRESULT hr = S_OK;
        if (command == WorkerCommand::start) {
          last_error_.store(S_OK, std::memory_order_release);
          ring_->reset();
          input_primed_ = false;
          input_has_started_ = false;
          output_primed_ = false;
          processed_packets_.store(0, std::memory_order_relaxed);
          render_underruns_.store(0, std::memory_order_relaxed);
          capture_underruns_.store(0, std::memory_order_relaxed);
          received_frames_.store(0, std::memory_order_relaxed);
          dropped_frames_.store(0, std::memory_order_relaxed);
          rendered_frames_.store(0, std::memory_order_relaxed);
          missing_frames_.store(0, std::memory_order_relaxed);
          discarded_frames_.store(0, std::memory_order_relaxed);
          if (rt_event_) {
            ResetEvent(rt_event_);
            rt_next_segment_ = 0;
            if (!is_input()) {
              std::memset(rt_buffer_address_, 0, rt_buffer_size_);
              if (rt_call_memory_barrier_) MemoryBarrier();
            }
          }
          mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_index);
          SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
          hr = transition_to(KSSTATE_RUN);
          if (FAILED(hr)) last_error_.store(hr, std::memory_order_release);
          if (FAILED(hr) && mmcss) { AvRevertMmThreadCharacteristics(mmcss); mmcss = nullptr; }
        } else if (command == WorkerCommand::stop) {
          hr = stop_pin();
          if (mmcss) { AvRevertMmThreadCharacteristics(mmcss); mmcss = nullptr; }
        } else if (command == WorkerCommand::shutdown) {
          stop_pin();
          if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
          mmcss = nullptr;
          done = true;
        }
        command_result_.store(hr, std::memory_order_release);
        SetEvent(acknowledge_event_);
        continue;
      }

      const auto command = static_cast<WorkerCommand>(command_.exchange(
          static_cast<LONG>(WorkerCommand::none), std::memory_order_acq_rel));
      if (command != WorkerCommand::none) {
        if (command == WorkerCommand::start) {
          command_result_.store(S_OK, std::memory_order_release);
          SetEvent(acknowledge_event_);
          continue;
        }
        HRESULT hr = stop_pin();
        if (mmcss) { AvRevertMmThreadCharacteristics(mmcss); mmcss = nullptr; }
        if (command == WorkerCommand::shutdown) done = true;
        command_result_.store(hr, std::memory_order_release);
        SetEvent(acknowledge_event_);
        continue;
      }

      if (rt_event_) {
        HANDLE waits[]{command_event_, rt_event_};
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) {
          const auto next_command = static_cast<WorkerCommand>(command_.exchange(
              static_cast<LONG>(WorkerCommand::none), std::memory_order_acq_rel));
          const HRESULT stop_hr = stop_pin();
          if (mmcss) { AvRevertMmThreadCharacteristics(mmcss); mmcss = nullptr; }
          if (next_command == WorkerCommand::shutdown) done = true;
          command_result_.store(stop_hr, std::memory_order_release);
          SetEvent(acknowledge_event_);
          continue;
        }
        if (wait == WAIT_OBJECT_0 + 1) {
          const HRESULT wave_rt_result = transfer_wave_rt();
          if (FAILED(wave_rt_result)) {
            last_error_.store(wave_rt_result, std::memory_order_release);
            stop_pin();
            running_.store(false, std::memory_order_release);
            if (mmcss) { AvRevertMmThreadCharacteristics(mmcss); mmcss = nullptr; }
          }
          continue;
        }
        if (wait == WAIT_FAILED) break;
        continue;
      }

      const HRESULT hr = is_input() ? transfer_capture() : transfer_render();
      if (FAILED(hr)) {
        const auto pending_command = static_cast<WorkerCommand>(command_.load(std::memory_order_acquire));
        if (pending_command == WorkerCommand::none) {
          last_error_.store(hr, std::memory_order_release);
        } else if (hr != HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED) &&
                   hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
          last_error_.store(hr, std::memory_order_release);
        }
        stop_pin();
        running_.store(false, std::memory_order_release);
        if (mmcss) { AvRevertMmThreadCharacteristics(mmcss); mmcss = nullptr; }
      } else {
        processed_packets_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (pin_handle_) stop_pin();
    unregister_wave_rt(pin_handle_);
    close_handle(pin_handle_);
    exited_.store(true, std::memory_order_release);
    SetEvent(acknowledge_event_);
  }

  AudioEndpoint endpoint_;
  ks_device_access::PinReservation pin_reservation_;
  unsigned sample_rate_;
  unsigned buffer_frames_;
  unsigned device_sample_rate_{};
  unsigned channels_{};
  unsigned bits_per_sample_{};
  unsigned block_align_{};
  unsigned stream_frames_{};
  SampleEncoding encoding_{SampleEncoding::float32};
  std::vector<float> conversion_buffer_;
  struct StreamPacket {
    std::vector<BYTE> data;
    KSSTREAM_HEADER header{};
    OVERLAPPED io{};
    bool in_flight{};
  };
  std::array<StreamPacket, 2> render_packets_;
  std::array<StreamPacket, 3> capture_packets_;
  unsigned capture_next_packet_{};
  bool capture_queued_{};
  bool input_primed_{};
  bool input_has_started_{};
  unsigned render_next_packet_{};
  bool render_queued_{};
  bool output_primed_{};
  std::unique_ptr<AudioRing> ring_;
  HANDLE pin_handle_{};
  HANDLE rt_event_{};
  BYTE* rt_buffer_address_{};
  ULONG rt_buffer_size_{};
  unsigned rt_frames_per_notification_{};
  unsigned rt_next_segment_{};
  bool rt_call_memory_barrier_{};
  bool rt_notification_registered_{};
  HANDLE command_event_{};
  HANDLE acknowledge_event_{};
  HANDLE ready_event_{};
  std::thread thread_;
  std::atomic<LONG> command_{static_cast<LONG>(WorkerCommand::none)};
  std::atomic<HRESULT> init_result_{E_PENDING};
  std::atomic<HRESULT> command_result_{S_OK};
  std::atomic<HRESULT> last_error_{S_OK};
  std::atomic<bool> exited_{false};
  std::atomic<bool> running_{false};
  std::atomic<KSSTATE> pin_state_{KSSTATE_STOP};
  std::atomic<unsigned> asio_sample_rate_{};
  std::atomic<unsigned> asio_buffer_frames_{128};
  std::atomic<unsigned> stream_latency_frames_{0};
  std::atomic<unsigned> processed_packets_{0};
  std::atomic<unsigned> render_underruns_{0};
  std::atomic<unsigned> capture_underruns_{0};
  std::atomic<uint64_t> received_frames_{0};
  std::atomic<uint64_t> dropped_frames_{0};
  std::atomic<uint64_t> rendered_frames_{0};
  std::atomic<uint64_t> missing_frames_{0};
  std::atomic<uint64_t> discarded_frames_{0};
};

EndpointAudioStream::EndpointAudioStream(AudioEndpoint endpoint, unsigned sample_rate,
                                         unsigned buffer_frames)
    : impl_(std::make_unique<Impl>(std::move(endpoint), sample_rate, buffer_frames)) {}
EndpointAudioStream::~EndpointAudioStream() = default;
bool EndpointAudioStream::initialize(std::string& error) { return impl_->initialize(error); }
bool EndpointAudioStream::wait_initialized(std::string& error) { return impl_->wait_initialized(error); }
bool EndpointAudioStream::start(std::string& error) {
  if (!request_start()) { error = "The KS pin worker is not available."; return false; }
  return wait_started(kWorkerHandshakeMs, error);
}
bool EndpointAudioStream::request_start() noexcept { return impl_->request_start(); }
bool EndpointAudioStream::wait_started(unsigned timeout_ms, std::string& error) noexcept {
  return impl_->wait_started(timeout_ms, error);
}
void EndpointAudioStream::stop() noexcept { impl_->stop(); }
void EndpointAudioStream::request_stop() noexcept { impl_->request_stop(); }
void EndpointAudioStream::wait_stopped(unsigned timeout_ms) noexcept { impl_->wait_stopped(timeout_ms); }
bool EndpointAudioStream::is_input() const noexcept { return impl_->is_input(); }
unsigned EndpointAudioStream::channels() const noexcept { return impl_->channels(); }
const std::wstring& EndpointAudioStream::id() const noexcept { return impl_->id(); }
const std::wstring& EndpointAudioStream::name() const noexcept { return impl_->name(); }
void EndpointAudioStream::read_input(float* destination, unsigned frames) noexcept {
  impl_->read_input(destination, frames);
}
void EndpointAudioStream::push_output(const float* source, unsigned frames) noexcept {
  impl_->push_output(source, frames);
}
void EndpointAudioStream::set_asio_sample_rate(unsigned rate) noexcept { impl_->set_asio_sample_rate(rate); }
void EndpointAudioStream::set_asio_buffer_frames(unsigned frames) noexcept {
  impl_->set_asio_buffer_frames(frames);
}
unsigned EndpointAudioStream::stream_latency_frames() const noexcept {
  return impl_->stream_latency_frames();
}
bool EndpointAudioStream::wave_rt_mode() const noexcept { return impl_->wave_rt_mode(); }
bool EndpointAudioStream::healthy() const noexcept { return impl_->healthy(); }
unsigned EndpointAudioStream::actual_sample_rate() const noexcept { return impl_->actual_sample_rate(); }
unsigned EndpointAudioStream::actual_buffer_frames() const noexcept { return impl_->actual_buffer_frames(); }
unsigned EndpointAudioStream::stream_buffer_latency_frames() const noexcept {
  return impl_->stream_buffer_latency_frames();
}
unsigned EndpointAudioStream::processed_packets() const noexcept { return impl_->processed_packets(); }
unsigned EndpointAudioStream::render_underruns() const noexcept { return impl_->render_underruns(); }
StreamStatistics EndpointAudioStream::statistics() const noexcept { return impl_->statistics(); }
long EndpointAudioStream::last_status() const noexcept { return impl_->last_status(); }

AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::initialize(const AudioSettings& settings,
                             const std::vector<AudioEndpoint>& catalog,
                             std::string& error) {
  stop();
  inputs_.clear();
  outputs_.clear();
  last_status_ = S_OK;
  sample_rate_ = settings.sample_rate;
  buffer_frames_ = settings.buffer_frames;
  if (sample_rate_ < 8000 || sample_rate_ > 384000 || !buffer_frames_) {
    last_status_ = E_INVALIDARG;
    error = "The saved sample rate or buffer size is invalid. Run the configurator first.";
    return false;
  }
  auto add = [&](const std::vector<std::wstring>& ids, Direction direction,
                 auto& destination) -> bool {
    for (const auto& id : ids) {
      auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& endpoint) {
        return endpoint.id == id && endpoint.direction == direction;
      });
      if (found == catalog.end()) {
        // Old profiles stored a logical device/role ID. Keep those profiles
        // usable by selecting the first matching KS pin until the new UI saves
        // an explicit per-channel selection.
        found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& endpoint) {
          return endpoint.direction == direction &&
              std::find(endpoint.legacy_ids.begin(), endpoint.legacy_ids.end(), id) !=
                  endpoint.legacy_ids.end();
        });
      }
      if (found == catalog.end()) continue;
      if (std::any_of(destination.begin(), destination.end(), [&](const auto& stream) {
            return stream->id() == found->id;
          })) continue;
      const auto endpoint_buffer = device_buffer_frames(settings, found->device_instance_id);
      auto stream = std::unique_ptr<EndpointAudioStream>(
          new EndpointAudioStream(*found, sample_rate_, endpoint_buffer));
      if (!stream->initialize(error)) {
        last_status_ = stream->last_status();
        return false;
      }
      stream->set_asio_sample_rate(sample_rate_);
      stream->set_asio_buffer_frames(buffer_frames_);
      destination.push_back(std::move(stream));
    }
    return true;
  };
  if (!add(settings.input_device_ids, Direction::input, inputs_) ||
      !add(settings.output_device_ids, Direction::output, outputs_)) {
    stop();
    inputs_.clear();
    outputs_.clear();
    return false;
  }
  auto wait_for_streams = [&](auto& streams) {
    for (auto& stream : streams) {
      if (!stream->wait_initialized(error)) {
        last_status_ = stream->last_status();
        return false;
      }
    }
    return true;
  };
  if (!wait_for_streams(inputs_) || !wait_for_streams(outputs_)) {
    stop();
    inputs_.clear();
    outputs_.clear();
    return false;
  }
  if (inputs_.empty() && outputs_.empty()) {
    last_status_ = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    error = "No selected KS pins are available. Open the ASIO control panel and select devices.";
    return false;
  }
  return true;
}

bool AudioEngine::start(std::string& error) {
  last_status_ = S_OK;
  auto request_streams = [&](auto& streams) {
    for (auto& stream : streams) if (!stream->request_start()) return false;
    return true;
  };
  if (!request_streams(inputs_) || !request_streams(outputs_)) {
    stop();
    last_status_ = E_FAIL;
    error = "A KS pin worker is not available.";
    return false;
  }
  const ULONGLONG deadline = GetTickCount64() + kWorkerHandshakeMs;
  auto wait_streams = [&](auto& streams) {
    for (auto& stream : streams) {
      const ULONGLONG now = GetTickCount64();
      const unsigned remaining = now < deadline ? static_cast<unsigned>(deadline - now) : 0;
      if (!stream->wait_started(remaining, error)) {
        last_status_ = stream->last_status();
        return false;
      }
    }
    return true;
  };
  if (!wait_streams(inputs_) || !wait_streams(outputs_)) { stop(); return false; }
  return true;
}

void AudioEngine::stop() noexcept {
  for (auto& stream : inputs_) stream->request_stop();
  for (auto& stream : outputs_) stream->request_stop();
  const ULONGLONG deadline = GetTickCount64() + kWorkerHandshakeMs;
  auto wait_streams = [&](auto& streams) {
    for (auto& stream : streams) {
      const ULONGLONG now = GetTickCount64();
      stream->wait_stopped(now < deadline ? static_cast<unsigned>(deadline - now) : 0);
    }
  };
  wait_streams(inputs_);
  wait_streams(outputs_);
}

void AudioEngine::set_asio_sample_rate(unsigned rate) noexcept {
  sample_rate_ = rate;
  for (auto& stream : inputs_) stream->set_asio_sample_rate(rate);
  for (auto& stream : outputs_) stream->set_asio_sample_rate(rate);
}
void AudioEngine::set_asio_buffer_frames(unsigned frames) noexcept {
  buffer_frames_ = frames;
  for (auto& stream : inputs_) stream->set_asio_buffer_frames(frames);
  for (auto& stream : outputs_) stream->set_asio_buffer_frames(frames);
}
unsigned AudioEngine::approximate_input_latency_frames() const noexcept {
  if (inputs_.empty()) return 0;
  unsigned latency = 0;
  for (const auto& stream : inputs_) {
    const unsigned device_rate = std::max(1u, stream->actual_sample_rate());
    const unsigned packet = stream->actual_buffer_frames();
    const auto plan = bridge_buffer_plan(packet,
        static_cast<double>(buffer_frames_) * device_rate / sample_rate_, device_rate);
    const auto bridge = static_cast<unsigned>(std::ceil(
        static_cast<double>(std::max(plan.prime, plan.target) + packet) * sample_rate_ / device_rate));
    latency = std::max(latency, stream->stream_latency_frames() + bridge);
  }
  return latency;
}
unsigned AudioEngine::approximate_output_latency_frames() const noexcept {
  if (outputs_.empty()) return 0;
  unsigned latency = 0;
  for (const auto& stream : outputs_) {
    const auto block = stream->stream_buffer_latency_frames();
    const auto plan = bridge_buffer_plan(buffer_frames_,
        static_cast<double>(stream->actual_buffer_frames()) * sample_rate_ /
            std::max(1u, stream->actual_sample_rate()), sample_rate_);
    latency = std::max(latency, stream->stream_latency_frames() + block * 2 +
        std::max(plan.prime, plan.target) + buffer_frames_ / 2 + buffer_frames_);
  }
  return latency;
}
EndpointAudioStream* AudioEngine::input_stream(size_t index) const noexcept {
  return index < inputs_.size() ? inputs_[index].get() : nullptr;
}
EndpointAudioStream* AudioEngine::output_stream(size_t index) const noexcept {
  return index < outputs_.size() ? outputs_[index].get() : nullptr;
}
