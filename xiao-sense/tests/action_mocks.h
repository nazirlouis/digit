#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "audio_cues.h"
#include "emotion_voice.h"
inline uint32_t nowMs = 1000;
inline uint32_t millis() { return nowMs; }
using SemaphoreHandle_t = std::recursive_mutex*;
constexpr int portMAX_DELAY = 0;
inline auto xSemaphoreCreateRecursiveMutex() { return new std::recursive_mutex; }
inline void xSemaphoreTakeRecursive(SemaphoreHandle_t m,int) { m->lock(); }
inline void xSemaphoreGiveRecursive(SemaphoreHandle_t m) { m->unlock(); }
#define configASSERT(x) assert(x)
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#ifdef FOLLOW_TRANSLATION
static struct { void println(const char*) {} template<class... T> void printf(const char*,T...) {} } Serial;
#endif
struct AudioEngine {
  bool playing = false;
  bool available = true;
  unsigned plays = 0;
  AudioCue lastCue = AudioCue::None;
  uint32_t playUntil = 0;
  bool playCue(AudioCue cue, VoiceTiming timing=VoiceTiming::Expression) { if(!available)return false;playing=true;++plays;lastCue=cue;playUntil=nowMs+emotionDurationMs(cue,timing);return true; }
  bool cuePlaying() { return playing; }
  void cancelOutput() { playing=false; }
};
inline AudioEngine audioEngine;
inline AudioCue emotionCueForFace(const char* name) {
  const char* names[]={"NEUTRAL","HAPPY","SAD","MAD","SERIOUS","CURIOUS","SURPRISED","SLEEPING","CONFUSED","SLEEPY","EXCITED","CRYING","LOVE"};
  for(unsigned i=0;i<13;++i) if(!strcmp(name,names[i])) return static_cast<AudioCue>(static_cast<unsigned>(AudioCue::Neutral)+i);
  return AudioCue::None;
}
namespace wake_word {
struct Status { bool reacting=false,pausedForVision=false,ready=true; char turnReason[96]=""; };
inline Status state;
inline uint32_t epoch=0,lastReaction=0;
inline uint32_t bearingGeneration=0,controlGeneration=0,actionGeneration=0;
inline int pendingDegrees=0;
inline bool reactionPending=false,turning=false,pendingBearing=false;
inline bool waitingForBearing=false;
inline uint32_t bearingWaitAt=0;
constexpr uint32_t kBearingWaitMs=500;
inline bool hasReaction=false,reactionActive=false;
inline int stateLock=0;
constexpr uint32_t kReactionMs=2600;
#ifdef MISSION_TRANSLATION
void cancel();
Status snapshot();
#else
#include "wake_control.inc"
#endif
}
namespace speech_commands {
struct Status { bool listening; };
inline bool listening=false;
inline uint32_t listenGeneration=0;
inline void listen(uint32_t generation) { listening=true;listenGeneration=generation; }
inline void cancel() { listening=false; }
inline Status snapshot() { return {listening}; }
}
namespace person_follow {
struct Status { bool enabled; const char* phase; int targetX; int errorPixels; uint8_t scorePercent; };
bool setEnabled(bool);
bool heartbeat();
void poll();
Status snapshot();
}
namespace photo_store {
inline bool pending=false,cancelled=false;
inline unsigned captures=0;
inline void begin() {}
inline bool capture(uint32_t) { ++captures; pending=true; return true; }
inline void cancel() { pending=false; cancelled=true; }
inline bool busy() { return pending; }
inline bool failed() { return false; }
}
namespace camera_service {}
namespace personality_events {
inline void cancel() {}
inline void interaction() {}
inline void safetyStopped() {}
}
namespace vision_service {
inline constexpr uint8_t kPersonConfidencePercent=55;
struct Status {
  bool ready=true,enabled=false,hasResult=true,personDetected=true,batteryMode=false,batteryReady=true;
  uint32_t resultAgeMs=0,resultTimestampMs=1,captureTimestampMs=0,totalMs=0;
  struct Box { int x1=100,y1=40,x2=210,y2=200; uint8_t scorePercent=90,classId=0; } boxes[5];
  uint8_t boxCount=1;
  int imageWidth=320,imageHeight=240;
};
inline Status state;
inline Status snapshot() { return state; }
inline bool setEnabled(bool enabled) { state.enabled=enabled; state.batteryMode=false; return state.ready; }
inline bool setBatteryEnabled(bool enabled) { state.enabled=enabled; state.batteryMode=enabled; return state.batteryReady; }
}
namespace zumo_link {
struct Status {
  bool connected=true,ready=true,calibrating=false,edge=false,actionCapable=true;
  bool voiceCapable=true,rangeValid=true,rangeNoReturn=false,lookActive=false,manualDriving=false,rampedLookCapable=true;
  bool wiggleCapable=true,sceneCapable=true,autoRampCapable=true,closeApproachCapable=true,approachArrived=false,chaseCloseCapable=true;
  int left=0,right=0,distanceMm=900;
  int rawDistanceMm=900,rangeStatus=0,encoderLeftCps=0,encoderRightCps=0;
  bool encoderControl=true;
  bool circleCapable=true;
  bool motionCapable=true,motionArmed=false,motionActive=false;
  uint32_t reactionVersion=reaction_catalog::version;
  uint32_t motionBoot=1,motionSession=0,motionJob=0,motionRangeAgeMs=0,motionSample=0,motionTimestampMs=0;
  int motionDistanceMm=-1,motionHeadingDeciDeg=0;
  int motionRangeStatus=-1,motionRawDistanceMm=-1;
  char motionState[16]="DISARMED",motionResult[24]="NONE";
  uint32_t controlMaxGapMs=0,controlOverruns=0,displayMaxSliceUs=0;
  int targetLeftCps=0,targetRightCps=0;
  uint32_t controlGeneration=0,replyAgeMs=0,safetyEvents=0,rangeAgeMs=0;
  uint32_t statusTimestampMs=0;
  char fault[24]="NONE",lookResult[24]="NONE",sceneResult[24]="NONE",wakeResult[24]="NONE",lastError[32]="NONE";
};
inline Status state;
inline unsigned motions=0;
inline unsigned stops=0;
inline bool lastStopSmooth=false;
inline uint32_t turnAt=0;
inline int selectedAcceleration=6000;
inline int lastStopDistance=150;
inline bool gesture=false,excitedGesture=false,sceneAvailable=true,sceneAcknowledged=true;
inline std::vector<int> angles;
inline std::vector<std::string> scenes;
inline int motionKind=0;
inline uint32_t motionAt=0,motionDeadline=0,motionLastSample=0,supervisorAt=0;
inline unsigned nextMotionJob=0,driveUpdates=0,measureJobs=0;
inline int motionSamples[3]={},motionSampleCount=0,pendingAngle=0;
inline Status snapshot() { return state; }
inline void stop(bool smooth=false) { lastStopSmooth=smooth;++stops;state.approachArrived=false;state.left=state.right=0;state.lookActive=state.manualDriving=false;state.motionActive=state.motionArmed=false;motionKind=0;strcpy(state.motionState,"DISARMED");++state.controlGeneration; }
inline bool safeDrive(int left,int right,int stopMm=150) { lastStopDistance=stopMm;state.left=left;state.right=right;++motions;return true; }
inline bool turn(int speed) { state.left=speed;state.right=-speed;++motions;return state.ready; }
inline bool look(int angle,uint32_t) { gesture=false;angles.push_back(angle);state.lookActive=true;turnAt=nowMs;std::strcpy(state.lookResult,"TURNING");++motions;return true; }
inline bool wiggle(uint32_t generation,bool excited=false) { look(15,generation);gesture=true;excitedGesture=excited;std::strcpy(state.lookResult,"PREPARING");return true; }
inline bool face(const char*) { return true; }
inline void cancelLook() { stop(); }
inline bool scene(const char* face,const char* label) { if(!sceneAvailable)return false;scenes.push_back(std::string(face)+" "+label);std::strcpy(state.sceneResult,sceneAcknowledged?"APPLIED":"SENDING");return true; }
inline void cancelScene() {strcpy(state.sceneResult,"CANCELLED");}
inline void cancelWake() {strcpy(state.wakeResult,"CANCELLED");}
inline bool wake() {if(!sceneAvailable)return false;strcpy(state.wakeResult,sceneAcknowledged?"APPLIED":"RENDERING");return true;}
inline int acceleration() { return selectedAcceleration; }
inline void superviseMotion() { supervisorAt=nowMs; }
inline bool armMotion() { state.motionArmed=state.ready && state.motionCapable && state.encoderControl;return state.motionArmed; }
inline uint32_t startJob(int kind,uint32_t deadline) {
  if(!state.motionArmed || state.motionActive)return 0;
  motionKind=kind; motionAt=nowMs; motionDeadline=deadline;
  state.motionActive=true;state.motionJob=++nextMotionJob;
  strcpy(state.motionState,"RUNNING");strcpy(state.motionResult,"ACCEPTED");
  return state.motionJob;
}
inline uint32_t driveJob(int left,int right,int stopMm,uint32_t maxMs=30000) {
  auto job=startJob(1,maxMs);if(job){state.left=left;state.right=right;lastStopDistance=stopMm;++motions;}return job;
}
inline uint32_t turnJob(int angle,uint32_t maxMs=20000) {
  auto job=startJob(2,maxMs);if(job){pendingAngle=angle;look(angle,state.controlGeneration);}return job;
}
inline uint32_t circleJob(int speed) {
  auto job=startJob(4,60000);if(job){state.left=speed/3;state.right=speed;++motions;}return job;
}
inline uint32_t measureJob() {
  auto job=startJob(3,2500);if(job){++measureJobs;motionLastSample=0;motionSampleCount=0;}return job;
}
inline bool updateDriveJob(uint32_t job,int left,int right) {
  if(job!=state.motionJob || motionKind!=1)return false;
  state.left=left;state.right=right;++driveUpdates;return true;
}
inline bool ackMotion(uint32_t job) {return job==state.motionJob;}
inline void cancelMotion(bool smooth=true) {stop(smooth);}
}
