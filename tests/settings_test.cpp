#include "../src/settings.cpp"
#include <cstdio>

int main() {
  const auto path = std::filesystem::current_path() / (L"settings-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
  {
    std::ofstream profile(path, std::ios::binary);
    profile << "[Engine]\nConfigured=1\nSampleRate=48000\nBufferFrames=256\n"
        "[Devices]\nInputIds=hardware-in;virtual:old:in\nOutputIds=hardware-out;virtual:old:out\n"
        "DeviceBuffers=hardware=128;virtual:old=512\n"
        "[Channels]\nConfigured=1\nInputKeys=hardware-in:ch=0;virtual:old:in:ch=0\n"
        "OutputKeys=hardware-out:ch=1;virtual:old:out:ch=0\n"
        "[VirtualCables]\nCount=1\n[VirtualCable0]\nId=old\nName=Old cable\nChannels=2\n"
        "[Unrelated]\nPreserved=42\n";
  }
  auto settings = load_profile(path);
  bool passed = settings.input_device_ids == std::vector<std::wstring>{L"hardware-in"} &&
      settings.output_device_ids == std::vector<std::wstring>{L"hardware-out"} &&
      settings.input_channel_keys == std::vector<std::wstring>{L"hardware-in:ch=0"} &&
      settings.output_channel_keys == std::vector<std::wstring>{L"hardware-out:ch=1"} &&
      settings.device_buffers.size() == 1 && settings.device_buffers[0].buffer_frames == 128 &&
      settings.buffer_frames == 256 && settings.sample_rate == 48000 && settings.initialized;
  passed = passed && GetPrivateProfileIntW(L"VirtualCables", L"Count", 0, path.c_str()) == 1;
  passed = save_profile(path, settings) && passed;
  const auto reloaded = load_profile(path);
  passed = passed && reloaded.output_channel_keys == settings.output_channel_keys &&
      read_value(path, L"VirtualCables", L"Count").empty() && read_value(path, L"VirtualCable0", L"Id").empty() &&
      GetPrivateProfileIntW(L"Unrelated", L"Preserved", 0, path.c_str()) == 42;
  WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
  DeleteFileW(path.c_str());
  std::printf("Removed cable entries ignored and cleaned on save; hardware settings preserved: %s\n", passed ? "PASS" : "FAIL");
  return passed ? 0 : 1;
}
