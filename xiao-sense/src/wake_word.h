#pragma once
#include <cstddef>
#include <cstdint>

namespace wake_word {
constexpr size_t kFrameSamples = 160;  // 10 ms at 16 kHz
struct Status {
  bool ready;
  bool pausedForVision;
  bool reacting;
  uint32_t detections;
  uint32_t lastDetectionMs;
  uint32_t droppedFrames;
  uint32_t inferenceUs;
  uint16_t micPeak;
  float lastScore;
  char error[96];
};
void begin();
// Audio producer never waits for inference. Stereo input is signed 32-bit I2S.
void submit(const int32_t* stereo, size_t frames);
void audioFault();
void setVisionEnabled(bool enabled);
// Serialize both models, including a last in-flight vision result at shutdown.
void beginVisionInference();
void endVisionInference();
void poll();
bool testReaction();
Status snapshot();
}  // namespace wake_word
