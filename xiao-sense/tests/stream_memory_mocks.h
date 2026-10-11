#pragma once

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

using esp_err_t = int;
using TickType_t = uint32_t;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = -2, PIXFORMAT_JPEG = 1;
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
struct httpd_req_t {};
struct camera_fb_t { int format; uint8_t* buf; size_t len; };
namespace mock {
inline bool ready = true, frameHeld = false, allocationFails = false;
inline unsigned captureLimit = 1, captures = 0, returns = 0;
inline unsigned allocationsCalled = 0, sendCalls = 0, failSendAt = 0, bodySends = 0;
inline unsigned failAllocationAt = 0;
inline int frameFormat = PIXFORMAT_JPEG;
inline size_t frameLength = 32;
inline uint8_t cameraBytes[32];
inline std::unordered_map<void*, size_t> allocations;
inline std::string status, contentType, message;
inline unsigned errorResponses = 0;
}
inline uint32_t pdMS_TO_TICKS(uint32_t time) { return time; }
inline uint32_t xTaskGetTickCount() { return 100; }
inline void vTaskDelay(uint32_t) { assert(!mock::frameHeld && mock::allocations.size() == 1); }
inline struct { void println(const char*) {} } Serial;
namespace app_config { constexpr unsigned kTargetCameraFps = 10; }
namespace camera_service {
inline constexpr size_t kMaxJpegBytes = 256 * 1024;
struct Frame { size_t size = 0; uint32_t timestampMs = 0; uint16_t width = 0, height = 0; };
inline bool ready() { return mock::ready; }
inline const char* lastError() { return "Camera unavailable"; }
inline bool copyLatestJpeg(uint8_t* destination, size_t capacity, Frame* frame) {
  assert(!mock::frameHeld && mock::allocations.size() == 1);
  if (mock::captures >= mock::captureLimit) return false;
  ++mock::captures;
  if (!mock::frameLength || mock::frameLength > capacity || mock::frameFormat != PIXFORMAT_JPEG) return false;
  for (size_t i = 0; i < mock::frameLength; ++i) destination[i] = static_cast<uint8_t>(i + 1);
  *frame = {mock::frameLength, 100, 640, 480};
  ++mock::returns;
  return true;
}
}
constexpr char kStreamContentType[] = "multipart/x-mixed-replace;boundary=frame";
constexpr char kStreamBoundary[] = "\r\n--frame\r\n";
constexpr char kStreamPartHeader[] = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
inline void setNoCache(httpd_req_t*) {}
inline int httpd_resp_set_type(httpd_req_t*, const char* type) { mock::contentType = type; return ESP_OK; }
inline int httpd_resp_set_status(httpd_req_t*, const char* status) { mock::status = status; return ESP_OK; }
inline int httpd_resp_sendstr(httpd_req_t*, const char* text) {
  assert(!mock::frameHeld && mock::allocations.empty());
  ++mock::errorResponses;
  mock::message = text;
  return ESP_OK;
}
inline void* heap_caps_malloc(size_t length, int caps) {
  assert(!mock::frameHeld && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  assert(length == camera_service::kMaxJpegBytes); // One bounded client buffer, reused across frames.
  ++mock::allocationsCalled;
  if (mock::allocationFails || mock::allocationsCalled == mock::failAllocationAt) return nullptr;
  void* data = std::malloc(length);
  assert(data);
  mock::allocations[data] = length;
  return data;
}
inline void heap_caps_free(void* data) {
  assert(!mock::frameHeld && mock::allocations.erase(data) == 1);
  std::free(data);
}
inline int httpd_resp_send_chunk(httpd_req_t*, const char* data, size_t length) {
  // Even boundary/header writes can stall, so the camera must already be free.
  assert(!mock::frameHeld && mock::allocations.size() == 1);
  ++mock::sendCalls;
  if (mock::allocations.count(const_cast<char*>(data))) {
    ++mock::bodySends;
    assert(length == mock::frameLength);
    for (size_t i = 0; i < length; ++i) assert(static_cast<uint8_t>(data[i]) == i + 1);
  }
  return mock::sendCalls == mock::failSendAt ? -9 : ESP_OK;
}
