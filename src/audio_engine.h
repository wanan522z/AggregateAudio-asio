#pragma once

#include "device_catalog.h"
#include "settings.h"

#include <memory>
#include <cstdint>
#include <string>
#include <vector>

struct StreamStatistics {
  unsigned sample_rate{};
  unsigned buffer_frames{};
  unsigned channels{};
  unsigned packets{};
  unsigned underruns{};
  unsigned buffered_frames{};
  unsigned flags{}; // input=1, WaveRT=2, running=4
  long status{};
  uint64_t received_frames{};
  uint64_t dropped_frames{};
  uint64_t rendered_frames{};
  uint64_t missing_frames{}; // Input frames replaced by silence after priming.
  uint64_t discarded_frames{}; // Stale ring frames skipped after a stall.
};

class EndpointAudioStream {
 public:
  ~EndpointAudioStream();
  EndpointAudioStream(const EndpointAudioStream&) = delete;
  EndpointAudioStream& operator=(const EndpointAudioStream&) = delete;
  bool initialize(std::string& error);
  bool wait_initialized(std::string& error);
  bool start(std::string& error);
  bool request_start() noexcept;
  bool wait_started(unsigned timeout_ms, std::string& error) noexcept;
  void stop() noexcept;
  void request_stop() noexcept;
  void wait_stopped(unsigned timeout_ms) noexcept;
  bool is_input() const noexcept;
  unsigned channels() const noexcept;
  const std::wstring& id() const noexcept;
  const std::wstring& name() const noexcept;
  void read_input(float* destination, unsigned frames) noexcept;
  void push_output(const float* source, unsigned frames) noexcept;
  void set_asio_sample_rate(unsigned rate) noexcept;
  void set_asio_buffer_frames(unsigned frames) noexcept;
  unsigned stream_latency_frames() const noexcept;
  bool wave_rt_mode() const noexcept;
  bool healthy() const noexcept;
  unsigned actual_sample_rate() const noexcept;
  unsigned actual_buffer_frames() const noexcept;
  unsigned stream_buffer_latency_frames() const noexcept;
  unsigned processed_packets() const noexcept;
  unsigned render_underruns() const noexcept;
  long last_status() const noexcept;
  StreamStatistics statistics() const noexcept;

 private:
  friend class AudioEngine;
  EndpointAudioStream(AudioEndpoint endpoint, unsigned sample_rate, unsigned buffer_frames);
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Owns one user-mode Kernel Streaming pin per selected endpoint. Endpoint
// streams run on their own worker threads; the ASIO callback only touches
// preallocated lock-free audio rings.
class AudioEngine {
 public:
  AudioEngine() = default;
  ~AudioEngine();
  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  bool initialize(const AudioSettings& settings, const std::vector<AudioEndpoint>& catalog,
                  std::string& error);
  bool start(std::string& error);
  void stop() noexcept;
  long last_status() const noexcept { return last_status_; }
  void set_asio_sample_rate(unsigned rate) noexcept;
  void set_asio_buffer_frames(unsigned frames) noexcept;
  unsigned approximate_input_latency_frames() const noexcept;
  unsigned approximate_output_latency_frames() const noexcept;

  unsigned sample_rate() const noexcept { return sample_rate_; }
  unsigned buffer_frames() const noexcept { return buffer_frames_; }
  size_t input_stream_count() const noexcept { return inputs_.size(); }
  size_t output_stream_count() const noexcept { return outputs_.size(); }
  EndpointAudioStream* input_stream(size_t index) const noexcept;
  EndpointAudioStream* output_stream(size_t index) const noexcept;

 private:
  unsigned sample_rate_{48000};
  unsigned buffer_frames_{128};
  long last_status_{};
  std::vector<std::unique_ptr<EndpointAudioStream>> inputs_;
  std::vector<std::unique_ptr<EndpointAudioStream>> outputs_;
};
