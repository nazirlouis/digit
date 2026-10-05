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
  if (w.end - w.begin < 4) return {nullptr,0};
  uint32_t count; memcpy(&count, w.begin, 4);
  const size_t full = count / 64, remaining = count % 64;
  const size_t bytes = full * 36 + (remaining ? 4 + remaining / 2 : 0);
  if (bytes > static_cast<size_t>(w.end - w.begin - 4)) return {nullptr,0};
  return {w.begin + 4, count};
}
