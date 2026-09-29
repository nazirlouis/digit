#pragma once

#include <stdint.h>

namespace person_follow {

struct Status {
  bool enabled;
  const char* phase;
  int targetX;
  int errorPixels;
  uint8_t scorePercent;
};

bool setEnabled(bool enabled);
bool heartbeat();
void poll();
Status snapshot();

}  // namespace person_follow
