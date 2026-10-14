#include "robot_actions.h"
#include "action_console.h"
#include "wake_word.h"
#include "local_stop.h"
#include "reaction_player.h"
#include "personality_events.h"
#include "Arduino.h"
#include "audio_engine.h"
#include "robot_session.h"
#include "vision_service.h"
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
uint32_t dashboardExpressionId=0, dashboardExpressionAt=0;
uint32_t jobSession=0, jobBoot=0, recoveryAt=0, recoveryRestAt=0, recoveryAfter=0;
bool recoveryPending=false;
bool edgeAllowed=false, edgeSeen=false;
bool reverseAtEdge=false;
int edgeMask=0, retreatMm=0;
uint32_t recoveryCount=0;
char actionRecoveryReason[24] = {};
bool positionKnown=true;
const char* failureReason="";
bool healthyForRecovery(const zumo_link::Status& z) {
  const bool floor = z.ready && !z.edge && !strcmp(z.fault,"NONE");
  const bool reverse = z.edgeReverseAllowed && z.edge && !strcmp(z.fault,"EDGE");
  return z.connected && (floor || reverse) && !z.calibrating &&
      z.replyAgeMs<=250 && z.statusTimestampMs && millis()-z.statusTimestampMs<=250 &&
      z.encoderControl && z.voiceCapable && z.distanceCalibrated && !z.manualDriving && !z.lookActive &&
      z.rangeAgeMs<250 && (z.rangeValid || z.rangeNoReturn);
}
const char* motionStopReason(const zumo_link::Status& z) {
  const char* reason = z.lastError;
  if (!strcmp(reason,"TRACK_MISMATCH")) return "Unequal track travel; possible obstruction or slip";
  if (!strcmp(reason,"OBSTACLE")) return "Front obstacle stopped movement";
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
  e.edgeMask=z.motionEdgeMask; e.retreatMm=z.motionRetreatMm;
  snprintf(e.edgeRetreatReject,sizeof(e.edgeRetreatReject),"%s",z.edgeRetreatReject);
  snprintf(e.edgeRetreatDetail,sizeof(e.edgeRetreatDetail),"%s",z.edgeRetreatDetail);
  snprintf(e.motionState,sizeof(e.motionState),"%s",z.motionState);
  snprintf(e.motionResult,sizeof(e.motionResult),"%s",z.motionResult);
  if (strcmp(kind,"started") && strcmp(kind,"finished")) {
    const bool stopped=!strcmp(kind,"stopped");
    action_console::local(stopped ? "stop" : kind,"",reason,
        stopped ? action_console::State::Completed : action_console::State::Blocked,
        stopped ? "Cancellation sent to local controller" : reason,
        !strncmp(reason,"Dashboard",9) || !strncmp(reason,"Manual drive",12) || !strcmp(reason,"Expression preview stopped") ? "Dashboard" : "Controller",
        stopped ? Command::Stop : command);
  }
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
Status snapshot() { Guard guard; return {command, phase, active, controlEpoch.load(), actionResult, travelled, turned, failureReason, edgeMask, retreatMm, recoveryCount, actionRecoveryReason, positionKnown}; }
uint32_t latestDebugId() { Guard guard; return debugId; }
bool debugEvent(uint32_t id, DebugEvent* result) {
  Guard guard;
  if (!id || id > debugId || debugId-id >= kDebugHistory) return false;
  *result = history[(id-1)%kDebugHistory]; return result->id == id;
}
void stop(const char* reason, bool smooth) {
  Guard guard; record("stopped", reason); ++controlEpoch;
  action_console::cancelSession(0,reason);
  action_console::updateId(dashboardExpressionId,action_console::State::Cancelled,reason); dashboardExpressionId=0;

  wake_word::cancel(); local_stop::cancel(); reaction_player::cancel(); personality_events::cancel();
  robot_session::cancel(); vision_service::setEnabled(false);
  audioEngine.cancelOutput(); zumo_link::stop(smooth); zumo_link::cancelScene();
  expressionUntil = 0; active = false; command = Command::None;
  recoveryPending=false; failureReason="";
  edgeAllowed=edgeSeen=false; edgeMask=retreatMm=0;
  recoveryCount=0; actionRecoveryReason[0]=0;
  phase = reason; actionResult = Result::Cancelled;
}
void manualOverride() {
  Guard guard;
  if (active || robot_session::liveActive() || wake_word::snapshot().reacting) stop("Manual control");
  else {
    action_console::updateId(dashboardExpressionId,action_console::State::Cancelled,"Manual control superseded the expression"); dashboardExpressionId=0;
    ++controlEpoch; wake_word::cancel(); reaction_player::cancel(); audioEngine.cancelOutput();
    robot_session::cancel(); zumo_link::cancelScene(); expressionUntil=0;
  }

}
static void suspendMotion(const char* reason, const char* description) {
  Guard guard;
  const auto z=zumo_link::snapshot();
  if (job && z.motionJob==job && z.motionSession==jobSession && z.motionBoot==jobBoot) {
    travelled=z.motionTravelledMm; turned=z.motionHeadingDeciDeg-headingOrigin;
    recoveryCount=z.motionRecoveryCount; retreatMm=z.motionRetreatMm;
    snprintf(actionRecoveryReason,sizeof(actionRecoveryReason),"%s",z.motionRecoveryReason);
  }
  record("paused",description);

  vision_service::setEnabled(false); audioEngine.cancelOutput();
  zumo_link::stop(false); zumo_link::cancelScene();
  expressionUntil=0; active=false; recoveryPending=false;
  job=jobSession=jobBoot=0; edgeAllowed=edgeSeen=false;
  positionKnown=false;
  phase=description;
  actionResult=Result::Failed; failureReason=reason;
}
void suspendForReconnect() { suspendMotion("CONNECTION_LOST","Connection interrupted; stopped pending a fresh observation"); }
const char* unavailableReason(Command value) {
  if (!isMovement(value)) return "";
  const auto z = zumo_link::snapshot();
  if (!z.connected || z.replyAgeMs > 600) return "Connect the Zumo first";
  if (z.calibrating) return "Waiting for startup surface calibration";
  const bool reverse = value==Command::Backward && z.edgeReverseAllowed && z.edge && !strcmp(z.fault,"EDGE");
  if (!reverse && (z.edge || !strcmp(z.fault,"EDGE"))) return "Edge detected; only a short straight reverse is allowed until the floor is clear";
  if (!strcmp(z.fault,"CAL_FAILED")) return "Startup surface calibration failed; place Digit on supported floor and calibrate";
  if (!reverse && strcmp(z.fault,"NONE")) return "Movement stopped by a controller fault";
  if (!reverse && !z.ready) return "Waiting for controller readiness";
  if (!z.motionCapable || !z.encoderControl || !z.voiceCapable) return "Calibrated encoder speed and gyro feedback required";
  if (value==Command::Push && !z.pushCapable) return "Zumo firmware does not support bounded object pushing";
  if (value==Command::Dance && !z.danceCapable) return "Install the matching Zumo dance controller first";
  if ((value == Command::Forward || value == Command::Backward || value==Command::Push) && !z.distanceCalibrated)
    return "Encoder-to-distance calibration required (saved for last)";
  if (z.rangeAgeMs>=250 || (!z.rangeValid && !z.rangeNoReturn)) return "Waiting for fresh front distance sensor";
  if (value != Command::Backward && value!=Command::Push && z.rangeValid && z.distanceMm < 50) return "Front obstacle closer than 5 cm";
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
  const auto speech = robot_session::activity();
  if (expected != generation() || !speech.liveActive || speech.liveProbe || speech.dryRun || active ||
      !isMovement(r.command)) return false;
  const int limit = r.command == Command::Push ? kPushCm : r.command == Command::Forward ? kForwardCm : r.command == Command::Backward ? kBackwardCm : kTurnDegrees;
  if ((r.command==Command::Dance ? r.amount!=0 : r.amount < 1 || r.amount > limit) || unavailableReason(r.command)[0]) return false;
  reverseAtEdge=r.command==Command::Backward && zumo_link::snapshot().edgeReverseAllowed;
  if (reverseAtEdge && r.amount>10) return false;

  // A new navigation request supersedes an idle expression gesture. Cancel
  // queued scenes before requesting neutral, so an old gesture cannot restart.
  cancelExpression(); zumo_link::scene("NEUTRAL","NONE");
  command=r.command; amount=r.amount; active=true; actionResult=Result::Running;
  recoveryPending=false; recoveryAt=recoveryRestAt=recoveryAfter=jobSession=jobBoot=0; failureReason="";
  const auto peer=zumo_link::snapshot();
  edgeAllowed=peer.edgeRetreat100Capable || (r.command==Command::Push && peer.localRecoveryCapable) ||
      (r.edgeRetreat && r.command==Command::Forward && (peer.localRecoveryCapable || !peer.edgeReverseCapable));
  edgeSeen=false; edgeMask=retreatMm=0;
  positionKnown=true;
  recoveryCount=0; actionRecoveryReason[0]=0;
  phase="Preparing movement"; job=0; started=millis(); travelled=turned=0;
  const auto z=zumo_link::snapshot(); safetyEvents=z.safetyEvents; headingOrigin=z.motionHeadingDeciDeg;
  record("started", name(command)); return true;
}
bool setExpression(const char* face, uint32_t durationMs) {
  Guard guard;
  if (durationMs < 500 || durationMs > 10000 || !zumo_link::isFaceName(face)) return false;
  const auto z=zumo_link::snapshot();
  // EDGE locks the tracks, but silent eye animations do not require drive readiness.
  if (!z.connected || (strcmp(z.fault,"NONE") && strcmp(z.fault,"EDGE")) || z.calibrating) return false;
  if (!zumo_link::scene(face,"NONE",durationMs)) return false;
  expressionUntil=millis()+durationMs; return true;
}
void recordDashboardExpression(const char* face, bool accepted) {
  Guard guard;
  action_console::updateId(dashboardExpressionId,action_console::State::Cancelled,"Superseded by a new dashboard expression");
  char arguments[64]; snprintf(arguments,sizeof(arguments),"expression=%s, duration_ms=3000",face);
  dashboardExpressionId=action_console::local("set_expression",arguments,"Selected in dashboard",
      accepted ? action_console::State::Queued : action_console::State::Blocked,
      accepted ? "Waiting for Zumo to confirm the face" : "Face unavailable or safety stop has priority","Dashboard",Command::Expression);
  dashboardExpressionAt=millis();
  if (!accepted) dashboardExpressionId=0;
}
void cancelExpression() {
  Guard guard;
  action_console::updateId(dashboardExpressionId,action_console::State::Cancelled,"Expression superseded or cancelled"); dashboardExpressionId=0;
  expressionUntil=0; zumo_link::cancelScene();
  zumo_link::scene("NEUTRAL","NONE");
}
bool recoverableMotionFailure(const zumo_link::Status& z) {
  Guard guard;
  const bool known=!strcmp(z.motionResult,"TRACK_MISMATCH") || !strcmp(z.motionResult,"OBSTACLE") ||
      (!strcmp(z.motionResult,"EDGE") && z.edgeReverseAllowed);
  return active && command==Command::Forward && job && jobSession && jobBoot &&
      z.motionJob==job && z.motionSession==jobSession && z.motionBoot==jobBoot &&
      !z.motionActive && !z.motionArmed &&
      (!strcmp(z.motionState,"DISARMED") || !strcmp(z.motionState,"FAULT")) && known &&
      z.safetyEvents==safetyEvents+(recoveryPending ? 0U : 1U) && healthyForRecovery(z);
}
void poll() {
  Guard guard; reaction_player::poll();
  if (dashboardExpressionId) {
    const auto faceStatus=zumo_link::snapshot();
    if (!strcmp(faceStatus.sceneResult,"APPLIED") || millis()-dashboardExpressionAt>=3000) {
      action_console::updateId(dashboardExpressionId,!strcmp(faceStatus.sceneResult,"APPLIED") ? action_console::State::Applied : action_console::State::Failed,
          !strcmp(faceStatus.sceneResult,"APPLIED") ? "Face confirmed by Zumo; gesture and sound are not confirmed by this acknowledgement" : "Expression failed or visibility timed out");
      dashboardExpressionId=0;
    }
  }
  if (expressionUntil && static_cast<int32_t>(millis()-expressionUntil)>=0) {
    if (zumo_link::scene("NEUTRAL","NONE")) expressionUntil=0;
  }
  if (!active) return;
  const auto z=zumo_link::snapshot();
  if (job && z.motionJob==job && z.motionSession==jobSession && z.motionBoot==jobBoot) {
    recoveryCount=z.motionRecoveryCount;
    positionKnown=z.motionPositionKnown;
    snprintf(actionRecoveryReason,sizeof(actionRecoveryReason),"%s",z.motionRecoveryReason);
    retreatMm=z.motionRetreatMm;
  }
  if (!recoveryPending && recoverableMotionFailure(z)) {
    recoveryPending=true; recoveryAt=millis(); recoveryRestAt=0; recoveryAfter=z.motionTimestampMs;
    safetyEvents=z.safetyEvents;
    failureReason=!strcmp(z.motionResult,"EDGE") ? "EDGE" : !strcmp(z.motionResult,"OBSTACLE") ? "OBSTACLE" : "TRACK_MISMATCH";
    travelled=z.motionTravelledMm; turned=z.motionHeadingDeciDeg-headingOrigin;
    phase="Forward movement blocked; confirming stopped tracks";
    record("blocked",failureReason);
  }
  const bool localRecovering=!strcmp(z.motionState,"RECOVERING") || !strcmp(z.motionState,"PAUSED");
  const bool edgeState=localRecovering && !strncmp(z.motionResult,"EDGE_",5) &&
      (!z.localRecoveryCapable || strcmp(z.motionRecoveryReason,"ENCODER_STALL"));
  const bool edgeDone=!strcmp(z.motionState,"DONE") && !strcmp(z.motionResult,"EDGE_RETREATED");
  if (edgeState || edgeDone) {
    const bool correlated=edgeAllowed && (z.edgeRetreat100Capable || command==Command::Forward || command==Command::Push) && job && jobSession && jobBoot &&
      z.motionJob==job && z.motionSession==jobSession && z.motionBoot==jobBoot && z.edgeRetreatCapable &&
      z.connected && z.replyAgeMs<=250 && z.motionTimestampMs && millis()-z.motionTimestampMs<=600 &&
      z.safetyEvents==safetyEvents && !z.calibrating && !z.manualDriving && !z.lookActive &&
      z.encoderControl && (z.voiceCapable || z.edgeDirectReverseCapable) && z.distanceCalibrated &&
      z.motionEdgeMask>=1 && z.motionEdgeMask<=31 && z.motionRetreatMm>=0 && z.motionRetreatMm<=(z.edgeDirectReverseCapable ? 110 : z.edgeRetreat100Capable ? 102 : 20) &&
      (edgeState ? !strcmp(z.fault,"EDGE_RETREAT") && z.motionArmed && z.motionActive :
        z.ready && !z.edge && !strcmp(z.fault,"NONE") && z.motionRetreatMm>=(z.edgeRetreat100Capable ? 98 : 5));
    if (!correlated || millis()-started>18000) { stop("Edge retreat could not be verified"); return; }
    zumo_link::superviseMotion();
    edgeMask=z.motionEdgeMask; retreatMm=z.motionRetreatMm;
    if (!edgeSeen) { edgeSeen=true; record("edge","EDGE_RETREAT"); }
    phase="Edge detected; Zumo reversing 10 cm before replanning";
    if (edgeDone) {
      travelled=z.motionTravelledMm; turned=z.motionHeadingDeciDeg-headingOrigin;
      actionResult=Result::EdgeRetreated; active=false; failureReason="EDGE";
      phase="Edge retreat completed; observing a new route";
      record("retreated","EDGE_RETREATED");
      zumo_link::ackMotion(job); zumo_link::cancelMotion(false);
    }
    return;
  }
  if (localRecovering) {
    // The local controller owns temporary sensor recovery. Do not turn its
    // bounded pause into a conversation cancellation or refresh an old job.
    const bool correlated=z.localRecoveryCapable && job && jobSession && jobBoot &&
        z.motionJob==job && z.motionSession==jobSession && z.motionBoot==jobBoot &&
        z.connected && z.replyAgeMs<=250 && z.motionTimestampMs && millis()-z.motionTimestampMs<=600 &&
        z.safetyEvents==safetyEvents && !z.calibrating && !z.manualDriving && !z.lookActive && !z.edge &&
        (!strcmp(z.fault,"NONE") || !strcmp(z.fault,"RECOVERING")) && z.motionArmed && z.motionActive &&
        z.motionRecoveryCount>0;
    if (!correlated || millis()-started>18000) { stop("Local recovery could not be verified"); return; }
    travelled=z.motionTravelledMm; turned=z.motionHeadingDeciDeg-headingOrigin;
    phase=!strcmp(z.motionRecoveryReason,"ENCODER_STALL") ? "Zumo backing away from obstruction" :
        "Zumo paused; recovering sensor feedback";
    zumo_link::superviseMotion();
    return;
  }
  const bool reverseGate = (reverseAtEdge || (recoveryPending && !strcmp(failureReason,"EDGE"))) &&
      z.edgeReverseCapable && z.edge && !strcmp(z.fault,"EDGE");
  if (!z.connected || (!reverseGate && (!z.ready || z.edge || strcmp(z.fault,"NONE"))) || z.calibrating || z.safetyEvents!=safetyEvents) {
    stop(!z.connected ? "Controller disconnected" : motionStopReason(z)); return;
  }
  if (recoveryPending) {
    if (!recoverableMotionFailure(z) || millis()-recoveryAt>=2000) {
      stop("Cannot confirm safe stopped state after blocked movement"); return;
    }
    const bool stopped=static_cast<int32_t>(z.statusTimestampMs-recoveryAfter)>0 &&
        !z.left && !z.right && !z.targetLeftCps && !z.targetRightCps &&
        z.encoderLeftCps>=-50 && z.encoderLeftCps<=50 && z.encoderRightCps>=-50 && z.encoderRightCps<=50;
    if (!stopped) recoveryRestAt=0;
    else if (!recoveryRestAt) recoveryRestAt=z.statusTimestampMs;
    else if (z.statusTimestampMs-recoveryRestAt>=150) {
      active=false; actionResult=Result::Recoverable;
      phase="Blocked movement stopped; recovery decision available";
      record("recoverable",failureReason);
    }
    return;
  }
  zumo_link::superviseMotion();
  if (!job) {
    if (millis()-started>2500) { phase="Movement could not arm"; active=false; actionResult=Result::Failed; zumo_link::cancelMotion(false); return; }
    if (!zumo_link::armMotion(reverseAtEdge)) return;
    headingOrigin=z.motionHeadingDeciDeg;
    job=command==Command::Dance ? zumo_link::danceJob() : command==Command::Push ? zumo_link::pushJob(amount*10) : reverseAtEdge ? zumo_link::edgeBackJob(amount*10) : (command==Command::Forward || command==Command::Backward) ?
      zumo_link::travelJob((command==Command::Forward ? amount : -amount)*10,15000,edgeAllowed) :
      zumo_link::turnJob(command==Command::TurnLeft ? amount : -amount,15000);
    if (!job) { phase="Movement request rejected"; active=false; actionResult=Result::Failed; zumo_link::cancelMotion(false); }
    else {
      const auto submitted=zumo_link::snapshot(); jobSession=submitted.motionSession; jobBoot=submitted.motionBoot;
      phase=command==Command::Dance ? "Dancing" : command==Command::Push ? "Pushing object" : "Moving";
    }
  } else if (z.motionJob==job && z.motionSession==jobSession && z.motionBoot==jobBoot && !strcmp(z.motionState,"DONE")) {
    travelled=z.motionTravelledMm; turned=z.motionHeadingDeciDeg-headingOrigin;
    if (!strcmp(z.motionResult,"BLOCKED_RECOVERED")) actionResult=Result::BlockedRecovered;
    else if (!strcmp(z.motionResult,"OBSTACLE_STOPPED")) actionResult=Result::ObstacleStopped;
    else if (!strcmp(z.motionResult,"PUSH_BLOCKED") && command==Command::Push) actionResult=Result::PushBlocked;
    else if (command==Command::Dance && strcmp(z.motionResult,"DANCE_DONE")) {
      stop("Dance did not finish; movement stopped"); return;
    } else if (command==Command::Push && strcmp(z.motionResult,"PUSH_DONE")) {
      stop("Unexpected push result; movement stopped"); return;
    } else actionResult=Result::Completed;
    failureReason=(actionResult==Result::Completed) ? "" : actionRecoveryReason;
    phase=actionResult==Result::Completed ? (command==Command::Dance ? "Dance completed; observing" : "Movement completed; observing") : "Movement interrupted; observing a new route";
    active=false;
    record("finished",phase); zumo_link::ackMotion(job); zumo_link::cancelMotion(false);
  } else if (millis()-started>18000) {
    phase="Movement time limit"; active=false; actionResult=Result::Failed; zumo_link::cancelMotion(false);
  }
}
}
