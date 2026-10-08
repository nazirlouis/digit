// Exercise the production HTTP handlers with audio/UART replaced by observers.
#include "emotion_voice.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
using esp_err_t = int;
struct httpd_req_t { std::map<std::string,std::string> query; std::string status="200 OK", body; };
void httpd_resp_set_status(httpd_req_t* r, const char* status) { r->status=status; }
bool queryValue(httpd_req_t* r,const char* key,char* out,size_t size) {
  auto it=r->query.find(key);
  if(it==r->query.end()||it->second.size()>=size)return false;
  strcpy(out,it->second.c_str());return true;
}
esp_err_t sendJson(httpd_req_t* r,const char* body) { r->body=body;return 0; }
enum class AudioState { Ready, Recording, Playing };
struct AudioStatus { bool available=true; AudioState state=AudioState::Ready; bool cuePlaying=false; };
struct AudioEngine {
  AudioStatus state;
  AudioCue last=AudioCue::None;
  unsigned plays=0,cancels=0;
  AudioStatus snapshot() { return state; }
  bool playCue(AudioCue cue) { assert(isEmotionCue(cue));last=cue;state.cuePlaying=true;++plays;return true; }
  void cancelOutput() { last=AudioCue::None;state.cuePlaying=false;++cancels; }
} audioEngine;
namespace zumo_link {
struct Status { bool connected=true,calibrating=false,manualDriving=false,lookActive=false;int left=0,right=0; } state;
std::string lastFace;
unsigned faces=0,polls=0;
Status snapshot(){return state;}
bool isFaceName(const char* name){return isEmotionCue(emotionCueForFace(name));}
bool face(const char* name){if(!state.connected||state.left||state.right)return false;lastFace=name;++faces;return true;}
void poll(){++polls;}
}
namespace person_follow {
struct Status { bool enabled=false; } state;
Status snapshot(){return state;}
void setEnabled(bool enabled){state.enabled=enabled;}
}
namespace robot_actions {
struct Guard {};
struct Status { bool active=false; } state;
uint32_t epoch=1;
uint32_t generation(){return epoch;}
Status snapshot(){return state;}
void manualOverride(){++epoch;}
void stop(const char*){++epoch;audioEngine.cancelOutput();state.active=false;}
}
namespace reaction_player {
enum class Source {Preview};
AudioCue queued=AudioCue::None;
unsigned requests=0;
unsigned play(const char* face,const char*,AudioCue cue,Source) {
  audioEngine.cancelOutput();zumo_link::lastFace=face;++zumo_link::faces;
  queued=cue;return ++requests;
}
void visible(){if(queued!=AudioCue::None)audioEngine.playCue(queued);queued=AudioCue::None;}
}
void escapeJsonString(const char* input,char* output,size_t size){snprintf(output,size,"%s",input);}
#include "face_generation.inc"
#include "face_controls.inc"

httpd_req_t request(const char* face,const char* voice="1") {
  return {{{"value",face},{"voice",voice},{"generation",std::to_string(robot_actions::generation())}}};
}
void rejected(httpd_req_t r,const char* status) {
  const auto plays=audioEngine.plays, faces=zumo_link::faces, epoch=robot_actions::epoch;
  zumoFaceHandler(&r);
  assert(r.status==status);
  assert(plays==audioEngine.plays&&faces==zumo_link::faces&&epoch==robot_actions::epoch);
}
int main() {
  const char* names[]={"NEUTRAL","HAPPY","SAD","MAD","SERIOUS","CURIOUS","SURPRISED",
                       "SLEEPING","CONFUSED","SLEEPY","EXCITED","CRYING","LOVE"};
  for(auto* name:names){const auto sounds=audioEngine.plays;auto r=request(name);zumoFaceHandler(&r);assert(r.status=="200 OK");
    assert(zumo_link::lastFace==name&&reaction_player::queued==emotionCueForFace(name));
    assert(audioEngine.plays==sounds);reaction_player::visible();assert(audioEngine.plays==sounds+1);}
  assert(audioEngine.plays==13&&audioEngine.cancels==13&&reaction_player::requests==13);
  auto silent=request("HAPPY","0");zumoFaceHandler(&silent);
  assert(silent.status=="200 OK"&&!audioEngine.state.cuePlaying&&audioEngine.plays==13);
  rejected(request("DIZZY"),"400 Bad Request");rejected(request("HAPPY","2"),"400 Bad Request");
  for(auto* flag:{&zumo_link::state.calibrating,&zumo_link::state.manualDriving,&zumo_link::state.lookActive,
                  &person_follow::state.enabled,&robot_actions::state.active}){
    *flag=true;rejected(request("HAPPY"),"409 Conflict");*flag=false;
  }
  zumo_link::state.connected=false;rejected(request("HAPPY"),"409 Conflict");zumo_link::state.connected=true;
  zumo_link::state.left=100;rejected(request("HAPPY"),"409 Conflict");zumo_link::state.left=0;
  audioEngine.state.available=false;rejected(request("HAPPY"),"409 Conflict");audioEngine.state.available=true;
  for(auto busy:{AudioState::Recording,AudioState::Playing}){
    audioEngine.state.state=busy;rejected(request("HAPPY"),"409 Conflict");
  }
  audioEngine.state.state=AudioState::Ready;
  auto delayed=request("LOVE");httpd_req_t stop;zumoFaceStopHandler(&stop);
  assert(!audioEngine.state.cuePlaying&&zumo_link::lastFace=="NEUTRAL");rejected(delayed,"409 Conflict");
  for(auto value:{"", "-1", "4294967296", "1x"}) {auto r=request("HAPPY");r.query["generation"]=value;rejected(r,"409 Conflict");}
  auto legacy=request("CURIOUS");legacy.query.erase("voice");legacy.query.erase("generation");
  zumoFaceHandler(&legacy);assert(legacy.status=="200 OK"&&audioEngine.plays==13);
  std::cout<<"PASS: real face handlers; thirteen mappings, replacement, face-only, busy guards, stale generations, Stop reset, legacy API\n";
}
