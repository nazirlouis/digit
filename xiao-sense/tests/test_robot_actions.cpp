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

  for (int id : {8,9,14}) {
    resetDevice();
    assert(robot_actions::start(static_cast<Command>(id),robot_actions::generation()));
    const auto epoch=robot_actions::generation(), motions=zumo_link::motions, stops=zumo_link::stops;
    for(int retired : {0,1,2,3,4,5,6,10,11,12,13,15,16,17,255}) {
      assert(!robot_actions::validCommand(retired));
      assert(!robot_actions::start(static_cast<Command>(retired),epoch));
      assert(robot_actions::generation()==epoch && robot_actions::snapshot().active);
      assert(zumo_link::motions==motions && zumo_link::stops==stops);
    }
    robot_actions::stop("Dashboard Stop",true);
    assert(zumo_link::lastStopSmooth && !robot_actions::snapshot().active);
    assert(!robot_actions::start(static_cast<Command>(id),epoch));
    robot_actions::stop();assert(!zumo_link::lastStopSmooth);
  }
  // Identification does not depend on the person model, floor, gyro or range.
  resetDevice();zumo_link::state.ready=false;zumo_link::state.voiceCapable=false;
  zumo_link::state.rangeValid=false;vision_service::state.ready=false;
  assert(!robot_actions::unavailableReason(Command::Identify)[0]);
  assert(robot_actions::start(Command::Identify,robot_actions::generation()));
  for(int failure=0;failure<8;++failure) {
    resetDevice();
    if(failure==0)zumo_link::state.voiceCapable=false;
    if(failure==1)zumo_link::state.rangeValid=false;
    if(failure==2)zumo_link::state.rangeAgeMs=250;
    if(failure==3)zumo_link::state.ready=false;
    if(failure==4)zumo_link::state.edge=true;
    if(failure==5)zumo_link::state.connected=false;
    if(failure==6)zumo_link::state.autoRampCapable=false;
    if(failure==7)zumo_link::state.sceneCapable=false;
    for(auto action : {Command::ComeSayHello,Command::Escape}) {
      assert(robot_actions::unavailableReason(action)[0]);
      assert(!robot_actions::start(action,robot_actions::generation()));
    }
  }
  resetDevice();vision_service::state.ready=false;
  assert(robot_actions::unavailableReason(Command::ComeSayHello)[0]);
  assert(!robot_actions::unavailableReason(Command::Escape)[0]);
  resetDevice();vision_service::state.batteryReady=false;
  assert(!robot_actions::unavailableReason(Command::ComeSayHello)[0]);
  assert(robot_actions::unavailableReason(Command::Identify)[0]);
  assert(robot_actions::unavailableReason(Command::Escape)[0]);
  zumo_link::state.connected=false;
  assert(robot_actions::start(Command::Stop,robot_actions::generation()));
  std::cout << "PASS exactly three action IDs, retired requests preserve active action, shared readiness and Stop without hardware\n";
}
