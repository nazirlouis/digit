#pragma once
#include "audio_cues.h"

AudioCue emotionCueForFace(const char* name);
bool isEmotionCue(AudioCue cue);

// Full reactions follow the face timeline; gesture chirps follow motor events.
enum class VoiceTiming : uint8_t { Expression, Gesture, Wake };
inline uint32_t emotionDurationMs(AudioCue cue, VoiceTiming timing = VoiceTiming::Expression) {
  if (cue < AudioCue::Neutral || cue > AudioCue::Love) return 0;
  constexpr uint16_t expression[] = {240,2920,4300,3110,3570,3420,2010,4050,3880,5550,3160,4700,3720};
  constexpr uint16_t gesture[] = {180,465,680,445,260,600,330,1050,875,1090,855,1320,900};
  const auto index = static_cast<unsigned>(cue)-static_cast<unsigned>(AudioCue::Neutral);
  return timing == VoiceTiming::Wake ? 1800 : timing == VoiceTiming::Gesture ? gesture[index] : expression[index];
}

// Small, allocation-free voice renderer. Only the audio task owns this state.
class EmotionVoice {
 public:
  void start(AudioCue cue, uint32_t sampleRate, VoiceTiming timing = VoiceTiming::Expression);
  int32_t next();
  bool playing() const { return count_ != 0; }

 private:
  AudioCue cue_ = AudioCue::None;
  VoiceTiming timing_ = VoiceTiming::Expression;
  uint32_t rate_ = 16000, frame_ = 0, phase_ = 0, vibratoPhase_ = 0;
  uint32_t leadFrames_ = 0;
  uint32_t bodyFrames_ = 0, weightMs_ = 0, elapsedWeightMs_ = 0;
  uint32_t noteFrames_ = 0, gapFrames_ = 0, pulsePhase_ = 0;
  uint8_t note_ = 0, count_ = 0;
  void loadNote();
};
