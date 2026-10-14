#pragma once
#include <cstdint>
namespace zumo_link { struct Status; }

namespace robot_actions {
// Preserve existing wire IDs; battery finding is a separate action.
enum class Command : uint8_t { None = 0, Stop = 7,
  StartTask = 20, Observe = 21, TurnLeft = 22, TurnRight = 23,
  Forward = 24, Backward = 25, Expression = 26, FinishTask = 27, Push = 28, Dance = 29 };
struct Definition { Command command; const char* toolName; };
inline constexpr Definition definitions[] = {
  {Command::Stop, "stop"}, {Command::StartTask, "start_task"},
  {Command::Observe, "observe"}, {Command::TurnLeft, "turn_left"},
  {Command::TurnRight, "turn_right"}, {Command::Forward, "move_forward"},
  {Command::Backward, "move_backward"}, {Command::Expression, "set_expression"},
  {Command::FinishTask, "finish_task"}, {Command::Push, "push_object"},
  {Command::Dance, "dance"},
};
inline constexpr int kForwardCm = 30, kBackwardCm = 10, kTurnDegrees = 90, kPushCm = 30;
struct ToolRequest {
  Command command = Command::None;
  int amount = 0, durationMs = 3000;
  bool edgeRetreat = false; // Set only by the local task policy, never Gemini JSON.
  char expression[12]{}, goal[128]{}, outcome[12]{}, reason[128]{};
  char actionReason[192]{}; // Short public explanation, independent of the outcome.
};
inline bool isMovement(Command c) {
  return c == Command::TurnLeft || c == Command::TurnRight || c == Command::Forward || c == Command::Backward || c == Command::Push || c == Command::Dance;
}
inline bool validCommand(int id) {
  for (const auto& definition : definitions)
    if (id == static_cast<int>(definition.command)) return true;
  return false;
}
enum class Result : uint8_t { None, Running, Completed, Failed, Cancelled, Recoverable, EdgeRetreated,
  BlockedRecovered, ObstacleStopped, PushBlocked };
struct Status { Command command; const char* phase; bool active; uint32_t generation; Result result = Result::None;
  int travelledMm = 0, turnedDeciDeg = 0; const char* failureReason = "";
  int edgeMask = 0, retreatMm = 0; uint32_t recoveryCount = 0; const char* recoveryReason = "";
  bool positionKnown = true; };
struct DebugEvent {
  uint32_t id, timestampMs, generation, rangeAgeMs, replyAgeMs;
  Command command;
  char kind[12], reason[112], fault[24], turn[24];
  int distanceMm, rawDistanceMm, rangeStatus, left, right, encoderLeft, encoderRight;
  bool rangeValid, connected, ready, encoderControl;
  uint32_t motionJob, motionSession, controlMaxGapMs, controlOverruns, displayMaxSliceUs;
  int targetLeftCps, targetRightCps, headingDeciDeg;
  int edgeMask, retreatMm;
  char edgeRetreatReject[24], edgeRetreatDetail[256];
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
void recordDashboardExpression(const char* expression, bool accepted);
void cancelExpression();
bool setDriveSpeed(int speed);
int driveSpeed();
void stop(const char* reason = "Stopped", bool smooth = false);
// Stop hardware on a cloud connection interruption without revoking the task's
// generation. The Live supervisor must obtain a new observation before reuse.
void suspendForReconnect();
void manualOverride();
void poll();
Status snapshot();
const char* name(Command command);
// Empty when ready. Shared by the dispatcher and dashboard readiness indicators.
const char* unavailableReason(Command command);
// Only a correlated forward-job obstruction can retain the current task.
// This identifies its one safety event; completion still waits for actual rest.
bool recoverableMotionFailure(const zumo_link::Status& status);
}  // namespace robot_actions
