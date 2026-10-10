#pragma once

#include "audio_engine.h"
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

constexpr unsigned kTelemetryVersion = 3;
constexpr size_t kTelemetryStreams = 64;

struct TelemetryStream {
  wchar_t name[128]{};
  StreamStatistics statistics{};
};

struct DriverTelemetrySnapshot {
  unsigned version{kTelemetryVersion};
  unsigned process_id{};
  unsigned running{};
  unsigned sample_rate{};
  unsigned buffer_frames{};
  unsigned time_info_mode{};
  unsigned stream_count{};
  unsigned output_channel_count{};
  uint64_t callbacks{};
  uint64_t silent_blocks{};
  uint64_t nonfinite_samples{};
  uint64_t missed_periods{};
  uint64_t max_callback_ns{};
  uint64_t max_interval_ns{};
  uint64_t output_ready_calls{};
  uint64_t elapsed_ns{};
  float output_peak{};
  uint64_t output_zero_frames{};
  uint64_t output_max_zero_run{};
  uint64_t early_output_blocks{};
  unsigned input_latency_frames{};
  unsigned output_latency_frames{};
  std::array<TelemetryStream, kTelemetryStreams> streams{};
};

struct SharedDriverTelemetry {
  alignas(8) volatile LONG64 sequence{};
  DriverTelemetrySnapshot snapshot{};
};

inline std::wstring driver_telemetry_name(DWORD process_id) {
  return L"Local\\AggregateAudio.Telemetry.v3." + std::to_wstring(process_id);
}

inline bool read_driver_telemetry(const SharedDriverTelemetry* shared, DriverTelemetrySnapshot& snapshot) noexcept {
  if (!shared) return false;
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    const auto sequence = shared->sequence;
    if (sequence & 1) { SwitchToThread(); continue; }
    MemoryBarrier();
    std::memcpy(&snapshot, &shared->snapshot, sizeof(snapshot));
    MemoryBarrier();
    if (sequence == shared->sequence) return snapshot.version == kTelemetryVersion;
  }
  return false;
}

class DriverTelemetry {
 public:
  ~DriverTelemetry() {
    if (shared_) UnmapViewOfFile(shared_);
    if (mapping_) CloseHandle(mapping_);
  }
  void initialize() noexcept {
    if (mapping_) return;
    try {
      const auto name = driver_telemetry_name(GetCurrentProcessId());
      mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
          sizeof(SharedDriverTelemetry), name.c_str());
      if (mapping_) shared_ = static_cast<SharedDriverTelemetry*>(
          MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, sizeof(SharedDriverTelemetry)));
    } catch (...) {}
  }
  void publish(const DriverTelemetrySnapshot& snapshot) noexcept {
    if (!shared_) return;
    InterlockedIncrement64(&shared_->sequence);
    std::memcpy(&shared_->snapshot, &snapshot, sizeof(snapshot));
    InterlockedIncrement64(&shared_->sequence);
  }
 private:
  HANDLE mapping_{};
  SharedDriverTelemetry* shared_{};
};
