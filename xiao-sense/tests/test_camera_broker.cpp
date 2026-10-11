#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "camera_service.h"
using SemaphoreHandle_t = int*;
constexpr int pdTRUE=1, PIXFORMAT_JPEG=1;
inline unsigned now=1000, returns=0;
inline bool held=false, rejectLock=false;
inline uint32_t millis(){return now;}
inline unsigned pdMS_TO_TICKS(unsigned x){return x;}
inline int xSemaphoreTake(SemaphoreHandle_t,unsigned){if(rejectLock)return 0;assert(!held);held=true;return 1;}
inline void xSemaphoreGive(SemaphoreHandle_t){assert(held);held=false;}
struct camera_fb_t {int format=PIXFORMAT_JPEG;size_t len=4;uint8_t* buf;unsigned width=640,height=480;struct {long tv_sec=1,tv_usec=0;} timestamp;};
uint8_t bytes[]={1,2,3,4};
camera_fb_t captured{PIXFORMAT_JPEG,4,bytes,640,480};
inline camera_fb_t* esp_camera_fb_get(){return &captured;}
inline void esp_camera_fb_return(camera_fb_t*){assert(!held);++returns;}
struct Tick{};
inline void vTaskDelay(unsigned){throw Tick{};}
bool cameraReady=true;
#include "camera_capture.inc"
namespace camera_service {
#include "camera_copy.inc"
}
void capture(){try{captureTask(nullptr);}catch(Tick&){}assert(!held);}
int main(){
  int mutex=0;frameMutex=&mutex;uint8_t cache[8]={};latestJpeg=cache;
  uint8_t out[8]={};camera_service::Frame info;
  assert(!camera_service::copyLatestJpeg(out,sizeof(out),&info));
  capture();assert(returns==1&&latestFrame.size==4&&latestFrame.timestampMs==1000);
  assert(camera_service::copyLatestJpeg(out,sizeof(out),&info));
  assert(!memcmp(bytes,out,4)&&info.width==640&&info.height==480);
  assert(!camera_service::copyLatestJpeg(out,3,&info)&&info.size==0&&!held);
  now=1501;assert(!camera_service::copyLatestJpeg(out,sizeof(out),&info)&&info.size==0&&!held);
  now=1000;rejectLock=true;capture();assert(returns==2);
  assert(!camera_service::copyLatestJpeg(out,sizeof(out),&info));rejectLock=false;
  captured.len=camera_service::kMaxJpegBytes+1;capture();assert(returns==3&&latestFrame.size==4);
  cameraReady=false;assert(!camera_service::copyLatestJpeg(out,sizeof(out),&info));
  puts("Camera broker ownership, capacity, age, and contention tests passed");
}
