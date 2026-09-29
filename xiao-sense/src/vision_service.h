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
  uint32_t resultTimestampMs;
  uint32_t inferenceMs;
  uint32_t decodeMs;
  uint32_t totalMs;
  uint32_t intervalMs;
  char error[80];
  struct Box {
    int x1, y1, x2, y2;
    uint8_t scorePercent;
  } boxes[5];
  uint8_t boxCount;
  uint16_t imageWidth;
  uint16_t imageHeight;
};

void begin();
bool setEnabled(bool enabled);
Status snapshot();

}  // namespace vision_service
