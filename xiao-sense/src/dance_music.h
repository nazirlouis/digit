#pragma once
#include <cstdint>

// Original 4.8-second electronic melody. No file, allocation or speech engine.
class DanceMusic {
 public:
  static constexpr uint32_t kDurationMs = 4800;
  void start() { frame_ = phase_ = 0; playing_ = true; }
  void stop() { playing_ = false; }
  bool playing() const { return playing_; }
  int16_t next() {
    if (!playing_) return 0;
    constexpr uint16_t notes[] = {523,659,784,659,523,587,659,523,
        440,523,659,523,392,494,587,494,659,784,1047,784,659,587,523,523};
    constexpr uint32_t noteFrames=3200, attack=160, release=640;
    const uint32_t note=frame_/noteFrames, local=frame_%noteFrames;
    phase_ += static_cast<uint32_t>((uint64_t(notes[note])<<32)/16000);
    const int32_t ramp=static_cast<int32_t>(phase_>>16);
    const int32_t triangle=(ramp<32768 ? ramp : 65535-ramp)*2-32767;
    uint32_t gain=4096;
    if(local<attack) gain=gain*local/attack;
    else if(local>=noteFrames-release) gain=gain*(noteFrames-local)/release;
    const int16_t sample=static_cast<int16_t>(triangle*static_cast<int32_t>(gain)/32768);
    if(++frame_==24*noteFrames) playing_=false;
    return sample;
  }
 private:
  uint32_t frame_=0,phase_=0;
  bool playing_=false;
};
