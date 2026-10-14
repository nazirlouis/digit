#pragma once
#include "cJSON.h"
#include "../src/gemini_live.h"
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
using esp_transport_handle_t = void*;
using ws_transport_opcodes_t = int;
constexpr int WS_TRANSPORT_OPCODES_CONT=0, WS_TRANSPORT_OPCODES_TEXT=1, WS_TRANSPORT_OPCODES_BINARY=2,
  WS_TRANSPORT_OPCODES_CLOSE=8, WS_TRANSPORT_OPCODES_PING=9, WS_TRANSPORT_OPCODES_PONG=10, WS_TRANSPORT_OPCODES_FIN=128;
constexpr int MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2, ESP_OK=0, ESP_ERR_INVALID_ARG=-1, ESP_LOG_WARN=2, ESP_LOG_NONE=0;
constexpr int IPPROTO_TCP=6,TCP_NODELAY=1;
int esp_transport_get_socket(esp_transport_handle_t){return 7;}
int setsockopt(int fd,int protocol,int option,const void* value,size_t length){
 assert(fd==7&&protocol==IPPROTO_TCP&&option==TCP_NODELAY&&length==sizeof(int)&&*static_cast<const int*>(value)==1);return 0;
}
struct esp_transport_ws_config_t { const char* ws_path=nullptr; const char* headers=nullptr; bool propagate_control_frames=false; };
struct Frame { std::string data; int opcode=1; bool fin=true; uint32_t epoch=UINT32_MAX; };
struct Sent { std::string data; int opcode; uint32_t at; uint32_t epoch; };
std::deque<Frame> frames;
std::vector<std::vector<Frame>> connectionFrames;
std::vector<Sent> sentFrames;
std::vector<Sent> wireFrames;
std::string fragmentedSend;
uint32_t fragmentedAt=0, fragmentedEpoch=0;
size_t maxFrameWrite=SIZE_MAX, failWriteAt=0, stopAfterWrite=0, clearEpochAfterWrite=0;
int upgradeStatus=0;
std::vector<gemini_live::Event> events;
std::vector<std::string> eventText, cancelledIds;
struct ToolTrace { std::string id, name, reason, status, result; };
std::vector<ToolTrace> toolTraces;
Frame current;
size_t readOffset=0;
uint32_t mockMs=1000, stopAt=3000;
uint32_t sendAdvanceMs=0;
bool stopped=false, sendWorks=true, autoComplete=false;
bool completeOnCancel=false;
bool stopDuringRead=false, stopDuringPlayback=false, stopDuringEncode=false, clearEpochOnError=false;
uint32_t currentMediaEpoch=0;
unsigned connectFailures=0;
std::vector<uint32_t> connectAt;
int connects=0, audioReads=0, videoReads=0, videoSends=0, actionCalls=0, playedSamples=0;
std::string sent, capturedHeaders;
int sentOpcode=0, logLevel=-1;
size_t maxAudioReads=0;
std::vector<int16_t> audioSource;
std::vector<uint8_t> jpeg{0xff,0xd8,0,0,0xff,0xd9};
std::deque<std::pair<std::string,std::string>> completions;
gemini_live::ActionResult actionResult=gemini_live::ActionResult::Queued;
size_t activeAllocations=0;
void* heap_caps_malloc(size_t size,int) { auto* p=malloc(size); if(p)++activeAllocations; return p; }
void* heap_caps_calloc(size_t n,size_t size,int) { auto* p=calloc(n,size); if(p)++activeAllocations; return p; }
void heap_caps_free(void* p) { if(p){assert(activeAllocations);--activeAllocations;free(p);} }
void mbedtls_platform_zeroize(void* p,size_t n){ memset(p,0,n); }
uint32_t pdMS_TO_TICKS(uint32_t n) { return n; }
void vTaskDelay(uint32_t n) { mockMs += n; }
int64_t esp_timer_get_time(){return int64_t(mockMs)*1000;}
void esp_log_level_set(const char* tag,int level){if(!strcmp(tag,"transport_ws"))logLevel=level;}
void esp_crt_bundle_attach(){}
esp_transport_handle_t esp_transport_ssl_init(){return reinterpret_cast<void*>(1);}
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t){return reinterpret_cast<void*>(2);}
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t,void(*)()){}
void esp_transport_close(esp_transport_handle_t){frames.clear();readOffset=0;fragmentedSend.clear();}
void esp_transport_destroy(esp_transport_handle_t){}
int esp_transport_ws_set_config(esp_transport_handle_t,const esp_transport_ws_config_t* c){
 assert(std::string(c->ws_path).find("key=")==std::string::npos);
 capturedHeaders=c->headers;return ESP_OK;
}
int esp_transport_connect(esp_transport_handle_t,const char* host,int port,int){
 assert(std::string(host)=="generativelanguage.googleapis.com" && port==443);
 connectAt.push_back(mockMs);
 if(connectFailures){--connectFailures;++connects;return -1;}
 if(size_t(connects)<connectionFrames.size())for(auto& f:connectionFrames[connects])frames.push_back(f);
 ++connects;return 0;
}
int esp_transport_ws_get_upgrade_request_status(esp_transport_handle_t){return upgradeStatus;}
int esp_transport_ws_send_raw(esp_transport_handle_t,int opcode,const char* data,int length,int){
 wireFrames.push_back({std::string(data,length),opcode,mockMs,currentMediaEpoch});
 const bool fail=!sendWorks||(failWriteAt&&wireFrames.size()==failWriteAt);
 if(!fail&&size_t(length)<=maxFrameWrite) {
  if((opcode&15)==WS_TRANSPORT_OPCODES_TEXT && !(opcode&WS_TRANSPORT_OPCODES_FIN)) {
   assert(fragmentedSend.empty());fragmentedSend.assign(data,length);fragmentedAt=mockMs;fragmentedEpoch=currentMediaEpoch;
  } else if((opcode&15)==WS_TRANSPORT_OPCODES_CONT) {
   assert(!fragmentedSend.empty());fragmentedSend.append(data,length);
   if(opcode&WS_TRANSPORT_OPCODES_FIN) {
    sent=fragmentedSend;sentOpcode=WS_TRANSPORT_OPCODES_TEXT|WS_TRANSPORT_OPCODES_FIN;
    sentFrames.push_back({sent,sentOpcode,fragmentedAt,fragmentedEpoch});fragmentedSend.clear();
   }
  } else {
   sent.assign(data,length);sentOpcode=opcode;sentFrames.push_back({sent,opcode,mockMs,currentMediaEpoch});
  }
 }
 mockMs+=sendAdvanceMs;
 char* writable=const_cast<char*>(data);
 for(int i=0;i<length;++i)writable[i]^=0x5a;
 for(int i=0;i<length;++i)writable[i]^=0x5a;
 if(stopAfterWrite&&wireFrames.size()==stopAfterWrite)stopped=true;
 if(clearEpochAfterWrite&&wireFrames.size()==clearEpochAfterWrite)currentMediaEpoch=0;
 return fail?-1:static_cast<int>(std::min<size_t>(length,maxFrameWrite));
}
int esp_transport_poll_read(esp_transport_handle_t,int timeout){
 mockMs+=timeout;
 return readOffset||!frames.empty()?1:0;
}
int esp_transport_read(esp_transport_handle_t,char* dest,int cap,int){
 if(!readOffset){
   if(frames.empty())return 0;
   current=frames.front();frames.pop_front();
   if(current.epoch!=UINT32_MAX)currentMediaEpoch=current.epoch;
 }
 const size_t n=std::min<size_t>(cap,current.data.size()-readOffset);
 memcpy(dest,current.data.data()+readOffset,n);readOffset+=n;
 if(readOffset==current.data.size())readOffset=0;
 return static_cast<int>(n);
}
int esp_transport_ws_get_read_opcode(esp_transport_handle_t){return current.opcode;}
int esp_transport_ws_get_read_payload_len(esp_transport_handle_t){return static_cast<int>(current.data.size());}
bool esp_transport_ws_get_fin_flag(esp_transport_handle_t){return current.fin;}
const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int mbedtls_base64_encode(unsigned char* out,size_t cap,size_t* used,const unsigned char* in,size_t n){
 *used=4*((n+2)/3);if(cap<=*used)return -1;
 size_t o=0;for(size_t i=0;i<n;i+=3){
 unsigned v=unsigned(in[i])<<16;if(i+1<n)v|=unsigned(in[i+1])<<8;if(i+2<n)v|=in[i+2];
 out[o++]=alphabet[(v>>18)&63];out[o++]=alphabet[(v>>12)&63];
 out[o++]=i+1<n?alphabet[(v>>6)&63]:'=';out[o++]=i+2<n?alphabet[v&63]:'=';
 }out[o]=0;if(stopDuringEncode){stopDuringEncode=false;currentMediaEpoch=0;}return 0;
}
int mbedtls_base64_decode(unsigned char* out,size_t cap,size_t* used,const unsigned char* in,size_t n){
 if(n%4)return -1;*used=0;
 for(size_t i=0;i<n;i+=4){
 unsigned v=0;
 for(size_t j=0;j<4;++j){const char* p=strchr(alphabet,in[i+j]);if(!p&&in[i+j]!='=')return -1;v=(v<<6)|(p?unsigned(p-alphabet):0);}
 const size_t bytes=in[i+2]=='='?1:in[i+3]=='='?2:3;if(*used+bytes>cap)return -1;
 out[(*used)++]=static_cast<unsigned char>(v>>16);
 if(bytes>1)out[(*used)++]=static_cast<unsigned char>(v>>8);
 if(bytes>2)out[(*used)++]=static_cast<unsigned char>(v);
 }return 0;
}
bool cancelled(void*){return stopped||mockMs>=stopAt;}
size_t readAudio(int16_t* dest,size_t cap,void*){
 ++audioReads;
 if(stopDuringRead){stopDuringRead=false;currentMediaEpoch=0;return cap+1;}
 if(maxAudioReads&&size_t(audioReads)>maxAudioReads)return 0;
 const size_t n=std::min(cap,audioSource.size());std::copy_n(audioSource.data(),n,dest);return n;
}
bool playAudio(const int16_t*,size_t n,void*){if(stopDuringPlayback){stopDuringPlayback=false;currentMediaEpoch=0;return false;}playedSamples+=int(n);return true;}
bool mockTaskActive=false;
void event(gemini_live::Event e,const char* value,void*){if(e==gemini_live::Event::UserPrompt)mockTaskActive=true;events.push_back(e);eventText.emplace_back(value);if(clearEpochOnError&&e==gemini_live::Event::Error)currentMediaEpoch=0;}
gemini_live::ActionResult action(const char* callId,const robot_actions::ToolRequest& request,void*){
 assert(robot_actions::validCommand(static_cast<int>(request.command)));++actionCalls;
 if(autoComplete)completions.push_back({callId,"Completed: mocked local outcome"});
 return actionResult;
}
size_t readVideo(uint8_t* dest,size_t cap,uint32_t* sequence,void*){
 ++videoReads;if(jpeg.size()>cap)return 0;memcpy(dest,jpeg.data(),jpeg.size());*sequence=77;return jpeg.size();
}
void videoSent(const uint8_t* data,size_t n,uint32_t sequence,void*){assert(n==jpeg.size()&&sequence==77);assert(!memcmp(data,jpeg.data(),n));++videoSends;}
void cancelTool(const char* id,void*){cancelledIds.emplace_back(id);if(completeOnCancel)completions.push_back({id,"STALE_COMPLETION"});}
bool pollToolResult(char* id,size_t cap,char* result,size_t resultCap,void*){
 if(completions.empty())return false;auto value=completions.front();completions.pop_front();
 snprintf(id,cap,"%s",value.first.c_str());snprintf(result,resultCap,"%s",value.second.c_str());return true;
}
bool canResume(void*){return true;}
bool taskActive(void*){return mockTaskActive;}
uint32_t getMediaEpoch(void*){return currentMediaEpoch;}
void toolTrace(const char* id,const char* name,const robot_actions::ToolRequest& request,const char* status,const char* result,void*) {
 toolTraces.push_back({id,name,request.actionReason,status,result});
}
gemini_live::Callbacks callbacks(){auto cb=gemini_live::Callbacks{nullptr,cancelled,nullptr,event,action,readVideo,videoSent,cancelTool,pollToolResult,canResume,nullptr,taskActive};cb.toolEvent=toolTrace;return cb;}
void resetMocks(){
 mockTaskActive=false;
 toolTraces.clear();
 assert(activeAllocations==0);frames.clear();connectionFrames.clear();sentFrames.clear();wireFrames.clear();fragmentedSend.clear();events.clear();eventText.clear();
 maxFrameWrite=SIZE_MAX;failWriteAt=stopAfterWrite=clearEpochAfterWrite=0;upgradeStatus=0;
 cancelledIds.clear();completions.clear();current={};readOffset=0;mockMs=1000;stopAt=3000;stopped=false;sendWorks=true;
 sendAdvanceMs=0;
 currentMediaEpoch=0;connectFailures=0;completeOnCancel=false;
 stopDuringRead=stopDuringPlayback=stopDuringEncode=clearEpochOnError=false;connectAt.clear();
 connects=audioReads=videoReads=videoSends=actionCalls=playedSamples=0;sent.clear();capturedHeaders.clear();logLevel=-1;maxAudioReads=0;autoComplete=false;
 audioSource.clear();jpeg={0xff,0xd8,0,0,0xff,0xd9};actionResult=gemini_live::ActionResult::Queued;
}
#define GEMINI_LIVE_HOST_TEST
#include "../src/gemini_live.cpp"
