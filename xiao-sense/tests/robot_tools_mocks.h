#pragma once
#include <cassert>
#include <cstdint>
#include <algorithm>
using SemaphoreHandle_t=void*;
constexpr int portMAX_DELAY=0,pdTRUE=1;
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(){return reinterpret_cast<void*>(1);}
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t,int){return 1;}
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t){return 1;}
#define configASSERT(x) assert(x)
using std::min; using std::max;
inline uint32_t nowMs=1000;
inline uint32_t millis(){return nowMs;}
#include "robot_actions.h"
#include "action_console.h"
namespace action_console {
inline void cancelSession(uint32_t,const char*) {}
inline void updateId(uint32_t,State,const char*) {}
inline uint32_t local(const char*,const char*,const char*,State,const char*,const char*,robot_actions::Command) {return 0;}
}
#include "real_zumo_link.h"
namespace mock { inline zumo_link::Status z{}; inline unsigned stops=0,travels=0,turns=0,supervisions=0,outputCancels=0,calibrations=0; inline int travel=0,turn=0; inline bool armReady=false,calibrationAllowed=true; inline const char* expression=""; }
namespace robot_session {
struct Activity {bool liveActive=true,liveProbe=false,dryRun=false;}; inline Activity state;
inline Activity activity(){return state;} inline bool liveActive(){return state.liveActive;} inline void cancel(){state.liveActive=false;}
}
namespace wake_word {struct Status{bool reacting=false;};inline Status state;inline Status snapshot(){return state;} inline void cancel(){state.reacting=false;}}
namespace reaction_player {inline void cancel(){} inline void poll(){}}
namespace personality_events {inline void cancel(){} inline void interaction(){} inline void safetyStopped(){}}
namespace local_stop {inline void cancel(){}}
namespace vision_service {inline bool setEnabled(bool){return true;}}
inline struct {void cancelOutput(){++mock::outputCancels;}} audioEngine;
namespace zumo_link {
inline Status snapshot(){return mock::z;}
inline void stop(bool){++mock::stops;mock::z.motionActive=false;if(mock::z.calibrating){mock::z.calibrating=false;strcpy(mock::z.calibrationResult,"CANCELLED");}}
inline bool calibrateSurface(){if(!mock::calibrationAllowed)return false;++mock::calibrations;++mock::z.calibrationGeneration;mock::z.calibrating=true;mock::z.ready=false;strcpy(mock::z.calibrationResult,"QUEUED");return true;}
inline void cancelMotion(bool smooth){stop(smooth);}
inline void cancelScene(){}
inline bool face(const char*){return true;}
inline bool scene(const char* face,const char*,uint32_t){mock::expression=face;return true;}
inline bool isFaceName(const char* face){return !strcmp(face,"HAPPY")||!strcmp(face,"NEUTRAL");}
inline void superviseMotion(){++mock::supervisions;}
inline bool armMotion(bool){return mock::armReady;}
inline uint32_t edgeBackJob(int mm){++mock::travels;mock::travel=-mm;return 1;}
inline uint32_t travelJob(int mm,uint32_t,bool){++mock::travels;mock::travel=mm;return 1;}
inline uint32_t pushJob(int mm){++mock::travels;mock::travel=mm;return 1;}
inline uint32_t turnJob(int degrees,uint32_t){++mock::turns;mock::turn=degrees;return 1;}
inline uint32_t danceJob(){return 1;}
inline bool ackMotion(uint32_t){return true;}
}
