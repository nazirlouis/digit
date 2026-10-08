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
#include <freertos/semphr.h>

namespace {
using robot_actions::Command;
SemaphoreHandle_t gate = nullptr;
std::atomic<uint32_t> controlEpoch{1};
Command command = Command::None;
const char* phase = "Idle";
bool active = false;
uint32_t safetyEvents = 0;
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
void stop(const char* reason, bool smooth) {
  Guard guard;
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
  if (z.calibrating) return "Waiting for floor calibration";
  if (!z.sceneCapable) return "Update Zumo display support";
  if (value == Command::Identify) return v.batteryReady ? "" : "Object detector unavailable";
  if (z.edge) return "Edge stop: place Digit fully on the surface and recalibrate";
  if (!z.ready || strcmp(z.fault, "NONE")) return "Calibrate the floor to enable movement";
  if (!z.actionCapable || !z.autoRampCapable || !z.voiceCapable || !z.rampedLookCapable || !z.wiggleCapable)
    return "Gyroscope or updated movement firmware unavailable";
  if (!z.rangeValid || z.rangeAgeMs >= 250) return "Waiting for a fresh distance reading";
  if (value == Command::ComeSayHello) {
    if (!z.closeApproachCapable) return "Update Zumo for the 10 cm greeting approach";
    return v.ready ? "" : "Person detector unavailable";
  }
  return v.batteryReady ? "" : "Object detector unavailable";
}
bool start(Command value, uint32_t expected) {
  Guard guard;
  if (expected != controlEpoch.load()) return false;
  if (value == Command::Stop) { stop(); return true; }
  // Invalid/stale requests never interrupt the action already in progress.
  if (!validCommand(static_cast<int>(value))) return false;
  const char* reason = unavailableReason(value);
  if (reason[0]) { if (!active) phase = reason; return false; }
  stop("Starting action");
  command = value; active = true;
  safetyEvents = zumo_link::snapshot().safetyEvents;
  phase = name(value);
  if (!action_mission::start(value)) {
    stop(action_mission::phase()); return false;
  }
  return true;
}
void poll() {
  Guard guard;
  if (!active) return;
  const auto z = zumo_link::snapshot();
  if (!z.connected || z.calibrating || z.safetyEvents != safetyEvents ||
      (command != Command::Identify && (!z.ready || z.edge || strcmp(z.fault,"NONE")))) {
    stop(z.edge ? "Edge detected: place Digit fully on the surface and recalibrate" :
         "Action cancelled: Zumo stopped or locked"); return;
  }
  action_mission::poll();
  phase = action_mission::phase();
  if (!action_mission::active()) { active = false; command = Command::None; }
}
}
