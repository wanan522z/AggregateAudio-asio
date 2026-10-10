#include "audio_engine.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace {
std::mutex captured_mutex;
std::vector<std::array<float, 2>> captured;
std::atomic<bool> bad_channel_data{};
std::vector<ULONGLONG> output_times;
std::atomic<unsigned> mock_packets{}, reset_requests{}, engine_initializations{};
std::atomic<bool> mock_stalled{};

ULONGLONG host_timestamp_ms() {
  // GetTickCount64 can quantize a short handoff to a 15.6 ms tick. Measure the
  // simulated host's processing interval with the same monotonic QPC clock.
  return static_cast<ULONGLONG>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct TestStream {
  StreamStatistics statistics() const {
    StreamStatistics value{};
    value.flags = 4; value.packets = mock_packets.load();
    value.sample_rate = 48000; value.buffer_frames = 1024; value.channels = 2;
    return value;
  }
  unsigned channels() const { return 2; }
  const std::wstring& id() const { static const std::wstring value = L"test"; return value; }
  const std::wstring& name() const { return id(); }
  void read_input(float* target, unsigned frames) { std::fill_n(target, frames * 2, 0.25f); }
  void push_output(const float* source, unsigned frames) {
    if (!mock_stalled) ++mock_packets;
    for (unsigned frame = 0; frame < frames; ++frame) {
      if (source[frame * 2] != source[0] || source[frame * 2 + 1] != source[1]) bad_channel_data = true;
    }
    std::lock_guard lock(captured_mutex);
    captured.push_back({source[0], source[1]});
    output_times.push_back(host_timestamp_ms());
  }
};

struct TestEngine {
  TestStream stream;
  bool initialize(const AudioSettings&, const std::vector<AudioEndpoint>&, std::string&) {
    ++engine_initializations;
    mock_stalled = false;
    return true;
  }
  bool start(std::string&) { return true; }
  void stop() {}
  void set_asio_sample_rate(unsigned) {}
  void set_asio_buffer_frames(unsigned) {}
  unsigned approximate_input_latency_frames() const { return 0; }
  unsigned approximate_output_latency_frames() const { return 4096; }
  size_t input_stream_count() const { return 1; }
  size_t output_stream_count() const { return 1; }
  TestStream* input_stream(size_t) { return &stream; }
  TestStream* output_stream(size_t) { return &stream; }
};
}

AudioSettings load_audio_settings() { AudioSettings settings; settings.initialized = true; return settings; }
bool save_audio_settings(const AudioSettings&) { return true; }
std::vector<AudioEndpoint> enumerate_endpoints() { return {}; }
std::wstring channel_selection_key(const std::wstring& id, unsigned channel) { return id + std::to_wstring(channel); }

#define AudioEngine TestEngine
#include "../src/asio_driver.cpp"
#undef AudioEngine

BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID) { return TRUE; }
LONG RegisterAsioDriver(CLSID, char*, char*, char*, char*) { return 0; }
LONG UnregisterAsioDriver(CLSID, char*, char*) { return 0; }

namespace {
constexpr long kFrames = 1024;
ASIOBufferInfo host_buffers[3]{{ASIOFalse, 1, {}}, {ASIOTrue, 0, {}}, {ASIOFalse, 0, {}}};
std::atomic<unsigned> callback_count{};
std::mutex jobs_mutex;
std::condition_variable jobs_ready;
std::deque<std::pair<long, float>> deferred_jobs;
std::thread deferred_worker;
bool worker_stop{};
bool deferred_processing{};
bool use_time_info{};
bool notify_ready{};
std::array<ULONGLONG, 32> callback_times{};
AggregateASIO* active_driver{};
std::atomic<bool> bad_clock{};
uint64_t last_callback_timestamp{};
template<class Pair>
uint64_t pair_value(const Pair& value) { return (static_cast<uint64_t>(value.hi) << 32) | value.lo; }
void verify_clock(uint64_t expected_position, const ASIOTime* time = nullptr) {
  ASIOSamples samples{}; ASIOTimeStamp timestamp{};
  if (active_driver->getSamplePosition(&samples, &timestamp) != ASE_OK ||
      pair_value(samples) != expected_position) bad_clock = true;
  if (time && (pair_value(time->timeInfo.samplePosition) != pair_value(samples) ||
      pair_value(time->timeInfo.systemTime) != pair_value(timestamp))) bad_clock = true;
  const uint64_t system_ns = static_cast<uint64_t>(timeGetTime()) * 1'000'000ULL;
  const uint64_t actual_ns = pair_value(timestamp);
  if ((actual_ns > system_ns ? actual_ns - system_ns : system_ns - actual_ns) > 40'000'000ULL) bad_clock = true;
  last_callback_timestamp = pair_value(timestamp);
}

void fill_output(long half, float value) {
  std::fill_n(static_cast<float*>(host_buffers[2].buffers[half]), kFrames, value);
  std::fill_n(static_cast<float*>(host_buffers[0].buffers[half]), kFrames, -value);
  if (notify_ready && std::abs(value - 0.03f) > 0.000001f) {
    active_driver->outputReady();
    active_driver->outputReady(); // Duplicate notifications must not duplicate audio.
  }
}

void buffer_switch(long half, ASIOBool) {
  const auto block = callback_count.fetch_add(1) + 1;
  if (block < callback_times.size()) callback_times[block] = host_timestamp_ms();
  verify_clock(static_cast<uint64_t>(block - 1) * kFrames);
  const auto* input = static_cast<const float*>(host_buffers[1].buffers[half]);
  if (input[0] != 0.25f) bad_channel_data = true;
  if (deferred_processing) {
    { std::lock_guard lock(jobs_mutex); deferred_jobs.emplace_back(half, block * 0.01f); }
    jobs_ready.notify_one();
  } else fill_output(half, block * 0.01f);
}
ASIOTime* time_switch(ASIOTime* time, long half, ASIOBool direct) {
  verify_clock(static_cast<uint64_t>(callback_count.load()) * kFrames, time);
  buffer_switch(half, direct); return time;
}
long message(long selector, long, void*, double*) {
  if (selector == kAsioResetRequest) { ++reset_requests; return 1; }
  return selector == kAsioSupportsTimeInfo && use_time_info ? 1 : 0;
}

bool check_case(bool deferred, bool time_info, bool ready) {
  captured.clear();
  output_times.clear();
  callback_times.fill(0);
  callback_count = 0;
  bad_channel_data = false;
  bad_clock = false;
  deferred_processing = deferred;
  use_time_info = time_info;
  notify_ready = false;
  HRESULT result = S_OK;
  AggregateASIO driver(nullptr, &result);
  active_driver = &driver;
  ASIOCallbacks callbacks{buffer_switch, nullptr, message, time_switch};
  if (!driver.init(nullptr) || driver.createBuffers(host_buffers, 3, kFrames, &callbacks) != ASE_OK) return false;
  long input_latency = 0, output_latency = 0;
  driver.getLatencies(&input_latency, &output_latency);
  if (output_latency != 4096) return false;
  if (ready) {
    if (driver.outputReady() != ASE_OK) return false;
    driver.getLatencies(&input_latency, &output_latency);
    if (output_latency != 4096 - kFrames) return false;
  }
  if (deferred) {
    worker_stop = false;
    const auto delay_timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!delay_timer) return false;
    // DAWs have persistent processing workers. Creating a new OS thread from
    // every real-time callback makes this test depend on thread startup load.
    deferred_worker = std::thread([delay_timer] {
      for (;;) {
        std::unique_lock lock(jobs_mutex);
        jobs_ready.wait(lock, [] { return worker_stop || !deferred_jobs.empty(); });
        if (deferred_jobs.empty() && worker_stop) { CloseHandle(delay_timer); return; }
        const auto job = deferred_jobs.front(); deferred_jobs.pop_front();
        lock.unlock();
        // Sleep(3) may sleep for an entire default Windows timer tick, causing
        // this fake DAW to miss the 21 ms audio deadline for unrelated reasons.
        LARGE_INTEGER due{};
        due.QuadPart = -30'000;
        if (!SetWaitableTimer(delay_timer, &due, 0, nullptr, nullptr, FALSE) ||
            WaitForSingleObject(delay_timer, 1000) != WAIT_OBJECT_0) bad_channel_data = true;
        fill_output(job.first, job.second);
      }
    });
  }
  fill_output(1, 0.005f); // ASIO's prefilled block before the first callback.
  notify_ready = ready;
  if (driver.start() != ASE_OK) return false;
  const auto deadline = GetTickCount64() + 2000;
  while (callback_count < 7 && GetTickCount64() < deadline) Sleep(1);
  driver.stop();
  ASIOSamples stopped_position{}; ASIOTimeStamp stopped_timestamp{};
  driver.getSamplePosition(&stopped_position, &stopped_timestamp);
  bool clock_passed = !bad_clock && callback_count > 0 &&
      pair_value(stopped_position) == static_cast<uint64_t>(callback_count.load() - 1) * kFrames &&
      pair_value(stopped_timestamp) == last_callback_timestamp;
  Sleep(2);
  ASIOTimeStamp later_timestamp{};
  driver.getSamplePosition(&stopped_position, &later_timestamp);
  clock_passed = pair_value(later_timestamp) == pair_value(stopped_timestamp) && clock_passed;
  if (deferred_worker.joinable()) {
    { std::lock_guard lock(jobs_mutex); worker_stop = true; }
    jobs_ready.notify_one();
    deferred_worker.join();
  }
  bool passed = !bad_channel_data && captured.size() >= 6 && clock_passed;
  for (size_t block = 0; passed && block < 6; ++block) {
    const auto expected = block == 0 ? 0.005f : static_cast<float>(block) * 0.01f;
    passed = captured[block][0] == expected && captured[block][1] == -expected;
  }
  bool early_handoff = false;
  for (size_t block = 1; ready && block < std::min<size_t>(captured.size(), 6); ++block) {
    if (output_times[block] - callback_times[block] < 12) early_handoff = true;
  }
  if (ready) passed = early_handoff && captured.size() <= callback_count + 1 && passed;
  // Block 3 deliberately omits outputReady; it still appears once via fallback.
  if (ready && captured.size() > 3) passed = output_times[3] - callback_times[3] >= 12 && passed;
  std::printf("%s callback, %s processing, outputReady=%d: %s (%zu blocks)\n",
      time_info ? "time-info" : "legacy", deferred ? "deferred" : "synchronous", ready, passed ? "PASS" : "FAIL", captured.size());
  if (!passed) {
    std::printf("  channel-data=%d clock=%d early-handoff=%d callbacks=%u\n",
        !bad_channel_data.load(), clock_passed, early_handoff, callback_count.load());
    for (size_t block = 0; block < std::min<size_t>(captured.size(), 6); ++block) {
      std::printf("  block=%zu left=%.8f right=%.8f handoff-ms=%lld\n", block,
          captured[block][0], captured[block][1], block == 0 ? 0LL :
          static_cast<long long>(output_times[block]) - static_cast<long long>(callback_times[block]));
    }
  }
  driver.disposeBuffers();
  return passed;
}

bool check_device_recovery() {
  callback_count = 0;
  reset_requests = 0;
  mock_packets = 0;
  deferred_processing = use_time_info = notify_ready = false;
  HRESULT result = S_OK;
  AggregateASIO driver(nullptr, &result);
  active_driver = &driver;
  ASIOCallbacks handlers{buffer_switch, nullptr, message, time_switch};
  if (!driver.init(nullptr) || driver.createBuffers(host_buffers, 3, kFrames, &handlers) != ASE_OK ||
      driver.start() != ASE_OK) return false;
  Sleep(1200);
  mock_stalled = true; // A vendor buffer change leaves old KS packets pending.
  const auto deadline = GetTickCount64() + 4000;
  while (reset_requests == 0 && GetTickCount64() < deadline) Sleep(10);
  const auto initializations_before = engine_initializations.load();
  bool passed = reset_requests == 1;
  // A host may reset buffers on the same ASIO object, rather than recreating it.
  passed = driver.createBuffers(host_buffers, 3, kFrames, &handlers) == ASE_OK &&
      engine_initializations == initializations_before + 1 && !mock_stalled && passed;
  driver.disposeBuffers();
  std::printf("Device stall requests one host reset and reopens engine on the same ASIO object: %s\n", passed ? "PASS" : "FAIL");
  return passed;
}
}

int main() {
  bool passed = true;
  for (const bool time_info : {false, true}) for (const bool deferred : {false, true}) for (const bool ready : {false, true}) {
    passed = check_case(deferred, time_info, ready) && passed;
  }
  passed = check_device_recovery() && passed;
  return passed ? 0 : 1;
}
