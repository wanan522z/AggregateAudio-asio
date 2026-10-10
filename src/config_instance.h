#pragma once

#include <windows.h>

namespace config_instance {

inline constexpr wchar_t kMutexName[] = L"Local\\AggregateAudio.Config.Singleton.v1";
inline constexpr UINT kActivateMessage = WM_APP + 27;

// The session-local mutex covers startup as well as the complete window lifetime.
// A second process only posts an activation request, then exits.
class Instance {
 public:
  ~Instance() {
    if (owned_) ReleaseMutex(mutex_);
    if (mutex_) CloseHandle(mutex_);
  }
  Instance() = default;
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;

  // 1: start the UI; 0: existing UI notified; -1: startup failed.
  int claim_or_activate(const wchar_t* mutex_name, const wchar_t* window_class,
                        UINT message = kActivateMessage, bool foreground = true) {
    mutex_ = CreateMutexW(nullptr, FALSE, mutex_name);
    if (!mutex_) return -1;
    const auto deadline = GetTickCount64() + 10000;
    do {
      const auto result = WaitForSingleObject(mutex_, 0);
      if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
        owned_ = true;
        return 1;
      }
      if (result != WAIT_TIMEOUT) return -1;
      if (const auto window = FindWindowW(window_class, nullptr)) {
        DWORD process{};
        GetWindowThreadProcessId(window, &process);
        if (foreground) AllowSetForegroundWindow(process);
        if (PostMessageW(window, message, 0, 0)) return 0;
      }
      // Retry if the owning process is still creating its first window, or exits.
      Sleep(50);
    } while (GetTickCount64() < deadline);
    return -1;
  }

 private:
  HANDLE mutex_{};
  bool owned_{};
};

} // namespace config_instance
