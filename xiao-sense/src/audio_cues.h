#pragma once
#include <cstdint>
// Retired clip slots keep current procedural sound IDs stable.
enum class AudioCue : uint8_t {
  None, Hello, Following, Lost, Countdown, Photo, Clear, Obstacle, Person, Explore, Dance,
  Neutral, Happy, Sad, Mad, Serious, Curious, Surprised, Sleeping, Confused,
  Sleepy, Excited, Crying, Love,
};
