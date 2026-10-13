#include "ks_probe.h"

#include "audio_engine.h"
#include "settings.h"

#include <windows.h>

EndpointProbeResult probe_endpoint(const AudioEndpoint& endpoint,
                                   unsigned sample_rate,
                                   unsigned buffer_frames,
                                   unsigned duration_ms) {
  EndpointProbeResult result;
  result.endpoint_name = endpoint.name;
  result.requested_buffer_frames = buffer_frames;
  result.actual_buffer_frames = buffer_frames;
  result.actual_sample_rate = sample_rate;
  result.hresult = E_FAIL;

  AudioSettings settings;
  settings.sample_rate = sample_rate;
  settings.buffer_frames = buffer_frames;
  settings.initialized = true;
  if (endpoint.direction == Direction::input) settings.input_device_ids.push_back(endpoint.id);
  else settings.output_device_ids.push_back(endpoint.id);

  AudioEngine engine;
  std::string error;
  if (!engine.initialize(settings, {endpoint}, error)) {
    result.hresult = engine.last_status();
    return result;
  }
  auto* stream = endpoint.direction == Direction::input ? engine.input_stream(0)
                                                         : engine.output_stream(0);
  if (!stream) return result;
  result.actual_sample_rate = stream->actual_sample_rate();
  result.actual_buffer_frames = stream->actual_buffer_frames();
  result.wave_rt_mode = stream->wave_rt_mode();
  if (!engine.start(error)) {
    result.hresult = stream->last_status();
    engine.stop();
    return result;
  }

  Sleep(duration_ms);
  result.callback_count = stream->processed_packets();
  result.stable = stream->healthy() && result.callback_count > 0;
  result.hresult = stream->last_status();
  engine.stop();
  return result;
}
