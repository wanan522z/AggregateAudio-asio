// Read-only KS metadata inspection. No streaming pins are created.
#include "../src/device_catalog.cpp"
#include <cstdio>

std::string utf8(const std::wstring& value) {
  const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
      nullptr, 0, nullptr, nullptr);
  std::string result(count, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count,
      nullptr, nullptr);
  return result;
}

int main() {
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
