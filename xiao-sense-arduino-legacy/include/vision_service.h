#pragma once

#include <stdint.h>

namespace vision_service {

struct Status {
  bool ready;
  bool enabled;
  bool hasResult;
  bool personDetected;
  uint8_t personScorePercent;
  uint32_t resultAgeMs;
  uint32_t inferenceMs;
  char error[80];
};

void begin();
bool setEnabled(bool enabled);
Status snapshot();

}  // namespace vision_service
