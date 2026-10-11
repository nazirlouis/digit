#pragma once
#include <cstddef>
#include <cstdint>

// Capture metadata follows audio through framing, separately from the robot's
// control generation. Session zero denotes sound outside a command window.
namespace speech_pipeline {
constexpr size_t kInputSamples = 160;
struct Frame {
  uint32_t stream, session, capturedAt;
  uint16_t count;
  int16_t samples[kInputSamples];
};
struct Tag { uint32_t session, capturedAt; };

inline int16_t mono(int32_t right) { return static_cast<int16_t>(right >> 16); }
inline bool current(const Tag& tag, uint32_t session, bool listening, uint32_t now) {
  return listening && tag.session != 0 && tag.session == session && now - tag.capturedAt <= 1000;
}

class Assembler {
 public:
  Assembler(int16_t* samples, int chunk) : samples_(samples), chunk_(chunk) {}
  bool append(const Frame& frame, unsigned offset, Tag& completed) {
    if (stream_ != frame.stream) { stream_ = frame.stream; filled_ = 0; }
    if (!filled_) tag_ = {frame.session, frame.capturedAt};
    if (tag_.session != frame.session) tag_.session = 0; // Discard a mixed wake/Stop chunk.
    samples_[filled_++] = frame.samples[offset];
    if (filled_ != chunk_) return false;
    filled_ = 0; completed = tag_; return true;
  }
 private:
  int16_t* samples_;
  int chunk_, filled_ = 0;
  uint32_t stream_ = 0;
  Tag tag_{};
};
} // namespace speech_pipeline
