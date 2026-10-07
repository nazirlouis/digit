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
bool cameraFresh = true, motorStatusFresh = true;
uint32_t cameraPeriod=25, cameraLatency=0, nextFrame=0;
void reset() {
  robot_actions::stop(); nowMs += 1000;
  zumo_link::state = {}; vision_service::state = {};
  zumo_link::angles.clear(); zumo_link::scenes.clear();
  zumo_link::gesture=zumo_link::excitedGesture=false;
  zumo_link::sceneAvailable=true; zumo_link::selectedAcceleration=6000;
  motorStatusFresh=true; audioEngine.available=true; audioEngine.playing=false; cameraFresh=true; cameraPeriod=25; cameraLatency=0; nextFrame=nowMs;
}
void tick() {
  nowMs+=25;
  if(cameraFresh && vision_service::state.enabled && nowMs>=nextFrame) {
    nextFrame=nowMs+cameraPeriod;
    vision_service::state.captureTimestampMs=nowMs-cameraLatency;
    vision_service::state.totalMs=cameraLatency;
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
  if(motorStatusFresh) zumo_link::state.statusTimestampMs=nowMs;
  person_follow::poll();
  robot_actions::poll();
}
void run(unsigned ms) { for(unsigned t=0;t<ms;t+=25) tick(); }
void begin(Command value) {
  const bool accepted=robot_actions::start(value,robot_actions::generation());
  if(!accepted) std::cerr << "Start rejected: " << robot_actions::name(value) << ": " << robot_actions::snapshot().phase << '\n';
  assert(accepted);
}
void untilDone(unsigned limit=180000) {
  for(unsigned t=0;t<limit && robot_actions::snapshot().active;t+=25) tick();
  assert(!robot_actions::snapshot().active);
  assert(!zumo_link::state.left && !zumo_link::state.right && !zumo_link::state.lookActive);
  assert(!vision_service::state.enabled);
}
bool shown(const char* scene) { return std::find(zumo_link::scenes.begin(),zumo_link::scenes.end(),scene)!=zumo_link::scenes.end(); }
void untilDriving(int speed=2800) {
  for(int i=0;i<600 && !zumo_link::state.left;++i) tick();
  assert(zumo_link::state.left==speed && zumo_link::state.right==speed);
}
int main() {
  robot_actions::begin();
  // Follow uses only camera turning, even without gyro, object model or ToF.
  reset();
  zumo_link::state.voiceCapable=false; zumo_link::state.rangeValid=false;
  vision_service::state.batteryReady=false;
  begin(Command::FollowMe);run(3000);
  assert(robot_actions::snapshot().active && person_follow::snapshot().enabled);
  assert(!zumo_link::state.left && !zumo_link::state.right);
  vision_service::state.boxes[0].x1=0;vision_service::state.boxes[0].x2=40;
  tick();assert(zumo_link::state.left==3000 && zumo_link::state.right==-3000);
  const auto correctionStart=nowMs;
  while(zumo_link::state.left && nowMs-correctionStart<=750) tick();
  assert(!zumo_link::state.left && nowMs-correctionStart<=700);
  // Frames every 25 ms cannot extend the original deadline. Delayed frames
  // exposed during motion/settling cannot start the next correction either.
  cameraLatency=689;
  vision_service::state.boxes[0].x1=280;vision_service::state.boxes[0].x2=320;
  const auto stoppedMotions=zumo_link::motions;
  run(700);assert(!zumo_link::state.left && zumo_link::motions==stoppedMotions);
  run(200);assert(zumo_link::state.left==-3000 && zumo_link::state.right==3000);
  cameraLatency=0;
  vision_service::state.boxes[0].x1=140;vision_service::state.boxes[0].x2=180;
  tick();assert(!zumo_link::state.left && !zumo_link::state.right);
  // Lost target holds position and stays armed for their return.
  vision_service::state.boxCount=0;run(5000);
  assert(robot_actions::snapshot().active && !zumo_link::state.left);
  vision_service::state.boxCount=1;
  vision_service::state.boxes[0].x1=0;vision_service::state.boxes[0].x2=40;
  tick();assert(zumo_link::state.left==3000);
  vision_service::state.boxes[0].scorePercent=54;
  tick();assert(!zumo_link::state.left);
  vision_service::state.boxes[0].scorePercent=90;run(250);
  cameraFresh=false;run(1300);assert(!zumo_link::state.left);
  cameraFresh=true;cameraLatency=2500;tick();assert(!zumo_link::state.left);
  cameraLatency=900;cameraPeriod=900;run(2500);
  assert(robot_actions::snapshot().active);
  assert(zumo_link::state.left==-zumo_link::state.right);
  const auto oldFollowGeneration=robot_actions::generation();
  robot_actions::stop();run(3000);
  assert(!person_follow::snapshot().enabled && !vision_service::state.enabled);
  assert(!zumo_link::state.left && !zumo_link::state.right);
  assert(!robot_actions::start(Command::FollowMe,oldFollowGeneration));
  assert(!robot_actions::start(static_cast<Command>(15),robot_actions::generation()));

  // Small corrections are shorter, but retain enough ramp to break away.
  uint32_t durations[2]{};
  for(int index=0;index<2;++index) {
    reset();begin(Command::FollowMe);run(250);
    const int center=index?20:130;
    vision_service::state.boxes[0].x1=center-20;vision_service::state.boxes[0].x2=center+20;
    tick();assert(zumo_link::state.left==3000);
    const auto start=nowMs;
    while(zumo_link::state.left && nowMs-start<=750) tick();
    durations[index]=nowMs-start;
    assert(!zumo_link::state.left && durations[index]<=700);
  }
  assert(durations[0]>=550 && durations[0]<durations[1]);
  // Wider center band ignores +/- 20 pixel detection wobble.
  reset();begin(Command::FollowMe);run(250);
  for(int center : {140,180,140,180}) {
    vision_service::state.boxes[0].x1=center-20;vision_service::state.boxes[0].x2=center+20;
    run(1000);assert(!zumo_link::state.left);
  }
  // Crossing center first stops; it never reverses on that moving-frame result.
  vision_service::state.boxes[0].x1=0;vision_service::state.boxes[0].x2=40;tick();
  vision_service::state.boxes[0].x1=280;vision_service::state.boxes[0].x2=320;tick();
  assert(!zumo_link::state.left);
  run(100);assert(!zumo_link::state.left);
  run(150);assert(zumo_link::state.left==-3000);
  // Cached zero motor values cannot acknowledge the STOP; missing STATUS
  // cancels tracking, even when camera frames and other activity continue.
  reset();begin(Command::FollowMe);motorStatusFresh=false;
  vision_service::state.boxes[0].x1=0;vision_service::state.boxes[0].x2=40;
  const auto noStatusMotions=zumo_link::motions;run(2100);
  assert(!robot_actions::snapshot().active && zumo_link::motions==noStatusMotions);
  for(int fault=0;fault<5;++fault) {
    reset();begin(Command::FollowMe);
    vision_service::state.boxes[0].x1=0;vision_service::state.boxes[0].x2=40;
    run(250);assert(zumo_link::state.left);
    if(fault==0)zumo_link::state.edge=true;
    if(fault==1)zumo_link::state.connected=false;
    if(fault==2)vision_service::state.batteryMode=true;
    if(fault==3)robot_actions::manualOverride();
    if(fault==4)++zumo_link::state.safetyEvents;
    tick();assert(!robot_actions::snapshot().active && !person_follow::snapshot().enabled);
    assert(!zumo_link::state.left && !zumo_link::state.right);
  }
  std::cout<<"PASS Follow me: bounded adaptive corrections, post-stop capture gate, 689 ms latency, centering, lost targets, Stop and safety cancellation\n";
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
  assert(zumo_link::lastStopDistance==100);
  zumo_link::state.distanceMm=500; run(2000);
  assert(robot_actions::snapshot().active && !zumo_link::excitedGesture);
  zumo_link::state.distanceMm=100; untilDone();
  assert(zumo_link::excitedGesture && shown("EXCITED FOUND"));
  reset(); begin(Command::FindMe); untilDriving();
  cameraFresh=false; run(1300); assert(!zumo_link::state.left);
  untilDone();
  reset(); cameraPeriod=400; zumo_link::selectedAcceleration=500;
  begin(Command::FindMe); untilDriving(); run(5500);
  assert(zumo_link::state.left==2800); // Slow selected ramp has time to reach useful torque.
  zumo_link::state.distanceMm=100; untilDone();
  reset(); begin(Command::FindMe); untilDriving();
  zumo_link::state.rangeValid=false; tick(); assert(!zumo_link::state.left);
  untilDone(); assert(shown("CONFUSED OBSTACLE"));
  reset(); vision_service::state.boxes[0].x1=230; vision_service::state.boxes[0].x2=310;
  begin(Command::FindMe); run(1500);
  assert(!zumo_link::angles.empty() && zumo_link::angles[0]>0);
  assert(!zumo_link::state.left); robot_actions::stop();
  std::cout<<"PASS person approach, mirrored steering, ToF stopping, camera/target loss and synchronized celebration\n";

  // A dashboard-visible 55% person must count, even across one missed frame.
  reset(); vision_service::state.boxes[0].scorePercent=55; begin(Command::FindMe);
  for(int i=0;i<100 && strcmp(robot_actions::snapshot().phase,"Person seen; confirming");++i)tick();
  assert(!strcmp(robot_actions::snapshot().phase,"Person seen; confirming"));
  assert(!zumo_link::state.left && zumo_link::angles.empty());
  vision_service::state.boxCount=0; tick();
  assert(!strcmp(robot_actions::snapshot().phase,"Person briefly lost; checking again"));
  vision_service::state.boxCount=1; tick(); untilDriving();
  reset(); vision_service::state.boxes[0].scorePercent=54; begin(Command::FindMe); run(1700);
  assert(!zumo_link::state.left && zumo_link::angles.empty());
  reset(); begin(Command::FindMe);
  for(int i=0;i<100 && strcmp(robot_actions::snapshot().phase,"Person seen; confirming");++i)tick();
  vision_service::state.boxCount=0; run(75); vision_service::state.boxCount=1; tick();
  assert(!strcmp(robot_actions::snapshot().phase,"Person seen; confirming"));
  assert(!zumo_link::state.left);
  reset(); zumo_link::state.distanceMm=200; begin(Command::FindMe);
  for(int i=0;i<200 && !zumo_link::state.left;++i)tick();
  assert(zumo_link::state.left==2400 && zumo_link::lastStopDistance==100);
  // Local arrival can be followed by a small range bounce; never drive again before confirming.
  zumo_link::state.approachArrived=true; zumo_link::state.distanceMm=110; tick();
  assert(!zumo_link::state.left); untilDone(); assert(shown("EXCITED FOUND"));
  reset(); zumo_link::state.closeApproachCapable=false;
  assert(!robot_actions::start(Command::FindMe,robot_actions::generation()));
  reset(); zumo_link::state.edge=true;
  assert(!robot_actions::start(Command::FindMe,robot_actions::generation()));
  assert(strstr(robot_actions::snapshot().phase,"Edge stop"));
  std::cout<<"PASS 10 cm target, close steps above torque threshold, local arrival, 55% person and two-of-three confirmation\n";

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
  assert(zumo_link::lastStopDistance==50);
  zumo_link::state.distanceMm=100; tick();
  assert(zumo_link::state.left && !shown("HAPPY MARKER"));
  zumo_link::state.distanceMm=80; tick();
  assert(zumo_link::state.left && !shown("HAPPY MARKER"));
  zumo_link::state.distanceMm=50; run(2000);
  assert(shown("HAPPY MARKER") && robot_actions::snapshot().active && !zumo_link::state.left);
  cameraPeriod=400; run(10000);
  assert(robot_actions::snapshot().active && !zumo_link::state.left); // Still watching fresh frames between results.
  const auto caughtScenes=zumo_link::scenes.size();
  for(int range:{60,80,99}) {
    zumo_link::state.distanceMm=range; run(1500);
    assert(!zumo_link::state.left && zumo_link::scenes.size()==caughtScenes);
  }
  zumo_link::state.distanceMm=100; untilDriving(2400);
  assert(zumo_link::lastStopDistance==50);
  vision_service::state.boxes[0].classId=2; tick(); assert(!zumo_link::state.left);
  untilDone(); assert(shown("CONFUSED LOST"));
  assert(zumo_link::angles.size()==4);
  reset(); begin(Command::Chase); untilDriving();
  zumo_link::state.distanceMm=50; untilDone(70000);
  assert(!strcmp(robot_actions::snapshot().phase,"Chase finished"));
  reset(); zumo_link::state.distanceMm=200; begin(Command::Chase); untilDriving(2400);
  zumo_link::state.approachArrived=true; zumo_link::state.distanceMm=60; tick();
  assert(!zumo_link::state.left); run(2000); assert(shown("HAPPY BATTERY"));
  zumo_link::state.distanceMm=100; untilDriving(2400);
  reset(); zumo_link::state.chaseCloseCapable=false;
  assert(!robot_actions::start(Command::Chase,robot_actions::generation()));
  assert(strstr(robot_actions::snapshot().phase,"5 cm"));
  std::cout<<"PASS chase 5 cm stop, 10 cm resume, short close steps, local arrival bounce, class lock, lost target and time limit\n";

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
  for(int id : {8,9,10,11,12,13,14,16}) {
    for(int stopAt=0;stopAt<15000;stopAt+=125) {
      reset(); vision_service::state.boxes[0].classId=static_cast<uint8_t>((id>=10&&id<=12)?id-10:id==16?0:1);
      if(id==16) { vision_service::state.boxes[0].x1=0;vision_service::state.boxes[0].x2=40; }
      begin(static_cast<Command>(id));
      for(int t=0;t<stopAt;t+=25) {
        if(t>6000) zumo_link::state.distanceMm=id==8?100:id==13?50:400;
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
  std::cout<<"PASS Stop at 960 mission points, fresh-wake takeover, generation races, link/edge events, audio/display/model failures\n";
}
