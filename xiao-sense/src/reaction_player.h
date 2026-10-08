#pragma once
#include "emotion_voice.h"
#include <cstdint>

// Calls are serialized by robot_actions::Guard (including dashboard requests).
// This coordinator never issues motor commands or treats decoration as safety.
namespace reaction_player {
enum class Source : uint8_t { Action, Preview, Ambient, Wake };
enum class State : uint8_t { Idle, Waiting, Playing, Complete, Skipped, Cancelled };
struct Status {
  uint32_t token;
  State state;
  uint32_t visibleAt;
  uint32_t durationMs;
  bool visible;
  const char* reason;
};
uint32_t play(const char* face, const char* label, AudioCue cue,
              Source source = Source::Action, uint32_t cooldownMs = 0,
              VoiceTiming timing = VoiceTiming::Expression);
void poll();
void cancel();
bool active();
Status snapshot();
bool finished(uint32_t token);
}
