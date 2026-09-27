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
constexpr uint32_t kStatusPeriodMs = 500;
constexpr uint32_t kLinkTimeoutMs = 2000;

zumo_link::Status status = {false, false, false, false, 0, 0, -1, 0,
                            "NONE", "NONE"};
int desiredLeft = 0;
int desiredRight = 0;
bool driveWanted = false;
bool stopPending = false;
bool calibrationPending = false;
int facePending = 0;
uint32_t lastBrowserDrive = 0;
uint32_t lastReply = 0;
uint32_t lastDriveSent = 0;
uint32_t lastStatusSent = 0;
uint16_t nextId = 1;
uint16_t calibrationId = 0;
char receiveLine[96];
size_t received = 0;
bool dropping = false;

uint16_t sendPrefix() {
  const uint16_t id = nextId;
  nextId = nextId == 65535 ? 1 : nextId + 1;
  zumoUart.print(id);
  zumoUart.print(' ');
  return id;
}

void clearDrive() {
  driveWanted = false;
  desiredLeft = 0;
  desiredRight = 0;
}

void handleLine(const char* line, uint32_t now) {
  unsigned id = 0;
  int ready = 0, left = 0, right = 0, battery = -1, edge = 0;
  char fault[24] = {};
  if (sscanf(line, "%u OK READY=%d FAULT=%23s L=%d R=%d BAT=%d EDGE=%d",
             &id, &ready, fault, &left, &right, &battery, &edge) == 7) {
    portENTER_CRITICAL(&stateLock);
    lastReply = now;
    status.connected = true;
    status.ready = ready != 0 && !status.calibrating;
    status.edge = edge != 0;
    status.left = left;
    status.right = right;
    status.batteryMillivolts = battery;
    snprintf(status.fault, sizeof(status.fault), "%s", fault);
    if (!status.ready) {
      clearDrive();
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
  if (strcmp(line, "0 EVENT READY") == 0) {
    status.ready = true;
    status.calibrating = false;
    status.edge = false;
    snprintf(status.fault, sizeof(status.fault), "NONE");
    snprintf(status.lastError, sizeof(status.lastError), "NONE");
  } else if (strcmp(line, "0 EVENT CAL_FAILED") == 0 ||
             strcmp(line, "0 EVENT EDGE") == 0 ||
             strcmp(line, "0 EVENT BUTTON_C") == 0 ||
             strcmp(line, "0 EVENT SENSOR") == 0) {
    const char* reason = line + strlen("0 EVENT ");
    status.ready = false;
    status.calibrating = false;
    status.edge = strcmp(reason, "EDGE") == 0;
    status.left = status.right = 0;
    snprintf(status.fault, sizeof(status.fault), "%s", reason);
    clearDrive();
    stopPending = true;
  } else if (strcmp(line, "0 EVENT TIMEOUT") == 0) {
    status.left = status.right = 0;
    clearDrive();
    stopPending = true;
  } else if (strstr(line, " OK CALIBRATING") != nullptr) {
    status.calibrating = true;
    status.ready = false;
  } else if (strstr(line, " ERR ") != nullptr) {
    const char* reason = strstr(line, " ERR ") + 5;
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
  result.connected = lastReply != 0 && result.replyAgeMs < kLinkTimeoutMs;
  portEXIT_CRITICAL(&stateLock);
  return result;
}

bool drive(int left, int right) {
  if (left < -2000 || left > 2000 || right < -2000 || right > 2000) {
    return false;
  }
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateLock);
  const bool allowed = status.ready && lastReply != 0 &&
                       now - lastReply < kLinkTimeoutMs &&
                       !status.calibrating;
  if (allowed) {
    desiredLeft = left;
    desiredRight = right;
    facePending = 0;
    driveWanted = left != 0 || right != 0;
    lastBrowserDrive = now;
    if (!driveWanted) {
      stopPending = true;
    }
  }
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

void stop() {
  portENTER_CRITICAL(&stateLock);
  clearDrive();
  calibrationPending = false;
  status.calibrating = false;
  stopPending = true;
  portEXIT_CRITICAL(&stateLock);
}

bool calibrate() {
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateLock);
  const bool allowed = lastReply != 0 && now - lastReply < kLinkTimeoutMs &&
                       !driveWanted && !status.calibrating;
  if (allowed) {
    status.ready = false;
    status.calibrating = true;
    snprintf(status.lastError, sizeof(status.lastError), "NONE");
    stopPending = true;
    calibrationPending = true;
  }
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

bool face(bool happy) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateLock);
  const bool allowed = lastReply != 0 && now - lastReply < kLinkTimeoutMs &&
                       !driveWanted;
  if (allowed) {
    facePending = happy ? 1 : 2;
  }
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

void poll() {
  const uint32_t now = millis();
  pollReceive(now);

  bool sendStop = false, sendCalibration = false;
  int sendFace = 0;
  bool sendDrive = false;
  int left = 0, right = 0;
  portENTER_CRITICAL(&stateLock);
  const bool connected = lastReply != 0 && now - lastReply < kLinkTimeoutMs;
  if (!connected) {
    status.connected = false;
    status.ready = false;
    status.calibrating = false;
    clearDrive();
  }
  if (driveWanted && now - lastBrowserDrive >= kBrowserLeaseMs) {
    clearDrive();
    stopPending = true;
  }
  if (stopPending) {
    sendStop = true;
    stopPending = false;
  } else if (calibrationPending) {
    sendCalibration = true;
    calibrationPending = false;
  } else if (facePending != 0) {
    sendFace = facePending;
    facePending = 0;
  } else if (driveWanted && connected && status.ready &&
             now - lastDriveSent >= kDrivePeriodMs) {
    sendDrive = true;
    left = desiredLeft;
    right = desiredRight;
    lastDriveSent = now;
  }
  portEXIT_CRITICAL(&stateLock);

  if (sendStop) {
    sendPrefix();
    zumoUart.println("STOP");
  } else if (sendCalibration) {
    calibrationId = sendPrefix();
    zumoUart.println("CALIBRATE");
  } else if (sendFace) {
    sendPrefix();
    zumoUart.println(sendFace == 1 ? "FACE HAPPY" : "FACE SAD");
  } else if (sendDrive) {
    sendPrefix();
    zumoUart.printf("DRIVE %d %d\n", left, right);
  }
  if (now - lastStatusSent >= kStatusPeriodMs) {
    lastStatusSent = now;
    sendPrefix();
    zumoUart.println("STATUS");
  }
}

}  // namespace zumo_link
