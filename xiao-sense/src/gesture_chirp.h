#pragma once
#include "emotion_voice.h"
#include "dance_music.h"
#include <cstddef>
#include <cstdint>

// Shared face order: NEUTRAL..LOVE (1..13); 14 is the local wake reply.
// Uses the approved authored droid phrases, without allocation or speech.
namespace gesture_chirp {
struct Event { uint32_t generation = 0; uint8_t expression = 0; };
constexpr uint32_t kSampleRate = 16000;
constexpr uint8_t kWakeExpression = 14;
constexpr uint8_t kDanceExpression = 15;

class Player {
 public:
  // A cancelled or speech-suppressed event is consumed, never queued to replay.
  size_t render(Event event, bool speech, uint32_t cancelGeneration,
                int16_t* output, size_t count) {
    if (cancelGeneration != cancelGeneration_) {
      cancelGeneration_ = cancelGeneration;
      seen_ = event.generation;
      voice_.start(AudioCue::None, kSampleRate);
      music_.stop();
      return 0;
    }
    if (event.generation != seen_) {
      seen_ = event.generation;
      if(event.expression==kDanceExpression) music_.start(); else music_.stop();
      const bool wake = event.expression == kWakeExpression;
      // The approved Neutral preview is silent, including explicit idle scenes.
      const bool emotion = event.expression >= 2 && event.expression <= 13;
      const auto cue = wake ? AudioCue::Happy : emotion ?
          static_cast<AudioCue>(static_cast<unsigned>(AudioCue::Neutral) + event.expression - 1) : AudioCue::None;
      voice_.start(cue, kSampleRate, wake ? VoiceTiming::Wake : VoiceTiming::Expression);
    }
    if (speech || !event.expression) { voice_.start(AudioCue::None, kSampleRate); music_.stop(); }
    if(music_.playing()&&output) {
      size_t written=0;
      while(written<count&&music_.playing()) output[written++]=music_.next();
      return written;
    }
    if (!voice_.playing() || !output) return 0;
    size_t written = 0;
    while (written < count && voice_.playing())
      output[written++] = static_cast<int16_t>(voice_.next() / 4096);
    return written;
  }
 private:
  uint32_t seen_ = 0, cancelGeneration_ = 0;
  EmotionVoice voice_;
  DanceMusic music_;
};
}  // namespace gesture_chirp
