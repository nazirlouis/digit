#include "personality_events.h"
#include "Arduino.h"
#include "audio_engine.h"
#include "reaction_player.h"
#include "robot_actions.h"
#include "speech_commands.h"
#include "wake_word.h"
#include "zumo_link.h"
#include <cstring>

namespace personality_events {
namespace {
constexpr uint32_t kFreshStatusMs=600, kReadyDwellMs=1000;
constexpr uint32_t kStartupWindowMs=20000, kEventExpiryMs=2000;
constexpr uint32_t kFaultClearMs=1000, kCooldownMs=8000;
Status status{false,Event::None,0,"Waiting for readiness"};
bool initialized=false, startupPending=true, stable=false;
bool wasListening=false, faultKnown=false, faultEpisode=false, faultClear=false;
bool hasPlayed=false;
uint32_t firstPoll=0, stableSince=0, listenGeneration=0, pendingGeneration=0;
uint32_t pendingAt=0, faultClearAt=0, lastPlayedAt=0, ownedToken=0;
AudioCue faultCue=AudioCue::Serious;

bool ownsActiveReaction() {
  return ownedToken && reaction_player::active() && reaction_player::snapshot().token==ownedToken;
}
void cancelOwned() {
  if (ownsActiveReaction()) reaction_player::cancel();
  ownedToken=0;
}
void clearPending(const char* reason) {
  status.pending=Event::None;
  status.reason=reason;
}
void queue(Event event,uint32_t now,uint32_t generation) {
  // A real fault takes precedence over a missed optional command reaction.
  if (status.pending==Event::Fault && event!=Event::Fault) return;
  status.pending=event; pendingAt=now; pendingGeneration=generation;
  status.reason="Waiting for quiet stopped moment";
}
bool stopped(const zumo_link::Status& z) {
  return z.left==0 && z.right==0 && z.targetLeftCps==0 && z.targetRightCps==0 &&
      z.encoderLeftCps==0 && z.encoderRightCps==0 && !z.lookActive &&
      !z.manualDriving && !z.motionActive;
}
void present(Event event,uint32_t now) {
  const char* face=event==Event::Ready ? "HAPPY" : event==Event::MissedCommand ? "CONFUSED" :
      faultCue==AudioCue::Surprised ? "SURPRISED" : "SERIOUS";
  const auto cue=event==Event::Ready ? AudioCue::Happy : event==Event::MissedCommand ? AudioCue::Confused : faultCue;
  ownedToken=reaction_player::play(face,"NONE",cue,reaction_player::Source::Ambient,kCooldownMs);
  clearPending(ownedToken ? "Reaction requested" : "Higher priority reaction active");
  if (ownedToken) { ++status.played; hasPlayed=true; lastPlayedAt=now; }
}
}

void cancel() {
  cancelOwned();
  startupPending=stable=false;
  wasListening=false;
  status.suppressed=true;
  clearPending("Stopped; waiting for interaction");
}
void interaction() {
  cancelOwned();
  startupPending=stable=false;
  wasListening=false;
  status.suppressed=false;
  clearPending("User interaction");
}
void safetyStopped() {
  cancel();
  status.suppressed=false;
  status.reason="Safety stop";
}
Status snapshot() { return status; }

void poll() {
  const uint32_t now=millis();
  if (!initialized) { initialized=true; firstPoll=now; }
  const auto z=zumo_link::snapshot();
  const auto action=robot_actions::snapshot();
  const auto speech=speech_commands::snapshot();
  const auto wake=wake_word::snapshot();
  const auto audio=audioEngine.snapshot();
  const bool fresh=z.connected && z.replyAgeMs<=kFreshStatusMs &&
      z.statusTimestampMs && now-z.statusTimestampMs<=kFreshStatusMs;
  const bool fault=z.edge || (z.fault[0] && strcmp(z.fault,"NONE"));
  const bool userBusy=action.active || speech.listening || wake.reacting || !stopped(z);
  const bool quiet=!userBusy && !wake.pausedForVision && audio.available &&
      audio.state==AudioState::Ready && !audio.cuePlaying && !audio.wakeChimePlaying;

  // Interaction always wins, including speech that starts during a greeting.
  // This only cancels our own token, never an action, wake, or dashboard preview.
  if (ownsActiveReaction() && (!fresh || userBusy || audio.state==AudioState::Recording)) cancelOwned();
  if (userBusy) { startupPending=false; stable=false; }

  if (speech.listening && !wasListening) listenGeneration=action.generation;
  if (!speech.listening && wasListening && !status.suppressed &&
      !action.active && action.generation==listenGeneration && !fault &&
      !strcmp(speech.result,"No command recognized")) {
    queue(Event::MissedCommand,now,action.generation);
  }
  wasListening=speech.listening;

  // Reconnecting to an already latched fault is not a new emotional event.
  // A healthy dwell rearms one notification per real fault episode.
  if (!fresh) {
    faultKnown=faultClear=false;
    if (status.pending!=Event::None) clearPending("Status became stale");
  } else if (!faultKnown) {
    faultKnown=true; faultEpisode=fault; faultClear=false;
  } else if (fault) {
    faultClear=false;
    if (!faultEpisode) {
      faultEpisode=true;
      if (!status.suppressed) {
        faultCue=z.edge ? AudioCue::Surprised : AudioCue::Serious;
        queue(Event::Fault,now,action.generation);
      }
    }
  } else if (faultEpisode) {
    if (!faultClear) { faultClear=true; faultClearAt=now; }
    else if (now-faultClearAt>=kFaultClearMs) faultEpisode=false;
  }

  if (status.suppressed) return;
  if (status.pending!=Event::None &&
      (now-pendingAt>=kEventExpiryMs || action.generation!=pendingGeneration ||
       (status.pending==Event::Fault && !fault) ||
       (status.pending==Event::MissedCommand && (fault || speech.listening || action.active)))) {
    clearPending("Event expired or superseded");
  }
  if (status.pending!=Event::None && fresh && quiet && !z.calibrating && !reaction_player::active()) {
    if (!hasPlayed || now-lastPlayedAt>=kCooldownMs) present(status.pending,now);
    else clearPending("Ambient cooldown");
  }

  if (!startupPending) return;
  if (now-firstPoll>=kStartupWindowMs) { startupPending=stable=false; return; }
  const bool ready=fresh && z.ready && !z.calibrating && !fault && quiet &&
      speech.ready && wake.ready && !reaction_player::active();
  if (!ready) { stable=false; return; }
  if (!stable) { stable=true; stableSince=now; }
  else if (now-stableSince>=kReadyDwellMs) {
    startupPending=false;
    present(Event::Ready,now);
  }
}
}
