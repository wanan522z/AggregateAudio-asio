#include "../src/device_catalog.cpp"
#include "settings.h"
#include <cstdio>

namespace {
const std::wstring audio = L"\\\\?\\usb#device#{6994ad04-93ef-11d0-a3cc-00a0c9223196}\\";
const std::wstring capture = L"\\\\?\\usb#device#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\\";
const std::wstring render = L"\\\\?\\usb#device#{65e8773e-8f56-11d0-a3b9-00a0c9223196}\\";
AudioEndpoint port(std::wstring id, std::wstring path, unsigned pin, Direction direction,
                   std::wstring role = L"Capture") {
  AudioEndpoint endpoint;
  endpoint.id = std::move(id);
  endpoint.device_instance_id = L"device-A";
  endpoint.device_name = L"test";
  endpoint.direction = direction;
  endpoint.pin_role = std::move(role);
  endpoint.ks_filter_path = std::move(path);
  endpoint.ks_pin_id = pin;
  endpoint.channels = 2;
  endpoint.native_sample_rates = {48000};
  KsPinCandidate candidate;
  candidate.filter_path = endpoint.ks_filter_path;
  candidate.pin_id = pin;
  candidate.channels = 2;
  candidate.sample_rates = {48000};
  endpoint.ks_pin_candidates.push_back(std::move(candidate));
  endpoint.legacy_ids.push_back(endpoint.id + L":old");
  return endpoint;
}
bool contains_alias(const AudioEndpoint& endpoint, const std::wstring& id) {
  return contains_case_insensitive(endpoint.legacy_ids, id);
}
}

int main() {
  std::vector<AudioEndpointName> names{{Direction::input, normalize_audio_endpoint_path(audio + L"input1"), L"Analog 1/2"}};
  bool passed = endpoint_name_for_path(capture + L"input1", Direction::input, names) == L"Analog 1/2" &&
      endpoint_name_for_path(capture + L"input12", Direction::input, names).empty();
  std::vector<AudioEndpoint> ports;
  append_endpoint(ports, port(L"named", audio + L"input1", 0, Direction::input));
  append_endpoint(ports, port(L"capture-alias", capture + L"input1", 0, Direction::input));
  passed = ports.size() == 1 && ports[0].ks_pin_candidates.size() == 1 &&
      contains_alias(ports[0], L"capture-alias") && contains_alias(ports[0], L"capture-alias:old") && passed;
  // Same names must not hide another channel pair, pin, direction, or device.
  append_endpoint(ports, port(L"pair2", capture + L"input2", 0, Direction::input));
  append_endpoint(ports, port(L"pin2", capture + L"input1", 2, Direction::input));
  append_endpoint(ports, port(L"output", render + L"input1", 0, Direction::output));
  auto second_device = port(L"device2", capture + L"input1", 0, Direction::input);
  second_device.device_instance_id = L"device-B";
  append_endpoint(ports, std::move(second_device));
  passed = ports.size() == 5 && passed;
  std::printf("Interface aliases, old IDs and distinct physical ports: %s\n", passed ? "PASS" : "FAIL");

  ports.clear();
  append_endpoint(ports, port(L"speaker", audio + L"speaker", 0, Direction::output, L"Speakers"));
  append_endpoint(ports, port(L"speaker-alias", render + L"speaker", 0, Direction::output, L"Speakers"));
  append_endpoint(ports, port(L"speaker3", audio + L"speaker3", 0, Direction::output, L"Speakers"));
  append_endpoint(ports, port(L"offload", audio + L"speaker3", 2, Direction::output, L"Speakers"));
  append_endpoint(ports, port(L"monitor1", audio + L"speaker", 3, Direction::input));
  append_endpoint(ports, port(L"monitor2", capture + L"speaker3", 3, Direction::input));
  append_endpoint(ports, port(L"mic", audio + L"microphone", 0, Direction::input, L"Microphone"));
  bool distinct = ports.size() == 6 && ports[0].ks_pin_candidates.size() == 1 &&
      ports[1].id == L"speaker3" && ports[2].id == L"offload" &&
      ports[3].id == L"monitor1" && ports[4].id == L"monitor2" && ports[5].id == L"mic";
  std::printf("Equally named filters, render factories and capture factories stay selectable: %s\n", distinct ? "PASS" : "FAIL");
  AudioSettings settings;
  settings.output_device_ids = {L"speaker-alias", L"speaker", L"offload"};
  settings.output_channel_keys = {L"speaker-alias:ch=1", L"speaker:ch=1", L"offload:ch=0"};
  canonicalize_endpoint_selections(settings, ports);
  bool migration = settings.output_device_ids == std::vector<std::wstring>{L"speaker", L"offload"} &&
      settings.output_channel_keys == std::vector<std::wstring>{L"speaker:ch=1", L"offload:ch=0"};
  std::printf("Legacy channel selections migrate without selecting other factories: %s\n", migration ? "PASS" : "FAIL");
  const std::vector<KSTOPOLOGY_CONNECTION> graph{{1, 0, 0, 1}, {KSFILTER_NODE, 0, 1, 1},
      {KSFILTER_NODE, 2, 1, 2}, {1, 3, KSFILTER_NODE, 3}, {0, 0, KSFILTER_NODE, 1}};
  const auto connected = connected_filter_pins(graph, 0);
  const bool topology = connected.size() == 4 && std::find(connected.begin(), connected.end(), 1ul) != connected.end() &&
      normalize_audio_endpoint_path(L"\\??\\usb#device#{6994ad04-93ef-11d0-a3cc-00a0c9223196}\\speaker") ==
          normalize_audio_endpoint_path(audio + L"speaker");
  std::printf("Audio engine topology reaches connector and normalizes kernel link prefixes: %s\n", topology ? "PASS" : "FAIL");
  ports.clear();
  auto normal = port(L"normal", audio + L"speaker", 0, Direction::output, L"Speakers");
  normal.physical_port_id = L"topology-speaker|bridge=0";
  normal.legacy_device_ids = {L"shared-system-container"};
  auto extra = port(L"extra", audio + L"speaker", 2, Direction::output, L"Speakers");
  extra.physical_port_id = normal.physical_port_id;
  append_endpoint(ports, extra);
  append_endpoint(ports, normal);
  auto headphones = port(L"headphones", audio + L"speaker3", 0, Direction::output, L"Headphones");
  headphones.physical_port_id = L"topology-headphones|bridge=0";
  append_endpoint(ports, headphones);
  auto monitor = port(L"monitor", audio + L"speaker", 3, Direction::input, L"Speaker loopback");
  monitor.physical_port_id = normal.physical_port_id;
  append_endpoint(ports, monitor);
  bool physical = ports.size() == 3 && ports[0].ks_pin_candidates.size() == 2 && ports[0].ks_pin_id == 0 &&
      contains_alias(ports[0], L"normal") && ports[1].id == L"headphones" && ports[2].id == L"monitor";
  // Channel layouts, uncertain topology and unrelated devices are retained.
  auto wider = normal;
  wider.id = L"multichannel";
  wider.channels = 8;
  wider.ks_pin_id = 4;
  wider.ks_pin_candidates.front().pin_id = 4;
  wider.ks_pin_candidates.front().channels = 8;
  append_endpoint(ports, wider);
  auto other = normal;
  other.id = L"second-adapter";
  other.device_instance_id = L"device-B";
  append_endpoint(ports, other);
  physical = physical && ports.size() == 5;
  std::printf("Only proven connector aliases merge; headphones, loopback and other adapters remain: %s\n",
      physical ? "PASS" : "FAIL");
  settings.device_buffers = {{L"shared-system-container", 512}};
  ports[0].legacy_device_ids = normal.legacy_device_ids;
  canonicalize_endpoint_selections(settings, ports);
  const bool buffer_migration = std::any_of(settings.device_buffers.begin(), settings.device_buffers.end(),
      [](const auto& item) { return item.device_id == L"device-A" && item.buffer_frames == 512; });
  std::printf("Shared-container device buffers migrate to the actual adapter: %s\n", buffer_migration ? "PASS" : "FAIL");
  return passed && distinct && migration && topology && physical && buffer_migration ? 0 : 1;
}
