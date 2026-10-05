#pragma once
#include <cstdint>

namespace robot_actions {
// Keep wire IDs stable; retired routine IDs 2 through 6 are rejected.
enum class Command : uint8_t { None = 0, Hello = 1, Stop = 7 };
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
}  // namespace robot_actions
