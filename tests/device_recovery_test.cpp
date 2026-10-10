#include "device_recovery.h"
#include "selected_device_notifications.h"
#include <cstdio>

int main() {
  bool passed = true;
  auto check = [&](bool value, const char* label) {
    std::printf("%s: %s\n", label, value ? "PASS" : "FAIL"); passed &= value;
  };
  StreamStatistics stream{}; stream.flags = 4; stream.sample_rate = 48000; stream.buffer_frames = 256; stream.channels = 2;
  DeviceRecoveryWatchdog guard;
  check(!guard.observe(100, 1, 1, true, {stream}), "Startup grace");
  bool silence_safe = true;
  for (unsigned step = 1; step < 16; ++step) {
    ++stream.packets;
    silence_safe &= !guard.observe(100 + step * 250, 1, step + 1, true, {stream});
  }
  check(silence_safe, "Healthy silence never triggers a reset");
  check(!guard.observe(4000, 1, 18, true, {stream}) && guard.observe(5500, 1, 24, true, {stream}),
      "Stalled selected stream triggers recovery");
  check(!guard.observe(8000, 1, 28, true, {stream}), "Only one reset for a continuing fault");
  guard.observe(8100, 1, 29, false, {});
  guard.observe(8200, 2, 1, true, {stream});
  for (unsigned step = 1; step <= 12; ++step) {
    ++stream.packets; guard.observe(8200 + step * 250, 2, step + 1, true, {stream});
  }
  check(guard.observe(11500, 2, 15, true, {stream}, true), "Healthy recovery rearms device-change detection");
  DeviceRecoveryWatchdog errors;
  errors.observe(100, 1, 1, true, {stream});
  stream.status = static_cast<long>(0x80070006); stream.flags = 0;
  check(!errors.observe(1200, 1, 2, true, {stream}) && errors.observe(1750, 1, 3, true, {stream}),
      "Stream failure is debounced and recovers");
  DeviceRecoveryWatchdog stopped;
  stopped.observe(100, 1, 1, true, {stream});
  check(!stopped.observe(6000, 1, 1, false, {stream}), "Stopped ASIO does not trigger recovery");
  DeviceRecoveryWatchdog starved;
  stream.status = 0; stream.flags = 4;
  starved.observe(100, 1, 1, true, {stream});
  check(!starved.observe(10000, 1, 1, true, {stream}), "Host callback starvation is not treated as a device change");
  using selected_audio_notifications::matches;
  check(matches(L"\\\\?\\usb#a#{6994ad04-93ef-11d0-a3cc-00a0c9223196}\\pair1",
      L"\\??\\USB#A#{65e8773e-8f56-11d0-a3b9-00a0c9223196}\\pair1") &&
      !matches(L"\\\\?\\usb#a#{6994ad04-93ef-11d0-a3cc-00a0c9223196}\\pair1",
      L"\\\\?\\usb#b#{6994ad04-93ef-11d0-a3cc-00a0c9223196}\\pair1"),
      "Only matching selected device paths count as changes");
  return passed ? 0 : 1;
}
