#include "robot_actions.h"
#include "action_mission.h"
#include "Arduino.h"
#include "audio_engine.h"
#include "person_follow.h"
#include "photo_store.h"
#include "speech_commands.h"
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
uint32_t safetyEvents = 0;
constexpr unsigned kDebugHistory = 24;
robot_actions::DebugEvent history[kDebugHistory] = {};
uint32_t debugId = 0;
char lastPhase[112] = {};
const char* motionStopReason(const zumo_link::Status& z) {
  const char* reason = z.lastError;
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
void begin() { gate = xSemaphoreCreateRecursiveMutex(); configASSERT(gate); photo_store::begin(); }
uint32_t generation() { return controlEpoch.load(); }
const char* name(Command value) {
  switch (value) {
    case Command::None: return "Idle";
    case Command::Stop: return "Stop";
    case Command::ComeSayHello: return "Come say hello";
    case Command::Identify: return "What's this?";
    case Command::Escape: return "Find a way out";
    default: return "Unknown";
  }
}
Status snapshot() { Guard guard; return {command, phase, active, controlEpoch.load()}; }
uint32_t latestDebugId() { Guard guard; return debugId; }
bool debugEvent(uint32_t id, DebugEvent* result) {
  Guard guard;
  if (!id || id > debugId || debugId-id >= kDebugHistory) return false;
  *result = history[(id-1)%kDebugHistory];
  return result->id == id;
}
void stop(const char* reason, bool smooth) {
  Guard guard;
  if (strcmp(reason,"Starting action")) record("stopped",reason);
  ++controlEpoch;
  action_mission::cancel();
  wake_word::cancel(); speech_commands::cancel(); photo_store::cancel();
  const bool wasFollowing = person_follow::snapshot().enabled;
  person_follow::setEnabled(false);
  if (wasFollowing || vision_service::snapshot().batteryMode) vision_service::setEnabled(false);
  audioEngine.cancelOutput();
  zumo_link::stop(smooth);
  active = false; command = Command::None; phase = reason;
}
void manualOverride() {
  Guard guard;
  if (active || speech_commands::snapshot().listening || wake_word::snapshot().reacting) stop("Manual control");
  else { ++controlEpoch; wake_word::cancel(); speech_commands::cancel(); }
}
const char* unavailableReason(Command value) {
  if (value == Command::Stop) return "";
  if (!validCommand(static_cast<int>(value))) return "Action is retired";
  const auto z = zumo_link::snapshot();
  const auto v = vision_service::snapshot();
  if (!z.connected) return "Connect the Zumo first";
  if (z.calibrating) return "Waiting for surface calibration";
  if (!z.sceneCapable) return "Update Zumo display support";
  if (value == Command::Identify) return v.batteryReady ? "" : "Object detector unavailable";
  if (z.edge) return "Edge stop: place Digit fully on the surface and recalibrate";
  if (!z.ready || strcmp(z.fault, "NONE")) return "Calibrate the surface to enable movement";
  if (!z.motionCapable) return "Update both controllers for autonomous motion jobs";
  if (!z.encoderControl) return "Calibrated encoder feedback is required for autonomous movement";
  if (!z.actionCapable || !z.autoRampCapable || !z.voiceCapable || !z.rampedLookCapable || !z.wiggleCapable)
    return "Gyroscope or updated movement firmware unavailable";
  // The Zumo checks the sensor's local age when accepting a job. Network and
  // telemetry delays must not turn a healthy local reading into a fake fault.
  if (!z.rangeValid) return "Waiting for a valid distance reading";
  if (value == Command::ComeSayHello) {
    if (!z.closeApproachCapable) return "Update Zumo for the 10 cm greeting approach";
    return v.ready ? "" : "Person detector unavailable";
  }
  if (!z.closeApproachCapable) return "Update Zumo for the 10 cm battery approach";
  if (!z.chaseCloseCapable) return "Update Zumo for 5 cm maze clearance";
  return v.batteryReady ? "" : "Object detector unavailable";
}
bool start(Command value, uint32_t expected) {
  Guard guard;
  if (expected != controlEpoch.load()) return false;
  if (value == Command::Stop) { stop(); return true; }
  // Invalid/stale requests never interrupt the action already in progress.
  if (!validCommand(static_cast<int>(value))) return false;
  const char* reason = unavailableReason(value);
  if (reason[0]) { record("rejected",reason); if (!active) phase = reason; return false; }
  stop("Starting action");
  command = value; active = true;
  safetyEvents = zumo_link::snapshot().safetyEvents;
  phase = name(value);
  if (!action_mission::start(value)) {
    stop(action_mission::phase()); return false;
  }
  record("started",name(value)); lastPhase[0]=0;
  return true;
}
void poll() {
  Guard guard;
  if (!active) return;
  const auto z = zumo_link::snapshot();
  if (!z.connected || z.calibrating || z.safetyEvents != safetyEvents ||
      (command != Command::Identify && (!z.ready || z.edge || strcmp(z.fault,"NONE")))) {
    stop(!z.connected ? "Controller connection lost" : z.edge ? "Edge detected: place Digit fully on the surface and recalibrate" :
         z.calibrating ? "Surface calibration started" : !strcmp(z.fault,"ENCODER") ? "Encoder feedback fault" :
         !strcmp(z.fault,"SENSOR") ? "Distance or surface sensor fault" : !strcmp(z.fault,"TURN_FAULT") ? "Turn controller fault" :
         !strcmp(z.lastError,"TIMEOUT") ? "Controller motion heartbeat timed out" :
         !strcmp(z.lastError,"OBSTACLE") ? "Front distance emergency stop" :
         !strcmp(z.lastError,"RANGE_UNAVAILABLE") ? "Controller rejected missing or stale distance" :
         motionStopReason(z)); return;
  }
  action_mission::poll();
  phase = action_mission::phase();
  if (strcmp(lastPhase,phase) || !action_mission::active()) {
    record(action_mission::active() ? "phase" : "finished",phase);
    snprintf(lastPhase,sizeof(lastPhase),"%s",phase);
  }
  if (!action_mission::active()) { active = false; command = Command::None; }
}
}
