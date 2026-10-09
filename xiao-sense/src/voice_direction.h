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
// on/off persist the preference; reset retains the fixed mounting reference.
bool configure(const char* action);
// Captures one fresh, stable bearing relative to fixed front (positive = left).
bool target(int& degrees, uint32_t& generation, const char** reason = nullptr,
            bool* retryable = nullptr);
bool accepts(uint32_t generation);
}
