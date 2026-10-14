#pragma once
#include <cstdint>
#include "robot_actions.h"

namespace action_console {
inline constexpr unsigned kCapacity = 50;
enum class State : uint8_t { Requested, Queued, Running, Applied, Completed, Blocked, Rejected, Failed, Cancelled, TestOnly };
struct Entry {
  uint32_t id, revision, ms, updatedMs, connection, session;
  robot_actions::Command command;
  State state;
  char source[12], callId[129], function[65], arguments[320], purpose[192], result[384];
};
struct Snapshot { bool available; uint32_t bootId, revision, oldestId, newestId; };
// Allocated once in PSRAM. Logging failure never changes robot control.
void begin();
Snapshot snapshot();
bool read(uint32_t id, Entry& entry);
uint32_t requested(uint32_t connection, uint32_t session, const char* callId,
                   const char* function, const robot_actions::ToolRequest& tool);
void update(uint32_t connection, const char* callId, State state, const char* result);
void updateId(uint32_t id, State state, const char* result);
void cancelSession(uint32_t session, const char* reason);
uint32_t local(const char* function, const char* arguments, const char* purpose,
           State state, const char* result, const char* source = "Controller",
           robot_actions::Command command = robot_actions::Command::None);
const char* name(State state);
const char* category(robot_actions::Command command);
}  // namespace action_console
