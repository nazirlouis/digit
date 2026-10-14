#pragma once
#include <cstddef>
#include <cstdint>

// Streaming 24 -> 16 kHz conversion. Preserve triplets across network chunks.
// The two weighted averages keep constant signals and reduce high-frequency
// energy before downsampling. All arithmetic stays in signed 32-bit range.
namespace live_audio {
// Gentle 70 Hz ring modulation at the speaker's 16 kHz rate: 85% original
// speech + 15% modulated speech. The convex mix cannot exceed the input peak.
// No delayed samples or allocation; silence stays silent and Stop has no tail.
class RobotVoice {
 public:
  int16_t process(int16_t sample) {
    const int32_t carrier = kSine[(uint32_t(phase_) * 64U) / 16000U];
    phase_ = static_cast<uint16_t>(phase_ + 70U);
    if (phase_ >= 16000U) phase_ -= 16000U;
    const int32_t wet = (int32_t(sample) * carrier) / 32768;
    return static_cast<int16_t>((int32_t(sample) * 85 + wet * 15) / 100);
  }
  void reset() { phase_ = 0; }

 private:
  uint16_t phase_ = 0;
  inline static constexpr int16_t kSine[64] = {
    0, 3212, 6393, 9512, 12540, 15447, 18205, 20788,
    23170, 25330, 27246, 28899, 30274, 31357, 32138, 32610,
    32767, 32610, 32138, 31357, 30274, 28899, 27246, 25330,
    23170, 20788, 18205, 15447, 12540, 9512, 6393, 3212,
    0, -3212, -6393, -9512, -12540, -15447, -18205, -20788,
    -23170, -25330, -27246, -28899, -30274, -31357, -32138, -32610,
    -32767, -32610, -32138, -31357, -30274, -28899, -27246, -25330,
    -23170, -20788, -18205, -15447, -12540, -9512, -6393, -3212
  };
};

class Resampler {
 public:
  template<typename Write>
  bool append(const int16_t* input, size_t count, Write write) {
    for (size_t i = 0; i < count; ++i) {
      triplet_[used_++] = input[i];
      if (used_ == 3) {
        const int16_t output[2] = {
          static_cast<int16_t>((2 * int32_t(triplet_[0]) + triplet_[1]) / 3),
          static_cast<int16_t>((int32_t(triplet_[1]) + 2 * int32_t(triplet_[2])) / 3)};
        used_ = 0;
        if (!write(output, 2)) return false;
      }
    }
    return true;
  }
 private:
  int16_t triplet_[3]{};
  size_t used_ = 0;
};
}  // namespace live_audio
