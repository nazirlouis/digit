#pragma once
#include <cstddef>
#include <cstdint>
enum class AudioCue : uint8_t { None, Hello, Following, Lost, Countdown, Photo, Clear, Obstacle, Person, Explore, Dance };
struct CueSamples { const uint8_t* data; size_t count; };
CueSamples cueSamples(AudioCue cue);
