#include "personality_events.h"
#include "zumo_link.h"
namespace personality_events {
namespace { Status status{false,Event::None,0,"Gemini owns expressions"}; }
void cancel() { status.suppressed=true; status.pending=Event::None; }
void interaction() { status.suppressed=false; }
void safetyStopped() { cancel(); status.reason="Safety stop"; zumo_link::face("SERIOUS"); }
Status snapshot() { return status; }
void poll() {}  // No ambient sounds or face resets compete with Gemini.
}
