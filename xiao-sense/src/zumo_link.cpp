#include "zumo_link.h"
#include "reaction_catalog.h"
#include "dance_music.h"

#include <cstring>
#include <algorithm>
#include <cstdlib>

namespace {

HardwareSerial zumoUart(1);
portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;

constexpr int kRxPin = 4;  // XIAO D3 <- Zumo GP28
constexpr int kTxPin = 3;  // XIAO D2 -> Zumo GP29
constexpr uint32_t kBaud = 115200;
constexpr uint32_t kDrivePeriodMs = 100;
constexpr uint32_t kBrowserLeaseMs = 250;
constexpr uint32_t kStatusPeriodMs = 100;
constexpr uint32_t kLinkTimeoutMs = 2000;
constexpr int kMinimumMovementSpeed = 3000;
int movementTarget(int speed) {
  if (speed > 0 && speed < kMinimumMovementSpeed) return kMinimumMovementSpeed;
  if (speed < 0 && speed > -kMinimumMovementSpeed) return -kMinimumMovementSpeed;
  return speed;
}
constexpr const char* kFaceNames[] = {
    "NEUTRAL", "HAPPY", "SAD", "MAD", "SERIOUS", "CURIOUS", "SURPRISED",
    "SLEEPING", "CONFUSED", "SLEEPY", "EXCITED", "CRYING", "LOVE"};

int faceIndex(const char* name) {
  if (name == nullptr) return 0;
  for (size_t i = 0; i < sizeof(kFaceNames) / sizeof(kFaceNames[0]); ++i) {
    if (strcmp(name, kFaceNames[i]) == 0) return static_cast<int>(i) + 1;
  }
  return 0;
}

zumo_link::Status status = [] {
  zumo_link::Status value{};
  value.batteryMillivolts = value.distanceMm = value.rawDistanceMm = value.rangeStatus = -1;
  value.rangeAgeMs = value.motionRangeAgeMs = UINT32_MAX;
  strcpy(value.fault, "NONE"); strcpy(value.lastError, "NONE");
  strcpy(value.ledResult, "NONE"); strcpy(value.wakeResult, "NONE");
  strcpy(value.lookResult, "NONE"); strcpy(value.sceneResult, "NONE");
  strcpy(value.calibrationResult, "NONE");
  strcpy(value.motionState, "DISARMED"); strcpy(value.motionResult, "NONE");
  return value;
}();
int desiredLeft = 0;
int desiredRight = 0;
int desiredAcceleration = 6000;
int desiredStopMm = 150;
bool driveWanted = false;
bool manualEdgeHoldBlocked = false;
bool reverseSession = false;
bool turnWanted = false;
bool safeWanted = false;
bool stopPending = false;
bool brakePending = false;
bool calibrationPending = false;
int facePending = 0;
int scenePending = 0;
char sceneLabel[16] = {};
uint32_t sceneDurationMs=0;
uint16_t sceneId = 0;
uint32_t sceneSentAt = 0;
uint32_t presentationGeneration = 1;
uint16_t gestureWire = 0;
int gestureFace = 0;
uint32_t gestureRequestedAt = 0, gestureStartedAt = 0, gestureGeneration = 0;
bool gestureStarted = false, gestureOnVisibility = false;
uint32_t danceCueBoot=0,danceCueSession=0;
uint16_t danceCueJob=0;
bool danceCueConsumed=true;
bool stillExpression(int face) { return face == 1 || face == 5 || face == 8; }
void cancelGestureAudio() {
  gestureWire = 0; gestureFace = 0; gestureStarted = false;
  gestureOnVisibility = false;
  ++gestureGeneration;
}
bool ledPending = false;
int ledTarget = -1;
uint8_t ledRed = 0, ledGreen = 0, ledBlue = 0, ledBrightness = 0;
uint16_t ledCommandId = 0;
uint32_t ledSentAt = 0;
bool wakePending = false;
uint16_t wakeCommandId = 0;
uint32_t wakeSentAt = 0;
bool lookPending = false;
bool lookWiggle = false;
bool lookExcited = false;
int lookDegrees = 0;
int configuredAcceleration = 6000, lookAcceleration = 6000;
uint16_t lookId = 0;
uint32_t lookStarted = 0, lastKeepSent = 0;
uint32_t lastBrowserDrive = 0;
uint32_t lastReply = 0;
uint32_t lastRangeReply = 0;
uint32_t reportedRangeAge = 0;
uint32_t lastDriveSent = 0;
uint32_t lastStatusSent = 0;
uint16_t nextId = 1;
uint16_t calibrationId = 0;
uint16_t calibrationVerifyId = 0;
uint32_t calibrationStarted = 0, calibrationBoot = 0;
bool calibrationActive = false, calibrationAccepted = false;
bool calibrationVerifyPending = false, calibrationStrict = false;
constexpr uint32_t kCalibrationTimeoutMs = 3000;
char receiveLine[768];
size_t received = 0;
bool dropping = false;

constexpr uint32_t kSupervisorLeaseMs = 600;
constexpr uint32_t kMotionRetryMs = 180;
uint32_t motionSession = 0, nextMotionSession = 0, supervisorAt = 0;
uint32_t motionSentAt = 0, motionQueuedAt = 0, heartbeatAt = 0;
uint16_t nextMotionJob = 1, pendingMotionJob = 0, ackMotionJob = 0;
uint32_t driveRevision = 0;
uint32_t heartbeatCounter = 0;
bool motionWanted = false, armPending = false;
char motionPending[128] = {};
char motionUpdate[128] = {};
struct MotionWire { uint16_t wire; uint32_t session; uint16_t job; bool heartbeat; };
MotionWire motionWires[16] = {};
unsigned motionWireCursor = 0;

uint16_t motionCrc(const char* data, size_t length) {
  uint16_t crc = 0xffff;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(static_cast<uint8_t>(data[i])) << 8;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
  }
  return crc;
}

bool terminalMotion(const char* value) {
  return !strcmp(value, "DONE") || !strcmp(value, "FAULT") || !strcmp(value, "DISARMED");
}

bool motionEdgeAllowed = false;
bool motionBlockedBackoff = false;
uint32_t motionEdgeSafety = 0;

void discardMotion(const char* reason) {
  motionEdgeAllowed = false;
  motionBlockedBackoff = false;
  motionWanted = armPending = false;
  motionPending[0] = motionUpdate[0] = 0;
  pendingMotionJob = ackMotionJob = 0;
  motionSession = 0;
  status.motionArmed = status.motionActive = false;
  snprintf(status.motionState, sizeof(status.motionState), "DISARMED");
  snprintf(status.motionResult, sizeof(status.motionResult), "%.23s", reason);
}

void queueMotion(const char* verb, uint16_t job, const char* args = "") {
  snprintf(motionPending, sizeof(motionPending), "M2 %08lx %lu %u %s%s%s",
      static_cast<unsigned long>(status.motionBoot), static_cast<unsigned long>(motionSession),
      job, verb, args[0] ? " " : "", args);
  pendingMotionJob = job;
  motionQueuedAt = millis(); motionSentAt = 0;
}

bool handleMotionLine(const char* line, uint32_t now) {
  const char* body = strstr(line, " M2 ");
  if (!body) return false;
  ++body;
  const char* checksum = strstr(body, " *");
  if (!checksum || strlen(checksum + 2) != 4) return true;
  char* end = nullptr;
  const unsigned long supplied = strtoul(checksum + 2, &end, 16);
  if (*end || supplied != motionCrc(body, static_cast<size_t>(checksum-body))) return true;
  unsigned long boot = 0, session = 0, age = 0;
  unsigned job = 0;
  char state[16] = {}, result[24] = {};
  int distance = -1, heading = 0;
  if (sscanf(body, "M2 %lx %lu %u %15s %23s %d %d %lu",
      &boot, &session, &job, state, result, &distance, &heading, &age) != 8 || job > 65535) return true;
  portENTER_CRITICAL(&stateLock);
  if (status.motionCapable && boot == status.motionBoot && session > nextMotionSession)
    nextMotionSession=session;
  if (status.motionCapable && boot == status.motionBoot && !motionWanted &&
      strcmp(state,"DISARMED") && strcmp(state,"FAULT") && strcmp(state,"DONE")) {
    // A restarted XIAO never adopts a movement left over from its prior boot.
    stopPending=true; brakePending=false;
  }
  if (!status.motionCapable || boot != status.motionBoot || !motionWanted || session != motionSession ||
      (job && job != status.motionJob) || (job == 0 && status.motionJob && !armPending)) {
    portEXIT_CRITICAL(&stateLock); return true;
  }
  // A retry or delayed ACK must never regress a completed job back to RUNNING.
  if (job && status.motionJob == job && terminalMotion(status.motionState) &&
      !terminalMotion(state)) { portEXIT_CRITICAL(&stateLock); return true; }
  const bool recovering = !strcmp(state,"RECOVERING") || !strcmp(state,"PAUSED");
  const bool retreated = !strcmp(state,"DONE") && !strcmp(result,"EDGE_RETREATED");
  unsigned long recoveryCount=0;
  char recoveryReason[24] = {};
  const char* recoveryData=strstr(body," REC=");
  const bool recoveryFields=recoveryData &&
      sscanf(recoveryData," REC=%lu CAUSE=%23s",&recoveryCount,recoveryReason)==2;
  const bool blockedBackoff=status.localRecoveryCapable && !strcmp(recoveryReason,"ENCODER_STALL") &&
      ((recovering && (!strncmp(result,"EDGE_",5) || !strncmp(result,"BLOCK_",6))) ||
       (!strcmp(state,"DONE") && !strcmp(result,"BLOCKED_RECOVERED") && (motionBlockedBackoff || strstr(body," BACK="))));
  const bool retreating = recovering && !strncmp(result,"EDGE_",5) && !blockedBackoff;
  if (status.localRecoveryCapable && job && recoveryCount<status.motionRecoveryCount) {
    // An old pre-recovery reply must not overwrite measured progress or state.
    portEXIT_CRITICAL(&stateLock); return true;
  }
  if (recovering && !retreating && (!status.localRecoveryCapable || !job || !recoveryFields ||
      !recoveryCount || !strcmp(recoveryReason,"NONE") || stopPending || status.safetyEvents!=motionEdgeSafety)) {
    discardMotion("RECOVERY_PROTOCOL"); ++status.safetyEvents; stopPending=true; brakePending=false;
    snprintf(status.lastError,sizeof(status.lastError),"RECOVERY_PROTOCOL");
    portEXIT_CRITICAL(&stateLock); return true;
  }
  int edgeMask=0, backMm=0;
  const char* edgeData = strstr(body," ER=");
  const bool edgeFields=edgeData && sscanf(edgeData," ER=%d BACK=%d",&edgeMask,&backMm)==2;
  if (blockedBackoff && (!motionEdgeAllowed || !edgeFields || edgeMask || backMm<0 || backMm>20 ||
      (!strcmp(state,"DONE") && backMm<10) || stopPending || status.safetyEvents!=motionEdgeSafety)) {
    discardMotion("RECOVERY_PROTOCOL"); ++status.safetyEvents; stopPending=true; brakePending=false;
    snprintf(status.lastError,sizeof(status.lastError),"RECOVERY_PROTOCOL");
    portEXIT_CRITICAL(&stateLock); return true;
  }
  if (motionBlockedBackoff && !strcmp(state,"DONE") && !blockedBackoff) {
    discardMotion("RECOVERY_PROTOCOL"); ++status.safetyEvents; stopPending=true; brakePending=false;
    snprintf(status.lastError,sizeof(status.lastError),"RECOVERY_PROTOCOL");
    portEXIT_CRITICAL(&stateLock); return true;
  }
  if (blockedBackoff) motionBlockedBackoff=true;
  if (status.motionEdgeMask && !strcmp(state,"DONE") && !retreated) {
    discardMotion("EDGE_PROTOCOL"); ++status.safetyEvents; stopPending=true; brakePending=false;
    snprintf(status.lastError,sizeof(status.lastError),"EDGE_PROTOCOL");
    portEXIT_CRITICAL(&stateLock); return true;
  }
  if (retreating || retreated) {
    const bool known = retreated || !strcmp(result,"EDGE_WAIT") || !strcmp(result,"EDGE_BACK") || !strcmp(result,"EDGE_SETTLE");
    if (!job || !motionEdgeAllowed || stopPending || status.safetyEvents!=motionEdgeSafety ||
        !status.edgeRetreatCapable || !known || !edgeFields ||
        edgeMask<1 || edgeMask>31 || backMm<0 || backMm>(status.edgeDirectReverseCapable ? 110 : status.edgeRetreat100Capable ? 102 : 20) ||
        (retreated && backMm<(status.edgeRetreat100Capable ? 98 : 5))) {
      discardMotion("EDGE_PROTOCOL"); ++status.safetyEvents; stopPending=true; brakePending=false;
      snprintf(status.lastError,sizeof(status.lastError),"EDGE_PROTOCOL");
      portEXIT_CRITICAL(&stateLock); return true;
    }
  } else if (status.motionEdgeMask && !terminalMotion(state)) {
    // A delayed pre-edge RUNNING/CALIBRATING reply cannot undo the recovery.
    portEXIT_CRITICAL(&stateLock); return true;
  }
  lastReply = now;
  status.connected = true;
  status.motionSession = session;
  status.motionTimestampMs = now;
  status.motionJob = job;
  status.motionDistanceMm = distance;
  status.motionRangeStatus = status.motionRawDistanceMm = -1;
  const char* rangeDiagnostic = strstr(body, " RS=");
  if (rangeDiagnostic) sscanf(rangeDiagnostic," RS=%d RAW=%d",&status.motionRangeStatus,&status.motionRawDistanceMm);
  status.motionHeadingDeciDeg = heading;
  status.motionTravelledMm = 0;
  status.motionRecoveryCount=static_cast<uint32_t>(recoveryCount);
  snprintf(status.motionRecoveryReason,sizeof(status.motionRecoveryReason),"%s",recoveryReason);
  int positionKnown=1;
  const char* knownData=strstr(body," KNOWN=");
  if (knownData && sscanf(knownData," KNOWN=%d",&positionKnown)!=1) positionKnown=0;
  // Lost pose certainty cannot be restored by a delayed or incomplete frame
  // from the same job. Only a newly submitted movement resets this flag.
  status.motionPositionKnown=status.motionPositionKnown && positionKnown==1;
  if (!strcmp(result,"EDGE") && status.edgeReverseCapable) {
    status.ready=false; status.edge=true;
    snprintf(status.fault,sizeof(status.fault),"EDGE");
    status.edgeReverseAllowed=strstr(body," REV=1") != nullptr;
  }
  const char* travel = strstr(body, " TRAVEL=");
  if (travel) sscanf(travel, " TRAVEL=%d", &status.motionTravelledMm);
  if (!strcmp(state,"DONE") && !strcmp(result,"EDGE_BACK_DONE")) {
    if (!reverseSession || !job || !travel || status.motionTravelledMm>0 ||
        status.motionTravelledMm < (status.edgeDirectReverseCapable ? -110 : -100) ||
        status.safetyEvents!=motionEdgeSafety || stopPending) {
      discardMotion("EDGE_PROTOCOL"); ++status.safetyEvents; stopPending=true; brakePending=false;
      portEXIT_CRITICAL(&stateLock); return true;
    }
    status.ready=true; status.edge=false; status.edgeReverseAllowed=false;
    snprintf(status.fault,sizeof(status.fault),"NONE");
    snprintf(status.lastError,sizeof(status.lastError),"NONE");
  }
  status.motionRangeAgeMs = age;
  status.motionSample = job;
  status.motionEdgeMask = edgeMask;
  status.motionRetreatMm = backMm;
  // Diagnostic-only tokens, accepted only after CRC and job correlation.
  if (const char* rejected=strstr(body," EREJ=")) {
    sscanf(rejected," EREJ=%23s",status.edgeRetreatReject);
    if (const char* detail=strstr(body," EDETAIL="))
      sscanf(detail," EDETAIL=%255s",status.edgeRetreatDetail);
  }
  snprintf(status.motionState, sizeof(status.motionState), "%s", state);
  snprintf(status.motionResult, sizeof(status.motionResult), "%s", result);
  status.motionArmed = strcmp(state,"DISARMED") && strcmp(state,"FAULT");
  status.motionActive = !strcmp(state,"RUNNING") || !strcmp(state,"CALIBRATING") ||
      !strcmp(state,"BRAKING") || !strcmp(state,"SETTLING") || recovering;
  const bool danceOwner=motionWanted && danceCueJob && job==danceCueJob &&
      boot==danceCueBoot && session==danceCueSession && !stopPending;
  if(danceOwner && !danceCueConsumed && !strcmp(state,"RUNNING") && !strcmp(result,"DANCING") &&
      status.ready && !status.edge && !strcmp(status.fault,"NONE") && !status.calibrating) {
    danceCueConsumed=true; gestureFace=15; gestureWire=0;
    gestureOnVisibility=false; gestureStarted=true;
    gestureRequestedAt=gestureStartedAt=now; ++gestureGeneration;
  } else if(gestureFace==15) {
    const bool musicState=(!strcmp(state,"RUNNING")&&!strcmp(result,"DANCING")) ||
        (!strcmp(state,"SETTLING")&&!strcmp(result,"DANCE_SETTLING"));
    if(!danceOwner||!musicState) cancelGestureAudio();
  }
  if (retreating) {
    status.ready=false; status.edge=true;
    snprintf(status.fault,sizeof(status.fault),"EDGE_RETREAT");
  }
  if (retreated) {
    // The matching checksummed completion certifies stopped encoders. Direct
    // reverse does not certify clear floor or clear any independent event.
    status.ready=true; status.edge=false;
    snprintf(status.fault,sizeof(status.fault),"NONE");
    status.encoderLeftCps=status.encoderRightCps=0;
    status.targetLeftCps=status.targetRightCps=0;
  }
  if (!strcmp(state,"DONE")) {
    motionUpdate[0]=0;
    // M2 completion certifies encoder-confirmed rest, unlike legacy ARRIVED.
    status.left=status.right=0;
    status.statusTimestampMs=now;
  }
  const char* diagnostics = strstr(body, " GAP=");
  if (diagnostics) {
    unsigned long gap=0, overruns=0, slice=0;
    if (sscanf(diagnostics, " GAP=%lu OVERRUN=%lu DSUS=%lu TL=%d TR=%d", &gap, &overruns,
        &slice, &status.targetLeftCps, &status.targetRightCps) >= 3) {
      status.controlMaxGapMs=gap; status.controlOverruns=overruns; status.displayMaxSliceUs=slice;
    }
  }
  if (job == pendingMotionJob) motionPending[0] = 0;
  if (!job && status.motionArmed) armPending = false;
  if (!strcmp(state,"FAULT") || !strcmp(state,"DISARMED")) {
    ++status.safetyEvents;
    snprintf(status.lastError, sizeof(status.lastError), "%.23s", result);
    discardMotion(result);
  }
  portEXIT_CRITICAL(&stateLock);
  return true;
}

uint16_t reserveWireId() {
  const uint16_t id = nextId;
  nextId = nextId == 65535 ? 1 : nextId + 1;
  return id;
}

uint16_t sendPrefix() {
  const uint16_t id = reserveWireId();
  zumoUart.print(id);
  zumoUart.print(' ');
  return id;
}

void queueStop(bool smooth = false) {
  cancelGestureAudio();
  stopPending = true;
  brakePending = smooth;
}

void clearDrive() {
  driveWanted = false;
  turnWanted = false;
  safeWanted = false;
  desiredLeft = 0;
  desiredRight = 0;
}

void finishLook(const char* result, bool sendStop) {
  const bool wasActive = status.lookActive;
  status.lookActive = false;
  lookPending = false;
  lookId = 0;
  if (wasActive) {
    snprintf(status.lookResult, sizeof(status.lookResult), "%s", result);
    if (sendStop) queueStop();
  }
}

void finishCalibration(const char* result) {
  calibrationActive = calibrationPending = calibrationAccepted = false;
  calibrationVerifyPending = false;
  calibrationId = calibrationVerifyId = 0;
  status.calibrating = false;
  status.ready = !strcmp(result, "COMPLETED");
  snprintf(status.calibrationResult, sizeof(status.calibrationResult), "%.23s", result);
}

bool calibrationStationary() {
  return !driveWanted && !motionWanted && !status.motionArmed && !status.motionActive &&
      !status.lookActive && !lookPending && !status.left && !status.right &&
      !status.targetLeftCps && !status.targetRightCps && status.encoderControl &&
      std::abs(status.encoderLeftCps) <= 50 && std::abs(status.encoderRightCps) <= 50;
}

void manualOverride() {
  cancelGestureAudio();
  if (calibrationActive) finishCalibration("CANCELLED");
  if (motionWanted) { discardMotion("CANCELLED"); queueStop(); }
  ++status.controlGeneration;
  status.approachArrived = false;
  scenePending = 0;
  sceneId = 0;
  finishLook("CANCELLED", true);
}

void handleLine(const char* line, uint32_t now) {
  unsigned gestureId = 0;
  char phase[8] = {}, expression[16] = {}, completion[8] = {}, extra = 0;
  const int gestureFields = sscanf(line, "%u EVENT GESTURE %7s %15s %7s %c", &gestureId, phase, expression, completion, &extra);
  if (gestureFields == 3 || gestureFields == 4) {
    portENTER_CRITICAL(&stateLock);
    if (gestureWire && gestureId == gestureWire && faceIndex(expression) == gestureFace) {
      if (!strcmp(phase,"END")) {
        // Natural motor completion must not cut off the approved longer phrase.
        // A legacy bare END or any cancellation still stops sound immediately.
        if (gestureStarted && !strcmp(completion,"DONE")) gestureWire = 0;
        else cancelGestureAudio();
      }
      else if (!strcmp(phase,"START") && !completion[0] && !gestureOnVisibility && !gestureStarted && now-gestureRequestedAt < 1500 &&
               lastReply && now-lastReply < 500 && status.ready && !status.edge &&
               !strcmp(status.fault,"NONE") && !status.calibrating && !driveWanted &&
               !motionWanted && !status.lookActive && !stopPending) {
        gestureStarted = true; gestureStartedAt = now; ++gestureGeneration;
      }
    }
    portEXIT_CRITICAL(&stateLock);
    return;
  }
  if (handleMotionLine(line, now)) return;
  unsigned id = 0;
  int ready = 0, left = 0, right = 0, battery = -1, edge = 0, voice = 0;
  char fault[24] = {};
  if (sscanf(line, "%u OK READY=%d FAULT=%23s L=%d R=%d BAT=%d EDGE=%d VOICE=%d",
             &id, &ready, fault, &left, &right, &battery, &edge, &voice) >= 7) {
    portENTER_CRITICAL(&stateLock);
    lastReply = now;
    status.connected = true;
    status.ready = ready != 0 && !status.calibrating;
    status.edge = edge != 0;
    status.left = left;
    status.right = right;
    status.statusTimestampMs = now;
    status.batteryMillivolts = battery;
    status.voiceCapable = voice == 1;
    int distance = -1, valid = 0, capable = 0;
    const char* range = strstr(line, " DIST=");
    if (range) sscanf(range, " DIST=%d RANGE=%d AUTO=%d", &distance, &valid, &capable);
    lastRangeReply = range ? now : 0;
    unsigned age = 0;
    const char* ageField = strstr(line, " RA=");
    if (ageField && sscanf(ageField, " RA=%u", &age) != 1) age = 65535;
    reportedRangeAge = std::min(age, 65535U);
    status.rangeStatus = -1;
    status.rawDistanceMm = distance;
    const char* diagnostic = strstr(line, " RS=");
    if (diagnostic) sscanf(diagnostic, " RS=%d RAW=%d", &status.rangeStatus, &status.rawDistanceMm);
    int encoder = 0;
    status.encoderLeftCps = status.encoderRightCps = 0;
    const char* enc = strstr(line, " ENC=");
    if (enc) sscanf(enc, " ENC=%d VL=%d VR=%d", &encoder, &status.encoderLeftCps, &status.encoderRightCps);
    status.encoderControl = encoder == 1;
    status.expressionCapable = strstr(line," EXPR=1") != nullptr;
    status.encoderLeftCount=status.encoderRightCount=0;
    const char* counts=strstr(line," CL=");
    if (counts) sscanf(counts," CL=%d CR=%d",&status.encoderLeftCount,&status.encoderRightCount);
    const char* loop = strstr(line, " GAP=");
    if (loop) {
      unsigned long gap=0, overruns=0, slice=0;
      if (sscanf(loop," GAP=%lu OVERRUN=%lu DSUS=%lu TL=%d TR=%d",&gap,&overruns,&slice,
          &status.targetLeftCps,&status.targetRightCps) >= 3) {
        status.controlMaxGapMs=gap;status.controlOverruns=overruns;status.displayMaxSliceUs=slice;
      }
    } else {
      status.controlMaxGapMs=status.controlOverruns=status.displayMaxSliceUs=0;
      status.targetLeftCps=status.targetRightCps=0;
    }
    const bool capableMotion = strstr(line, " MOTION=1") != nullptr;
    unsigned long boot = 0;
    unsigned long sessionFloor = 0;
    const char* bootField = strstr(line, " BOOT=");
    if (bootField) sscanf(bootField, " BOOT=%lx", &boot);
    const char* sessionField = strstr(line, " SESSION=");
    if (sessionField) sscanf(sessionField," SESSION=%lu",&sessionFloor);
    if (motionWanted && (!capableMotion || !boot || boot != status.motionBoot)) {
      discardMotion("PEER_RESTART");
      ++status.safetyEvents;
      snprintf(status.lastError, sizeof(status.lastError), "PEER_RESTART");
      queueStop();
    }
    if (boot != status.motionBoot) { nextMotionSession=0; cancelGestureAudio(); }
    if (calibrationActive && calibrationBoot && boot != calibrationBoot) {
      finishCalibration("PEER_RESTART");
      queueStop();
    }
    nextMotionSession=std::max(nextMotionSession,static_cast<uint32_t>(sessionFloor));
    status.motionCapable = capableMotion && boot && sessionField;
    status.distanceCalibrated = strstr(line," TRAVEL=1") != nullptr && strstr(line," DCAL=1") != nullptr;
    status.edgeRetreatCapable = strstr(line," ERET=1") != nullptr;
    status.edgeRetreat100Capable = status.edgeRetreatCapable && strstr(line," E100=1") != nullptr;
    status.edgeDirectReverseCapable = status.edgeRetreat100Capable && strstr(line," EDIRECT=1") != nullptr;
    status.automaticEdgeRecovery = status.edgeRetreat100Capable && edge && !strcmp(fault,"EDGE") && strstr(line," EREC=1");
    if (status.automaticEdgeRecovery) manualEdgeHoldBlocked=true;
    status.edgeReverseCapable = strstr(line," EREV=1") != nullptr;
    status.localRecoveryCapable = status.motionCapable && strstr(line," RECOVERY=1") != nullptr;
    status.pushCapable = status.localRecoveryCapable && strstr(line," PUSH=1") != nullptr;
    status.danceCapable = status.motionCapable && strstr(line," DANCE=1") != nullptr;
    status.edgeReverseAllowed = status.edgeReverseCapable && edge && !strcmp(fault,"EDGE") && strstr(line," REV=1");
    if (status.motionBoot != boot) status.edgeRetreatReject[0]=status.edgeRetreatDetail[0]=0;
    status.motionBoot = boot;
    status.distanceMm = distance;
    status.rangeValid = valid == 1;
    status.rangeNoReturn = strstr(line, " NR=1") != nullptr;
    status.actionCapable = capable == 1;
    const char* brake = strstr(line, " BRAKE=");
    status.brakeCapable = brake && brake[7] == '1';
    status.rampedLookCapable = strstr(line, " LOOKRAMP=1") != nullptr;
    status.wiggleCapable = strstr(line, " WIGGLE=2") != nullptr;
    status.sceneCapable = strstr(line, " SCENE=1") != nullptr;
    unsigned reactionVersion = 0;
    const char* reactionField = strstr(line, " REACT=");
    if (reactionField) sscanf(reactionField, " REACT=%x", &reactionVersion);
    status.reactionVersion = reactionVersion;
    status.circleCapable = strstr(line, " CIRCLE=2") != nullptr;
    const char* spinField = strstr(line, " SPIN=");
    status.spinCapable = spinField && spinField[6] == '1' &&
        (spinField[7] == ' ' || spinField[7] == '\0');
    status.autoRampCapable = strstr(line, " SAFE2=1") != nullptr;
    status.chaseCloseCapable = strstr(line, " CLOSE=2") != nullptr;
    status.closeApproachCapable = status.chaseCloseCapable || strstr(line, " CLOSE=1") != nullptr;
    snprintf(status.fault, sizeof(status.fault), "%s", fault);
    // A confirmed normal floor status supersedes the historical edge warning.
    // Readiness never restores cancelled drive requests or motion sessions.
    if (status.ready && !status.edge && !strcmp(fault,"NONE") && !strcmp(status.lastError,"EDGE"))
      snprintf(status.lastError, sizeof(status.lastError), "NONE");
    if (calibrationActive && calibrationAccepted && calibrationVerifyId && id == calibrationVerifyId) {
      calibrationVerifyId = 0;
      if (ready && !edge && !strcmp(fault,"NONE") &&
          (!calibrationStrict || calibrationStationary())) {
        finishCalibration("COMPLETED");
        snprintf(status.lastError, sizeof(status.lastError), "NONE");
      }
    }
    if (!status.ready) {
      if (!(status.edgeReverseAllowed && !safeWanted && !turnWanted && desiredLeft<0 && desiredLeft==desiredRight)) clearDrive();
      finishLook("NOT_READY", true);
    }
    portEXIT_CRITICAL(&stateLock);
    return;
  }

  if (sscanf(line, "%u", &id) != 1 || id > 65535) {
    return;
  }
  portENTER_CRITICAL(&stateLock);
  lastReply = now;
  status.connected = true;
  if (id == sceneId && sceneId) {
    const char* error = strstr(line, " ERR ");
    // Still emotions have their own cue after a complete visible frame.
    // Only explicit bounded expressions qualify: Stop/expiry/navigation
    // neutral resets must remain silent and cannot start a new sound.
    if (id == gestureWire && gestureOnVisibility && !gestureStarted &&
        now-gestureRequestedAt < 1500 && strstr(line," OK SCENE_VISIBLE")) {
      gestureStarted = true; gestureStartedAt = now; ++gestureGeneration;
    }
    if (id == gestureWire && error) cancelGestureAudio();
    snprintf(status.sceneResult, sizeof(status.sceneResult), "%s",
             strstr(line, " OK SCENE_VISIBLE") ? "APPLIED" : strstr(line, " OK SCENE") ? "RENDERING" : error ? error + 5 : "BAD_REPLY");
    if (!status.motionCapable && !strcmp(status.sceneResult,"RENDERING"))
      snprintf(status.sceneResult,sizeof(status.sceneResult),"APPLIED");
    if (strcmp(status.sceneResult,"RENDERING")) sceneId = 0;
  }
  if (id == lookId && lookId != 0) {
    const char* error = strstr(line, " ERR ");
    if (strstr(line, " OK LOOK_DONE")) {
      status.left = status.right = 0;
      finishLook("DONE", false);
    } else if (strstr(line, " OK LOOKING")) {
      snprintf(status.lookResult, sizeof(status.lookResult), "%s", lookWiggle ? "PREPARING" : "TURNING");
    } else if (lookWiggle && strstr(line, " OK WIGGLE_START")) {
      snprintf(status.lookResult, sizeof(status.lookResult), "WIGGLING");
    } else if (lookWiggle && strstr(line, " OK WIGGLE_CENTER")) {
      snprintf(status.lookResult, sizeof(status.lookResult), "CENTERING");
    } else {
      finishLook(error ? error + 5 : "BAD_REPLY", true);
    }
  }
  if (id == wakeCommandId && wakeCommandId != 0) {
    const char* error = strstr(line, " ERR ");
    snprintf(status.wakeResult, sizeof(status.wakeResult), "%s",
             strstr(line, " OK WAKE_VISIBLE") ? "APPLIED" :
             strstr(line, " OK WAKE") ? (status.motionCapable ? "RENDERING" : "APPLIED") :
             error ? error + 5 : "BAD_REPLY");
    if (!strcmp(status.wakeResult, "APPLIED")) {
      cancelGestureAudio();
      gestureWire = static_cast<uint16_t>(id);
      gestureFace = 14;  // The local wake acknowledgment, not a new face.
      gestureStarted = true;
      gestureStartedAt = now;
      ++gestureGeneration;
    }
    if (strcmp(status.wakeResult, "RENDERING")) wakeCommandId = 0;
  }
  if (strcmp(line, "0 EVENT WAKE_FAILED") == 0) {
    snprintf(status.wakeResult, sizeof(status.wakeResult), "WAKE_IO");
    if (gestureFace == 14) cancelGestureAudio();
  }
  if (id == ledCommandId && ledCommandId != 0) {
    if (strstr(line, " OK LED") != nullptr) {
      snprintf(status.ledResult, sizeof(status.ledResult), "APPLIED");
    } else if (strstr(line, " ERR ") != nullptr) {
      snprintf(status.ledResult, sizeof(status.ledResult), "%s",
               strstr(line, " ERR ") + 5);
    }
    ledCommandId = 0;
  }
  if (strcmp(line, "0 EVENT READY") == 0) {
    if (calibrationActive) {
      // Unnumbered events cannot complete a request. A current acknowledgement,
      // the sampling interval, and a subsequent numbered STATUS are all needed.
      if (calibrationAccepted && now-calibrationStarted >= 500) {
        calibrationVerifyPending = true;
        snprintf(status.calibrationResult, sizeof(status.calibrationResult), "VERIFYING");
      }
    } else if (!status.calibrationGeneration) {
      status.ready = true;
      status.calibrating = false;
      status.edge = false;
      snprintf(status.fault, sizeof(status.fault), "NONE");
      snprintf(status.lastError, sizeof(status.lastError), "NONE");
    }
  } else if (!strcmp(line,"0 EVENT EDGE_AUTO_START") && status.edgeRetreat100Capable && !motionWanted && !stopPending) {
    // The local manual recovery owns the tracks. Status polling maintains its
    // host lease; discard the held forward command without sending STOP.
    status.ready=false; status.edge=true; status.automaticEdgeRecovery=true;
    manualEdgeHoldBlocked=true;
    snprintf(status.fault,sizeof(status.fault),"EDGE");
    clearDrive(); cancelGestureAudio();
  } else if (strcmp(line, "0 EVENT CAL_FAILED") == 0 ||
             strcmp(line, "0 EVENT EDGE") == 0 ||
             strcmp(line, "0 EVENT BUTTON_C") == 0 ||
             strcmp(line, "0 EVENT TURN_FAULT") == 0 ||
             strcmp(line, "0 EVENT SENSOR") == 0 ||
             strcmp(line, "0 EVENT ENCODER") == 0 || !strncmp(line,"0 EVENT EDGE_",13)) {
    const char* reason = line + strlen("0 EVENT ");
    if (calibrationActive) finishCalibration(reason);
    // One physical edge produces both a terminal M2 result and an event.
    const bool duplicateEdge = !strcmp(reason,"EDGE") && status.edge && !strcmp(status.motionResult,"EDGE") &&
        !strcmp(status.lastError,"EDGE") && now-status.motionTimestampMs<=250;
    if (!duplicateEdge) ++status.safetyEvents;
    status.ready = false;
    status.calibrating = false;
    status.edge = strcmp(reason, "EDGE") == 0;
    if (!duplicateEdge) status.edgeReverseAllowed=false;
    status.left = status.right = 0;
    snprintf(status.fault, sizeof(status.fault), "%.23s", reason);
    snprintf(status.lastError, sizeof(status.lastError), "%.31s", reason);
    if (motionWanted) discardMotion(reason);
    clearDrive();
    finishLook(reason, true);
    queueStop();
  } else if ((strcmp(line, "0 EVENT ARRIVED") == 0 || strstr(line, " OK ARRIVED")) &&
             safeWanted && (desiredStopMm == 50 || desiredStopMm == 100)) {
    // The Zumo is already braking locally; this is arrival, not a safety fault.
    status.approachArrived = true;
    clearDrive();
  } else if (strcmp(line, "0 EVENT TIMEOUT") == 0 ||
             strcmp(line, "0 EVENT OBSTACLE") == 0) {
    const char* reason = line + strlen("0 EVENT ");
    if (calibrationActive) finishCalibration(reason);
    snprintf(status.lastError,sizeof(status.lastError),"%.31s",reason);
    if (motionWanted) discardMotion(reason);
    ++status.safetyEvents;
    status.left = status.right = 0;
    clearDrive();
    finishLook(reason, true);
    queueStop();
  } else if (strstr(line, " OK CALIBRATING") != nullptr) {
    if (calibrationActive && calibrationId && id == calibrationId) {
      calibrationAccepted = true;
      calibrationStarted = now;
      status.calibrating = true;
      status.ready = false;
      snprintf(status.calibrationResult, sizeof(status.calibrationResult), "CALIBRATING");
    } else if (!id && !status.calibrationGeneration) {
      status.calibrating = true; status.ready = false;
    }
  } else if (strstr(line, " ERR ") != nullptr) {
    const char* reason = strstr(line, " ERR ") + 5;
    if (!strncmp(reason,"M2_",3)) {
      bool current = false;
      for (const auto& wire : motionWires) {
        if (wire.wire==id && wire.session==motionSession &&
            (wire.heartbeat || (wire.job==status.motionJob && !terminalMotion(status.motionState)))) current=true;
      }
      if (!motionWanted || !current) { portEXIT_CRITICAL(&stateLock); return; }
      discardMotion(reason); ++status.safetyEvents; queueStop();
    }
    if (!strcmp(reason,"OBSTACLE") || !strcmp(reason,"RANGE_UNAVAILABLE") || !strcmp(reason,"NOT_READY")) {
      ++status.safetyEvents; clearDrive(); queueStop();
    }
    snprintf(status.lastError, sizeof(status.lastError), "%s", reason);
    if (calibrationActive && calibrationId && id == calibrationId) finishCalibration(reason);
  }
  portEXIT_CRITICAL(&stateLock);
}

void pollReceive(uint32_t now) {
  // A bound keeps a busy or malformed peer from delaying motor control.
  for (int count = 0; count < 512 && zumoUart.available(); ++count) {
    const char character = static_cast<char>(zumoUart.read());
    if (character == '\r') {
      continue;
    }
    if (character == '\n') {
      if (!dropping && received) {
        receiveLine[received] = '\0';
        handleLine(receiveLine, now);
      }
      received = 0;
      dropping = false;
    } else if (!dropping) {
      if (received < sizeof(receiveLine) - 1) {
        receiveLine[received++] = character;
      } else {
        received = 0;
        dropping = true;
      }
    }
  }
}

}  // namespace

namespace zumo_link {

void begin() {
  zumoUart.begin(kBaud, SERIAL_8N1, kRxPin, kTxPin);
  Serial.println("Zumo UART: RX GPIO4/D3, TX GPIO3/D2, 115200 8N1");
}

Status snapshot() {
  portENTER_CRITICAL(&stateLock);
  const uint32_t now = millis();
  Status result = status;
  result.replyAgeMs = lastReply == 0 ? UINT32_MAX : now - lastReply;
  result.rangeAgeMs = lastRangeReply == 0 ? UINT32_MAX : now - lastRangeReply + reportedRangeAge;
  result.connected = lastReply != 0 && result.replyAgeMs < kLinkTimeoutMs;
  result.manualDriving = driveWanted;
  if (result.motionTimestampMs && result.motionRangeAgeMs != UINT32_MAX)
    result.motionRangeAgeMs += now-result.motionTimestampMs;
  portEXIT_CRITICAL(&stateLock);
  return result;
}

void gestureAudio(uint32_t& generation, uint8_t& expression) {
  portENTER_CRITICAL(&stateLock);
  const uint32_t now = millis();
  const bool dance=gestureFace==15&&motionWanted&&status.motionArmed&&status.motionActive&&
      status.motionBoot==danceCueBoot&&status.motionSession==danceCueSession&&status.motionJob==danceCueJob&&
      (!strcmp(status.motionResult,"DANCING")||!strcmp(status.motionResult,"DANCE_SETTLING"));
  const bool safe = lastReply && now-lastReply < 500 && status.ready && !status.edge &&
      !strcmp(status.fault,"NONE") && !status.calibrating && !driveWanted && (dance||(!motionWanted&&!status.motionArmed)) &&
      !status.lookActive && !stopPending && !wakePending && !wakeCommandId &&
      (!gestureOnVisibility || (!status.left && !status.right));
  const uint32_t cueDuration = gestureFace == 15 ? DanceMusic::kDurationMs : gestureFace == 14 ? reaction_catalog::wake[1].voiceMs :
      gestureFace >= 1 && gestureFace <= 13 ? reaction_catalog::expression[gestureFace-1].voiceMs : 0;
  if ((gestureWire || gestureStarted) &&
      (!safe || (gestureStarted ? now-gestureStartedAt >= cueDuration+180 : now-gestureRequestedAt >= 1500)))
    cancelGestureAudio();
  generation = gestureGeneration;
  expression = gestureStarted ? static_cast<uint8_t>(gestureFace) : 0;
  portEXIT_CRITICAL(&stateLock);
}

void superviseMotion() {
  portENTER_CRITICAL(&stateLock);
  supervisorAt = millis();
  portEXIT_CRITICAL(&stateLock);
}

bool armMotion(bool reverseOnly) {
  portENTER_CRITICAL(&stateLock);
  const auto now = millis();
  if (motionWanted) {
    const bool armed = status.motionArmed && !armPending;
    portEXIT_CRITICAL(&stateLock); return armed;
  }
  const bool edgeBack = reverseOnly && status.edgeReverseAllowed && status.edge && !strcmp(status.fault,"EDGE");
  const bool allowed = status.motionCapable && status.encoderControl &&
      ((status.ready && !status.edge && !strcmp(status.fault,"NONE")) || edgeBack) && lastReply && now-lastReply < 1000 &&
      !status.calibrating && !driveWanted && !status.lookActive && !stopPending &&
      // A gesture pause can have zero PWM while the tracks are still settling.
      std::abs(status.encoderLeftCps) <= 50 && std::abs(status.encoderRightCps) <= 50 &&
      !status.left && !status.right && nextMotionSession != UINT32_MAX && now-supervisorAt < kSupervisorLeaseMs;
  if (allowed) {
    reverseSession=edgeBack;
    motionSession = ++nextMotionSession;
    if (!motionSession) motionSession = ++nextMotionSession;
    nextMotionJob = 1; driveRevision = heartbeatCounter = 0;
    motionWanted = armPending = true;
    status.motionSession = motionSession;
    status.motionJob = 0;
    status.motionEdgeMask=status.motionRetreatMm=0;
    status.edgeRetreatReject[0]=status.edgeRetreatDetail[0]=0;
    status.motionRecoveryCount=0; status.motionRecoveryReason[0]=0;
    status.motionPositionKnown=true;
    motionEdgeAllowed=false;
    motionBlockedBackoff=false;
    status.motionArmed = status.motionActive = false;
    snprintf(status.motionState,sizeof(status.motionState),"ARMING");
    snprintf(status.motionResult,sizeof(status.motionResult),"NONE");
    queueMotion("ARM",0);
  }
  portEXIT_CRITICAL(&stateLock);
  return false; // Only the peer's acknowledgement grants autonomous authority.
}

static uint32_t submitJob(const char* verb, const char* args, bool edgeRetreat=false) {
  portENTER_CRITICAL(&stateLock);
  const bool allowed = motionWanted && status.motionArmed && !armPending &&
      (reverseSession == !strcmp(verb,"BACK")) &&
      !status.motionActive && !motionPending[0] && !stopPending &&
      millis()-supervisorAt < kSupervisorLeaseMs && nextMotionJob != 0;
  const uint16_t job = allowed ? nextMotionJob++ : 0;
  if (job) {
    danceCueJob=!strcmp(verb,"DANCE") ? job : 0;
    danceCueBoot=status.motionBoot; danceCueSession=motionSession;
    danceCueConsumed=danceCueJob==0;
    motionEdgeAllowed=edgeRetreat || (status.edgeRetreat100Capable && strcmp(verb,"MEASURE") && strcmp(verb,"BACK"));
    motionBlockedBackoff=false;
    motionEdgeSafety=status.safetyEvents;
    status.motionEdgeMask=status.motionRetreatMm=0;
    status.edgeRetreatReject[0]=status.edgeRetreatDetail[0]=0;
    status.motionRecoveryCount=0; status.motionRecoveryReason[0]=0;
    status.motionPositionKnown=true;
    motionUpdate[0]=0;
    status.motionJob = job; status.motionActive = true;
    snprintf(status.motionState,sizeof(status.motionState),"SENDING");
    snprintf(status.motionResult,sizeof(status.motionResult),"NONE");
    queueMotion(verb,job,args);
    driveRevision = 0;
  }
  portEXIT_CRITICAL(&stateLock);
  return job;
}

uint32_t driveJob(int left, int right, int stopMm, uint32_t maxMs) {
  if (left < 3000 || left > 3200 || right < 3000 || right > 3200 ||
      (stopMm != 50 && stopMm != 100) || maxMs < 100 || maxMs > 60000) return 0;
  char args[64];
  snprintf(args,sizeof(args),"%d %d %d %d %lu",left,right,acceleration(),stopMm,static_cast<unsigned long>(maxMs));
  return submitJob("DRIVE",args);
}

uint32_t travelJob(int distanceMm, uint32_t maxMs, bool edgeRetreat) {
  if (!distanceMm || distanceMm < -100 || distanceMm > 300 || maxMs < 100 || maxMs > 15000 || !snapshot().distanceCalibrated) return 0;
  const bool permit = edgeRetreat && distanceMm>0 && snapshot().edgeRetreatCapable;
  char args[48]; snprintf(args,sizeof(args),"%d 3000 %d %lu%s",distanceMm,acceleration(),static_cast<unsigned long>(maxMs),permit?" 1":"");
  return submitJob("TRAVEL",args,permit);
}

uint32_t edgeBackJob(int distanceMm) {
  if (distanceMm<10 || distanceMm>100 || !snapshot().edgeReverseCapable) return 0;
  char args[12]; snprintf(args,sizeof(args),"%d",distanceMm);
  return submitJob("BACK",args);
}

uint32_t pushJob(int distanceMm) {
  const auto z=snapshot();
  if (distanceMm<10 || distanceMm>300 || !z.pushCapable || !z.localRecoveryCapable || !z.distanceCalibrated) return 0;
  char args[12]; snprintf(args,sizeof(args),"%d",distanceMm);
  // There is deliberately no legacy DRIVE fallback or caller-selected speed.
  return submitJob("PUSH",args,z.edgeRetreatCapable);
}

uint32_t turnJob(int degrees, uint32_t maxMs) {
  if (!degrees || degrees < -180 || degrees > 180 || maxMs < 100 || maxMs > 30000) return 0;
  char args[48];
  snprintf(args,sizeof(args),"%d %d %lu",degrees,acceleration(),static_cast<unsigned long>(maxMs));
  return submitJob("TURN",args);
}

uint32_t circleJob(int speed) {
  if (speed < 3000 || speed > 6000 || !snapshot().circleCapable) return 0;
  char args[48];
  snprintf(args,sizeof(args),"%d 3 %d 60000",speed,acceleration());
  return submitJob("CIRCLE",args);
}

uint32_t measureJob() { return submitJob("MEASURE","2500"); }
uint32_t danceJob() {
  if(!snapshot().danceCapable) return 0;
  return submitJob("DANCE","8000");
}

uint32_t spinJob(uint32_t maxMs) {
  if (maxMs < 1000 || maxMs > 30000 || !snapshot().spinCapable) return 0;
  char args[16];
  snprintf(args,sizeof(args),"%lu",static_cast<unsigned long>(maxMs));
  return submitJob("SPIN",args);
}

bool updateDriveJob(uint32_t job, int left, int right) {
  if (left < 3000 || left > 3200 || right < 3000 || right > 3200) return false;
  portENTER_CRITICAL(&stateLock);
  const bool allowed = motionWanted && status.motionArmed && status.motionActive &&
      !strcmp(status.motionState,"RUNNING") && job && job==status.motionJob && !stopPending;
  if (allowed) snprintf(motionUpdate,sizeof(motionUpdate),"M2 %08lx %lu %lu UPDATE %lu %d %d",
      static_cast<unsigned long>(status.motionBoot),static_cast<unsigned long>(motionSession),
      static_cast<unsigned long>(job),static_cast<unsigned long>(++driveRevision),left,right);
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

bool ackMotion(uint32_t job) {
  portENTER_CRITICAL(&stateLock);
  const bool allowed = motionWanted && job && job==status.motionJob && terminalMotion(status.motionState);
  if (allowed) ackMotionJob = static_cast<uint16_t>(job);
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

void cancelMotion(bool smooth) {
  // Existing BRAKE/STOP is also an out-of-band protocol cancellation and
  // disarms the peer. A later movement needs a new ARM session.
  stop(smooth);
}

bool drive(int left, int right, int acceleration) {
  if (left < -6000 || left > 6000 || right < -6000 || right > 6000 ||
      acceleration < 500 || acceleration > 6000) {
    return false;
  }
  portENTER_CRITICAL(&stateLock);
  const uint32_t now = millis();
  if (manualEdgeHoldBlocked && (left || right) && lastReply && now-lastReply<250 && !stopPending &&
      (!strcmp(status.fault,"EDGE") || !strcmp(status.fault,"NONE"))) {
    portEXIT_CRITICAL(&stateLock); return true;  // Discard held commands while local reverse owns the tracks.
  }
  if (!left && !right) manualEdgeHoldBlocked=false;
  const bool normal = status.ready && !status.edge && !strcmp(status.fault,"NONE");
  const bool reverse = status.edgeReverseAllowed && status.edge && !strcmp(status.fault,"EDGE") && left<0 && left==right;
  const bool allowed = (normal || reverse) && lastReply != 0 &&
                       now - lastReply < kLinkTimeoutMs &&
                       !status.calibrating;
  if (allowed) {
    manualOverride();
    desiredLeft = movementTarget(left);
    desiredRight = movementTarget(right);
    desiredAcceleration = acceleration;
    configuredAcceleration = acceleration;
    turnWanted = false;
    safeWanted = false;
    facePending = 0;
    driveWanted = left != 0 || right != 0;
    lastBrowserDrive = now;
    if (!driveWanted) {
      queueStop(status.brakeCapable);
    }
  }
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

bool turn(int speed) {
  if (speed < -6000 || speed > 6000) return false;
  portENTER_CRITICAL(&stateLock);
  const uint32_t now = millis();
  const bool allowed = status.ready && lastReply != 0 &&
                       now - lastReply < kLinkTimeoutMs && !status.calibrating;
  if (allowed) {
    manualOverride();
    desiredLeft = movementTarget(speed);
    desiredRight = -movementTarget(speed);
    turnWanted = true;
    safeWanted = false;
    facePending = 0;
    driveWanted = speed != 0;
    lastBrowserDrive = now;
    if (!driveWanted) queueStop();
  }
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

void stop(bool smooth) {
  portENTER_CRITICAL(&stateLock);
  manualEdgeHoldBlocked=false;
  manualOverride();
  clearDrive();
  wakePending = false;
  wakeCommandId = 0;
  facePending = 0;
  ledPending = false;
  calibrationPending = false;
  status.calibrating = false;
  queueStop(smooth && status.brakeCapable && status.ready && lastReply &&
            millis() - lastReply < kLinkTimeoutMs);
  portEXIT_CRITICAL(&stateLock);
}

bool safeDrive(int left, int right, int stopDistanceMm) {
  if (left < 0 || left > 3200 || right < 0 || right > 3200) return false;
  if (stopDistanceMm != 50 && stopDistanceMm != 100 && stopDistanceMm != 150) return false;
  portENTER_CRITICAL(&stateLock);
  const uint32_t now = millis();
  const bool allowed = status.ready && !status.edge && !strcmp(status.fault, "NONE") &&
      lastReply && now - lastReply < 500 && status.actionCapable &&
      status.rangeValid && status.autoRampCapable && !status.calibrating && !status.lookActive &&
      lastRangeReply && now - lastRangeReply + reportedRangeAge < 250 &&
      (stopDistanceMm != 100 || status.closeApproachCapable) &&
      (stopDistanceMm != 50 || status.chaseCloseCapable) && !status.approachArrived &&
      !stopPending && !facePending && !scenePending && !sceneId &&
      (left + right <= 0 || status.distanceMm >= 50);
  if (allowed) {
    desiredLeft = movementTarget(left);
    desiredRight = movementTarget(right);
    desiredAcceleration = configuredAcceleration;
    desiredStopMm = stopDistanceMm;
    driveWanted = left || right;
    turnWanted = false;
    safeWanted = true;
    lastBrowserDrive = now;
    if (!driveWanted) queueStop();
  }
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

bool beginCalibration(bool strict) {
  portENTER_CRITICAL(&stateLock);
  const uint32_t now = millis();
  const bool allowed = lastReply != 0 && now - lastReply < kLinkTimeoutMs &&
      !driveWanted && !status.calibrating && !calibrationActive &&
      (!strict || (status.statusTimestampMs && now-status.statusTimestampMs <= 250 &&
       !stopPending && calibrationStationary() &&
       (!strcmp(status.fault,"NONE") || !strcmp(status.fault,"EDGE") || !strcmp(status.fault,"CAL_FAILED"))));
  if (allowed) {
    manualOverride();
    ++status.calibrationGeneration;
    if (!status.calibrationGeneration) ++status.calibrationGeneration;
    calibrationActive = true;
    calibrationStrict = strict;
    calibrationAccepted = calibrationVerifyPending = false;
    calibrationId = calibrationVerifyId = 0;
    calibrationStarted = now;
    calibrationBoot = status.motionBoot;
    status.ready = false;
    status.calibrating = true;
    snprintf(status.calibrationResult, sizeof(status.calibrationResult), "QUEUED");
    snprintf(status.lastError, sizeof(status.lastError), "NONE");
    queueStop();
    calibrationPending = true;
  }
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

bool calibrate() { return beginCalibration(false); }
bool calibrateSurface() { return beginCalibration(true); }

bool isFaceName(const char* name) {
  return faceIndex(name) != 0;
}

bool wake() {
  portENTER_CRITICAL(&stateLock);
  const uint32_t now = millis();
  const bool connected = lastReply != 0 && now - lastReply < kLinkTimeoutMs;
  const bool allowed = connected && !driveWanted && !status.lookActive && !status.calibrating &&
                       !calibrationPending && !stopPending && !facePending && !ledPending &&
                       !wakePending && !wakeCommandId && status.left == 0 && status.right == 0;
  if (allowed) { cancelGestureAudio(); wakePending = true; wakeSentAt = now; }
  snprintf(status.wakeResult, sizeof(status.wakeResult), "%s",
           allowed ? "SENDING" : connected ? "BUSY" : "DISCONNECTED");
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

static bool requestTurn(int degrees, uint32_t expectedControlGeneration, bool wiggle) {
  if (degrees < -180 || degrees > 180) return false;
  portENTER_CRITICAL(&stateLock);
  const uint32_t now = millis();
  const bool allowed = status.controlGeneration == expectedControlGeneration &&
      lastReply && now - lastReply < kLinkTimeoutMs && status.ready && status.voiceCapable && status.rampedLookCapable &&
      (!wiggle || status.wiggleCapable) &&
      !status.edge && !status.calibrating && !status.left && !status.right && !driveWanted &&
      !stopPending && !calibrationPending && !facePending && !ledPending && !scenePending && !sceneId &&
      !wakePending && !wakeCommandId && !status.lookActive;
  if (allowed) {
    lookPending = status.lookActive = true;
    lookDegrees = degrees;
    lookWiggle = wiggle;
    lookExcited = false;
    lookAcceleration = configuredAcceleration;
    lookStarted = now;
    snprintf(status.lookResult, sizeof(status.lookResult), "SENDING");
  }
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

bool look(int degrees, uint32_t expectedControlGeneration) {
  return requestTurn(degrees, expectedControlGeneration, false);
}
bool wiggle(uint32_t expectedControlGeneration, bool excited) {
  const bool accepted = requestTurn(15, expectedControlGeneration, true);
  if (accepted) {
    portENTER_CRITICAL(&stateLock);
    lookExcited = excited;
    portEXIT_CRITICAL(&stateLock);
  }
  return accepted;
}

bool scene(const char* expression, const char* label, uint32_t durationMs) {
  if (durationMs && (durationMs<500 || durationMs>10000)) return false;
  const int face = faceIndex(expression);
  if (!face || !label || !label[0] || strlen(label) >= sizeof(sceneLabel)) return false;
  for (const char* p = label; *p; ++p) if ((*p < 'A' || *p > 'Z') && *p != '_') return false;
  portENTER_CRITICAL(&stateLock);
  const uint32_t now = millis();
  const bool allowed = lastReply && now - lastReply < 500 && status.sceneCapable && (!durationMs || status.expressionCapable) &&
      // A latched EDGE fault still blocks every motion path, not display-only scenes.
      !driveWanted && !stopPending && (!strcmp(status.fault,"NONE") || !strcmp(status.fault,"EDGE")) &&
      !status.calibrating && !scenePending && !sceneId && !facePending && !wakePending && !wakeCommandId;
  if (allowed) {
    scenePending = face; sceneDurationMs=durationMs;
    cancelGestureAudio();
    snprintf(sceneLabel, sizeof(sceneLabel), "%s", label);
    snprintf(status.sceneResult, sizeof(status.sceneResult), "SENDING");
  }
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

void cancelScene() {
  portENTER_CRITICAL(&stateLock);
  cancelGestureAudio();
  ++presentationGeneration;
  scenePending = 0; sceneId = 0;
  snprintf(status.sceneResult, sizeof(status.sceneResult), "CANCELLED");
  portEXIT_CRITICAL(&stateLock);
}

void cancelWake() {
  portENTER_CRITICAL(&stateLock);
  if (gestureFace == 14) cancelGestureAudio();
  ++presentationGeneration;
  // Cleanup also runs before unrelated face reactions. Only unfinished wake
  // requests are cancelled; retain acknowledged results and diagnostic errors.
  if (wakePending || wakeCommandId)
    snprintf(status.wakeResult, sizeof(status.wakeResult), "CANCELLED");
  wakePending = false; wakeCommandId = 0;
  portEXIT_CRITICAL(&stateLock);
}

bool setAcceleration(int value) {
  if (value < 500 || value > 6000) return false;
  portENTER_CRITICAL(&stateLock);
  configuredAcceleration = value;
  portEXIT_CRITICAL(&stateLock);
  return true;
}
int acceleration() {
  portENTER_CRITICAL(&stateLock);
  const int value = configuredAcceleration;
  portEXIT_CRITICAL(&stateLock);
  return value;
}

void cancelLook() {
  portENTER_CRITICAL(&stateLock);
  manualOverride();
  portEXIT_CRITICAL(&stateLock);
}

bool face(const char* name) {
  const int index = faceIndex(name);
  if (index == 0) return false;
  portENTER_CRITICAL(&stateLock);
  const uint32_t now = millis();
  const bool allowed = lastReply != 0 && now - lastReply < kLinkTimeoutMs &&
                       !driveWanted && !status.lookActive && !status.calibrating &&
                       status.left == 0 && status.right == 0;
  if (allowed) {
    facePending = index;
    cancelGestureAudio();
    if (wakePending) snprintf(status.wakeResult, sizeof(status.wakeResult), "CANCELLED");
    wakePending = false;
  }
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

bool led(int target, uint8_t red, uint8_t green, uint8_t blue,
         uint8_t brightness) {
  if (target < -1 || target > 5 || brightness > 31) return false;
  portENTER_CRITICAL(&stateLock);
  const uint32_t now = millis();
  const bool allowed = lastReply != 0 && now - lastReply < kLinkTimeoutMs;
  if (allowed) {
    ledTarget = target;
    if (wakePending) snprintf(status.wakeResult, sizeof(status.wakeResult), "CANCELLED");
    wakePending = false;
    ledRed = red;
    ledGreen = green;
    ledBlue = blue;
    ledBrightness = brightness;
    ledPending = true;
    ledCommandId = 0;  // Ignore an older command's acknowledgement.
    snprintf(status.ledResult, sizeof(status.ledResult), "SENDING");
  }
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

void poll() {
  pollReceive(millis());

  bool sendStop = false, sendBrake = false, sendCalibration = false;
  int sendFace = 0;
  bool sendLed = false;
  bool sendWake = false;
  bool sendLook = false;
  bool sendWiggle = false;
  bool sendExcited = false;
  int sendScene = 0;
  uint32_t sendSceneDuration=0;
  char label[16] = {};
  uint16_t keepId = 0;
  int angle = 0;
  int target = -1;
  uint8_t red = 0, green = 0, blue = 0, brightness = 0;
  bool sendDrive = false;
  bool sendTurn = false;
  bool sendSafe = false;
  char sendMotion[128] = {}, sendHeartbeat[80] = {};
  uint32_t dispatchGeneration = 0, dispatchSession = 0;
  uint32_t dispatchPresentation = 0;
  uint16_t dispatchJob = 0;
  int left = 0, right = 0, acceleration = 6000, stopDistanceMm = 150;
  portENTER_CRITICAL(&stateLock);
  // Producers can renew leases or queue work while this task receives UART.
  // Read time under the same lock as those timestamps: an older `now` minus a
  // newly published timestamp otherwise wraps and falsely expires the job.
  const uint32_t now = millis();
  const bool connected = lastReply != 0 && now - lastReply < kLinkTimeoutMs;
  const uint32_t turnDeadline = lookWiggle ? 5000U :
      6500U + 2U * ((3000000U + lookAcceleration - 1) / lookAcceleration);
  if (calibrationActive && (!connected || now-calibrationStarted >= kCalibrationTimeoutMs)) {
    finishCalibration(connected ? "TIMEOUT" : "DISCONNECTED");
    queueStop();
  }
  if (calibrationActive && calibrationStrict &&
      (now-status.statusTimestampMs > 250 || !calibrationStationary())) {
    finishCalibration(now-status.statusTimestampMs > 250 ? "STATUS_STALE" : "MOVING");
    queueStop();
  }
  if (status.lookActive && (!connected || !status.ready || status.calibrating ||
      driveWanted || stopPending || now - lookStarted > turnDeadline ||
      (!strcmp(status.lookResult, "SENDING") && now - lookStarted > 1000))) {
    finishLook(!connected ? "DISCONNECTED" : "CANCELLED", true);
  }
  if (wakeCommandId && now - wakeSentAt >= (status.motionCapable ? 1500U : 1000U)) {
    wakeCommandId = 0;
    snprintf(status.wakeResult, sizeof(status.wakeResult), "NO_REPLY");
  }
  if (sceneId && now - sceneSentAt >= (status.motionCapable ? 3000U : 1000U)) {
    sceneId = 0;
    snprintf(status.sceneResult, sizeof(status.sceneResult), "NO_REPLY");
  }
  if (wakePending && (!connected || driveWanted || status.calibrating || calibrationPending ||
                      stopPending || status.left || status.right || now - wakeSentAt > 500)) {
    wakePending = false;
    snprintf(status.wakeResult, sizeof(status.wakeResult), "%s", connected ? "CANCELLED" : "DISCONNECTED");
  }
  if (ledCommandId != 0 && now - ledSentAt >= 1000) {
    ledCommandId = 0;
    if (!ledPending) snprintf(status.ledResult, sizeof(status.ledResult), "NO_REPLY");
  }
  if (!connected) {
    if (motionWanted) discardMotion("DISCONNECTED");
    status.connected = false;
    status.ready = false;
    status.calibrating = false;
    status.voiceCapable = false;
    clearDrive();
    ledPending = false;
    facePending = 0;
    scenePending = 0;
    sceneId = 0;
    ledCommandId = 0;
    if (strcmp(status.ledResult, "SENDING") == 0)
      snprintf(status.ledResult, sizeof(status.ledResult), "DISCONNECTED");
  }
  if (driveWanted && now - lastBrowserDrive >= kBrowserLeaseMs) {
    clearDrive();
    queueStop();
  }
  if (motionWanted && (now-supervisorAt >= kSupervisorLeaseMs ||
      (motionPending[0] && now-motionQueuedAt >= 1500))) {
    const char* reason = now-supervisorAt >= kSupervisorLeaseMs ? "SUPERVISOR_TIMEOUT" : "JOB_ACK_TIMEOUT";
    discardMotion(reason);
    snprintf(status.lastError,sizeof(status.lastError),"%s",reason);
    ++status.safetyEvents;
    queueStop();
  }
  if (stopPending) {
    sendStop = true;
    sendBrake = brakePending && connected && status.ready && status.brakeCapable;
    stopPending = false;
    brakePending = false;
  } else if (motionWanted && ackMotionJob) {
    dispatchJob=ackMotionJob;
    snprintf(sendMotion,sizeof(sendMotion),"M2 %08lx %lu %u ACK",
        static_cast<unsigned long>(status.motionBoot),static_cast<unsigned long>(motionSession),ackMotionJob);
    ackMotionJob=0;
  } else if (motionWanted && motionPending[0] && (!motionSentAt || now-motionSentAt >= kMotionRetryMs)) {
    dispatchJob=pendingMotionJob;
    snprintf(sendMotion,sizeof(sendMotion),"%s",motionPending);
    motionSentAt=now;
  } else if (motionWanted && motionUpdate[0]) {
    dispatchJob=static_cast<uint16_t>(status.motionJob);
    snprintf(sendMotion,sizeof(sendMotion),"%s",motionUpdate);
    motionUpdate[0]=0;
  } else if (calibrationPending && connected) {
    sendCalibration = true;
    calibrationPending = false;
  } else if (facePending != 0) {
    sendFace = facePending;
    facePending = 0;
  } else if (scenePending && connected) {
    sendScene = scenePending; sendSceneDuration=sceneDurationMs;
    scenePending = 0;
    snprintf(label, sizeof(label), "%s", sceneLabel);
  } else if (ledPending && connected) {
    sendLed = true;
    target = ledTarget;
    red = ledRed;
    green = ledGreen;
    blue = ledBlue;
    brightness = ledBrightness;
    ledPending = false;
  } else if (lookPending && connected) {
    sendLook = true;
    sendWiggle = lookWiggle;
    sendExcited = lookExcited;
    angle = lookDegrees;
    acceleration = lookAcceleration;
    lookPending = false;
  } else if (status.lookActive && lookId && now - lastKeepSent >= kDrivePeriodMs) {
    keepId = lookId;
    lastKeepSent = now;
  } else if (wakePending && connected) {
    sendWake = true;
    wakePending = false;
  } else if (driveWanted && connected && (status.ready ||
             (status.edgeReverseAllowed && !safeWanted && !turnWanted && desiredLeft<0 && desiredLeft==desiredRight)) &&
             now - lastDriveSent >= kDrivePeriodMs) {
    sendDrive = true;
    sendTurn = turnWanted;
    sendSafe = safeWanted;
    left = desiredLeft;
    right = desiredRight;
    acceleration = desiredAcceleration;
    stopDistanceMm = desiredStopMm;
    lastDriveSent = now;
  }
  if (!sendStop && motionWanted && !armPending && now-heartbeatAt >= 100) {
    snprintf(sendHeartbeat,sizeof(sendHeartbeat),"M2 %08lx %lu 0 HB %lu",
        static_cast<unsigned long>(status.motionBoot),static_cast<unsigned long>(motionSession),
        static_cast<unsigned long>(++heartbeatCounter));
    heartbeatAt=now;
  }
  dispatchGeneration=status.controlGeneration;
  dispatchSession=motionSession;
  dispatchPresentation=presentationGeneration;
  portEXIT_CRITICAL(&stateLock);

  // All UART writes belong to this task. Revalidate selected work after the
  // lock is released, then publish its reply identity under the lock before
  // transmitting. Stop invalidates generations and sessions immediately.
  portENTER_CRITICAL(&stateLock);
  if (dispatchGeneration!=status.controlGeneration) {
    sendCalibration=sendLed=sendWake=sendLook=sendDrive=false;
    sendFace=sendScene=0; keepId=0;
    sendMotion[0]=sendHeartbeat[0]=0;
  }
  if (!motionWanted || dispatchSession!=motionSession)
    sendMotion[0]=sendHeartbeat[0]=0;
  if (dispatchPresentation != presentationGeneration) {
    sendScene = 0; sendWake = false;
  }
  if (sendCalibration && calibrationStrict &&
      (!calibrationActive || millis()-status.statusTimestampMs > 250 || !calibrationStationary())) {
    sendCalibration = false;
    if (calibrationActive) { finishCalibration("MOVING"); queueStop(); }
  }
  const bool hasPrimary=sendStop || sendMotion[0] || sendCalibration || sendFace || sendScene ||
      sendLed || sendWake || sendLook || keepId || sendDrive;
  const uint16_t wire=hasPrimary ? reserveWireId() : 0;
  if (sendCalibration) { calibrationId=wire; calibrationStarted=now; }
  if (sendScene) { sceneId=wire; sceneSentAt=now; }
  if (sendScene || sendFace) {
    cancelGestureAudio();
    const int expression=sendScene ? sendScene : sendFace;
    if (!stillExpression(expression) || (sendScene && sendSceneDuration)) {
      gestureWire=wire; gestureFace=expression; gestureRequestedAt=now;
      gestureOnVisibility=stillExpression(expression);
    }
  }
  if (sendLed) { ledCommandId=wire; ledSentAt=now; }
  if (sendWake) { wakeCommandId=wire; wakeSentAt=now; }
  if (sendLook && status.lookActive) { lookId=wire; lastKeepSent=now; }
  if (sendMotion[0]) motionWires[motionWireCursor++%16]={wire,dispatchSession,dispatchJob,false};
  const uint16_t heartbeatWire=sendHeartbeat[0] ? reserveWireId() : 0;
  if (heartbeatWire) motionWires[motionWireCursor++%16]={heartbeatWire,dispatchSession,0,true};
  portEXIT_CRITICAL(&stateLock);
  if (hasPrimary) { zumoUart.print(wire); zumoUart.print(' '); }
  if (sendStop) {
    zumoUart.println(sendBrake ? "BRAKE" : "STOP");
  } else if (sendMotion[0]) {
    zumoUart.printf("%s *%04X\n",sendMotion,motionCrc(sendMotion,strlen(sendMotion)));
  } else if (sendCalibration) {
    zumoUart.println("CALIBRATE");
  } else if (sendFace) {
    zumoUart.printf("FACE %s\n", kFaceNames[sendFace - 1]);
  } else if (sendScene) {
    if (sendSceneDuration) zumoUart.printf("SCENE %s %s %lu\n",kFaceNames[sendScene-1],label,static_cast<unsigned long>(sendSceneDuration));
    else zumoUart.printf("SCENE %s %s\n", kFaceNames[sendScene - 1], label);
  } else if (sendLed) {
    if (target == -1) zumoUart.printf("LED ALL %u %u %u %u\n", red, green, blue, brightness);
    else zumoUart.printf("LED %d %u %u %u %u\n", target, red, green, blue, brightness);
  } else if (sendWake) {
    zumoUart.println("WAKE");
  } else if (sendLook) {
    if (sendWiggle) zumoUart.println(sendExcited ? "WIGGLE EXCITED" : "WIGGLE");
    else zumoUart.printf("LOOK %d %d\n", angle, acceleration);
  } else if (keepId) {
    zumoUart.printf("KEEP %u\n", keepId);
  } else if (sendDrive) {
    if (sendTurn) zumoUart.printf("TURN %d\n", left);
    else if (sendSafe && stopDistanceMm < 150) zumoUart.printf("SAFE %d %d %d %d\n", left, right, acceleration, stopDistanceMm);
    else if (sendSafe) zumoUart.printf("SAFE %d %d %d\n", left, right, acceleration);
    else zumoUart.printf("DRIVE %d %d %d\n", left, right, acceleration);
  }
  if (sendHeartbeat[0]) {
    zumoUart.print(heartbeatWire); zumoUart.print(' ');
    zumoUart.printf("%s *%04X\n",sendHeartbeat,motionCrc(sendHeartbeat,strlen(sendHeartbeat)));
  }
  if (now - lastStatusSent >= kStatusPeriodMs) {
    lastStatusSent = now;
    portENTER_CRITICAL(&stateLock);
    const auto statusWire = reserveWireId();
    if (calibrationActive && calibrationAccepted && calibrationVerifyPending) {
      calibrationVerifyId = statusWire;
      calibrationVerifyPending = false;
    }
    portEXIT_CRITICAL(&stateLock);
    zumoUart.print(statusWire); zumoUart.print(' ');
    zumoUart.println("STATUS");
  }
}

}  // namespace zumo_link
