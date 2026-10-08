#pragma once
#include <cstdint>

namespace robot_actions {
// Preserve the approach/identify/escape wire IDs. All other actions are retired.
enum class Command : uint8_t { None = 0, Stop = 7,
  ComeSayHello = 8, Identify = 9, Escape = 14 };
inline bool validCommand(int id) { return id == 7 || id == 8 || id == 9 || id == 14; }
struct Status { Command command; const char* phase; bool active; uint32_t generation; };
// Main-loop producers and HTTP control handlers share this short-lived lock.
// Camera capture and neural inference never run while holding it.
class Guard {
 public:
  Guard();
  ~Guard();
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};
void begin();
uint32_t generation();
bool start(Command command, uint32_t expectedGeneration);
void stop(const char* reason = "Stopped", bool smooth = false);
void manualOverride();
void poll();
Status snapshot();
const char* name(Command command);
// Empty when ready. Shared by the dispatcher and dashboard readiness indicators.
const char* unavailableReason(Command command);
}  // namespace robot_actions
