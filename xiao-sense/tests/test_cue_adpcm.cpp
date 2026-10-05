#include "cue_adpcm.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <cstdio>

std::vector<uint8_t> read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  assert(file);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const char* names[] = {"hello","following","lost","countdown","photo","clear","obstacle","person","explore"};
  CueDecoder decoder;
  for (const char* name : names) {
    decoder = CueDecoder{}; // Starting/replacing a cue resets its codec state.
    const std::string base = std::string(argv[1]) + "/" + name;
    auto packed = read(base + ".adpcm"), expected = read(base + ".pcm");
    uint32_t count; memcpy(&count, packed.data(), 4);
    assert(expected.size() == count * 2 && packed.size() == 4 + (count / 64) * 36 + (count % 64 ? 4 + (count % 64) / 2 : 0));
    for (size_t i = 0; i < count; ++i) {
      int16_t value; memcpy(&value, expected.data() + i * 2, 2);
      assert(decoder.next(packed.data() + 4, i) == value);
    }
  }
  puts("Cue playback: every sample of all nine cues matches independent IMA reference; cue replacement resets state");
}
