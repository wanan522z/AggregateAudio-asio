#pragma once

#include "audio_engine.h"
#include <algorithm>
#include <cstdint>
#include <vector>

// Control-thread watchdog. Silence is valid audio: inspect stream completion
// counters and errors, never the amplitude of the samples.
class DeviceRecoveryWatchdog {
 public:
  bool observe(uint64_t now, uint64_t generation, uint64_t cycles,
               bool running, const std::vector<StreamStatistics>& streams, bool device_changed = false) {
    if (!running || streams.empty()) {
      history_.clear();
      generation_ = 0;
      return false;
    }
    if (generation != generation_ || history_.size() != streams.size()) {
      generation_ = generation;
      started_at_ = now;
      last_cycles_ = cycles;
      history_.clear();
      for (const auto& stream : streams) history_.push_back({stream, now, 0});
      healthy_since_ = 0;
      return false;
    }
    const bool scheduler_progress = cycles != last_cycles_;
    last_cycles_ = cycles;
    bool fault = device_changed;
    bool healthy = true;
    for (size_t index = 0; index < streams.size(); ++index) {
      const auto& stream = streams[index];
      auto& previous = history_[index];
      const bool progressed = stream.packets != previous.statistics.packets;
      if (progressed || !scheduler_progress) previous.last_progress = now;
      const bool bad_status = stream.status < 0 || !(stream.flags & 4u);
      if (bad_status && !previous.failed_since) previous.failed_since = now;
      if (!bad_status) previous.failed_since = 0;
      const bool parameters_changed = stream.sample_rate != previous.statistics.sample_rate ||
          stream.buffer_frames != previous.statistics.buffer_frames || stream.channels != previous.statistics.channels;
      const bool stalled = scheduler_progress && now - previous.last_progress >= 1500;
      const bool failed = previous.failed_since && now - previous.failed_since >= 500;
      fault |= parameters_changed || stalled || failed;
      healthy &= !bad_status && now - previous.last_progress < 750;
      previous.statistics = stream;
    }
    if (healthy && !fault) {
      if (!healthy_since_) healthy_since_ = now;
      if (now - healthy_since_ >= 2000) latched_ = false;
    } else healthy_since_ = 0;
    if (!fault || latched_ || now - started_at_ < 1000) return false;
    latched_ = true;
    return true;
  }

 private:
  struct History { StreamStatistics statistics; uint64_t last_progress; uint64_t failed_since; };
  std::vector<History> history_;
  uint64_t generation_{};
  uint64_t started_at_{};
  uint64_t last_cycles_{};
  uint64_t healthy_since_{};
  bool latched_{};
};
