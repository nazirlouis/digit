#pragma once
#include <cmath>
#include <cstdint>

namespace voice_mount {
inline constexpr float frontDegrees = 270;
inline constexpr float leftDegrees = 0;  // 360 degrees is the same bearing.
inline constexpr int orientation = 1;   // Increasing array bearing turns left.
}

// Preserve the NVS layout so existing on/off preferences survive the switch
// to a fixed mounting reference. Old sampled front/left values are ignored.
struct VoiceSettings {
  uint32_t version = 1;
  float front = voice_mount::frontDegrees;
  int32_t orientation = 1;
  uint8_t frontSet = 1, calibrated = 1, enabled = 1;
  bool valid() const {
    return version == 1 && std::isfinite(front) && front >= 0 && front < 360 &&
           (orientation == 1 || orientation == -1) && frontSet <= 1 &&
           calibrated <= frontSet && enabled <= 1;
  }
};
