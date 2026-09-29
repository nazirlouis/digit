#pragma once

#include <Arduino.h>

namespace zumo_link {

struct Status {
  bool connected;
  bool ready;
  bool calibrating;
  bool edge;
  int left;
  int right;
  int batteryMillivolts;
  uint32_t replyAgeMs;
  char fault[24];
  char lastError[32];
};

void begin();
void poll();
Status snapshot();
bool drive(int left, int right);
void stop();
bool calibrate();
bool face(bool happy);

}  // namespace zumo_link
