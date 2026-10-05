#pragma once
#include "audio_cues.h"

AudioCue emotionCueForFace(const char* name);
bool isEmotionCue(AudioCue cue);

// Small, allocation-free voice renderer. Only the audio task owns this state.
class EmotionVoice {
 public:
  void start(AudioCue cue, uint32_t sampleRate);
  int32_t next();
  bool playing() const { return count_ != 0; }

 private:
  AudioCue cue_ = AudioCue::None;
  uint32_t rate_ = 16000, frame_ = 0, phase_ = 0, vibratoPhase_ = 0;
  uint32_t leadFrames_ = 0;
  uint8_t note_ = 0, count_ = 0;
};
