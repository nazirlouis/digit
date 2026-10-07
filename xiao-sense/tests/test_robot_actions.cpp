// The runner compiles an unchanged copy of the production controller beside
// mock hardware headers. Exercise cancellation at every point in each routine.
#include "robot_actions.cpp"
#include <iostream>
#include <thread>
using robot_actions::Command;

void resetDevice() {
  robot_actions::stop();
  zumo_link::state = {};
  vision_service::state = {};
  audioEngine.playing = false;
  audioEngine.available = true;
  photo_store::pending = false;
  nowMs += 1000;
}
void tick() {
  nowMs += 25;
  if (zumo_link::state.lookActive) {
    const auto elapsed=nowMs-zumo_link::turnAt;
    if(elapsed>1400) {
      zumo_link::state.lookActive=false;
      std::strcpy(zumo_link::state.lookResult,"DONE");
    } else if(elapsed>1000) std::strcpy(zumo_link::state.lookResult,"CENTERING");
    else if(elapsed>450) std::strcpy(zumo_link::state.lookResult,"WIGGLING");
  }
  if (nowMs%100==0) ++vision_service::state.resultTimestampMs;
  if(nowMs>=audioEngine.playUntil)audioEngine.playing=false;
  photo_store::pending=false;
  robot_actions::poll();
}
int main() {
  robot_actions::begin();
  resetDevice();
  vision_service::state.enabled=true;
  vision_service::state.batteryMode=true;
  robot_actions::stop("Dashboard Stop");
  assert(!vision_service::state.enabled && !vision_service::state.batteryMode);
  std::cout << "PASS battery test cancels on Stop\n";
  resetDevice();
  // A manual takeover cancels an actual wake reaction once. Every subsequent
  // 100 ms hold refresh must preserve the motor target and its acceleration.
  wake_word::reactionActive=true;
  wake_word::hasReaction=true;
  wake_word::lastReaction=nowMs;
  assert(wake_word::snapshot().reacting);
  const auto beforeTakeover=zumo_link::stops;
  robot_actions::manualOverride();
  assert(zumo_link::stops==beforeTakeover+1);
  assert(!wake_word::snapshot().reacting);
  assert(wake_word::hasReaction); // Detection cooldown remains in force.
  zumo_link::state.left=zumo_link::state.right=4000;
  zumo_link::state.manualDriving=true;
  const auto afterTakeover=zumo_link::stops;
  for(int i=0;i<30;++i) {
    nowMs+=100;
    robot_actions::manualOverride();
    assert(!wake_word::snapshot().reacting);
    assert(zumo_link::stops==afterTakeover);
    assert(zumo_link::state.left==4000 && zumo_link::state.right==4000);
  }
  robot_actions::stop("Dashboard Stop");
  assert(!zumo_link::state.left && !zumo_link::state.right);
  std::cout << "PASS wake cancellation keeps cooldown without repeated motor Stops during held drive\n";
  resetDevice();
  assert(robot_actions::start(Command::Hello,robot_actions::generation()));
  const auto brakeGeneration=robot_actions::generation();
  robot_actions::stop("Dashboard Stop",true);
  assert(zumo_link::lastStopSmooth);
  assert(!robot_actions::snapshot().active);
  assert(!robot_actions::start(Command::Hello,brakeGeneration));
  for(int i=0;i<40;++i)tick();
  assert(!robot_actions::snapshot().active && !zumo_link::state.left && !zumo_link::state.right);
  robot_actions::stop();
  assert(!zumo_link::lastStopSmooth);
  std::cout << "PASS smooth dashboard stop cancels pending actions; safety stop remains immediate\n";
  for (int id : {1}) {
    for (int stopAt=0;stopAt<20000;stopAt+=125) {
      resetDevice();
      assert(robot_actions::start(static_cast<Command>(id),robot_actions::generation()));
      for (int t=0;t<stopAt;t+=25) tick();
      const auto old=robot_actions::generation();
      robot_actions::stop("Dashboard Stop");
      const auto sent=zumo_link::motions, photos=photo_store::captures, sounds=audioEngine.plays;
      assert(!robot_actions::start(static_cast<Command>(id),old));
      for(int t=0;t<1000;t+=25) tick();
      assert(!robot_actions::snapshot().active);
      assert(!zumo_link::state.left && !zumo_link::state.right && !zumo_link::state.lookActive);
      assert(zumo_link::motions==sent && photo_store::captures==photos);
      assert(audioEngine.plays==sounds);
      assert(!audioEngine.playing && !speech_commands::listening && !person_follow::snapshot().enabled);
    }
    std::cout << "PASS stop at 160 points: " << robot_actions::name(static_cast<Command>(id)) << '\n';
  }
  resetDevice();
  auto sounds=audioEngine.plays;
  assert(robot_actions::start(Command::Hello,robot_actions::generation()));
  while(!zumo_link::state.lookActive)tick();
  for(int i=0;i<18;++i)tick();
  assert(audioEngine.plays==sounds); // Silent during variable gyro preparation.
  tick();
  assert(audioEngine.plays==sounds+1 && audioEngine.lastCue==AudioCue::Happy);
  assert(zumo_link::state.lookActive && audioEngine.playing);
  while(strcmp(zumo_link::state.lookResult,"CENTERING"))tick();
  assert(audioEngine.plays==sounds+2 && audioEngine.lastCue==AudioCue::Neutral);
  assert(zumo_link::state.lookActive && audioEngine.playing);
  while(robot_actions::snapshot().active)tick();
  assert(audioEngine.plays==sounds+2 && !audioEngine.playing);
  // A lost start event must not replay Happy after the center chirp.
  resetDevice();
  assert(robot_actions::start(Command::Hello,robot_actions::generation()));
  while(!zumo_link::state.lookActive)tick();
  sounds=audioEngine.plays;
  std::strcpy(zumo_link::state.lookResult,"CENTERING");robot_actions::poll();
  assert(audioEngine.plays==sounds+1 && audioEngine.lastCue==AudioCue::Neutral);
  std::strcpy(zumo_link::state.lookResult,"WIGGLING");robot_actions::poll();
  assert(audioEngine.plays==sounds+1);
  // Failed playback cancels the motion instead of leaving an unsynchronized greeting.
  resetDevice();
  assert(robot_actions::start(Command::Hello,robot_actions::generation()));
  while(!zumo_link::state.lookActive)tick();
  audioEngine.available=false;
  std::strcpy(zumo_link::state.lookResult,"WIGGLING");robot_actions::poll();
  assert(!robot_actions::snapshot().active && !zumo_link::state.lookActive);
  std::cout << "PASS greeting sounds follow actual wiggle phases, play once, overlap motion, and never replay after it\n";
  // Retired wire IDs must fail without cancelling or disturbing an active Hello.
  resetDevice();
  assert(robot_actions::start(Command::Hello,robot_actions::generation()));
  const auto epoch=robot_actions::generation(), motions=zumo_link::motions, stops=zumo_link::stops;
  for (int id : {0,2,3,4,5,6,15,17,255}) {
    assert(!robot_actions::start(static_cast<Command>(id),epoch));
    assert(robot_actions::generation()==epoch && robot_actions::snapshot().active);
    assert(zumo_link::motions==motions && zumo_link::stops==stops);
  }
  const auto turns=zumo_link::motions;
  for (int i=0;i<1200 && robot_actions::snapshot().active;++i) tick();
  assert(!robot_actions::snapshot().active);
  assert(zumo_link::motions==turns+1); // One continuous gesture on the Zumo.
  assert(!strcmp(robot_actions::snapshot().phase,"Routine complete"));
  resetDevice();
  zumo_link::state.voiceCapable=false;
  assert(!robot_actions::start(Command::Hello,robot_actions::generation()));
  resetDevice();
  zumo_link::state.rampedLookCapable=false;
  assert(!robot_actions::start(Command::Hello,robot_actions::generation()));
  resetDevice();
  zumo_link::state.wiggleCapable=false;
  assert(!robot_actions::start(Command::Hello,robot_actions::generation()));
  resetDevice();
  zumo_link::state.rangeValid=false;
  vision_service::state.ready=false;
  assert(robot_actions::start(Command::Hello,robot_actions::generation()));
  zumo_link::state.edge=true;tick();
  assert(!robot_actions::snapshot().active);
  resetDevice();
  assert(robot_actions::start(Command::Hello,robot_actions::generation()));
  ++zumo_link::state.safetyEvents;tick();
  assert(!robot_actions::snapshot().active);
  resetDevice();
  assert(robot_actions::start(Command::Hello,robot_actions::generation()));
  zumo_link::state.connected=false;tick();
  assert(!robot_actions::snapshot().active);
  zumo_link::state.ready=false;
  assert(robot_actions::start(Command::Stop,robot_actions::generation()));
  std::cout << "PASS retired IDs rejected, Hello completes one continuous wiggle, gyro/edge/link safety, Stop without ready hardware\n";
}
