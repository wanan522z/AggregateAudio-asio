#include "settings.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {

std::filesystem::path settings_path() {
  PWSTR local_app_data = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local_app_data))) {
    return {};
  }
  std::filesystem::path directory(local_app_data);
  CoTaskMemFree(local_app_data);
  directory /= L"AggregateASIO";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) return {};
  return directory / L"settings.ini";
}

std::wstring read_value(const std::filesystem::path& path, const wchar_t* section,
                        const wchar_t* key, const wchar_t* fallback = L"") {
  std::array<wchar_t, 8192> buffer{};
  GetPrivateProfileStringW(section, key, fallback, buffer.data(),
                           static_cast<DWORD>(buffer.size()), path.c_str());
  return buffer.data();
}

std::vector<std::wstring> split_ids(const std::wstring& text) {
  std::vector<std::wstring> ids;
  size_t begin = 0;
  while (begin < text.size()) {
    const size_t end = text.find(L';', begin);
    const auto id = text.substr(begin, end == std::wstring::npos ? end : end - begin);
    if (!id.empty()) ids.push_back(id);
    if (end == std::wstring::npos) break;
    begin = end + 1;
  }
  return ids;
}

std::wstring join_ids(const std::vector<std::wstring>& ids) {
  std::wstring result;
  for (const auto& id : ids) {
    if (!result.empty()) result += L';';
    result += id;
  }
  return result;
}

std::wstring join_device_buffers(const std::vector<DeviceBufferSetting>& buffers) {
  std::wstring result;
  for (const auto& item : buffers) {
    if (item.device_id.empty()) continue;
    if (!result.empty()) result += L';';
    result += item.device_id + L'=' + std::to_wstring(item.buffer_frames);
  }
  return result;
}

std::vector<DeviceBufferSetting> split_device_buffers(const std::wstring& text) {
  std::vector<DeviceBufferSetting> result;
  constexpr std::array<unsigned, 6> choices{64, 128, 256, 512, 1024, 2048};
  size_t begin = 0;
  while (begin < text.size()) {
    const size_t end = text.find(L';', begin);
    const auto item = text.substr(begin, end == std::wstring::npos ? end : end - begin);
    const size_t separator = item.rfind(L'=');
    if (separator != std::wstring::npos && separator > 0 && separator + 1 < item.size()) {
      const auto value = item.substr(separator + 1);
      wchar_t* parsed_end = nullptr;
      const auto frames = std::wcstoul(value.c_str(), &parsed_end, 10);
      if (parsed_end && *parsed_end == L'\0' &&
          std::find(choices.begin(), choices.end(), static_cast<unsigned>(frames)) != choices.end()) {
        result.push_back({item.substr(0, separator), static_cast<unsigned>(frames)});
      }
    }
    if (end == std::wstring::npos) break;
    begin = end + 1;
  }
  return result;
}

std::vector<unsigned> supported_rates(const AudioEndpoint& endpoint) {
  return endpoint.native_sample_rates;
}

bool ensure_unicode_profile(const std::filesystem::path& path) {
  WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
  std::ifstream input(path, std::ios::binary);
  std::error_code file_error;
  const bool existing = std::filesystem::exists(path, file_error);
  if (file_error || (existing && (!input || std::filesystem::file_size(path, file_error) > 1024 * 1024)) || file_error)
    return false;
  const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff &&
      static_cast<unsigned char>(bytes[1]) == 0xfe) return true;
  input.close();
  const bool utf8 = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef &&
      static_cast<unsigned char>(bytes[1]) == 0xbb && static_cast<unsigned char>(bytes[2]) == 0xbf;
  const auto offset = utf8 ? 3u : 0u;
  if (bytes.size() > 1024 * 1024) return false;
  const auto length = static_cast<int>(bytes.size() - offset);
  const int count = length ? MultiByteToWideChar(utf8 ? CP_UTF8 : CP_ACP, 0, bytes.data() + offset,
      length, nullptr, 0) : 0;
  if (length && !count) return false;
  std::wstring text(static_cast<size_t>(count), L'\0');
  if (length) MultiByteToWideChar(utf8 ? CP_UTF8 : CP_ACP, 0, bytes.data() + offset, length, text.data(), count);
  auto temporary = path;
  temporary += L".unicode-" + std::to_wstring(GetCurrentProcessId());
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    const unsigned char bom[]{0xff, 0xfe};
    output.write(reinterpret_cast<const char*>(bom), sizeof(bom));
    output.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size() * sizeof(wchar_t)));
    output.close();
    if (!output) return false;
  }
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    return false;
  }
  WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
  return true;
}

} // namespace

namespace {
AudioSettings load_profile(const std::filesystem::path& path) {
  AudioSettings settings;
  if (path.empty()) return settings;

  settings.input_device_ids = split_ids(read_value(path, L"Devices", L"InputIds"));
  settings.output_device_ids = split_ids(read_value(path, L"Devices", L"OutputIds"));
  settings.input_channel_keys = split_ids(read_value(path, L"Channels", L"InputKeys"));
  settings.output_channel_keys = split_ids(read_value(path, L"Channels", L"OutputKeys"));
  settings.device_buffers = split_device_buffers(read_value(path, L"Devices", L"DeviceBuffers"));
  settings.sample_rate = GetPrivateProfileIntW(L"Engine", L"SampleRate", 48000, path.c_str());
  settings.buffer_frames = GetPrivateProfileIntW(L"Engine", L"BufferFrames", 128, path.c_str());
  settings.channel_selection_configured =
      GetPrivateProfileIntW(L"Channels", L"Configured", 0, path.c_str()) != 0;
  settings.initialized = GetPrivateProfileIntW(L"Engine", L"Configured", 0, path.c_str()) != 0;
  // Ignore channels from the removed ASIO-only cable feature. Keep hardware
  // selections intact; persistent cleanup happens only on an explicit save.
  const auto removed = [](const auto& value) { return value.starts_with(L"virtual:"); };
  std::erase_if(settings.input_device_ids, removed);
  std::erase_if(settings.output_device_ids, removed);
  std::erase_if(settings.input_channel_keys, removed);
  std::erase_if(settings.output_channel_keys, removed);
  std::erase_if(settings.device_buffers, [&](const auto& item) { return removed(item.device_id); });
  if (!settings.sample_rate) settings.sample_rate = 48000;
  if (!settings.buffer_frames) settings.buffer_frames = 128;
  return settings;
}

bool save_profile(const std::filesystem::path& path, const AudioSettings& settings) {
  if (path.empty()) return false;
  if (!ensure_unicode_profile(path)) return false;
  const unsigned obsolete_count = std::min(GetPrivateProfileIntW(L"VirtualCables", L"Count", 0, path.c_str()), 16u);
  for (unsigned index = 0; index < obsolete_count; ++index) {
    const auto section = L"VirtualCable" + std::to_wstring(index);
    if (!WritePrivateProfileStringW(section.c_str(), nullptr, nullptr, path.c_str())) return false;
  }
  if (!WritePrivateProfileStringW(L"VirtualCables", nullptr, nullptr, path.c_str())) return false;
  const auto input_ids = join_ids(settings.input_device_ids);
  const auto output_ids = join_ids(settings.output_device_ids);
  const auto input_channel_keys = join_ids(settings.input_channel_keys);
  const auto output_channel_keys = join_ids(settings.output_channel_keys);
  const auto device_buffers = join_device_buffers(settings.device_buffers);
  const auto sample_rate = std::to_wstring(settings.sample_rate);
  const auto buffer_frames = std::to_wstring(settings.buffer_frames);
  const auto configured = std::to_wstring(settings.initialized ? 1 : 0);
  const auto channel_configured = std::to_wstring(settings.channel_selection_configured ? 1 : 0);
  return WritePrivateProfileStringW(L"Devices", L"InputIds", input_ids.c_str(), path.c_str()) &&
         WritePrivateProfileStringW(L"Devices", L"OutputIds", output_ids.c_str(), path.c_str()) &&
         WritePrivateProfileStringW(L"Devices", L"DeviceBuffers", device_buffers.c_str(), path.c_str()) &&
         WritePrivateProfileStringW(L"Channels", L"InputKeys", input_channel_keys.c_str(), path.c_str()) &&
         WritePrivateProfileStringW(L"Channels", L"OutputKeys", output_channel_keys.c_str(), path.c_str()) &&
         WritePrivateProfileStringW(L"Channels", L"Configured", channel_configured.c_str(), path.c_str()) &&
         WritePrivateProfileStringW(L"Engine", L"SampleRate", sample_rate.c_str(), path.c_str()) &&
         WritePrivateProfileStringW(L"Engine", L"BufferFrames", buffer_frames.c_str(), path.c_str()) &&
         WritePrivateProfileStringW(L"Engine", L"Configured", configured.c_str(), path.c_str());
}
} // namespace

AudioSettings load_audio_settings() { return load_profile(settings_path()); }
bool save_audio_settings(const AudioSettings& settings) { return save_profile(settings_path(), settings); }

std::wstring channel_selection_key(const std::wstring& endpoint_id, unsigned channel) {
  return endpoint_id + L":ch=" + std::to_wstring(channel);
}

unsigned device_buffer_frames(const AudioSettings& settings, const std::wstring& device_id) {
  const auto found = std::find_if(settings.device_buffers.begin(), settings.device_buffers.end(),
      [&](const auto& item) { return item.device_id == device_id; });
  if (found == settings.device_buffers.end()) return settings.buffer_frames;
  constexpr std::array<unsigned, 6> choices{64, 128, 256, 512, 1024, 2048};
  return std::find(choices.begin(), choices.end(), found->buffer_frames) != choices.end()
      ? found->buffer_frames : settings.buffer_frames;
}

AutoAudioSettings recommend_audio_settings(
    const std::vector<AudioEndpoint>& endpoints,
    const std::vector<std::wstring>& input_ids,
    const std::vector<std::wstring>& output_ids) {
  AutoAudioSettings recommendation;
  std::vector<unsigned> common_rates;
  bool has_selected_endpoint = false;
  bool has_unknown_native_rates = false;
  bool common_initialized = false;
  unsigned long long slowest_minimum_period = 0;

  auto include_selected = [&](const std::vector<std::wstring>& ids, Direction direction) {
    for (const auto& id : ids) {
      const auto found = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& endpoint) {
        return endpoint.id == id && endpoint.direction == direction;
      });
      if (found == endpoints.end()) continue;
      has_selected_endpoint = true;
      auto rates = supported_rates(*found);
      if (rates.empty()) {
        has_unknown_native_rates = true;
        slowest_minimum_period = std::max(slowest_minimum_period, found->minimum_period_100ns);
        continue;
      }
      if (!common_initialized) {
        common_rates = std::move(rates);
        common_initialized = true;
      } else {
        std::vector<unsigned> intersection;
        std::ranges::set_intersection(common_rates, rates, std::back_inserter(intersection));
        common_rates = std::move(intersection);
      }
      slowest_minimum_period = std::max(slowest_minimum_period, found->minimum_period_100ns);
    }
  };

  include_selected(input_ids, Direction::input);
  include_selected(output_ids, Direction::output);
  std::ranges::sort(common_rates);
  common_rates.erase(std::unique(common_rates.begin(), common_rates.end()), common_rates.end());
  recommendation.common_sample_rates = common_rates;

  if (std::find(common_rates.begin(), common_rates.end(), 48000u) != common_rates.end()) {
    recommendation.sample_rate = 48000;
  } else if (!common_rates.empty()) {
    recommendation.sample_rate = *std::min_element(common_rates.begin(), common_rates.end(),
        [](unsigned lhs, unsigned rhs) {
          const auto lhs_delta = lhs > 48000 ? lhs - 48000 : 48000 - lhs;
          const auto rhs_delta = rhs > 48000 ? rhs - 48000 : 48000 - rhs;
          return lhs_delta < rhs_delta;
        });
  } else if (has_selected_endpoint) {
    // Device clocks do not have a common native format. Keep a stable engine
    // rate and let the per-device bridge resample each endpoint.
    recommendation.sample_rate = 48000;
    recommendation.needs_sample_rate_conversion = true;
  }
  if (has_unknown_native_rates) recommendation.needs_sample_rate_conversion = true;

  if (slowest_minimum_period) {
    const double minimum_frames = static_cast<double>(slowest_minimum_period) *
                                  recommendation.sample_rate / 10'000'000.0;
    constexpr std::array<unsigned, 6> buffer_sizes{64, 128, 256, 512, 1024, 2048};
    const auto buffer = std::find_if(buffer_sizes.begin(), buffer_sizes.end(),
                                     [&](unsigned size) { return size >= minimum_frames; });
    recommendation.buffer_frames = buffer == buffer_sizes.end() ? buffer_sizes.back() : *buffer;
  }
  return recommendation;
}
