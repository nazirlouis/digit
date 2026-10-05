#pragma once
#include <cstddef>
#include <cstdint>
enum class AudioCue : uint8_t {
  None, Hello, Following, Lost, Countdown, Photo, Clear, Obstacle, Person, Explore, Dance,
  Neutral, Happy, Sad, Mad, Serious, Curious, Surprised, Sleeping, Confused,
  Sleepy, Excited, Crying, Love,
};
struct CueSamples { const uint8_t* data; size_t count; };
CueSamples cueSamples(AudioCue cue);
