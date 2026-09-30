#pragma once
#include <cstdint>

namespace robot_actions {
enum class Command : uint8_t { None, Hello, Follow, Dance, LookAround, Picture, Explore, Stop };
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
void stop(const char* reason = "Stopped");
void manualOverride();
void poll();
Status snapshot();
const char* name(Command command);
}  // namespace robot_actions
