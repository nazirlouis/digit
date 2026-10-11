#include "speech_pipeline.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace speech_pipeline;

int main() {
  int16_t samples[512]{};
  Assembler assembler(samples, 512);
  Frame frame{};
  frame.stream = 1; frame.session = 2; frame.count = 160;
  std::vector<Tag> tags;
  unsigned absolute = 0, chunks = 0;
  // 10-ms I2S reads cross chunk boundaries without loss.
  for (unsigned f = 0; f < 100; ++f) {
    frame.capturedAt = f * 10;
    for (unsigned i = 0; i < frame.count; ++i) frame.samples[i] = static_cast<int16_t>(absolute++);
    for (unsigned i = 0; i < frame.count; ++i) {
      Tag tag{};
      if (!assembler.append(frame, i, tag)) continue;
      assert(tag.session == 2);
      assert(tag.capturedAt == (chunks * 512 / 160) * 10);
      for (unsigned s = 0; s < 512; ++s) assert(samples[s] == static_cast<int16_t>(chunks * 512 + s));
      ++chunks; tags.push_back(tag);
    }
  }
  assert(chunks == 31);

  // A Stop/new wake halfway through a chunk cannot relabel old audio.
  frame.session = 3;
  frame.capturedAt = 1000;
  bool mixed = false;
  for (unsigned i = 0; i < 512; ++i) {
    Tag tag{};
    if (assembler.append(frame, i % 160, tag)) { assert(tag.session == 0); mixed = true; }
  }
  assert(mixed);
  bool clean = false;
  for (unsigned i = 0; i < 512; ++i) {
    Tag tag{};
    if (assembler.append(frame, i % 160, tag)) { assert(tag.session == 3); clean = true; }
  }
  assert(clean);

  // A dropped read discards partial input instead of joining across the gap.
  ++frame.stream;
  frame.session = 4; frame.capturedAt = 2000;
  for (unsigned i = 0; i < 511; ++i) {
    Tag tag{}; assert(!assembler.append(frame, i % 160, tag));
  }
  Tag afterGap{};
  assert(assembler.append(frame, 0, afterGap));
  assert(afterGap.session == 4 && afterGap.capturedAt == 2000);

  assert(current(afterGap, 4, true, 2500));
  assert(!current(afterGap, 4, false, 2500)); // Stop during inference.
  assert(!current(afterGap, 5, true, 2500));  // A newer wake during inference.
  assert(!current(afterGap, 4, true, 3001));  // Stalled pipeline.
  assert(!current({0, 2000}, 4, true, 2500)); // Idle/chime audio.
  assert(current({4, UINT32_MAX - 100}, 4, true, 100)); // Timer rollover.

  assert(mono(INT32_MAX) == INT16_MAX);
  assert(mono(INT32_MIN) == INT16_MIN);
  assert(mono(0x10000000) == 4096); // No artificial x4 gain/clipping.
  assert(mono(-0x10000000) == -4096);
  puts("Speech pipeline: frame continuity, wake/Stop boundaries, gaps, stale audio and PCM format passed");
}
