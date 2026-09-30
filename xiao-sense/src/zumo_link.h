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
  char ledResult[24];
  char wakeResult[24];
  bool voiceCapable;
  bool lookActive;
  uint32_t controlGeneration;
  char lookResult[24];
  bool manualDriving;
};

void begin();
void poll();
Status snapshot();
bool drive(int left, int right);
bool turn(int speed);
void stop();
bool calibrate();
bool isFaceName(const char* name);
bool face(const char* name);
bool wake();
bool look(int degrees, uint32_t expectedControlGeneration);
void cancelLook();
bool led(int target, uint8_t red, uint8_t green, uint8_t blue,
         uint8_t brightness);

}  // namespace zumo_link
