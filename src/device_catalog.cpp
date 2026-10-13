#include "device_catalog.h"

#include <windows.h>
#include <objbase.h>
#include <winioctl.h>
#include <setupapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include "ks_audio_format.h"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr DEVPROPKEY kDeviceContainerIdKey{
    {0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
constexpr PROPERTYKEY kAudioEndpointKsPathKey{
    {0x233164c8, 0x1b2c, 0x4c7d, {0xbc, 0x68, 0xb6, 0x71, 0x68, 0x7a, 0x25, 0x67}}, 1};

struct AudioEndpointName {
  Direction direction{};
  std::wstring path;
  std::wstring name;
};

bool same_text(const std::wstring& left, const std::wstring& right);

struct UniqueHandle {
  HANDLE value{INVALID_HANDLE_VALUE};
  ~UniqueHandle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE handle) : value(handle) {}
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;
};

struct DeviceInfoSet {
  HDEVINFO value{INVALID_HANDLE_VALUE};
  ~DeviceInfoSet() { if (value != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value); }
};

HRESULT get_filter_property(HANDLE filter, ULONG id, void* output,
                            ULONG output_size, ULONG* returned = nullptr) {
  KSPROPERTY property{};
  property.Set = KSPROPSETID_Pin;
  property.Id = id;
  property.Flags = KSPROPERTY_TYPE_GET;
  ULONG bytes = 0;
  const BOOL ok = DeviceIoControl(filter, IOCTL_KS_PROPERTY,
      &property, sizeof(property), output, output_size, &bytes, nullptr);
  if (returned) *returned = bytes;
  return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

HRESULT get_pin_property(HANDLE filter, ULONG pin_id, ULONG id, void* output,
                         ULONG output_size, ULONG* returned = nullptr) {
  KSP_PIN property{};
  property.Property.Set = KSPROPSETID_Pin;
  property.Property.Id = id;
  property.Property.Flags = KSPROPERTY_TYPE_GET;
  property.PinId = pin_id;
  ULONG bytes = 0;
  const BOOL ok = DeviceIoControl(filter, IOCTL_KS_PROPERTY,
      &property, sizeof(property), output, output_size, &bytes, nullptr);
  if (returned) *returned = bytes;
  return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

std::wstring get_registry_name(HDEVINFO devices, SP_DEVINFO_DATA& info) {
  std::array<wchar_t, 1024> name{};
  DWORD type = 0;
  DWORD bytes = 0;
  if (SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_FRIENDLYNAME,
                                        &type, reinterpret_cast<PBYTE>(name.data()),
                                        static_cast<DWORD>(name.size() * sizeof(wchar_t)),
                                        &bytes) && name[0]) {
    return name.data();
  }
  if (SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_DEVICEDESC,
                                        &type, reinterpret_cast<PBYTE>(name.data()),
                                        static_cast<DWORD>(name.size() * sizeof(wchar_t)),
                                        &bytes) && name[0]) {
    return name.data();
  }
  return L"Kernel Streaming audio filter";
}

std::wstring guid_text(const GUID& value) {
  wchar_t text[40]{};
  if (!StringFromGUID2(value, text, static_cast<int>(std::size(text)))) return {};
  return text;
}

std::wstring get_container_identity(HDEVINFO devices, SP_DEVINFO_DATA& info) {
  GUID container_id{};
  DEVPROPTYPE property_type = 0;
  if (SetupDiGetDevicePropertyW(devices, &info, &kDeviceContainerIdKey,
          &property_type, reinterpret_cast<PBYTE>(&container_id), sizeof(container_id),
          nullptr, 0) && property_type == DEVPROP_TYPE_GUID &&
      !IsEqualGUID(container_id, GUID_NULL)) {
    return L"container:" + guid_text(container_id);
  }
  return {};
}

std::wstring get_device_identity(HDEVINFO devices, SP_DEVINFO_DATA& info) {
  const auto container = get_container_identity(devices, info);
  // Windows uses this shared container for built-in devices. It is not a
  // sound-card identity; unrelated on-board adapters must keep separate groups.
  if (!container.empty() && !same_text(container, L"container:{00000000-0000-0000-FFFF-FFFFFFFFFFFF}"))
    return container;

  DWORD required = 0;
  SetupDiGetDeviceInstanceIdW(devices, &info, nullptr, 0, &required);
  if (!required) return {};
  std::wstring result(required, L'\0');
  if (!SetupDiGetDeviceInstanceIdW(devices, &info, result.data(), required, nullptr)) return {};
  result.resize(wcslen(result.c_str()));
  return L"instance:" + result;
}

std::wstring normalize_audio_endpoint_path(const std::wstring& path) {
  auto result = path;
  if (result.starts_with(L"\\??\\")) result.replace(0, 4, L"\\\\?\\");
  const auto prefix = result.find(L"\\\\?\\");
  if (prefix != std::wstring::npos) result = result.substr(prefix);
  std::transform(result.begin(), result.end(), result.begin(), towlower);
  // These interface classes are aliases of the same filter. Keep its reference
  // string: that suffix distinguishes the USB channel pairs and real ports.
  const auto category = result.find(L"#{");
  if (category != std::wstring::npos) {
    const auto end = result.find(L'}', category);
    if (end != std::wstring::npos) {
      const auto value = result.substr(category + 1, end - category);
      if (value == L"{6994ad04-93ef-11d0-a3cc-00a0c9223196}" ||
          value == L"{65e8773d-8f56-11d0-a3b9-00a0c9223196}" ||
          value == L"{65e8773e-8f56-11d0-a3b9-00a0c9223196}") {
        result.replace(category + 1, end - category, L"{audio-filter}");
      }
    }
  }
  return result;
}

struct PhysicalPort {
  std::wstring id;
  std::wstring name;
  bool render_connector{};
};

std::vector<KSTOPOLOGY_CONNECTION> topology_connections(HANDLE filter) {
  KSPROPERTY property{KSPROPSETID_Topology, KSPROPERTY_TOPOLOGY_CONNECTIONS, KSPROPERTY_TYPE_GET};
  std::array<BYTE, 65536> bytes{};
  DWORD returned{};
  if (!DeviceIoControl(filter, IOCTL_KS_PROPERTY, &property, sizeof(property), bytes.data(),
      static_cast<DWORD>(bytes.size()), &returned, nullptr) || returned < sizeof(KSMULTIPLE_ITEM)) return {};
  const auto* list = reinterpret_cast<const KSMULTIPLE_ITEM*>(bytes.data());
  if (list->Size > returned || list->Size < sizeof(KSMULTIPLE_ITEM) ||
      list->Count > (list->Size - sizeof(KSMULTIPLE_ITEM)) / sizeof(KSTOPOLOGY_CONNECTION)) return {};
  const auto* connections = reinterpret_cast<const KSTOPOLOGY_CONNECTION*>(bytes.data() + sizeof(KSMULTIPLE_ITEM));
  return {connections, connections + list->Count};
}

std::vector<ULONG> connected_filter_pins(const std::vector<KSTOPOLOGY_CONNECTION>& graph, ULONG stream_pin) {
  std::unordered_set<ULONG> nodes;
  std::unordered_set<ULONG> pins{stream_pin};
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& edge : graph) {
      const bool from = edge.FromNode == KSFILTER_NODE ? edge.FromNodePin == stream_pin : nodes.contains(edge.FromNode);
      const bool to = edge.ToNode == KSFILTER_NODE ? edge.ToNodePin == stream_pin : nodes.contains(edge.ToNode);
      if (!from && !to) continue;
      // Traversal stops at factory pins: independent stream pins do not bridge
      // otherwise disconnected processing paths just because their IDs match.
      if (edge.FromNode == KSFILTER_NODE) changed |= pins.insert(edge.FromNodePin).second;
      else changed |= nodes.insert(edge.FromNode).second;
      if (edge.ToNode == KSFILTER_NODE) changed |= pins.insert(edge.ToNodePin).second;
      else changed |= nodes.insert(edge.ToNode).second;
    }
  }
  return {pins.begin(), pins.end()};
}

bool get_pin_category(HANDLE filter, ULONG pin_id, GUID& category);
std::wstring get_pin_name(HANDLE filter, ULONG pin_id);
std::wstring category_name(const GUID& category, Direction direction);

PhysicalPort physical_port_for_pin(HANDLE filter, ULONG stream_pin,
                                   const std::vector<KSTOPOLOGY_CONNECTION>& graph) {
  PhysicalPort result;
  std::wstring topology_path;
  ULONG bridge_pin{};
  for (const auto pin : connected_filter_pins(graph, stream_pin)) {
    std::array<BYTE, 8192> bytes{};
    ULONG returned{};
    if (FAILED(get_pin_property(filter, pin, KSPROPERTY_PIN_PHYSICALCONNECTION, bytes.data(),
        static_cast<ULONG>(bytes.size()), &returned)) || returned < sizeof(KSPIN_PHYSICALCONNECTION)) continue;
    const auto* link = reinterpret_cast<const KSPIN_PHYSICALCONNECTION*>(bytes.data());
    constexpr auto offset = offsetof(KSPIN_PHYSICALCONNECTION, SymbolicLinkName);
    if (link->Size > returned || link->Size < offset + sizeof(wchar_t)) continue;
    const auto length = (link->Size - offset) / sizeof(wchar_t);
    const auto* end = std::find(link->SymbolicLinkName, link->SymbolicLinkName + length, L'\0');
    if (end == link->SymbolicLinkName + length || end == link->SymbolicLinkName) continue;
    const std::wstring path(link->SymbolicLinkName, end);
    const auto id = normalize_audio_endpoint_path(path) + L"|bridge=" + std::to_wstring(link->Pin);
    // Ambiguous branching topology is not sufficient evidence for merging.
    if (!result.id.empty() && result.id != id) return {};
    result.id = id;
    topology_path = path;
    if (topology_path.starts_with(L"\\??\\")) topology_path.replace(0, 4, L"\\\\?\\");
    bridge_pin = link->Pin;
  }
  if (result.id.empty()) return result;
  UniqueHandle topology(CreateFileW(topology_path.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (topology.value == INVALID_HANDLE_VALUE) return result;
  ULONG count{};
  if (FAILED(get_filter_property(topology.value, KSPROPERTY_PIN_CTYPES, &count, sizeof(count))) || count > 256)
    return result;
  unsigned terminals{};
  for (ULONG pin = 0; pin < count; ++pin) {
    if (pin == bridge_pin) continue;
    GUID category{};
    if (!get_pin_category(topology.value, pin, category) || IsEqualGUID(category, KSCATEGORY_AUDIO)) continue;
    ++terminals;
    result.name = get_pin_name(topology.value, pin);
    if (IsEqualGUID(category, KSNODETYPE_SPEAKER) || IsEqualGUID(category, KSNODETYPE_DESKTOP_SPEAKER) ||
        IsEqualGUID(category, KSNODETYPE_ROOM_SPEAKER)) result.name = L"扬声器";
    else if (IsEqualGUID(category, KSNODETYPE_HEADPHONES)) result.name = L"耳机";
    else if (result.name.empty()) result.name = category_name(category, Direction::input);
    KSPIN_DATAFLOW flow{};
    result.render_connector = SUCCEEDED(get_pin_property(topology.value, pin, KSPROPERTY_PIN_DATAFLOW,
        &flow, sizeof(flow))) && flow == KSPIN_DATAFLOW_OUT;
  }
  if (terminals != 1) { result.name.clear(); result.render_connector = false; }
  return result;
}

std::vector<AudioEndpointName> enumerate_audio_endpoint_names() {
  std::vector<AudioEndpointName> result;
  const HRESULT init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninitialize = SUCCEEDED(init_result);
  if (FAILED(init_result) && init_result != RPC_E_CHANGED_MODE) return result;

  IMMDeviceEnumerator* enumerator = nullptr;
  if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&enumerator)))) {
    for (const auto flow : {eRender, eCapture}) {
      IMMDeviceCollection* collection = nullptr;
      if (FAILED(enumerator->EnumAudioEndpoints(flow,
          DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_UNPLUGGED, &collection))) continue;
      UINT count = 0;
      if (SUCCEEDED(collection->GetCount(&count))) {
        for (UINT index = 0; index < count; ++index) {
          IMMDevice* device = nullptr;
          if (FAILED(collection->Item(index, &device))) continue;
          IPropertyStore* properties = nullptr;
          if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT path_value{};
            PROPVARIANT name_value{};
            PropVariantInit(&path_value);
            PropVariantInit(&name_value);
            const HRESULT path_result = properties->GetValue(kAudioEndpointKsPathKey, &path_value);
            const HRESULT name_result = properties->GetValue(PKEY_Device_DeviceDesc, &name_value);
            if (SUCCEEDED(path_result) && path_value.vt == VT_LPWSTR && path_value.pwszVal &&
                SUCCEEDED(name_result) && name_value.vt == VT_LPWSTR && name_value.pwszVal &&
                path_value.pwszVal[0] && name_value.pwszVal[0]) {
              result.push_back({flow == eCapture ? Direction::input : Direction::output,
                                normalize_audio_endpoint_path(path_value.pwszVal),
                                name_value.pwszVal});
            }
            PropVariantClear(&path_value);
            PropVariantClear(&name_value);
            properties->Release();
          }
          device->Release();
        }
      }
      collection->Release();
    }
    enumerator->Release();
  }
  if (uninitialize) CoUninitialize();
  return result;
}

bool path_prefix_match(const std::wstring& left, const std::wstring& right) {
  const auto common = std::min(left.size(), right.size());
  if (!common || CompareStringOrdinal(left.data(), static_cast<int>(common),
                                      right.data(), static_cast<int>(common), TRUE) != CSTR_EQUAL) {
    return false;
  }
  return left.size() == right.size() || left[common] == L'\\' || right[common] == L'\\';
}

std::wstring endpoint_name_for_path(const std::wstring& filter_path, Direction direction,
                                    const std::vector<AudioEndpointName>& endpoint_names) {
  const auto normalized_filter_path = normalize_audio_endpoint_path(filter_path);
  const AudioEndpointName* best = nullptr;
  size_t best_match_length = 0;
  for (const auto& item : endpoint_names) {
    if (item.direction != direction || !path_prefix_match(normalized_filter_path, item.path)) continue;
    const size_t match_length = std::min(normalized_filter_path.size(), item.path.size());
    if (match_length > best_match_length) {
      best = &item;
      best_match_length = match_length;
    }
  }
  return best ? best->name : std::wstring{};
}

bool contains_case_insensitive(const std::vector<std::wstring>& items, const std::wstring& value) {
  return std::any_of(items.begin(), items.end(), [&](const auto& item) {
    return same_text(item, value);
  });
}

bool specific_pin_name(const std::wstring& name) {
  return !name.empty() && !same_text(name, L"Capture") && !same_text(name, L"Render");
}

void merge_endpoint(AudioEndpoint& target, AudioEndpoint endpoint) {
  const auto candidate = std::move(endpoint.ks_pin_candidates.front());
  const bool new_preferred = std::find(candidate.sample_rates.begin(), candidate.sample_rates.end(), 48000u) !=
                                 candidate.sample_rates.end() &&
      (target.ks_pin_candidates.empty() ||
       std::find(target.ks_pin_candidates.front().sample_rates.begin(),
                 target.ks_pin_candidates.front().sample_rates.end(), 48000u) ==
           target.ks_pin_candidates.front().sample_rates.end() ||
       (!target.physical_port_id.empty() && target.physical_port_id == endpoint.physical_port_id &&
        candidate.pin_id < target.ks_pin_candidates.front().pin_id));
  const bool duplicate_candidate = std::any_of(target.ks_pin_candidates.begin(),
      target.ks_pin_candidates.end(), [&](const auto& existing) {
        return normalize_audio_endpoint_path(existing.filter_path) ==
                   normalize_audio_endpoint_path(candidate.filter_path) &&
               existing.pin_id == candidate.pin_id;
      });
  if (!duplicate_candidate) {
    if (new_preferred) target.ks_pin_candidates.insert(target.ks_pin_candidates.begin(), candidate);
    else target.ks_pin_candidates.push_back(candidate);
  }
  if (new_preferred) {
    target.ks_filter_path = endpoint.ks_filter_path;
    target.ks_pin_id = endpoint.ks_pin_id;
    target.ks_interface_set = endpoint.ks_interface_set;
    target.ks_interface_id = endpoint.ks_interface_id;
    target.ks_medium_set = endpoint.ks_medium_set;
    target.ks_medium_id = endpoint.ks_medium_id;
    target.name = endpoint.name;
    target.pin_role = endpoint.pin_role;
  }
  target.channels = std::max(target.channels, endpoint.channels);
  target.minimum_sample_rate = target.minimum_sample_rate
      ? std::min(target.minimum_sample_rate, endpoint.minimum_sample_rate) : endpoint.minimum_sample_rate;
  target.maximum_sample_rate = std::max(target.maximum_sample_rate, endpoint.maximum_sample_rate);
  for (const auto rate : endpoint.native_sample_rates) target.native_sample_rates.push_back(rate);
  std::ranges::sort(target.native_sample_rates);
  target.native_sample_rates.erase(std::unique(target.native_sample_rates.begin(),
      target.native_sample_rates.end()), target.native_sample_rates.end());
  for (const auto& format : endpoint.native_formats) {
    if (std::find(target.native_formats.begin(), target.native_formats.end(), format) ==
        target.native_formats.end()) target.native_formats.push_back(format);
  }
  for (const auto& legacy : endpoint.legacy_ids) {
    if (!contains_case_insensitive(target.legacy_ids, legacy)) target.legacy_ids.push_back(legacy);
  }
  if (endpoint.id != target.id && !contains_case_insensitive(target.legacy_ids, endpoint.id)) {
    target.legacy_ids.push_back(endpoint.id);
  }
}

void append_endpoint(std::vector<AudioEndpoint>& endpoints, AudioEndpoint endpoint) {
  const auto existing = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& item) {
    if (item.direction != endpoint.direction || item.device_instance_id != endpoint.device_instance_id) return false;
    if (!item.physical_port_id.empty() && item.physical_port_id == endpoint.physical_port_id &&
        item.channels == endpoint.channels) return true;
    return std::any_of(item.ks_pin_candidates.begin(), item.ks_pin_candidates.end(), [&](const auto& candidate) {
          return candidate.pin_id == endpoint.ks_pin_id && normalize_audio_endpoint_path(candidate.filter_path) ==
              normalize_audio_endpoint_path(endpoint.ks_filter_path);
        });
  });
  if (existing == endpoints.end()) endpoints.push_back(std::move(endpoint));
  else merge_endpoint(*existing, std::move(endpoint));
}

std::wstring get_pin_name(HANDLE filter, ULONG pin_id) {
  std::array<wchar_t, 512> name{};
  if (SUCCEEDED(get_pin_property(filter, pin_id, KSPROPERTY_PIN_NAME,
                                 name.data(), static_cast<ULONG>(sizeof(name)))) && name[0]) {
    return name.data();
  }
  return {};
}

bool get_pin_category(HANDLE filter, ULONG pin_id, GUID& category) {
  return SUCCEEDED(get_pin_property(filter, pin_id, KSPROPERTY_PIN_CATEGORY,
                                    &category, sizeof(category)));
}

std::wstring category_name(const GUID& category, Direction direction) {
  if (IsEqualGUID(category, KSNODETYPE_MICROPHONE) ||
      IsEqualGUID(category, KSNODETYPE_DESKTOP_MICROPHONE)) return L"Microphone";
  if (IsEqualGUID(category, KSNODETYPE_LINE_CONNECTOR)) {
    return direction == Direction::input ? L"Line In" : L"Line Out";
  }
  if (IsEqualGUID(category, KSNODETYPE_SPDIF_INTERFACE)) {
    return direction == Direction::input ? L"S/PDIF In" : L"S/PDIF Out";
  }
  if (IsEqualGUID(category, KSNODETYPE_HEADPHONES)) return L"Headphones";
  if (IsEqualGUID(category, KSNODETYPE_SPEAKER) ||
      IsEqualGUID(category, KSNODETYPE_DESKTOP_SPEAKER) ||
      IsEqualGUID(category, KSNODETYPE_ROOM_SPEAKER)) return L"Speakers";
  return {};
}

bool same_text(const std::wstring& left, const std::wstring& right) {
  return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                              right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring stable_endpoint_id(const std::wstring& identity) {
  uint64_t hash = 14695981039346656037ULL;
  for (const wchar_t character : identity) {
    const auto value = static_cast<uint16_t>(character);
    hash ^= static_cast<uint8_t>(value & 0xff);
    hash *= 1099511628211ULL;
    hash ^= static_cast<uint8_t>(value >> 8);
    hash *= 1099511628211ULL;
  }
  return L"ks:" + std::to_wstring(hash);
}

bool get_pin_interface_and_medium(HANDLE filter, ULONG pin_id,
                                  GUID& interface_set, ULONG& interface_id,
                                  GUID& medium_set, ULONG& medium_id) {
  interface_set = KSINTERFACESETID_Standard;
  interface_id = KSINTERFACE_STANDARD_STREAMING;
  medium_set = KSMEDIUMSETID_Standard;
  medium_id = KSMEDIUM_TYPE_ANYINSTANCE;

  std::vector<BYTE> interfaces(64 * 1024);
  ULONG bytes = 0;
  if (SUCCEEDED(get_pin_property(filter, pin_id, KSPROPERTY_PIN_INTERFACES,
                                 interfaces.data(), static_cast<ULONG>(interfaces.size()), &bytes)) &&
      bytes >= sizeof(KSMULTIPLE_ITEM) + sizeof(KSPIN_INTERFACE)) {
    const auto* interface_list = reinterpret_cast<const KSMULTIPLE_ITEM*>(interfaces.data());
    if (interface_list->Size <= bytes && interface_list->Count > 0 &&
        interface_list->Size >= sizeof(KSMULTIPLE_ITEM) +
            interface_list->Count * sizeof(KSPIN_INTERFACE)) {
      const auto* interface_items = reinterpret_cast<const KSPIN_INTERFACE*>(
          interfaces.data() + sizeof(KSMULTIPLE_ITEM));
      for (ULONG index = 0; index < interface_list->Count; ++index) {
        if (IsEqualGUID(interface_items[index].Set, KSINTERFACESETID_Standard) &&
            interface_items[index].Id == KSINTERFACE_STANDARD_STREAMING) {
          interface_set = interface_items[index].Set;
          interface_id = interface_items[index].Id;
          break;
        }
        if (index == 0) {
          interface_set = interface_items[index].Set;
          interface_id = interface_items[index].Id;
        }
      }
    }
  }

  std::vector<BYTE> mediums(64 * 1024);
  bytes = 0;
  if (SUCCEEDED(get_pin_property(filter, pin_id, KSPROPERTY_PIN_MEDIUMS,
                                 mediums.data(), static_cast<ULONG>(mediums.size()), &bytes)) &&
      bytes >= sizeof(KSMULTIPLE_ITEM) + sizeof(KSPIN_MEDIUM)) {
    const auto* medium_list = reinterpret_cast<const KSMULTIPLE_ITEM*>(mediums.data());
    if (medium_list->Size <= bytes && medium_list->Count > 0 &&
        medium_list->Size >= sizeof(KSMULTIPLE_ITEM) +
            medium_list->Count * sizeof(KSPIN_MEDIUM)) {
      const auto* medium_items = reinterpret_cast<const KSPIN_MEDIUM*>(
          mediums.data() + sizeof(KSMULTIPLE_ITEM));
      medium_set = medium_items[0].Set;
      medium_id = medium_items[0].Id;
      for (ULONG index = 0; index < medium_list->Count; ++index) {
        if (IsEqualGUID(medium_items[index].Set, KSMEDIUMSETID_Standard) &&
            medium_items[index].Id == KSMEDIUM_TYPE_ANYINSTANCE) {
          medium_set = medium_items[index].Set;
          medium_id = medium_items[index].Id;
          break;
        }
      }
    }
  }
  return true;
}

bool summarize_pin_formats(HANDLE filter, ULONG pin_id, AudioEndpoint& endpoint) {
  const auto ranges = ks_audio_format::read_ranges(filter, pin_id);
  unsigned minimum_rate = 384000;
  unsigned maximum_rate = 0;
  unsigned maximum_channels = 0;
  unsigned minimum_bits = 32;
  unsigned maximum_bits = 0;
  constexpr std::array<unsigned, 13> rates{
      8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
  constexpr std::array<unsigned, 5> bit_depths{32, 24, 20, 16, 8};

  for (const auto& audio : ranges) {
    const auto* range = &audio;
    const unsigned min_rate = range->MinimumSampleFrequency ? range->MinimumSampleFrequency : 8000;
    const unsigned max_rate = range->MaximumSampleFrequency ? range->MaximumSampleFrequency : 384000;
    const unsigned min_depth = range->MinimumBitsPerSample ? range->MinimumBitsPerSample : 8;
    const unsigned max_depth = range->MaximumBitsPerSample ? range->MaximumBitsPerSample : 32;
    maximum_channels = std::max(maximum_channels,
                                static_cast<unsigned>(range->MaximumChannels));
    minimum_rate = std::min(minimum_rate, min_rate);
    maximum_rate = std::max(maximum_rate, max_rate);
    minimum_bits = std::min(minimum_bits, min_depth);
    maximum_bits = std::max(maximum_bits, max_depth);

    auto range_rates = std::vector<unsigned>(rates.begin(), rates.end());
    range_rates.push_back(min_rate);
    range_rates.push_back(max_rate);
    for (const unsigned rate : range_rates) {
      if (rate < 8000 || rate > 384000) continue;
      if (rate < min_rate || rate > max_rate) continue;
      endpoint.native_sample_rates.push_back(rate);
      for (const unsigned depth : bit_depths) {
        const unsigned tested_depth = depth;
        if (tested_depth < min_depth || tested_depth > max_depth) continue;
        const bool is_float = IsEqualGUID(range->DataRange.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        if (is_float && depth != 32) continue;
        endpoint.native_formats.push_back(std::to_string(tested_depth) +
            (is_float ? "-bit float @ " : "-bit PCM @ ") + std::to_string(rate));
      }
    }
  }

  std::ranges::sort(endpoint.native_sample_rates);
  endpoint.native_sample_rates.erase(
      std::unique(endpoint.native_sample_rates.begin(), endpoint.native_sample_rates.end()),
      endpoint.native_sample_rates.end());
  std::ranges::sort(endpoint.native_formats);
  endpoint.native_formats.erase(
      std::unique(endpoint.native_formats.begin(), endpoint.native_formats.end()),
      endpoint.native_formats.end());
  if (maximum_channels == 0 || maximum_rate == 0 || endpoint.native_sample_rates.empty()) return false;

  endpoint.minimum_sample_rate = minimum_rate;
  endpoint.maximum_sample_rate = maximum_rate;
  endpoint.channels = std::clamp(maximum_channels, 1u, 64u);
  endpoint.minimum_period_100ns = 0;
  endpoint.default_period_100ns = 0;
  endpoint.sample_rate = std::find(endpoint.native_sample_rates.begin(),
                                   endpoint.native_sample_rates.end(), 48000u) !=
                        endpoint.native_sample_rates.end()
      ? 48000 : endpoint.native_sample_rates.front();
  endpoint.bits_per_sample = std::clamp(maximum_bits, 8u, 32u);
  (void)minimum_bits;
  return true;
}

void append_filter_pins(HDEVINFO devices, SP_DEVICE_INTERFACE_DATA& interface_data,
                        std::vector<AudioEndpoint>& endpoints,
                        std::unordered_set<std::wstring>& seen_filter_paths,
                        const std::vector<AudioEndpointName>& endpoint_names) {
  DWORD required = 0;
  SP_DEVINFO_DATA device_info{};
  device_info.cbSize = sizeof(device_info);
  SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, nullptr, 0, &required, &device_info);
  if (!required) return;
  std::vector<BYTE> detail_buffer(required);
  auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_buffer.data());
  detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
  device_info = {};
  device_info.cbSize = sizeof(device_info);
  if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail, required,
                                        nullptr, &device_info)) return;
  std::wstring normalized_path = detail->DevicePath;
  std::transform(normalized_path.begin(), normalized_path.end(), normalized_path.begin(), towlower);
  if (!seen_filter_paths.insert(std::move(normalized_path)).second) return;
  auto device_instance_id = get_device_identity(devices, device_info);
  const auto previous_device_id = get_container_identity(devices, device_info);
  if (device_instance_id.empty()) device_instance_id = L"path:" + std::wstring(detail->DevicePath);

  UniqueHandle filter(CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!filter.value || filter.value == INVALID_HANDLE_VALUE) return;

  ULONG pin_count = 0;
  if (FAILED(get_filter_property(filter.value, KSPROPERTY_PIN_CTYPES,
                                 &pin_count, sizeof(pin_count))) || pin_count == 0) return;
  const auto filter_name = get_registry_name(devices, device_info);
  const auto graph = topology_connections(filter.value);
  for (ULONG pin_id = 0; pin_id < pin_count; ++pin_id) {
    KSPIN_DATAFLOW dataflow{};
    if (FAILED(get_pin_property(filter.value, pin_id, KSPROPERTY_PIN_DATAFLOW,
                                &dataflow, sizeof(dataflow)))) continue;

    // Dataflow describes audio traveling into or out of the filter. The
    // separate Communication property describes IRP connectivity and must not
    // be used to decide whether this is a capture or render stream.
    if (dataflow != KSPIN_DATAFLOW_IN && dataflow != KSPIN_DATAFLOW_OUT) continue;
    KSPIN_COMMUNICATION communication{};
    if (SUCCEEDED(get_pin_property(filter.value, pin_id, KSPROPERTY_PIN_COMMUNICATION,
            &communication, sizeof(communication))) &&
        (communication == KSPIN_COMMUNICATION_NONE || communication == KSPIN_COMMUNICATION_BRIDGE)) continue;
    const Direction direction = dataflow == KSPIN_DATAFLOW_OUT
                                    ? Direction::input : Direction::output;

    AudioEndpoint endpoint;
    endpoint.direction = direction;
    endpoint.device_name = filter_name;
    endpoint.device_instance_id = device_instance_id;
    if (!previous_device_id.empty() && previous_device_id != device_instance_id)
      endpoint.legacy_device_ids.push_back(previous_device_id);
    endpoint.ks_filter_path = detail->DevicePath;
    endpoint.ks_pin_id = pin_id;
    endpoint.pin_category = GUID_NULL;
    endpoint.has_pin_category = get_pin_category(filter.value, pin_id, endpoint.pin_category);
    const auto pin_name = get_pin_name(filter.value, pin_id);
    const auto mmdevice_name = endpoint_name_for_path(endpoint.ks_filter_path, direction, endpoint_names);
    const auto category_label = endpoint.has_pin_category
        ? category_name(endpoint.pin_category, direction) : std::wstring{};
    const auto physical = physical_port_for_pin(filter.value, pin_id, graph);
    endpoint.physical_port_id = physical.id;
    if (direction == Direction::input && physical.render_connector && !physical.name.empty()) {
      endpoint.pin_role = physical.name + L"回送";
    } else if (!mmdevice_name.empty()) {
      endpoint.pin_role = mmdevice_name;
    } else if (!physical.name.empty()) {
      endpoint.pin_role = physical.name;
    } else if (specific_pin_name(pin_name)) {
      endpoint.pin_role = pin_name;
    } else if (!category_label.empty()) {
      endpoint.pin_role = category_label;
    } else if (!pin_name.empty()) {
      endpoint.pin_role = pin_name;
    } else {
      endpoint.pin_role = direction == Direction::input ? L"Capture" : L"Render";
    }
    endpoint.name = filter_name + L" / " + endpoint.pin_role +
                    L" (KS Pin " + std::to_wstring(pin_id) + L")";
    const auto pin_identity = device_instance_id + L"|" + detail->DevicePath +
        L"|pin=" + std::to_wstring(pin_id) +
        (direction == Direction::input ? L"|in" : L"|out");
    const auto legacy_v2_id = stable_endpoint_id(pin_identity);
    endpoint.id = stable_endpoint_id(device_instance_id + L"|" +
        normalize_audio_endpoint_path(endpoint.ks_filter_path) + L"|pin=" + std::to_wstring(pin_id) +
        (direction == Direction::input ? L"|in" : L"|out"));
    const auto legacy_logical_id = device_instance_id +
        (direction == Direction::input ? L"|input|" : L"|output|") + endpoint.pin_role;
    const auto legacy_id = std::wstring(detail->DevicePath) + L"|pin=" + std::to_wstring(pin_id) +
                           (direction == Direction::input ? L"|in" : L"|out");
    if (!get_pin_interface_and_medium(filter.value, pin_id,
            endpoint.ks_interface_set, endpoint.ks_interface_id,
            endpoint.ks_medium_set, endpoint.ks_medium_id)) continue;
    if (!summarize_pin_formats(filter.value, pin_id, endpoint)) continue;

    KsPinCandidate pin_candidate;
    pin_candidate.filter_path = endpoint.ks_filter_path;
    pin_candidate.pin_id = endpoint.ks_pin_id;
    pin_candidate.interface_set = endpoint.ks_interface_set;
    pin_candidate.interface_id = endpoint.ks_interface_id;
    pin_candidate.medium_set = endpoint.ks_medium_set;
    pin_candidate.medium_id = endpoint.ks_medium_id;
    pin_candidate.channels = endpoint.channels;
    pin_candidate.sample_rates = endpoint.native_sample_rates;
    pin_candidate.formats = endpoint.native_formats;

    endpoint.ks_pin_candidates.push_back(std::move(pin_candidate));
    endpoint.legacy_ids.push_back(legacy_id);
    endpoint.legacy_ids.push_back(legacy_logical_id);
    endpoint.legacy_ids.push_back(legacy_v2_id);
    if (!previous_device_id.empty() && previous_device_id != device_instance_id) {
      endpoint.legacy_ids.push_back(stable_endpoint_id(previous_device_id + L"|" +
          normalize_audio_endpoint_path(endpoint.ks_filter_path) + L"|pin=" + std::to_wstring(pin_id) +
          (direction == Direction::input ? L"|in" : L"|out")));
      endpoint.legacy_ids.push_back(stable_endpoint_id(previous_device_id + L"|" + detail->DevicePath +
          L"|pin=" + std::to_wstring(pin_id) + (direction == Direction::input ? L"|in" : L"|out")));
    }
    if (!mmdevice_name.empty()) {
      endpoint.legacy_ids.push_back(stable_endpoint_id(device_instance_id +
          (direction == Direction::input ? L"|input|endpoint|" : L"|output|endpoint|") + mmdevice_name));
    }
    append_endpoint(endpoints, std::move(endpoint));
  }
}

} // namespace

std::vector<AudioEndpoint> enumerate_endpoints() {
  std::vector<AudioEndpoint> endpoints;
  const auto endpoint_names = enumerate_audio_endpoint_names();
  std::unordered_set<std::wstring> seen_filter_paths;
  const std::array<const GUID*, 3> categories{
      &KSCATEGORY_AUDIO, &KSCATEGORY_CAPTURE, &KSCATEGORY_RENDER};
  for (const auto* category : categories) {
    DeviceInfoSet devices{SetupDiGetClassDevsW(category, nullptr, nullptr,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT)};
    if (devices.value == INVALID_HANDLE_VALUE) continue;

    for (DWORD index = 0;; ++index) {
      SP_DEVICE_INTERFACE_DATA interface_data{};
      interface_data.cbSize = sizeof(interface_data);
      if (!SetupDiEnumDeviceInterfaces(devices.value, nullptr, category,
                                       index, &interface_data)) {
        if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
        continue;
      }
      append_filter_pins(devices.value, interface_data, endpoints, seen_filter_paths, endpoint_names);
    }
  }
  // Older versions used only a friendly name for some IDs. Such names can
  // identify multiple real factories. Assign those ambiguous old aliases to
  // their original first factory; keep all distinct physical IDs selectable.
  std::unordered_set<std::wstring> assigned_aliases;
  for (auto& endpoint : endpoints) {
    std::erase_if(endpoint.legacy_ids, [&](const auto& id) {
      return !assigned_aliases.insert(id).second;
    });
  }
  return endpoints;
}
