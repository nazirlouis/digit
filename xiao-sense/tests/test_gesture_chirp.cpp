#include "gesture_chirp.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>
using gesture_chirp::Event;
using gesture_chirp::Player;

std::vector<int16_t> render(uint8_t face, size_t block) {
  Player player; Event event{1,face}; std::vector<int16_t> result;
  int16_t samples[640];
  for (unsigned i=0;i<16000;++i) {
    const auto count=player.render(event,false,0,samples,block);
    if (!count) return result;
    result.insert(result.end(),samples,samples+count);
  }
  assert(false && "Chirp must end"); return {};
}
void wav(const char* path,const std::vector<int16_t>& samples) {
  std::ofstream file(path,std::ios::binary);
  auto word=[&](uint32_t value,unsigned bytes){for(unsigned i=0;i<bytes;++i)file.put(char(value>>(i*8)));};
  file.write("RIFF",4); word(36+static_cast<uint32_t>(samples.size())*2,4);
  file.write("WAVEfmt ",8); word(16,4); word(1,2); word(1,2); word(16000,4); word(32000,4); word(2,2); word(16,2);
  file.write("data",4); word(static_cast<uint32_t>(samples.size())*2,4);
  for (auto sample:samples) word(static_cast<uint16_t>(sample),2);
  assert(file.good());
}
int main(int argc,char** argv) {
  std::vector<int16_t> preview;
  std::vector<std::vector<int16_t>> voices;
  // Fingerprints of the approved original PCM previews, not this adapter.
  const uint64_t approved[] = {
    0xf3c8951c80301088ULL,0xe65de1e2cfac3c16ULL,0x8c7ed4a37708fd83ULL,
    0x6c16f43a1a0dc2d0ULL,0x5df1e1fe12af6c84ULL,0x019d2d7c4849398cULL,
    0xc130ecedb16ba002ULL,0xd3b33ca6bc22bcb9ULL,0x86b5c13ce38f2f8dULL,
    0x42a6363b981fa1e4ULL,0x1961883b8788a5eaULL,0x932d3d4f9ad73835ULL,
  };
  auto hash=[](const std::vector<int16_t>& pcm) {
    uint64_t result=14695981039346656037ULL;
    for(auto sample:pcm) result=(result^static_cast<uint16_t>(sample))*1099511628211ULL;
    return result;
  };
  assert(render(1,640).empty()); // Approved Neutral is a quiet idle expression.
  for(uint8_t face=2;face<=13;++face) {
    const auto pcm=render(face,640);
    assert(pcm==render(face,17)); // Timing independent of I2S block size.
    const auto cue=static_cast<AudioCue>(static_cast<unsigned>(AudioCue::Neutral)+face-1);
    assert(pcm.size()==emotionDurationMs(cue)*16 && pcm.size()<=2190*16);
    assert(hash(pcm)==approved[face-2]); // Every sample matches the approved sound.
    for (const auto& previous:voices) assert(pcm!=previous);
    voices.push_back(pcm);
    assert(pcm.front()==0 && std::abs(pcm.back())<=1);
    assert(std::any_of(pcm.begin(),pcm.end(),[](int16_t s){return std::abs(s)>1000;}));
    for(auto sample:pcm) assert(std::abs(sample)<=16000);
    preview.insert(preview.end(),pcm.begin(),pcm.end());
    preview.insert(preview.end(),11200,0);
  }
  const auto wake=render(gesture_chirp::kWakeExpression,640);
  assert(wake==render(gesture_chirp::kWakeExpression,17));
  assert(wake.size()==780*16 && hash(wake)==0xabec6d43c8723eafULL);
  preview.insert(preview.end(),wake.begin(),wake.end());
  Player p; int16_t samples[640];
  assert(p.render({1,2},false,0,samples,640)==640);
  assert(!p.render({1,2},true,0,samples,640)); // Speech takes over immediately.
  assert(!p.render({1,2},false,0,samples,640)); // It must not restart later.
  assert(!p.render({2,11},true,0,samples,640));
  assert(!p.render({2,11},false,0,samples,640));
  assert(p.render({3,13},false,0,samples,640));
  assert(!p.render({3,13},false,1,samples,640)); // Global audio cancellation.
  assert(!p.render({3,13},false,1,samples,640));
  assert(p.render({4,6},false,1,samples,640));
  assert(!p.render({5,0},false,1,samples,640)); // Gesture END / safety / replacement.
  assert(!p.render({6,255},false,1,samples,640));
  assert(p.render({7,3},false,1,samples,640));
  assert(!p.render({8,1},false,1,samples,640)); // Explicit neutral stays quiet too.
  assert(!p.render({9,0},false,1,samples,640)); // Stop/automatic neutral stays quiet.
  assert(p.render({10,gesture_chirp::kWakeExpression},false,1,samples,640));
  assert(!p.render({10,gesture_chirp::kWakeExpression},false,2,samples,640));
  assert(!p.render({10,gesture_chirp::kWakeExpression},false,2,samples,640));
  if (argc>1) wav(argv[1],preview);
  DanceMusic melody;melody.start();std::vector<int16_t> music;
  while(melody.playing()) music.push_back(melody.next());
  assert(music.size()==76800&&std::any_of(music.begin(),music.end(),[](int16_t s){return s!=0;}));
  gesture_chirp::Player dancePlayer;
  assert(dancePlayer.render({1,gesture_chirp::kDanceExpression},false,0,samples,640)==640);
  assert(!dancePlayer.render({2,0},false,0,samples,640));
  assert(!dancePlayer.render({2,0},false,0,samples,640));
  assert(dancePlayer.render({3,gesture_chirp::kDanceExpression},false,0,samples,640)==640);
  assert(!dancePlayer.render({3,gesture_chirp::kDanceExpression},false,1,samples,640));
  assert(!dancePlayer.render({3,gesture_chirp::kDanceExpression},false,1,samples,640));
  if(argc>2) wav(argv[2],music);
  puts("PASS finite original 4.8 second dance melody, Stop/cancellation and no replay");
  puts("PASS approved original emotion PCM, silent Neutral, exact 780 ms wake reply, chunk invariance, suppression/Stop/replacement with no replay");
}
