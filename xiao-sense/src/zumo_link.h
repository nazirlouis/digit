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
  uint32_t reactionVersion;
  char fault[24];
  char lastError[32];
  char ledResult[24];
  char wakeResult[24];
  bool voiceCapable;
  bool lookActive;
  uint32_t controlGeneration;
  char lookResult[24];
  bool manualDriving;
  bool actionCapable;
  bool rangeValid;
  bool rangeNoReturn;
  int distanceMm;
  uint32_t safetyEvents;
  bool brakeCapable;
  bool rampedLookCapable;
  bool wiggleCapable;
  bool sceneCapable;
  bool autoRampCapable;
  char sceneResult[24];
  uint32_t rangeAgeMs;
  bool closeApproachCapable;
  bool approachArrived;
  bool chaseCloseCapable;
  uint32_t statusTimestampMs;
  int rangeStatus;
  int rawDistanceMm;
  bool encoderControl;
  int encoderLeftCps;
  int encoderRightCps;
  bool motionCapable;
  bool distanceCalibrated;
  bool expressionCapable;
  int encoderLeftCount, encoderRightCount;
  int motionTravelledMm;
  bool circleCapable;
  bool spinCapable;
  bool motionArmed;
  bool motionActive;
  uint32_t motionBoot;
  uint32_t motionSession;
  uint32_t motionJob;
  uint32_t motionRangeAgeMs;
  uint32_t motionSample;
  uint32_t motionTimestampMs;
  int motionDistanceMm;
  int motionRangeStatus;
  int motionRawDistanceMm;
  int motionHeadingDeciDeg;
  char motionState[16];
  char motionResult[24];
  uint32_t controlMaxGapMs;
  uint32_t controlOverruns;
  uint32_t displayMaxSliceUs;
  int targetLeftCps;
  int targetRightCps;
};

void begin();
void poll();
Status snapshot();
// Only the mission supervisor refreshes this lease. The UART task cannot
// keep a stalled mission alive just by continuing to send heartbeats.
void superviseMotion();
bool armMotion();
uint32_t driveJob(int left, int right, int stopDistanceMm, uint32_t maxMs = 30000);
uint32_t travelJob(int distanceMm, uint32_t maxMs = 15000);
uint32_t turnJob(int degrees, uint32_t maxMs = 20000);
uint32_t circleJob(int speed);
// A single local gyro-controlled 360-degree turn, with encoder-confirmed rest.
uint32_t spinJob(uint32_t maxMs = 12000);
uint32_t measureJob();
bool updateDriveJob(uint32_t job, int left, int right);
bool ackMotion(uint32_t job);
void cancelMotion(bool smooth = true);
bool drive(int left, int right, int acceleration = 6000);
bool turn(int speed);
bool safeDrive(int left, int right, int stopDistanceMm = 150);
void stop(bool smooth = false);
bool calibrate();
bool isFaceName(const char* name);
bool face(const char* name);
bool wake();
bool look(int degrees, uint32_t expectedControlGeneration);
bool wiggle(uint32_t expectedControlGeneration, bool excited = false);
bool scene(const char* expression, const char* label, uint32_t durationMs = 0);
// Discard queued presentations and their reply identities without touching motors.
void cancelScene();
void cancelWake();
bool setAcceleration(int acceleration);
int acceleration();
void cancelLook();
bool led(int target, uint8_t red, uint8_t green, uint8_t blue,
         uint8_t brightness);

}  // namespace zumo_link
