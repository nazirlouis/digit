#include "emotion_voice.h"
#include <algorithm>
#include <cstring>

namespace {
using reaction_catalog::Tone;
// Quarter-wave interpolation keeps synthesis allocation-free and avoids costly
// per-sample trigonometry. All note lengths/contours come from the authored data.
constexpr int16_t sineQuarter[] = {
  0,804,1608,2410,3212,4011,4808,5602,6393,7180,7962,8739,9512,10278,11039,11793,
  12539,13279,14010,14732,15446,16151,16846,17530,18204,18868,19519,20159,20787,21403,22005,22594,
  23170,23731,24279,24811,25329,25832,26319,26790,27245,27683,28105,28510,28898,29268,29621,29956,
  30273,30571,30852,31113,31356,31580,31785,31971,32137,32285,32412,32521,32609,32678,32728,32757,32767
};
int32_t sinePoint(unsigned index) {
  index &= 255;
  const unsigned quadrant = index >> 6, offset = index & 63;
  const int32_t value = sineQuarter[(quadrant & 1) ? 64 - offset : offset];
  return quadrant >= 2 ? -value : value;
}
float sine(uint32_t phase) {
  const unsigned index = phase >> 24;
  const float fraction = static_cast<float>((phase >> 8) & 65535) / 65536.f;
  return (sinePoint(index) + (sinePoint(index + 1) - sinePoint(index)) * fraction) / 32768.f;
}
float smooth(float value) { return value * value * (3.f - 2.f * value); }
constexpr float kCycle = 4294967296.f;
constexpr float kRadiansToPhase = kCycle / 6.283185307179586f;
uint32_t phaseAt(float cycles) {
  // Do not convert an unwrapped floating point phase to uint32_t.
  const float fraction = cycles - static_cast<uint32_t>(cycles);
  return static_cast<uint32_t>(fraction * kCycle);
}
uint32_t phaseOffset(float radians) {
  return static_cast<uint32_t>(static_cast<int32_t>(radians * kRadiansToPhase));
}
const reaction_catalog::Phrase& phraseFor(unsigned index, VoiceTiming timing) {
  return (timing == VoiceTiming::Wake ? reaction_catalog::wakePhrases :
          timing == VoiceTiming::Gesture ? reaction_catalog::gesturePhrases :
          reaction_catalog::expressionPhrases)[index];
}
}

bool isEmotionCue(AudioCue cue) { return cue >= AudioCue::Neutral && cue <= AudioCue::Love; }
AudioCue emotionCueForFace(const char* name) {
  if (name) for (unsigned i = 0; i < 13; ++i) {
    if (!strcmp(name, reaction_catalog::names[i]))
      return static_cast<AudioCue>(static_cast<unsigned>(AudioCue::Neutral) + i);
  }
  return AudioCue::None;
}

void EmotionVoice::start(AudioCue cue, uint32_t sampleRate, VoiceTiming timing) {
  phrase_ = nullptr; rate_ = sampleRate;
  frame_ = phase_ = modPhase_ = leadFrames_ = remainingFrames_ = elapsedMs_ = 0;
  note_ = 0;
  noteFrames_ = gapFrames_ = 0; noise_ = 24;
  if (!isEmotionCue(cue) || sampleRate < 8000 || sampleRate > 48000) return;
  const auto index = static_cast<unsigned>(cue) - static_cast<unsigned>(AudioCue::Neutral);
  phrase_ = &phraseFor(index, timing);
  leadFrames_ = sampleRate * emotionLeadMs(cue, timing) / 1000;
  remainingFrames_ = sampleRate * emotionDurationMs(cue, timing) / 1000;
  loadNote();
}

void EmotionVoice::loadNote() {
  const auto& syllable = phrase_->notes[note_];
  // Cumulative boundaries avoid rounding drift at sample rates other than 16 kHz.
  // Durations are never stretched to fill a face animation.
  const auto boundary = [&](uint32_t ms) { return uint32_t(uint64_t(rate_) * ms / 1000); };
  const uint32_t start = boundary(elapsedMs_);
  elapsedMs_ += syllable.ms;
  noteFrames_ = boundary(elapsedMs_) - start;
  elapsedMs_ += syllable.gapMs;
  gapFrames_ = boundary(elapsedMs_) - start - noteFrames_;
}

int32_t EmotionVoice::next() {
  if (!playing()) return 0;
  --remainingFrames_;
  if (leadFrames_) { --leadFrames_; return 0; }
  // Keep the timeline alive through the quiet settling/return, not the sound.
  if (note_ >= phrase_->count) return 0;
  const auto& syllable = phrase_->notes[note_];
  int32_t output = 0;
  if (frame_ < noteFrames_) {
    const float progress = static_cast<float>(frame_) / std::max<uint32_t>(1, noteFrames_ - 1);
    const float seconds = static_cast<float>(frame_) / rate_;
    const float position = progress * (syllable.points - 1);
    const unsigned section = std::min(static_cast<unsigned>(position), unsigned(syllable.points - 2));
    const float fraction = smooth(position - section);
    float frequency = syllable.hz[section] +
        (static_cast<float>(syllable.hz[section + 1]) - syllable.hz[section]) * fraction;
    frequency *= 1.f + syllable.warble * sine(phaseAt((17.f + 9.f * progress) * seconds));
    const float delta = frequency * (kCycle / rate_);
    phase_ += static_cast<uint32_t>(delta);
    modPhase_ += static_cast<uint32_t>(delta * (syllable.tone == Tone::Rasp ? .5f : 1.98f));
    float voice;
    if (syllable.tone == Tone::Whistle) {
      voice = sine(phase_) + .10f * sine(phase_ * 2U);
    } else if (syllable.tone == Tone::Chatter) {
      voice = .66f * sine(phase_ + phaseOffset(1.2f * sine(modPhase_))) + .15f * sine(phase_ * 3U);
      voice *= .72f + .28f * sine(phaseAt(42.f * seconds));
    } else {
      noise_ ^= noise_ << 13; noise_ ^= noise_ >> 17; noise_ ^= noise_ << 5;
      const float noise = static_cast<float>(noise_ >> 8) / 8388608.f - 1.f;
      voice = .54f * sine(phase_ + phaseOffset(1.8f * sine(modPhase_))) +
              .22f * sine(phase_ * 2U) + .07f * noise;
      voice *= .76f + .24f * sine(phaseAt(31.f * seconds));
    }
    const float attack = std::min(1.f, static_cast<float>(frame_) / (rate_ * .009f));
    const float release = std::min(1.f, static_cast<float>(noteFrames_ - 1 - frame_) / (rate_ * .025f));
    const float envelope = smooth(attack) * smooth(release);
    // Offline-authored attenuation keeps auditions and production at the same
    // relative loudness. Never boost or alter the user's master volume here.
    output = static_cast<int32_t>(voice * envelope * syllable.level * phrase_->gain * 0x08000000);
  }
  if (++frame_ >= noteFrames_ + gapFrames_) {
    frame_ = phase_ = modPhase_ = 0;
    if (++note_ < phrase_->count) loadNote();
  }
  return output;
}
