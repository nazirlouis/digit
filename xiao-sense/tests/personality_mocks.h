#pragma once
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include "personality_events.h"
#include "reaction_player.h"
#include "robot_actions.h"
#include "speech_commands.h"
#include "wake_word.h"
#include "zumo_link.h"
#include "audio_engine.h"

inline uint32_t nowMs=1000;
inline uint32_t millis() { return nowMs; }
namespace robot_actions {
inline Status state{Command::None,"Idle",false,1};
inline Status snapshot() { return state; }
}
namespace speech_commands {
inline Status state{};
inline Status snapshot() { return state; }
}
namespace wake_word {
inline Status state{};
inline Status snapshot() { return state; }
}
namespace zumo_link {
inline Status state{};
inline Status snapshot() { return state; }
}
namespace reaction_player {
struct Request { std::string face,label; AudioCue cue; Source source; };
inline std::vector<Request> requests;
inline Status state{0,State::Idle,0,0,false,"Idle"};
inline unsigned cancelled=0;
inline bool active() { return state.state==State::Playing || state.state==State::Waiting; }
inline Status snapshot() { return state; }
inline uint32_t play(const char* face,const char* label,AudioCue cue,Source source,uint32_t,VoiceTiming) {
  assert(!active());
  requests.push_back({face,label,cue,source});
  ++state.token; state.state=State::Playing;
  return state.token;
}
inline void cancel() { ++cancelled; state.state=State::Cancelled; }
}
