#pragma once
#include <cstddef>
#include <cstdint>

// Streaming 24 -> 16 kHz conversion. Preserve triplets across network chunks.
// The two weighted averages keep constant signals and reduce high-frequency
// energy before downsampling. All arithmetic stays in signed 32-bit range.
namespace live_audio {
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
