#include "reaction_player.h"
#include "Arduino.h"
#include "audio_engine.h"
#include "zumo_link.h"
#include <cstdio>
#include <cstring>

namespace reaction_player {
namespace {
constexpr uint32_t kDispatchMs = 1500, kVisibleMs = 3500;
Status current{0,State::Idle,0,0,false,"Idle"};
uint32_t serial=0, requestedAt=0, dispatchedAt=0;
bool dispatched=false;
bool wake=false;
char expression[12]={}, caption[16]={};
AudioCue sound=AudioCue::None;
VoiceTiming timing=VoiceTiming::Expression;
uint32_t lastSound[13]={};
bool sounded[13]={};
int soundIndex(AudioCue cue) {
  return cue>=AudioCue::Neutral && cue<=AudioCue::Love ?
      static_cast<int>(cue)-static_cast<int>(AudioCue::Neutral) : -1;
}
void skip(const char* reason) {
  audioEngine.cancelOutput(); zumo_link::cancelScene(); zumo_link::cancelWake();
  current.state=State::Skipped; current.reason=reason;
}
}

bool active() { return current.state==State::Waiting || current.state==State::Playing; }
Status snapshot() { return current; }
bool finished(uint32_t token) { return !token || token!=current.token || !active(); }
void cancel() {
  if (active()) {
    audioEngine.cancelOutput(); zumo_link::cancelScene(); zumo_link::cancelWake();
    current.state=State::Cancelled; current.reason="Cancelled";
  }
}
uint32_t play(const char* face, const char* label, AudioCue cue, Source source,
              uint32_t cooldownMs, VoiceTiming selectedTiming) {
  if (source==Source::Ambient && active()) return 0;
  cancel();
  // Replacement owns the speaker even if the previous sound came from an
  // older caller. A silent preview must not retain the previous voice.
  audioEngine.cancelOutput(); zumo_link::cancelScene(); zumo_link::cancelWake();
  if (!++serial) ++serial;
  current={serial,State::Waiting,0,0,false,"Waiting for face"};
  requestedAt=millis(); dispatchedAt=0; dispatched=false;
  if (selectedTiming==VoiceTiming::Gesture ||
      (selectedTiming==VoiceTiming::Wake && source!=Source::Wake)) {
    skip("Unsupported reaction timing profile"); return current.token;
  }
  if (!face || !label || !*face || !*label || strlen(face)>=sizeof(expression) || strlen(label)>=sizeof(caption)) {
    skip("Invalid reaction"); return current.token;
  }
  snprintf(expression,sizeof(expression),"%s",face);
  snprintf(caption,sizeof(caption),"%s",label);
  wake=source==Source::Wake;
  timing=wake ? VoiceTiming::Wake : selectedTiming;
  sound=wake ? AudioCue::Happy : cue;
  const AudioCue faceCue=emotionCueForFace(face);
  current.durationMs=emotionDurationMs(faceCue,timing);
  const int index=soundIndex(cue);
  if (source!=Source::Preview && cooldownMs && index>=0 && sounded[index] &&
      millis()-lastSound[index]<cooldownMs) sound=AudioCue::None;
  poll();
  return current.token;
}
void poll() {
  if (!active()) return;
  const auto now=millis();
  if (current.state==State::Waiting) {
    // Dispatch and first-frame visibility share one wake preparation budget;
    // optional decoration must not consume the command-listening window.
    if (wake && now-requestedAt>=1500) {
      skip("Wake preparation timeout; reaction skipped"); return;
    }
    if (!dispatched) {
      const auto z=zumo_link::snapshot();
      if (z.connected && z.reactionVersion!=reaction_catalog::version) {
        skip("Update both controllers for matching reactions"); return;
      }
      if (wake ? zumo_link::wake() : zumo_link::scene(expression,caption)) { dispatched=true; dispatchedAt=now; }
      else if (now-requestedAt>=kDispatchMs) skip("Face unavailable; reaction skipped");
      return;
    }
    const auto z=zumo_link::snapshot();
    const char* result=wake ? z.wakeResult : z.sceneResult;
    if (!strcmp(result,"SENDING") || !strcmp(result,"RENDERING")) {
      if (now-dispatchedAt>=(wake ? 1500U : kVisibleMs)) skip("Face visibility timeout; reaction skipped");
      return;
    }
    if (strcmp(result,"APPLIED")) { skip("Face failed; reaction skipped"); return; }
    current.visible=true; current.visibleAt=now;
    if (sound!=AudioCue::None && !audioEngine.playCue(sound,timing)) {
      skip("Voice unavailable; reaction skipped"); return;
    }
    const int index=soundIndex(sound);
    if (index>=0) { sounded[index]=true; lastSound[index]=now; }
    current.state=State::Playing; current.reason="Playing";
  }
  // The shared duration includes quiet facial settling. Do not infer face
  // completion from whether the speaker is currently making a sound.
  if (current.state==State::Playing && now-current.visibleAt>=current.durationMs) {
    if (sound!=AudioCue::None) audioEngine.cancelOutput();
    current.state=State::Complete; current.reason="Complete";
  }
}
}
