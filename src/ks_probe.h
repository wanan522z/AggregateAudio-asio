#pragma once

#include "device_catalog.h"

struct EndpointProbeResult {
  std::wstring endpoint_name;
  unsigned requested_buffer_frames{};
  unsigned actual_buffer_frames{};
  unsigned actual_sample_rate{};
  unsigned callback_count{};
  unsigned timeout_count{};
  long hresult{};
  bool stable{};
  bool wave_rt_mode{};
};

// Briefly opens one selected KS pin, streams silence/discards capture data,
// and closes the pin. This calibrates basic open/stream viability, not DAW latency.
EndpointProbeResult probe_endpoint(const AudioEndpoint& endpoint,
                                   unsigned sample_rate,
                                   unsigned buffer_frames,
                                   unsigned duration_ms = 1000);
