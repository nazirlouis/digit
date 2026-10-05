#pragma once
#include <cmath>
#include <cstdint>

// Versioned NVS record. A new installation opts into voice turning; actual
// motion still requires direction calibration, floor readiness and a fresh cue.
struct VoiceSettings {
  uint32_t version = 1;
  float front = 0;
  int32_t orientation = 1;
  uint8_t frontSet = 0, calibrated = 0, enabled = 1;
  bool valid() const {
    return version == 1 && std::isfinite(front) && front >= 0 && front < 360 &&
           (orientation == 1 || orientation == -1) && frontSet <= 1 &&
           calibrated <= frontSet && enabled <= 1;
  }
};
