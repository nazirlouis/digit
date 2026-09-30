#include "robot_actions.h"
#include "Arduino.h"
#include "audio_engine.h"
#include "camera_service.h"
#include "person_follow.h"
#include "photo_store.h"
#include "speech_commands.h"
#include "vision_service.h"
#include "wake_word.h"
#include "zumo_link.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <freertos/semphr.h>

namespace {
using robot_actions::Command;
SemaphoreHandle_t gate = nullptr;
std::atomic<uint32_t> controlEpoch{1};
Command command = Command::None;
const char* phase = "Idle";
bool active = false, ownsVision = false, entered = false, turnIssued = false;
bool pulsing = false, seenPerson = false, spokeLost = false;
uint32_t started = 0, stepAt = 0, pulseAt = 0, lastRefresh = 0, lastResult = 0;
uint32_t safetyEvents = 0;
unsigned step = 0, centered = 0, scanCount = 0;
int pulseLeft = 0, pulseRight = 0, nearest = -1;
bool scanPerson = false;
struct Step { uint32_t ms; int left, right, angle; const char* face; AudioCue cue; const char* phase; };
const Step hello[] = {
  {2700,0,0,0,"HAPPY",AudioCue::Hello,"Saying hello"},
  {0,0,0,15,nullptr,AudioCue::None,"Hello wiggle"},
  {0,0,0,-30,nullptr,AudioCue::None,"Hello wiggle"},
  {0,0,0,15,nullptr,AudioCue::None,"Turning back"},
  {900,0,0,0,"HAPPY",AudioCue::None,"Hello!"},
};
const Step dance[] = {
  {500,0,0,0,"EXCITED",AudioCue::Dance,"Starting dance"},
  {180,-1400,1400,0,nullptr,AudioCue::None,"Wiggle left"},
  {180,1400,-1400,0,nullptr,AudioCue::None,"Wiggle right"},
  {350,0,0,0,"HAPPY",AudioCue::None,"Happy beat"},
  {180,-1400,1400,0,nullptr,AudioCue::None,"Wiggle left"},
  {180,1400,-1400,0,nullptr,AudioCue::None,"Wiggle right"},
  {350,0,0,0,"SURPRISED",AudioCue::None,"Surprise beat"},
  {220,1200,1200,0,nullptr,AudioCue::None,"Shuffle forward"},
  {220,-1200,-1200,0,nullptr,AudioCue::None,"Shuffle back"},
  {400,0,0,0,"EXCITED",AudioCue::Dance,"Final spin"},
  {0,0,0,180,nullptr,AudioCue::None,"Final spin"},
  {0,0,0,180,nullptr,AudioCue::None,"Final spin"},
  {1000,0,0,0,"HAPPY",AudioCue::Photo,"Dance complete"},
};

void finish(const char* reason) {
  active = false; command = Command::None; phase = reason; pulsing = false;
  audioEngine.cancelOutput();
  zumo_link::stop();
  if (ownsVision) vision_service::setEnabled(false);
  ownsVision = false;
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
  if ((s.left || s.right) && (!lastRefresh || now - lastRefresh >= 70)) {
    if (!zumo_link::safeDrive(s.left, s.right)) { finish("Movement blocked by distance sensor"); return; }
    lastRefresh = now;
  }
}
bool enableVision() {
  const auto vision = vision_service::snapshot();
  ownsVision = !vision.enabled;
  return vision.ready && (vision.enabled || vision_service::setEnabled(true));
}
void stopPulse() {
  if (pulsing) zumo_link::stop();
  pulsing = false;
}
void trackPerson(uint32_t now, bool photo) {
  const auto vision = vision_service::snapshot();
  const auto z = zumo_link::snapshot();
  if (!vision.ready || !vision.enabled) { finish("Person detection stopped"); return; }
  if (photo && now - started > 45000) { finish("Picture cancelled: no centered person"); return; }
  if (pulsing && now - pulseAt >= 120) stopPulse();
  if (!vision.hasResult || vision.resultAgeMs > 900) {
    stopPulse(); centered = 0; phase = "Waiting for a fresh camera result"; return;
  }
  const vision_service::Status::Box* box = nullptr;
  int bestArea = 0;
  for (uint8_t i = 0; i < vision.boxCount; ++i) {
    const auto& b = vision.boxes[i];
    const int area = (b.x2-b.x1)*(b.y2-b.y1);
    if (b.x2 > b.x1 && b.y2 > b.y1 && area > bestArea) { bestArea = area; box = &b; }
  }
  const int error = box ? (box->x1+box->x2)/2 - vision.imageWidth/2 : 0;
  const bool aligned = box && abs(error) <= vision.imageWidth/10;
  if (photo && step == 1) {
    if (!aligned) { step = 0; centered = 0; audioEngine.cancelOutput(); phase = "Centering again"; }
    else if (now-stepAt >= 3300 && !audioEngine.cuePlaying()) {
      if (!photo_store::capture(controlEpoch.load())) { finish("Photo capture unavailable"); return; }
      step = 2; phase = "Saving picture";
    }
    return;
  }
  if (photo && step == 2) {
    if (photo_store::busy()) return;
    const bool failed = photo_store::failed();
    finish(failed ? "Photo capture failed" : "Picture ready in dashboard");
    if (!failed) { zumo_link::face("HAPPY"); audioEngine.playCue(AudioCue::Photo); }
    return;
  }
  if (vision.resultTimestampMs == lastResult) return;
  lastResult = vision.resultTimestampMs;
  stopPulse();
  if (!box) {
    centered = 0; phase = "Searching for a person";
    if (seenPerson && !spokeLost) { zumo_link::face("CONFUSED"); audioEngine.playCue(AudioCue::Lost); spokeLost = true; }
    return;
  }
  if (!seenPerson || spokeLost) {
    seenPerson = true; spokeLost = false;
    zumo_link::face("HAPPY");
    audioEngine.playCue(photo ? AudioCue::Person : AudioCue::Following);
  }
  if (aligned) {
    if (photo) {
      if (++centered >= 2) {
        step = 1; stepAt = now; phase = "Three, two, one";
        zumo_link::face("SURPRISED"); audioEngine.playCue(AudioCue::Countdown);
      }
      return;
    }
    if (!z.rangeValid || z.distanceMm <= 650) { phase = "Keeping distance"; return; }
    // Do not approach a large nearby box even if the range beam misses them.
    if (box->y2-box->y1 >= vision.imageHeight*8/10) { phase = "Person is close"; return; }
    pulseLeft = pulseRight = z.distanceMm > 1000 ? 1600 : 1000;
    phase = "Following";
  } else {
    centered = 0;
    // Same direction correction as the physically tested dashboard tracker.
    pulseLeft = error > 0 ? -1400 : 1400; pulseRight = -pulseLeft;
    phase = "Centering person";
  }
  if (!zumo_link::safeDrive(pulseLeft, pulseRight)) { finish("Following blocked by distance sensor"); return; }
  pulsing = true; pulseAt = now;
}
void explore(uint32_t now) {
  const auto z = zumo_link::snapshot();
  if (step == 0) {
    if (!z.rangeValid || z.replyAgeMs > 400) { finish("Explore stopped: distance unavailable"); return; }
    phase = "Exploring";
    if (z.distanceMm <= 230) {
      zumo_link::stop(); step = 1; stepAt = now + 200; turnIssued = false;
      phase = "Obstacle ahead: turning"; return;
    }
    if (now - lastRefresh >= 70) {
      const int speed = z.distanceMm < 400 ? 900 : 1600;
      if (!zumo_link::safeDrive(speed,speed)) { finish("Explore movement rejected"); return; }
      lastRefresh = now;
    }
  } else if (step == 1 && static_cast<int32_t>(now-stepAt) >= 0 && turn(90, now)) {
    step = 2; stepAt = now + 250;
  }
  if (step == 2 && static_cast<int32_t>(now-stepAt) >= 0) { step = 0; }
}
void lookAround(uint32_t now) {
  static const int angles[] = {45,-90,45};
  if (step < 6) {
    if (static_cast<int32_t>(now-stepAt) < 0) return;
    if (step % 2 == 0) {
      phase = "Looking around";
      if (turn(angles[step/2], now)) { ++step; stepAt = now; lastResult = vision_service::snapshot().resultTimestampMs; }
    } else {
      const auto v = vision_service::snapshot();
      const auto z = zumo_link::snapshot();
      if (z.rangeValid && (nearest < 0 || z.distanceMm < nearest)) nearest = z.distanceMm;
      if (v.hasResult && v.resultTimestampMs != lastResult && v.resultAgeMs < 900) {
        scanPerson |= v.personDetected;
        if (!entered) { zumo_link::face(v.personDetected ? "HAPPY" : "CURIOUS"); entered = true; }
      }
      if (now-stepAt >= 1700) { ++step; entered = false; stepAt = now; }
    }
  } else {
    finish(scanPerson ? "Scan complete: person detected" : nearest > 0 && nearest < 400 ?
        "Scan complete: nearby obstacle" : nearest < 0 ? "Scan complete: range unavailable" : "Scan complete: clear ahead");
    audioEngine.playCue(scanPerson ? AudioCue::Person : nearest > 0 && nearest >= 400 ? AudioCue::Clear : AudioCue::Obstacle);
  }
}
}
namespace robot_actions {
Guard::Guard() { if (gate) xSemaphoreTakeRecursive(gate, portMAX_DELAY); }
Guard::~Guard() { if (gate) xSemaphoreGiveRecursive(gate); }
void begin() { gate = xSemaphoreCreateRecursiveMutex(); configASSERT(gate); photo_store::begin(); }
uint32_t generation() { return controlEpoch.load(); }
const char* name(Command value) {
  static const char* names[] = {"Idle","Say hello","Follow me","Do a dance","Look around","Take my picture","Explore","Stop"};
  const unsigned index = static_cast<unsigned>(value);
  return index < 8 ? names[index] : "Unknown";
}
Status snapshot() { Guard guard; return {command, phase, active, controlEpoch.load()}; }
void stop(const char* reason) {
  Guard guard;
  ++controlEpoch;
  wake_word::cancel(); speech_commands::cancel(); photo_store::cancel();
  person_follow::setEnabled(false);
  audioEngine.cancelOutput();
  finish(reason);
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
  if (value < Command::Hello || value > Command::Explore) return false;
  const auto z = zumo_link::snapshot();
  if (!z.connected || !z.ready || z.calibrating || z.edge || strcmp(z.fault,"NONE") || !z.actionCapable) {
    phase = "Calibrate the connected, updated Zumo first"; return false;
  }
  if ((value == Command::Hello || value == Command::Dance || value == Command::LookAround || value == Command::Explore) && !z.voiceCapable) {
    phase = "Gyroscope unavailable"; return false;
  }
  if ((value == Command::Follow || value == Command::Dance || value == Command::Explore || value == Command::Picture) && !z.rangeValid) {
    phase = "Front distance sensor unavailable"; return false;
  }
  if ((value == Command::Follow || value == Command::Picture || value == Command::LookAround) && !vision_service::snapshot().ready) {
    phase = "Person detection unavailable"; return false;
  }
  stop("Starting routine");
  command = value; active = true; started = millis(); stepAt = started + 200;
  safetyEvents = z.safetyEvents;
  step = centered = scanCount = 0; lastResult = lastRefresh = 0;
  entered = turnIssued = pulsing = seenPerson = spokeLost = scanPerson = false; nearest = -1;
  if (value == Command::Follow || value == Command::Picture || value == Command::LookAround) {
    if (!enableVision()) { finish("Person detection unavailable"); return false; }
  }
  if (value == Command::Explore) audioEngine.playCue(AudioCue::Explore);
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
  else if (command == Command::Dance) sequence(dance, sizeof(dance)/sizeof(dance[0]), now);
  else if (command == Command::Follow || command == Command::Picture) trackPerson(now, command == Command::Picture);
  else if (command == Command::Explore) explore(now);
  else if (command == Command::LookAround) lookAround(now);
}
}
