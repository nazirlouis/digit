#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
inline uint32_t nowMs = 1000;
inline uint32_t millis(){return nowMs;}
inline void delay(unsigned){}
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
constexpr int pdPASS=1;
inline int xTaskCreatePinnedToCore(void(*)(void*),const char*,int,void*,int,void*,int){return pdPASS;}
struct { template<class... T> void printf(const char*,T...){} } Serial;
enum class AudioState { Ready, Playing };
struct AudioMock {
  bool available=true,wakeChimePlaying=false,cuePlaying=false;
  AudioState state=AudioState::Ready;
  AudioMock snapshot(){return *this;}
  bool readXvfCommand(uint8_t,uint8_t,uint8_t*,size_t,uint8_t*){return false;}
};
inline AudioMock audioEngine;
namespace speech_commands {
struct Status { bool liveSpeaking = false; };
inline Status state;
inline Status snapshot() { return state; }
inline bool liveSpeaking() { return state.liveSpeaking; }
}
namespace wake_word {
struct Status {bool pausedForVision=false;};
inline Status state;
inline Status snapshot(){return state;}
}
namespace zumo_link {
struct Status {bool connected=true,ready=false,voiceCapable=true,rampedLookCapable=true;
  bool manualDriving=false,lookActive=false,calibrating=false,edge=false;int left=0,right=0;char fault[24]="NONE";};
inline Status state;
inline Status snapshot(){return state;}
inline unsigned cancelled=0;
inline void cancelLook(){++cancelled;state.lookActive=false;}
}
using nvs_handle_t = int;
using esp_err_t = int;
constexpr int ESP_OK=0,NVS_READONLY=0,NVS_READWRITE=1;
inline std::vector<uint8_t> savedNvs;
inline int nvs_open(const char*,int mode,nvs_handle_t* handle){*handle=1;return mode==NVS_READONLY&&savedNvs.empty()?1:0;}
inline int nvs_get_blob(nvs_handle_t,const char*,void* data,size_t* size){
  if(savedNvs.size()>*size)return 1;
  memcpy(data,savedNvs.data(),savedNvs.size());*size=savedNvs.size();return 0;
}
inline int nvs_set_blob(nvs_handle_t,const char*,const void* data,size_t size){
  auto bytes=static_cast<const uint8_t*>(data);savedNvs.assign(bytes,bytes+size);return 0;
}
inline int nvs_commit(nvs_handle_t){return 0;}
inline void nvs_close(nvs_handle_t){}
