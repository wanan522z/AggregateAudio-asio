// Simulate occupied KS factories and races without accessing audio hardware.
#include "audio_engine.h"
#include <windows.h>
#include <winioctl.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <cstdio>
#include <cstring>
#include <utility>
#include <unordered_set>

namespace {
DWORD filter_error{}, create_error{}, count_error{ERROR_NOT_SUPPORTED};
DWORD global_count_error{}, local_count_error{};
bool global_busy{}, local_busy{}, short_counts{}, rt_supported{}, rt_mapping_fails{};
bool negotiated_pcm{}, intersection_supported{true};
unsigned native_bits{32}, native_valid_bits{24}, intersection_queries{};
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
DWORD WINAPI fake_create_pin(HANDLE, PKSPIN_CONNECT connect, ACCESS_MASK, PHANDLE pin) {
  ++create_calls;
  if (create_error) return create_error;
  if (busy_on_call == create_calls) return ERROR_BUSY;
  if (negotiated_pcm) {
    const auto* data = reinterpret_cast<const KSDATAFORMAT*>(connect + 1);
    const auto* wave = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(data + 1);
    if (data->SubFormat != KSDATAFORMAT_SUBTYPE_PCM ||
        wave->Format.wFormatTag != WAVE_FORMAT_EXTENSIBLE || wave->Format.nChannels != 2 ||
        wave->Format.nSamplesPerSec != 48000 || wave->Format.wBitsPerSample != native_bits ||
        wave->Samples.wValidBitsPerSample != native_valid_bits ||
        wave->Format.nBlockAlign != 2 * native_bits / 8 ||
        (intersection_supported && data->SampleSize != 0)) return ERROR_NO_MATCH;
  }
  *pin = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  device_handles.insert(*pin);
  return ERROR_SUCCESS;
}
BOOL WINAPI fake_ioctl(HANDLE, DWORD code, LPVOID input, DWORD, LPVOID output,
                       DWORD output_size, LPDWORD returned, LPOVERLAPPED) {
  *returned = 0;
  if (code == IOCTL_KS_PROPERTY && input) {
    const auto& property = *static_cast<KSPROPERTY*>(input);
    if (negotiated_pcm && property.Set == KSPROPSETID_Pin && property.Id == KSPROPERTY_PIN_DATARANGES) {
      // Include a shorter non-audio range before the audio capabilities.
      struct Ranges { KSMULTIPLE_ITEM list; KSDATARANGE other; KSDATARANGE_AUDIO audio; } ranges{};
      ranges.list = {sizeof(ranges), 2};
      ranges.other.FormatSize = sizeof(KSDATARANGE);
      ranges.audio.DataRange = {sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
          KSDATAFORMAT_TYPE_AUDIO, KSDATAFORMAT_SUBTYPE_PCM, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX};
      ranges.audio.MaximumChannels = 2;
      ranges.audio.MinimumBitsPerSample = ranges.audio.MaximumBitsPerSample = native_valid_bits;
      ranges.audio.MinimumSampleFrequency = ranges.audio.MaximumSampleFrequency = 48000;
      if (output_size < sizeof(ranges)) return FALSE;
      std::memcpy(output, &ranges, sizeof(ranges));
      *returned = sizeof(ranges);
      return TRUE;
    }
    if (negotiated_pcm && property.Set == KSPROPSETID_Pin && property.Id == KSPROPERTY_PIN_DATAINTERSECTION) {
      ++intersection_queries;
      if (!intersection_supported) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
      const auto* range = reinterpret_cast<const KSDATARANGE_AUDIO*>(
          static_cast<const BYTE*>(input) + sizeof(KSP_PIN) + sizeof(KSMULTIPLE_ITEM));
      if (range->DataRange.SubFormat != KSDATAFORMAT_SUBTYPE_PCM || range->MaximumChannels != 2 ||
          range->MinimumBitsPerSample != native_valid_bits || range->MinimumSampleFrequency != 48000) {
        SetLastError(ERROR_NO_MATCH); return FALSE;
      }
      if (output_size < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE)) return FALSE;
      auto* data = static_cast<KSDATAFORMAT*>(output);
      *data = {sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE), 0, 0, 0,
          KSDATAFORMAT_TYPE_AUDIO, KSDATAFORMAT_SUBTYPE_PCM, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX};
      auto* wave = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(data + 1);
      *wave = {};
      wave->Format = {WAVE_FORMAT_EXTENSIBLE, 2, 48000, 48000 * 2 * native_bits / 8,
          static_cast<WORD>(2 * native_bits / 8), static_cast<WORD>(native_bits), 22};
      wave->Samples.wValidBitsPerSample = static_cast<WORD>(native_valid_bits);
      wave->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
      wave->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      *returned = data->FormatSize;
      return TRUE;
    }
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
  negotiated_pcm = false; intersection_supported = true; intersection_queries = 0;
  native_bits = 32; native_valid_bits = 24;
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
  for (const auto layout : {std::pair{32u, 24u}, std::pair{24u, 24u}, std::pair{32u, 20u},
                           std::pair{16u, 16u}, std::pair{24u, 22u}, std::pair{8u, 8u}}) {
    for (const Direction direction : {Direction::input, Direction::output}) {
      reset(); negotiated_pcm = true; native_bits = layout.first; native_valid_bits = layout.second;
      auto native_device = device;
      native_device.direction = direction;
      AudioSettings native_settings;
      (direction == Direction::input ? native_settings.input_device_ids : native_settings.output_device_ids) = {device.id};
      AudioEngine engine;
      check(engine.initialize(native_settings, {native_device}, error) && create_calls == 1 && intersection_queries == 1,
          "Advertised PCM intersection preserves exact container, valid bits and sample size on first open");
    }
  }
  reset(); negotiated_pcm = true; intersection_supported = false;
  {
    AudioEngine engine;
    check(engine.initialize(settings, {device}, error) && intersection_queries == 1,
        "Legacy driver without intersection can open 24 valid bits in a 32-bit container");
  }
  reset(); negotiated_pcm = true; create_error = ERROR_BUSY;
  {
    AudioEngine engine;
    check(!engine.initialize(settings, {device}, error) && create_calls == 1 && intersection_queries == 1 &&
        ks_device_access::is_busy(engine.last_status()), "Negotiated open race stops before legacy retries");
  }
  {
    BYTE sample[4]{};
    encode_sample(sample, SampleEncoding::pcm32, 0.1234567f, 24);
    check(sample[0] == 0 && std::abs(decode_sample(sample, SampleEncoding::pcm32) - 0.1234567f) < 0.000001f,
        "24-in-32 PCM stays left aligned with zero render padding and correct capture scale");
    encode_sample(sample, SampleEncoding::pcm24, -0.1234567f, 20);
    check((sample[0] & 0x0f) == 0 && std::abs(decode_sample(sample, SampleEncoding::pcm24) + 0.1234567f) < 0.000002f,
        "20-in-24 PCM preserves sign, scale and zero padding");
    for (const float value : {-1.0f, 0.0f, 0.5f}) {
      encode_sample(sample, SampleEncoding::pcm8, value, 8);
      check(decode_sample(sample, SampleEncoding::pcm8) == value, "Unsigned 8-bit PCM uses midpoint silence");
    }
    std::vector<BYTE> format;
    ks_audio_format::WaveFormat info{};
    check(SUCCEEDED(ks_audio_format::intersect(nullptr, 0, 2, 48000, 24, false, format, info)),
        "Fixture provides a valid driver intersection");
    auto* data = reinterpret_cast<KSDATAFORMAT*>(format.data());
    auto* wave = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(data + 1);
    wave->Format.nBlockAlign = 1;
    check(!ks_audio_format::validate(format.data(), format.size(), info), "Reject incorrect negotiated frame stride");
    wave->Format.nBlockAlign = 8;
    wave->Samples.wValidBitsPerSample = 33;
    check(!ks_audio_format::validate(format.data(), format.size(), info), "Reject valid bits exceeding container width");
    wave->Samples.wValidBitsPerSample = 24;
    data->FormatSize = 4096;
    check(!ks_audio_format::validate(format.data(), format.size(), info), "Reject truncated driver intersection");
  }
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
