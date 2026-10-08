#pragma once
#include <cstdint>
enum class AudioState : uint8_t { Unavailable, Ready, Recording, Playing, Error };
struct AudioStatus {
  AudioState state=AudioState::Ready;
  bool available=true, cuePlaying=false, wakeChimePlaying=false;
};
struct AudioEngine {
  AudioStatus state;
  AudioStatus snapshot() { return state; }
};
inline AudioEngine audioEngine;
