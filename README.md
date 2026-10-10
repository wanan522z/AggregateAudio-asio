# Aggregate Audio

Windows x64 prototype of a user-selectable multi-device ASIO driver. The
configurator enumerates Kernel Streaming (KS) capture and render pins, groups
them under their physical device, and shows each pin's channels as individually
selectable rows. It stores channel selections, per-device KS stream buffers,
the sample rate, and the global ASIO host buffer in
`%LOCALAPPDATA%\AggregateASIO\settings.ini`.

The ASIO DLL opens each selected KS pin directly with `KsCreatePin` and
negotiates a PCM/float format. On WaveRT pins it requests a driver-mapped cyclic
buffer and hardware notifications; other pins use standard KS stream read/write
requests. Each pin has its own worker thread and lock-free bridge ring; the
ASIO callback only touches preallocated buffers. A small linear resampler and
occupancy servo compensate for different device rates and clocks. Pins are
opened independently, so a device that rejects its advertised format is
reported as unavailable rather than silently routed through WASAPI.

Capture and render workers keep two asynchronous KS packets queued. Capture
rotates three buffers to queue the next read before decoding the completed one;
render refills a completed packet while the other remains queued. WaveRT uses
hardware notifications and the current render play position. Both bridges
prime before consuming audio and retain fractional resampling phase between
blocks. The ASIO timer keeps
absolute deadlines to avoid accumulating callback/timer delays.
ASIO sample positions and timestamps are latched together for the current
block. Windows timestamps use the `timeGetTime` epoch with QPC interpolation.
Bridge buffering accounts for producer and consumer block sizes separately.
When a callback or device worker stalls, its consumer skips stale queued
frames with a short fade instead of retaining a growing monitoring delay.
This recovery discards audio only when the backlog exceeds the buffering
limit; discarded frames are exposed in diagnostics. A host's `outputReady`
notification wakes the scheduler to submit a completed block early, avoiding
one ASIO period of output delay. Hosts that omit the notification retain the
standard double-buffer fallback, with no duplicate submissions.

## Build

Use Visual Studio 2022 Build Tools with the Windows 10/11 SDK and CMake 3.24+:

```powershell
cmake -S . -B build\device-groups-v3 -G "Visual Studio 17 2022" -A x64
cmake --build build\device-groups-v3 --config Release
```

The outputs are `build\device-groups-v3\Release\AggregateAudio.dll` and
`build\device-groups-v3\Release\AggregateAudioConfig.exe`. Keep both files in
the same folder; the ASIO control-panel entry starts the configurator.

The configurator has a compact desktop layout that follows per-monitor DPI and
fits the monitor's work area. Buffer fields adapt to narrower windows. The
top-right Settings button opens the session log in the same window; returning
preserves pending channel and buffer edits. Reopening the ASIO control panel
restores and activates the existing configurator, including during startup.
The initial device page hides the full-page log from control creation and
completes DPI layout before first display. Device refresh preserves the selected
channel, collapsed groups and the visible channel's position by stable
IDs, including when enumeration order changes. A removed channel falls back
to its surviving device.

The list's horizontal and vertical scrollbars and the Settings log share a
light track, rounded thumb, and subtle chevrons. Native scroll ranges, wheel
and keyboard navigation remain available; pointer capture ends on release,
cancellation, or a page switch. Thumb dragging preserves channel selections.

The executable embeds the same five-bar brand as the window header, with icon
sizes from 16 to 256 pixels. Starting ASIO launches the configurator in tray
mode without showing a window. One notification icon covers all running hosts
in the Windows session; clicking it restores the existing panel. Closing the
panel keeps it in the tray while ASIO runs. The icon disappears when ASIO stops,
and a hidden configurator exits after a short grace period. Explorer restarts
and overlapping host starts retain the single-window behavior.

A background watchdog monitors the selected endpoints' state and native format
notifications, plus KS errors and packet completion. If callbacks continue but
a device stream stalls for 1.5 seconds, it requests one ASIO reset from the host.
Errors and notifications are debounced, healthy silence is accepted, and reset
requests are throttled. Recreating buffers on the same ASIO object re-enumerates
the selected devices and opens fresh KS pins, without requiring a settings save.

Tab navigates the controls, Enter saves on the device page, and Escape returns
from Settings or closes the device page. `config_ui_test` checks the native
layout at 100%, 150%, and 200% scaling, scrolled-row drawing boundaries, log and
selection persistence and concurrent process activation using hidden fixtures
without opening audio devices or saving a profile.
It also checks scrollbar dragging to both ends, track/arrow navigation, capture
release, and unchanged log text and selection at each DPI.

KS stream factories merge only when their topology reaches the same single
physical connector, direction, and channel layout. Alternate factories remain
available as candidates, and old channel IDs and device buffer settings migrate
in memory. Separate headphone, speaker, microphone, and loopback connectors are
retained and named using their topology. The shared Windows system container
does not combine unrelated built-in adapters. `catalog_inspect` reads this
metadata without creating streaming pins. `settings_test` checks legacy-profile
migration and the removal of obsolete ASIO cable entries without changing hardware
selections. `device_recovery_test` checks fault detection, silence, debounce and
rearming. `asio_output_test` also verifies a stalled stream requests a host reset
and reopens its engine. `config_tray_test` checks the embedded icon and tray
lifecycle using a fake shell notification backend.

Output regression tests are included in the build. Run them with:

```powershell
ctest --test-dir build\device-groups-v3 -C Release --output-on-failure
```

These tests exercise the ASIO callback with synchronous and deferred host
processing, both legacy and time-info callbacks, channel routing,
early output hand-off, duplicate/omitted readiness notifications, latency
budget monotonicity, backlog recovery and different device/host block sizes,
KS stream header submission, queued capture/render continuity and cancellation, partial
and empty capture packets, capture starvation counters, restart and device loss,
fractional resampling phase, KS interface deduplication and old selection IDs,
latched ASIO clock pairs and the Windows timestamp epoch, and WaveRT recovery
after a skipped notification. They use
simulated devices and do not establish audible output or physical latency.

To inspect the running DAW without interrupting it, use its process ID:

```powershell
.\AggregateAudioDiagnostics.exe <host-process-id> 15
```

The read-only diagnostics show callback intervals and duration, silent output
blocks, zero-valued output frames and their longest consecutive run, invalid
samples, and each stream's received/consumed frames, overflow, underrun and
missing input frame counters. Startup input priming is excluded from missing
frames. Diagnostics also report stale frames discarded during recovery,
early output submissions, and the input/output latency estimates returned to
the host. Estimates exclude DAW plugin latency and are not physical loopback
measurements. Silence during recovery after starvation is included in missing
frames. The driver publishes numeric counters through shared
memory every 250 ms; it does not record audio or write a log on the audio thread.

## Configure and register

For installation on another Windows 10/11 x64 PC, use the offline
`dist\AggregateAudio-v12-x64-Setup.exe`. It installs the required Microsoft
x64 runtime, registers the driver for 64-bit ASIO hosts, and provides shortcuts
and a standard uninstall entry. Close DAWs and the configurator first, and
select the new PC's channels after installation. User settings are not included
in the package. Build and verification instructions are in
`packaging\README.md`; `packaging\INSTALL-zh-CN.txt` contains user instructions.

Run `AggregateAudioConfig.exe` (or open the driver's control panel), expand a
physical device group, and check the input and output channels to include.
Select a channel row to edit that device's KS stream buffer. “ASIO host buffer”
remains a single global block size because the ASIO host requests one block
size for the aggregate driver. “Auto detect device buffer” probes each selected
device's pins one at a time, sends digital silence to render pins, and discards
captured audio. It chooses the smallest per-device setting that opens and
streams during the short probe. This is an initial recommendation, not a
DAW-load or physical loopback latency measurement. The window stays responsive
while the probe runs.

The driver currently advertises power-of-two ASIO buffers from 64 to 2048
frames and common sample rates from 32 to 384 kHz. The actual KS pin format and
stream block can differ from the requested ASIO profile when a device accepts
only a nearby rate. Saving settings signals an open driver to send the standard
ASIO reset request so the host can reinitialize the driver and reload its
channel list without exiting the DAW. Whether the host accepts that request is
DAW-dependent; if it ignores the request, reopen the ASIO device in the DAW.

To register the 64-bit driver for ASIO hosts, open an elevated PowerShell in
the output folder and run:

```powershell
regsvr32 .\AggregateAudio.dll
```

To remove it later, run `regsvr32 /u .\AggregateAudio.dll`. Registration writes
the standard ASIO and COM registry entries. This machine currently points the
ASIO registry entry to the `build\device-groups-v3\Release` DLL.

## Limits

- KS pin factories are driver-defined and do not map one-to-one to friendly
  Windows audio endpoints. The configurator lists the pins and formats each
  device exposes to user mode; a listed pin may still reject a stream when
  opened.
- WaveRT mapped-buffer mode can avoid per-packet stream I/O and use hardware
  notifications. Other pins use generic KS stream requests. Actual latency
  depends on each pin driver and hardware; a single-direction latency below
  10 ms is not verified or guaranteed.
- The reported ASIO latencies are conservative estimates based on the selected
  ASIO buffer and internal ring buffers. No physical loopback latency
  measurement has been run here. The user confirmed
  that queued capture resolved the regular FL Studio monitoring stutter;
  low-buffer performance and latency are still being tested.
- Stream workers wait for both I/O completion and stop commands, stop the pin,
  cancel pending requests, and wait for completion before releasing buffers.
  Cancellation and restart use simulated-device regression tests; behavior
  still needs verification on the actual devices.
- A device loss requests a DAW-side ASIO reset; the host decides when to stop
  and reopen the driver.
- Clock drift correction is lightweight linear interpolation; it does not
  provide word-clock synchronization between outputs.
- The configurator uses Windows audio endpoint names such as `Playback 1/2`,
  `Loopback 1/2`, and `Analog 1/2` when Windows provides a matching KS endpoint.
  Audio/Capture/Render interface aliases of the same
  filter and pin are merged without losing the filter reference string that
  distinguishes channel pairs. Render/offload factories remain separate,
  as do different filter reference strings and
  capture factories. Friendly-name equality does not establish equivalence.
  Former endpoint and channel IDs remain usable; ambiguous old name IDs select
  the first factory that used that name.
- The ASIO SDK in `third_party/asio-sdk` is under its supplied license, which
  offers GPLv3 and a proprietary option. Resolve the license before
  distributing the driver.
