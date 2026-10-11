#pragma once
#include <cstddef>
#include <cstdint>

namespace local_stop {
struct Status {
  bool ready, active;
  uint32_t detections, droppedFrames, inferenceUs;
  float lastScore;
  char error[96];
};
// Initialize after wake_word::begin() so its shared inference lock exists.
void begin();
// Nonblocking producer; same right-channel 32-bit I2S input as wake/speech.
void submit(const int32_t* stereo, size_t count);
// Main control loop only; detection dispatch uses the normal global Stop path.
void poll();
// Global Stop invalidates in-flight audio/results. Do not call this for each
// held manual-drive lease refresh, or no complete keyword can be assembled.
void cancel();
void audioFault();
Status snapshot();
}  // namespace local_stop
