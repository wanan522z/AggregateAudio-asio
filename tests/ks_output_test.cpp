#include "audio_engine.h"
#include <windows.h>
#include <winioctl.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace {
std::atomic<bool> invalid_header{};
std::atomic<unsigned> nonzero_writes{};
std::array<float, 512> rt_samples{};
std::atomic<HANDLE> rt_notification{};
std::atomic<ULONGLONG> play_offset{};
bool wave_rt{};
struct PendingWrite {
  HANDLE pin{};
  KSSTREAM_HEADER* header{};
  std::vector<BYTE> snapshot;
  bool complete{};
  DWORD status{ERROR_SUCCESS};
};
std::mutex pending_mutex;
std::unordered_map<LPOVERLAPPED, PendingWrite> pending_writes;
std::deque<LPOVERLAPPED> hardware_queue;
std::vector<float> played;
std::atomic<unsigned> direct_writes_remaining{};
std::atomic<unsigned> cancelled_writes{};

BOOL WINAPI fake_cancel_io(HANDLE pin, LPOVERLAPPED request) {
  std::lock_guard lock(pending_mutex);
  for (auto& [io, write] : pending_writes) {
    if (write.pin != pin || (request && request != io) || write.complete) continue;
    write.complete = true;
    write.status = ERROR_OPERATION_ABORTED;
    std::erase(hardware_queue, io);
    cancelled_writes++;
    SetEvent(io->hEvent);
  }
  return TRUE;
}
BOOL WINAPI fake_overlapped_result(HANDLE, LPOVERLAPPED io, LPDWORD bytes, BOOL wait) {
  if (wait) WaitForSingleObject(io->hEvent, 1000);
  std::lock_guard lock(pending_mutex);
  const auto found = pending_writes.find(io);
  if (found == pending_writes.end() || !found->second.complete) {
    SetLastError(ERROR_IO_INCOMPLETE); return FALSE;
  }
  *bytes = sizeof(KSSTREAM_HEADER);
  const DWORD status = found->second.status;
  pending_writes.erase(found);
  SetLastError(status);
  return status == ERROR_SUCCESS;
}

HANDLE WINAPI fake_create_file(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) {
  return CreateEventW(nullptr, FALSE, FALSE, nullptr);
}
DWORD WINAPI fake_create_pin(HANDLE, PKSPIN_CONNECT, ACCESS_MASK, PHANDLE pin) {
  *pin = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  return *pin ? ERROR_SUCCESS : GetLastError();
}
BOOL WINAPI fake_ioctl(HANDLE pin, DWORD code, LPVOID input, DWORD input_size,
                       LPVOID output, DWORD output_size, LPDWORD returned, LPOVERLAPPED io) {
  *returned = 0;
  if (code == IOCTL_KS_WRITE_STREAM || code == IOCTL_KS_READ_STREAM) {
    if (input || input_size || !output || output_size != sizeof(KSSTREAM_HEADER)) {
      invalid_header = true; SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
    }
    auto* header = static_cast<KSSTREAM_HEADER*>(output);
    if (header->Size != sizeof(*header) || !header->Data || !header->FrameExtent ||
        header->PresentationTime.Numerator != 1 || header->PresentationTime.Denominator != 1) invalid_header = true;
    const auto samples = header->FrameExtent / sizeof(float);
    auto* data = static_cast<float*>(header->Data);
    if (code == IOCTL_KS_WRITE_STREAM) {
      if (header->DataUsed != header->FrameExtent) invalid_header = true;
      if (std::any_of(data, data + samples, [](float sample) { return sample != 0; })) nonzero_writes++;
      if (!io || !io->hEvent) { invalid_header = true; return FALSE; }
      std::lock_guard lock(pending_mutex);
      if (pending_writes.contains(io)) invalid_header = true;
      PendingWrite write;
      write.pin = pin; write.header = header;
      const auto* bytes = static_cast<const BYTE*>(header->Data);
      write.snapshot.assign(bytes, bytes + header->DataUsed);
      if (direct_writes_remaining > 0) {
        direct_writes_remaining--;
        write.complete = true;
        pending_writes[io] = std::move(write);
        *returned = sizeof(*header);
        return TRUE;
      }
      pending_writes[io] = std::move(write);
      hardware_queue.push_back(io);
      SetLastError(ERROR_IO_PENDING);
      return FALSE;
    } else {
      std::fill_n(data, samples, 0.25f);
      header->DataUsed = header->FrameExtent;
      if (!io || !io->hEvent) { invalid_header = true; return FALSE; }
      std::lock_guard lock(pending_mutex);
      if (pending_writes.contains(io)) invalid_header = true;
      PendingWrite read;
      read.pin = pin;
      read.header = header;
      read.complete = true;
      pending_writes[io] = std::move(read);
    }
    *returned = sizeof(*header);
    Sleep(1);
    return TRUE;
  }
  if (code == IOCTL_KS_PROPERTY && input) {
    const auto& property = *static_cast<const KSPROPERTY*>(input);
    if (property.Flags == KSPROPERTY_TYPE_BASICSUPPORT) {
      if (wave_rt && property.Set == KSPROPSETID_RtAudio &&
          property.Id == KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION && output_size >= sizeof(ULONG)) {
        *static_cast<ULONG*>(output) = KSPROPERTY_TYPE_GET;
        *returned = sizeof(ULONG); return TRUE;
      }
      SetLastError(ERROR_NOT_SUPPORTED); return FALSE;
    }
    if (property.Set == KSPROPSETID_Connection && property.Id == KSPROPERTY_CONNECTION_STATE) return TRUE;
    if (wave_rt && property.Set == KSPROPSETID_RtAudio) {
      if (property.Id == KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION) {
        if (output_size < sizeof(KSRTAUDIO_BUFFER)) {
          SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE;
        }
        auto* buffer = static_cast<KSRTAUDIO_BUFFER*>(output);
        *buffer = {rt_samples.data(), static_cast<ULONG>(sizeof(rt_samples)), TRUE};
        *returned = sizeof(*buffer); return TRUE;
      }
      if (property.Id == KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT) {
        rt_notification = static_cast<KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY*>(input)->NotificationEvent;
        return TRUE;
      }
      if (property.Id == KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT) { rt_notification = nullptr; return TRUE; }
    }
    if (wave_rt && property.Set == KSPROPSETID_Audio && property.Id == KSPROPERTY_AUDIO_POSITION) {
      auto* position = static_cast<KSAUDIO_POSITION*>(output);
      *position = {play_offset.load(), play_offset.load()};
      *returned = sizeof(*position); return TRUE;
    }
  }
  SetLastError(ERROR_NOT_SUPPORTED);
  return FALSE;
}
}

#define CreateFileW fake_create_file
#define KsCreatePin fake_create_pin
#define DeviceIoControl fake_ioctl
#define CancelIoEx fake_cancel_io
#define GetOverlappedResult fake_overlapped_result
#include "../src/audio_engine.cpp"
#undef CreateFileW
#undef KsCreatePin
#undef DeviceIoControl
#undef CancelIoEx
#undef GetOverlappedResult

unsigned device_buffer_frames(const AudioSettings& settings, const std::wstring&) { return settings.buffer_frames; }

namespace {
AudioEndpoint endpoint(Direction direction) {
  AudioEndpoint result;
  result.id = direction == Direction::input ? L"test-input" : L"test-output";
  result.direction = direction;
  result.name = result.id;
  result.channels = 2;
  result.native_sample_rates = {48000};
  return result;
}
bool wait_packets(EndpointAudioStream* stream, unsigned count) {
  const auto deadline = GetTickCount64() + 2000;
  while (stream->processed_packets() < count && GetTickCount64() < deadline) Sleep(1);
  return stream->processed_packets() >= count;
}
bool wait_queue(unsigned count) {
  const auto deadline = GetTickCount64() + 1000;
  do {
    { std::lock_guard lock(pending_mutex); if (hardware_queue.size() >= count) return true; }
    Sleep(1);
  } while (GetTickCount64() < deadline);
  return false;
}
bool complete_next() {
  std::lock_guard lock(pending_mutex);
  if (hardware_queue.empty()) return false;
  const auto io = hardware_queue.front(); hardware_queue.pop_front();
  auto& write = pending_writes.at(io);
  if (std::memcmp(write.header->Data, write.snapshot.data(), write.snapshot.size()) != 0) invalid_header = true;
  const auto* signal = static_cast<const float*>(write.header->Data);
  played.insert(played.end(), signal, signal + write.header->DataUsed / sizeof(float));
  write.complete = true;
  SetEvent(io->hEvent);
  return true;
}
bool test_fractional_phase() {
  AudioRing ring(1, 16384);
  std::vector<float> ramp(4096);
  for (unsigned i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i);
  ring.push(ramp.data(), static_cast<unsigned>(ramp.size()));
  double position = 0;
  std::array<float, 37> block{};
  for (unsigned pass = 0; pass < 50; ++pass) {
    const auto fill = ring.fill_frames();
    const double ratio = 1.00037 * (1 + std::clamp((static_cast<double>(fill) - 2048) * 0.00000075, -0.0015, 0.0015));
    ring.read_resampled(block.data(), static_cast<unsigned>(block.size()), 2048, 1.00037);
    for (unsigned i = 0; i < block.size(); ++i) {
      if (std::abs(block[i] - (position + i * ratio)) > 0.0003) return false;
    }
    position += block.size() * ratio;
  }
  ring.reset();
  return ring.fill_frames() == 0;
}

bool test_bridge_latency() {
  bool passed = true;
  for (unsigned rate : {44100u, 48000u, 96000u}) {
    for (unsigned host : {64u, 128u, 256u, 512u, 1024u, 2048u}) {
      unsigned previous_input = 0, previous_output = 0;
      for (unsigned device : {64u, 128u, 256u, 512u, 1024u, 2048u}) {
        const auto in = bridge_buffer_plan(device, host, rate);
        const auto out = bridge_buffer_plan(host, device, rate);
        const unsigned input_latency = std::max(in.prime, in.target) + device;
        const unsigned output_latency = std::max(out.prime, out.target) + device * 2 + host / 2;
        passed = input_latency >= previous_input && output_latency >= previous_output && passed;
        previous_input = input_latency;
        previous_output = output_latency;
      }
    }
  }
  const auto plan = bridge_buffer_plan(64, 256, 48000);
  AudioRing ring(2, 16384);
  std::vector<float> ramp(8192 * 2);
  for (unsigned frame = 0; frame < 8192; ++frame) {
    ramp[frame * 2] = 0.05f + frame * 0.00001f;
    ramp[frame * 2 + 1] = -ramp[frame * 2];
  }
  std::array<float, 512> block{};
  ring.push(ramp.data(), 512);
  ring.read_resampled(block.data(), 256, plan.target);
  const float previous = block[510];
  ring.push(ramp.data() + 1024, 4096); // Simulate an ASIO/device stall.
  const auto discarded = ring.trim_backlog(plan.limit, plan.recovery);
  const auto missing = ring.read_resampled(block.data(), 256, plan.target);
  passed = discarded > 3000 && missing == 0 && ring.fill_frames() < plan.limit && passed;
  passed = block[0] > previous && block[0] - previous < 0.002f && block[64] > 0.09f && passed;
  for (unsigned frame = 0; frame < 256; ++frame) {
    passed = std::isfinite(block[frame * 2]) && block[frame * 2 + 1] == -block[frame * 2] && passed;
  }
  // Clock drift at a mismatched device/host block size must not trigger
  // repeated trimming or underruns during normal streaming.
  for (const bool capture : {false, true}) {
    AudioRing stream(1, 16384);
    const unsigned producer = capture ? 64 : 256, consumer = capture ? 256 : 64;
    const double ratio = capture ? 1.0001 : 1.0 / 1.0001;
    const auto budget = bridge_buffer_plan(producer, consumer * ratio, 48000);
    std::vector<float> signal(producer, 0.25f), result(consumer);
    while (stream.fill_frames() < budget.prime) stream.push(signal.data(), producer);
    double produced = 0;
    for (unsigned packet = 0; packet < 1000; ++packet) {
      produced += consumer * ratio;
      while (produced >= producer) { stream.push(signal.data(), producer); produced -= producer; }
      passed = stream.trim_backlog(budget.limit, budget.recovery) == 0 && passed;
      passed = stream.read_resampled(result.data(), consumer, budget.target, ratio) == 0 && passed;
    }
  }
  return passed;
}
}

int main() {
  AudioSettings settings;
  settings.buffer_frames = 128;
  const auto input = endpoint(Direction::input), output = endpoint(Direction::output);
  settings.input_device_ids = {input.id};
  settings.output_device_ids = {output.id};
  std::string error;
  bool passed = true;
  {
    AudioEngine engine;
    if (!engine.initialize(settings, {input, output}, error) || !engine.start(error)) return 1;
    std::array<float, 256> signal{}, recorded{};
    unsigned frame_index = 0;
    const auto produce = [&] {
      for (unsigned frame = 0; frame < 128; ++frame, ++frame_index) {
        signal[frame * 2] = 0.05f + frame_index * 0.00001f;
        signal[frame * 2 + 1] = -signal[frame * 2];
      }
      engine.output_stream(0)->push_output(signal.data(), 128);
    };
    passed = wait_queue(2);
    for (unsigned block = 0; block < 4; ++block) produce();
    for (unsigned block = 0; block < 30; ++block) {
      produce();
      passed = complete_next() && passed;
      passed = wait_queue(2) && passed;
    }
    // Await observable capture data instead of assuming its worker has primed
    // after a five-millisecond sleep on every Windows timer configuration.
    const auto capture_deadline = GetTickCount64() + 2000;
    do {
      engine.input_stream(0)->read_input(recorded.data(), 128);
      if (recorded[0] == 0.25f) break;
      Sleep(1);
    } while (GetTickCount64() < capture_deadline);
    passed = !invalid_header && nonzero_writes > 0 && recorded[0] == 0.25f && passed;
    bool started = false;
    float previous = 0;
    for (size_t i = 0; i < played.size(); i += 2) {
      if (!started && played[i] == 0) continue;
      if (started && (played[i] - previous < 0.000008f || played[i] - previous > 0.000012f)) passed = false;
      if (played[i + 1] != -played[i]) passed = false;
      started = true; previous = played[i];
    }
    passed = started && passed;
    passed = engine.output_stream(0)->render_underruns() == 0 && passed;
    const auto before_stop = GetTickCount64();
    engine.stop();
    passed = GetTickCount64() - before_stop < 1000 && cancelled_writes >= 2 && passed;
    { std::lock_guard lock(pending_mutex); passed = pending_writes.empty() && passed; }
    direct_writes_remaining = 1;
    bool restarted = engine.start(error) && wait_queue(2);
    engine.stop();
    { std::lock_guard lock(pending_mutex); restarted = pending_writes.empty() && restarted; }
    passed = restarted && passed;
    if (!passed) std::printf("  invalid-header=%d nonzero-writes=%u capture=%.8f played=%zu cancelled=%u restarted=%d\n",
        invalid_header.load(), nonzero_writes.load(), recorded[0], played.size(), cancelled_writes.load(), restarted);
    std::printf("KS continuous queue, sample order, cancellation, direct completion and restart: %s\n", passed ? "PASS" : "FAIL");
  }
  const bool phase = test_fractional_phase();
  std::printf("Resampling preserves fractional phase across 50 blocks: %s\n", phase ? "PASS" : "FAIL");
  passed = phase && passed;
  const bool latency = test_bridge_latency();
  std::printf("Monotonic buffer budgets, bounded stall recovery and mismatched block clocks: %s\n", latency ? "PASS" : "FAIL");
  passed = latency && passed;
  wave_rt = true;
  settings.input_device_ids.clear();
  {
    AudioEngine engine;
    if (!engine.initialize(settings, {output}, error) || !engine.start(error)) return 1;
    auto* stream = engine.output_stream(0);
    std::array<float, 1024> signal{};
    signal.fill(0.375f);
    // An incomplete first ASIO block must be retained while output primes.
    stream->push_output(signal.data(), 128);
    play_offset = sizeof(rt_samples) / 2;
    SetEvent(rt_notification.load());
    bool correct = wait_packets(stream, 1) && rt_samples[0] == 0;
    stream->push_output(signal.data(), 384);
    play_offset = sizeof(rt_samples) / 2; // Hardware plays half 1; refill half 0.
    SetEvent(rt_notification.load());
    correct = wait_packets(stream, 2) && correct && rt_samples[0] == 0.375f && rt_samples[256] == 0;
    // A full cycle's notification was missed. The cursor is still in half 1.
    SetEvent(rt_notification.load());
    correct = wait_packets(stream, 3) && correct && rt_samples[0] == 0.375f && rt_samples[256] == 0;
    engine.stop();
    // Restart must clear stale audio before playing again.
    correct = engine.start(error) && correct && rt_samples[0] == 0;
    engine.stop();
    std::printf("WaveRT cursor after skipped notification and restart: %s\n", correct ? "PASS" : "FAIL");
    passed = correct && passed;
  }
  return passed ? 0 : 1;
}
