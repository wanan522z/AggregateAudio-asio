#pragma once

#include "device_catalog.h"

#include <windows.h>
#include <winioctl.h>
#include <ks.h>
#include <cstdint>

namespace ks_device_access {

inline bool is_busy(long status) noexcept {
  return status == HRESULT_FROM_WIN32(ERROR_BUSY) ||
      status == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
      status == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION) ||
      status == HRESULT_FROM_WIN32(ERROR_DEVICE_IN_USE) ||
      status == HRESULT_FROM_WIN32(ERROR_PIPE_BUSY);
}

inline constexpr wchar_t kBusyMessage[] =
    L"设备被占用，请释放后再试。\n\n请停止使用该设备的 DAW、播放器、录音或其他音频程序，再重新尝试。";

inline std::wstring reservation_name(const std::wstring& endpoint_id) {
  uint64_t hash = 14695981039346656037ULL;
  for (const wchar_t value : endpoint_id) {
    hash ^= static_cast<uint16_t>(value);
    hash *= 1099511628211ULL;
  }
  return L"Local\\AggregateAudio.KsPin." + std::to_wstring(hash);
}

// Serialize our own DAW/configurator instances even when a driver omits counts.
// A semaphore can be released by the shutdown thread, unlike an owned mutex.
class PinReservation {
 public:
  ~PinReservation() {
    if (handle_) {
      ReleaseSemaphore(handle_, 1, nullptr);
      CloseHandle(handle_);
    }
  }
  PinReservation() = default;
  PinReservation(const PinReservation&) = delete;
  PinReservation& operator=(const PinReservation&) = delete;
  HRESULT acquire(const std::wstring& endpoint_id) {
    if (handle_) return S_OK;
    const auto name = reservation_name(endpoint_id);
    const HANDLE handle = CreateSemaphoreW(nullptr, 1, 1, name.c_str());
    if (!handle) return HRESULT_FROM_WIN32(GetLastError());
    const DWORD wait = WaitForSingleObject(handle, 0);
    if (wait == WAIT_OBJECT_0) {
      handle_ = handle;
      return S_OK;
    }
    const HRESULT status = wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_BUSY)
        : HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(handle);
    return status;
  }
 private:
  HANDLE handle_{};
};

// Read factory counts without creating or starting a streaming pin. Some older
// drivers omit these properties; KsCreatePin still has to handle that race.
inline HRESULT check_pin(HANDLE filter, unsigned pin_id) noexcept {
  for (const ULONG id : {KSPROPERTY_PIN_GLOBALCINSTANCES, KSPROPERTY_PIN_CINSTANCES}) {
    KSP_PIN property{};
    property.Property.Set = KSPROPSETID_Pin;
    property.Property.Id = id;
    property.Property.Flags = KSPROPERTY_TYPE_GET;
    property.PinId = pin_id;
    KSPIN_CINSTANCES instances{};
    ULONG returned{};
    if (!DeviceIoControl(filter, IOCTL_KS_PROPERTY, &property, sizeof(property),
                         &instances, sizeof(instances), &returned, nullptr)) {
      const DWORD error = GetLastError();
      // GLOBALCINSTANCES is optional. Thesycon/E2x2 reports ERROR_NOT_FOUND
      // for that property while implementing local CINSTANCES correctly.
      if (error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION ||
          error == ERROR_NOT_FOUND || error == ERROR_SET_NOT_FOUND ||
          error == ERROR_CALL_NOT_IMPLEMENTED || error == ERROR_INVALID_PARAMETER) continue;
      return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    }
    if (returned < sizeof(instances)) return E_UNEXPECTED;
    // Existing instances alone do not mean exclusive occupation: a factory can
    // legitimately support multiple clients. Only a known full capacity blocks.
    if (instances.PossibleCount && instances.CurrentCount >= instances.PossibleCount)
      return HRESULT_FROM_WIN32(ERROR_BUSY);
  }
  return S_OK;
}

inline HRESULT check_endpoint(const AudioEndpoint& endpoint) {
  const auto name = reservation_name(endpoint.id);
  const HANDLE reservation = OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, name.c_str());
  if (reservation) {
    const DWORD wait = WaitForSingleObject(reservation, 0);
    const HRESULT status = wait == WAIT_OBJECT_0 ? S_OK
        : wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_BUSY) : HRESULT_FROM_WIN32(GetLastError());
    if (wait == WAIT_OBJECT_0) ReleaseSemaphore(reservation, 1, nullptr);
    CloseHandle(reservation);
    if (FAILED(status)) return status;
  } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  auto check = [](const std::wstring& path, unsigned pin_id) {
    const HANDLE filter = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (filter == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    const HRESULT status = check_pin(filter, pin_id);
    CloseHandle(filter);
    return status;
  };
  if (endpoint.ks_pin_candidates.empty())
    return check(endpoint.ks_filter_path, endpoint.ks_pin_id);
  HRESULT last_status = E_FAIL;
  for (const auto& candidate : endpoint.ks_pin_candidates) {
    last_status = check(candidate.filter_path, candidate.pin_id);
    // Aliases can expose the same hardware. Do not bypass an occupied factory.
    if (SUCCEEDED(last_status) || is_busy(last_status)) return last_status;
  }
  return last_status;
}

} // namespace ks_device_access
