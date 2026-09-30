#pragma once
#include <cstdint>

namespace voice_direction {
struct Status {
  bool available, stable, frontSet, calibrated, enabled;
  float degrees, relative;
  uint32_t ageMs, generation;
  uint8_t readResource, readCommand, readStatus;
  char source[16];
  char error[80];
};
void begin();
Status snapshot();
// front/left capture fresh speech without moving motors. on/off/reset.
bool configure(const char* action);
// Captures one fresh, stable, calibrated bearing (positive = robot left).
bool target(int& degrees, uint32_t& generation);
bool accepts(uint32_t generation);
}
