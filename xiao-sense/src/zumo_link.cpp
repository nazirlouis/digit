#include "zumo_link.h"

#include <cstring>

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

zumo_link::Status status = {false, false, false, false, 0, 0, -1, 0,
                            "NONE", "NONE", "NONE", "NONE", false, false, 0, "NONE", false, false, false, -1, 0, false, false, false, false, false, "NONE", UINT32_MAX};
int desiredLeft = 0;
int desiredRight = 0;
int desiredAcceleration = 6000;
bool driveWanted = false;
bool turnWanted = false;
bool safeWanted = false;
bool stopPending = false;
bool brakePending = false;
bool calibrationPending = false;
int facePending = 0;
int scenePending = 0;
char sceneLabel[16] = {};
uint16_t sceneId = 0;
uint32_t sceneSentAt = 0;
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
uint32_t lastDriveSent = 0;
uint32_t lastStatusSent = 0;
uint16_t nextId = 1;
uint16_t calibrationId = 0;
char receiveLine[192];
size_t received = 0;
bool dropping = false;

uint16_t sendPrefix() {
  const uint16_t id = nextId;
  nextId = nextId == 65535 ? 1 : nextId + 1;
  zumoUart.print(id);
  zumoUart.print(' ');
  return id;
}

void queueStop(bool smooth = false) {
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

void manualOverride() {
  ++status.controlGeneration;
  scenePending = 0;
  sceneId = 0;
  finishLook("CANCELLED", true);
}

void handleLine(const char* line, uint32_t now) {
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
    status.batteryMillivolts = battery;
    status.voiceCapable = voice == 1;
    int distance = -1, valid = 0, capable = 0;
    const char* range = strstr(line, " DIST=");
    if (range) sscanf(range, " DIST=%d RANGE=%d AUTO=%d", &distance, &valid, &capable);
    lastRangeReply = range ? now : 0;
    status.distanceMm = distance;
    status.rangeValid = valid == 1;
    status.actionCapable = capable == 1;
    const char* brake = strstr(line, " BRAKE=");
    status.brakeCapable = brake && brake[7] == '1';
    status.rampedLookCapable = strstr(line, " LOOKRAMP=1") != nullptr;
    status.wiggleCapable = strstr(line, " WIGGLE=2") != nullptr;
    status.sceneCapable = strstr(line, " SCENE=1") != nullptr;
    status.autoRampCapable = strstr(line, " SAFE2=1") != nullptr;
    snprintf(status.fault, sizeof(status.fault), "%s", fault);
    if (!status.ready) {
      clearDrive();
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
    snprintf(status.sceneResult, sizeof(status.sceneResult), "%s",
             strstr(line, " OK SCENE") ? "APPLIED" : error ? error + 5 : "BAD_REPLY");
    sceneId = 0;
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
             strstr(line, " OK WAKE") ? "APPLIED" : error ? error + 5 : "BAD_REPLY");
    wakeCommandId = 0;
  }
  if (strcmp(line, "0 EVENT WAKE_FAILED") == 0)
    snprintf(status.wakeResult, sizeof(status.wakeResult), "WAKE_IO");
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
    status.ready = true;
    status.calibrating = false;
    status.edge = false;
    snprintf(status.fault, sizeof(status.fault), "NONE");
    snprintf(status.lastError, sizeof(status.lastError), "NONE");
  } else if (strcmp(line, "0 EVENT CAL_FAILED") == 0 ||
             strcmp(line, "0 EVENT EDGE") == 0 ||
             strcmp(line, "0 EVENT BUTTON_C") == 0 ||
             strcmp(line, "0 EVENT TURN_FAULT") == 0 ||
             strcmp(line, "0 EVENT SENSOR") == 0) {
    const char* reason = line + strlen("0 EVENT ");
    ++status.safetyEvents;
    status.ready = false;
    status.calibrating = false;
    status.edge = strcmp(reason, "EDGE") == 0;
    status.left = status.right = 0;
    snprintf(status.fault, sizeof(status.fault), "%.23s", reason);
    clearDrive();
    finishLook(reason, true);
    queueStop();
  } else if (strcmp(line, "0 EVENT TIMEOUT") == 0 ||
             strcmp(line, "0 EVENT OBSTACLE") == 0) {
    ++status.safetyEvents;
    status.left = status.right = 0;
    clearDrive();
    finishLook("TIMEOUT", true);
    queueStop();
  } else if (strstr(line, " OK CALIBRATING") != nullptr) {
    status.calibrating = true;
    status.ready = false;
  } else if (strstr(line, " ERR ") != nullptr) {
    const char* reason = strstr(line, " ERR ") + 5;
    if (!strcmp(reason,"OBSTACLE") || !strcmp(reason,"RANGE_UNAVAILABLE") || !strcmp(reason,"NOT_READY")) {
      ++status.safetyEvents; clearDrive(); queueStop();
    }
    snprintf(status.lastError, sizeof(status.lastError), "%s", reason);
    if (id == calibrationId) {
      status.calibrating = false;
      status.ready = false;
    }
  }
  portEXIT_CRITICAL(&stateLock);
}

void pollReceive(uint32_t now) {
  // A bound keeps a busy or malformed peer from delaying motor control.
  for (int count = 0; count < 128 && zumoUart.available(); ++count) {
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
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateLock);
  Status result = status;
  result.replyAgeMs = lastReply == 0 ? UINT32_MAX : now - lastReply;
  result.rangeAgeMs = lastRangeReply == 0 ? UINT32_MAX : now - lastRangeReply;
  result.connected = lastReply != 0 && result.replyAgeMs < kLinkTimeoutMs;
  result.manualDriving = driveWanted;
  portEXIT_CRITICAL(&stateLock);
  return result;
}

bool drive(int left, int right, int acceleration) {
  if (left < -6000 || left > 6000 || right < -6000 || right > 6000 ||
      acceleration < 500 || acceleration > 6000) {
    return false;
  }
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateLock);
  const bool allowed = status.ready && lastReply != 0 &&
                       now - lastReply < kLinkTimeoutMs &&
                       !status.calibrating;
  if (allowed) {
    manualOverride();
    desiredLeft = left;
    desiredRight = right;
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
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateLock);
  const bool allowed = status.ready && lastReply != 0 &&
                       now - lastReply < kLinkTimeoutMs && !status.calibrating;
  if (allowed) {
    manualOverride();
    desiredLeft = speed;
    desiredRight = -speed;
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

bool safeDrive(int left, int right) {
  if (left < 0 || left > 3200 || right < 0 || right > 3200) return false;
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateLock);
  const bool allowed = status.ready && !status.edge && !strcmp(status.fault, "NONE") &&
      lastReply && now - lastReply < 500 && status.actionCapable &&
      status.rangeValid && status.autoRampCapable && !status.calibrating && !status.lookActive &&
      lastRangeReply && now - lastRangeReply < 250 &&
      !stopPending && !facePending && !scenePending && !sceneId &&
      (left + right <= 0 || status.distanceMm > 150);
  if (allowed) {
    desiredLeft = left;
    desiredRight = right;
    desiredAcceleration = configuredAcceleration;
    driveWanted = left || right;
    turnWanted = false;
    safeWanted = true;
    lastBrowserDrive = now;
    if (!driveWanted) queueStop();
  }
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

bool calibrate() {
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateLock);
  const bool allowed = lastReply != 0 && now - lastReply < kLinkTimeoutMs &&
                       !driveWanted && !status.calibrating;
  if (allowed) {
    manualOverride();
    status.ready = false;
    status.calibrating = true;
    snprintf(status.lastError, sizeof(status.lastError), "NONE");
    queueStop();
    calibrationPending = true;
  }
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

bool isFaceName(const char* name) {
  return faceIndex(name) != 0;
}

bool wake() {
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateLock);
  const bool connected = lastReply != 0 && now - lastReply < kLinkTimeoutMs;
  const bool allowed = connected && !driveWanted && !status.lookActive && !status.calibrating &&
                       !calibrationPending && !stopPending && !facePending && !ledPending &&
                       !wakePending && !wakeCommandId && status.left == 0 && status.right == 0;
  if (allowed) { wakePending = true; wakeSentAt = now; }
  snprintf(status.wakeResult, sizeof(status.wakeResult), "%s",
           allowed ? "SENDING" : connected ? "BUSY" : "DISCONNECTED");
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

static bool requestTurn(int degrees, uint32_t expectedControlGeneration, bool wiggle) {
  if (degrees < -180 || degrees > 180) return false;
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateLock);
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

bool scene(const char* expression, const char* label) {
  const int face = faceIndex(expression);
  if (!face || !label || !label[0] || strlen(label) >= sizeof(sceneLabel)) return false;
  for (const char* p = label; *p; ++p) if ((*p < 'A' || *p > 'Z') && *p != '_') return false;
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateLock);
  const bool allowed = lastReply && now - lastReply < 500 && status.sceneCapable &&
      !status.left && !status.right && !status.lookActive && !driveWanted && !stopPending &&
      !status.calibrating && !scenePending && !sceneId && !facePending && !wakePending && !wakeCommandId;
  if (allowed) {
    scenePending = face;
    snprintf(sceneLabel, sizeof(sceneLabel), "%s", label);
    snprintf(status.sceneResult, sizeof(status.sceneResult), "SENDING");
  }
  portEXIT_CRITICAL(&stateLock);
  return allowed;
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
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateLock);
  const bool allowed = lastReply != 0 && now - lastReply < kLinkTimeoutMs &&
                       !driveWanted && !status.lookActive && !status.calibrating &&
                       status.left == 0 && status.right == 0;
  if (allowed) {
    facePending = index;
    if (wakePending) snprintf(status.wakeResult, sizeof(status.wakeResult), "CANCELLED");
    wakePending = false;
  }
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

bool led(int target, uint8_t red, uint8_t green, uint8_t blue,
         uint8_t brightness) {
  if (target < -1 || target > 5 || brightness > 31) return false;
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateLock);
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
  const uint32_t now = millis();
  pollReceive(now);

  bool sendStop = false, sendBrake = false, sendCalibration = false;
  int sendFace = 0;
  bool sendLed = false;
  bool sendWake = false;
  bool sendLook = false;
  bool sendWiggle = false;
  bool sendExcited = false;
  int sendScene = 0;
  char label[16] = {};
  uint16_t keepId = 0;
  int angle = 0;
  int target = -1;
  uint8_t red = 0, green = 0, blue = 0, brightness = 0;
  bool sendDrive = false;
  bool sendTurn = false;
  bool sendSafe = false;
  int left = 0, right = 0, acceleration = 6000;
  portENTER_CRITICAL(&stateLock);
  const bool connected = lastReply != 0 && now - lastReply < kLinkTimeoutMs;
  const uint32_t turnDeadline = lookWiggle ? 5000U :
      6500U + 2U * ((2400000U + lookAcceleration - 1) / lookAcceleration);
  if (status.lookActive && (!connected || !status.ready || status.calibrating ||
      driveWanted || stopPending || now - lookStarted > turnDeadline ||
      (!strcmp(status.lookResult, "SENDING") && now - lookStarted > 1000))) {
    finishLook(!connected ? "DISCONNECTED" : "CANCELLED", true);
  }
  if (wakeCommandId && now - wakeSentAt >= 1000) {
    wakeCommandId = 0;
    snprintf(status.wakeResult, sizeof(status.wakeResult), "NO_REPLY");
  }
  if (sceneId && now - sceneSentAt >= 1000) {
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
  if (stopPending) {
    sendStop = true;
    sendBrake = brakePending && connected && status.ready && status.brakeCapable;
    stopPending = false;
    brakePending = false;
  } else if (calibrationPending) {
    sendCalibration = true;
    calibrationPending = false;
  } else if (facePending != 0) {
    sendFace = facePending;
    facePending = 0;
  } else if (scenePending && connected) {
    sendScene = scenePending;
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
  } else if (driveWanted && connected && status.ready &&
             now - lastDriveSent >= kDrivePeriodMs) {
    sendDrive = true;
    sendTurn = turnWanted;
    sendSafe = safeWanted;
    left = desiredLeft;
    right = desiredRight;
    acceleration = desiredAcceleration;
    lastDriveSent = now;
  }
  portEXIT_CRITICAL(&stateLock);

  if (sendStop) {
    sendPrefix();
    zumoUart.println(sendBrake ? "BRAKE" : "STOP");
  } else if (sendCalibration) {
    calibrationId = sendPrefix();
    zumoUart.println("CALIBRATE");
  } else if (sendFace) {
    sendPrefix();
    zumoUart.printf("FACE %s\n", kFaceNames[sendFace - 1]);
  } else if (sendScene) {
    sceneId = sendPrefix();
    sceneSentAt = now;
    zumoUart.printf("SCENE %s %s\n", kFaceNames[sendScene - 1], label);
  } else if (sendLed) {
    ledCommandId = sendPrefix();
    ledSentAt = now;
    if (target == -1) zumoUart.printf("LED ALL %u %u %u %u\n", red, green, blue, brightness);
    else zumoUart.printf("LED %d %u %u %u %u\n", target, red, green, blue, brightness);
  } else if (sendWake) {
    const uint16_t id = sendPrefix();
    portENTER_CRITICAL(&stateLock);
    wakeCommandId = id;
    wakeSentAt = now;
    portEXIT_CRITICAL(&stateLock);
    zumoUart.println("WAKE");
  } else if (sendLook) {
    const uint16_t id = sendPrefix();
    portENTER_CRITICAL(&stateLock);
    // A concurrent manual request will queue STOP immediately after this line.
    if (status.lookActive) { lookId = id; lastKeepSent = now; }
    portEXIT_CRITICAL(&stateLock);
    if (sendWiggle) zumoUart.println(sendExcited ? "WIGGLE EXCITED" : "WIGGLE");
    else zumoUart.printf("LOOK %d %d\n", angle, acceleration);
  } else if (keepId) {
    sendPrefix();
    zumoUart.printf("KEEP %u\n", keepId);
  } else if (sendDrive) {
    sendPrefix();
    if (sendTurn) zumoUart.printf("TURN %d\n", left);
    else if (sendSafe) zumoUart.printf("SAFE %d %d %d\n", left, right, acceleration);
    else zumoUart.printf("DRIVE %d %d %d\n", left, right, acceleration);
  }
  if (now - lastStatusSent >= kStatusPeriodMs) {
    lastStatusSent = now;
    sendPrefix();
    zumoUart.println("STATUS");
  }
}

}  // namespace zumo_link
