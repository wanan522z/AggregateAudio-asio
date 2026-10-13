#pragma once

#include "device_catalog.h"

#include <algorithm>
#include <string>
#include <vector>

inline constexpr wchar_t kSettingsChangedEventName[] = L"Local\\AggregateAudio.SettingsChanged.v1";

struct DeviceBufferSetting {
  std::wstring device_id;
  unsigned buffer_frames{128};
};

struct AudioSettings {
  std::vector<std::wstring> input_device_ids;
  std::vector<std::wstring> output_device_ids;
  std::vector<std::wstring> input_channel_keys;
  std::vector<std::wstring> output_channel_keys;
  std::vector<DeviceBufferSetting> device_buffers;
  unsigned sample_rate{48000};
  unsigned buffer_frames{128};
  bool channel_selection_configured{};
  bool initialized{};
};

struct AutoAudioSettings {
  unsigned sample_rate{48000};
  unsigned buffer_frames{128};
  bool needs_sample_rate_conversion{};
  std::vector<unsigned> common_sample_rates;
};

AudioSettings load_audio_settings();
bool save_audio_settings(const AudioSettings& settings);
std::wstring channel_selection_key(const std::wstring& endpoint_id, unsigned channel);

inline bool endpoint_has_channel_selection(const AudioSettings& settings,
                                           const std::wstring& endpoint_id, Direction direction) {
  const auto& keys = direction == Direction::input
      ? settings.input_channel_keys : settings.output_channel_keys;
  const auto prefix = endpoint_id + L":ch=";
  return std::any_of(keys.begin(), keys.end(), [&](const auto& key) { return key.starts_with(prefix); });
}

// Replace only the choices represented by the current catalog. Disconnected
// endpoints and temporarily unavailable channels retain their previous state.
void update_available_device_settings(AudioSettings& settings,
                                      const std::vector<AudioEndpoint>& catalog,
                                      const AudioSettings& current);

// Migrate only the in-memory profile. Saving remains an explicit UI action.
inline void canonicalize_endpoint_selections(AudioSettings& settings,
                                             const std::vector<AudioEndpoint>& catalog) {
  const auto canonical_id = [&](const std::wstring& id, Direction direction) -> std::wstring {
    const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& endpoint) {
      return endpoint.direction == direction && (endpoint.id == id ||
          std::find(endpoint.legacy_ids.begin(), endpoint.legacy_ids.end(), id) != endpoint.legacy_ids.end());
    });
    return found == catalog.end() ? id : found->id;
  };
  const auto migrate = [&](std::vector<std::wstring>& values, Direction direction, bool channels) {
    std::vector<std::wstring> migrated;
    for (const auto& value : values) {
      const auto suffix = channels ? value.rfind(L":ch=") : std::wstring::npos;
      auto next = suffix == std::wstring::npos ? canonical_id(value, direction) :
          canonical_id(value.substr(0, suffix), direction) + value.substr(suffix);
      if (std::find(migrated.begin(), migrated.end(), next) == migrated.end()) migrated.push_back(std::move(next));
    }
    values = std::move(migrated);
  };
  migrate(settings.input_device_ids, Direction::input, false);
  migrate(settings.output_device_ids, Direction::output, false);
  migrate(settings.input_channel_keys, Direction::input, true);
  migrate(settings.output_channel_keys, Direction::output, true);
  const auto previous_buffers = settings.device_buffers;
  for (const auto& endpoint : catalog) {
    if (std::any_of(settings.device_buffers.begin(), settings.device_buffers.end(), [&](const auto& item) {
          return item.device_id == endpoint.device_instance_id;
        })) continue;
    const auto old = std::find_if(previous_buffers.begin(), previous_buffers.end(), [&](const auto& item) {
      return std::find(endpoint.legacy_device_ids.begin(), endpoint.legacy_device_ids.end(), item.device_id) !=
          endpoint.legacy_device_ids.end();
    });
    if (old != previous_buffers.end()) settings.device_buffers.push_back({endpoint.device_instance_id, old->buffer_frames});
  }
}
unsigned device_buffer_frames(const AudioSettings& settings, const std::wstring& device_id);
AutoAudioSettings recommend_audio_settings(
    const std::vector<AudioEndpoint>& endpoints,
    const std::vector<std::wstring>& input_ids,
    const std::vector<std::wstring>& output_ids);
