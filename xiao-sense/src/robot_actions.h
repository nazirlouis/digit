#pragma once
#include <cstdint>

namespace robot_actions {
// Preserve existing wire IDs; battery finding is a separate action.
enum class Command : uint8_t { None = 0, Stop = 7,
  StartTask = 20, Observe = 21, TurnLeft = 22, TurnRight = 23,
  Forward = 24, Backward = 25, Expression = 26, FinishTask = 27 };
struct Definition { Command command; const char* toolName; };
inline constexpr Definition definitions[] = {
  {Command::Stop, "stop"}, {Command::StartTask, "start_task"},
  {Command::Observe, "observe"}, {Command::TurnLeft, "turn_left"},
  {Command::TurnRight, "turn_right"}, {Command::Forward, "move_forward"},
  {Command::Backward, "move_backward"}, {Command::Expression, "set_expression"},
  {Command::FinishTask, "finish_task"},
};
inline constexpr int kForwardCm = 30, kBackwardCm = 10, kTurnDegrees = 90;
struct ToolRequest {
  Command command = Command::None;
  int amount = 0, durationMs = 3000;
  char expression[12]{}, goal[128]{}, outcome[12]{}, reason[128]{};
};
inline bool isMovement(Command c) {
  return c == Command::TurnLeft || c == Command::TurnRight || c == Command::Forward || c == Command::Backward;
}
inline bool validCommand(int id) {
  for (const auto& definition : definitions)
    if (id == static_cast<int>(definition.command)) return true;
  return false;
}
enum class Result : uint8_t { None, Running, Completed, Failed, Cancelled };
struct Status { Command command; const char* phase; bool active; uint32_t generation; Result result = Result::None;
  int travelledMm = 0, turnedDeciDeg = 0; };
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
// Main-loop Live dispatcher only. Preserves the owning conversation through
// an accepted action; all external Stop/manual/fault paths still cancel it.
bool startFromLive(Command command, uint32_t expectedGeneration);
bool startFromLive(const ToolRequest& request, uint32_t expectedGeneration);
bool setExpression(const char* expression, uint32_t durationMs);
void cancelExpression();
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
