#include "audio_cues.h"
#include <cstring>
#define CUE(name) extern "C" const uint8_t cue_##name##_start[], cue_##name##_end[];
CUE(hello) CUE(following) CUE(lost) CUE(countdown) CUE(photo) CUE(clear) CUE(obstacle) CUE(person) CUE(explore)
namespace {
struct Wave { const uint8_t* begin; const uint8_t* end; };
#define WAVE(name) {cue_##name##_start, cue_##name##_end}
const Wave waves[] = {{nullptr,nullptr}, WAVE(hello), WAVE(following), WAVE(lost), WAVE(countdown),
                     WAVE(photo), WAVE(clear), WAVE(obstacle), WAVE(person), WAVE(explore)};
}
CueSamples cueSamples(AudioCue cue) {
  const unsigned index = static_cast<unsigned>(cue);
  if (!index || index >= sizeof(waves)/sizeof(waves[0])) return {nullptr,0};
  const auto& w = waves[index];
  for (const uint8_t* p = w.begin + 12; p + 8 <= w.end;) {
    uint32_t bytes; memcpy(&bytes, p + 4, 4);
    if (bytes > static_cast<size_t>(w.end - p - 8)) break;
    if (!memcmp(p, "data", 4)) return {reinterpret_cast<const int16_t*>(p + 8), bytes/2};
    p += 8 + bytes + (bytes & 1);
  }
  return {nullptr,0};
}
