#pragma once
#include "audio_cues.h"
#include "reaction_catalog.h"

AudioCue emotionCueForFace(const char* name);
bool isEmotionCue(AudioCue cue);

// Full reactions follow the face timeline; gesture chirps follow motor events.
enum class VoiceTiming : uint8_t { Expression, Gesture, Wake };
inline constexpr uint32_t emotionDurationMs(AudioCue cue, VoiceTiming timing = VoiceTiming::Expression) {
  if (cue < AudioCue::Neutral || cue > AudioCue::Love) return 0;
  const auto index = static_cast<unsigned>(cue)-static_cast<unsigned>(AudioCue::Neutral);
  return (timing == VoiceTiming::Wake ? reaction_catalog::wake :
          timing == VoiceTiming::Gesture ? reaction_catalog::gesture : reaction_catalog::expression)[index].voiceMs;
}
inline constexpr uint32_t emotionLeadMs(AudioCue cue, VoiceTiming timing = VoiceTiming::Expression) {
  if (cue < AudioCue::Neutral || cue > AudioCue::Love) return 0;
  const auto index = static_cast<unsigned>(cue)-static_cast<unsigned>(AudioCue::Neutral);
  return (timing == VoiceTiming::Wake ? reaction_catalog::wake :
          timing == VoiceTiming::Gesture ? reaction_catalog::gesture : reaction_catalog::expression)[index].leadMs;
}
inline constexpr uint32_t emotionFaceDurationMs(AudioCue cue, VoiceTiming timing = VoiceTiming::Expression) {
  // Persistent idle/sleeping eyes use their finite audio audition span here.
  // Callers that need persistence inspect the generated Timing::persistent flag.
  return emotionDurationMs(cue, timing);
}

// Small, allocation-free voice renderer. Only the audio task owns this state.
class EmotionVoice {
 public:
  void start(AudioCue cue, uint32_t sampleRate, VoiceTiming timing = VoiceTiming::Expression);
  int32_t next();
  bool playing() const { return remainingFrames_ != 0; }

 private:
  const reaction_catalog::Phrase* phrase_ = nullptr;
  uint32_t rate_ = 16000, frame_ = 0, phase_ = 0, modPhase_ = 0;
  uint32_t leadFrames_ = 0, remainingFrames_ = 0, elapsedMs_ = 0;
  uint32_t noteFrames_ = 0, gapFrames_ = 0, noise_ = 24;
  uint8_t note_ = 0;
  void loadNote();
};
