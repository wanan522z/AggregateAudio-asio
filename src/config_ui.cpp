#include "config_ui.h"
#include "config_ui_theme.h"
#include "config_instance.h"
#include "config_tray.h"

#include "device_catalog.h"
#include "ks_probe.h"
#include "settings.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windows.h>
#include "ks_device_access.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cwchar>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr wchar_t kWindowClass[] = L"AggregateAudioConfigWindow";
constexpr int kDeviceList = 1001;
constexpr int kSampleRate = 1003;
constexpr int kAsioBuffer = 1004;
constexpr int kRefresh = 1005;
constexpr int kAutoDetect = 1006;
constexpr int kSave = 1007;
constexpr int kClose = 1008;
constexpr int kRecommendation = 1009;
constexpr int kStatusLog = 1010;
constexpr int kDeviceBuffer = 1011;
constexpr int kActiveDevice = 1012;
constexpr int kSettings = 1013;
constexpr int kRepository = 1014;
constexpr UINT_PTR kProbeTimer = 1;
constexpr UINT_PTR kTrayTimer = 2;
const UINT kTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

struct ChannelRef {
  size_t endpoint_index{};
  unsigned endpoint_channel{};
  size_t device_group_index{};
};

struct DeviceGroup {
  std::wstring id;
  std::wstring name;
  std::vector<size_t> channel_indices;
  unsigned buffer_frames{128};
  int list_group_id{};
};

struct ProbeDevice {
  std::wstring id;
  std::wstring name;
  std::vector<AudioEndpoint> endpoints;
};

struct ProbeJob {
  std::mutex mutex;
  std::wstring latest_message;
  std::vector<DeviceBufferSetting> result_device_buffers;
  std::wstring access_error;
  std::atomic_bool cancelled{};
  std::atomic_bool done{};
  std::atomic_bool passed{};
};

struct AppState {
  HWND window{};
  HWND device_list{};
  HWND sample_rate{};
  HWND asio_buffer{};
  HWND device_buffer{};
  HWND active_device{};
  HWND recommendation{};
  HWND status_log{};
  HWND sample_rate_label{};
  HWND asio_buffer_label{};
  HWND device_buffer_label{};
  HWND active_device_label{};
  HWND refresh{};
  HWND auto_detect{};
  HWND save{};
  HWND close{};
  HWND settings_button{};
  HWND repository_link{};
  HWND attribution{};
  config_theme::Theme theme;
  RECT device_card{};
  RECT settings_card{};
  RECT log_card{};
  RECT footer{};
  std::vector<AudioEndpoint> endpoints;
  std::vector<DeviceGroup> devices;
  std::vector<ChannelRef> channels;
  AudioSettings saved_settings;
  AutoAudioSettings recommendation_data;
  std::shared_ptr<ProbeJob> probe_job;
  std::wstring last_probe_message;
  size_t active_device_index{static_cast<size_t>(-1)};
  bool populating{};
  bool showing_settings{};
  bool tray_enabled{true};
  bool tray_background{};
  bool asio_active{};
  ULONGLONG last_active_at{};
  HICON app_icon{};
  config_tray::Icon tray_icon;
};

void layout_controls(AppState& app, int width, int height);

RECT fit_window_to_work_area(RECT rect, const RECT& work, int inset) {
  const int width = std::min(static_cast<int>(rect.right - rect.left),
      std::max(1, static_cast<int>(work.right - work.left) - inset * 2));
  const int height = std::min(static_cast<int>(rect.bottom - rect.top),
      std::max(1, static_cast<int>(work.bottom - work.top) - inset * 2));
  const int left = std::clamp(static_cast<int>(rect.left), static_cast<int>(work.left) + inset,
      std::max(static_cast<int>(work.left) + inset, static_cast<int>(work.right) - inset - width));
  const int top = std::clamp(static_cast<int>(rect.top), static_cast<int>(work.top) + inset,
      std::max(static_cast<int>(work.top) + inset, static_cast<int>(work.bottom) - inset - height));
  return {left, top, left + width, top + height};
}

RECT monitor_work_area(HMONITOR monitor) {
  MONITORINFO info{sizeof(info)};
  GetMonitorInfoW(monitor, &info);
  return info.rcWork;
}

void show_settings(AppState& app, bool show) {
  app.showing_settings = show;
  for (const auto control : {app.sample_rate, app.asio_buffer, app.device_buffer,
       app.sample_rate_label, app.asio_buffer_label, app.device_buffer_label,
       app.active_device_label, app.active_device, app.refresh, app.auto_detect,
       app.recommendation}) ShowWindow(control, show ? SW_HIDE : SW_SHOW);
  ShowWindow(app.device_list, !show && !app.channels.empty() ? SW_SHOW : SW_HIDE);
  ShowWindow(app.status_log, show ? SW_SHOW : SW_HIDE);
  ShowWindow(app.repository_link, show ? SW_SHOW : SW_HIDE);
  ShowWindow(app.attribution, show ? SW_SHOW : SW_HIDE);
  SetWindowTextW(app.settings_button, show ? L"返回设备" : L"Settings");
  RECT client{};
  GetClientRect(app.window, &client);
  layout_controls(app, client.right, client.bottom);
  InvalidateRect(app.window, nullptr, TRUE);
}

bool same_text(const std::wstring& left, const std::wstring& right) {
  return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                              right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

int compare_channel_names(const std::wstring& left, const std::wstring& right) {
  const int result = CompareStringEx(LOCALE_NAME_INVARIANT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
      left.c_str(), static_cast<int>(left.size()), right.c_str(), static_cast<int>(right.size()),
      nullptr, nullptr, 0);
  return result ? result - CSTR_EQUAL : left.compare(right);
}

std::wstring format_rates(const std::vector<unsigned>& rates) {
  std::wstring text;
  for (const auto rate : rates) {
    if (!text.empty()) text += L", ";
    text += std::to_wstring(rate / 1000);
    if ((rate % 1000) >= 100) {
      text += L'.';
      text += std::to_wstring((rate % 1000) / 100);
    }
    text += L"k";
  }
  return text.empty() ? L"未知" : text;
}

std::wstring format_formats(const std::vector<std::string>& formats) {
  std::wstring text;
  for (const auto& format : formats) {
    if (!text.empty()) text += L", ";
    text.append(format.begin(), format.end());
  }
  return text.empty() ? L"未知" : text;
}

std::wstring channel_name(const AudioEndpoint& endpoint, unsigned channel) {
  std::wstring name = endpoint.direction == Direction::input ? L"输入 " : L"输出 ";
  name += std::to_wstring(channel + 1);
  if (!endpoint.pin_role.empty()) name += L" · " + endpoint.pin_role;
  name += L" (KS Pin " + std::to_wstring(endpoint.ks_pin_id) + L")";
  return name;
}

void append_log(AppState& app, const std::wstring& message) {
  const auto length = GetWindowTextLengthW(app.status_log);
  SendMessageW(app.status_log, EM_SETSEL, length, length);
  SendMessageW(app.status_log, EM_REPLACESEL, FALSE,
               reinterpret_cast<LPARAM>((message + L"\r\n").c_str()));
}

void update_tray(AppState& app) {
  if (!app.tray_enabled) return;
  const auto hosts = config_tray::running_hosts();
  app.asio_active = !hosts.empty();
  if (app.asio_active) app.last_active_at = GetTickCount64();
  app.tray_icon.update(app.window, app.app_icon, hosts);
  if (app.tray_background && !app.asio_active && GetTickCount64() - app.last_active_at >= 5000)
    DestroyWindow(app.window);
}

void close_or_hide(AppState& app) {
  if (app.tray_enabled && app.asio_active) {
    app.tray_background = true;
    ShowWindow(app.window, SW_HIDE);
  } else DestroyWindow(app.window);
}

unsigned combo_value(HWND combo, unsigned fallback) {
  const auto index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
  if (index == CB_ERR) return fallback;
  const auto value = SendMessageW(combo, CB_GETITEMDATA, index, 0);
  return value == CB_ERR ? fallback : static_cast<unsigned>(value);
}

void select_combo_value(HWND combo, unsigned value) {
  const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
  for (int i = 0; i < count; ++i) {
    if (static_cast<unsigned>(SendMessageW(combo, CB_GETITEMDATA, i, 0)) == value) {
      SendMessageW(combo, CB_SETCURSEL, i, 0);
      return;
    }
  }
  if (count > 0) SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

unsigned selected_sample_rate(const AppState& app) {
  return combo_value(app.sample_rate, app.recommendation_data.sample_rate);
}

std::vector<size_t> checked_channel_indices(const AppState& app) {
  std::vector<size_t> selected;
  if (!app.device_list) return selected;
  const int rows = ListView_GetItemCount(app.device_list);
  for (int row = 0; row < rows; ++row) {
    if (!ListView_GetCheckState(app.device_list, row)) continue;
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (!ListView_GetItem(app.device_list, &item)) continue;
    const auto index = static_cast<size_t>(item.lParam);
    if (index < app.channels.size()) selected.push_back(index);
  }
  return selected;
}

std::vector<std::wstring> selected_endpoint_ids(const AppState& app, Direction direction) {
  std::vector<std::wstring> ids;
  for (const auto channel_index : checked_channel_indices(app)) {
    const auto& channel = app.channels[channel_index];
    const auto& endpoint = app.endpoints[channel.endpoint_index];
    if (endpoint.direction != direction) continue;
    if (std::find(ids.begin(), ids.end(), endpoint.id) == ids.end()) ids.push_back(endpoint.id);
  }
  return ids;
}

std::vector<std::wstring> selected_channel_keys(const AppState& app, Direction direction) {
  std::vector<std::wstring> keys;
  for (const auto channel_index : checked_channel_indices(app)) {
    const auto& channel = app.channels[channel_index];
    const auto& endpoint = app.endpoints[channel.endpoint_index];
    if (endpoint.direction != direction) continue;
    keys.push_back(channel_selection_key(endpoint.id, channel.endpoint_channel));
  }
  return keys;
}

std::vector<const AudioEndpoint*> selected_endpoints(const AppState& app) {
  std::vector<const AudioEndpoint*> result;
  for (const auto channel_index : checked_channel_indices(app)) {
    const auto& endpoint = app.endpoints[app.channels[channel_index].endpoint_index];
    if (std::none_of(result.begin(), result.end(), [&](const auto* item) {
          return item->id == endpoint.id;
        })) result.push_back(&endpoint);
  }
  return result;
}

std::vector<DeviceBufferSetting> current_device_buffers(const AppState& app) {
  std::vector<DeviceBufferSetting> result;
  result.reserve(app.devices.size());
  for (const auto& device : app.devices) result.push_back({device.id, device.buffer_frames});
  return result;
}

unsigned current_device_buffer(const AppState& app) {
  if (app.active_device_index < app.devices.size()) {
    return app.devices[app.active_device_index].buffer_frames;
  }
  return combo_value(app.device_buffer, app.saved_settings.buffer_frames);
}

void update_device_group_header(AppState& app, size_t device_index) {
  if (device_index >= app.devices.size() || !app.device_list) return;
  auto& device = app.devices[device_index];
  size_t selected = 0;
  for (const auto channel_index : device.channel_indices) {
    if (ListView_GetCheckState(app.device_list, static_cast<int>(channel_index))) ++selected;
  }
  const auto header = device.name + L"   ·   " + std::to_wstring(selected) + L" / " +
      std::to_wstring(device.channel_indices.size()) + L" 声道已启用" +
      L"   ·   KS " + std::to_wstring(device.buffer_frames) + L" 帧";
  LVGROUP group{};
  group.cbSize = sizeof(group);
  group.mask = LVGF_HEADER;
  group.pszHeader = const_cast<wchar_t*>(header.c_str());
  group.cchHeader = static_cast<int>(header.size());
  ListView_SetGroupInfo(app.device_list, device.list_group_id, &group);
}

void update_all_device_group_headers(AppState& app) {
  for (size_t index = 0; index < app.devices.size(); ++index) {
    update_device_group_header(app, index);
  }
  InvalidateRect(app.window, &app.device_card, FALSE);
}

size_t device_group_for_row(const AppState& app, int row) {
  if (row < 0 || row >= static_cast<int>(app.channels.size())) return static_cast<size_t>(-1);
  return app.channels[static_cast<size_t>(row)].device_group_index;
}

void update_current_device(AppState& app) {
  int row = ListView_GetNextItem(app.device_list, -1, LVNI_SELECTED);
  size_t index = device_group_for_row(app, row);
  if (index >= app.devices.size()) {
    index = app.devices.empty() ? static_cast<size_t>(-1) : 0;
  }
  app.active_device_index = index;
  if (index < app.devices.size()) {
    const auto& device = app.devices[index];
    SetWindowTextW(app.active_device, device.name.c_str());
    select_combo_value(app.device_buffer, device.buffer_frames);
    const bool probing = app.probe_job && !app.probe_job->done.load();
    EnableWindow(app.device_buffer, !probing);
  } else {
    SetWindowTextW(app.active_device, L"请选择设备声道");
  }
}

void update_latency_text(AppState& app) {
  const unsigned rate = selected_sample_rate(app);
  const unsigned asio_frames = combo_value(app.asio_buffer, 128);
  const unsigned device_frames = current_device_buffer(app);
  const double asio_ms = rate ? 1000.0 * asio_frames / rate : 0.0;
  const double device_ms = rate ? 1000.0 * device_frames / rate : 0.0;
  std::array<wchar_t, 240> message{};
  std::swprintf(message.data(), message.size(),
      L"缓冲时长   ASIO %.2f ms / 块     ·     当前设备 KS %.2f ms / 块\n"
      L"以上为块时长估算；实际延迟还包含设备驱动与 DAW 缓冲。", asio_ms, device_ms);
  SetWindowTextW(app.recommendation, message.data());
}

void update_recommendation(AppState& app) {
  if (app.populating) return;
  const auto inputs = selected_endpoint_ids(app, Direction::input);
  const auto outputs = selected_endpoint_ids(app, Direction::output);
  app.recommendation_data = recommend_audio_settings(app.endpoints, inputs, outputs);

  const unsigned previous_rate = combo_value(app.sample_rate, app.saved_settings.sample_rate);
  std::vector<unsigned> rates = app.recommendation_data.common_sample_rates;
  if (rates.empty()) rates = {44100, 48000, 88200, 96000, 192000};
  if (std::find(rates.begin(), rates.end(), app.recommendation_data.sample_rate) == rates.end()) {
    rates.push_back(app.recommendation_data.sample_rate);
  }
  if (std::find(rates.begin(), rates.end(), previous_rate) == rates.end()) rates.push_back(previous_rate);
  std::ranges::sort(rates);
  rates.erase(std::unique(rates.begin(), rates.end()), rates.end());

  app.populating = true;
  SendMessageW(app.sample_rate, CB_RESETCONTENT, 0, 0);
  for (const auto rate : rates) {
    const auto label = std::to_wstring(rate) + L" Hz";
    const auto index = static_cast<int>(SendMessageW(app.sample_rate, CB_ADDSTRING, 0,
                                                      reinterpret_cast<LPARAM>(label.c_str())));
    if (index != CB_ERR && index != CB_ERRSPACE) {
      SendMessageW(app.sample_rate, CB_SETITEMDATA, index, rate);
    }
  }
  const unsigned chosen_rate = std::find(rates.begin(), rates.end(), previous_rate) != rates.end()
                                  ? previous_rate : app.recommendation_data.sample_rate;
  select_combo_value(app.sample_rate, chosen_rate);
  app.populating = false;

  update_latency_text(app);
  if (inputs.empty() && outputs.empty()) {
    SetWindowTextW(app.recommendation,
        L"勾选需要加入 ASIO 的输入或输出声道。\n设备 KS 缓冲可分别设置；ASIO 主机缓冲对所有设备生效。");
  } else if (app.recommendation_data.needs_sample_rate_conversion) {
    SetWindowTextW(app.recommendation,
        L"所选端点没有共同原生采样率，或驱动未报告完整格式。\n引擎会逐设备协商并转换采样率；建议从 48 kHz 开始。");
  }
}

void add_list_columns(HWND list) {
  const std::array<std::pair<const wchar_t*, int>, 5> columns{{
      {L"声道 / KS 端点", 420}, {L"方向", 70}, {L"KS Pin", 75},
      {L"原生采样率", 135}, {L"格式", 220}}};
  for (int i = 0; i < static_cast<int>(columns.size()); ++i) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(columns[static_cast<size_t>(i)].first);
    column.cx = columns[static_cast<size_t>(i)].second;
    column.iSubItem = i;
    ListView_InsertColumn(list, i, &column);
  }
}

void resize_columns(HWND list, const config_theme::Theme& theme) {
  RECT rect{};
  GetClientRect(list, &rect);
  const int available = static_cast<int>(rect.right - rect.left) - theme.px(20);
  const int detail_width = std::max(theme.px(270), available * 53 / 100);
  ListView_SetColumnWidth(list, 0, std::max(theme.px(230), available - detail_width));
  ListView_SetColumnWidth(list, 1, theme.px(60));
  ListView_SetColumnWidth(list, 2, theme.px(52));
  ListView_SetColumnWidth(list, 3, detail_width * 40 / 100);
  ListView_SetColumnWidth(list, 4, detail_width - detail_width * 40 / 100 - theme.px(112));
}

bool selection_contains_endpoint(const AudioSettings& settings, const AudioEndpoint& endpoint) {
  const auto& ids = endpoint.direction == Direction::input
      ? settings.input_device_ids : settings.output_device_ids;
  return std::find(ids.begin(), ids.end(), endpoint.id) != ids.end() ||
      std::any_of(endpoint.legacy_ids.begin(), endpoint.legacy_ids.end(), [&](const auto& legacy_id) {
        return std::find(ids.begin(), ids.end(), legacy_id) != ids.end();
      });
}

bool selection_contains_channel(const AudioSettings& settings, const AudioEndpoint& endpoint,
                                unsigned channel) {
  if (!settings.channel_selection_configured) return selection_contains_endpoint(settings, endpoint);
  const auto& keys = endpoint.direction == Direction::input
      ? settings.input_channel_keys : settings.output_channel_keys;
  const auto key = channel_selection_key(endpoint.id, channel);
  if (std::find(keys.begin(), keys.end(), key) != keys.end()) return true;
  return std::any_of(endpoint.legacy_ids.begin(), endpoint.legacy_ids.end(), [&](const auto& alias) {
    return std::find(keys.begin(), keys.end(), channel_selection_key(alias, channel)) != keys.end();
  });
}

void populate_device_list(AppState& app, const AudioSettings& selection) {
  ListView_DeleteAllItems(app.device_list);
  ListView_EnableGroupView(app.device_list, FALSE);
  SendMessageW(app.device_list, LVM_REMOVEALLGROUPS, 0, 0);
  app.devices.clear();
  app.channels.clear();

  std::vector<size_t> endpoint_groups(app.endpoints.size());
  for (size_t endpoint_index = 0; endpoint_index < app.endpoints.size(); ++endpoint_index) {
    const auto& endpoint = app.endpoints[endpoint_index];
    auto found = std::find_if(app.devices.begin(), app.devices.end(), [&](const auto& item) {
      return same_text(item.id, endpoint.device_instance_id);
    });
    if (found == app.devices.end()) {
      DeviceGroup device;
      device.id = endpoint.device_instance_id;
      device.name = endpoint.device_name.empty() ? L"KS 音频设备" : endpoint.device_name;
      device.buffer_frames = device_buffer_frames(selection, device.id);
      device.list_group_id = static_cast<int>(app.devices.size() + 1);
      app.devices.push_back(std::move(device));
      endpoint_groups[endpoint_index] = app.devices.size() - 1;
    } else {
      endpoint_groups[endpoint_index] = static_cast<size_t>(std::distance(app.devices.begin(), found));
    }
  }

  for (auto& device : app.devices) {
    LVGROUP group{};
    group.cbSize = sizeof(group);
    group.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE;
    group.stateMask = LVGS_COLLAPSIBLE;
    group.state = LVGS_COLLAPSIBLE;
    group.pszHeader = const_cast<wchar_t*>(device.name.c_str());
    group.cchHeader = static_cast<int>(device.name.size());
    group.iGroupId = device.list_group_id;
    ListView_InsertGroup(app.device_list, -1, &group);
  }
  ListView_EnableGroupView(app.device_list, TRUE);

  std::vector<size_t> endpoint_order;
  for (size_t index = 0; index < app.endpoints.size(); ++index) endpoint_order.push_back(index);
  std::stable_sort(endpoint_order.begin(), endpoint_order.end(), [&](size_t left, size_t right) {
    if (endpoint_groups[left] != endpoint_groups[right]) return endpoint_groups[left] < endpoint_groups[right];
    const auto& a = app.endpoints[left];
    const auto& b = app.endpoints[right];
    const int name = compare_channel_names(a.pin_role.empty() ? a.name : a.pin_role,
        b.pin_role.empty() ? b.name : b.pin_role);
    if (name) return name < 0;
    if (a.direction != b.direction) return a.direction == Direction::input;
    if (a.ks_pin_id != b.ks_pin_id) return a.ks_pin_id < b.ks_pin_id;
    return a.id < b.id;
  });
  for (const size_t endpoint_index : endpoint_order) {
    const auto& endpoint = app.endpoints[endpoint_index];
    const size_t device_index = endpoint_groups[endpoint_index];
    for (unsigned channel = 0; channel < endpoint.channels; ++channel) {
      const size_t channel_index = app.channels.size();
      app.channels.push_back({endpoint_index, channel, device_index});
      const auto name = channel_name(endpoint, channel);
      LVITEMW item{};
      item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_GROUPID;
      item.iItem = ListView_GetItemCount(app.device_list);
      item.iGroupId = app.devices[device_index].list_group_id;
      item.pszText = const_cast<wchar_t*>(name.c_str());
      item.lParam = static_cast<LPARAM>(channel_index);
      const int row = ListView_InsertItem(app.device_list, &item);
      if (row < 0) {
        app.channels.pop_back();
        continue;
      }
      app.devices[device_index].channel_indices.push_back(channel_index);
      ListView_SetCheckState(app.device_list, row,
          selection_contains_channel(selection, endpoint, channel));

      const wchar_t* direction = endpoint.direction == Direction::input ? L"输入" : L"输出";
      const auto pin = std::to_wstring(endpoint.ks_pin_id);
      const auto rates = format_rates(endpoint.native_sample_rates);
      const auto formats = format_formats(endpoint.native_formats);
      ListView_SetItemText(app.device_list, row, 1, const_cast<wchar_t*>(direction));
      ListView_SetItemText(app.device_list, row, 2, const_cast<wchar_t*>(pin.c_str()));
      ListView_SetItemText(app.device_list, row, 3, const_cast<wchar_t*>(rates.c_str()));
      ListView_SetItemText(app.device_list, row, 4, const_cast<wchar_t*>(formats.c_str()));
    }
  }
  update_all_device_group_headers(app);
  if (ListView_GetItemCount(app.device_list) > 0) {
    ListView_SetItemState(app.device_list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
  }
  update_current_device(app);
  ShowWindow(app.device_list, app.channels.empty() || app.showing_settings ? SW_HIDE : SW_SHOW);
}

AudioSettings selection_from_current_ui(const AppState& app) {
  AudioSettings selection = app.saved_settings;
  selection.input_device_ids = selected_endpoint_ids(app, Direction::input);
  selection.output_device_ids = selected_endpoint_ids(app, Direction::output);
  selection.input_channel_keys = selected_channel_keys(app, Direction::input);
  selection.output_channel_keys = selected_channel_keys(app, Direction::output);
  selection.device_buffers = current_device_buffers(app);
  selection.channel_selection_configured = true;
  return selection;
}

struct DeviceListView {
  std::wstring selected_channel;
  std::wstring selected_device;
  std::wstring anchor_channel;
  std::wstring anchor_device;
  int anchor_top{};
  int horizontal_scroll{};
  std::vector<std::pair<std::wstring, bool>> collapsed_groups;
};

std::wstring channel_key_at_row(const AppState& app, int row) {
  LVITEMW item{};
  item.mask = LVIF_PARAM;
  item.iItem = row;
  if (row < 0 || !ListView_GetItem(app.device_list, &item)) return {};
  const auto index = static_cast<size_t>(item.lParam);
  if (index >= app.channels.size()) return {};
  const auto& channel = app.channels[index];
  return channel_selection_key(app.endpoints[channel.endpoint_index].id, channel.endpoint_channel);
}

DeviceListView capture_device_list_view(const AppState& app) {
  DeviceListView view;
  const int selected = ListView_GetNextItem(app.device_list, -1, LVNI_SELECTED);
  view.selected_channel = channel_key_at_row(app, selected);
  const auto selected_device = device_group_for_row(app, selected);
  if (selected_device < app.devices.size()) view.selected_device = app.devices[selected_device].id;
  view.horizontal_scroll = GetScrollPos(app.device_list, SB_HORZ);
  for (const auto& device : app.devices) {
    const auto state = ListView_GetGroupState(app.device_list, device.list_group_id, LVGS_COLLAPSED);
    view.collapsed_groups.emplace_back(device.id, (state & LVGS_COLLAPSED) != 0);
  }
  RECT body{}, header{};
  GetClientRect(app.device_list, &body);
  GetWindowRect(ListView_GetHeader(app.device_list), &header);
  MapWindowPoints(nullptr, app.device_list, reinterpret_cast<POINT*>(&header), 2);
  body.top = std::max(body.top, header.bottom);
  // GetTopIndex is unreliable in grouped ListViews. Anchor to a visible row's
  // stable channel ID and pixel position instead of its enumeration index.
  for (int row = 0; row < ListView_GetItemCount(app.device_list); ++row) {
    const auto device_index = device_group_for_row(app, row);
    if (device_index >= app.devices.size() ||
        (ListView_GetGroupState(app.device_list, app.devices[device_index].list_group_id,
            LVGS_COLLAPSED) & LVGS_COLLAPSED)) continue;
    RECT bounds{}, visible{};
    if (!ListView_GetItemRect(app.device_list, row, &bounds, LVIR_BOUNDS) ||
        !IntersectRect(&visible, &bounds, &body)) continue;
    if (!view.anchor_channel.empty() && bounds.top >= view.anchor_top) continue;
    view.anchor_channel = channel_key_at_row(app, row);
    view.anchor_device = app.devices[device_index].id;
    view.anchor_top = bounds.top;
  }
  if (view.anchor_channel.empty()) {
    // A fully collapsed list can have visible group headers but no visible rows.
    for (const auto& device : app.devices) {
      RECT bounds{}, visible{};
      if (ListView_GetGroupRect(app.device_list, device.list_group_id, LVGGR_HEADER, &bounds) &&
          IntersectRect(&visible, &bounds, &body) &&
          (view.anchor_device.empty() || bounds.top < view.anchor_top)) {
        view.anchor_device = device.id;
        view.anchor_top = bounds.top;
      }
    }
  }
  return view;
}

int row_for_channel_key(const AppState& app, const std::wstring& key) {
  if (key.empty()) return -1;
  AudioSettings selection;
  selection.channel_selection_configured = true;
  selection.input_channel_keys = selection.output_channel_keys = {key};
  for (size_t row = 0; row < app.channels.size(); ++row) {
    const auto& channel = app.channels[row];
    if (selection_contains_channel(selection, app.endpoints[channel.endpoint_index], channel.endpoint_channel))
      return static_cast<int>(row);
  }
  return -1;
}

void restore_device_list_state(AppState& app, const DeviceListView& view) {
  for (const auto& device : app.devices) {
    const auto found = std::find_if(view.collapsed_groups.begin(), view.collapsed_groups.end(),
        [&](const auto& group) { return same_text(device.id, group.first); });
    if (found == view.collapsed_groups.end()) continue;
    LVGROUP group{};
    group.cbSize = sizeof(group);
    group.mask = LVGF_STATE;
    group.stateMask = LVGS_COLLAPSED;
    group.state = found->second ? LVGS_COLLAPSED : 0;
    ListView_SetGroupInfo(app.device_list, device.list_group_id, &group);
  }
  int selected = row_for_channel_key(app, view.selected_channel);
  if (selected < 0 && !view.selected_device.empty()) {
    const auto device = std::find_if(app.devices.begin(), app.devices.end(),
        [&](const auto& group) { return same_text(group.id, view.selected_device); });
    if (device != app.devices.end() && !device->channel_indices.empty())
      selected = static_cast<int>(device->channel_indices.front());
  }
  if (selected >= 0) {
    ListView_SetItemState(app.device_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(app.device_list, selected, LVIS_SELECTED | LVIS_FOCUSED,
        LVIS_SELECTED | LVIS_FOCUSED);
  }
  update_current_device(app);
}

void restore_device_list_position(AppState& app, const DeviceListView& view) {
  const int horizontal_delta = view.horizontal_scroll - GetScrollPos(app.device_list, SB_HORZ);
  if (horizontal_delta) ListView_Scroll(app.device_list, horizontal_delta, 0);
  const int anchor = row_for_channel_key(app, view.anchor_channel);
  RECT bounds{};
  if (anchor >= 0) {
    // Report-view scrolling rounds to native row increments. Position directly
    // from the new row bounds so EnsureVisible does not introduce another jump.
    if (ListView_GetItemRect(app.device_list, anchor, &bounds, LVIR_BOUNDS))
      ListView_Scroll(app.device_list, 0, bounds.top - view.anchor_top);
  } else if (!view.anchor_device.empty()) {
    // If the top channel disappeared, retain the surviving device's header.
    const auto device = std::find_if(app.devices.begin(), app.devices.end(),
        [&](const auto& group) { return same_text(group.id, view.anchor_device); });
    if (device != app.devices.end() &&
        ListView_GetGroupRect(app.device_list, device->list_group_id, LVGGR_HEADER, &bounds))
      ListView_Scroll(app.device_list, 0, bounds.top - view.anchor_top);
  }
}

bool same_device_list_content(const std::vector<AudioEndpoint>& first,
                              const std::vector<AudioEndpoint>& second) {
  return first.size() == second.size() && std::equal(first.begin(), first.end(), second.begin(),
      [](const auto& left, const auto& right) {
        return left.id == right.id && left.device_instance_id == right.device_instance_id &&
            left.device_name == right.device_name && left.pin_role == right.pin_role &&
            left.direction == right.direction && left.ks_pin_id == right.ks_pin_id &&
            left.channels == right.channels && left.native_sample_rates == right.native_sample_rates &&
            left.native_formats == right.native_formats;
      });
}

void refresh_devices(AppState& app) {
  AudioSettings selection = app.saved_settings;
  if (!app.endpoints.empty() && app.device_list) {
    selection = selection_from_current_ui(app);
  }
  auto endpoints = enumerate_endpoints();
  canonicalize_endpoint_selections(selection, endpoints);
  if (!app.channels.empty() && same_device_list_content(app.endpoints, endpoints)) {
    // Keep the existing native items when their displayed content is unchanged.
    // Still replace KS metadata for probes and device recovery, without touching
    // selection, collapsed groups or the exact scroll position.
    app.endpoints = std::move(endpoints);
    app.saved_settings = selection;
    update_current_device(app);
    update_recommendation(app);
    return;
  }
  const auto view = capture_device_list_view(app);
  {
    struct RefreshDrawing {
      AppState& app;
      explicit RefreshDrawing(AppState& state) : app(state) {
        app.populating = true;
        SendMessageW(app.device_list, WM_SETREDRAW, FALSE, 0);
      }
      ~RefreshDrawing() {
        app.populating = false;
        SendMessageW(app.device_list, WM_SETREDRAW, TRUE, 0);
        ShowWindow(app.device_list, app.channels.empty() || app.showing_settings ? SW_HIDE : SW_SHOW);
        RedrawWindow(app.device_list, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
      }
    } drawing(app);
    app.endpoints = std::move(endpoints);
    populate_device_list(app, selection);
    restore_device_list_state(app, view);
    app.saved_settings = selection;
  }
  // Group geometry and scrolling are deferred while WM_SETREDRAW is disabled.
  // Restore the viewport only after the common control has resumed layout.
  restore_device_list_position(app, view);
  update_recommendation(app);

  size_t input_channels = 0;
  size_t output_channels = 0;
  for (const auto& channel : app.channels) {
    if (app.endpoints[channel.endpoint_index].direction == Direction::input) ++input_channels;
    else ++output_channels;
  }
  append_log(app, L"发现 " + std::to_wstring(app.devices.size()) + L" 个总设备、" +
      std::to_wstring(input_channels) + L" 个输入声道和 " +
      std::to_wstring(output_channels) + L" 个输出声道。KS Pin 按声道列在各设备组下面。");
}

void fill_buffer_combo(HWND combo, unsigned selected) {
  constexpr std::array<unsigned, 6> choices{64, 128, 256, 512, 1024, 2048};
  for (const auto frames : choices) {
    const auto label = std::to_wstring(frames) + L" 帧";
    const auto index = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0,
                                                      reinterpret_cast<LPARAM>(label.c_str())));
    if (index != CB_ERR && index != CB_ERRSPACE) {
      SendMessageW(combo, CB_SETITEMDATA, index, frames);
    }
  }
  select_combo_value(combo, selected);
}

void show_device_access_error(AppState& app, const AudioEndpoint& endpoint, long status) {
  const auto name = endpoint.name.empty() ? endpoint.device_name : endpoint.name;
  const std::wstring message = name + L"\n\n" +
      (ks_device_access::is_busy(status) ? ks_device_access::kBusyMessage
          : L"无法访问设备，请检查设备连接和驱动后再试。");
  append_log(app, message);
  MessageBoxW(app.window, message.c_str(),
      ks_device_access::is_busy(status) ? L"设备被占用" : L"设备不可用", MB_OK | MB_ICONWARNING);
}

bool driver_is_open() {
  const HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, kSettingsChangedEventName);
  if (!event) return false;
  CloseHandle(event);
  return true;
}

bool check_selected_device_access(AppState& app, bool allow_existing_driver_pins) {
  const bool existing_driver = allow_existing_driver_pins && driver_is_open();
  const auto selection = selection_from_current_ui(app);
  for (const auto& endpoint : app.endpoints) {
    if (!selection_contains_endpoint(selection, endpoint)) continue;
    // Editing an already selected pin changes metadata, without a second open.
    // Buffer detection, in contrast, always needs its own streaming pin.
    if (existing_driver && selection_contains_endpoint(app.saved_settings, endpoint)) continue;
    const auto status = ks_device_access::check_endpoint(endpoint);
    if (FAILED(status)) {
      show_device_access_error(app, endpoint, status);
      return false;
    }
  }
  return true;
}

bool can_enable_channel(AppState& app, size_t channel_index) {
  if (channel_index >= app.channels.size()) return false;
  const auto endpoint_index = app.channels[channel_index].endpoint_index;
  for (const auto checked : checked_channel_indices(app)) {
    if (checked != channel_index && app.channels[checked].endpoint_index == endpoint_index) return true;
  }
  const auto& endpoint = app.endpoints[endpoint_index];
  if (driver_is_open() && selection_contains_endpoint(app.saved_settings, endpoint)) return true;
  const auto status = ks_device_access::check_endpoint(endpoint);
  if (SUCCEEDED(status)) return true;
  show_device_access_error(app, endpoint, status);
  return false;
}

void save_profile(AppState& app) {
  AudioSettings settings = selection_from_current_ui(app);
  if (settings.input_channel_keys.empty() && settings.output_channel_keys.empty()) {
    MessageBoxW(app.window, L"请至少勾选一个输入或输出声道。", L"尚未选择声道",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (settings.input_channel_keys.size() > 128 || settings.output_channel_keys.size() > 128) {
    MessageBoxW(app.window, L"每个方向最多启用 128 个 ASIO 声道。", L"声道数量过多", MB_OK | MB_ICONINFORMATION);
    return;
  }
  settings.sample_rate = selected_sample_rate(app);
  settings.buffer_frames = combo_value(app.asio_buffer, 128);
  settings.initialized = true;
  if (!check_selected_device_access(app, true)) return;
  if (!save_audio_settings(settings)) {
    MessageBoxW(app.window, L"设置保存失败。请检查当前 Windows 用户的 Local AppData 权限。",
                L"保存失败", MB_OK | MB_ICONERROR);
    return;
  }
  app.saved_settings = settings;
  append_log(app, L"已保存声道选择、设备 KS 缓冲和全局 ASIO 缓冲。");
  HANDLE settings_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kSettingsChangedEventName);
  const bool refresh_requested = settings_event && SetEvent(settings_event);
  if (settings_event) CloseHandle(settings_event);
  if (refresh_requested) {
    append_log(app, L"已通知正在使用的 ASIO 驱动请求 DAW 重载声道列表。");
    MessageBoxW(app.window,
        L"设置已保存，并已请求 DAW 重新初始化此 ASIO 驱动以更新声道列表。通常会短暂停止音频，但无需退出 DAW。",
        L"保存成功", MB_OK | MB_ICONINFORMATION);
  } else {
    append_log(app, L"没有检测到正在运行的 ASIO 驱动；新设置会在下次启用时读取。");
    MessageBoxW(app.window,
        L"设置已保存。当前没有检测到已打开的 ASIO 驱动；新设置会在 DAW 下次启用该驱动时读取。",
        L"保存成功", MB_OK | MB_ICONINFORMATION);
  }
}

std::vector<ProbeDevice> selected_probe_devices(const AppState& app) {
  std::vector<ProbeDevice> result;
  for (const auto channel_index : checked_channel_indices(app)) {
    const auto& channel = app.channels[channel_index];
    const auto& endpoint = app.endpoints[channel.endpoint_index];
    auto found = std::find_if(result.begin(), result.end(), [&](const auto& device) {
      return same_text(device.id, endpoint.device_instance_id);
    });
    if (found == result.end()) {
      result.push_back({endpoint.device_instance_id,
                        endpoint.device_name.empty() ? L"KS 音频设备" : endpoint.device_name,
                        {endpoint}});
    } else if (std::none_of(found->endpoints.begin(), found->endpoints.end(), [&](const auto& item) {
                 return item.id == endpoint.id;
               })) {
      found->endpoints.push_back(endpoint);
    }
  }
  return result;
}

void set_probe_controls_enabled(AppState& app, bool enabled) {
  EnableWindow(app.device_list, enabled);
  EnableWindow(app.sample_rate, enabled);
  EnableWindow(app.asio_buffer, enabled);
  EnableWindow(app.device_buffer, enabled);
  EnableWindow(app.refresh, enabled);
  EnableWindow(app.auto_detect, enabled);
  EnableWindow(app.save, enabled);
  SetWindowTextW(app.auto_detect, enabled ? L"自动检测设备缓冲" : L"正在检测…");
  InvalidateRect(app.window, nullptr, FALSE);
}

void start_auto_detect(AppState& app) {
  if (app.probe_job && !app.probe_job->done.load()) return;
  auto devices = selected_probe_devices(app);
  if (devices.empty()) {
    MessageBoxW(app.window, L"请先勾选至少一个输入或输出声道。", L"自动检测",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (!check_selected_device_access(app, false)) return;

  const auto sample_rate = selected_sample_rate(app);
  const HWND window = app.window;
  const auto job = std::make_shared<ProbeJob>();
  app.probe_job = job;
  app.last_probe_message.clear();
  set_probe_controls_enabled(app, false);
  append_log(app, L"开始逐设备 KS 缓冲探测：输出 Pin 会收到数字静音，输入数据会丢弃。");
  SetTimer(window, kProbeTimer, 250, nullptr);

  std::thread([job, devices = std::move(devices), sample_rate]() {
    constexpr std::array<unsigned, 6> candidates{64, 128, 256, 512, 1024, 2048};
    bool all_devices_passed = true;
    try {
      for (const auto& device : devices) {
        if (job->cancelled.load()) break;
        unsigned chosen = 2048;
        bool device_passed = false;
        for (const auto candidate : candidates) {
          if (job->cancelled.load()) break;
          bool all_stable = true;
          {
            std::lock_guard lock(job->mutex);
            job->latest_message = L"正在检测 " + device.name + L" 的 " +
                                  std::to_wstring(candidate) + L" 帧 KS 缓冲…";
          }
          for (const auto& endpoint : device.endpoints) {
            if (job->cancelled.load()) break;
            const auto result = probe_endpoint(endpoint, sample_rate, candidate, 400);
            if (ks_device_access::is_busy(result.hresult)) {
              std::lock_guard lock(job->mutex);
              job->access_error = device.name + L" / " + endpoint.pin_role + L"\n\n" +
                  ks_device_access::kBusyMessage;
              job->latest_message = job->access_error;
              job->passed.store(false);
              job->done.store(true);
              return;
            }
            std::wstring probe_message = device.name + L" / " + endpoint.pin_role + L"：";
            probe_message += result.stable ? L"短时通过，" : L"未通过，";
            probe_message += std::to_wstring(result.actual_buffer_frames) + L" 帧 / " +
                std::to_wstring(result.actual_sample_rate) + L" Hz";
            {
              std::lock_guard lock(job->mutex);
              job->latest_message = std::move(probe_message);
            }
            if (!result.stable) all_stable = false;
          }
          if (job->cancelled.load()) break;
          if (all_stable) {
            chosen = candidate;
            device_passed = true;
            break;
          }
        }
        if (!device_passed) all_devices_passed = false;
        {
          std::lock_guard lock(job->mutex);
          job->result_device_buffers.push_back({device.id, chosen});
          job->latest_message = device.name + L"：选择 " + std::to_wstring(chosen) +
              L" 帧 KS 缓冲" + (device_passed ? L"。" : L"（未通过探测，采用最大档）。");
        }
      }
    } catch (...) {
      std::lock_guard lock(job->mutex);
      job->latest_message = L"探测异常；未完成的设备请使用较大的 KS 缓冲档位。";
      all_devices_passed = false;
    }
    job->passed.store(all_devices_passed);
    job->done.store(true);
  }).detach();
}

void create_controls(AppState& app) {
  const auto make = [&](DWORD ex, const wchar_t* klass, const wchar_t* text, DWORD style,
                        int x, int y, int width, int height, int id) {
    HWND control = CreateWindowExW(ex, klass, text, style | WS_CLIPSIBLINGS, x, y, width, height,
                                   app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app.theme.body), TRUE);
    return control;
  };

  const DWORD list_style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                           LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_SHAREIMAGELISTS;
  app.device_list = make(0, WC_LISTVIEWW, L"", list_style,
                         22, 61, 1060, 360, kDeviceList);
  ListView_SetExtendedListViewStyle(app.device_list,
      LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
  add_list_columns(app.device_list);
  app.theme.style_list(app.device_list);

  app.sample_rate_label = make(0, L"STATIC", L"采样率", WS_CHILD | WS_VISIBLE,
                               16, 455, 56, 24, 1103);
  app.sample_rate = make(0, WC_COMBOBOXW, L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
      74, 451, 145, 300, kSampleRate);
  app.asio_buffer_label = make(0, L"STATIC", L"ASIO 主机缓冲", WS_CHILD | WS_VISIBLE,
                               230, 455, 98, 24, 1104);
  app.asio_buffer = make(0, WC_COMBOBOXW, L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
      332, 451, 115, 250, kAsioBuffer);
  app.device_buffer_label = make(0, L"STATIC", L"设备 KS 缓冲", WS_CHILD | WS_VISIBLE,
                                 460, 455, 92, 24, 1105);
  app.device_buffer = make(0, WC_COMBOBOXW, L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
      556, 451, 115, 250, kDeviceBuffer);
  app.active_device_label = make(0, L"STATIC", L"当前设备", WS_CHILD | WS_VISIBLE,
                                 681, 431, 300, 24, 1106);
  app.active_device = make(0, L"STATIC", L"请选择设备声道",
                           WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX,
                           681, 455, 300, 24, kActiveDevice);

  app.refresh = make(0, L"BUTTON", L"刷新设备", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                     16, 495, 104, 30, kRefresh);
  app.auto_detect = make(0, L"BUTTON", L"自动检测设备缓冲", WS_CHILD | WS_VISIBLE |
                         WS_TABSTOP | BS_OWNERDRAW, 130, 495, 150, 30, kAutoDetect);
  app.save = make(0, L"BUTTON", L"保存设置", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                  290, 495, 104, 30, kSave);
  app.recommendation = make(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                            16, 535, 1070, 42, kRecommendation);
  // The initial page is the device page. Keep the full-page log hidden from
  // creation, including before the first WM_SIZE or page-selection message.
  app.status_log = make(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP |
                        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                        16, 585, 1070, 190, kStatusLog);
  app.theme.style_scrollbars(app.status_log);
  app.repository_link = make(0, WC_LINK, L"<a href=\"https://github.com/wanan522z/AggregateAudio-asio\">GitHub 仓库 · AggregateAudio-asio</a>",
      WS_CHILD | WS_TABSTOP, 0, 0, 1, 1, kRepository);
  app.attribution = make(0, L"STATIC", L"powered by WanAn522z",
      WS_CHILD | SS_LEFT | SS_CENTERIMAGE, 0, 0, 1, 1, 0);
  app.close = make(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                   992, 790, 100, 30, kClose);
  app.settings_button = make(0, L"BUTTON", L"Settings", WS_CHILD | WS_VISIBLE |
      WS_TABSTOP | BS_OWNERDRAW, 0, 0, 100, 32, kSettings);
  for (const auto button : {app.refresh, app.auto_detect, app.save, app.close, app.settings_button}) {
    app.theme.style_button(button);
  }
  for (const auto combo : {app.sample_rate, app.asio_buffer, app.device_buffer}) {
    app.theme.style_combo(combo);
  }
}

void apply_control_fonts(AppState& app) {
  EnumChildWindows(app.window, [](HWND control, LPARAM data) -> BOOL {
    const auto& theme = *reinterpret_cast<config_theme::Theme*>(data);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(theme.body), TRUE);
    return TRUE;
  }, reinterpret_cast<LPARAM>(&app.theme));
  for (const auto label : {app.sample_rate_label, app.asio_buffer_label,
                           app.device_buffer_label, app.active_device_label,
                           app.recommendation, app.status_log, app.repository_link, app.attribution}) {
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(app.theme.small_font), TRUE);
  }
  for (const auto combo : {app.sample_rate, app.asio_buffer, app.device_buffer}) {
    SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), app.theme.px(30));
    SendMessageW(combo, CB_SETITEMHEIGHT, 0, app.theme.px(32));
  }
  SendMessageW(app.status_log, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
      MAKELPARAM(app.theme.px(2), app.theme.px(8)));
}

void layout_controls(AppState& app, int width, int height) {
  if (!app.device_list || width <= 0 || height <= 0) return;
  const auto px = [&](int value) { return app.theme.px(value); };
  const int margin = px(16);
  const int content_width = width - margin * 2;
  const bool narrow = width < px(820);
  const int actions_y = height - px(48);
  const int recommendation_y = actions_y - px(42);
  const int settings_height = px(narrow ? 104 : 78);
  const int settings_y = recommendation_y - settings_height - px(10);
  app.device_card = {margin, px(60), width - margin, settings_y - px(10)};
  app.settings_card = {margin, settings_y, width - margin, settings_y + settings_height};
  app.log_card = {margin, px(60), width - margin, actions_y - px(12)};
  app.footer = {margin, actions_y, width - margin, height - px(16)};

  MoveWindow(app.settings_button, width - margin - px(100), px(15), px(100), px(32), TRUE);
  MoveWindow(app.device_list, margin + px(10), app.device_card.top + px(42),
      std::max(1, content_width - px(20)),
      std::max(1L, app.device_card.bottom - app.device_card.top - px(52)), TRUE);
  resize_columns(app.device_list, app.theme);

  const int field_y = settings_y + px(32);
  const int label_y = settings_y + px(10);
  const int inner_width = content_width - px(28);
  const int gap = px(12);
  const int field_width = (inner_width - gap * (narrow ? 2 : 3)) / (narrow ? 3 : 4);
  const std::array<HWND, 3> labels{app.sample_rate_label, app.asio_buffer_label, app.device_buffer_label};
  const std::array<HWND, 3> combos{app.sample_rate, app.asio_buffer, app.device_buffer};
  for (int index = 0; index < 3; ++index) {
    const int x = margin + px(14) + (field_width + gap) * index;
    MoveWindow(labels[index], x, label_y, field_width, px(18), TRUE);
    // The height includes the native drop-down; the closed face has its own item height.
    MoveWindow(combos[index], x, field_y, field_width, px(240), TRUE);
  }
  const int device_x = narrow ? margin + px(14) : margin + px(14) + (field_width + gap) * 3;
  MoveWindow(app.active_device_label, device_x, narrow ? settings_y + px(76) : label_y,
             narrow ? px(60) : field_width, px(18), TRUE);
  MoveWindow(app.active_device, narrow ? device_x + px(68) : device_x,
             narrow ? settings_y + px(72) : field_y,
             narrow ? inner_width - px(68) : field_width, px(narrow ? 26 : 34), TRUE);

  MoveWindow(app.refresh, margin, actions_y, px(108), px(32), TRUE);
  MoveWindow(app.auto_detect, margin + px(118), actions_y, px(164), px(32), TRUE);
  MoveWindow(app.save, margin + px(292), actions_y, px(108), px(32), TRUE);
  MoveWindow(app.recommendation, margin + px(24), recommendation_y,
             content_width - px(24), px(34), TRUE);
  MoveWindow(app.status_log, app.log_card.left + px(14), app.log_card.top + px(34),
             content_width - px(28), std::max(1L, app.log_card.bottom - app.log_card.top - px(100)), TRUE);
  MoveWindow(app.repository_link, app.log_card.left + px(14), app.log_card.bottom - px(60),
             content_width - px(28), px(24), TRUE);
  MoveWindow(app.attribution, app.log_card.left + px(14), app.log_card.bottom - px(34),
             content_width - px(28), px(22), TRUE);
  MoveWindow(app.close, width - margin - px(80), actions_y, px(80), px(32), TRUE);
  InvalidateRect(app.window, nullptr, FALSE);
}

void paint_window(AppState& app, HDC dc) {
  using namespace config_theme;
  RECT client{};
  GetClientRect(app.window, &client);
  FillRect(dc, &client, app.theme.canvas);
  const auto px = [&](int value) { return app.theme.px(value); };
  draw_brand(dc, {px(18), px(16), px(46), px(44)});
  text(dc, L"Aggregate Audio", {px(58), px(10), client.right - px(130), px(36)},
       app.theme.title, kInk);
  text(dc, L"让每台设备，协同发声。", {px(59), px(35), client.right - px(130), px(51)},
       app.theme.small_font, kMuted);
  const RECT badge{client.right - px(299), px(18), client.right - px(128), px(44)};
  if (client.right >= px(700)) {
  rounded_rect(dc, badge, px(15), RGB(235, 241, 237));
  rounded_rect(dc, {badge.left + px(13), badge.top + px(12), badge.left + px(19), badge.top + px(18)},
               px(3), kAccent);
  text(dc, L"Kernel Streaming", {badge.left + px(27), badge.top, badge.right - px(10), badge.bottom},
       app.theme.small_font, kAccent);
  }

  if (app.showing_settings) {
    rounded_rect(dc, app.log_card, px(12), kLog, kBorder);
    text(dc, L"运行日志 · 本次会话", {app.log_card.left + px(14), app.log_card.top + px(6),
         app.log_card.right - px(14), app.log_card.top + px(28)}, app.theme.strong, kInk);
    return;
  }

  rounded_rect(dc, app.device_card, px(14), kSurface, kBorder);
  text(dc, L"设备与声道", {app.device_card.left + px(14), app.device_card.top + px(7),
       app.device_card.right - px(260), app.device_card.top + px(34)}, app.theme.strong, kInk);
  const auto summary = std::to_wstring(app.devices.size()) + L" 台设备  ·  " +
      std::to_wstring(checked_channel_indices(app).size()) + L" 个声道已启用";
  text(dc, summary, {app.device_card.right - px(250), app.device_card.top + px(7),
       app.device_card.right - px(14), app.device_card.top + px(34)},
       app.theme.small_font, kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
  if (app.channels.empty()) {
    const int middle = (app.device_card.top + app.device_card.bottom) / 2;
    text(dc, L"暂未发现可用的音频声道", {app.device_card.left, middle - px(12),
         app.device_card.right, middle + px(12)}, app.theme.body, kMuted,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  rounded_rect(dc, app.settings_card, px(14), kSurface, kBorder);
  const auto hint = app.device_card.left;
  const int info_y = app.settings_card.bottom + px(10);
  rounded_rect(dc, {hint, info_y + px(3), hint + px(16), info_y + px(19)}, px(8), RGB(231, 234, 229));
  text(dc, L"i", {hint, info_y + px(2), hint + px(16), info_y + px(19)}, app.theme.small_font, kMuted,
       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* app = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    app = static_cast<AppState*>(create->lpCreateParams);
    app->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  if (!app) return DefWindowProcW(window, message, wparam, lparam);
  if (message == kTaskbarCreated && app->tray_enabled) {
    app->tray_icon.explorer_restarted();
    update_tray(*app);
    return 0;
  }

  switch (message) {
    case WM_CREATE: {
      app->theme.set_dpi(GetDpiForWindow(window));
      create_controls(*app);
      app->app_icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(config_tray::kIconResource),
          IMAGE_ICON, 32, 32, LR_SHARED));
      SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(app->app_icon));
      SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(app->app_icon));
      app->last_active_at = GetTickCount64();
      if (app->tray_enabled) {
        SetTimer(window, kTrayTimer, 1000, nullptr);
        update_tray(*app);
      }
      apply_control_fonts(*app);
      app->saved_settings = load_audio_settings();
      fill_buffer_combo(app->asio_buffer,
          app->saved_settings.initialized ? app->saved_settings.buffer_frames : 128);
      fill_buffer_combo(app->device_buffer,
          app->saved_settings.initialized ? app->saved_settings.buffer_frames : 128);
      try { refresh_devices(*app); }
      catch (...) { append_log(*app, L"枚举 KS 设备失败，请点“刷新设备”重试。"); }
      select_combo_value(app->sample_rate, app->saved_settings.sample_rate);
      update_current_device(*app);
      update_latency_text(*app);
      if (app->saved_settings.initialized) {
        append_log(*app, L"已载入上次保存的配置。保存时会请求当前 DAW 重新初始化 ASIO 驱动。");
      } else {
        append_log(*app, L"首次配置：展开总设备组选择通道；可以逐台自动检测 KS 缓冲。");
      }
      // CreateWindow/SetWindowPos need not send WM_SIZE when the requested size
      // stays unchanged. Commit DPI-scaled geometry and page visibility before
      // the window can be shown or activated by a second process.
      show_settings(*app, app->showing_settings);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PRINTCLIENT:
      paint_window(*app, reinterpret_cast<HDC>(wparam));
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      const auto dc = BeginPaint(window, &paint);
      RECT client{};
      GetClientRect(window, &client);
      const auto buffer_dc = CreateCompatibleDC(dc);
      const auto bitmap = CreateCompatibleBitmap(dc, std::max(1L, client.right), std::max(1L, client.bottom));
      const auto previous = SelectObject(buffer_dc, bitmap);
      paint_window(*app, buffer_dc);
      BitBlt(dc, 0, 0, client.right, client.bottom, buffer_dc, 0, 0, SRCCOPY);
      SelectObject(buffer_dc, previous);
      DeleteObject(bitmap);
      DeleteDC(buffer_dc);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
      const auto dc = reinterpret_cast<HDC>(wparam);
      const auto control = reinterpret_cast<HWND>(lparam);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, control == app->active_device ? config_theme::kInk : config_theme::kMuted);
      if (control == app->status_log || control == app->repository_link || control == app->attribution) {
        SetBkColor(dc, config_theme::kLog);
        return reinterpret_cast<LRESULT>(app->theme.log);
      }
      if (control == app->recommendation) {
        SetBkColor(dc, config_theme::kCanvas);
        return reinterpret_cast<LRESULT>(app->theme.canvas);
      }
      SetBkColor(dc, config_theme::kSurface);
      return reinterpret_cast<LRESULT>(app->theme.surface);
    }
    case WM_MEASUREITEM: {
      auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
      if (item->CtlType == ODT_COMBOBOX) {
        item->itemHeight = app->theme.px(32);
        return TRUE;
      }
      break;
    }
    case WM_DRAWITEM: {
      const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      if (item.CtlType == ODT_BUTTON) {
        config_theme::draw_button(app->theme, item, item.CtlID == kSave);
        return TRUE;
      }
      if (item.CtlType == ODT_COMBOBOX) {
        config_theme::draw_combo_item(app->theme, item);
        return TRUE;
      }
      break;
    }
    case WM_SIZE:
      if (wparam == SIZE_MINIMIZED) return 0;
      layout_controls(*app, LOWORD(lparam), HIWORD(lparam));
      return 0;
    case config_instance::kActivateMessage: {
      app->tray_background = false;
      ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
      const auto popup = GetLastActivePopup(window);
      const auto target = IsWindowVisible(popup) ? popup : window;
      SetForegroundWindow(target);
      return 0;
    }
    case config_tray::kStatusMessage:
      update_tray(*app);
      return 0;
    case config_tray::kDeviceRefreshMessage:
      try { refresh_devices(*app); } catch (...) {}
      append_log(*app, L"检测到已选设备变化或音频流停滞，已请求 DAW 重载驱动并刷新设备列表。");
      return 0;
    case config_tray::kMessage: {
      const auto event = LOWORD(lparam);
      if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONDBLCLK) {
        SendMessageW(window, config_instance::kActivateMessage, 0, 0);
      } else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
        POINT point{}; GetCursorPos(&point);
        const auto menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"打开 Aggregate Audio");
        AppendMenuW(menu, MF_STRING, 2, L"运行日志");
        SetForegroundWindow(window);
        const auto action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu);
        if (action) {
          SendMessageW(window, config_instance::kActivateMessage, 0, 0);
          if (action == 2) show_settings(*app, true);
        }
        PostMessageW(window, WM_NULL, 0, 0);
      }
      return 0;
    }
    case WM_DPICHANGED: {
      app->theme.set_dpi(HIWORD(wparam));
      apply_control_fonts(*app);
      app->theme.update_list_images(app->device_list);
      const auto suggested_rect = *reinterpret_cast<RECT*>(lparam);
      const auto suggested = fit_window_to_work_area(suggested_rect,
          monitor_work_area(MonitorFromRect(&suggested_rect, MONITOR_DEFAULTTONEAREST)), app->theme.px(8));
      SetWindowPos(window, nullptr, suggested.left, suggested.top,
          suggested.right - suggested.left, suggested.bottom - suggested.top,
          SWP_NOZORDER | SWP_NOACTIVATE);
      RECT client{};
      GetClientRect(window, &client);
      layout_controls(*app, client.right, client.bottom);
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      const auto work = monitor_work_area(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST));
      info->ptMinTrackSize.x = std::min(app->theme.px(660),
          static_cast<int>(work.right - work.left) - app->theme.px(16));
      info->ptMinTrackSize.y = std::min(app->theme.px(400),
          static_cast<int>(work.bottom - work.top) - app->theme.px(16));
      return 0;
    }
    case WM_NOTIFY: {
      const auto* header = reinterpret_cast<const NMHDR*>(lparam);
      if (header->idFrom == kRepository && (header->code == NM_CLICK || header->code == NM_RETURN)) {
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open",
            L"https://github.com/wanan522z/AggregateAudio-asio", nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) MessageBoxW(window, L"无法打开浏览器，请手动访问 GitHub 仓库。",
            L"Aggregate Audio", MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      if (header->idFrom == kDeviceList && header->code == NM_CUSTOMDRAW) {
        return config_theme::draw_list(app->theme, *reinterpret_cast<NMLVCUSTOMDRAW*>(lparam));
      }
      if (header->idFrom == kDeviceList && header->code == LVN_ITEMCHANGING && !app->populating) {
        const auto* change = reinterpret_cast<const NMLISTVIEW*>(lparam);
        const bool enabling = (change->uChanged & LVIF_STATE) &&
            (change->uNewState & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2) &&
            (change->uOldState & LVIS_STATEIMAGEMASK) != INDEXTOSTATEIMAGEMASK(2);
        if (enabling && !can_enable_channel(*app, static_cast<size_t>(change->iItem))) return TRUE;
      }
      if (header->idFrom == kDeviceList && header->code == LVN_ITEMCHANGED && !app->populating) {
        const auto* change = reinterpret_cast<const NMLISTVIEW*>(lparam);
        const bool checkbox_changed = (change->uChanged & LVIF_STATE) &&
            ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK);
        const bool selection_changed = (change->uChanged & LVIF_STATE) &&
            ((change->uOldState ^ change->uNewState) & LVIS_SELECTED);
        if (checkbox_changed) {
          update_all_device_group_headers(*app);
          update_recommendation(*app);
        }
        if (selection_changed) {
          update_current_device(*app);
          update_latency_text(*app);
        }
      }
      return 0;
    }
    case DM_GETDEFID:
      return MAKELRESULT(app->showing_settings ? kClose : kSave, DC_HASDEFID);
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kSettings:
          show_settings(*app, !app->showing_settings);
          SetFocus(app->settings_button);
          return 0;
        case kRefresh:
          try { refresh_devices(*app); }
          catch (...) { MessageBoxW(window, L"枚举 KS 设备失败。", L"刷新失败", MB_OK | MB_ICONERROR); }
          return 0;
        case kAutoDetect:
          start_auto_detect(*app);
          return 0;
        case kSave:
          if (IsWindowEnabled(app->save)) save_profile(*app);
          return 0;
        case kClose:
          close_or_hide(*app);
          return 0;
        case IDCANCEL:
          if (app->showing_settings) {
            show_settings(*app, false);
            SetFocus(app->settings_button);
          } else close_or_hide(*app);
          return 0;
        case kSampleRate:
        case kAsioBuffer:
          if (HIWORD(wparam) == CBN_SELCHANGE && !app->populating) {
            update_current_device(*app);
            update_latency_text(*app);
          }
          return 0;
        case kDeviceBuffer:
          if (HIWORD(wparam) == CBN_SELCHANGE && !app->populating &&
              app->active_device_index < app->devices.size()) {
            app->devices[app->active_device_index].buffer_frames =
                combo_value(app->device_buffer, app->saved_settings.buffer_frames);
            update_device_group_header(*app, app->active_device_index);
            update_latency_text(*app);
          }
          return 0;
      }
      break;
    case WM_TIMER:
      if (wparam == kTrayTimer) {
        try { update_tray(*app); } catch (...) {}
        return 0;
      }
      if (wparam == kProbeTimer && app->probe_job) {
        std::wstring progress_text;
        bool done = false;
        bool passed = false;
        std::vector<DeviceBufferSetting> results;
        std::wstring access_error;
        {
          std::lock_guard lock(app->probe_job->mutex);
          progress_text = app->probe_job->latest_message;
          done = app->probe_job->done.load();
          passed = app->probe_job->passed.load();
          if (done) {
            results = app->probe_job->result_device_buffers;
            access_error = app->probe_job->access_error;
          }
        }
        if (!progress_text.empty() && progress_text != app->last_probe_message) {
          app->last_probe_message = progress_text;
          append_log(*app, progress_text);
        }
        if (done) {
          KillTimer(window, kProbeTimer);
          set_probe_controls_enabled(*app, true);
          if (!access_error.empty()) {
            MessageBoxW(window, access_error.c_str(), L"设备被占用", MB_OK | MB_ICONWARNING);
            return 0;
          }
          for (const auto& result : results) {
            const auto found = std::find_if(app->devices.begin(), app->devices.end(),
                [&](const auto& device) { return same_text(device.id, result.device_id); });
            if (found == app->devices.end()) continue;
            found->buffer_frames = result.buffer_frames;
            update_device_group_header(*app,
                static_cast<size_t>(std::distance(app->devices.begin(), found)));
          }
          update_current_device(*app);
          update_latency_text(*app);
          append_log(*app, passed
              ? L"逐设备缓冲检测完成：每台设备已分别选取短时探测通过的最小档位。"
              : L"检测完成：未通过的设备使用 2048 帧。请检查占用和 KS 驱动兼容性。");
        }
        return 0;
      }
      break;
    case WM_CLOSE:
      close_or_hide(*app);
      return 0;
    case WM_DESTROY:
      app->tray_icon.remove();
      KillTimer(window, kTrayTimer);
      if (app->probe_job && !app->probe_job->done.load()) {
        app->probe_job->cancelled.store(true);
        KillTimer(window, kProbeTimer);
      }
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int run_config_ui(HINSTANCE instance, int show_command) {
  int argument_count{};
  auto* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  bool tray_mode = false;
  for (int index = 1; arguments && index < argument_count; ++index)
    if (std::wcscmp(arguments[index], L"--tray") == 0) tray_mode = true;
  if (arguments) LocalFree(arguments);
  config_instance::Instance single_instance;
  const int claim = single_instance.claim_or_activate(config_instance::kMutexName, kWindowClass,
      tray_mode ? config_tray::kStatusMessage : config_instance::kActivateMessage, !tray_mode);
  if (claim <= 0) return claim < 0 ? 1 : 0;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  Gdiplus::GdiplusStartupInput graphics_input;
  ULONG_PTR graphics_token{};
  if (Gdiplus::GdiplusStartup(&graphics_token, &graphics_input, nullptr) != Gdiplus::Ok) return 1;
  struct GraphicsLifetime {
    ULONG_PTR token;
    ~GraphicsLifetime() { Gdiplus::GdiplusShutdown(token); }
  } graphics_lifetime{graphics_token};
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_LINK_CLASS};
  InitCommonControlsEx(&controls);

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(config_tray::kIconResource));
  window_class.hIconSm = window_class.hIcon;
  window_class.hbrBackground = nullptr;
  window_class.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&window_class)) return 1;

  AppState app;
  app.tray_background = tray_mode;
  app.theme.set_dpi(GetDpiForSystem());
  POINT cursor{};
  GetCursorPos(&cursor);
  const auto work = monitor_work_area(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY));
  auto initial = fit_window_to_work_area(
      {work.left + app.theme.px(24), work.top + app.theme.px(24),
       work.left + app.theme.px(1004), work.top + app.theme.px(684)}, work, app.theme.px(8));
  HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, kWindowClass, L"Aggregate Audio · 音频设备配置",
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, initial.left, initial.top,
      initial.right - initial.left, initial.bottom - initial.top,
      nullptr, nullptr, instance, &app);
  if (!window) return 1;
  // Creation may select a monitor with a different DPI from the system default.
  initial = fit_window_to_work_area(
      {work.left + app.theme.px(24), work.top + app.theme.px(24),
       work.left + app.theme.px(1004), work.top + app.theme.px(684)}, work, app.theme.px(8));
  SetWindowPos(window, nullptr, initial.left, initial.top, initial.right - initial.left,
      initial.bottom - initial.top, SWP_NOZORDER | SWP_NOACTIVATE);
  const COLORREF caption_color = config_theme::kCanvas;
  DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption_color, sizeof(caption_color));
  const auto corner = DWMWCP_ROUND;
  DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
  ShowWindow(window, tray_mode ? SW_HIDE : show_command);
  UpdateWindow(window);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(window, &message)) continue;
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}
