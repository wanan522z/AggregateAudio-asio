#pragma once

#include <windows.h>
#include <winioctl.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace ks_audio_format {

// DATARANGES describes valid sample bits, which need not equal the storage
// width. DATAINTERSECTION supplies the driver's complete connection format.
inline std::vector<KSDATARANGE_AUDIO> read_ranges(HANDLE filter, ULONG pin_id) {
  KSP_PIN property{};
  property.Property = {KSPROPSETID_Pin, KSPROPERTY_PIN_DATARANGES, KSPROPERTY_TYPE_GET};
  property.PinId = pin_id;
  std::vector<BYTE> bytes(128 * 1024);
  DWORD returned{};
  if (!DeviceIoControl(filter, IOCTL_KS_PROPERTY, &property, sizeof(property),
      bytes.data(), static_cast<DWORD>(bytes.size()), &returned, nullptr) ||
      returned < sizeof(KSMULTIPLE_ITEM) || returned > bytes.size()) return {};
  const auto* list = reinterpret_cast<const KSMULTIPLE_ITEM*>(bytes.data());
  if (list->Size < sizeof(KSMULTIPLE_ITEM) || list->Size > returned) return {};
  std::vector<KSDATARANGE_AUDIO> result;
  size_t offset = sizeof(KSMULTIPLE_ITEM);
  for (ULONG index = 0; index < list->Count; ++index) {
    if (offset + sizeof(KSDATARANGE) > list->Size) break;
    const auto* range = reinterpret_cast<const KSDATARANGE*>(bytes.data() + offset);
    if (range->FormatSize < sizeof(KSDATARANGE) || range->FormatSize > list->Size - offset) break;
    if (range->FormatSize >= sizeof(KSDATARANGE_AUDIO) &&
        range->MajorFormat == KSDATAFORMAT_TYPE_AUDIO &&
        (range->Specifier == KSDATAFORMAT_SPECIFIER_WAVEFORMATEX || range->Specifier == GUID_NULL) &&
        (range->SubFormat == KSDATAFORMAT_SUBTYPE_PCM ||
         range->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT || range->SubFormat == GUID_NULL)) {
      const auto& audio = *reinterpret_cast<const KSDATARANGE_AUDIO*>(range);
      if (audio.MaximumChannels && audio.MinimumBitsPerSample <= audio.MaximumBitsPerSample &&
          audio.MinimumSampleFrequency <= audio.MaximumSampleFrequency) result.push_back(audio);
    }
    offset += range->FormatSize;
  }
  return result;
}

inline bool supports(const std::vector<KSDATARANGE_AUDIO>& ranges, unsigned channels,
                     unsigned rate, unsigned valid_bits, bool floating_point) {
  if (ranges.empty()) return true; // Optional metadata missing on legacy drivers.
  const GUID subtype = floating_point ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
  return std::any_of(ranges.begin(), ranges.end(), [&](const auto& range) {
    return (range.DataRange.SubFormat == subtype || range.DataRange.SubFormat == GUID_NULL) &&
        channels <= range.MaximumChannels &&
        valid_bits >= range.MinimumBitsPerSample &&
        (!range.MaximumBitsPerSample || valid_bits <= range.MaximumBitsPerSample) &&
        rate >= range.MinimumSampleFrequency &&
        (!range.MaximumSampleFrequency || rate <= range.MaximumSampleFrequency);
  });
}

struct WaveFormat {
  unsigned channels{}, rate{}, bits{}, valid_bits{};
  bool floating_point{};
};

inline bool validate(const BYTE* bytes, size_t size, WaveFormat& result) {
  constexpr size_t wave_base_size = offsetof(WAVEFORMATEX, cbSize);
  if (size < sizeof(KSDATAFORMAT) + wave_base_size) return false;
  const auto* data = reinterpret_cast<const KSDATAFORMAT*>(bytes);
  if (data->FormatSize > size || data->FormatSize < sizeof(KSDATAFORMAT) + wave_base_size ||
      data->MajorFormat != KSDATAFORMAT_TYPE_AUDIO ||
      data->Specifier != KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) return false;
  const auto* wave = reinterpret_cast<const WAVEFORMATEX*>(data + 1);
  const unsigned extra_size = data->FormatSize >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX) ? wave->cbSize : 0;
  if ((extra_size && sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX) + extra_size > data->FormatSize) ||
      !wave->nChannels || wave->nChannels > 64 ||
      wave->nSamplesPerSec < 8000 || wave->nSamplesPerSec > 384000 ||
      (wave->wBitsPerSample != 8 && wave->wBitsPerSample != 16 &&
       wave->wBitsPerSample != 24 && wave->wBitsPerSample != 32) ||
      wave->nBlockAlign != wave->nChannels * (wave->wBitsPerSample / 8) ||
      wave->nAvgBytesPerSec != wave->nSamplesPerSec * wave->nBlockAlign ||
      (data->SampleSize && data->SampleSize != wave->nBlockAlign)) return false;
  GUID subtype{};
  unsigned valid_bits = wave->wBitsPerSample;
  if (wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    if (extra_size < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return false;
    const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
    subtype = extended->SubFormat;
    valid_bits = extended->Samples.wValidBitsPerSample;
    if (!valid_bits || valid_bits > wave->wBitsPerSample) return false;
  } else if (wave->wFormatTag == WAVE_FORMAT_PCM) subtype = KSDATAFORMAT_SUBTYPE_PCM;
  else if (wave->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) subtype = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  else return false;
  const bool floating_point = subtype == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  if (data->SubFormat != subtype || (subtype != KSDATAFORMAT_SUBTYPE_PCM && !floating_point) ||
      (floating_point && (wave->wBitsPerSample != 32 || valid_bits != 32))) return false;
  result = {wave->nChannels, wave->nSamplesPerSec, wave->wBitsPerSample, valid_bits, floating_point};
  return true;
}

inline HRESULT intersect(HANDLE filter, ULONG pin_id, unsigned channels, unsigned rate,
                         unsigned valid_bits, bool floating_point,
                         std::vector<BYTE>& bytes, WaveFormat& format) {
  struct Request {
    KSP_PIN pin;
    KSMULTIPLE_ITEM multiple;
    KSDATARANGE_AUDIO range;
  } request{};
  request.pin.Property = {KSPROPSETID_Pin, KSPROPERTY_PIN_DATAINTERSECTION, KSPROPERTY_TYPE_GET};
  request.pin.PinId = pin_id;
  request.multiple = {sizeof(KSMULTIPLE_ITEM) + sizeof(KSDATARANGE_AUDIO), 1};
  request.range.DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
  request.range.DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
  request.range.DataRange.SubFormat = floating_point ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
  request.range.DataRange.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
  request.range.MaximumChannels = channels;
  request.range.MinimumBitsPerSample = request.range.MaximumBitsPerSample = valid_bits;
  request.range.MinimumSampleFrequency = request.range.MaximumSampleFrequency = rate;
  bytes.assign(4096, 0);
  DWORD returned{};
  if (!DeviceIoControl(filter, IOCTL_KS_PROPERTY, &request, sizeof(request),
      bytes.data(), static_cast<DWORD>(bytes.size()), &returned, nullptr)) {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
  }
  if (returned > bytes.size() || !validate(bytes.data(), returned, format) ||
      format.channels != channels || format.rate != rate ||
      format.valid_bits != valid_bits || format.floating_point != floating_point)
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  bytes.resize(reinterpret_cast<const KSDATAFORMAT*>(bytes.data())->FormatSize);
  return S_OK;
}

} // namespace ks_audio_format
