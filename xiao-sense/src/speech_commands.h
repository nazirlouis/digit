#pragma once
#include <cstddef>
#include <cstdint>
namespace speech_commands {
struct Status {
  bool ready; bool listening; uint32_t recognized; int lastCommand; float score; char error[96];
  uint32_t samples, chunks, inferenceUs; uint16_t micPeak;
  int candidateCommand; float candidateScore; char result[32];
  uint32_t feedChunks, fetchChunks, totalFeeds, totalFetches, droppedFrames;
  uint16_t modelPeak; char decoderText[128];
};
void begin();
void listen(uint32_t generation);
void cancel();
void audioFault();
void submit(const int32_t* stereo, size_t frames);
void poll();
Status snapshot();
// A copy of the latest complete window, taken from the exact detect() input.
// Caller frees samples with free(). This diagnostic never blocks recognition.
struct AudioCapture { int16_t* samples; size_t count; uint32_t session; };
AudioCapture copyAudio();
struct AudioTestStatus {
  bool busy, complete, accepted; uint8_t gain; uint32_t chunks;
  int decoderState, command; float score; char text[128];
};
// Ownership transfers only on success. Decoder-only: never dispatches actions.
bool testAudio(int16_t* samples, size_t count, uint8_t gain);
AudioTestStatus audioTestStatus();
}
