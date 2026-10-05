#include "robot_actions.h"
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
uint32_t stepAt = 0, safetyEvents = 0;
unsigned step = 0;
struct Step { uint32_t ms; int angle; const char* face; AudioCue cue; const char* phase; };
const Step hello[] = {
  {2700,0,"HAPPY",AudioCue::Hello,"Saying hello"},
  {0,15,nullptr,AudioCue::None,"Hello wiggle"},
  {0,-30,nullptr,AudioCue::None,"Hello wiggle"},
  {0,15,nullptr,AudioCue::None,"Turning back"},
  {900,0,"HAPPY",AudioCue::None,"Hello!"},
};

void finish(const char* reason, bool smooth = false) {
  active = false; command = Command::None; phase = reason;
  audioEngine.cancelOutput();
  zumo_link::stop(smooth);
}
void advance(uint32_t now) {
  zumo_link::stop(); ++step; entered = turnIssued = false; stepAt = now + 150;
}
bool turn(int degrees, uint32_t now) {
  const auto z = zumo_link::snapshot();
  if (!turnIssued) {
    if (zumo_link::look(degrees, z.controlGeneration)) { turnIssued = true; stepAt = now; }
    else if (static_cast<int32_t>(now - stepAt) > 1500) finish("Turn could not start");
    return false;
  }
  if (z.lookActive) return false;
  if (strcmp(z.lookResult, "DONE")) { finish("Turn interrupted or failed"); return false; }
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
  if (s.angle) { if (turn(s.angle, now)) advance(now); return; }
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
    default: return "Unknown";
  }
}
Status snapshot() { Guard guard; return {command, phase, active, controlEpoch.load()}; }
void stop(const char* reason, bool smooth) {
  Guard guard;
  ++controlEpoch;
  wake_word::cancel(); speech_commands::cancel(); photo_store::cancel();
  person_follow::setEnabled(false);
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
  if (value != Command::Hello) return false;
  const auto z = zumo_link::snapshot();
  if (!z.connected || !z.ready || z.calibrating || z.edge || strcmp(z.fault,"NONE") || !z.actionCapable) {
    phase = "Calibrate the connected, updated Zumo first"; return false;
  }
  if (!z.voiceCapable) {
    phase = "Gyroscope unavailable"; return false;
  }
  stop("Starting routine");
  command = value; active = true; stepAt = millis() + 200;
  safetyEvents = z.safetyEvents;
  step = 0;
  entered = turnIssued = false;
  phase = name(value);
  return true;
}
void poll() {
  Guard guard;
  if (!active) return;
  const auto z = zumo_link::snapshot();
  if (!z.connected || !z.ready || z.calibrating || z.edge || strcmp(z.fault,"NONE") || z.safetyEvents != safetyEvents) {
    stop("Routine cancelled: Zumo stopped or locked"); return;
  }
  const uint32_t now = millis();
  if (command == Command::Hello) sequence(hello, sizeof(hello)/sizeof(hello[0]), now);

}
}
