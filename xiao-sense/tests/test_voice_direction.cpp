#include "voice_direction.cpp"
#include <cassert>
#include <iostream>
#include <limits>

void reboot(){state={};hasSample=false;voice_direction::begin();}
void speech(float degrees){state.degrees=degrees;state.available=state.stable=hasSample=true;sampledAt=nowMs;}
int main(){
  reboot();assert(state.enabled&&state.calibrated&&state.frontSet);
  int angle;uint32_t generation;
  speech(270);assert(!voice_direction::target(angle,generation)); // Floor is still locked.
  assert(!voice_direction::configure("front")&&!voice_direction::configure("left"));
  zumo_link::state.ready=true;
  const float raw[]={270,0,360,180,90,359,1,269,271};
  const int expected[]={0,90,90,-90,-180,89,91,-1,1};
  for(unsigned i=0;i<sizeof(raw)/sizeof(raw[0]);++i){
    speech(raw[i]);assert(voice_direction::target(angle,generation)&&angle==expected[i]);
  }
  assert(voice_direction::accepts(generation));
  const char* reason=nullptr;bool retryable=false;
  speech(90);assert(voice_direction::target(angle,generation,&reason,&retryable)&&std::abs(angle)==180);
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
  assert(voice_direction::configure("reset"));reboot();assert(state.enabled&&state.calibrated&&state.frontSet);
  // Existing custom calibration must not override the fixed mounting, while
  // the user's disabled preference survives the firmware upgrade.
  VoiceSettings legacy{};legacy.front=20;legacy.orientation=-1;legacy.enabled=0;
  auto legacyBytes=reinterpret_cast<const uint8_t*>(&legacy);
  savedNvs.assign(legacyBytes,legacyBytes+sizeof(legacy));reboot();
  assert(!state.enabled&&state.calibrated&&front==270&&orientation==1);
  assert(voice_direction::configure("reset"));reboot();assert(!state.enabled&&state.calibrated);
  assert(voice_direction::configure("on"));speech(0);
  assert(voice_direction::target(angle,generation)&&angle==90);
  savedNvs={1,2,3};reboot();assert(state.enabled&&state.calibrated);
  VoiceSettings bad{};bad.front=std::numeric_limits<float>::quiet_NaN();
  auto bytes=reinterpret_cast<const uint8_t*>(&bad);savedNvs.assign(bytes,bytes+sizeof(bad));
  reboot();assert(state.enabled&&state.calibrated);
  std::cout<<"PASS fixed 270 front and 0/360 left, wraparound, legacy migration, saved on/off, corrupt settings and movement gates\n";
}
