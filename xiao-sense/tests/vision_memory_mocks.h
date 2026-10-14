#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

namespace mock {
inline uint32_t now = 1000;
inline std::unordered_map<void*, size_t> allocations;
inline unsigned allocationCalls = 0, failAllocationAt = 0;
inline unsigned modelCreated = 0, modelDestroyed = 0, modelRuns = 0;
inline unsigned inferenceBegins = 0, inferenceEnds = 0, gateDepth = 0;
inline unsigned decodeCalls = 0, framesReturned = 0, notifications = 0;
inline unsigned waitsRemaining = 1;
inline bool listening = false, visionEnabled = false, taskCreationFails = false;
inline bool frameAvailable = true, decodeFails = false;
inline void (*workerTask)(void*) = nullptr;
inline std::function<void()> captureHook, gateHook, runHook, allocateHook;
struct StopWorker {};
inline void* allocate(size_t length) {
  ++allocationCalls;
  if (allocateHook) allocateHook();
  if (allocationCalls == failAllocationAt) return nullptr;
  void* data = std::malloc(length);
  assert(data);
  allocations[data] = length;
  return data;
}
inline size_t allocatedBytes() {
  size_t bytes = 0;
  for (const auto& allocation : allocations) bytes += allocation.second;
  return bytes;
}
}  // namespace mock

using portMUX_TYPE = int;
using TaskHandle_t = void*;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
constexpr int pdPASS = 1, pdTRUE = 1;
inline uint32_t millis() { return mock::now; }
inline uint32_t pdMS_TO_TICKS(uint32_t time) { return time; }
inline int xTaskCreatePinnedToCore(void (*task)(void*), const char*, unsigned,
                                 void*, unsigned, TaskHandle_t* handle, unsigned) {
  if (mock::taskCreationFails) return 0;
  mock::workerTask = task;
  *handle = reinterpret_cast<void*>(1);
  return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t) { ++mock::notifications; }
inline unsigned ulTaskNotifyTake(int, uint32_t timeout) {
  mock::now += timeout;
  if (--mock::waitsRemaining == 0) throw mock::StopWorker{};
  return 0;
}
inline struct {
  template <typename... Args> void printf(const char*, Args...) {}
} Serial;

constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
inline void* heap_caps_malloc(size_t size, int) { return mock::allocate(size); }
inline void* heap_caps_aligned_alloc(size_t, size_t size, int) { return mock::allocate(size); }
inline void heap_caps_free(void* data) {
  if (!data) return;
  assert(mock::allocations.erase(data) == 1);
  std::free(data);
}

constexpr int PIXFORMAT_JPEG = 1;
struct camera_fb_t {
  unsigned format = PIXFORMAT_JPEG, width = 640, height = 480;
  size_t len = 32;
  uint8_t* buf;
  struct { long tv_sec = 1, tv_usec = 0; } timestamp;
};
inline camera_fb_t* esp_camera_fb_get() {
  static uint8_t data[32] = {};
  static camera_fb_t frame{PIXFORMAT_JPEG, 640, 480, sizeof(data), data};
  if (mock::captureHook) mock::captureHook();
  return mock::frameAvailable ? &frame : nullptr;
}
inline void esp_camera_fb_return(camera_fb_t*) { ++mock::framesReturned; }
namespace camera_service {
struct Frame { size_t size=0; uint32_t timestampMs=0; uint16_t width=0,height=0; };
inline bool ready() { return true; }
inline bool copyLatestJpeg(uint8_t* data, size_t capacity, Frame* info) {
  auto* frame=esp_camera_fb_get();
  if(!frame)return false;
  bool valid=frame->len<=capacity;
  if(valid) { std::memcpy(data,frame->buf,frame->len); *info={frame->len,1000,640,480}; }
  esp_camera_fb_return(frame);
  return valid;
}
}

constexpr int JPEG_PIXEL_FORMAT_RGB565_LE = 1, JPEG_ROTATE_0D = 0, JPEG_ERR_OK = 0;
using jpeg_error_t = int;
using jpeg_dec_handle_t = void*;
struct jpeg_dec_config_t {
  int output_type;
  struct { int width, height; } scale;
  int rotate;
};
struct jpeg_dec_io_t { uint8_t* inbuf; int inbuf_len; uint8_t* outbuf; };
struct jpeg_dec_header_info_t { unsigned width = 640, height = 480; };
inline int jpeg_dec_open(jpeg_dec_config_t*, jpeg_dec_handle_t* handle) {
  ++mock::decodeCalls;
  *handle = reinterpret_cast<void*>(1);
  return JPEG_ERR_OK;
}
inline int jpeg_dec_parse_header(jpeg_dec_handle_t, jpeg_dec_io_t*, jpeg_dec_header_info_t*) { return JPEG_ERR_OK; }
inline int jpeg_dec_get_outbuf_len(jpeg_dec_handle_t, int* size) { *size = 320 * 240 * 2; return JPEG_ERR_OK; }
inline int jpeg_dec_process(jpeg_dec_handle_t, jpeg_dec_io_t*) { return mock::decodeFails ? 1 : JPEG_ERR_OK; }
inline void jpeg_dec_close(jpeg_dec_handle_t) {}

using esp_partition_subtype_t = int;
constexpr int ESP_PARTITION_TYPE_DATA = 1, ESP_OK = 0;
struct esp_partition_t { uint32_t size = 1024; };
inline const esp_partition_t* esp_partition_find_first(int, int, const char*) {
  static esp_partition_t partition;
  return &partition;
}
namespace object_model { inline constexpr char kFileName[] = "battery.espdl"; }
inline int esp_partition_read(const esp_partition_t*, uint32_t offset, void* output, size_t count) {
  uint8_t package[1024] = {};
  const char* person = "pedestrian_detect_pico_s8_v1.espdl";
  const uint32_t header[2] = {0x324c4450, 2};
  const uint32_t entries[6] = {0, 64, static_cast<uint32_t>(std::strlen(person)),
                             0, 128, static_cast<uint32_t>(std::strlen(object_model::kFileName))};
  std::memcpy(package, header, sizeof(header));
  std::memcpy(package + 8, entries, sizeof(entries));
  std::memcpy(package + 64, person, std::strlen(person));
  std::memcpy(package + 128, object_model::kFileName, std::strlen(object_model::kFileName));
  assert(offset + count <= sizeof(package));
  std::memcpy(output, package + offset, count);
  return ESP_OK;
}

namespace speech_commands {
struct Status { bool listening; };
inline Status snapshot() { return {mock::listening}; }
}
namespace wake_word {
inline void setVisionEnabled(bool enabled) { mock::visionEnabled = enabled; }
inline void beginVisionInference() {
  assert(mock::gateDepth++ == 0);
  ++mock::inferenceBegins;
  if (mock::gateHook) mock::gateHook();
}
inline void endVisionInference() {
  assert(mock::gateDepth-- == 1);
  ++mock::inferenceEnds;
}
}

namespace dl {
namespace image {
constexpr int DL_IMAGE_PIX_TYPE_RGB565LE = 1;
struct img_t { void* data; int width, height, pix_type; };
}
namespace detect {
struct Result { std::vector<int> box; float score; int category; };
class Detect {
 public:
  Detect() { assert(mock::gateDepth == 1); ++mock::modelCreated; }
  virtual ~Detect() { assert(mock::gateDepth == 1); ++mock::modelDestroyed; }
  virtual Detect& set_score_thr(float, int) { return *this; }
  virtual std::vector<Result>& run(const image::img_t& image) {
    assert(mock::gateDepth == 1 && image.data && mock::allocatedBytes() == 219136);
    ++mock::modelRuns;
    if (mock::runHook) mock::runHook();
    return results;
  }
 private:
  std::vector<Result> results = {{{10, 20, 50, 90}, 0.8f, 0}};
};
}
}
class PedestrianDetect : public dl::detect::Detect {
 public:
  enum Model { PICO_S8_V1 };
  PedestrianDetect(Model, bool) {}
};
class BatteryDetector : public dl::detect::Detect {};
