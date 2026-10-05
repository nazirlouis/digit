#include "emotion_voice.h"
#include <algorithm>
#include <cstring>

namespace {
struct Syllable {
  uint16_t fromHz, toHz, ms, gapMs;
  float level, vibratoHz, wobbleHz, brightness;
};
struct Phrase { const char* name; uint8_t count; Syllable notes[5]; };
// A common rounded voice, shaped into thirteen short, repeatable utterances.
// Level is relative to the existing speaker test; the dashboard controls volume.
const Phrase phrases[] = {
  {"NEUTRAL", 1, {{520, 570, 180, 0, .55f, 3, 5, .12f}}},
  {"HAPPY", 2, {{510, 720, 170, 65, .78f, 7, 6, .20f}, {680, 960, 230, 0, .85f, 10, 7, .20f}}},
  {"SAD", 1, {{570, 290, 680, 0, .62f, 9, 4, .08f}}},
  {"MAD", 2, {{290, 240, 150, 95, .78f, 20, 24, .45f}, {310, 230, 200, 0, .80f, 23, 22, .45f}}},
  {"SERIOUS", 1, {{460, 440, 260, 0, .78f, 0, 0, .22f}}},
  {"CURIOUS", 2, {{470, 530, 120, 100, .55f, 3, 5, .12f}, {540, 1050, 380, 0, .78f, 8, 5, .16f}}},
  {"SURPRISED", 2, {{650, 1350, 100, 100, .90f, 0, 0, .20f}, {1040, 800, 130, 0, .60f, 4, 6, .12f}}},
  {"SLEEPING", 1, {{360, 220, 1050, 0, .38f, 9, 2, .05f}}},
  {"CONFUSED", 3, {{540, 800, 210, 95, .65f, 15, 7, .16f}, {700, 410, 240, 110, .68f, 18, 6, .16f}, {470, 660, 220, 0, .62f, 15, 7, .16f}}},
  {"SLEEPY", 2, {{390, 620, 370, 30, .48f, 7, 3, .08f}, {620, 250, 690, 0, .46f, 10, 3, .06f}}},
  {"EXCITED", 5, {{550, 700, 95, 35, .73f, 12, 9, .23f}, {650, 850, 95, 30, .78f, 15, 10, .23f}, {790, 1050, 95, 30, .80f, 18, 11, .23f}, {920, 1250, 120, 35, .85f, 20, 11, .23f}, {1080, 1350, 220, 0, .82f, 24, 12, .23f}}},
  {"CRYING", 3, {{680, 450, 280, 120, .62f, 40, 8, .10f}, {620, 360, 340, 130, .60f, 45, 7, .10f}, {530, 300, 450, 0, .52f, 35, 6, .08f}}},
  {"LOVE", 2, {{420, 640, 330, 100, .60f, 12, 5, .10f}, {600, 510, 470, 0, .58f, 16, 5, .08f}}},
};
constexpr unsigned kCount = sizeof(phrases) / sizeof(phrases[0]);
static_assert(static_cast<unsigned>(AudioCue::Love) - static_cast<unsigned>(AudioCue::Neutral) + 1 == kCount, "Every emotion needs a phrase");

// Quarter-wave lookup with interpolation avoids per-sample trigonometry on ESP32.
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
}

bool isEmotionCue(AudioCue cue) { return cue >= AudioCue::Neutral && cue <= AudioCue::Love; }
AudioCue emotionCueForFace(const char* name) {
  if (name) for (unsigned i = 0; i < kCount; ++i) {
    if (!strcmp(name, phrases[i].name)) return static_cast<AudioCue>(static_cast<unsigned>(AudioCue::Neutral) + i);
  }
  return AudioCue::None;
}

void EmotionVoice::start(AudioCue cue, uint32_t sampleRate) {
  cue_ = cue; rate_ = sampleRate;
  frame_ = phase_ = vibratoPhase_ = note_ = count_ = 0;
  leadFrames_ = sampleRate * 90 / 1000; // Give the UART face time to begin reacting.
  if (isEmotionCue(cue) && sampleRate >= 8000)
    count_ = phrases[static_cast<unsigned>(cue) - static_cast<unsigned>(AudioCue::Neutral)].count;
}

int32_t EmotionVoice::next() {
  if (!playing()) return 0;
  if (leadFrames_) { --leadFrames_; return 0; }
  const auto& syllable = phrases[static_cast<unsigned>(cue_) - static_cast<unsigned>(AudioCue::Neutral)].notes[note_];
  const uint32_t total = rate_ * syllable.ms / 1000, gap = rate_ * syllable.gapMs / 1000;
  int32_t output = 0;
  if (frame_ < total) {
    const float progress = static_cast<float>(frame_) / (total - 1);
    const float frequency = syllable.fromHz + (syllable.toHz - static_cast<float>(syllable.fromHz)) * smooth(progress)
                            + syllable.vibratoHz * sine(vibratoPhase_);
    phase_ += static_cast<uint32_t>(frequency * (4294967296.f / rate_));
    vibratoPhase_ += static_cast<uint32_t>(syllable.wobbleHz * (4294967296.f / rate_));
    const float attack = std::min(1.f, static_cast<float>(frame_) / (rate_ * .018f));
    const float release = std::min(1.f, static_cast<float>(total - 1 - frame_) / (rate_ * .065f));
    const float envelope = smooth(attack) * smooth(release);
    const float voice = (sine(phase_) + syllable.brightness * sine(phase_ * 2U) + .04f * sine(phase_ * 3U))
                        / (1.04f + syllable.brightness);
    output = static_cast<int32_t>(voice * envelope * syllable.level * 0x08000000);
  }
  if (++frame_ >= total + gap) {
    frame_ = phase_ = vibratoPhase_ = 0;
    if (++note_ == count_) count_ = 0;
  }
  return output;
}
