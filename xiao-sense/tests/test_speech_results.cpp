#include "speech_commands.h"
#include "speech_pipeline.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
using std::max;
enum esp_mn_state_t { ESP_MN_STATE_DETECTING, ESP_MN_STATE_DETECTED, ESP_MN_STATE_TIMEOUT };
struct esp_mn_results_t { int num; int command_id[5]; float prob[5]; char string[256]; };
speech_commands::Status state{};
speech_commands::AudioTestStatus testState{};
uint32_t session = 1, controlGeneration = 0, opensAt = 0, pendingGeneration = 0, nowMs = 0;
constexpr uint32_t kVoiceTailMs = 120, kVoiceWaitTimeoutMs = 2500, kStallMs = 10000;
bool waitingForVoice = false;
enum class AudioState { Ready, Playing };
struct AudioMock {
  bool cuePlaying=false, wakeChimePlaying=false;
  AudioState state=AudioState::Ready;
  AudioMock snapshot(){ return *this; }
  void cancelOutput(){ cuePlaying=wakeChimePlaying=false; state=AudioState::Ready; }
} audioEngine;
int pendingCommand = 0;
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
uint32_t millis() { return nowMs; }
struct { void println(const char*) {} template<class... T> void printf(const char*, T...) {} } Serial;
namespace robot_actions {
enum class Command { None = 0, Stop = 7 };
int dispatched = 0;
uint32_t generation = 42;
bool start(Command, uint32_t expected) { if (expected != generation) return false; ++dispatched; return true; }
}

// These functions are extracted verbatim from the firmware by run_actions.ps1.
#include "speech_result.inc"
namespace speech_commands {
#include "speech_control.inc"
}

int main() {
  using namespace speech_commands;
  state.ready = true;
  audioEngine.cuePlaying = audioEngine.wakeChimePlaying = true;
  listen(42); poll();
  nowMs += 800; poll();
  assert(waitingForVoice && state.listening && !strcmp(state.result, "Wake voice"));
  audioEngine.cancelOutput(); poll();
  assert(!waitingForVoice && opensAt == nowMs + 120 && state.listening);
  cancel();
  audioEngine.cuePlaying = true; listen(42);
  nowMs += 2500; poll();
  assert(!state.listening && !audioEngine.cuePlaying && !strcmp(state.result, "Wake voice interrupted"));
  audioEngine.cuePlaying = true; listen(42); cancel(); audioEngine.cancelOutput();
  nowMs += 1000; poll(); assert(!state.listening);
  testState.busy = true;
  listen(42); poll(); assert(!snapshot().listening && pendingCommand == 0);
  testState.busy = false;
  listen(42); poll();
  nowMs = opensAt;
  speech_pipeline::Tag tag{session, nowMs};
  esp_mn_results_t result{};
  result.num = 1; result.command_id[0] = 7; result.prob[0] = 0.40f;
  strcpy(result.string, "stop");
  publishResult(tag, ESP_MN_STATE_DETECTED, &result, 11000, 12000);
  assert(!snapshot().listening && snapshot().recognized == 1 && pendingCommand == 7);
  assert(snapshot().score == 0.40f); // Demo acceptance has no extra 0.70 cutoff.
  publishResult(tag, ESP_MN_STATE_DETECTED, &result, 11000, 12000);
  assert(snapshot().recognized == 1); // At most one command per wake.
  poll(); assert(robot_actions::dispatched == 1);
  poll(); assert(robot_actions::dispatched == 1);

  listen(42); poll(); tag = {session, opensAt}; nowMs = opensAt;
  cancel();
  publishResult(tag, ESP_MN_STATE_DETECTED, &result, 11000, 12000);
  poll(); assert(robot_actions::dispatched == 1 && snapshot().recognized == 1);

  listen(42); poll(); tag = {session, opensAt}; nowMs = opensAt;
  publishResult(tag, ESP_MN_STATE_DETECTED, &result, 11000, 12000);
  cancel(); poll(); assert(robot_actions::dispatched == 1); // Stop after inference, before dispatch.

  listen(42); poll(); tag = {session, opensAt};
  listen(43); poll(); nowMs = opensAt;
  publishResult(tag, ESP_MN_STATE_DETECTED, &result, 11000, 12000);
  assert(snapshot().listening && pendingCommand == 0); // New wake invalidates old results.
  tag = {session, nowMs};
  publishResult(tag, ESP_MN_STATE_TIMEOUT, &result, 11000, 12000);
  assert(!snapshot().listening && pendingCommand == 0);
  assert(!strcmp(snapshot().result, "No command recognized")); // Timeout partial text is diagnostic only.

  listen(42); poll(); tag = {session, opensAt}; nowMs = opensAt;
  result.string[0] = '\0';
  publishResult(tag, ESP_MN_STATE_DETECTED, &result, 11000, 12000);
  assert(snapshot().listening && pendingCommand == 0);
  strcpy(result.string, "stop"); result.command_id[0] = 8;
  publishResult(tag, ESP_MN_STATE_DETECTED, &result, 11000, 12000);
  assert(snapshot().listening && pendingCommand == 0);
  for (int id=2;id<=6;++id) {
    result.command_id[0] = id;
    publishResult(tag, ESP_MN_STATE_DETECTED, &result, 11000, 12000);
    assert(snapshot().listening && pendingCommand == 0);
  }
  result.command_id[0] = 7;
  nowMs += 1001;
  publishResult(tag, ESP_MN_STATE_DETECTED, &result, 11000, 12000);
  assert(snapshot().listening && pendingCommand == 0); // Inference finished on stale sound.
  nowMs = opensAt + 6000; poll(); assert(snapshot().listening); // Normal timeout belongs to the model.
  nowMs = opensAt + 10000; poll(); assert(!snapshot().listening && pendingCommand == 0);
  assert(!strcmp(snapshot().result, "Audio interrupted"));
  puts("Speech results: acceptance, single dispatch, Stop during/after inference, new wake, timeout and stalls passed");
}
