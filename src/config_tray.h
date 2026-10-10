#pragma once

#include "driver_telemetry.h"
#include <shellapi.h>
#include <tlhelp32.h>
#include <vector>

namespace config_tray {
inline constexpr UINT kMessage = WM_APP + 29;
inline constexpr UINT kStatusMessage = WM_APP + 28;
inline constexpr UINT kDeviceRefreshMessage = WM_APP + 30;
inline constexpr wchar_t kWindowClass[] = L"AggregateAudioConfigWindow";
inline constexpr UINT kIconResource = 101;

inline std::vector<DriverTelemetrySnapshot> running_hosts() {
  std::vector<DriverTelemetrySnapshot> result;
  const auto processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (processes == INVALID_HANDLE_VALUE) return result;
  PROCESSENTRY32W process{sizeof(process)};
  for (BOOL available = Process32FirstW(processes, &process); available; available = Process32NextW(processes, &process)) {
    const auto name = driver_telemetry_name(process.th32ProcessID);
    const auto mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!mapping) continue;
    const auto* shared = static_cast<const SharedDriverTelemetry*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(SharedDriverTelemetry)));
    DriverTelemetrySnapshot snapshot;
    if (read_driver_telemetry(shared, snapshot) && snapshot.running && snapshot.process_id == process.th32ProcessID)
      result.push_back(snapshot);
    if (shared) UnmapViewOfFile(shared);
    CloseHandle(mapping);
  }
  CloseHandle(processes);
  return result;
}

class Icon {
 public:
  using Notify = BOOL (WINAPI*)(DWORD, PNOTIFYICONDATAW);
  explicit Icon(Notify notify = Shell_NotifyIconW) : notify_(notify) {}
  ~Icon() { remove(); }
  void update(HWND window, HICON icon, const std::vector<DriverTelemetrySnapshot>& hosts) {
    if (hosts.empty()) { remove(); return; }
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kMessage;
    data.hIcon = icon;
    const auto& host = hosts.front();
    std::swprintf(data.szTip, std::size(data.szTip), L"Aggregate Audio · ASIO 运行中\n%u Hz · %u 帧 · %zu 个宿主",
        host.sample_rate, host.buffer_frames, hosts.size());
    if (notify_(present_ ? NIM_MODIFY : NIM_ADD, &data)) {
      if (!present_) {
        data.uVersion = NOTIFYICON_VERSION_4;
        notify_(NIM_SETVERSION, &data);
      }
      present_ = true;
      window_ = window;
    } else present_ = false;
  }
  void explorer_restarted() noexcept { present_ = false; }
  bool present() const noexcept { return present_; }
  void remove() noexcept {
    if (!present_) return;
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window_; data.uID = 1;
    notify_(NIM_DELETE, &data);
    present_ = false;
  }
 private:
  Notify notify_;
  bool present_{};
  HWND window_{};
};
} // namespace config_tray
