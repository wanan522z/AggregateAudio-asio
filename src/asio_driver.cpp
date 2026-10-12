#include "audio_engine.h"
#include "driver_telemetry.h"
#include "device_recovery.h"
#include "selected_device_notifications.h"

#include <rpc.h>
#include <rpcndr.h>
#include <windows.h>
#include <mmsystem.h>
#include <ole2.h>
#include <basetyps.h>
#include <unknwn.h>
#include <objbase.h>
#include <avrt.h>
#include <shellapi.h>

#include "combase.h"
#include "iasiodrv.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

// {D3AA79B2-9D91-4B6F-A2B1-55CE84C2A43D}
CLSID CLSID_AggregateASIO = {0xd3aa79b2, 0x9d91, 0x4b6f, {0xa2, 0xb1, 0x55, 0xce, 0x84, 0xc2, 0xa4, 0x3d}};
constexpr long kMinBufferFrames = 64;
constexpr long kMaxBufferFrames = 2048;
constexpr size_t kMaximumAsioChannels = 256;

std::atomic<HMODULE> g_module{nullptr};
std::atomic<ULONGLONG> g_last_automatic_reset{};

struct ChannelBinding {
  size_t endpoint_index{};
  unsigned endpoint_channel{};
};

struct AsioBuffer {
  bool input{};
  long channel{};
  std::vector<float> storage;
  float* halves[2]{};
};

bool is_supported_rate(unsigned rate) {
  constexpr std::array<unsigned, 8> rates{32000, 44100, 48000, 88200,
                                           96000, 176400, 192000, 384000};
  return std::find(rates.begin(), rates.end(), rate) != rates.end();
}

void copy_message(char* destination, size_t capacity, const std::string& message) {
  if (!destination || !capacity) return;
  strncpy_s(destination, capacity, message.c_str(), _TRUNCATE);
}

std::string narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) return "audio endpoint";
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), size, nullptr, nullptr);
  return result;
}

template <class SamplePair>
void write_samples(SamplePair& target, uint64_t samples) {
  target.hi = static_cast<unsigned long>(samples >> 32);
  target.lo = static_cast<unsigned long>(samples & 0xffffffffULL);
}

class AggregateASIO final : public IASIO, public CUnknown {
 public:
  AggregateASIO(LPUNKNOWN outer, HRESULT* result)
      : CUnknown(const_cast<TCHAR*>(TEXT("Aggregate Audio")), outer, result) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency_ = frequency.QuadPart;
    LARGE_INTEGER before{}, after{};
    QueryPerformanceCounter(&before);
    const auto milliseconds = timeGetTime();
    QueryPerformanceCounter(&after);
    timestamp_qpc_origin_ = before.QuadPart + (after.QuadPart - before.QuadPart) / 2;
    timestamp_ns_origin_ = static_cast<uint64_t>(milliseconds) * 1'000'000ULL;
  }

  ~AggregateASIO() override {
    stop_settings_monitor();
    stop();
    disposeBuffers();
    if (output_ready_event_) CloseHandle(output_ready_event_);
    if (scheduler_wake_) CloseHandle(scheduler_wake_);
  }

  DECLARE_IUNKNOWN

  static CUnknown* CreateInstance(LPUNKNOWN outer, HRESULT* result) {
    auto* object = new (std::nothrow) AggregateASIO(outer, result);
    if (!object && result) *result = E_OUTOFMEMORY;
    return object;
  }

  HRESULT STDMETHODCALLTYPE NonDelegatingQueryInterface(REFIID iid, void** object) override {
    if (iid == CLSID_AggregateASIO) return GetInterface(static_cast<IASIO*>(this), object);
    return CUnknown::NonDelegatingQueryInterface(iid, object);
  }

  ASIOBool init(void* system_handle) override {
    (void)system_handle;
    std::lock_guard lock(api_mutex_);
    if (initialized_) return ASIOTrue;
    try {
      endpoints_ = enumerate_endpoints();
      settings_ = load_audio_settings();
      canonicalize_endpoint_selections(settings_, endpoints_);
      std::string error;
      if (!engine_.initialize(settings_, endpoints_, error)) {
        set_error(error);
        return ASIOFalse;
      }
      rebuild_channel_bindings();
      sample_rate_.store(settings_.sample_rate, std::memory_order_release);
      buffer_frames_ = settings_.buffer_frames;
      initialized_ = true;
      telemetry_.initialize();
      if (!start_settings_monitor()) {
        initialized_ = false;
        engine_.stop();
        set_error("Could not start the ASIO settings change monitor.");
        return ASIOFalse;
      }
      error_message_[0] = '\0';
      return ASIOTrue;
    } catch (const std::exception& error) {
      set_error(error.what());
      return ASIOFalse;
    }
  }

  void getDriverName(char* name) override { copy_message(name, 32, "Aggregate Audio"); }
  long getDriverVersion() override { return 1; }
  void getErrorMessage(char* message) override { copy_message(message, 128, error_message_); }

  ASIOError start() override {
    std::lock_guard lock(api_mutex_);
    if (!initialized_) return ASE_NotPresent;
    if (!buffers_created_ || !callbacks_) return ASE_InvalidMode;
    if (started_.load(std::memory_order_acquire)) return ASE_OK;
    if (scheduler_.joinable()) {
      if (scheduler_.get_id() == std::this_thread::get_id()) return ASE_InvalidMode;
      scheduler_.join();
    }
    std::string error;
    bool engine_started = false;
    try {
      engine_started = engine_.start(error);
    } catch (const std::exception& exception) {
      engine_.stop();
      set_error(exception.what());
      return ASE_NoMemory;
    } catch (...) {
      engine_.stop();
      set_error("Could not start the selected audio endpoints.");
      return ASE_HWMalfunction;
    }
    if (!engine_started) {
      set_error(error);
      return ASE_HWMalfunction;
    }
    publish_sample_position(0, qpc_nanoseconds());
    telemetry_snapshot_ = {};
    output_zero_run_ = 0;
    output_ready_calls_.store(0, std::memory_order_relaxed);
    active_output_generation_.store(0, std::memory_order_release);
    ready_output_generation_.store(0, std::memory_order_relaxed);
    telemetry_snapshot_.process_id = GetCurrentProcessId();
    telemetry_snapshot_.sample_rate = sample_rate_.load(std::memory_order_relaxed);
    telemetry_snapshot_.buffer_frames = buffer_frames_;
    telemetry_snapshot_.time_info_mode = time_info_mode_;
    telemetry_snapshot_.output_channel_count = static_cast<unsigned>(std::count_if(
        buffers_.begin(), buffers_.end(), [](const auto& buffer) { return !buffer.input; }));
    update_telemetry(true);
    if (!scheduler_wake_) {
      engine_.stop();
      set_error("Could not create the ASIO scheduler wake event.");
      return ASE_NoMemory;
    }
    ResetEvent(scheduler_wake_);
    if (output_ready_event_) ResetEvent(output_ready_event_);
    scheduler_stop_.store(false, std::memory_order_release);
    try {
      scheduler_ = std::thread([this] { scheduler_loop(); });
    } catch (...) {
      engine_.stop();
      set_error("Could not create the ASIO callback thread.");
      return ASE_NoMemory;
    }
    started_.store(true, std::memory_order_release);
    run_generation_.fetch_add(1, std::memory_order_release);
    tray_launch_requested_.store(true, std::memory_order_release);
    return ASE_OK;
  }

  ASIOError stop() override {
    scheduler_stop_.store(true, std::memory_order_release);
    const bool has_scheduler = scheduler_.joinable();
    const bool called_from_scheduler = has_scheduler && scheduler_.get_id() == std::this_thread::get_id();
    if (has_scheduler && !called_from_scheduler) {
      SetEvent(scheduler_wake_);
      scheduler_.join();
    }
    started_.store(false, std::memory_order_release);
    if (!has_scheduler) engine_.stop();
    return ASE_OK;
  }

  ASIOError getChannels(long* inputs, long* outputs) override {
    if (!inputs || !outputs) return ASE_InvalidParameter;
    *inputs = static_cast<long>(input_bindings_.size());
    *outputs = static_cast<long>(output_bindings_.size());
    return ASE_OK;
  }

  ASIOError getLatencies(long* input_latency, long* output_latency) override {
    if (!input_latency || !output_latency) return ASE_InvalidParameter;
    *input_latency = static_cast<long>(engine_.approximate_input_latency_frames());
    *output_latency = static_cast<long>(engine_.approximate_output_latency_frames());
    if (output_ready_enabled_.load(std::memory_order_relaxed) && *output_latency >= static_cast<long>(buffer_frames_)) {
      *output_latency -= static_cast<long>(buffer_frames_);
    }
    return ASE_OK;
  }

  ASIOError getBufferSize(long* minimum, long* maximum, long* preferred,
                          long* granularity) override {
    if (!minimum || !maximum || !preferred || !granularity) return ASE_InvalidParameter;
    *minimum = kMinBufferFrames;
    *maximum = kMaxBufferFrames;
    *preferred = static_cast<long>(buffer_frames_);
    *granularity = -1; // power-of-two sizes
    return ASE_OK;
  }

  ASIOError canSampleRate(ASIOSampleRate rate) override {
    if (!std::isfinite(rate) || rate < 0) return ASE_InvalidParameter;
    return is_supported_rate(static_cast<unsigned>(rate)) &&
                   std::floor(rate) == rate ? ASE_OK : ASE_NoClock;
  }

  ASIOError getSampleRate(ASIOSampleRate* rate) override {
    if (!rate) return ASE_InvalidParameter;
    *rate = sample_rate_.load(std::memory_order_acquire);
    return ASE_OK;
  }

  ASIOError setSampleRate(ASIOSampleRate rate) override {
    if (canSampleRate(rate) != ASE_OK) return ASE_NoClock;
    std::lock_guard lock(api_mutex_);
    if (started_.load(std::memory_order_acquire)) return ASE_InvalidMode;
    const auto new_rate = static_cast<unsigned>(rate);
    if (new_rate == sample_rate_.load(std::memory_order_relaxed)) return ASE_OK;
    sample_rate_.store(new_rate, std::memory_order_release);
    engine_.set_asio_sample_rate(new_rate);
    settings_.sample_rate = new_rate;
    settings_.initialized = true;
    save_audio_settings(settings_);
    callback_sample_rate_changed_.store(true, std::memory_order_release);
    return ASE_OK;
  }

  ASIOError getClockSources(ASIOClockSource* clocks, long* count) override {
    if (!clocks || !count) return ASE_InvalidParameter;
    clocks[0].index = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup = -1;
    clocks[0].isCurrentSource = ASIOTrue;
    strcpy_s(clocks[0].name, "Aggregate software clock");
    *count = 1;
    return ASE_OK;
  }

  ASIOError setClockSource(long index) override {
    if (index != 0) return ASE_NotPresent;
    callback_clock_changed_.store(true, std::memory_order_release);
    return ASE_OK;
  }

  ASIOError getSamplePosition(ASIOSamples* samples, ASIOTimeStamp* timestamp) override {
    if (!samples || !timestamp) return ASE_InvalidParameter;
    uint64_t position = 0, nanos = 0;
    for (;;) {
      const auto sequence = position_sequence_.load(std::memory_order_acquire);
      if (sequence & 1) continue;
      position = sample_position_.load(std::memory_order_relaxed);
      nanos = sample_timestamp_.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (sequence == position_sequence_.load(std::memory_order_relaxed)) break;
    }
    write_samples(*samples, position);
    write_samples(*timestamp, nanos);
    return ASE_OK;
  }

  ASIOError getChannelInfo(ASIOChannelInfo* info) override {
    if (!info || info->channel < 0) return ASE_InvalidParameter;
    const auto& bindings = info->isInput ? input_bindings_ : output_bindings_;
    if (static_cast<size_t>(info->channel) >= bindings.size()) return ASE_InvalidParameter;
    const auto binding = bindings[static_cast<size_t>(info->channel)];
    auto* stream = info->isInput ? engine_.input_stream(binding.endpoint_index)
                                 : engine_.output_stream(binding.endpoint_index);
    if (!stream) return ASE_NotPresent;
    info->type = ASIOSTFloat32LSB;
    info->channelGroup = 0;
    info->isActive = is_channel_active(info->isInput != 0, info->channel) ? ASIOTrue : ASIOFalse;
    const auto channel_name = narrow(stream->name()) +
        (info->isInput ? " - Input " : " - Output ") + std::to_string(binding.endpoint_channel + 1);
    copy_message(info->name, sizeof(info->name), channel_name);
    return ASE_OK;
  }

  ASIOError createBuffers(ASIOBufferInfo* buffer_infos, long num_channels, long size,
                          ASIOCallbacks* callbacks) override {
    std::lock_guard lock(api_mutex_);
    if (!buffer_infos || !callbacks || !callbacks->bufferSwitch || num_channels <= 0 ||
        num_channels > static_cast<long>(kMaximumAsioChannels) ||
        size < kMinBufferFrames || size > kMaxBufferFrames || (size & (size - 1)) != 0) {
      return ASE_InvalidParameter;
    }
    if (!initialized_) return ASE_NotPresent;
    stop();
    dispose_buffer_storage();

    if (engine_needs_refresh_.exchange(false, std::memory_order_acq_rel)) {
      try {
        endpoints_ = enumerate_endpoints();
        settings_ = load_audio_settings();
        canonicalize_endpoint_selections(settings_, endpoints_);
        std::string error;
        if (!engine_.initialize(settings_, endpoints_, error)) {
          engine_needs_refresh_.store(true, std::memory_order_release);
          set_error(error);
          return ASE_HWMalfunction;
        }
        rebuild_channel_bindings();
        sample_rate_.store(settings_.sample_rate, std::memory_order_release);
      } catch (...) {
        engine_needs_refresh_.store(true, std::memory_order_release);
        set_error("Could not refresh selected audio devices.");
        return ASE_HWMalfunction;
      }
    }

    std::vector<AsioBuffer> new_buffers;
    try {
      new_buffers.reserve(static_cast<size_t>(num_channels));
      for (long i = 0; i < num_channels; ++i) {
        const bool input = buffer_infos[i].isInput != 0;
        const long channel = buffer_infos[i].channelNum;
        const auto& bindings = input ? input_bindings_ : output_bindings_;
        if (channel < 0 || static_cast<size_t>(channel) >= bindings.size()) return ASE_InvalidParameter;
        AsioBuffer buffer;
        buffer.input = input;
        buffer.channel = channel;
        buffer.storage.assign(static_cast<size_t>(size) * 2, 0.0f);
        buffer.halves[0] = buffer.storage.data();
        buffer.halves[1] = buffer.storage.data() + size;
        new_buffers.push_back(std::move(buffer));
      }
    } catch (...) {
      return ASE_NoMemory;
    }
    buffer_frames_ = static_cast<unsigned>(size);
    engine_.set_asio_buffer_frames(buffer_frames_);
    settings_.buffer_frames = buffer_frames_;
    buffers_ = std::move(new_buffers);
    callbacks_ = callbacks;
    reset_callbacks_.store(callbacks, std::memory_order_release);
    time_info_mode_ = callbacks_->asioMessage &&
                      callbacks_->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
    std::memset(&time_info_, 0, sizeof(time_info_));
    time_info_.timeInfo.speed = 1.0;
    time_info_.timeInfo.sampleRate = sample_rate_.load(std::memory_order_relaxed);
    time_info_.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
    buffers_created_ = true;
    try {
      rebuild_scratch();
    } catch (...) {
      dispose_buffer_storage();
      return ASE_NoMemory;
    }
    for (long i = 0; i < num_channels; ++i) {
      buffer_infos[i].buffers[0] = buffers_[static_cast<size_t>(i)].halves[0];
      buffer_infos[i].buffers[1] = buffers_[static_cast<size_t>(i)].halves[1];
    }
    settings_.initialized = true;
    save_audio_settings(settings_);
    return ASE_OK;
  }

  ASIOError disposeBuffers() override {
    if (started_.load(std::memory_order_acquire)) stop();
    std::lock_guard lock(api_mutex_);
    dispose_buffer_storage();
    return ASE_OK;
  }

  ASIOError controlPanel() override {
    wchar_t path[MAX_PATH]{};
    const HMODULE module = g_module.load(std::memory_order_acquire);
    if (!module || !GetModuleFileNameW(module, path, MAX_PATH)) return ASE_NotPresent;
    std::wstring config_path(path);
    const auto slash = config_path.find_last_of(L"\\/");
    config_path.resize(slash == std::wstring::npos ? 0 : slash + 1);
    config_path += L"AggregateAudioConfig.exe";
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", config_path.c_str(),
                                                                 L"--configure", nullptr, SW_SHOWNORMAL));
    return result > 32 ? ASE_OK : ASE_NotPresent;
  }

  ASIOError future(long selector, void* option) override {
    switch (selector) {
      case kAsioCanTimeInfo:
        return ASE_SUCCESS;
      case kAsioEngineVersion:
        if (option) *static_cast<long*>(option) = 2;
        return ASE_SUCCESS;
      default:
        return ASE_NotPresent;
    }
  }

  ASIOError outputReady() override {
    if (!output_ready_event_) return ASE_NotPresent;
    output_ready_enabled_.store(true, std::memory_order_relaxed);
    output_ready_calls_.fetch_add(1, std::memory_order_relaxed);
    const auto generation = active_output_generation_.load(std::memory_order_acquire);
    if (generation && !scheduler_stop_.load(std::memory_order_acquire)) {
      ready_output_generation_.store(generation, std::memory_order_release);
      SetEvent(output_ready_event_);
    }
    return ASE_OK;
  }

 private:
  void publish_sample_position(uint64_t position, uint64_t nanos) noexcept {
    position_sequence_.fetch_add(1, std::memory_order_acq_rel);
    sample_position_.store(position, std::memory_order_relaxed);
    sample_timestamp_.store(nanos, std::memory_order_relaxed);
    position_sequence_.fetch_add(1, std::memory_order_release);
  }

  void update_telemetry(bool running) noexcept {
    telemetry_snapshot_.running = running;
    telemetry_snapshot_.output_ready_calls = output_ready_calls_.load(std::memory_order_relaxed);
    long input_latency = 0, output_latency = 0;
    getLatencies(&input_latency, &output_latency);
    telemetry_snapshot_.input_latency_frames = static_cast<unsigned>(input_latency);
    telemetry_snapshot_.output_latency_frames = static_cast<unsigned>(output_latency);
    unsigned index = 0;
    const auto append = [&](bool input) {
      const auto count = input ? engine_.input_stream_count() : engine_.output_stream_count();
      for (size_t i = 0; i < count && index < kTelemetryStreams; ++i, ++index) {
        auto* stream = input ? engine_.input_stream(i) : engine_.output_stream(i);
        auto& target = telemetry_snapshot_.streams[index];
        wcsncpy_s(target.name, stream->name().c_str(), _TRUNCATE);
        target.statistics = stream->statistics();
      }
    };
    append(true); append(false);
    telemetry_snapshot_.stream_count = index;
    telemetry_.publish(telemetry_snapshot_);
  }
  void set_error(const std::string& message) {
    copy_message(error_message_, sizeof(error_message_), message);
  }

  bool is_channel_active(bool input, long channel) const {
    return std::any_of(buffers_.begin(), buffers_.end(), [&](const auto& buffer) {
      return buffer.input == input && buffer.channel == channel;
    });
  }

  void rebuild_channel_bindings() {
    input_bindings_.clear();
    output_bindings_.clear();
    auto add_streams = [this](AudioEngine& engine, bool input, std::vector<ChannelBinding>& result) {
      const auto stream_count = input ? engine.input_stream_count() : engine.output_stream_count();
      for (size_t endpoint = 0; endpoint < stream_count; ++endpoint) {
        auto* stream = input ? engine.input_stream(endpoint) : engine.output_stream(endpoint);
        for (unsigned channel = 0; channel < stream->channels(); ++channel) {
          if (result.size() >= kMaximumAsioChannels) return;
          const auto& saved_keys = input ? settings_.input_channel_keys
                                         : settings_.output_channel_keys;
          if (settings_.channel_selection_configured &&
              std::find(saved_keys.begin(), saved_keys.end(),
                        channel_selection_key(stream->id(), channel)) == saved_keys.end()) {
            continue;
          }
          result.push_back({endpoint, channel});
        }
      }
    };
    add_streams(engine_, true, input_bindings_);
    add_streams(engine_, false, output_bindings_);
  }

  void dispose_buffer_storage() {
    reset_callbacks_.store(nullptr, std::memory_order_release);
    buffers_.clear();
    input_scratch_.clear();
    output_scratch_.clear();
    callbacks_ = nullptr;
    buffers_created_ = false;
  }

  void rebuild_scratch() {
    input_scratch_.clear();
    output_scratch_.clear();
    input_scratch_.resize(engine_.input_stream_count());
    output_scratch_.resize(engine_.output_stream_count());
    for (size_t i = 0; i < input_scratch_.size(); ++i) {
      input_scratch_[i].assign(static_cast<size_t>(buffer_frames_) * engine_.input_stream(i)->channels(), 0.0f);
    }
    for (size_t i = 0; i < output_scratch_.size(); ++i) {
      output_scratch_[i].assign(static_cast<size_t>(buffer_frames_) * engine_.output_stream(i)->channels(), 0.0f);
    }
  }

  bool start_settings_monitor() noexcept {
    if (settings_event_ || settings_monitor_.joinable()) return true;
    settings_event_ = CreateEventW(nullptr, FALSE, FALSE, kSettingsChangedEventName);
    monitor_stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!settings_event_ || !monitor_stop_event_) return false;
    try {
      settings_monitor_ = std::thread([this] { settings_monitor_loop(); });
    } catch (...) {
      CloseHandle(settings_event_);
      CloseHandle(monitor_stop_event_);
      settings_event_ = nullptr;
      monitor_stop_event_ = nullptr;
      return false;
    }
    return true;
  }

  void stop_settings_monitor() noexcept {
    if (monitor_stop_event_) SetEvent(monitor_stop_event_);
    if (settings_monitor_.joinable() && settings_monitor_.get_id() != std::this_thread::get_id()) {
      settings_monitor_.join();
    }
    if (settings_event_) CloseHandle(settings_event_);
    if (monitor_stop_event_) CloseHandle(monitor_stop_event_);
    settings_event_ = nullptr;
    monitor_stop_event_ = nullptr;
  }

  void launch_tray() noexcept {
    const auto module = g_module.load(std::memory_order_acquire);
    if (!module) return; // In-memory driver fixtures do not launch applications.
    try {
      std::array<wchar_t, 32768> module_path{};
      if (!GetModuleFileNameW(module, module_path.data(), static_cast<DWORD>(module_path.size()))) return;
      std::wstring path(module_path.data());
      path.resize(path.find_last_of(L"\\/") + 1);
      path += L"AggregateAudioConfig.exe";
      ShellExecuteW(nullptr, L"open", path.c_str(), L"--tray", nullptr, SW_HIDE);
    } catch (...) {}
  }

  void settings_monitor_loop() noexcept {
    const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
      selected_audio_notifications::Watcher notifications;
      DeviceRecoveryWatchdog recovery;
      uint64_t watched_generation = UINT64_MAX;
      ULONGLONG changed_at = 0;
      bool manual_reset_pending = false;
      bool automatic_reset_pending = false;
      HANDLE waits[]{monitor_stop_event_, settings_event_};
      for (;;) {
        const auto wait = WaitForMultipleObjects(2, waits, FALSE, 250);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
        if (wait == WAIT_OBJECT_0 + 1) manual_reset_pending = true;
        try {
          if (tray_launch_requested_.exchange(false, std::memory_order_acq_rel)) launch_tray();
          const auto now = GetTickCount64();
          const auto generation = run_generation_.load(std::memory_order_acquire);
          std::vector<StreamStatistics> streams;
          std::vector<std::wstring> paths;
          bool running = false;
          {
            std::unique_lock lock(api_mutex_, std::try_to_lock);
            if (!lock.owns_lock()) continue;
            running = started_.load(std::memory_order_acquire);
            for (const bool input : {true, false}) {
              const auto count = input ? engine_.input_stream_count() : engine_.output_stream_count();
              for (size_t index = 0; index < count; ++index) {
                auto* stream = input ? engine_.input_stream(index) : engine_.output_stream(index);
                streams.push_back(stream->statistics());
              }
            }
            if (generation != watched_generation) {
              for (const auto& endpoint : endpoints_) {
                const auto& ids = endpoint.direction == Direction::input ? settings_.input_device_ids : settings_.output_device_ids;
                if (std::find(ids.begin(), ids.end(), endpoint.id) != ids.end()) paths.push_back(endpoint.ks_filter_path);
              }
            }
          }
          if (generation != watched_generation) {
            notifications.initialize(std::move(paths));
            watched_generation = generation;
            changed_at = 0;
          }
          if (notifications.consume()) changed_at = now;
          const bool changed = changed_at && now - changed_at >= 500;
          const bool automatic = recovery.observe(now, generation, scheduler_cycles_.load(std::memory_order_acquire), running, streams, changed);
          automatic_reset_pending |= automatic;
          if (!running) automatic_reset_pending = false;
          if (changed) changed_at = 0;
          auto* callbacks = reset_callbacks_.load(std::memory_order_acquire);
          if (!(manual_reset_pending || automatic_reset_pending) || !callbacks || !callbacks->asioMessage) continue;
          if (!manual_reset_pending) {
            auto previous = g_last_automatic_reset.load(std::memory_order_acquire);
            if ((previous && now - previous < 5000) ||
                !g_last_automatic_reset.compare_exchange_strong(previous, now, std::memory_order_acq_rel)) continue;
          }
          const bool automatic_request = !manual_reset_pending;
          engine_needs_refresh_.store(true, std::memory_order_release);
          manual_reset_pending = false;
          automatic_reset_pending = false;
          callbacks->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
          if (automatic_request) {
            if (const auto window = FindWindowW(L"AggregateAudioConfigWindow", nullptr))
              PostMessageW(window, WM_APP + 30, 0, 0);
          }
        } catch (...) {} // A notification failure must not stop audio callbacks.
      }
    }
    if (SUCCEEDED(com_result)) CoUninitialize();
  }

  uint64_t qpc_nanoseconds() const noexcept {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    if (qpc_frequency_ <= 0) return 0;
    // ASIO's Windows timestamp epoch is timeGetTime, not raw QPC. Use QPC
    // only for interpolation within that clock, avoiding millisecond steps.
    return timestamp_ns_origin_ + static_cast<uint64_t>(
        (static_cast<long double>(counter.QuadPart - timestamp_qpc_origin_) * 1'000'000'000.0L) /
        static_cast<long double>(qpc_frequency_));
  }

  void prepare_inputs(unsigned half) noexcept {
    for (size_t endpoint = 0; endpoint < input_scratch_.size(); ++endpoint) {
      auto* stream = engine_.input_stream(endpoint);
      stream->read_input(input_scratch_[endpoint].data(), buffer_frames_);
    }
    for (auto& buffer : buffers_) {
      if (!buffer.input) continue;
      const auto binding = input_bindings_[static_cast<size_t>(buffer.channel)];
      const unsigned endpoint_channels = engine_.input_stream(binding.endpoint_index)->channels();
      const float* source = input_scratch_[binding.endpoint_index].data() + binding.endpoint_channel;
      float* target = buffer.halves[half];
      for (unsigned frame = 0; frame < buffer_frames_; ++frame) {
        target[frame] = source[static_cast<size_t>(frame) * endpoint_channels];
      }
    }
  }

  void submit_outputs(unsigned half) noexcept {
    float peak = 0;
    for (auto& scratch : output_scratch_) std::fill(scratch.begin(), scratch.end(), 0.0f);
    for (const auto& buffer : buffers_) {
      if (buffer.input) continue;
      const auto binding = output_bindings_[static_cast<size_t>(buffer.channel)];
      const unsigned endpoint_channels = engine_.output_stream(binding.endpoint_index)->channels();
      float* target = output_scratch_[binding.endpoint_index].data() + binding.endpoint_channel;
      const float* source = buffer.halves[half];
      for (unsigned frame = 0; frame < buffer_frames_; ++frame) {
        const float value = source[frame];
        if (std::isfinite(value)) {
          target[static_cast<size_t>(frame) * endpoint_channels] = value;
          peak = std::max(peak, std::abs(value));
        } else ++telemetry_snapshot_.nonfinite_samples;
      }
    }
    for (size_t endpoint = 0; endpoint < output_scratch_.size(); ++endpoint) {
      engine_.output_stream(endpoint)->push_output(output_scratch_[endpoint].data(), buffer_frames_);
    }
    for (unsigned frame = 0; frame < buffer_frames_; ++frame) {
      bool zero = true;
      for (const auto& scratch : output_scratch_) {
        const size_t channels = scratch.size() / buffer_frames_;
        for (size_t channel = 0; channel < channels; ++channel) {
          if (scratch[static_cast<size_t>(frame) * channels + channel] != 0.0f) zero = false;
        }
      }
      if (zero) {
        ++telemetry_snapshot_.output_zero_frames;
        ++output_zero_run_;
        telemetry_snapshot_.output_max_zero_run = std::max(telemetry_snapshot_.output_max_zero_run, output_zero_run_);
      } else output_zero_run_ = 0;
    }
    telemetry_snapshot_.output_peak = std::max(telemetry_snapshot_.output_peak, peak);
    if (peak == 0) ++telemetry_snapshot_.silent_blocks;
  }

  void scheduler_loop() noexcept {
    DWORD mmcss_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_index);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!timer) {
      set_error("Could not create a high-resolution ASIO timer.");
      if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
      started_.store(false, std::memory_order_release);
      engine_.stop();
      return;
    }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart <= 0) {
      set_error("Windows did not provide a usable performance counter.");
      CloseHandle(timer);
      if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
      started_.store(false, std::memory_order_release);
      engine_.stop();
      return;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const auto rate = sample_rate_.load(std::memory_order_acquire);
    const int64_t period_ticks = std::max<int64_t>(1,
        static_cast<int64_t>((static_cast<long double>(buffer_frames_) * frequency.QuadPart) / rate));
    int64_t next = now.QuadPart + period_ticks;
    uint64_t position = 0;
    uint64_t previous_callback_ns = 0;
    const uint64_t start_ns = qpc_nanoseconds();
    const uint64_t period_ns = static_cast<uint64_t>(buffer_frames_) * 1'000'000'000ULL / rate;
    uint64_t telemetry_deadline = start_ns;
    unsigned half = 0;
    uint64_t generation = 0;
    unsigned pending_half = 1;
    bool output_submitted = true;
    const auto submit_ready_output = [&] {
      if (!output_submitted && ready_output_generation_.load(std::memory_order_acquire) == generation) {
        submit_outputs(pending_half);
        output_submitted = true;
        ++telemetry_snapshot_.early_output_blocks;
      }
    };
    while (!scheduler_stop_.load(std::memory_order_acquire)) {
      QueryPerformanceCounter(&now);
      const auto remaining = std::max<int64_t>(1, next - now.QuadPart);
      LARGE_INTEGER due{};
      due.QuadPart = -std::max<int64_t>(1,
          static_cast<int64_t>((static_cast<long double>(remaining) * 10'000'000.0L) /
                               frequency.QuadPart));
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      HANDLE waits[]{timer, scheduler_wake_, output_ready_event_};
      const DWORD wait = WaitForMultipleObjects(output_ready_event_ ? 3 : 2, waits, FALSE, INFINITE);
      if (wait == WAIT_OBJECT_0 + 1 || scheduler_stop_.load(std::memory_order_acquire)) break;
      if (wait == WAIT_OBJECT_0 + 2) {
        submit_ready_output();
        continue;
      }
      if (wait != WAIT_OBJECT_0) continue;
      const unsigned current_rate = sample_rate_.load(std::memory_order_acquire);
      const unsigned current_buffer = buffer_frames_;
      // Hosts without outputReady use the preceding half at the next period.
      // The scheduler is the sole ring producer in both hand-off modes.
      if (generation == 0 || !output_submitted) submit_outputs(half ^ 1u);
      prepare_inputs(half);
      pending_half = half;
      output_submitted = false;
      active_output_generation_.store(++generation, std::memory_order_release);
      const auto nanos = qpc_nanoseconds();
      publish_sample_position(position, nanos);
      if (previous_callback_ns) {
        const auto interval = nanos - previous_callback_ns;
        telemetry_snapshot_.max_interval_ns = std::max(telemetry_snapshot_.max_interval_ns, interval);
        if (interval > period_ns * 3 / 2) telemetry_snapshot_.missed_periods += interval / period_ns - 1;
      }
      previous_callback_ns = nanos;
      if (time_info_mode_ && callbacks_ && callbacks_->bufferSwitchTimeInfo) {
        time_info_.timeInfo.speed = 1.0;
        time_info_.timeInfo.sampleRate = current_rate;
        write_samples(time_info_.timeInfo.samplePosition, position);
        write_samples(time_info_.timeInfo.systemTime, nanos);
        time_info_.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
        if (callback_sample_rate_changed_.exchange(false, std::memory_order_acq_rel)) {
          time_info_.timeInfo.flags |= kSampleRateChanged;
        }
        if (callback_clock_changed_.exchange(false, std::memory_order_acq_rel)) {
          time_info_.timeInfo.flags |= kClockSourceChanged;
        }
        callbacks_->bufferSwitchTimeInfo(&time_info_, static_cast<long>(half), ASIOTrue);
      } else if (callbacks_ && callbacks_->bufferSwitch) {
        if (callback_sample_rate_changed_.exchange(false, std::memory_order_acq_rel) &&
            callbacks_->sampleRateDidChange) callbacks_->sampleRateDidChange(current_rate);
        callback_clock_changed_.store(false, std::memory_order_release);
        callbacks_->bufferSwitch(static_cast<long>(half), ASIOTrue);
      }
      submit_ready_output();
      position += current_buffer;
      ++telemetry_snapshot_.callbacks;
      scheduler_cycles_.fetch_add(1, std::memory_order_release);
      const auto end_ns = qpc_nanoseconds();
      telemetry_snapshot_.max_callback_ns = std::max(telemetry_snapshot_.max_callback_ns, end_ns - nanos);
      telemetry_snapshot_.elapsed_ns = end_ns - start_ns;
      if (end_ns >= telemetry_deadline) {
        update_telemetry(true);
        telemetry_deadline = end_ns + 250'000'000ULL;
      }
      half ^= 1;
      QueryPerformanceCounter(&now);
      next += period_ticks;
      // Preserve the clock when a callback/timer runs a little late. Resetting
      // the next deadline to now + one block loses samples on every late tick
      // and starves every output, irrespective of device buffer size.
      if (next + period_ticks * 4 < now.QuadPart) next = now.QuadPart - period_ticks * 4;
    }
    CancelWaitableTimer(timer);
    active_output_generation_.store(0, std::memory_order_release);
    CloseHandle(timer);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    started_.store(false, std::memory_order_release);
    engine_.stop();
    update_telemetry(false);
  }

  std::mutex api_mutex_;
  AudioEngine engine_;
  std::vector<AudioEndpoint> endpoints_;
  AudioSettings settings_;
  std::vector<ChannelBinding> input_bindings_;
  std::vector<ChannelBinding> output_bindings_;
  std::vector<AsioBuffer> buffers_;
  std::vector<std::vector<float>> input_scratch_;
  std::vector<std::vector<float>> output_scratch_;
  ASIOCallbacks* callbacks_{};
  std::atomic<ASIOCallbacks*> reset_callbacks_{};
  std::atomic<bool> engine_needs_refresh_{};
  std::atomic<bool> tray_launch_requested_{};
  std::atomic<uint64_t> run_generation_{};
  std::atomic<uint64_t> scheduler_cycles_{};
  ASIOTime time_info_{};
  std::atomic<unsigned> sample_rate_{48000};
  std::atomic<uint64_t> sample_position_{0};
  std::atomic<uint64_t> sample_timestamp_{0};
  std::atomic<uint64_t> position_sequence_{0};
  std::atomic<uint64_t> output_ready_calls_{0};
  std::atomic<bool> output_ready_enabled_{false};
  std::atomic<uint64_t> active_output_generation_{0};
  std::atomic<uint64_t> ready_output_generation_{0};
  DriverTelemetry telemetry_;
  DriverTelemetrySnapshot telemetry_snapshot_{};
  uint64_t output_zero_run_{};
  int64_t qpc_frequency_{};
  int64_t timestamp_qpc_origin_{};
  uint64_t timestamp_ns_origin_{};
  unsigned buffer_frames_{128};
  std::atomic<bool> scheduler_stop_{true};
  std::atomic<bool> started_{false};
  std::atomic<bool> callback_sample_rate_changed_{false};
  std::atomic<bool> callback_clock_changed_{false};
  std::thread scheduler_;
  std::thread settings_monitor_;
  HANDLE scheduler_wake_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  HANDLE output_ready_event_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  HANDLE settings_event_{};
  HANDLE monitor_stop_event_{};
  bool initialized_{};
  bool buffers_created_{};
  bool time_info_mode_{};
  char error_message_[128]{};
};

} // namespace

CFactoryTemplate g_Templates[1] = {{L"Aggregate Audio", &CLSID_AggregateASIO,
                                     AggregateASIO::CreateInstance, nullptr}};
int g_cTemplates = 1;

HINSTANCE g_hInst = nullptr;
BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_hInst = instance;
    g_module.store(instance, std::memory_order_release);
  }
  return DllEntryPoint(instance, reason, reserved);
}

extern LONG RegisterAsioDriver(CLSID, char*, char*, char*, char*);
extern LONG UnregisterAsioDriver(CLSID, char*, char*);

extern "C" HRESULT __stdcall DllRegisterServer() {
  const LONG result = RegisterAsioDriver(
      CLSID_AggregateASIO, const_cast<char*>("AggregateAudio.dll"),
      const_cast<char*>("Aggregate Audio"),
      const_cast<char*>("Aggregate Audio"),
      const_cast<char*>("Apartment"));
  if (result != 0) return E_FAIL;

  // Hosts can display Description rather than the ASIO key name. The SDK
  // helper also keeps an existing COM description when the DLL path matches;
  // overwrite it so re-registering upgrades the name on existing installs.
  constexpr wchar_t display_name[] = L"Aggregate Audio";
  LONG status = RegSetKeyValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO\\Aggregate Audio",
      L"Description", REG_SZ, display_name, sizeof(display_name));
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  status = RegSetKeyValueW(HKEY_CLASSES_ROOT,
      L"CLSID\\{D3AA79B2-9D91-4B6F-A2B1-55CE84C2A43D}", nullptr,
      REG_SZ, display_name, sizeof(display_name));
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

  // Steinberg's helper does not propagate registry write failures. Verify the
  // ASIO host discovery entry before reporting successful self-registration.
  HKEY asio_key{};
  status = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                              L"SOFTWARE\\ASIO\\Aggregate Audio", 0,
                              KEY_QUERY_VALUE, &asio_key);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

  wchar_t registered_clsid[64]{};
  DWORD value_type = 0;
  DWORD value_size = sizeof(registered_clsid);
  status = RegQueryValueExW(asio_key, L"CLSID", nullptr, &value_type,
                            reinterpret_cast<LPBYTE>(registered_clsid),
                            &value_size);
  RegCloseKey(asio_key);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  if (value_type != REG_SZ ||
      _wcsicmp(registered_clsid,
               L"{D3AA79B2-9D91-4B6F-A2B1-55CE84C2A43D}") != 0) {
    return E_FAIL;
  }

  return S_OK;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
  constexpr LONG ok = 0;
  return UnregisterAsioDriver(CLSID_AggregateASIO, const_cast<char*>("AggregateAudio.dll"),
                              const_cast<char*>("Aggregate Audio")) == ok ? S_OK : E_FAIL;
}
