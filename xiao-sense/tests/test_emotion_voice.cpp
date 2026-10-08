#include "emotion_voice.h"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <vector>
#include <fstream>
#include <string>

void writeWav(const std::string& name, const std::vector<int32_t>& samples) {
  std::ofstream out(name, std::ios::binary);
  auto put=[&](uint32_t value,unsigned bytes){for(unsigned i=0;i<bytes;++i)out.put(char(value>>(i*8)));};
  out.write("RIFF",4);put(36+static_cast<uint32_t>(samples.size())*2,4);out.write("WAVEfmt ",8);
  put(16,4);put(1,2);put(1,2);put(16000,4);put(32000,4);put(2,2);put(16,2);
  out.write("data",4);put(static_cast<uint32_t>(samples.size())*2,4);
  for(auto sample:samples)put(static_cast<uint16_t>(static_cast<int16_t>(sample/4096)),2);
  assert(out.good());
}

int main() {
  const char* names[] = {"NEUTRAL", "HAPPY", "SAD", "MAD", "SERIOUS", "CURIOUS", "SURPRISED",
                         "SLEEPING", "CONFUSED", "SLEEPY", "EXCITED", "CRYING", "LOVE"};
  std::set<uint64_t> fingerprints;
  for (const auto* name : names) {
    const auto cue = emotionCueForFace(name);
    assert(isEmotionCue(cue));
    EmotionVoice voice;
    voice.start(cue, 16000);
    std::vector<int32_t> samples;
    uint64_t hash = 14695981039346656037ULL;
    int64_t peak = 0, jump = 0, previous = 0;
    while (voice.playing()) {
      const int32_t value = voice.next();
      samples.push_back(value);
      peak = std::max(peak, std::abs(static_cast<int64_t>(value)));
      jump = std::max(jump, std::abs(value - previous)); previous = value;
      hash = (hash ^ static_cast<uint32_t>(value)) * 1099511628211ULL;
      assert(samples.size() <= 96000); // Every reaction is finite, including sleep.
    }
    assert(peak > 1000000 && peak <= 0x08000000); // Audible, with mixer headroom.
    assert(jump < 0x04000000); // No large discontinuities at syllable boundaries.
    assert(samples.front() == 0 && samples.back() == 0 && voice.next() == 0);
    for (unsigned i = 0; i < 1440; ++i) assert(samples[i] == 0); // Face lead-in.
    assert(samples.size() == emotionDurationMs(cue)*16);
    assert(fingerprints.insert(hash).second); // Each emotion has a distinct phrase.
    writeWav(std::string("expression_")+name+".wav",samples);
    voice.start(cue, 16000);
    for (auto sample : samples) assert(voice.next() == sample); // Repeatable audition.
    voice.start(AudioCue::None, 16000);
    assert(!voice.playing() && voice.next() == 0);
    std::cout << name << ": " << samples.size() / 16 << " ms\n";
    for(auto timing:{VoiceTiming::Gesture,VoiceTiming::Wake}) {
      voice.start(cue,16000,timing);samples.clear();
      while(voice.playing())samples.push_back(voice.next());
      assert(samples.size()==emotionDurationMs(cue,timing)*16);
      assert(samples.front()==0 && samples.back()==0);
      if(timing==VoiceTiming::Wake && cue==AudioCue::Happy)writeWav("wake.wav",samples);
    }
  }
  assert(emotionCueForFace(nullptr) == AudioCue::None);
  assert(emotionCueForFace("DIZZY") == AudioCue::None);
  EmotionVoice voice;
  voice.start(AudioCue::Happy, 0); assert(!voice.playing() && voice.next() == 0);
  voice.start(AudioCue::Hello, 16000); assert(!voice.playing());
  voice.start(AudioCue::Happy, 96000); assert(!voice.playing());
  for(auto rate:{8000U,48000U}) {
    voice.start(AudioCue::Sleepy,rate);unsigned count=0;
    while(voice.playing()){voice.next();++count;assert(count<=rate*6);}
    assert(count==rate*emotionDurationMs(AudioCue::Sleepy)/1000);
  }
  voice.start(AudioCue::Love, 16000);
  for (int i = 0; i < 5000; ++i) voice.next();
  voice.start(AudioCue::Neutral, 16000); // Replacement resets all oscillator state.
  EmotionVoice fresh; fresh.start(AudioCue::Neutral, 16000);
  while (fresh.playing()) assert(voice.next() == fresh.next());
  assert(!voice.playing());
  std::cout << "PASS: thirteen bounded, distinct, finite voices; fades, repeat, replacement, invalid input\n";
}
