#include "personality_mocks.h"
#include "personality_events.cpp"
#include <iostream>

void refresh() { zumo_link::state.statusTimestampMs=nowMs; zumo_link::state.replyAgeMs=0; }
void tick(uint32_t delta=0) { nowMs+=delta;refresh();personality_events::poll(); }
void complete() { reaction_player::state.state=reaction_player::State::Complete; }
void beginSpeech() { speech_commands::state.listening=true;tick(); }
void endSpeech(const char* result="No command recognized") {
  speech_commands::state.listening=false;
  std::strcpy(speech_commands::state.result,result); tick();
}
void missed() { beginSpeech();endSpeech(); }
void expect(unsigned count,const char* face=nullptr) {
  assert(reaction_player::requests.size()==count);
  if(face) assert(reaction_player::requests.back().face==face);
  for(const auto& request:reaction_player::requests) {
    assert(request.source==reaction_player::Source::Ambient);
    assert(request.label=="NONE");
  }
}

int main(int argc,char** argv) {
  assert(argc==2);
  const std::string test=argv[1];
  zumo_link::state.connected=zumo_link::state.ready=true;
  std::strcpy(zumo_link::state.fault,"NONE");
  speech_commands::state.ready=wake_word::state.ready=true;
  refresh();
  if(test=="startup") {
    tick();tick(999);expect(0);tick(1);expect(1,"HAPPY");
    complete();tick(10000);expect(1);
  } else if(test=="freshness") {
    tick();nowMs+=1001;zumo_link::state.replyAgeMs=1001;personality_events::poll();expect(0);
    tick();tick(999);expect(0);tick(1);expect(1,"HAPPY");
  } else if(test=="expiry") {
    zumo_link::state.ready=false;tick();tick(20000);
    zumo_link::state.ready=true;tick();tick(2000);expect(0);
  } else if(test=="busy_startup") {
    tick();zumo_link::state.left=3000;tick(100);
    zumo_link::state.left=0;tick(1000);tick(10000);expect(0);
  } else if(test=="stop") {
    tick();personality_events::cancel();tick(1000);missed();tick(10000);expect(0);
    personality_events::interaction();tick(1000);expect(0);missed();expect(1,"CONFUSED");
  } else if(test=="speech") {
    personality_events::interaction();beginSpeech();++robot_actions::state.generation;endSpeech();expect(0);
    missed();expect(1,"CONFUSED");complete();tick(1000);expect(1);
    missed();expect(1);tick(8000);missed();expect(2,"CONFUSED");
  } else if(test=="exact_result") {
    personality_events::interaction();
    for(const auto* result:{"Cancelled","Audio interrupted","Wake voice interrupted","No command recognized yet"}) {
      beginSpeech();endSpeech(result);expect(0);
    }
    beginSpeech();robot_actions::state.active=true;endSpeech();expect(0);
    robot_actions::state.active=false;missed();expect(1,"CONFUSED");
  } else if(test=="queue_expiry") {
    personality_events::interaction();audioEngine.state.state=AudioState::Playing;
    missed();expect(0);assert(personality_events::snapshot().pending==personality_events::Event::MissedCommand);
    tick(2000);audioEngine.state.state=AudioState::Ready;tick();expect(0);
    reaction_player::state.token=55;reaction_player::state.state=reaction_player::State::Playing;
    missed();tick(2001);complete();tick();expect(0);assert(reaction_player::cancelled==0);
  } else if(test=="queue_takeover") {
    personality_events::interaction();audioEngine.state.state=AudioState::Playing;
    missed();expect(0);++robot_actions::state.generation;
    audioEngine.state.state=AudioState::Ready;tick();expect(0);
    assert(personality_events::snapshot().pending==personality_events::Event::None);
  } else if(test=="fault") {
    personality_events::interaction();tick();
    zumo_link::state.edge=true;zumo_link::state.left=3000;zumo_link::state.targetLeftCps=3000;tick();expect(0);
    zumo_link::state.left=zumo_link::state.targetLeftCps=0;zumo_link::state.encoderLeftCps=1;tick(500);expect(0);
    zumo_link::state.encoderLeftCps=0;tick(100);expect(1,"SURPRISED");
    complete();tick(10000);expect(1);
    zumo_link::state.edge=false;tick();tick(500);zumo_link::state.edge=true;tick();expect(1);
    zumo_link::state.edge=false;tick();tick(1000);zumo_link::state.edge=true;tick();expect(2,"SURPRISED");
  } else if(test=="fault_expiry") {
    personality_events::interaction();tick();std::strcpy(zumo_link::state.fault,"SENSOR");
    zumo_link::state.left=3000;tick();tick(2000);zumo_link::state.left=0;tick();expect(0);
    tick(10000);expect(0);
  } else if(test=="fault_after_safety_stop") {
    tick();personality_events::cancel();personality_events::safetyStopped();
    zumo_link::state.edge=true;zumo_link::state.left=100;tick();expect(0);
    zumo_link::state.left=0;tick(100);expect(1,"SURPRISED");
    complete();tick(10000);expect(1);
  } else if(test=="fault_after_user_stop") {
    tick();personality_events::cancel();zumo_link::state.edge=true;tick();tick(10000);expect(0);
    personality_events::interaction();tick();expect(0);
  } else if(test=="fault_reconnect") {
    personality_events::interaction();std::strcpy(zumo_link::state.fault,"SENSOR");tick();expect(0);
    zumo_link::state.connected=false;tick();zumo_link::state.connected=true;tick();tick(10000);expect(0);
    std::strcpy(zumo_link::state.fault,"NONE");tick();tick(1000);
    std::strcpy(zumo_link::state.fault,"SENSOR");tick();expect(1,"SERIOUS");
  } else if(test=="ownership") {
    tick();tick(1000);expect(1,"HAPPY");
    beginSpeech();assert(reaction_player::cancelled==1);
    reaction_player::state.token=44;reaction_player::state.state=reaction_player::State::Playing;
    personality_events::cancel();personality_events::interaction();
    assert(reaction_player::cancelled==1 && reaction_player::active());
  } else if(test=="wrap") {
    nowMs=0xfffffe00U;tick();tick(999);expect(0);tick(1);expect(1,"HAPPY");
    complete();tick(8000);personality_events::interaction();missed();expect(2,"CONFUSED");
  } else return 2;
  std::cout<<"PASS personality "<<test<<"\n";
}
