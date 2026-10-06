// Production mission + owner controllers; simulated camera, UART and time.
#include "robot_actions.cpp"
#include <iostream>
struct { void println(const char*) {} template<class... T> void printf(const char*,T...) {} } Serial;
namespace voice_direction { bool accepts(uint32_t) { return true; } }
namespace wake_word {
bool react() { return audioEngine.playCue(AudioCue::Curious); }
#include "wake_poll.inc"
}
using robot_actions::Command;
bool cameraFresh = true;
uint32_t cameraPeriod=25, nextFrame=0;
void reset() {
  robot_actions::stop(); nowMs += 1000;
  zumo_link::state = {}; vision_service::state = {};
  zumo_link::angles.clear(); zumo_link::scenes.clear();
  zumo_link::gesture=zumo_link::excitedGesture=false;
  zumo_link::sceneAvailable=true; zumo_link::selectedAcceleration=6000;
  audioEngine.available=true; audioEngine.playing=false; cameraFresh=true; cameraPeriod=25; nextFrame=nowMs;
}
void tick() {
  nowMs+=25;
  if(cameraFresh && vision_service::state.enabled && nowMs>=nextFrame) {
    nextFrame=nowMs+cameraPeriod;
    vision_service::state.captureTimestampMs=nowMs;
    vision_service::state.resultTimestampMs=nowMs;
    vision_service::state.resultAgeMs=0;
  } else vision_service::state.resultAgeMs+=25;
  if(zumo_link::state.lookActive) {
    const auto age=nowMs-zumo_link::turnAt;
    if(age>1400) { zumo_link::state.lookActive=false; strcpy(zumo_link::state.lookResult,"DONE"); }
    else if(zumo_link::gesture && age>1000) strcpy(zumo_link::state.lookResult,"CENTERING");
    else if(zumo_link::gesture && age>450) strcpy(zumo_link::state.lookResult,"WIGGLING");
  }
  if(nowMs>=audioEngine.playUntil) audioEngine.playing=false;
  robot_actions::poll();
}
void run(unsigned ms) { for(unsigned t=0;t<ms;t+=25) tick(); }
void begin(Command value) { assert(robot_actions::start(value,robot_actions::generation())); }
void untilDone(unsigned limit=180000) {
  for(unsigned t=0;t<limit && robot_actions::snapshot().active;t+=25) tick();
  assert(!robot_actions::snapshot().active);
  assert(!zumo_link::state.left && !zumo_link::state.right && !zumo_link::state.lookActive);
  assert(!vision_service::state.enabled);
}
bool shown(const char* scene) { return std::find(zumo_link::scenes.begin(),zumo_link::scenes.end(),scene)!=zumo_link::scenes.end(); }
void untilDriving() {
  for(int i=0;i<600 && !zumo_link::state.left;++i) tick();
  assert(zumo_link::state.left==2800 && zumo_link::state.right==2800);
}
int main() {
  robot_actions::begin();
  // Recognition requires repeated matching fresh boxes; it never drives.
  for(uint8_t category=0;category<3;++category) {
    reset(); zumo_link::state.ready=false; zumo_link::state.voiceCapable=false;
    vision_service::state.boxes[0].classId=category;
    const auto motions=zumo_link::motions;
    begin(Command::Identify); untilDone();
    assert(zumo_link::motions==motions);
    assert(shown(category==0?"HAPPY BATTERY":category==1?"HAPPY MARKER":"HAPPY SCREWDRIVER"));
  }
  reset(); vision_service::state.boxCount=0; begin(Command::Identify); untilDone();
  assert(shown("CONFUSED NOT_SURE"));
  reset(); cameraFresh=false; begin(Command::Identify); untilDone();
  assert(shown("CONFUSED NOT_SURE"));
  reset(); begin(Command::Identify); run(250);
  for(int i=0;i<80;++i) { vision_service::state.boxes[0].classId=i%2; tick(); }
  assert(!shown("HAPPY BATTERY") && !shown("HAPPY MARKER"));
  robot_actions::stop();
  std::cout<<"PASS stationary identification, all labels, uncertain and stale images, repeated confirmation\n";

  reset(); begin(Command::FindMe); untilDriving();
  // Loss on the next camera result stops forward motion; there is no blind pursuit.
  vision_service::state.boxCount=0; tick();
  assert(!zumo_link::state.left && zumo_link::lastStopSmooth);
  robot_actions::stop();
  reset(); begin(Command::FindMe); untilDriving();
  zumo_link::state.distanceMm=500; untilDone();
  assert(zumo_link::excitedGesture && shown("EXCITED FOUND"));
  reset(); begin(Command::FindMe); untilDriving();
  cameraFresh=false; run(1300); assert(!zumo_link::state.left);
  untilDone();
  reset(); cameraPeriod=400; zumo_link::selectedAcceleration=500;
  begin(Command::FindMe); untilDriving(); run(5500);
  assert(zumo_link::state.left==2800); // Slow selected ramp has time to reach useful torque.
  zumo_link::state.distanceMm=500; untilDone();
  reset(); begin(Command::FindMe); untilDriving();
  zumo_link::state.rangeValid=false; tick(); assert(!zumo_link::state.left);
  untilDone(); assert(shown("CONFUSED OBSTACLE"));
  reset(); vision_service::state.boxes[0].x1=230; vision_service::state.boxes[0].x2=310;
  begin(Command::FindMe); run(1500);
  assert(!zumo_link::angles.empty() && zumo_link::angles[0]>0);
  assert(!zumo_link::state.left); robot_actions::stop();
  std::cout<<"PASS person approach, mirrored steering, ToF stopping, camera/target loss and synchronized celebration\n";

  // Find each object after one scan, then turn back and reacquire any person.
  for(int id=10;id<=12;++id) {
    reset(); vision_service::state.boxCount=0; begin(static_cast<Command>(id));
    while(zumo_link::angles.empty()) tick();
    while(zumo_link::state.lookActive) tick();
    vision_service::state.boxCount=1; vision_service::state.boxes[0].classId=static_cast<uint8_t>(id-10);
    for(int i=0;i<300 && vision_service::state.batteryMode;++i) tick();
    assert(!vision_service::state.batteryMode && vision_service::state.enabled);
    zumo_link::state.distanceMm=500; untilDone();
    assert(zumo_link::angles.size()>=3 && zumo_link::angles[0]==30 && zumo_link::angles[1]==-30);
    assert(shown(id==10?"SURPRISED BATTERY":id==11?"SURPRISED MARKER":"SURPRISED SCREWDRIVER"));
    assert(shown("EXCITED FOUND"));
  }
  reset(); vision_service::state.boxCount=0; begin(Command::FindBattery); untilDone();
  assert(zumo_link::angles.size()==12 && shown("CONFUSED LOST"));
  std::cout<<"PASS all object classes, bounded search, return heading and person reacquisition\n";

  reset(); vision_service::state.boxes[0].classId=1; begin(Command::Chase); untilDriving();
  zumo_link::state.distanceMm=400; run(2000);
  assert(shown("HAPPY MARKER") && robot_actions::snapshot().active && !zumo_link::state.left);
  cameraPeriod=400; run(10000);
  assert(robot_actions::snapshot().active && !zumo_link::state.left); // Still watching fresh frames between results.
  zumo_link::state.distanceMm=650; untilDriving();
  vision_service::state.boxes[0].classId=2; tick(); assert(!zumo_link::state.left);
  untilDone(); assert(shown("CONFUSED LOST"));
  assert(zumo_link::angles.size()==4);
  reset(); begin(Command::Chase); untilDriving();
  zumo_link::state.distanceMm=400; untilDone(70000);
  assert(!strcmp(robot_actions::snapshot().phase,"Chase finished"));
  std::cout<<"PASS chase class lock, stop/resume distance band, lost-target sweep and time limit\n";

  reset(); vision_service::state.boxCount=0; zumo_link::state.distanceMm=300;
  begin(Command::Escape);
  // The range scan chooses an opening before any forward motion.
  for(int i=0;i<1500 && !zumo_link::state.left;++i) {
    if(!zumo_link::angles.empty()) zumo_link::state.distanceMm=900;
    tick();
  }
  assert(zumo_link::state.left==2800 && zumo_link::angles.size()>=3);
  vision_service::state.boxCount=1; vision_service::state.boxes[0].classId=1;
  zumo_link::state.distanceMm=500; untilDone(); assert(shown("EXCITED EXIT"));
  reset(); vision_service::state.boxCount=0; zumo_link::state.distanceMm=200;
  begin(Command::Escape); untilDone(); assert(shown("CONFUSED OBSTACLE"));
  reset(); vision_service::state.boxCount=0; begin(Command::Escape); untilDone();
  assert(shown("CONFUSED LOST"));
  std::cout<<"PASS measured escape openings, exit marker, blocked course and bounded progress\n";

  // Cancel across search, turns, approach, presentations and celebrations.
  for(int id=8;id<=14;++id) {
    for(int stopAt=0;stopAt<15000;stopAt+=125) {
      reset(); vision_service::state.boxes[0].classId=static_cast<uint8_t>((id>=10&&id<=12)?id-10:1);
      begin(static_cast<Command>(id));
      for(int t=0;t<stopAt;t+=25) {
        if(t>6000) zumo_link::state.distanceMm=400;
        tick();
      }
      auto generation=robot_actions::generation();
      robot_actions::stop("Stop");
      const auto motions=zumo_link::motions, sounds=audioEngine.plays;
      assert(!robot_actions::start(static_cast<Command>(id),generation));
      run(3000);
      assert(!robot_actions::snapshot().active && !vision_service::state.enabled);
      assert(zumo_link::motions==motions && audioEngine.plays==sounds && !audioEngine.playing);
    }
  }
  reset(); begin(Command::FindMe); untilDriving(); ++zumo_link::state.safetyEvents; tick();
  assert(!robot_actions::snapshot().active && !zumo_link::state.left);
  reset(); begin(Command::FindMe); untilDriving(); zumo_link::state.connected=false; tick();
  assert(!robot_actions::snapshot().active && !zumo_link::state.left);
  reset(); begin(Command::FindMe); untilDriving();
  const auto wakeGeneration=robot_actions::generation();
  wake_word::actionGeneration=wakeGeneration; wake_word::reactionPending=true;
  wake_word::lastReaction=nowMs; wake_word::poll();
  assert(!robot_actions::snapshot().active && !zumo_link::state.left);
  assert(speech_commands::listening && speech_commands::listenGeneration==robot_actions::generation());
  assert(robot_actions::generation()!=wakeGeneration && !vision_service::state.enabled);
  const auto wakeMotions=zumo_link::motions; run(15000); assert(zumo_link::motions==wakeMotions);
  reset(); audioEngine.available=false; begin(Command::FindMe); untilDone(4000);
  reset(); zumo_link::sceneAvailable=false; begin(Command::Identify); untilDone(4000);
  reset(); zumo_link::state.autoRampCapable=false;
  assert(!robot_actions::start(Command::FindMe,robot_actions::generation()));
  reset(); vision_service::state.batteryReady=false;
  assert(!robot_actions::start(Command::Identify,robot_actions::generation()));
  std::cout<<"PASS Stop at 840 mission points, fresh-wake takeover, generation races, link/edge events, audio/display/model failures\n";
}
