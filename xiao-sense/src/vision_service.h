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
    uint8_t classId;
  } boxes[5];
  uint8_t boxCount;
  uint16_t imageWidth;
  uint16_t imageHeight;
  bool batteryReady;
  bool batteryMode;
  bool batteryDetected;
  uint8_t batteryScorePercent;
};

void begin();
bool setEnabled(bool enabled);
bool setBatteryEnabled(bool enabled);
Status snapshot();

}  // namespace vision_service
