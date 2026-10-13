// Explicit hardware connection check. Pins remain stopped: no capture or render.
#include "audio_engine.h"
#include <windows.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
  if (argc != 2 || !argv[1][0]) {
    std::puts("Usage: ks_open_check.exe <device-name-fragment> (opens stopped KS pins, no audio I/O)");
    return 2;
  }
  unsigned matches{}, failed{};
  for (const auto& endpoint : enumerate_endpoints()) {
    if (endpoint.name.find(argv[1]) == std::wstring::npos) continue;
    ++matches;
    std::string error;
    AudioSettings settings;
    settings.sample_rate = 48000;
    settings.buffer_frames = 128;
    (endpoint.direction == Direction::input ? settings.input_device_ids : settings.output_device_ids) = {endpoint.id};
    AudioEngine engine;
    const bool connected = engine.initialize(settings, {endpoint}, error);
    const auto* stream = endpoint.direction == Direction::input ? engine.input_stream(0) : engine.output_stream(0);
    const auto stats = stream ? stream->statistics() : StreamStatistics{};
    std::wprintf(L"%s %s: %s rate=%u channels=%u frames=%u status=0x%08lX\n",
        endpoint.direction == Direction::input ? L"IN" : L"OUT", endpoint.name.c_str(),
        connected ? L"PASS" : L"FAIL", stats.sample_rate, stats.channels, stats.buffer_frames,
        static_cast<unsigned long>(engine.last_status()));
    if (!connected) { ++failed; std::printf("  %s\n", error.c_str()); }
  }
  std::printf("Stopped-pin connection check: %u endpoints, %u failures; no audio started.\n", matches, failed);
  return matches && !failed ? 0 : 1;
}
