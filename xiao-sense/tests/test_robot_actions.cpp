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
  photo_store::pending = false;
  nowMs += 1000;
}
void tick() {
  nowMs += 25;
  if (zumo_link::state.lookActive && nowMs-zumo_link::turnAt>200) {
    zumo_link::state.lookActive=false;
    std::strcpy(zumo_link::state.lookResult,"DONE");
  }
  if (nowMs%100==0) ++vision_service::state.resultTimestampMs;
  audioEngine.playing=false;
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
  resetDevice();
  assert(robot_actions::start(Command::Follow,robot_actions::generation()));
  vision_service::state.batteryMode=true;
  const auto beforeBatteryFrame=zumo_link::motions;
  robot_actions::poll();
  assert(!robot_actions::snapshot().active);
  assert(zumo_link::motions==beforeBatteryFrame);
  std::cout << "PASS battery test cancels on Stop; battery boxes cannot drive person following\n";
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
  assert(robot_actions::start(Command::Explore,robot_actions::generation()));
  const auto brakeGeneration=robot_actions::generation();
  robot_actions::stop("Dashboard Stop",true);
  assert(zumo_link::lastStopSmooth);
  assert(!robot_actions::snapshot().active);
  assert(!robot_actions::start(Command::Explore,brakeGeneration));
  for(int i=0;i<40;++i)tick();
  assert(!robot_actions::snapshot().active && !zumo_link::state.left && !zumo_link::state.right);
  robot_actions::stop();
  assert(!zumo_link::lastStopSmooth);
  std::cout << "PASS smooth dashboard stop cancels pending actions; safety stop remains immediate\n";
  for (int id=1;id<=6;++id) {
    for (int stopAt=0;stopAt<20000;stopAt+=125) {
      resetDevice();
      assert(robot_actions::start(static_cast<Command>(id),robot_actions::generation()));
      for (int t=0;t<stopAt;t+=25) tick();
      const auto old=robot_actions::generation();
      robot_actions::stop("Dashboard Stop");
      const auto sent=zumo_link::motions, photos=photo_store::captures;
      assert(!robot_actions::start(static_cast<Command>(id),old));
      for(int t=0;t<1000;t+=25) tick();
      assert(!robot_actions::snapshot().active);
      assert(!zumo_link::state.left && !zumo_link::state.right && !zumo_link::state.lookActive);
      assert(zumo_link::motions==sent && photo_store::captures==photos);
      assert(!audioEngine.playing && !speech_commands::listening && !person_follow::enabled);
    }
    std::cout << "PASS stop at 160 points: " << robot_actions::name(static_cast<Command>(id)) << '\n';
  }
  resetDevice();
  for (auto routine : {Command::Hello,Command::Dance,Command::LookAround,Command::Picture}) {
    resetDevice();
    const auto photos = photo_store::captures;
    assert(robot_actions::start(routine,robot_actions::generation()));
    for (int i=0;i<1200 && robot_actions::snapshot().active;++i) tick();
    assert(!robot_actions::snapshot().active);
    if (routine==Command::Picture) assert(photo_store::captures==photos+1);
  }
  resetDevice();
  assert(robot_actions::start(Command::Explore,robot_actions::generation()));
  for(int i=0;i<10;++i)tick();
  assert(zumo_link::state.left>0);
  zumo_link::state.distanceMm=180;tick();
  assert(!zumo_link::state.left);
  for(int i=0;i<30;++i)tick();
  zumo_link::state.distanceMm=900;
  for(int i=0;i<40;++i)tick();
  assert(zumo_link::state.left>0);
  ++zumo_link::state.safetyEvents;tick();
  assert(!robot_actions::snapshot().active && !zumo_link::state.left);
  for(int i=0;i<40;++i)tick();
  assert(!zumo_link::state.left);
  resetDevice();
  zumo_link::state.rangeValid=false;
  assert(!robot_actions::start(Command::Explore,robot_actions::generation()));
  resetDevice();
  assert(robot_actions::start(Command::Follow,robot_actions::generation()));
  for(int i=0;i<20;++i)tick();
  vision_service::state.resultAgeMs=2000;tick();
  assert(!zumo_link::state.left);
  resetDevice();
  assert(robot_actions::start(Command::Dance,robot_actions::generation()));
  zumo_link::state.edge=true;tick();
  assert(!robot_actions::snapshot().active);
  std::cout << "PASS exploration obstacle turn, transport timeout, missing range, stale vision, edge stop\n";
}
