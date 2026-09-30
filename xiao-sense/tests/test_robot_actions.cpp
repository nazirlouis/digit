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
