#include "person_follow.h"

#include <cstdlib>
#include <cstring>
#include "Arduino.h"
#include "vision_service.h"
#include "zumo_link.h"

namespace {
constexpr uint32_t kDashboardLeaseMs = 1500;
constexpr uint32_t kFreshResultMs = 900;
constexpr uint32_t kTurnPulseMs = 180;
constexpr uint32_t kTurnRefreshMs = 80;
constexpr int kTurnSpeed = 4000;
constexpr int kTolerancePercent = 10;
// The first physical test showed displayed left/right steering was reversed.
constexpr bool kInvertTurnDirection = true;

portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
person_follow::Status state = {false, "Off", -1, 0, 0};
uint32_t lastHeartbeat = 0;
uint32_t lastResult = 0;
uint32_t pulseUntil = 0;
uint32_t lastTurnRefresh = 0;
int pulseDirection = 0;
uint32_t modeGeneration = 0;

void stopPulse(const char* phase) {
  bool wasTurning;
  portENTER_CRITICAL(&lock);
  if (!state.enabled) { portEXIT_CRITICAL(&lock); return; }
  wasTurning = pulseDirection != 0;
  pulseDirection = 0;
  pulseUntil = 0;
  state.phase = phase;
  portEXIT_CRITICAL(&lock);
  if (wasTurning) zumo_link::stop();
}

void disable(const char* reason) {
  portENTER_CRITICAL(&lock);
  const bool wasEnabled = state.enabled || pulseDirection != 0;
  state.enabled = false;
  ++modeGeneration;
  state.phase = reason;
  state.targetX = -1;
  state.errorPixels = 0;
  state.scorePercent = 0;
  pulseDirection = 0;
  pulseUntil = 0;
  portEXIT_CRITICAL(&lock);
  if (wasEnabled) zumo_link::stop();
}

const vision_service::Status::Box* largestPerson(const vision_service::Status& vision) {
  const vision_service::Status::Box* best = nullptr;
  int bestArea = 0;
  for (uint8_t index = 0; index < vision.boxCount; ++index) {
    const auto& box = vision.boxes[index];
    const int width = box.x2 - box.x1;
    const int height = box.y2 - box.y1;
    if (width <= 0 || height <= 0) continue;
    const int area = width * height;
    if (area > bestArea) { best = &box; bestArea = area; }
  }
  return best;
}
}  // namespace

namespace person_follow {
bool setEnabled(bool enabled) {
  if (!enabled) { disable("Off"); return true; }
  const auto vision = vision_service::snapshot();
  const auto zumo = zumo_link::snapshot();
  if (!vision.ready || !vision.enabled || !zumo.connected || !zumo.ready ||
      zumo.calibrating || zumo.edge || strcmp(zumo.fault, "NONE") != 0) return false;
  zumo_link::stop();
  const uint32_t now = millis();
  portENTER_CRITICAL(&lock);
  state.enabled = true;
  ++modeGeneration;
  state.phase = "Waiting for next person frame";
  state.targetX = -1;
  state.errorPixels = 0;
  state.scorePercent = 0;
  lastHeartbeat = now;
  lastResult = vision.resultTimestampMs;
  pulseUntil = 0;
  pulseDirection = 0;
  portEXIT_CRITICAL(&lock);
  Serial.println("Person follow: armed for turn-only tracking");
  return true;
}

bool heartbeat() {
  portENTER_CRITICAL(&lock);
  const bool enabled = state.enabled;
  if (enabled) lastHeartbeat = millis();
  portEXIT_CRITICAL(&lock);
  return enabled;
}

Status snapshot() {
  portENTER_CRITICAL(&lock);
  Status copy = state;
  portEXIT_CRITICAL(&lock);
  return copy;
}

void poll() {
  const uint32_t now = millis();
  portENTER_CRITICAL(&lock);
  const bool enabled = state.enabled;
  const uint32_t heartbeatAt = lastHeartbeat;
  const uint32_t resultAt = lastResult;
  const uint32_t turnUntil = pulseUntil;
  const uint32_t refreshedAt = lastTurnRefresh;
  const int direction = pulseDirection;
  const uint32_t generation = modeGeneration;
  portEXIT_CRITICAL(&lock);
  if (!enabled) return;
  if (now - heartbeatAt > kDashboardLeaseMs) { disable("Dashboard closed"); return; }
  const auto zumo = zumo_link::snapshot();
  if (!zumo.connected || !zumo.ready || zumo.calibrating || zumo.edge ||
      strcmp(zumo.fault, "NONE") != 0) { disable("Zumo stopped or locked"); return; }
  const auto vision = vision_service::snapshot();
  if (!vision.ready || !vision.enabled) { disable("Person detection stopped"); return; }

  if (direction && static_cast<int32_t>(now - turnUntil) >= 0) {
    stopPulse("Waiting for next person frame");
    return;
  }
  if (vision.hasResult && vision.resultTimestampMs != resultAt &&
      vision.resultAgeMs <= kFreshResultMs) {
    const auto* box = largestPerson(vision);
    const int center = vision.imageWidth / 2;
    const int tolerance = vision.imageWidth * kTolerancePercent / 100;
    const int target = box ? (box->x1 + box->x2) / 2 : -1;
    const int error = box ? target - center : 0;
    portENTER_CRITICAL(&lock);
    if (!state.enabled) { portEXIT_CRITICAL(&lock); return; }
    lastResult = vision.resultTimestampMs;
    state.targetX = target;
    state.errorPixels = error;
    state.scorePercent = box ? box->scorePercent : 0;
    portEXIT_CRITICAL(&lock);
    if (!box) { stopPulse("Searching for a person"); return; }
    if (abs(error) <= tolerance) { stopPulse("Person centered"); return; }
    const int newDirection = (error > 0 ? 1 : -1) * (kInvertTurnDirection ? -1 : 1);
    const bool accepted = zumo_link::turn(newDirection * kTurnSpeed);
    portENTER_CRITICAL(&lock);
    const bool stillEnabled = state.enabled && modeGeneration == generation;
    if (accepted && stillEnabled) {
      pulseDirection = newDirection;
      pulseUntil = now + kTurnPulseMs;
      lastTurnRefresh = now;
      state.phase = newDirection > 0 ? "Turning right" : "Turning left";
    }
    portEXIT_CRITICAL(&lock);
    if (!stillEnabled) { zumo_link::stop(); return; }
    if (!accepted) { disable("Zumo rejected turn"); return; }
    Serial.printf("Person follow: target=%d/320 error=%d px turn=%s for %lu ms\n",
                  target, error, newDirection > 0 ? "right" : "left",
                  static_cast<unsigned long>(kTurnPulseMs));
    return;
  }
  if (direction && now - refreshedAt >= kTurnRefreshMs) {
    const bool accepted = zumo_link::turn(direction * kTurnSpeed);
    portENTER_CRITICAL(&lock);
    const bool stillEnabled = state.enabled && modeGeneration == generation &&
                              pulseDirection == direction;
    if (accepted && stillEnabled) lastTurnRefresh = now;
    portEXIT_CRITICAL(&lock);
    if (!stillEnabled) { zumo_link::stop(); return; }
    if (!accepted) { disable("Zumo rejected turn"); return; }
  }
  if (vision.resultAgeMs > kFreshResultMs && direction) {
    stopPulse("Waiting for fresh person frame");
  }
}
}  // namespace person_follow
