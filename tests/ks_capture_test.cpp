#include "audio_engine.h"
#include <windows.h>
#include <winioctl.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace {
struct ReadRequest {
  HANDLE pin{};
  KSSTREAM_HEADER* header{};
  BYTE* data{};
  ULONG extent{};
  bool complete{};
  DWORD status{};
};
std::mutex requests_mutex;
std::unordered_map<LPOVERLAPPED, ReadRequest> requests;
std::deque<LPOVERLAPPED> queued;
std::atomic<bool> invalid_request{};
std::atomic<unsigned> cancelled{};
std::atomic<unsigned> direct_remaining{};
unsigned captured_frame{};

void fill_read(ReadRequest& request, unsigned frames) {
  if (request.header->Data != request.data || request.header->FrameExtent != request.extent ||
      request.header->DataUsed != 0 || frames * 2 * sizeof(float) > request.extent) invalid_request = true;
  auto* samples = reinterpret_cast<float*>(request.data);
  for (unsigned frame = 0; frame < frames; ++frame, ++captured_frame) {
    samples[frame * 2] = 0.05f + captured_frame * 0.00001f;
    samples[frame * 2 + 1] = -samples[frame * 2];
  }
  request.header->DataUsed = frames * 2 * sizeof(float);
  request.complete = true;
}
HANDLE WINAPI fake_create_file(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) {
  return CreateEventW(nullptr, FALSE, FALSE, nullptr);
}
DWORD WINAPI fake_create_pin(HANDLE, PKSPIN_CONNECT, ACCESS_MASK, PHANDLE pin) {
  *pin = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  return *pin ? ERROR_SUCCESS : GetLastError();
}
BOOL WINAPI fake_ioctl(HANDLE pin, DWORD code, LPVOID input, DWORD input_size,
                      LPVOID output, DWORD output_size, LPDWORD returned, LPOVERLAPPED io) {
  *returned = 0;
  if (code == IOCTL_KS_PROPERTY && input) {
    const auto& property = *static_cast<KSPROPERTY*>(input);
    if (property.Set == KSPROPSETID_Connection && property.Id == KSPROPERTY_CONNECTION_STATE) return TRUE;
  }
  if (code != IOCTL_KS_READ_STREAM) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
  if (input || input_size || !output || output_size != sizeof(KSSTREAM_HEADER) || !io || !io->hEvent) {
    invalid_request = true; SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
  }
  auto* header = static_cast<KSSTREAM_HEADER*>(output);
  if (header->Size != sizeof(*header) || !header->Data || !header->FrameExtent || header->DataUsed ||
      header->PresentationTime.Numerator != 1 || header->PresentationTime.Denominator != 1) invalid_request = true;
  std::lock_guard lock(requests_mutex);
  if (requests.contains(io)) invalid_request = true;
  ReadRequest request{pin, header, static_cast<BYTE*>(header->Data), header->FrameExtent};
  if (direct_remaining > 0) {
    direct_remaining--;
    fill_read(request, 64);
    requests[io] = request;
    *returned = sizeof(*header);
    return TRUE;
  }
  requests[io] = request;
  queued.push_back(io);
  SetLastError(ERROR_IO_PENDING);
  return FALSE;
}
BOOL WINAPI fake_cancel(HANDLE pin, LPOVERLAPPED request) {
  std::lock_guard lock(requests_mutex);
  for (auto& [io, read] : requests) {
    if (read.pin != pin || (request && request != io) || read.complete) continue;
    read.complete = true;
    read.status = ERROR_OPERATION_ABORTED;
    std::erase(queued, io);
    cancelled++;
    SetEvent(io->hEvent);
  }
  return TRUE;
}
BOOL WINAPI fake_result(HANDLE, LPOVERLAPPED io, LPDWORD returned, BOOL wait) {
  if (wait) WaitForSingleObject(io->hEvent, 1000);
  std::lock_guard lock(requests_mutex);
  const auto found = requests.find(io);
  if (found == requests.end() || !found->second.complete) {
    SetLastError(ERROR_IO_INCOMPLETE); return FALSE;
  }
  const DWORD status = found->second.status;
  *returned = sizeof(KSSTREAM_HEADER);
  requests.erase(found);
  SetLastError(status);
  return status == ERROR_SUCCESS;
}
}

#define CreateFileW fake_create_file
#define KsCreatePin fake_create_pin
#define DeviceIoControl fake_ioctl
#define CancelIoEx fake_cancel
#define GetOverlappedResult fake_result
#include "../src/audio_engine.cpp"
#undef CreateFileW
#undef KsCreatePin
#undef DeviceIoControl
#undef CancelIoEx
#undef GetOverlappedResult

unsigned device_buffer_frames(const AudioSettings& settings, const std::wstring&) { return settings.buffer_frames; }

namespace {
bool wait_queue() {
  const auto deadline = GetTickCount64() + 1000;
  do {
    { std::lock_guard lock(requests_mutex); if (queued.size() == 2) return true; }
    Sleep(1);
  } while (GetTickCount64() < deadline);
  return false;
}
bool wait_packets(EndpointAudioStream* stream, unsigned count) {
  const auto deadline = GetTickCount64() + 1000;
  while (stream->processed_packets() < count && stream->healthy() && GetTickCount64() < deadline) Sleep(1);
  return stream->processed_packets() >= count;
}
bool complete_next(unsigned frames, DWORD status = ERROR_SUCCESS) {
  std::lock_guard lock(requests_mutex);
  if (queued.size() != 2) return false;
  const auto io = queued.front(); queued.pop_front();
  auto& read = requests.at(io);
  fill_read(read, frames);
  read.status = status;
  SetEvent(io->hEvent);
  return true;
}
bool empty_requests() { std::lock_guard lock(requests_mutex); return requests.empty() && queued.empty(); }
}

int main() {
  AudioEndpoint input;
  input.id = input.name = L"capture-test";
  input.direction = Direction::input;
  input.channels = 2;
  input.native_sample_rates = {48000};
  AudioSettings settings;
  settings.buffer_frames = 128;
  settings.input_device_ids = {input.id};
  AudioEngine engine;
  std::string error;
  if (!engine.initialize(settings, {input}, error) || !engine.start(error)) return 1;
  auto* stream = engine.input_stream(0);
  bool passed = wait_queue();
  std::array<float, 256> samples{};
  stream->read_input(samples.data(), 128);
  passed = std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0; }) && passed;
  const std::array<unsigned, 4> initial_frames{128, 64, 0, 128};
  for (unsigned packet = 0; packet < initial_frames.size(); ++packet) {
    passed = complete_next(initial_frames[packet]) && passed;
    passed = wait_packets(stream, packet + 1) && wait_queue() && passed;
  }
  passed = stream->statistics().received_frames == 320 && stream->statistics().dropped_frames == 0 && passed;
  float previous = 0;
  bool first = true;
  const auto check_samples = [&] {
    for (unsigned frame = 0; frame < 128; ++frame) {
      const float sample = samples[frame * 2];
      if (sample < 0.05f || samples[frame * 2 + 1] != -sample) passed = false;
      if (!first && (sample - previous < 0.000009f || sample - previous > 0.000011f)) passed = false;
      first = false;
      previous = sample;
    }
  };
  stream->read_input(samples.data(), 128);
  check_samples();
  for (unsigned packet = 0; packet < 40; ++packet) {
    passed = complete_next(128) && passed;
    passed = wait_packets(stream, packet + 5) && wait_queue() && passed;
    stream->read_input(samples.data(), 128);
    check_samples();
  }
  auto stats = stream->statistics();
  passed = stats.received_frames == 5440 && stats.missing_frames == 0 && stats.underruns == 0 && !invalid_request && passed;
  std::printf("Queued capture, partial/empty packets and 41 continuous input blocks: %s\n", passed ? "PASS" : "FAIL");
  for (unsigned block = 0; block < 6; ++block) stream->read_input(samples.data(), 128);
  stats = stream->statistics();
  const bool starvation = stats.missing_frames > 0 && stats.underruns > 0;
  const auto before_stop = GetTickCount64();
  engine.stop();
  bool stopped = GetTickCount64() - before_stop < 1000 && empty_requests() && cancelled >= 2;
  direct_remaining = 1;
  captured_frame = 0;
  bool restarted = engine.start(error) && wait_packets(stream, 1) && wait_queue();
  stats = stream->statistics();
  restarted = stats.received_frames == 64 && stats.missing_frames == 0 && stats.underruns == 0 && restarted;
  stream->read_input(samples.data(), 128);
  restarted = std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0; }) && restarted;
  bool failed_read = complete_next(0, ERROR_DEVICE_NOT_CONNECTED);
  const auto deadline = GetTickCount64() + 1000;
  while (stream->healthy() && GetTickCount64() < deadline) Sleep(1);
  engine.stop();
  failed_read = !stream->healthy() && stream->last_status() == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) && empty_requests() && failed_read;
  passed = starvation && stopped && restarted && failed_read && !invalid_request && passed;
  std::printf("Capture starvation telemetry, cancellation, direct completion, restart and device loss: %s\n", passed ? "PASS" : "FAIL");
  return passed ? 0 : 1;
}
