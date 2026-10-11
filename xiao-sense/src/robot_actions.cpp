#include "robot_actions.h"
#include "reaction_player.h"
#include "personality_events.h"
#include "Arduino.h"
#include "audio_engine.h"
#include "speech_commands.h"
#include "local_stop.h"
#include "vision_service.h"
#include "wake_word.h"
#include "zumo_link.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <freertos/semphr.h>

namespace {
using robot_actions::Command;
SemaphoreHandle_t gate = nullptr;
std::atomic<uint32_t> controlEpoch{1};
Command command = Command::None;
const char* phase = "Idle";
bool active = false;
robot_actions::Result actionResult = robot_actions::Result::None;
int selectedDriveSpeed = 4000;
uint32_t safetyEvents = 0;
constexpr unsigned kDebugHistory = 24;
robot_actions::DebugEvent history[kDebugHistory] = {};
uint32_t debugId = 0;
int amount=0, travelled=0, turned=0, headingOrigin=0;
uint32_t job=0, started=0, expressionUntil=0;
const char* motionStopReason(const zumo_link::Status& z) {
  const char* reason = z.lastError;
  if (!strcmp(reason,"RANGE_STALE")) return "Distance sensor stopped updating for 250 ms";
  if (!strcmp(reason,"RANGE_IO")) return "Distance sensor connection failed";
  if (!strncmp(reason,"RANGE_STATUS_",13)) return "Distance sensor reported an unreliable measurement; see sensor status in motion result";
  if (!strcmp(reason,"HEARTBEAT_TIMEOUT")) return "Zumo stopped: XIAO heartbeat expired";
  if (!strcmp(reason,"SUPERVISOR_TIMEOUT")) return "Mission stopped responding; motors stopped";
  if (!strcmp(reason,"CONTROL_STALE")) return "Zumo control loop was delayed; motors stopped";
  if (!strcmp(reason,"JOB_TIMEOUT")) return "Local movement time limit reached";
  if (!strcmp(reason,"JOB_ACK_TIMEOUT")) return "Zumo did not acknowledge the motion request";
  if (!strcmp(reason,"PEER_RESTART")) return "Zumo restarted; old movement cancelled";
  if (!strcmp(reason,"APPROACH_UNSETTLED")) return "Wall approach could not settle; check the distance trace";
  if (!strncmp(reason,"GYRO_",5)) return "Gyroscope feedback failed; motors stopped";
  if (!strncmp(reason,"M2_",3)) return "Zumo rejected the motion request; see its result in the debug log";
  return "Action cancelled: Zumo stopped or locked";
}
void record(const char* kind, const char* reason) {
  const auto z = zumo_link::snapshot();
  auto& e = history[debugId % kDebugHistory];
  e = {};
  e.id = ++debugId; e.timestampMs = millis(); e.generation = controlEpoch.load(); e.command = command;
  snprintf(e.kind,sizeof(e.kind),"%s",kind); snprintf(e.reason,sizeof(e.reason),"%s",reason);
  snprintf(e.fault,sizeof(e.fault),"%s",z.fault); snprintf(e.turn,sizeof(e.turn),"%s",z.lookResult);
  e.distanceMm=z.distanceMm; e.rawDistanceMm=z.rawDistanceMm; e.rangeStatus=z.rangeStatus;
  e.rangeAgeMs=z.rangeAgeMs; e.replyAgeMs=z.replyAgeMs; e.rangeValid=z.rangeValid;
  if (z.motionJob && z.motionRangeStatus >= 0 &&
      (!strncmp(z.lastError,"RANGE_",6) || !strcmp(z.lastError,"OBSTACLE"))) {
    e.distanceMm=z.motionDistanceMm; e.rawDistanceMm=z.motionRawDistanceMm;
    e.rangeStatus=z.motionRangeStatus; e.rangeAgeMs=z.motionRangeAgeMs;
    e.rangeValid=z.motionRangeStatus==0 && z.motionDistanceMm>0 && z.motionRangeAgeMs<250;
  }
  e.left=z.left; e.right=z.right; e.encoderLeft=z.encoderLeftCps; e.encoderRight=z.encoderRightCps;
  e.encoderControl=z.encoderControl; e.connected=z.connected; e.ready=z.ready;
  e.motionJob=z.motionJob; e.motionSession=z.motionSession;
  e.controlMaxGapMs=z.controlMaxGapMs; e.controlOverruns=z.controlOverruns;
  e.displayMaxSliceUs=z.displayMaxSliceUs;
  e.targetLeftCps=z.targetLeftCps; e.targetRightCps=z.targetRightCps;
  e.headingDeciDeg=z.motionHeadingDeciDeg;
  snprintf(e.motionState,sizeof(e.motionState),"%s",z.motionState);
  snprintf(e.motionResult,sizeof(e.motionResult),"%s",z.motionResult);
}
}

namespace robot_actions {
Guard::Guard() { if (gate) xSemaphoreTakeRecursive(gate, portMAX_DELAY); }
Guard::~Guard() { if (gate) xSemaphoreGiveRecursive(gate); }
void begin() { gate = xSemaphoreCreateRecursiveMutex(); configASSERT(gate); }
uint32_t generation() { return controlEpoch.load(); }
bool setDriveSpeed(int speed) {
  Guard guard;
  if (speed < 3000 || speed > 6000) return false;
  selectedDriveSpeed = speed;
  return true;
}
int driveSpeed() { Guard guard; return selectedDriveSpeed; }
const char* name(Command value) {
  for (const auto& d : definitions) if (d.command == value) return d.toolName;
  return "Idle";
}
Status snapshot() { Guard guard; return {command, phase, active, controlEpoch.load(), actionResult, travelled, turned}; }
uint32_t latestDebugId() { Guard guard; return debugId; }
bool debugEvent(uint32_t id, DebugEvent* result) {
  Guard guard;
  if (!id || id > debugId || debugId-id >= kDebugHistory) return false;
  *result = history[(id-1)%kDebugHistory]; return result->id == id;
}
void stop(const char* reason, bool smooth) {
  Guard guard; record("stopped", reason); ++controlEpoch;
  personality_events::cancel(); reaction_player::cancel(); wake_word::cancel();
  speech_commands::cancel(); local_stop::cancel(); vision_service::setEnabled(false);
  audioEngine.cancelOutput(); zumo_link::stop(smooth); zumo_link::cancelScene();
  expressionUntil = 0; active = false; command = Command::None;
  phase = reason; actionResult = Result::Cancelled;
}
void manualOverride() {
  Guard guard;
  if (active || speech_commands::liveActive() || wake_word::snapshot().reacting) stop("Manual control");
  else {
    ++controlEpoch; reaction_player::cancel(); audioEngine.cancelOutput();
    wake_word::cancel(); speech_commands::cancel(); zumo_link::cancelScene(); expressionUntil=0;
  }
  personality_events::interaction();
}
const char* unavailableReason(Command value) {
  if (!isMovement(value)) return "";
  const auto z = zumo_link::snapshot();
  if (!z.connected || z.replyAgeMs > 600) return "Connect the Zumo first";
  if (z.calibrating || !z.ready || z.edge || strcmp(z.fault,"NONE")) return "Calibrate surface and clear the safety stop";
  if (!z.motionCapable || !z.encoderControl || !z.voiceCapable) return "Calibrated encoder speed and gyro feedback required";
  if ((value == Command::Forward || value == Command::Backward) && !z.distanceCalibrated)
    return "Encoder-to-distance calibration required (saved for last)";
  if (!z.rangeValid && !z.rangeNoReturn) return "Waiting for fresh front distance sensor";
  if (value != Command::Backward && z.rangeValid && z.distanceMm < 50) return "Front obstacle closer than 5 cm";
  return "";
}
bool start(Command value, uint32_t expected) {
  Guard guard;
  if (expected != generation() || value != Command::Stop) return false;
  stop(); return true;
}
bool startFromLive(Command value, uint32_t expected) { ToolRequest r; r.command=value; return startFromLive(r,expected); }
bool startFromLive(const ToolRequest& r, uint32_t expected) {
  Guard guard;
  const auto speech = speech_commands::activity();
  if (expected != generation() || !speech.liveActive || speech.liveProbe || speech.dryRun || active || !isMovement(r.command)) return false;
  const int limit = r.command == Command::Forward ? kForwardCm : r.command == Command::Backward ? kBackwardCm : kTurnDegrees;
  if (r.amount < 1 || r.amount > limit || unavailableReason(r.command)[0]) return false;
  reaction_player::cancel(); wake_word::cancel();
  command=r.command; amount=r.amount; active=true; actionResult=Result::Running;
  phase="Preparing movement"; job=0; started=millis(); travelled=turned=0;
  const auto z=zumo_link::snapshot(); safetyEvents=z.safetyEvents; headingOrigin=z.motionHeadingDeciDeg;
  record("started", name(command)); return true;
}
bool setExpression(const char* face, uint32_t durationMs) {
  Guard guard;
  if (durationMs < 500 || durationMs > 10000 || !zumo_link::isFaceName(face)) return false;
  const auto z=zumo_link::snapshot();
  if (!z.connected || z.edge || strcmp(z.fault,"NONE") || z.calibrating) return false;
  if (!zumo_link::scene(face,"NONE",durationMs)) return false;
  expressionUntil=millis()+durationMs; return true;
}
void cancelExpression() {
  Guard guard; expressionUntil=0; zumo_link::cancelScene();
}
void poll() {
  Guard guard; reaction_player::poll();
  if (expressionUntil && static_cast<int32_t>(millis()-expressionUntil)>=0) {
    if (zumo_link::scene("NEUTRAL","NONE")) expressionUntil=0;
  }
  if (!active) return;
  const auto z=zumo_link::snapshot();
  if (!z.connected || !z.ready || z.edge || z.calibrating || z.safetyEvents!=safetyEvents || strcmp(z.fault,"NONE")) {
    stop(!z.connected ? "Controller disconnected" : motionStopReason(z)); personality_events::safetyStopped(); return;
  }
  zumo_link::superviseMotion();
  if (!job) {
    if (millis()-started>2500) { phase="Movement could not arm"; active=false; actionResult=Result::Failed; zumo_link::cancelMotion(false); return; }
    if (!zumo_link::armMotion()) return;
    headingOrigin=z.motionHeadingDeciDeg;
    job=(command==Command::Forward || command==Command::Backward) ?
      zumo_link::travelJob((command==Command::Forward ? amount : -amount)*10) :
      zumo_link::turnJob(command==Command::TurnLeft ? amount : -amount,15000);
    if (!job) { phase="Movement request rejected"; active=false; actionResult=Result::Failed; zumo_link::cancelMotion(false); }
    else phase="Moving";
  } else if (z.motionJob==job && !strcmp(z.motionState,"DONE")) {
    travelled=z.motionTravelledMm; turned=z.motionHeadingDeciDeg-headingOrigin;
    actionResult=Result::Completed; phase="Movement completed; observing"; active=false;
    record("finished",phase); zumo_link::ackMotion(job); zumo_link::cancelMotion(false);
  } else if (millis()-started>18000) {
    phase="Movement time limit"; active=false; actionResult=Result::Failed; zumo_link::cancelMotion(false);
  }
}
}
