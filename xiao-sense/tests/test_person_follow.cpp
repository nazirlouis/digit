// Production mission + owner controllers; simulated camera, UART and time.
#include "robot_actions.cpp"
#include <iostream>
struct { void println(const char*) {} template<class... T> void printf(const char*,T...) {} } Serial;
namespace voice_direction {
struct Status { uint32_t generation=0; };
Status state;
bool bearingReady=false, retry=true;
const char* failure="Voice direction not stable";
Status snapshot() { return state; }
bool accepts(uint32_t generation) { return generation==state.generation; }
bool target(int& degrees,uint32_t& generation,const char** reason,bool* retryable) {
  *reason=failure;*retryable=retry;
  if(!bearingReady)return false;
  degrees=180;generation=state.generation;return true;
}
}
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
  if(person_follow::snapshot().enabled)person_follow::heartbeat();
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
void beginTracking() {
  assert(vision_service::setEnabled(true));
  assert(person_follow::setEnabled(true));
}
int main() {
  robot_actions::begin();
  // Follow uses only camera turning, even without gyro, object model or ToF.
  reset();
  zumo_link::state.voiceCapable=false; zumo_link::state.rangeValid=false;
  vision_service::state.batteryReady=false;
  beginTracking();run(3000);
  assert(person_follow::snapshot().enabled && person_follow::snapshot().enabled);
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
  assert(person_follow::snapshot().enabled && !zumo_link::state.left);
  vision_service::state.boxCount=1;
  vision_service::state.boxes[0].x1=0;vision_service::state.boxes[0].x2=40;
  tick();assert(zumo_link::state.left==3000);
  vision_service::state.boxes[0].scorePercent=54;
  tick();assert(!zumo_link::state.left);
  vision_service::state.boxes[0].scorePercent=90;run(250);
  cameraFresh=false;run(1300);assert(!zumo_link::state.left);
  cameraFresh=true;cameraLatency=2500;tick();assert(!zumo_link::state.left);
  cameraLatency=900;cameraPeriod=900;run(2500);
  assert(person_follow::snapshot().enabled);
  assert(zumo_link::state.left==-zumo_link::state.right);
  const auto oldFollowGeneration=robot_actions::generation();
  robot_actions::stop();run(3000);
  assert(!person_follow::snapshot().enabled && !vision_service::state.enabled);
  assert(!zumo_link::state.left && !zumo_link::state.right);
  assert(!robot_actions::start(static_cast<Command>(16),oldFollowGeneration));
  assert(!robot_actions::start(static_cast<Command>(15),robot_actions::generation()));

  // Small corrections are shorter, but retain enough ramp to break away.
  uint32_t durations[2]{};
  for(int index=0;index<2;++index) {
    reset();beginTracking();run(250);
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
  reset();beginTracking();run(250);
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
  reset();beginTracking();motorStatusFresh=false;
  vision_service::state.boxes[0].x1=0;vision_service::state.boxes[0].x2=40;
  const auto noStatusMotions=zumo_link::motions;run(2100);
  assert(!person_follow::snapshot().enabled && zumo_link::motions==noStatusMotions);
  for(int fault=0;fault<5;++fault) {
    reset();beginTracking();
    vision_service::state.boxes[0].x1=0;vision_service::state.boxes[0].x2=40;
    run(250);assert(zumo_link::state.left);
    if(fault==0)zumo_link::state.edge=true;
    if(fault==1)zumo_link::state.connected=false;
    if(fault==2)vision_service::state.batteryMode=true;
    if(fault==3)robot_actions::stop("Manual control");
    if(fault==4)strcpy(zumo_link::state.fault,"SENSOR");
    tick();assert(!person_follow::snapshot().enabled);
    assert(!zumo_link::state.left && !zumo_link::state.right);
  }
  std::cout<<"PASS standalone camera turning: bounded corrections, post-stop capture gate, 689 ms latency, reacquisition, Stop and safety cancellation\n";
}
