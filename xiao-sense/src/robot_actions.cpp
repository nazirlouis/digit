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
bool active = false, entered = false, turnIssued = false;
bool greetingVoiceStarted = false, centerVoiceStarted = false;
uint32_t stepAt = 0, safetyEvents = 0;
unsigned step = 0;
struct Step { uint32_t ms; bool wiggle; const char* face; AudioCue cue; const char* phase; };
const Step hello[] = {
  {0,true,nullptr,AudioCue::None,"Preparing hello"},
  {260,false,"NEUTRAL",AudioCue::None,"Settling"},
};

void finish(const char* reason, bool smooth = false) {
  active = false; command = Command::None; phase = reason;
  audioEngine.cancelOutput();
  zumo_link::stop(smooth);
}
void advance(uint32_t now) {
  // The completed gesture already stops. Extra STOP commands reset the happy face.
  ++step; entered = turnIssued = false; stepAt = now + 100;
}
bool wiggle(uint32_t now) {
  const auto z = zumo_link::snapshot();
  if (!turnIssued) {
    if (zumo_link::wiggle(z.controlGeneration)) { turnIssued = true; stepAt = now; }
    else if (static_cast<int32_t>(now - stepAt) > 1500) finish("Turn could not start");
    return false;
  }
  if (z.lookActive) {
    // Zumo renders happy eyes and announces the end of stationary gyro setup.
    // Cue timing follows the real gesture, not an estimate of its duration.
    if (!greetingVoiceStarted && !strcmp(z.lookResult, "WIGGLING")) {
      greetingVoiceStarted = true;
      phase = "Saying hello";
      if (!audioEngine.playCue(AudioCue::Happy)) finish("Greeting audio unavailable");
    }
    if (!centerVoiceStarted && !strcmp(z.lookResult, "CENTERING")) {
      centerVoiceStarted = greetingVoiceStarted = true;
      phase = "Hello: back to center";
      if (!audioEngine.playCue(AudioCue::Neutral)) finish("Greeting audio unavailable");
    }
    return false;
  }
  if (strcmp(z.lookResult, "DONE")) { finish("Turn interrupted or failed"); return false; }
  audioEngine.cancelOutput(); // No separate sound or face replay after the wiggle.
  turnIssued = false;
  return true;
}
void sequence(const Step* steps, unsigned count, uint32_t now) {
  if (step >= count) { finish("Routine complete"); return; }
  if (static_cast<int32_t>(now - stepAt) < 0) return;
  const auto& s = steps[step];
  if (!entered) {
    const auto z = zumo_link::snapshot();
    if (z.left || z.right) {
      if (now - stepAt > 1000) finish("Waiting for motors to stop failed");
      return;
    }
    entered = true; stepAt = now; phase = s.phase;
    if (s.face) zumo_link::face(s.face);
    if (s.cue != AudioCue::None) audioEngine.playCue(s.cue);
  }
  if (s.wiggle) { if (wiggle(now)) advance(now); return; }
  if (now - stepAt >= s.ms) { advance(now); return; }
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
    case Command::Hello: return "Say hello";
    case Command::Stop: return "Stop";
    case Command::FollowMe: return "Follow me";
    case Command::FindMe: return "Find me";
    case Command::Identify: return "What's this?";
    case Command::FindBattery: return "Find a battery";
    case Command::FindMarker: return "Find a marker";
    case Command::FindScrewdriver: return "Find a screwdriver";
    case Command::Chase: return "Chase this";
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
  if (wasFollowing) vision_service::setEnabled(false);
  if (vision_service::snapshot().batteryMode) vision_service::setEnabled(false);
  audioEngine.cancelOutput();
  finish(reason, smooth);
}
void manualOverride() {
  Guard guard;
  if (active || speech_commands::snapshot().listening || wake_word::snapshot().reacting) stop("Manual control");
  else { ++controlEpoch; wake_word::cancel(); speech_commands::cancel(); }
}
bool start(Command value, uint32_t expected) {
  Guard guard;
  if (expected != controlEpoch.load()) return false;
  if (value == Command::Stop) { stop(); return true; }
  if (!validCommand(static_cast<int>(value))) return false;
  const auto z = zumo_link::snapshot();
  const bool tracking = value == Command::FollowMe;
  const bool stationary = value == Command::Identify;
  if (!stationary && z.edge) {
    phase = "Edge stop: place Digit fully on the surface and recalibrate"; return false;
  }
  if (!z.connected || z.calibrating || (!stationary &&
      (!z.ready || z.edge || strcmp(z.fault,"NONE") || !z.actionCapable))) {
    phase = "Calibrate the connected, updated Zumo first"; return false;
  }
  if (!stationary && !tracking && !z.voiceCapable) {
    phase = "Gyroscope unavailable"; return false;
  }
  if (!stationary && !tracking && (!z.rampedLookCapable || !z.wiggleCapable)) {
    phase = "Update Zumo turn files for synchronized greetings"; return false;
  }
  if (tracking && !vision_service::snapshot().ready) {
    phase = "Person detector unavailable"; return false;
  }
  if (value != Command::Hello && !tracking) {
    if (value == Command::FindMe && !z.closeApproachCapable) {
      phase = "Update Zumo for the 10 cm approach"; return false;
    }
    if (value == Command::Chase && !z.chaseCloseCapable) {
      phase = "Update Zumo for the 5 cm chase"; return false;
    }
    const auto v = vision_service::snapshot();
    if (!z.sceneCapable || (!stationary && !z.autoRampCapable)) {
      phase = "Update Zumo action and display files first"; return false;
    }
    if (!v.ready || (value != Command::FindMe && !v.batteryReady)) {
      phase = "Required detection model is unavailable"; return false;
    }
  }
  stop("Starting routine");
  command = value; active = true; stepAt = millis() + 200;
  safetyEvents = z.safetyEvents;
  step = 0;
  entered = turnIssued = false;
  greetingVoiceStarted = centerVoiceStarted = false;
  phase = name(value);
  if (tracking) {
    if (!vision_service::setEnabled(true) || !person_follow::setEnabled(true)) {
      vision_service::setEnabled(false); finish("Person tracking unavailable"); return false;
    }
  } else if (value != Command::Hello && !action_mission::start(value)) {
    finish(action_mission::phase()); return false;
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
         "Routine cancelled: Zumo stopped or locked"); return;
  }
  const uint32_t now = millis();
  if (command == Command::FollowMe) {
    const auto follow = person_follow::snapshot();
    if (!follow.enabled) {
      vision_service::setEnabled(false); finish(follow.phase); return;
    }
    person_follow::heartbeat();
    phase = follow.phase;
  } else if (command == Command::Hello) sequence(hello, sizeof(hello)/sizeof(hello[0]), now);
  else {
    action_mission::poll();
    phase = action_mission::phase();
    if (!action_mission::active()) { active = false; command = Command::None; }
  }
}
}
