#pragma once
#include <cstdint>

// Silent local fault presentation; Gemini owns ordinary expressions.
namespace personality_events {
enum class Event : uint8_t { None, Ready, MissedCommand, Fault };
struct Status {
  bool suppressed;
  Event pending;
  uint32_t played;
  const char* reason;
};
void poll();
// Cancel stale optional presentations on Stop or manual takeover.
void cancel();
void interaction();
// Only the automatic safety-stop path may use this. It permits a fresh fault
// transition after braking while preserving cancellation of stale reactions.
void safetyStopped();
Status snapshot();
}
