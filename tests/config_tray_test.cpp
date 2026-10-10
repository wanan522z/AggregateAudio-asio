#include "config_tray.h"
#include <cstdio>

namespace {
unsigned adds{}, modifies{}, deletes{}, versions{};
bool reject_modify{};
BOOL WINAPI fake_notify(DWORD operation, PNOTIFYICONDATAW data) {
  if (operation == NIM_ADD) ++adds;
  if (operation == NIM_MODIFY) { ++modifies; if (reject_modify) return FALSE; }
  if (operation == NIM_DELETE) ++deletes;
  if (operation == NIM_SETVERSION) ++versions;
  return data->uID == 1;
}
}
int main() {
  const auto icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, 0));
  bool passed = icon != nullptr;
  config_tray::Icon tray(fake_notify);
  DriverTelemetrySnapshot host; host.running = 1; host.process_id = 100; host.sample_rate = 48000; host.buffer_frames = 256;
  tray.update(nullptr, icon, {});
  passed &= adds == 0;
  tray.update(nullptr, icon, {host});
  tray.update(nullptr, icon, {host, host});
  passed &= tray.present() && adds == 1 && modifies == 1 && versions == 1;
  reject_modify = true;
  tray.update(nullptr, icon, {host});
  reject_modify = false;
  tray.update(nullptr, icon, {host});
  passed &= adds == 2 && tray.present();
  tray.explorer_restarted();
  tray.update(nullptr, icon, {host});
  passed &= adds == 3;
  tray.update(nullptr, icon, {});
  passed &= !tray.present() && deletes == 1;
  DestroyIcon(icon);
  std::printf("Embedded brand icon, one tray icon, multiple hosts, Explorer restart and stop cleanup: %s\n", passed ? "PASS" : "FAIL");
  return passed ? 0 : 1;
}
