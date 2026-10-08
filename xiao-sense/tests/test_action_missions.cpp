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
bool cameraFresh = true, motorStatusFresh = true, turnCompletes = true, audioCompletes = true;
uint32_t cameraPeriod=25, cameraLatency=0, nextFrame=0;
void reset() {
  robot_actions::stop(); nowMs += 1000;
  zumo_link::state = {}; vision_service::state = {};
  zumo_link::angles.clear(); zumo_link::scenes.clear();
  zumo_link::gesture=zumo_link::excitedGesture=false;
  zumo_link::sceneAcknowledged=true; turnCompletes=audioCompletes=true;
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
  if(zumo_link::state.lookActive && turnCompletes) {
    const auto age=nowMs-zumo_link::turnAt;
    if(age>1400) { zumo_link::state.lookActive=false; strcpy(zumo_link::state.lookResult,"DONE"); }
    else if(zumo_link::gesture && age>1000) strcpy(zumo_link::state.lookResult,"CENTERING");
    else if(zumo_link::gesture && age>450) strcpy(zumo_link::state.lookResult,"WIGGLING");
  }
  // Simulated local motion runtime. Host rangeAge is transport age; only the
  // local validity/distance below governs the simulated Zumo safety stop.
  if(zumo_link::state.motionActive) {
    using namespace zumo_link;
    const char* completed=nullptr;
    if(motionKind==1) {
      if(!state.rangeValid) {++state.safetyEvents;strcpy(state.lastError,"RANGE_UNAVAILABLE");}
      else if(state.distanceMm<=lastStopDistance || state.approachArrived) completed="WALL_STOPPED";
    } else if(motionKind==2 && turnCompletes && nowMs-motionAt>1400) {
      state.motionHeadingDeciDeg+=pendingAngle*10;completed="TURN_DONE";
    } else if(motionKind==3 && nowMs-motionAt>=120 && nowMs-motionLastSample>=50 && state.rangeValid) {
      motionLastSample=nowMs;motionSamples[motionSampleCount++]=state.distanceMm;
      if(motionSampleCount==3) {
        std::sort(motionSamples,motionSamples+3);state.motionDistanceMm=motionSamples[1];completed="MEASURED";
      }
    }
    if(completed) {
      state.motionActive=false;state.left=state.right=0;state.motionTimestampMs=nowMs;
      state.motionSample=state.motionJob;state.motionRangeAgeMs=0;
      strcpy(state.motionState,"DONE");strcpy(state.motionResult,completed);
      motionKind=0;
    }
  }
  if(audioCompletes && nowMs>=audioEngine.playUntil) audioEngine.playing=false;
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
  if(robot_actions::snapshot().active) std::cerr<<"Still active: "<<robot_actions::snapshot().phase<<" after "<<limit<<" ms\n";
  assert(!robot_actions::snapshot().active);
  assert(!zumo_link::state.left && !zumo_link::state.right && !zumo_link::state.lookActive);
  assert(!vision_service::state.enabled);
}
bool shown(const char* scene) { return std::find(zumo_link::scenes.begin(),zumo_link::scenes.end(),scene)!=zumo_link::scenes.end(); }
void untilDriving(int speed=3000) {
  for(int i=0;i<600 && !zumo_link::state.left;++i) tick();
  assert(zumo_link::state.left==speed && zumo_link::state.right==speed);
}
int main() {
  std::cout<<std::unitbuf;
  robot_actions::begin();
  // A delayed rear bearing can be acquired before any reply audio starts.
  const auto wakeIdle = []() {
    reset();voice_direction::state={};voice_direction::bearingReady=false;
    voice_direction::retry=true;voice_direction::failure="Voice direction not stable";
    wake_word::state.ready=true;wake_word::pendingBearing=false;
    wake_word::bearingGeneration=0;
    wake_word::controlGeneration=zumo_link::state.controlGeneration;
    wake_word::actionGeneration=robot_actions::generation();
    wake_word::reactionPending=true;wake_word::lastReaction=nowMs;
    wake_word::poll();
  };
  wakeIdle();
  assert(wake_word::waitingForBearing && !speech_commands::listening && !audioEngine.playing);
  nowMs+=200;voice_direction::bearingReady=true;wake_word::poll();
  assert(wake_word::turning && !wake_word::waitingForBearing);
  assert(zumo_link::angles.back()==180 && !audioEngine.playing);
  zumo_link::state.lookActive=false;strcpy(zumo_link::state.lookResult,"DONE");
  wake_word::poll();assert(speech_commands::listening && audioEngine.playing);
  assert(!strcmp(wake_word::snapshot().turnReason,"Turn completed"));
  // Uncertain or stale direction waits at most 500 ms, then answers once.
  wakeIdle();voice_direction::failure="Voice direction too old";
  const auto plays=audioEngine.plays;
  nowMs+=499;wake_word::poll();assert(!speech_commands::listening);
  ++nowMs;wake_word::poll();assert(speech_commands::listening && audioEngine.plays==plays+1);
  assert(strstr(wake_word::snapshot().turnReason,"too old"));
  nowMs+=100;voice_direction::bearingReady=true;wake_word::poll();
  assert(!wake_word::turning && audioEngine.plays==plays+1 && zumo_link::angles.empty());
  // Stop/manual/settings changes cannot release delayed motion or reply audio.
  for(int cancellation=0;cancellation<3;++cancellation) {
    wakeIdle();const auto sounds=audioEngine.plays;
    if(cancellation==0)robot_actions::stop();
    if(cancellation==1)robot_actions::manualOverride();
    if(cancellation==2)++zumo_link::state.controlGeneration;
    nowMs+=200;voice_direction::bearingReady=true;wake_word::poll();
    assert(!wake_word::turning && !wake_word::waitingForBearing && !speech_commands::listening);
    assert(zumo_link::angles.empty() && audioEngine.plays==sounds);
  }
  wakeIdle();++voice_direction::state.generation;voice_direction::bearingReady=true;
  wake_word::poll();assert(!wake_word::turning && speech_commands::listening);
  assert(strstr(wake_word::snapshot().turnReason,"setup changed"));
  wakeIdle();voice_direction::retry=false;voice_direction::failure="Zumo edge stop";
  wake_word::poll();assert(!wake_word::waitingForBearing && speech_commands::listening);
  assert(strstr(wake_word::snapshot().turnReason,"edge stop"));
  std::cout<<"PASS wake bearing acquisition, rear turn, 500 ms deadline, reply once, Stop/manual/setup cancellation and skip reasons\n";
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

  reset(); begin(Command::ComeSayHello); untilDriving();
  const auto continuousJob=zumo_link::state.motionJob;
  const auto continuousStarts=zumo_link::motions;
  vision_service::state.boxes[0].x1=130;vision_service::state.boxes[0].x2=240;run(300);
  assert(zumo_link::state.left==3000 && zumo_link::state.right>3000);
  assert(zumo_link::state.motionJob==continuousJob && zumo_link::motions==continuousStarts);
  vision_service::state.boxes[0].x1=80;vision_service::state.boxes[0].x2=190;run(300);
  assert(zumo_link::state.left>3000 && zumo_link::state.right==3000);
  assert(zumo_link::state.motionJob==continuousJob && zumo_link::motions==continuousStarts);
  robot_actions::stop();
  reset(); begin(Command::ComeSayHello); untilDriving();
  // Loss on the next camera result stops forward motion; there is no blind pursuit.
  vision_service::state.boxCount=0; tick();
  assert(!zumo_link::state.left && zumo_link::lastStopSmooth);
  robot_actions::stop();
  reset(); begin(Command::ComeSayHello); untilDriving();
  assert(zumo_link::lastStopDistance==100);
  zumo_link::state.distanceMm=500; run(2000);
  assert(robot_actions::snapshot().active && !zumo_link::excitedGesture);
  zumo_link::state.distanceMm=100; untilDone();
  assert(zumo_link::excitedGesture && shown("LOVE FOUND"));
  reset(); begin(Command::ComeSayHello); untilDriving();
  cameraFresh=false; run(1300); assert(!zumo_link::state.left);
  untilDone();
  reset(); cameraPeriod=400; zumo_link::selectedAcceleration=500;
  begin(Command::ComeSayHello); untilDriving(); run(5500);
  assert(zumo_link::state.left==3000); // Slow selected ramp has time to reach useful torque.
  zumo_link::state.distanceMm=100; untilDone();
  reset(); begin(Command::ComeSayHello); untilDriving();
  zumo_link::state.rangeValid=false; tick(); assert(!zumo_link::state.left);
  untilDone(); assert(strstr(robot_actions::snapshot().phase,"distance"));
  reset(); vision_service::state.boxes[0].x1=230; vision_service::state.boxes[0].x2=310;
  begin(Command::ComeSayHello); run(4000);
  assert(!zumo_link::angles.empty() && zumo_link::angles[0]>0);
  assert(!zumo_link::state.left); robot_actions::stop();
  std::cout<<"PASS person approach, mirrored steering, ToF stopping, camera/target loss and synchronized celebration\n";

  // A dashboard-visible 55% person must count, even across one missed frame.
  reset(); vision_service::state.boxes[0].scorePercent=55; begin(Command::ComeSayHello);
  for(int i=0;i<400 && strcmp(robot_actions::snapshot().phase,"Person seen; confirming");++i)tick();
  assert(!strcmp(robot_actions::snapshot().phase,"Person seen; confirming"));
  assert(!zumo_link::state.left && zumo_link::angles.empty());
  vision_service::state.boxCount=0; tick();
  assert(!strcmp(robot_actions::snapshot().phase,"Person briefly lost; checking again"));
  vision_service::state.boxCount=1; tick(); untilDriving();
  reset(); vision_service::state.boxes[0].scorePercent=54; begin(Command::ComeSayHello); run(1700);
  assert(!zumo_link::state.left && zumo_link::angles.empty());
  reset(); begin(Command::ComeSayHello);
  for(int i=0;i<400 && strcmp(robot_actions::snapshot().phase,"Person seen; confirming");++i)tick();
  vision_service::state.boxCount=0; run(75); vision_service::state.boxCount=1; tick();
  assert(!strcmp(robot_actions::snapshot().phase,"Person seen; confirming"));
  assert(!zumo_link::state.left);
  reset(); zumo_link::state.distanceMm=200; begin(Command::ComeSayHello);
  for(int i=0;i<600 && !zumo_link::state.left;++i)tick();
  assert(zumo_link::state.left==3000 && zumo_link::lastStopDistance==100);
  // Local arrival can be followed by a small range bounce; never drive again before confirming.
  zumo_link::state.approachArrived=true; zumo_link::state.distanceMm=110; tick();
  assert(!zumo_link::state.left); untilDone(); assert(shown("LOVE FOUND"));
  reset(); zumo_link::state.closeApproachCapable=false;
  assert(!robot_actions::start(Command::ComeSayHello,robot_actions::generation()));
  reset(); zumo_link::state.edge=true;
  assert(!robot_actions::start(Command::ComeSayHello,robot_actions::generation()));
  assert(strstr(robot_actions::snapshot().phase,"Edge stop"));
  std::cout<<"PASS 10 cm target, close steps above torque threshold, local arrival, 55% person and two-of-three confirmation\n";

  // Every fresh clearance above 5 cm permits exploratory travel.
  // Continuous travel does not pause for periodic camera checks or step expiry.
  reset();vision_service::state.boxCount=0;begin(Command::Escape);
  while(!audioEngine.playing)tick();
  assert(audioEngine.lastCue==AudioCue::Curious);
  while(!shown("SERIOUS EXIT"))tick();
  assert(!audioEngine.playing); // Silent replacement cannot inherit Curious audio.
  robot_actions::stop();
  reset(); vision_service::state.boxCount=0; zumo_link::state.distanceMm=200;
  begin(Command::Escape); untilDriving();
  const auto straightStops=zumo_link::stops;
  const auto straightScenes=zumo_link::scenes.size();
  run(7000);
  assert(zumo_link::state.left==3000 && zumo_link::stops==straightStops);
  assert(zumo_link::scenes.size()==straightScenes && zumo_link::angles.empty());
  vision_service::state.boxCount=1; tick(); // Battery seen while moving: stop to confirm.
  assert(!zumo_link::state.left); robot_actions::stop();
  // Choose the longer side at right angles. A huge rear opening must never count.
  for(bool longerLeft : {false,true}) {
    reset(); vision_service::state.boxCount=0; zumo_link::state.distanceMm=40;
    begin(Command::Escape);
    for(int i=0;i<1600 && !zumo_link::state.left;++i) {
      int yaw=0; for(int angle:zumo_link::angles) yaw+=angle;
      while(yaw>180)yaw-=360; while(yaw < -180)yaw+=360;
      assert(yaw!=180 && yaw!=-180);
      zumo_link::state.distanceMm=yaw==90?(longerLeft?250:150):yaw==-90?(longerLeft?150:250):40;
      tick();
    }
    assert(zumo_link::state.left==3000);
    assert(zumo_link::angles.size()==(longerLeft?3U:2U));
    assert(zumo_link::angles[0]==90 && zumo_link::angles[1]==-180);
    if(longerLeft)assert(zumo_link::angles[2]==180);
    robot_actions::stop();
  }
  for(int distance : {51,100,150,349,499}) {
    reset(); vision_service::state.boxCount=0; zumo_link::state.distanceMm=distance;
    begin(Command::Escape); untilDriving(3000);
    assert(zumo_link::lastStopDistance==50 && zumo_link::angles.empty());
    zumo_link::state.distanceMm=50; tick();
    assert(!zumo_link::state.left && !zumo_link::state.right);
    robot_actions::stop();
  }
  reset(); vision_service::state.boxCount=0; zumo_link::state.distanceMm=80;
  begin(Command::Escape); untilDriving(3000);
  zumo_link::state.approachArrived=true; tick(); // Local 5 cm stop, with range bounce.
  assert(!zumo_link::state.left);
  vision_service::state.boxCount=1; zumo_link::state.distanceMm=110;
  untilDriving(3000); // Wall arrival must not become battery arrival at 11 cm.
  assert(!zumo_link::excitedGesture && zumo_link::lastStopDistance==100);
  robot_actions::stop();
  reset(); vision_service::state.boxCount=0; zumo_link::state.distanceMm=80;
  begin(Command::Escape); untilDriving(3000);
  zumo_link::state.rangeAgeMs=350; tick(); assert(zumo_link::state.left==3000);
  motorStatusFresh=false; run(500); assert(zumo_link::state.left==3000);
  robot_actions::stop();
  reset(); zumo_link::state.chaseCloseCapable=false;
  assert(!robot_actions::start(Command::Escape,robot_actions::generation()));
  std::cout<<"PASS continuous maze travel, longest 90-degree side, no rear scan, 50 mm stop, stale range and distinct battery arrival\n";

  reset(); vision_service::state.boxCount=0;
  begin(Command::Escape); untilDriving();
  zumo_link::state.rangeValid=false; tick();
  untilDone();
  assert(strstr(robot_actions::snapshot().phase,"distance") && !shown("CONFUSED OBSTACLE"));
  assert(zumo_link::angles.empty());

  // Camera waiting time is not a distance-sensor outage. A late one-frame
  // range interruption must wait for recovery rather than immediately fail.
  reset(); vision_service::state.boxCount=0; cameraFresh=false;
  begin(Command::Escape); run(4000);
  assert(robot_actions::snapshot().active);
  cameraFresh=true; zumo_link::state.rangeValid=false; tick();
  assert(robot_actions::snapshot().active && !shown("CONFUSED NOT_SURE"));
  zumo_link::state.rangeValid=true; untilDriving();
  robot_actions::stop();

  // Reproduce the captured 305 ms-old STATUS after a long stationary wait.
  reset(); vision_service::state.boxCount=0; cameraFresh=false;
  begin(Command::Escape); run(4000);
  motorStatusFresh=false; run(325);
  assert(robot_actions::snapshot().active && !zumo_link::state.left);
  motorStatusFresh=true; cameraFresh=true; untilDriving();
  robot_actions::stop();
  // A genuinely missing stream still ends the stationary action, bounded.
  reset(); vision_service::state.boxCount=0; cameraFresh=false;
  begin(Command::Escape); run(2500);
  motorStatusFresh=false; run(3400);
  assert(!robot_actions::snapshot().active && !zumo_link::state.left);

  reset(); vision_service::state.boxCount=0; zumo_link::state.distanceMm=50;
  begin(Command::Escape); untilDone();
  assert(shown("CURIOUS SEARCHING") && !shown("CURIOUS OBSTACLE"));
  robot_actions::stop();

  // A locally completed wall at 55 mm is inside the stopped tolerance. It
  // cannot become a new "opening" and cause repeated drive/stop cycles.
  reset();vision_service::state.boxCount=0;zumo_link::state.distanceMm=200;
  begin(Command::Escape);untilDriving();
  const auto deadEndStarts=zumo_link::motions;
  zumo_link::state.distanceMm=55;zumo_link::state.approachArrived=true;tick();
  for(int i=0;i<1200 && robot_actions::snapshot().active;++i) {
    const int yaw=zumo_link::state.motionHeadingDeciDeg/10;
    zumo_link::state.distanceMm=yaw==0?55:40;tick();
  }
  assert(!robot_actions::snapshot().active && shown("CONFUSED OBSTACLE"));
  assert(zumo_link::motions==deadEndStarts+2); // Side turns only, no new drive.

  reset(); vision_service::state.boxCount=0; zumo_link::state.distanceMm=50;
  begin(Command::Escape);
  // The range scan chooses an opening before any forward motion.
  for(int i=0;i<1500 && !zumo_link::state.left;++i) {
    if(!zumo_link::angles.empty()) zumo_link::state.distanceMm=900;
    tick();
  }
  assert(zumo_link::state.left==3000 && zumo_link::angles.size()>=3);
  vision_service::state.boxCount=1; vision_service::state.boxes[0].classId=0;
  zumo_link::state.distanceMm=500; run(6000);
  assert(robot_actions::snapshot().active && !zumo_link::excitedGesture);
  assert(shown("HAPPY BATTERY") && zumo_link::lastStopDistance==100);
  zumo_link::state.distanceMm=100; untilDone(); assert(shown("EXCITED EXIT"));
  // Reaching a marker/screwdriver is never an exit; only a confirmed battery counts.
  for(uint8_t category : {1,2}) {
    reset(); vision_service::state.boxes[0].classId=category;
    zumo_link::state.distanceMm=100; begin(Command::Escape); untilDone();
    assert(!zumo_link::excitedGesture && !shown("EXCITED EXIT"));
  }
  reset(); zumo_link::state.distanceMm=200; begin(Command::Escape); untilDriving(3000);
  assert(zumo_link::lastStopDistance==100);
  zumo_link::state.approachArrived=true; zumo_link::state.distanceMm=110; tick();
  assert(!zumo_link::state.left); untilDone(); assert(shown("EXCITED EXIT"));
  reset(); begin(Command::Escape); untilDriving();
  vision_service::state.boxes[0].classId=1; tick();
  assert(!zumo_link::state.left); zumo_link::state.distanceMm=100; untilDone();
  assert(!shown("EXCITED EXIT"));
  reset(); begin(Command::Escape); untilDriving();
  zumo_link::state.rangeValid=false; tick(); assert(!zumo_link::state.left);
  untilDone(); assert(!shown("EXCITED EXIT"));
  reset(); zumo_link::state.closeApproachCapable=false;
  assert(!robot_actions::start(Command::Escape,robot_actions::generation()));
  reset(); vision_service::state.boxCount=0; zumo_link::state.distanceMm=50;
  begin(Command::Escape); untilDone(); assert(shown("CONFUSED OBSTACLE"));
  reset(); vision_service::state.boxCount=0; begin(Command::Escape); untilDone();
  assert(shown("CONFUSED LOST"));
  std::cout<<"PASS measured escape openings, battery-only exit at 10 cm, target/range loss, blocked course and bounded progress\n";

  // Cancel across search, turns, approach, presentations and celebrations.
  for(int id : {8,9,14}) {
    for(int stopAt=0;stopAt<15000;stopAt+=125) {
      reset(); vision_service::state.boxes[0].classId=static_cast<uint8_t>(id==9?1:0);
      begin(static_cast<Command>(id));
      for(int t=0;t<stopAt;t+=25) {
        if(t>6000) zumo_link::state.distanceMm=100;
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
  reset(); begin(Command::ComeSayHello); untilDriving(); ++zumo_link::state.safetyEvents; tick();
  assert(!robot_actions::snapshot().active && !zumo_link::state.left);
  reset(); begin(Command::ComeSayHello); untilDriving(); zumo_link::state.connected=false; tick();
  assert(!robot_actions::snapshot().active && !zumo_link::state.left);
  reset(); begin(Command::ComeSayHello); untilDriving();
  const auto wakeGeneration=robot_actions::generation();
  wake_word::actionGeneration=wakeGeneration; wake_word::reactionPending=true;
  wake_word::lastReaction=nowMs; wake_word::poll();
  assert(!robot_actions::snapshot().active && !zumo_link::state.left);
  assert(speech_commands::listening && speech_commands::listenGeneration==robot_actions::generation());
  assert(robot_actions::generation()!=wakeGeneration && !vision_service::state.enabled);
  const auto wakeMotions=zumo_link::motions; run(15000); assert(zumo_link::motions==wakeMotions);
  reset(); audioEngine.available=false; begin(Command::ComeSayHello); untilDone(4000);
  reset(); zumo_link::sceneAvailable=false; begin(Command::Identify); untilDone(4000);
  reset(); zumo_link::state.autoRampCapable=false;
  assert(!robot_actions::start(Command::ComeSayHello,robot_actions::generation()));
  reset(); vision_service::state.batteryReady=false;
  assert(!robot_actions::start(Command::Identify,robot_actions::generation()));
  std::cout<<"PASS Stop at 360 mission points, fresh-wake takeover, generation races, link/edge events, audio/display/model failures\n";
  // Reports from before a stop request cannot authorize scenes or new motion.
  reset(); motorStatusFresh=false;
  begin(Command::ComeSayHello);untilDone(4000);
  assert(zumo_link::angles.empty() && zumo_link::scenes.empty());
  reset();begin(Command::ComeSayHello);untilDriving();
  vision_service::state.boxCount=0;tick();motorStatusFresh=false;
  const auto stoppedMotions=zumo_link::motions;
  vision_service::state.boxCount=1;untilDone(4000);
  assert(zumo_link::motions==stoppedMotions);
  // A zero-speed echo cannot skip the post-stop capture and settling gate.
  reset();begin(Command::ComeSayHello);untilDriving();
  vision_service::state.boxCount=0;tick();
  cameraLatency=700;vision_service::state.boxCount=1;
  const auto beforeSettling=zumo_link::motions;
  run(650);assert(zumo_link::motions==beforeSettling);
  run(2200);assert(zumo_link::motions>beforeSettling);
  robot_actions::stop();
  // Lost display replies and stuck turns/playback have short, terminal deadlines.
  reset();zumo_link::sceneAcknowledged=false;
  begin(Command::Identify);untilDone(5000);
  assert(strstr(robot_actions::snapshot().phase,"acknowledgement"));
  reset();begin(Command::Identify);
  while(!shown("SURPRISED BATTERY"))tick();
  zumo_link::sceneAcknowledged=false;untilDone(7000);
  assert(strstr(robot_actions::snapshot().phase,"acknowledgement"));
  reset();turnCompletes=false;
  vision_service::state.boxes[0].x1=230;vision_service::state.boxes[0].x2=310;
  begin(Command::ComeSayHello);untilDone(19000);
  assert(strstr(robot_actions::snapshot().phase,"Turn timed out"));
  assert(!zumo_link::state.lookActive && !audioEngine.playing);
  reset();audioCompletes=false;
  begin(Command::Identify);untilDone(4000);
  assert(strstr(robot_actions::snapshot().phase,"playback timed out"));
  // Object evidence is spatially coherent and bounded in time.
  reset();cameraPeriod=1000;begin(Command::Identify);untilDone();
  assert(!shown("HAPPY BATTERY"));
  reset();vision_service::state.boxCount=0;begin(Command::Identify);run(emotionDurationMs(AudioCue::Curious)+200);
  vision_service::state.boxCount=1;
  for(int i=0;i<120;++i) {
    vision_service::state.boxes[0].y1=i%2?150:0;
    vision_service::state.boxes[0].y2=i%2?230:80;
    tick();
  }
  assert(!shown("SURPRISED BATTERY"));robot_actions::stop();
  reset();vision_service::state.boxes[0].scorePercent=49;
  begin(Command::Identify);untilDone();assert(shown("CONFUSED NOT_SURE"));
  // One occluded frame is allowed; two misses cannot combine unrelated sightings.
  reset();vision_service::state.boxCount=0;begin(Command::Identify);run(emotionDurationMs(AudioCue::Curious)+200);
  vision_service::state.boxCount=1;tick();tick();
  vision_service::state.boxCount=0;tick();
  vision_service::state.boxCount=1;tick();
  untilDone();assert(shown("SURPRISED BATTERY") && shown("HAPPY BATTERY"));
  reset();vision_service::state.boxCount=0;begin(Command::Identify);run(emotionDurationMs(AudioCue::Curious)+200);
  for(int i=0;i<80;++i) { vision_service::state.boxCount=i%4<2?1:0;tick(); }
  assert(!shown("SURPRISED BATTERY"));robot_actions::stop();
  // Repeated inference timestamps must not count a single exposure repeatedly.
  reset();vision_service::state.boxCount=0;begin(Command::Identify);run(emotionDurationMs(AudioCue::Curious)+200);
  vision_service::state.boxCount=1;tick();cameraFresh=false;
  for(int i=0;i<30;++i) {
    vision_service::state.resultTimestampMs=nowMs+25;
    vision_service::state.resultAgeMs=0;tick();
  }
  assert(!shown("SURPRISED BATTERY"));robot_actions::stop();
  // A slow ramp gets its negotiated time; a later slider change cannot extend it.
  reset();turnCompletes=false;zumo_link::selectedAcceleration=500;
  vision_service::state.boxes[0].x1=230;vision_service::state.boxes[0].x2=310;
  begin(Command::ComeSayHello);
  while(!zumo_link::state.lookActive)tick();
  const auto slowTurnAt=nowMs;run(15000);assert(robot_actions::snapshot().active);
  zumo_link::selectedAcceleration=6000;untilDone(6000);
  assert(nowMs-slowTurnAt>=19000 && strstr(robot_actions::snapshot().phase,"Turn timed out"));
  reset();vision_service::state.ready=false;zumo_link::state.rangeValid=false;
  begin(Command::Identify);untilDone();assert(shown("SURPRISED BATTERY") && shown("HAPPY BATTERY"));
  // One long-distance outlier per three readings cannot create an escape route.
  reset();vision_service::state.boxCount=0;zumo_link::state.distanceMm=50;
  begin(Command::Escape);
  for(int i=0;i<400 && !strstr(robot_actions::snapshot().phase,"checking for a way");++i)tick();
  for(int i=0;i<1600 && robot_actions::snapshot().active;++i) {
    zumo_link::state.distanceMm=i%3==0?1800:50;tick();
  }
  assert(!robot_actions::snapshot().active && shown("CONFUSED OBSTACLE"));
  assert(!zumo_link::state.left && !zumo_link::state.right);
  // No replay of excited audio after a later CENTERING phase has been observed.
  reset();zumo_link::state.distanceMm=100;begin(Command::ComeSayHello);
  for(int i=0;i<400 && !zumo_link::gesture;++i)tick();
  strcpy(zumo_link::state.lookResult,"CENTERING");robot_actions::poll();
  const auto phaseSounds=audioEngine.plays;
  strcpy(zumo_link::state.lookResult,"WIGGLING");robot_actions::poll();
  assert(audioEngine.plays==phaseSounds);robot_actions::stop();
  std::cout<<"PASS stop acknowledgements, 700 ms camera latency, display/turn/audio deadlines, object coherence and scan outliers\n";

}
