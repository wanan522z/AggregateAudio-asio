#include "driver_telemetry.h"
#include <cstdio>

bool read_snapshot(const SharedDriverTelemetry* shared, DriverTelemetrySnapshot& snapshot) {
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    const LONG64 sequence = shared->sequence;
    if (sequence & 1) { SwitchToThread(); continue; }
    MemoryBarrier();
    std::memcpy(&snapshot, &shared->snapshot, sizeof(snapshot));
    MemoryBarrier();
    if (sequence == shared->sequence) return snapshot.version == kTelemetryVersion;
  }
  return false;
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) { std::printf("Usage: AggregateAudioDiagnostics.exe <host PID> [seconds]\n"); return 1; }
  const DWORD pid = wcstoul(argv[1], nullptr, 10);
  const auto name = driver_telemetry_name(pid);
  HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
  if (!mapping) { std::printf("No Aggregate Audio telemetry for PID %lu (error %lu). Restart the host with the diagnostic build.\n", pid, GetLastError()); return 2; }
  const auto* shared = static_cast<const SharedDriverTelemetry*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(SharedDriverTelemetry)));
  if (!shared) { CloseHandle(mapping); return 2; }
  const unsigned seconds = argc > 2 ? std::clamp(static_cast<unsigned>(wcstoul(argv[2], nullptr, 10)), 1u, 60u) : 1;
  for (unsigned second = 0; second < seconds; ++second) {
    DriverTelemetrySnapshot snapshot;
    if (!read_snapshot(shared, snapshot)) continue;
    std::printf("ASIO pid=%u running=%u rate=%u buffer=%u time_info=%u outputs=%u callbacks=%llu silent=%llu peak=%.6f nonfinite=%llu missed_periods=%llu max_interval_ms=%.3f max_callback_ms=%.3f output_ready=%llu elapsed_s=%.3f zero_frames=%llu max_zero_run=%llu early_outputs=%llu input_latency=%u output_latency=%u\n",
        snapshot.process_id, snapshot.running, snapshot.sample_rate, snapshot.buffer_frames, snapshot.time_info_mode,
        snapshot.output_channel_count, snapshot.callbacks, snapshot.silent_blocks, snapshot.output_peak, snapshot.nonfinite_samples,
        snapshot.missed_periods, snapshot.max_interval_ns / 1e6, snapshot.max_callback_ns / 1e6,
        snapshot.output_ready_calls, snapshot.elapsed_ns / 1e9, snapshot.output_zero_frames, snapshot.output_max_zero_run,
        snapshot.early_output_blocks, snapshot.input_latency_frames, snapshot.output_latency_frames);
    for (unsigned i = 0; i < std::min(snapshot.stream_count, static_cast<unsigned>(kTelemetryStreams)); ++i) {
      const auto& stream = snapshot.streams[i];
      const auto& stats = stream.statistics;
      char label[1024]{};
      WideCharToMultiByte(CP_UTF8, 0, stream.name, -1, label, sizeof(label), nullptr, nullptr);
      std::printf("  %s %s %s rate=%u block=%u packets=%u underruns=%u fill=%u received=%llu dropped=%llu rendered=%llu missing=%llu discarded=%llu status=%08lX\n",
          stats.flags & 1 ? "IN" : "OUT", stats.flags & 2 ? "WaveRT" : "KS-IRP", label,
          stats.sample_rate, stats.buffer_frames, stats.packets, stats.underruns, stats.buffered_frames,
          stats.received_frames, stats.dropped_frames, stats.rendered_frames, stats.missing_frames, stats.discarded_frames,
          static_cast<unsigned long>(stats.status));
    }
    std::fflush(stdout);
    if (second + 1 < seconds) Sleep(1000);
  }
  UnmapViewOfFile(shared); CloseHandle(mapping); return 0;
}
