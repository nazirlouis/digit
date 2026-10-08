#include "action_mocks.h"
#include "reaction_player.h"
#include <iostream>
using namespace reaction_player;
void reset() {
  cancel(); nowMs+=10000; audioEngine.available=true; audioEngine.playing=false;
  zumo_link::state={}; zumo_link::sceneAvailable=true; zumo_link::sceneAcknowledged=true;
}
int main() {
  reset(); zumo_link::sceneAcknowledged=false;
  const auto before=audioEngine.plays;
  const auto first=play("CURIOUS","SEARCHING",AudioCue::Curious);
  nowMs+=500; poll(); assert(audioEngine.plays==before && !snapshot().visible);
  strcpy(zumo_link::state.sceneResult,"RENDERING"); poll(); assert(audioEngine.plays==before);
  strcpy(zumo_link::state.sceneResult,"APPLIED"); poll();
  assert(snapshot().visible && audioEngine.plays==before+1 && !finished(first));
  audioEngine.playing=false; nowMs+=100; poll(); assert(active()); // Quiet settling is still part of the face.
  nowMs+=snapshot().durationMs; poll(); assert(finished(first));

  reset(); zumo_link::sceneAcknowledged=false;
  const auto old=play("SAD","NONE",AudioCue::Sad);
  const auto sounds=audioEngine.plays;
  const auto replacement=play("HAPPY","NONE",AudioCue::Happy,Source::Preview);
  assert(replacement!=old && finished(old));
  poll(); assert(audioEngine.plays==sounds);
  strcpy(zumo_link::state.sceneResult,"APPLIED");poll();
  assert(audioEngine.lastCue==AudioCue::Happy && audioEngine.plays==sounds+1);
  cancel(); strcpy(zumo_link::state.sceneResult,"APPLIED");nowMs+=5000;poll();
  assert(!active() && !audioEngine.playing && audioEngine.plays==sounds+1);

  reset(); audioEngine.available=false;
  play("LOVE","FOUND",AudioCue::Love);poll();
  assert(snapshot().state==State::Skipped && !active());
  reset(); zumo_link::sceneAvailable=false;
  play("CURIOUS","SEARCHING",AudioCue::Curious);nowMs+=1500;poll();
  assert(snapshot().state==State::Skipped);
  reset(); zumo_link::sceneAcknowledged=false;
  play("CURIOUS","SEARCHING",AudioCue::Curious);nowMs+=3500;poll();
  assert(snapshot().state==State::Skipped && !audioEngine.playing);
  reset();zumo_link::state.reactionVersion=0;
  const auto priorScenes=zumo_link::scenes.size();
  play("CURIOUS","SEARCHING",AudioCue::Curious);poll();
  assert(snapshot().state==State::Skipped && zumo_link::scenes.size()==priorScenes);

  reset();
  const auto profileScenes=zumo_link::scenes.size();
  const auto profileSounds=audioEngine.plays;
  const auto gesture=play("HAPPY","NONE",AudioCue::Happy,Source::Action,0,VoiceTiming::Gesture);
  assert(snapshot().state==State::Skipped && finished(gesture));
  assert(!strcmp(snapshot().reason,"Unsupported reaction timing profile"));
  const auto wrongWake=play("HAPPY","NONE",AudioCue::Happy,Source::Preview,0,VoiceTiming::Wake);
  assert(snapshot().state==State::Skipped && finished(wrongWake));
  const auto wakeGesture=play("HAPPY","NONE",AudioCue::Happy,Source::Wake,0,VoiceTiming::Gesture);
  assert(snapshot().state==State::Skipped && finished(wakeGesture));
  poll();assert(zumo_link::scenes.size()==profileScenes && audioEngine.plays==profileSounds);

  reset();play("CURIOUS","SEARCHING",AudioCue::Curious,Source::Action,8000);poll();
  const auto once=audioEngine.plays;
  play("CURIOUS","SEARCHING",AudioCue::Curious,Source::Action,8000);poll();
  assert(audioEngine.plays==once); // Repeated scanning can keep its face quietly.
  play("CURIOUS","NONE",AudioCue::Curious,Source::Preview,8000);poll();
  assert(audioEngine.plays==once+1);
  play("CURIOUS","NONE",AudioCue::Curious,Source::Preview,8000);poll();
  assert(audioEngine.plays==once+2);
  const auto preview=snapshot().token;
  assert(play("SLEEPY","NONE",AudioCue::Sleepy,Source::Ambient)==0);
  assert(snapshot().token==preview);
  play("SAD","NONE",AudioCue::None,Source::Preview);poll();
  assert(!audioEngine.playing && audioEngine.plays==once+2);

  reset();zumo_link::sceneAcknowledged=false;
  play("HAPPY","NONE",AudioCue::Happy,Source::Wake);
  const auto wakeSounds=audioEngine.plays;poll();assert(audioEngine.plays==wakeSounds);
  strcpy(zumo_link::state.wakeResult,"APPLIED");poll();
  assert(audioEngine.lastCue==AudioCue::Happy && audioEngine.plays==wakeSounds+1);
  assert(snapshot().durationMs==emotionDurationMs(AudioCue::Happy,VoiceTiming::Wake));
  cancel();
  reset();zumo_link::sceneAvailable=false;zumo_link::sceneAcknowledged=false;
  const auto wakeStart=nowMs, wakePlays=audioEngine.plays;
  const auto delayedWake=play("HAPPY","NONE",AudioCue::Happy,Source::Wake);
  nowMs=wakeStart+1000;zumo_link::sceneAvailable=true;poll(); // Late dispatch.
  nowMs=wakeStart+1499;poll();assert(active());
  nowMs=wakeStart+1500;poll();
  assert(snapshot().state==State::Skipped && finished(delayedWake));
  strcpy(zumo_link::state.wakeResult,"APPLIED");nowMs+=100;poll();
  assert(audioEngine.plays==wakePlays && !active()); // Late visibility cannot release audio.
  std::cout<<"PASS reaction visibility, replacement, Stop, optional failure, cooldown, preview replay, wake\n";
}
