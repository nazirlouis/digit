#pragma once
#include <cstdint>

// Motion-free ambient reactions. Call under robot_actions::Guard alongside
// reaction_player; all user control paths should call interaction() or cancel().
namespace personality_events {
enum class Event : uint8_t { None, Ready, MissedCommand, Fault };
struct Status {
  bool suppressed;
  Event pending;
  uint32_t played;
  const char* reason;
};
void poll();
// Stop consumes the startup greeting and mutes ambient events until the next
// explicit interaction. Neither method can cancel someone else's reaction.
void cancel();
void interaction();
// Only the automatic safety-stop path may use this. It permits a fresh fault
// transition after braking while preserving cancellation of stale reactions.
void safetyStopped();
Status snapshot();
}
