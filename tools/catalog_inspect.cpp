// Read-only KS metadata inspection. No streaming pins are created.
#include "../src/device_catalog.cpp"
#include "../src/ks_device_access.h"
#include <cstdio>

std::string utf8(const std::wstring& value) {
  const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
      nullptr, 0, nullptr, nullptr);
  std::string result(count, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count,
      nullptr, nullptr);
  return result;
}

void inspect_access(const AudioEndpoint& endpoint) {
  std::printf("%s %s\n  id=%s\n", endpoint.direction == Direction::input ? "IN" : "OUT",
      utf8(endpoint.name).c_str(), utf8(endpoint.id).c_str());
  auto inspect = [](const std::wstring& path, unsigned pin_id) {
    UniqueHandle filter(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (filter.value == INVALID_HANDLE_VALUE) {
      std::printf("  pin=%u filter-error=%lu\n", pin_id, GetLastError());
      return;
    }
    for (const ULONG id : {KSPROPERTY_PIN_GLOBALCINSTANCES, KSPROPERTY_PIN_CINSTANCES}) {
      KSPIN_CINSTANCES counts{};
      ULONG returned{};
      const HRESULT status = get_pin_property(filter.value, pin_id, id, &counts, sizeof(counts), &returned);
      std::printf("  pin=%u %s status=0x%08lX bytes=%lu current=%lu possible=%lu\n", pin_id,
          id == KSPROPERTY_PIN_GLOBALCINSTANCES ? "global" : "local", static_cast<ULONG>(status),
          returned, counts.CurrentCount, counts.PossibleCount);
    }
    std::printf("  pin=%u access-check=0x%08lX\n", pin_id,
        static_cast<ULONG>(ks_device_access::check_pin(filter.value, pin_id)));
  };
  if (endpoint.ks_pin_candidates.empty()) inspect(endpoint.ks_filter_path, endpoint.ks_pin_id);
  else for (const auto& candidate : endpoint.ks_pin_candidates) inspect(candidate.filter_path, candidate.pin_id);
}

void inspect_formats(const AudioEndpoint& endpoint) {
  std::printf("%s %s\n", endpoint.direction == Direction::input ? "IN" : "OUT",
      utf8(endpoint.name).c_str());
  for (const auto& candidate : endpoint.ks_pin_candidates) {
    UniqueHandle filter(CreateFileW(candidate.filter_path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (filter.value == INVALID_HANDLE_VALUE) continue;
    std::printf("  pin=%u interface=%s/%lu medium=%s/%lu\n", candidate.pin_id,
        utf8(guid_text(candidate.interface_set)).c_str(), candidate.interface_id,
        utf8(guid_text(candidate.medium_set)).c_str(), candidate.medium_id);
    std::vector<BYTE> data(128 * 1024);
    ULONG bytes{};
    const HRESULT status = get_pin_property(filter.value, candidate.pin_id, KSPROPERTY_PIN_DATARANGES,
        data.data(), static_cast<ULONG>(data.size()), &bytes);
    std::printf("  ranges status=0x%08lX bytes=%lu\n", static_cast<ULONG>(status), bytes);
    if (FAILED(status) || bytes < sizeof(KSMULTIPLE_ITEM)) continue;
    const auto* multiple = reinterpret_cast<const KSMULTIPLE_ITEM*>(data.data());
    if (multiple->Size > bytes || multiple->Size < sizeof(KSMULTIPLE_ITEM)) continue;
    size_t offset = sizeof(KSMULTIPLE_ITEM);
    for (ULONG index = 0; index < multiple->Count; ++index) {
      if (offset + sizeof(KSDATARANGE) > multiple->Size) break;
      const auto* range = reinterpret_cast<const KSDATARANGE*>(data.data() + offset);
      if (range->FormatSize < sizeof(KSDATARANGE) || offset + range->FormatSize > multiple->Size) break;
      std::printf("  range %lu size=%lu sample=%lu major=%s sub=%s spec=%s\n", index,
          range->FormatSize, range->SampleSize, utf8(guid_text(range->MajorFormat)).c_str(),
          utf8(guid_text(range->SubFormat)).c_str(), utf8(guid_text(range->Specifier)).c_str());
      if (range->FormatSize >= sizeof(KSDATARANGE_AUDIO) && range->MajorFormat == KSDATAFORMAT_TYPE_AUDIO) {
        const auto* audio = reinterpret_cast<const KSDATARANGE_AUDIO*>(range);
        std::printf("    channels<=%lu bits=%lu..%lu rate=%lu..%lu\n", audio->MaximumChannels,
            audio->MinimumBitsPerSample, audio->MaximumBitsPerSample,
            audio->MinimumSampleFrequency, audio->MaximumSampleFrequency);
        for (const ULONG bits : {32u, 24u, 20u, 16u, 8u}) {
          if (range->Specifier != KSDATAFORMAT_SPECIFIER_WAVEFORMATEX ||
              48000 < audio->MinimumSampleFrequency || 48000 > audio->MaximumSampleFrequency) continue;
          if (bits < audio->MinimumBitsPerSample || bits > audio->MaximumBitsPerSample) continue;
          struct IntersectionRequest {
            KSP_PIN pin;
            KSMULTIPLE_ITEM multiple;
            KSDATARANGE_AUDIO range;
          } request{};
          request.pin.Property = {KSPROPSETID_Pin, KSPROPERTY_PIN_DATAINTERSECTION, KSPROPERTY_TYPE_GET};
          request.pin.PinId = candidate.pin_id;
          request.multiple = {sizeof(KSMULTIPLE_ITEM) + sizeof(KSDATARANGE_AUDIO), 1};
          request.range = *audio;
          request.range.DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
          request.range.MinimumBitsPerSample = request.range.MaximumBitsPerSample = bits;
          request.range.MinimumSampleFrequency = request.range.MaximumSampleFrequency = 48000;
          std::array<BYTE, 4096> result{};
          DWORD returned{};
          const BOOL ok = DeviceIoControl(filter.value, IOCTL_KS_PROPERTY, &request, sizeof(request),
              result.data(), static_cast<DWORD>(result.size()), &returned, nullptr);
          std::printf("    intersection 48000/%lu status=%lu bytes=%lu", bits,
              ok ? ERROR_SUCCESS : GetLastError(), returned);
          if (ok && returned >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX) &&
              reinterpret_cast<const KSDATAFORMAT*>(result.data())->Specifier == KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) {
            const auto* format = reinterpret_cast<const KSDATAFORMAT*>(result.data());
            const auto* wave = reinterpret_cast<const WAVEFORMATEX*>(format + 1);
            std::printf(" size=%lu sample=%lu tag=0x%04X channels=%u rate=%lu container=%u cb=%u",
                format->FormatSize, format->SampleSize, wave->wFormatTag, wave->nChannels,
                wave->nSamplesPerSec, wave->wBitsPerSample, wave->cbSize);
            if (wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE && returned >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE)) {
              const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
              std::printf(" valid=%u mask=0x%08lX", extended->Samples.wValidBitsPerSample, extended->dwChannelMask);
            }
          }
          std::printf("\n");
        }
      }
      offset += range->FormatSize;
    }
  }
}

int wmain(int argc, wchar_t** argv) {
  if (argc >= 2 && std::wstring(argv[1]) == L"--formats") {
    const std::wstring name_filter = argc >= 3 ? argv[2] : L"";
    unsigned matches{};
    for (const auto& endpoint : enumerate_endpoints()) {
      if (endpoint.name.find(name_filter) == std::wstring::npos) continue;
      inspect_formats(endpoint);
      ++matches;
    }
    std::printf("Read-only format inspection: %u endpoints; no streaming pins created.\n", matches);
    return matches ? 0 : 1;
  }
  if (argc >= 2 && std::wstring(argv[1]) == L"--access") {
    const std::wstring name_filter = argc >= 3 ? argv[2] : L"";
    unsigned matches{};
    for (const auto& endpoint : enumerate_endpoints()) {
      if (endpoint.name.find(name_filter) == std::wstring::npos) continue;
      inspect_access(endpoint);
      ++matches;
    }
    std::printf("Read-only inspection: %u endpoints; no streaming pins created.\n", matches);
    return matches ? 0 : 1;
  }
  std::unordered_set<std::wstring> seen;
  for (const auto& endpoint : enumerate_endpoints()) {
    std::printf("%s %s\n  device=%s\n  id=%s pin=%u channels=%u\n  path=%s\n",
        endpoint.direction == Direction::input ? "IN" : "OUT", utf8(endpoint.name).c_str(),
        utf8(endpoint.device_instance_id).c_str(), utf8(endpoint.id).c_str(), endpoint.ks_pin_id,
        endpoint.channels, utf8(endpoint.ks_filter_path).c_str());
    if (!seen.insert(normalize_audio_endpoint_path(endpoint.ks_filter_path)).second) continue;
    UniqueHandle filter(CreateFileW(endpoint.ks_filter_path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (filter.value == INVALID_HANDLE_VALUE) continue;
    ULONG count{};
    get_filter_property(filter.value, KSPROPERTY_PIN_CTYPES, &count, sizeof(count));
    for (ULONG pin = 0; pin < count; ++pin) {
      std::array<BYTE, 8192> data{};
      ULONG bytes{};
      if (SUCCEEDED(get_pin_property(filter.value, pin, KSPROPERTY_PIN_PHYSICALCONNECTION,
          data.data(), static_cast<ULONG>(data.size()), &bytes)) && bytes >= sizeof(KSPIN_PHYSICALCONNECTION)) {
        const auto* link = reinterpret_cast<KSPIN_PHYSICALCONNECTION*>(data.data());
        if (link->Size <= bytes) {
          std::wstring path = link->SymbolicLinkName;
          std::printf("  physical pin=%lu target=%lu %s\n", pin, link->Pin, utf8(path).c_str());
          if (path.starts_with(L"\\??\\")) path.replace(0, 4, L"\\\\?\\");
          UniqueHandle topology(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
          if (topology.value == INVALID_HANDLE_VALUE) continue;
          ULONG topology_count{};
          get_filter_property(topology.value, KSPROPERTY_PIN_CTYPES, &topology_count, sizeof(topology_count));
          for (ULONG terminal = 0; terminal < topology_count; ++terminal) {
            GUID category{};
            get_pin_category(topology.value, terminal, category);
            std::printf("    topology pin=%lu category=%s name=%s\n", terminal,
                utf8(guid_text(category)).c_str(), utf8(get_pin_name(topology.value, terminal)).c_str());
            KSP_PIN property{};
            property.Property.Set = KSPROPSETID_Jack;
            property.Property.Id = KSPROPERTY_JACK_CONTAINERID;
            property.Property.Flags = KSPROPERTY_TYPE_GET;
            property.PinId = terminal;
            GUID container{};
            DWORD returned{};
            if (DeviceIoControl(topology.value, IOCTL_KS_PROPERTY, &property, sizeof(property),
                &container, sizeof(container), &returned, nullptr))
              std::printf("    jack-container=%s\n", utf8(guid_text(container)).c_str());
          }
        }
      }
    }
    KSPROPERTY property{KSPROPSETID_Topology, KSPROPERTY_TOPOLOGY_CONNECTIONS, KSPROPERTY_TYPE_GET};
    std::array<BYTE, 65536> data{};
    DWORD bytes{};
    if (DeviceIoControl(filter.value, IOCTL_KS_PROPERTY, &property, sizeof(property),
        data.data(), static_cast<DWORD>(data.size()), &bytes, nullptr) && bytes >= sizeof(KSMULTIPLE_ITEM)) {
      const auto* list = reinterpret_cast<KSMULTIPLE_ITEM*>(data.data());
      if (list->Count <= (bytes - sizeof(KSMULTIPLE_ITEM)) / sizeof(KSTOPOLOGY_CONNECTION)) {
        const auto* connections = reinterpret_cast<KSTOPOLOGY_CONNECTION*>(data.data() + sizeof(KSMULTIPLE_ITEM));
        for (ULONG index = 0; index < list->Count; ++index) {
          const auto& connection = connections[index];
          std::printf("  graph %lu/%lu -> %lu/%lu\n", connection.FromNode, connection.FromNodePin,
              connection.ToNode, connection.ToNodePin);
        }
      }
    }
  }
}
