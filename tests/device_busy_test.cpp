// Simulate occupied KS factories and races without accessing audio hardware.
#include "audio_engine.h"
#include <windows.h>
#include <winioctl.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <cstdio>
#include <unordered_set>

namespace {
DWORD filter_error{}, create_error{}, count_error{ERROR_NOT_SUPPORTED};
DWORD global_count_error{}, local_count_error{};
bool global_busy{}, local_busy{}, short_counts{}, rt_supported{}, rt_mapping_fails{};
unsigned create_calls{}, filter_calls{}, mapping_calls{};
unsigned busy_on_call{};
unsigned possible_count{1}, busy_filter_call{};
unsigned global_queries{}, local_queries{};
std::unordered_set<HANDLE> device_handles;

HANDLE WINAPI fake_create_file(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) {
  ++filter_calls;
  if (filter_error) { SetLastError(filter_error); return INVALID_HANDLE_VALUE; }
  const HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  device_handles.insert(handle);
  return handle;
}
BOOL WINAPI fake_close(HANDLE handle) {
  device_handles.erase(handle);
  return CloseHandle(handle);
}
DWORD WINAPI fake_create_pin(HANDLE, PKSPIN_CONNECT, ACCESS_MASK, PHANDLE pin) {
  ++create_calls;
  if (create_error) return create_error;
  if (busy_on_call == create_calls) return ERROR_BUSY;
  *pin = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  device_handles.insert(*pin);
  return ERROR_SUCCESS;
}
BOOL WINAPI fake_ioctl(HANDLE, DWORD code, LPVOID input, DWORD, LPVOID output,
                       DWORD, LPDWORD returned, LPOVERLAPPED) {
  *returned = 0;
  if (code == IOCTL_KS_PROPERTY && input) {
    const auto& property = *static_cast<KSPROPERTY*>(input);
    if (property.Set == KSPROPSETID_Pin &&
        (property.Id == KSPROPERTY_PIN_GLOBALCINSTANCES || property.Id == KSPROPERTY_PIN_CINSTANCES)) {
      const bool global = property.Id == KSPROPERTY_PIN_GLOBALCINSTANCES;
      if (global) ++global_queries;
      else ++local_queries;
      const DWORD error = count_error ? count_error : global ? global_count_error : local_count_error;
      if (error) { SetLastError(error); return FALSE; }
      const bool busy = (property.Id == KSPROPERTY_PIN_GLOBALCINSTANCES ? global_busy : local_busy) ||
          (busy_filter_call && filter_calls == busy_filter_call);
      *static_cast<KSPIN_CINSTANCES*>(output) = {possible_count, busy ? 1u : 0u};
      *returned = short_counts ? 0 : sizeof(KSPIN_CINSTANCES);
      return TRUE;
    }
    if (property.Set == KSPROPSETID_RtAudio &&
        property.Id == KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION) {
      if (property.Flags == KSPROPERTY_TYPE_BASICSUPPORT && rt_supported) {
        *static_cast<ULONG*>(output) = KSPROPERTY_TYPE_GET;
        *returned = sizeof(ULONG);
        return TRUE;
      }
      if (property.Flags == KSPROPERTY_TYPE_GET) {
        ++mapping_calls;
        SetLastError(rt_mapping_fails ? ERROR_NOT_SUPPORTED : ERROR_GEN_FAILURE);
        return FALSE;
      }
    }
    if (property.Set == KSPROPSETID_Connection && property.Id == KSPROPERTY_CONNECTION_STATE) return TRUE;
  }
  SetLastError(ERROR_NOT_SUPPORTED);
  return FALSE;
}
}

#define CreateFileW fake_create_file
#define CloseHandle fake_close
#define KsCreatePin fake_create_pin
#define DeviceIoControl fake_ioctl
#include "../src/audio_engine.cpp"
#include "../src/ks_probe.cpp"
#undef CreateFileW
#undef CloseHandle
#undef KsCreatePin
#undef DeviceIoControl

unsigned device_buffer_frames(const AudioSettings& settings, const std::wstring&) { return settings.buffer_frames; }

namespace {
AudioEndpoint endpoint() {
  AudioEndpoint value;
  value.id = L"busy-test-" + std::to_wstring(GetCurrentProcessId());
  value.name = L"Occupied fixture";
  value.ks_filter_path = L"fixture";
  value.channels = 2;
  value.direction = Direction::output;
  value.native_sample_rates = {48000, 44100};
  KsPinCandidate candidate;
  candidate.filter_path = L"alias";
  candidate.channels = 2;
  value.ks_pin_candidates = {candidate, candidate};
  return value;
}
void reset() {
  filter_error = create_error = busy_on_call = 0;
  count_error = ERROR_NOT_SUPPORTED;
  global_busy = local_busy = short_counts = rt_supported = rt_mapping_fails = false;
  create_calls = filter_calls = mapping_calls = 0;
  possible_count = 1; busy_filter_call = 0;
  global_count_error = local_count_error = global_queries = local_queries = 0;
}
}

int main() {
  bool passed = true;
  const auto check = [&](bool condition, const char* label) {
    std::printf("%s: %s\n", label, condition ? "PASS" : "FAIL");
    passed = condition && passed;
  };
  const auto device = endpoint();
  AudioSettings settings;
  settings.output_device_ids = {device.id};
  std::string error;
  reset(); count_error = 0; global_count_error = ERROR_NOT_FOUND;
  {
    check(SUCCEEDED(ks_device_access::check_endpoint(device)) && local_queries == 1 && create_calls == 0,
        "E2x2 missing global property uses idle local counts in UI preflight");
    AudioEngine engine;
    check(engine.initialize(settings, {device}, error) && create_calls == 1 && local_queries == 2,
        "E2x2 0x80070490 property response does not block KS initialization");
  }
  reset(); count_error = 0; global_count_error = ERROR_NOT_FOUND; local_busy = true;
  {
    AudioEngine engine;
    check(!engine.initialize(settings, {device}, error) && create_calls == 0 && local_queries == 1 &&
        ks_device_access::is_busy(engine.last_status()), "Missing global property still respects full local capacity");
  }
  for (const DWORD status : {ERROR_NOT_FOUND, ERROR_SET_NOT_FOUND, ERROR_NOT_SUPPORTED,
                             ERROR_INVALID_FUNCTION, ERROR_CALL_NOT_IMPLEMENTED, ERROR_INVALID_PARAMETER}) {
    reset(); count_error = status;
    AudioEngine engine;
    check(engine.initialize(settings, {device}, error) && create_calls == 1 &&
        global_queries == 1 && local_queries == 1, "Missing optional counts preserve legacy KS compatibility");
  }
  for (const bool global : {true, false}) {
    reset(); count_error = 0; global_busy = global; local_busy = !global;
    AudioEngine engine;
    check(!engine.initialize(settings, {device}, error) && ks_device_access::is_busy(engine.last_status()) &&
        create_calls == 0 && filter_calls == 1 && device_handles.empty(),
        global ? "Global occupancy blocks pin creation" : "Local occupancy blocks pin creation");
    const auto result = probe_endpoint(device, 48000, 128, 1);
    check(ks_device_access::is_busy(result.hresult) && !result.stable && create_calls == 0,
        "Probe preserves occupied-device status");
  }
  for (const DWORD status : {ERROR_BUSY, ERROR_SHARING_VIOLATION, ERROR_LOCK_VIOLATION,
                             ERROR_DEVICE_IN_USE, ERROR_PIPE_BUSY}) {
    reset(); create_error = status;
    AudioEngine engine;
    check(!engine.initialize(settings, {device}, error) && ks_device_access::is_busy(engine.last_status()) &&
        create_calls == 1 && filter_calls == 1 && device_handles.empty(),
        "Open race stops all format and alias retries");
  }
  reset(); count_error = 0; global_busy = true; possible_count = 8;
  {
    AudioEngine engine;
    check(engine.initialize(settings, {device}, error) && create_calls == 1,
        "Multi-instance factory with remaining capacity is not falsely marked busy");
  }
  reset(); count_error = 0; busy_filter_call = 2;
  {
    auto second = device;
    second.id += L"-second";
    auto pair_settings = settings;
    pair_settings.output_device_ids.push_back(second.id);
    AudioEngine engine;
    check(!engine.initialize(pair_settings, {device, second}, error) && create_calls == 1 &&
        engine.output_stream_count() == 0 && device_handles.empty() &&
        SUCCEEDED(ks_device_access::check_endpoint(device)),
        "Later occupied device releases all previously opened pins and reservations");
  }
  reset(); filter_error = ERROR_SHARING_VIOLATION;
  {
    AudioEngine engine;
    check(!engine.initialize(settings, {device}, error) && create_calls == 0 && filter_calls == 1 &&
        ks_device_access::is_busy(engine.last_status()), "Occupied filter is not retried");
  }
  reset(); count_error = ERROR_ACCESS_DENIED;
  {
    AudioEngine engine;
    check(!engine.initialize(settings, {device}, error) && engine.last_status() == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) &&
        !ks_device_access::is_busy(engine.last_status()) && create_calls == 0 && device_handles.empty(),
        "Permission error remains distinct from occupancy");
  }
  reset(); count_error = 0; short_counts = true;
  {
    AudioEngine engine;
    check(!engine.initialize(settings, {device}, error) && create_calls == 0 && device_handles.empty(),
        "Malformed occupancy response does not open a pin");
  }
  reset(); rt_supported = rt_mapping_fails = true; busy_on_call = 2;
  {
    AudioEngine engine;
    check(!engine.initialize(settings, {device}, error) && create_calls == 2 && mapping_calls == 1 &&
        filter_calls == 1 && device_handles.empty() && ks_device_access::is_busy(engine.last_status()),
        "Busy fallback stops without further reopens");
  }
  reset();
  {
    AudioEngine owner;
    check(owner.initialize(settings, {device}, error) && create_calls == 1 && mapping_calls == 0,
        "Legacy KS opens once without unsupported WaveRT mapping");
    AudioEngine second;
    check(!second.initialize(settings, {device}, error) && ks_device_access::is_busy(second.last_status()) &&
        create_calls == 1 && ks_device_access::is_busy(ks_device_access::check_endpoint(device)),
        "Concurrent Aggregate Audio opens are blocked before accessing driver");
  }
  check(device_handles.empty(), "Shutdown releases filter and pin handles");
  {
    AudioEngine retry;
    check(retry.initialize(settings, {device}, error), "Device can be reopened after release");
  }
  check(device_handles.empty(), "Retry leaves no device handles");
  return passed ? 0 : 1;
}
