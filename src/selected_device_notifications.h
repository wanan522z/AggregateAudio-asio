#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace selected_audio_notifications {
inline std::wstring normalize_path(std::wstring path) {
  if (path.starts_with(L"\\??\\")) path.replace(0, 4, L"\\\\?\\");
  if (const auto prefix = path.find(L"\\\\?\\"); prefix != std::wstring::npos) path = path.substr(prefix);
  std::transform(path.begin(), path.end(), path.begin(), towlower);
  const auto begin = path.find(L"#{");
  if (begin != std::wstring::npos) {
    const auto end = path.find(L'}', begin);
    if (end != std::wstring::npos) {
      const auto category = path.substr(begin + 1, end - begin);
      if (category == L"{6994ad04-93ef-11d0-a3cc-00a0c9223196}" ||
          category == L"{65e8773d-8f56-11d0-a3b9-00a0c9223196}" ||
          category == L"{65e8773e-8f56-11d0-a3b9-00a0c9223196}")
        path.replace(begin + 1, end - begin, L"{audio-filter}");
    }
  }
  return path;
}
inline bool matches(const std::wstring& first, const std::wstring& second) {
  const auto left = normalize_path(first), right = normalize_path(second);
  const auto length = std::min(left.size(), right.size());
  return length && left.compare(0, length, right, 0, length) == 0 &&
      (left.size() == right.size() || (left.size() > length && left[length] == L'\\') ||
       (right.size() > length && right[length] == L'\\'));
}

// This client owns its enumerator reference and notification state. No callback
// refers to the driver or its audio engine, including during unregistration.
class Client final : public IMMNotificationClient {
 public:
  Client(IMMDeviceEnumerator* enumerator, std::vector<std::wstring> paths)
      : enumerator_(enumerator), paths_(std::move(paths)) { enumerator_->AddRef(); }
  ~Client() { enumerator_->Release(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** target) override {
    if (!target) return E_POINTER;
    *target = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
      *target = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto count = --references_;
    if (!count) delete this;
    return count;
  }
  bool include(LPCWSTR id) {
    if (!id) return false;
    {
      std::lock_guard lock(mutex_);
      if (selected_.contains(id)) return true;
    }
    IMMDevice* device{};
    if (FAILED(enumerator_->GetDevice(id, &device))) return false;
    IPropertyStore* properties{};
    bool selected = false;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
      constexpr PROPERTYKEY ks_path{{0x233164c8, 0x1b2c, 0x4c7d, {0xbc, 0x68, 0xb6, 0x71, 0x68, 0x7a, 0x25, 0x67}}, 1};
      PROPVARIANT path{};
      if (SUCCEEDED(properties->GetValue(ks_path, &path)) && path.vt == VT_LPWSTR && path.pwszVal) {
        selected = std::any_of(paths_.begin(), paths_.end(), [&](const auto& value) { return matches(value, path.pwszVal); });
      }
      PropVariantClear(&path);
      properties->Release();
    }
    device->Release();
    if (selected) { std::lock_guard lock(mutex_); selected_.insert(id); }
    return selected;
  }
  void seed() {
    IMMDeviceCollection* collection{};
    if (FAILED(enumerator_->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, &collection))) return;
    UINT count{};
    collection->GetCount(&count);
    for (UINT index = 0; index < count; ++index) {
      IMMDevice* device{};
      if (FAILED(collection->Item(index, &device))) continue;
      LPWSTR id{};
      if (SUCCEEDED(device->GetId(&id))) { include(id); CoTaskMemFree(id); }
      device->Release();
    }
    collection->Release();
  }
  bool consume() { return changed_.exchange(false, std::memory_order_acq_rel); }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD) override { mark(id); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override { mark(id); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override { mark(id); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY key) override {
    constexpr PROPERTYKEY device_format{{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};
    constexpr PROPERTYKEY oem_format{{0xe4870e26, 0x3cc5, 0x4cd2, {0xba, 0x46, 0xca, 0x0a, 0x9a, 0x70, 0xed, 0x04}}, 3};
    if (IsEqualPropertyKey(key, device_format) || IsEqualPropertyKey(key, oem_format)) mark(id);
    return S_OK;
  }
 private:
  void mark(LPCWSTR id) noexcept {
    try { if (include(id)) changed_.store(true, std::memory_order_release); } catch (...) {}
  }
  std::atomic<ULONG> references_{1};
  IMMDeviceEnumerator* enumerator_{};
  std::vector<std::wstring> paths_;
  std::mutex mutex_;
  std::unordered_set<std::wstring> selected_;
  std::atomic<bool> changed_{};
};

class Watcher {
 public:
  ~Watcher() { reset(); }
  void reset() noexcept {
    if (client_) { enumerator_->UnregisterEndpointNotificationCallback(client_); client_->Release(); client_ = nullptr; }
    if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
  }
  void initialize(std::vector<std::wstring> paths) {
    reset();
    if (paths.empty() || FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&enumerator_)))) return;
    client_ = new Client(enumerator_, std::move(paths));
    client_->seed();
    if (FAILED(enumerator_->RegisterEndpointNotificationCallback(client_))) reset();
  }
  bool consume() { return client_ && client_->consume(); }
 private:
  IMMDeviceEnumerator* enumerator_{};
  Client* client_{};
};
} // namespace selected_audio_notifications
