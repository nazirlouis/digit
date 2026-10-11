#include "reaction_player.h"
#include "Arduino.h"
#include "zumo_link.h"
#include <cstring>
#include <cstdio>
namespace reaction_player {
namespace {
Status current{0,State::Idle,0,0,false,"Idle"};
uint32_t serial=0, requestedAt=0, dispatchedAt=0;
bool dispatched=false, wake=false;
char expression[12]{}, caption[16]{};
void skip(const char* reason) { zumo_link::cancelScene(); zumo_link::cancelWake(); current.state=State::Skipped; current.reason=reason; }
}
bool active() { return current.state==State::Waiting || current.state==State::Playing; }
Status snapshot() { return current; }
bool finished(uint32_t token) { return !token || token!=current.token || !active(); }
void cancel() { if (active()) { skip("Cancelled"); current.state=State::Cancelled; } }
uint32_t play(const char* face, const char* label, Source source) {
  cancel();
  if (!face || !label || strlen(face)>=sizeof(expression) || strlen(label)>=sizeof(caption)) return 0;
  if (!++serial) ++serial;
  snprintf(expression,sizeof(expression),"%s",face); snprintf(caption,sizeof(caption),"%s",label);
  wake=source==Source::Wake; dispatched=false; requestedAt=millis();
  current={serial,State::Waiting,0,1500,false,"Waiting for face"}; poll(); return serial;
}
void poll() {
  if (!active()) return;
  const uint32_t now=millis();
  if (current.state==State::Waiting) {
    if (now-requestedAt>=(wake?1500U:3500U)) { skip("Face visibility timeout"); return; }
    if (!dispatched) {
      const auto z=zumo_link::snapshot();
      if (z.connected && z.reactionVersion!=reaction_catalog::version) { skip("Update both controllers for matching faces"); return; }
      if (wake?zumo_link::wake():zumo_link::scene(expression,caption)) { dispatched=true; dispatchedAt=now; }
      return;
    }
    const auto z=zumo_link::snapshot(); const char* result=wake?z.wakeResult:z.sceneResult;
    if (!strcmp(result,"SENDING") || !strcmp(result,"RENDERING")) return;
    if (strcmp(result,"APPLIED")) { skip("Face unavailable"); return; }
    current.visible=true; current.visibleAt=now; current.state=State::Playing; current.reason="Visible; silent";
  }
  if (current.state==State::Playing && now-current.visibleAt>=current.durationMs) { current.state=State::Complete; current.reason="Complete"; }
}
}
