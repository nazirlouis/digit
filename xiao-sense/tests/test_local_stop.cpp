#include "local_stop_gate.h"
#include "local_stop.h"
#include "speech_commands.h"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
local_stop::Status state{};
local_stop::Gate gate;
bool pending = false;
local_stop::Tag pendingTag{};
float pendingScore = 0;
uint32_t nowMs = 1000;
unsigned criticalDepth = 0, controlDepth = 0;
}
#define portENTER_CRITICAL(x) (++criticalDepth)
#define portEXIT_CRITICAL(x) (assert(criticalDepth > 0), --criticalDepth)
uint32_t millis() { return nowMs; }

namespace speech_commands {
Status testSpeech{};
Status snapshot() { return testSpeech; }
Activity activity() {
  Activity copy{}; copy.liveActive = testSpeech.liveActive; copy.listening = testSpeech.listening;
  copy.pendingAction = testSpeech.pendingAction; return copy;
}
void cancel() { testSpeech.listening = testSpeech.pendingAction = testSpeech.liveActive = false; }
}
namespace zumo_link {
struct Status { bool manualDriving = false, lookActive = false, motionActive = false; } testZumo;
Status snapshot() { return testZumo; }
}
namespace wake_word {
struct Status { bool reacting = false; } testWake;
Status snapshot() { return testWake; }
}
namespace robot_actions {
struct Status { bool active = false; uint32_t generation = 42; } testActions;
unsigned stops = 0, starts = 0;
class Guard {
 public:
  Guard() { ++controlDepth; }
  ~Guard() { assert(controlDepth > 0); --controlDepth; }
};
Status snapshot() { return testActions; }
void stop(const char* reason) {
  assert(controlDepth > 0 && criticalDepth == 0);
  assert(!strcmp(reason, "Local voice Stop"));
  ++stops; ++testActions.generation; testActions.active = false;
  speech_commands::cancel(); local_stop::cancel();
}
// The action dispatcher uses the same generation contract as robot_actions::start.
void dispatchPending(uint32_t expectedGeneration) {
  if (speech_commands::testSpeech.pendingAction && expectedGeneration == testActions.generation) {
    ++starts; testActions.active = true;
  }
  speech_commands::testSpeech.pendingAction = false;
}
}
namespace local_stop {
// Extracted verbatim from the firmware by run_local_stop.ps1.
#include "local_stop_control.inc"
}

void gateTests() {
  using local_stop::Gate;
  Gate testGate;
  assert(!testGate.accepts(testGate.tag(100), 100));  // Never dispatch idle audio.
  assert(testGate.update(true, false, 7));
  const auto command = testGate.tag(100);
  assert(testGate.accepts(command, 100 + Gate::kFreshMs));
  assert(!testGate.accepts(command, 101 + Gate::kFreshMs));
  testGate.invalidate();  // Global Stop during inference or a dropped audio frame.
  assert(!testGate.accepts(command, 200));
  const auto inFlight = testGate.tag(300);
  assert(testGate.update(true, false, 8));  // New action/session.
  assert(!testGate.accepts(inFlight, 350));
  const auto action = testGate.tag(400);
  assert(testGate.update(true, true, 9));  // Manual takeover discards action audio.
  assert(!testGate.accepts(action, 420));
  const auto manual = testGate.tag(500);
  for (uint32_t generation = 10; generation < 20; ++generation) {
    // A held drive button renews its 100-ms lease and increments both global
    // generations. This must preserve complete Stop audio and pending results.
    assert(!testGate.update(true, true, generation));
    assert(testGate.accepts(manual, 800));
  }
  testGate.invalidate();  // Releasing Stop before a new manual session.
  assert(!testGate.accepts(manual, 800));
  const auto laterManual = testGate.tag(850);
  assert(testGate.update(true, false, 21));  // A new action cannot inherit it.
  assert(!testGate.accepts(laterManual, 900));
  const auto recent = testGate.tag(1000);
  assert(testGate.update(false, false, 21));
  assert(!testGate.accepts(recent, 1010));  // Routine ended before dispatch.
  testGate.update(true, false, 22);
  assert(testGate.accepts(testGate.tag(UINT32_MAX - 100), 100));  // Clock rollover.
}

void handoffTests() {
  state.ready = true;
  speech_commands::testSpeech.listening = true;
  local_stop::poll();
  assert(state.active && robot_actions::stops == 0);
  const auto heardDuringListening = gate.tag(nowMs);
  pendingTag = heardDuringListening; pendingScore = 0.9f; pending = true;
  const uint32_t commandGeneration = robot_actions::testActions.generation;
  // Speech has finished listening and queued movement. All other activity is
  // idle: local Stop runs first in the actual main-loop order.
  speech_commands::testSpeech.listening = false;
  speech_commands::testSpeech.pendingAction = true;
  ++nowMs;
  local_stop::poll();
  robot_actions::dispatchPending(commandGeneration);
  assert(robot_actions::stops == 1 && robot_actions::starts == 0);
  assert(state.detections == 1 && state.lastScore == 0.9f && !pending);
  assert(!gate.accepts(heardDuringListening, nowMs));

  // The cancelled command's already-running model result cannot stop a newer
  // command or restore the old queued movement.
  speech_commands::testSpeech.pendingAction = true;
  pendingTag = heardDuringListening; pending = true;
  local_stop::poll();
  assert(robot_actions::stops == 1 && state.active && !pending);
  robot_actions::dispatchPending(robot_actions::testActions.generation);
  assert(robot_actions::starts == 1);

  // Completion genuinely returns the gate to idle and drops late Stop audio.
  local_stop::poll();
  const auto heardDuringAction = gate.tag(nowMs);
  robot_actions::testActions.active = false;
  pendingTag = heardDuringAction; pending = true;
  local_stop::poll();
  assert(!state.active && !gate.active() && !pending);
  assert(robot_actions::stops == 1 && state.detections == 1);
  assert(!gate.accepts(heardDuringAction, nowMs));
  // Connecting and replying retain a live session even when mic listening
  // and movement are both idle. Local Stop must cover both phases.
  speech_commands::testSpeech.liveActive = true;
  local_stop::poll(); assert(state.active);
  speech_commands::testSpeech.liveConnected = false;
  pendingTag = gate.tag(nowMs); pending = true; local_stop::poll();
  assert(robot_actions::stops == 2 && !speech_commands::testSpeech.liveActive);
  speech_commands::testSpeech.liveActive = speech_commands::testSpeech.liveSpeaking = true;
  local_stop::poll(); assert(state.active);
  pendingTag = gate.tag(nowMs); pending = true; local_stop::poll();
  assert(robot_actions::stops == 3 && !speech_commands::testSpeech.liveActive);
  assert(criticalDepth == 0 && controlDepth == 0);
}

int main() {
  gateTests();
  handoffTests();
  puts("Local Stop: coordination, pending-action handoff, Stop before movement, stale cancellation and true inactivity passed");
}
