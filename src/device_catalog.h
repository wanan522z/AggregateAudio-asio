#pragma once

#include <guiddef.h>
#include <string>
#include <vector>

enum class Direction { input, output };

struct KsPinCandidate {
  std::wstring filter_path;
  unsigned pin_id{};
  GUID interface_set{};
  unsigned long interface_id{};
  GUID medium_set{};
  unsigned long medium_id{};
  unsigned channels{};
  std::vector<unsigned> sample_rates;
  std::vector<std::string> formats;
};

struct AudioEndpoint {
  std::wstring id;
  std::wstring device_name;
  std::wstring name;
  std::wstring device_instance_id;
  std::vector<std::wstring> legacy_device_ids;
  std::wstring physical_port_id;
  std::wstring pin_role;
  GUID pin_category{};
  bool has_pin_category{};
  std::vector<std::wstring> legacy_ids;
  std::vector<KsPinCandidate> ks_pin_candidates;
  std::wstring ks_filter_path;
  Direction direction{};
  unsigned ks_pin_id{};
  GUID ks_interface_set{};
  unsigned long ks_interface_id{};
  GUID ks_medium_set{};
  unsigned long ks_medium_id{};
  unsigned channels{};
  unsigned sample_rate{};
  unsigned bits_per_sample{};
  unsigned long long default_period_100ns{};
  unsigned long long minimum_period_100ns{};
  unsigned minimum_sample_rate{};
  unsigned maximum_sample_rate{};
  std::vector<unsigned> native_sample_rates;
  std::vector<std::string> native_formats;
  bool is_default{};
};

// Merges interface aliases and stream factories proven to share one physical
// connector. Distinct connectors and unknown topology remain selectable.
std::vector<AudioEndpoint> enumerate_endpoints();
