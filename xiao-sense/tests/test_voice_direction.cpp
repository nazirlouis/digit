#include "voice_direction.cpp"
#include <cassert>
#include <iostream>
#include <limits>

void reboot(){state={};hasSample=false;front=0;orientation=1;voice_direction::begin();}
void speech(float degrees){state.degrees=degrees;state.available=state.stable=hasSample=true;sampledAt=nowMs;}
int main(){
  reboot();assert(state.enabled&&!state.calibrated);
  int angle;uint32_t generation;
  speech(350);assert(!voice_direction::target(angle,generation));
  assert(voice_direction::configure("front"));assert(state.frontSet&&!state.calibrated&&state.enabled);
  speech(355);assert(!voice_direction::configure("left"));
  speech(80);assert(voice_direction::configure("left"));assert(state.calibrated&&state.enabled);
  reboot();assert(state.enabled&&state.calibrated&&state.frontSet&&front==350&&orientation==1);
  speech(80);assert(!voice_direction::target(angle,generation)); // Floor is still locked.
  zumo_link::state.ready=true;
  assert(voice_direction::target(angle,generation)&&angle==90&&voice_direction::accepts(generation));
  const char* reason=nullptr;bool retryable=false;
  speech(170);assert(voice_direction::target(angle,generation,&reason,&retryable)&&std::abs(angle)==180);
  nowMs+=301;
  assert(!voice_direction::target(angle,generation,&reason,&retryable)&&retryable);
  assert(!strcmp(reason,"Voice direction too old"));
  speech(80);state.stable=false;
  assert(!voice_direction::target(angle,generation,&reason,&retryable)&&retryable);
  assert(!strcmp(reason,"Voice direction not stable"));
  state.available=false;
  assert(!voice_direction::target(angle,generation,&reason,&retryable)&&retryable);
  assert(!strcmp(reason,"Microphone direction unavailable"));
  zumo_link::state.edge=true;
  assert(!voice_direction::target(angle,generation,&reason,&retryable)&&!retryable);
  assert(!strcmp(reason,"Zumo edge stop"));
  zumo_link::state.edge=false;speech(80);
  audioEngine.cuePlaying=true;assert(!voice_direction::target(angle,generation));audioEngine.cuePlaying=false;
  zumo_link::state.rampedLookCapable=false;assert(!voice_direction::target(angle,generation));zumo_link::state.rampedLookCapable=true;
  zumo_link::state.manualDriving=true;assert(!voice_direction::target(angle,generation));zumo_link::state.manualDriving=false;
  nowMs+=301;assert(!voice_direction::target(angle,generation));speech(80);
  assert(voice_direction::configure("off"));assert(!voice_direction::accepts(generation));
  reboot();assert(!state.enabled&&state.calibrated);speech(80);assert(!voice_direction::target(angle,generation));
  assert(voice_direction::configure("on"));assert(voice_direction::target(angle,generation));
  assert(voice_direction::configure("reset"));reboot();assert(state.enabled&&!state.calibrated&&!state.frontSet);
  speech(20);assert(voice_direction::configure("front"));speech(290);assert(voice_direction::configure("left"));
  reboot();assert(orientation==-1);speech(290);assert(voice_direction::target(angle,generation)&&angle==90);
  savedNvs={1,2,3};reboot();assert(state.enabled&&!state.calibrated);
  VoiceSettings bad{};bad.front=std::numeric_limits<float>::quiet_NaN();
  auto bytes=reinterpret_cast<const uint8_t*>(&bad);savedNvs.assign(bytes,bytes+sizeof(bad));
  reboot();assert(state.enabled&&!state.calibrated);
  std::cout<<"PASS default-on preference, saved front/left/sign/off choice, corrupt settings, floor/firmware/audio/manual/freshness gates\n";
}
