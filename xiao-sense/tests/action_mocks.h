#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include "audio_cues.h"
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
struct AudioEngine {
  bool playing = false;
  bool available = true;
  unsigned plays = 0;
  AudioCue lastCue = AudioCue::None;
  uint32_t playUntil = 0;
  bool playCue(AudioCue cue) { if(!available)return false;playing=true;++plays;lastCue=cue;playUntil=nowMs+(cue==AudioCue::Happy?555:270);return true; }
  bool cuePlaying() { return playing; }
  void cancelOutput() { playing=false; }
};
inline AudioEngine audioEngine;
namespace wake_word {
struct Status { bool reacting=false,pausedForVision=false; };
inline Status state;
inline uint32_t epoch=0,lastReaction=0;
inline bool reactionPending=false,turning=false,pendingBearing=false;
inline bool hasReaction=false,reactionActive=false;
inline int stateLock=0;
constexpr uint32_t kReactionMs=2600;
#include "wake_control.inc"
}
namespace speech_commands {
struct Status { bool listening; };
inline bool listening=false;
inline void cancel() { listening=false; }
inline Status snapshot() { return {listening}; }
}
namespace person_follow { inline bool enabled=false; inline bool setEnabled(bool v) { enabled=v; return true; } }
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
namespace vision_service {
struct Status {
  bool ready=true,enabled=false,hasResult=true,personDetected=true,batteryMode=false;
  uint32_t resultAgeMs=0,resultTimestampMs=1;
  struct Box { int x1=100,y1=40,x2=210,y2=200; } boxes[5];
  uint8_t boxCount=1;
  int imageWidth=320,imageHeight=240;
};
inline Status state;
inline Status snapshot() { return state; }
inline bool setEnabled(bool enabled) { state.enabled=enabled; state.batteryMode=false; return state.ready; }
}
namespace zumo_link {
struct Status {
  bool connected=true,ready=true,calibrating=false,edge=false,actionCapable=true;
  bool voiceCapable=true,rangeValid=true,lookActive=false,manualDriving=false,rampedLookCapable=true;
  bool wiggleCapable=true;
  int left=0,right=0,distanceMm=900;
  uint32_t controlGeneration=0,replyAgeMs=0,safetyEvents=0;
  char fault[24]="NONE",lookResult[24]="NONE";
};
inline Status state;
inline unsigned motions=0;
inline unsigned stops=0;
inline bool lastStopSmooth=false;
inline uint32_t turnAt=0;
inline Status snapshot() { return state; }
inline void stop(bool smooth=false) { lastStopSmooth=smooth;++stops;state.left=state.right=0;state.lookActive=false;++state.controlGeneration; }
inline bool safeDrive(int left,int right) { state.left=left;state.right=right;++motions;return true; }
inline bool look(int,uint32_t) { state.lookActive=true;turnAt=nowMs;std::strcpy(state.lookResult,"TURNING");++motions;return true; }
inline bool wiggle(uint32_t generation) { look(15,generation);std::strcpy(state.lookResult,"PREPARING");return true; }
inline bool face(const char*) { return true; }
}
