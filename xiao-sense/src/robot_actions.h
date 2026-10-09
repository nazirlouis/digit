#pragma once
#include <cstdint>

namespace robot_actions {
// Preserve existing wire IDs; circle dance uses its own new action ID.
enum class Command : uint8_t { None = 0, Stop = 7,
  ComeSayHello = 8, Identify = 9, Escape = 14, CircleDance = 16 };
inline bool validCommand(int id) { return id == 7 || id == 8 || id == 9 || id == 14 || id == 16; }
struct Status { Command command; const char* phase; bool active; uint32_t generation; };
struct DebugEvent {
  uint32_t id, timestampMs, generation, rangeAgeMs, replyAgeMs;
  Command command;
  char kind[12], reason[112], fault[24], turn[24];
  int distanceMm, rawDistanceMm, rangeStatus, left, right, encoderLeft, encoderRight;
  bool rangeValid, connected, ready, encoderControl;
  uint32_t motionJob, motionSession, controlMaxGapMs, controlOverruns, displayMaxSliceUs;
  int targetLeftCps, targetRightCps, headingDeciDeg;
  char motionState[16], motionResult[24];
};
uint32_t latestDebugId();
bool debugEvent(uint32_t id, DebugEvent* result);
// Main-loop producers and HTTP control handlers share this short-lived lock.
// Camera capture and neural inference never run while holding it.
class Guard {
 public:
  Guard();
  ~Guard();
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};
void begin();
uint32_t generation();
bool start(Command command, uint32_t expectedGeneration);
bool setDriveSpeed(int speed);
int driveSpeed();
void stop(const char* reason = "Stopped", bool smooth = false);
void manualOverride();
void poll();
Status snapshot();
const char* name(Command command);
// Empty when ready. Shared by the dispatcher and dashboard readiness indicators.
const char* unavailableReason(Command command);
}  // namespace robot_actions
