#pragma once

#include <algorithm>
#include <cmath>

// All quantities are in the ring's sample rate. Allow one millisecond of
// scheduling jitter, and account for producer bursts separately from reads.
struct BridgeBufferPlan {
  unsigned required{};
  unsigned prime{};
  unsigned target{};
  unsigned limit{};
  unsigned recovery{};
};

inline BridgeBufferPlan bridge_buffer_plan(unsigned producer_frames,
                                           double consumer_frames,
                                           unsigned ring_rate) noexcept {
  const unsigned read = std::max(1u, static_cast<unsigned>(std::ceil(consumer_frames)));
  const unsigned required = static_cast<unsigned>(std::ceil(consumer_frames * 1.0015)) + 1;
  const unsigned jitter = std::max(8u, (ring_rate + 999) / 1000);
  const unsigned prime = std::max(producer_frames, required + jitter);
  const unsigned target = std::max(required, read + producer_frames / 2 + jitter);
  const unsigned limit = std::max(prime, target) + std::max(producer_frames, read) * 2;
  // Preserve two reads when catching up: queued packet completions can arrive
  // together after a stall, before the producer has supplied another block.
  const unsigned recovery = std::max({prime, target, required * 2});
  return {required, prime, target, limit, recovery};
}
