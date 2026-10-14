#include "action_console.h"
#include "Arduino.h"
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <cstdio>
#include <cstring>

namespace action_console {
namespace {
portMUX_TYPE consoleLock = portMUX_INITIALIZER_UNLOCKED;
Entry* entries = nullptr;
uint32_t newest = 0, revision = 0, bootId = 0;
bool terminal(State state) { return state != State::Requested && state != State::Queued && state != State::Running; }
void copy(char* output, size_t capacity, const char* value) {
  snprintf(output, capacity, "%s", value ? value : "");
  // Keep a bounded string valid UTF-8 if truncation cut a multi-byte character.
  const size_t size = strlen(output);
  size_t start = size;
  while (start && (static_cast<unsigned char>(output[start-1]) & 0xc0) == 0x80) --start;
  if (start) {
    --start;
    const unsigned char lead = static_cast<unsigned char>(output[start]);
    const size_t expected = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
    if (size - start < expected) output[start] = 0;
  }
}
Entry* find(uint32_t connection, const char* callId) {
  if (!entries || !callId || !*callId) return nullptr;
  for (unsigned i = 0; i < kCapacity; ++i)
    if (entries[i].id && entries[i].connection == connection && !strcmp(entries[i].callId, callId)) return &entries[i];
  return nullptr;
}
Entry* create() {
  if (!entries) return nullptr;
  auto* e = &entries[newest % kCapacity]; *e = {};
  e->id = ++newest; e->revision = ++revision; e->ms = e->updatedMs = millis();
  return e;
}
void change(Entry* e, State state, const char* result) {
  // A delayed queued notification or result cannot resurrect a cancelled call.
  if (!e || terminal(e->state)) return;
  e->state = state; copy(e->result, sizeof(e->result), result);
  e->updatedMs = millis(); e->revision = ++revision;
}
}
void begin() {
  if (entries) return;
  auto* storage = static_cast<Entry*>(heap_caps_malloc(kCapacity * sizeof(Entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!storage) return;
  memset(storage, 0, kCapacity * sizeof(Entry));
  portENTER_CRITICAL(&consoleLock);
  entries = storage; bootId = esp_random();
  portEXIT_CRITICAL(&consoleLock);
}
Snapshot snapshot() {
  portENTER_CRITICAL(&consoleLock);
  Snapshot s{entries != nullptr, bootId, revision, newest ? (newest > kCapacity ? newest - kCapacity + 1 : 1) : 0, newest};
  portEXIT_CRITICAL(&consoleLock); return s;
}
bool read(uint32_t id, Entry& entry) {
  portENTER_CRITICAL(&consoleLock);
  const bool valid = entries && id && id <= newest && newest - id < kCapacity && entries[(id-1)%kCapacity].id == id;
  if (valid) entry = entries[(id-1)%kCapacity];
  portEXIT_CRITICAL(&consoleLock); return valid;
}
uint32_t requested(uint32_t connection, uint32_t session, const char* callId, const char* function, const robot_actions::ToolRequest& tool) {
  // Format only recognized, validated fields; never retain arbitrary tool JSON.
  char arguments[320]{};
  using C = robot_actions::Command;
  switch (tool.command) {
    case C::Expression: snprintf(arguments,sizeof(arguments),"expression=%s, duration_ms=%d",tool.expression,tool.durationMs); break;
    case C::TurnLeft: case C::TurnRight: snprintf(arguments,sizeof(arguments),"degrees=%d",tool.amount); break;
    case C::Forward: case C::Backward: case C::Push: snprintf(arguments,sizeof(arguments),"distance_cm=%d",tool.amount); break;
    case C::StartTask: snprintf(arguments,sizeof(arguments),"goal=%s",tool.goal); break;
    case C::FinishTask: snprintf(arguments,sizeof(arguments),"outcome=%s, reason=%s",tool.outcome,tool.reason); break;
    default: break;
  }
  portENTER_CRITICAL(&consoleLock);
  auto* e = find(connection,callId);
  if (!e) {
    e = create();
    if (e) {
      e->connection=connection; e->session=session; e->command=tool.command; e->state=State::Requested;
      copy(e->source,sizeof(e->source),"Gemini"); copy(e->callId,sizeof(e->callId),callId);
      copy(e->function,sizeof(e->function),function); copy(e->arguments,sizeof(e->arguments),arguments);
      copy(e->purpose,sizeof(e->purpose),tool.actionReason);
    }
  }
  const uint32_t id = e ? e->id : 0;
  portEXIT_CRITICAL(&consoleLock); return id;
}
void update(uint32_t connection, const char* callId, State state, const char* result) {
  portENTER_CRITICAL(&consoleLock); change(find(connection,callId),state,result); portEXIT_CRITICAL(&consoleLock);
}
void updateId(uint32_t id, State state, const char* result) {
  portENTER_CRITICAL(&consoleLock);
  if (entries && id && id <= newest && newest-id < kCapacity) change(&entries[(id-1)%kCapacity],state,result);
  portEXIT_CRITICAL(&consoleLock);
}
void cancelSession(uint32_t session, const char* reason) {
  portENTER_CRITICAL(&consoleLock);
  if (entries) for (unsigned i=0; i<kCapacity; ++i)
    if (entries[i].id && entries[i].session==session && entries[i].command!=robot_actions::Command::Stop)
      change(&entries[i],State::Cancelled,reason);
  portEXIT_CRITICAL(&consoleLock);
}
uint32_t local(const char* function, const char* arguments, const char* purpose, State state, const char* result, const char* source, robot_actions::Command command) {
  portENTER_CRITICAL(&consoleLock);
  uint32_t id=0;
  if (auto* e=create()) {
    id=e->id;
    e->state=state; e->command=command;
    copy(e->source,sizeof(e->source),source); copy(e->function,sizeof(e->function),function);
    copy(e->arguments,sizeof(e->arguments),arguments); copy(e->purpose,sizeof(e->purpose),purpose); copy(e->result,sizeof(e->result),result);
  }
  portEXIT_CRITICAL(&consoleLock);
  return id;
}
const char* name(State state) {
  switch(state) {
    case State::Requested:return "Requested"; case State::Queued:return "Queued"; case State::Running:return "Running";
    case State::Applied:return "Applied"; case State::Completed:return "Completed"; case State::Blocked:return "Blocked";
    case State::Rejected:return "Rejected"; case State::Failed:return "Failed"; case State::Cancelled:return "Cancelled";
    case State::TestOnly:return "Test only";
  }
  return "Unknown";
}
const char* category(robot_actions::Command command) {
  if(command==robot_actions::Command::Expression || command==robot_actions::Command::Dance)return "Emotes";
  return robot_actions::isMovement(command) ? "Movement" : "Other";
}
}  // namespace action_console
