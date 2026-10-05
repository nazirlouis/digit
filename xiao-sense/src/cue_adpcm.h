#pragma once
#include <cstdint>
#include <cstddef>

// IMA/DVI ADPCM, high nibble first; 64-sample blocks reset transient error.
class CueDecoder {
 public:
  int16_t next(const uint8_t* data, size_t sample) {
    const auto* block = data + (sample / 64) * 36;
    const size_t position = sample % 64;
    if (!position) {
      predictor_ = static_cast<int16_t>(block[0] | (block[1] << 8));
      index_ = block[2];
      return static_cast<int16_t>(predictor_);
    }
    const size_t encoded = position - 1;
    const unsigned code = encoded & 1 ? block[4 + encoded / 2] & 15 : block[4 + encoded / 2] >> 4;
    const int step = steps[index_];
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    predictor_ += code & 8 ? -delta : delta;
    if (predictor_ > 32767) predictor_ = 32767;
    if (predictor_ < -32768) predictor_ = -32768;
    index_ += changes[code & 7];
    if (index_ > 88) index_ = 88;
    if (index_ < 0) index_ = 0;
    return static_cast<int16_t>(predictor_);
  }
 private:
  int predictor_ = 0, index_ = 0;
  inline static constexpr int changes[] = {-1,-1,-1,-1,2,4,6,8};
  inline static constexpr int steps[] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
    73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,
    408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,
    1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
    5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,
    18500,20350,22385,24623,27086,29794,32767
  };
};
