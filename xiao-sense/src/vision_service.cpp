#include "vision_service.h"

#include <cstring>
#include "Arduino.h"
#include "camera_service.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_partition.h"
#include "pedestrian_detect.hpp"
#include "battery_detector.h"
#include "object_model_config.h"
#include "wake_word.h"
#include "speech_commands.h"

namespace {
constexpr size_t kJpegCapacity = 256 * 1024;
constexpr int kWidth = 320;
constexpr int kHeight = 240;
// Leave headroom for occasional camera waits while the MJPEG stream is active.
constexpr uint32_t kPeriodMs = 400;
constexpr uint32_t kStaleMs = 5000;
constexpr float kScoreThreshold = 0.55f;
constexpr float kBatteryScoreThreshold = 0.35f;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
vision_service::Status state = {};
uint32_t result_at = 0;
uint32_t generation = 0;
uint8_t* jpeg_copy = nullptr;
uint8_t* rgb565 = nullptr;
dl::detect::Detect* detector = nullptr;
bool loaded_battery = false;
char decode_error[80] = "JPEG decode failed";

bool decode_frame(size_t jpeg_length) {
  jpeg_dec_config_t config = {};
  config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
  config.scale.width = kWidth;
  config.scale.height = kHeight;
  config.rotate = JPEG_ROTATE_0D;
  jpeg_dec_handle_t decoder = nullptr;
  jpeg_error_t result = jpeg_dec_open(&config, &decoder);
  if (result != JPEG_ERR_OK) {
    snprintf(decode_error, sizeof(decode_error), "JPEG open: %d", result);
    return false;
  }
  jpeg_dec_io_t io = {};
  io.inbuf = jpeg_copy;
  io.inbuf_len = static_cast<int>(jpeg_length);
  io.outbuf = rgb565;
  jpeg_dec_header_info_t header = {};
  int output_length = 0;
  result = jpeg_dec_parse_header(decoder, &io, &header);
  if (result != JPEG_ERR_OK) {
    snprintf(decode_error, sizeof(decode_error), "JPEG header: %d", result);
  } else {
    result = jpeg_dec_get_outbuf_len(decoder, &output_length);
    if (result != JPEG_ERR_OK) {
      snprintf(decode_error, sizeof(decode_error), "JPEG output size: %d", result);
    } else if (output_length != kWidth * kHeight * 2) {
      snprintf(decode_error, sizeof(decode_error), "JPEG output %d, source %ux%u",
               output_length, header.width, header.height);
    } else {
      result = jpeg_dec_process(decoder, &io);
      if (result != JPEG_ERR_OK) {
        snprintf(decode_error, sizeof(decode_error), "JPEG process: %d", result);
      }
    }
  }
  jpeg_dec_close(decoder);
  return result == JPEG_ERR_OK && output_length == kWidth * kHeight * 2;
}

void fail(const char* reason) {
  portENTER_CRITICAL(&lock);
  snprintf(state.error, sizeof(state.error), "%s", reason);
  portEXIT_CRITICAL(&lock);
  Serial.printf("Vision: %s\n", reason);
}
bool model_present(const char* name) {
  const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                               static_cast<esp_partition_subtype_t>(0x40),
                                                               "pedestrian_det");
  if (!partition) return false;
  uint32_t header[2] = {};
  if (esp_partition_read(partition, 0, header, sizeof(header)) != ESP_OK) return false;
  // A combined package must contain the selected model, not just nonblank data.
  if (memcmp(header, "PDL2", 4) != 0 || header[1] > 16) return false;
  for (uint32_t i = 0; i < header[1]; ++i) {
    uint32_t entry[3] = {};
    if (esp_partition_read(partition, 8 + 12 * i, entry, sizeof(entry)) != ESP_OK) return false;
    char model_name[64] = {};
    if (entry[2] == 0 || entry[2] >= sizeof(model_name) ||
        entry[1] > partition->size || entry[2] > partition->size - entry[1]) continue;
    if (esp_partition_read(partition, entry[1], model_name, entry[2]) == ESP_OK &&
        strcmp(model_name, name) == 0) return true;
  }
  return false;
}
void task(void*) {
  for (;;) {
    uint32_t accepted_interval_ms = 0;
    portENTER_CRITICAL(&lock);
    const bool enabled = state.enabled;
    const bool battery = state.batteryMode;
    const uint32_t current_generation = generation;
    portEXIT_CRITICAL(&lock);
    if (!enabled || speech_commands::snapshot().listening) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    const uint32_t started = millis();
    camera_fb_t* frame = esp_camera_fb_get();
    const bool frame_ok = frame && frame->format == PIXFORMAT_JPEG && frame->width == 640 &&
                          frame->height == 480 && frame->len > 0 && frame->len <= kJpegCapacity;
    size_t jpeg_length = 0;
    const uint32_t capture_ms = frame_ok ?
        uint32_t(uint64_t(frame->timestamp.tv_sec) * 1000 + frame->timestamp.tv_usec / 1000) : 0;
    if (frame_ok) { jpeg_length = frame->len; memcpy(jpeg_copy, frame->buf, jpeg_length); }
    if (frame) esp_camera_fb_return(frame);
    if (!frame_ok || !decode_frame(jpeg_length)) {
      fail(frame_ok ? decode_error : "Camera frame unavailable for detection");
      vTaskDelay(pdMS_TO_TICKS(kPeriodMs));
      continue;
    }
    const uint32_t decode_ms = millis() - started;
    dl::image::img_t image = {};
    image.data = rgb565;
    image.width = kWidth;
    image.height = kHeight;
    image.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
    wake_word::beginVisionInference();
    // Only the camera task owns these pointers. Mode changes invalidate the
    // result generation immediately; model allocation happens outside the lock.
    if (!detector || loaded_battery != battery) {
      delete detector;
      detector = battery ? static_cast<dl::detect::Detect*>(new BatteryDetector())
                         : static_cast<dl::detect::Detect*>(new PedestrianDetect(PedestrianDetect::PICO_S8_V1, true));
      detector->set_score_thr(battery ? kBatteryScoreThreshold : kScoreThreshold, 0);
      loaded_battery = battery;
    }
    auto& results = detector->run(image);
    wake_word::endVisionInference();
    const uint32_t inference_ms = millis() - started - decode_ms;
    vision_service::Status::Box boxes[5] = {};
    uint8_t count = 0;
    int highest = 0;
    int battery_highest = 0;
    for (const auto& result : results) {
      if (result.box.size() < 4 || result.score < (battery ? kBatteryScoreThreshold : kScoreThreshold)) continue;
      const int score = std::min(100, static_cast<int>(result.score * 100 + 0.5f));
      highest = std::max(highest, score);
      if (battery && result.category == 0) battery_highest = std::max(battery_highest, score);
      if (count < 5) boxes[count++] = {result.box[0], result.box[1], result.box[2], result.box[3],
                                       static_cast<uint8_t>(score), static_cast<uint8_t>(result.category)};
    }
    portENTER_CRITICAL(&lock);
    const bool accepted = state.enabled && generation == current_generation;
    if (accepted) {
      state.hasResult = true;
      state.captureTimestampMs = capture_ms;
      state.personDetected = !battery && count > 0;
      state.personScorePercent = battery ? 0 : static_cast<uint8_t>(highest);
      state.batteryDetected = battery_highest > 0;
      state.batteryScorePercent = static_cast<uint8_t>(battery_highest);
      state.boxCount = count;
      memcpy(state.boxes, boxes, sizeof(boxes));
      state.imageWidth = kWidth;
      state.imageHeight = kHeight;
      state.inferenceMs = inference_ms;
      state.decodeMs = decode_ms;
      state.totalMs = millis() - started;
      state.error[0] = '\0';
      const uint32_t now = millis();
      state.intervalMs = result_at ? now - result_at : 0;
      accepted_interval_ms = state.intervalMs;
      result_at = now;
    }
    portEXIT_CRITICAL(&lock);
    if (accepted) Serial.printf("Vision %s: capture+decode=%lu ms model=%lu ms total=%lu ms interval=%lu ms objects=%u best=%d%%\n",
                                battery ? "Objects" : "Person",
                                (unsigned long)decode_ms, (unsigned long)inference_ms,
                                (unsigned long)(millis() - started),
                                (unsigned long)accepted_interval_ms, count, highest);
    const uint32_t elapsed = millis() - started;
    vTaskDelay(pdMS_TO_TICKS(elapsed < kPeriodMs ? kPeriodMs - elapsed : 1));
  }
}
}  // namespace

namespace vision_service {
void begin() {
  if (!camera_service::ready()) { fail("Camera is unavailable for person detection"); return; }
  const bool person_ready = model_present("pedestrian_detect_pico_s8_v1.espdl");
  const bool battery_ready = model_present(object_model::kFileName);
  if (!person_ready && !battery_ready) { fail("Vision model bundle is missing; flash the complete project"); return; }
  jpeg_copy = static_cast<uint8_t*>(heap_caps_malloc(kJpegCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  rgb565 = static_cast<uint8_t*>(heap_caps_aligned_alloc(
      16, kWidth * kHeight * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!jpeg_copy || !rgb565) { fail("Not enough PSRAM for person detection"); return; }
  if (xTaskCreatePinnedToCore(task, "pedestrian", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
    fail("Could not start person detection task"); return;
  }
  portENTER_CRITICAL(&lock);
  state.ready = person_ready;
  state.batteryReady = battery_ready;
  state.imageWidth = kWidth;
  state.imageHeight = kHeight;
  state.error[0] = '\0';
  portEXIT_CRITICAL(&lock);
  Serial.printf("Vision: person=%s objects=%s; models load when started\n",
                person_ready ? "ready" : "missing", battery_ready ? "ready" : "missing");
}
bool setMode(bool enabled, bool battery) {
  portENTER_CRITICAL(&lock);
  if (enabled && !(battery ? state.batteryReady : state.ready)) { portEXIT_CRITICAL(&lock); return false; }
  state.enabled = enabled;
  state.batteryMode = battery;
  state.hasResult = false;
  state.personDetected = false;
  state.batteryDetected = false;
  state.personScorePercent = state.batteryScorePercent = 0;
  state.boxCount = 0;
  state.error[0] = '\0';
  state.intervalMs = 0;
  result_at = 0;
  ++generation;
  wake_word::setVisionEnabled(enabled);
  portEXIT_CRITICAL(&lock);
  return true;
}
bool setEnabled(bool enabled) { return setMode(enabled, false); }
bool setBatteryEnabled(bool enabled) { return setMode(enabled, true); }
Status snapshot() {
  portENTER_CRITICAL(&lock);
  Status copy = state;
  const uint32_t updated = result_at;
  portEXIT_CRITICAL(&lock);
  copy.resultAgeMs = updated ? millis() - updated : UINT32_MAX;
  copy.resultTimestampMs = updated;
  if (copy.resultAgeMs > kStaleMs) { copy.personDetected = copy.batteryDetected = false; copy.boxCount = 0; }
  return copy;
}
}  // namespace vision_service
