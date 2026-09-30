#include "vision_service.h"

#include <cstring>
#include "Arduino.h"
#include "camera_service.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_partition.h"
#include "pedestrian_detect.hpp"
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
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
vision_service::Status state = {};
uint32_t result_at = 0;
uint32_t generation = 0;
uint8_t* jpeg_copy = nullptr;
uint8_t* rgb565 = nullptr;
PedestrianDetect* detector = nullptr;
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
bool model_present() {
  const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                               static_cast<esp_partition_subtype_t>(0x40),
                                                               "pedestrian_det");
  if (!partition) return false;
  uint8_t header[16] = {};
  if (esp_partition_read(partition, 0, header, sizeof(header)) != ESP_OK) return false;
  for (uint8_t byte : header) if (byte != 0xff) return true;
  return false;
}
void task(void*) {
  for (;;) {
    uint32_t accepted_interval_ms = 0;
    portENTER_CRITICAL(&lock);
    const bool enabled = state.enabled;
    const uint32_t current_generation = generation;
    portEXIT_CRITICAL(&lock);
    if (!enabled || speech_commands::snapshot().listening) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    const uint32_t started = millis();
    camera_fb_t* frame = esp_camera_fb_get();
    const bool frame_ok = frame && frame->format == PIXFORMAT_JPEG && frame->width == 640 &&
                          frame->height == 480 && frame->len > 0 && frame->len <= kJpegCapacity;
    size_t jpeg_length = 0;
    if (frame_ok) { jpeg_length = frame->len; memcpy(jpeg_copy, frame->buf, jpeg_length); }
    if (frame) esp_camera_fb_return(frame);
    if (!frame_ok || !decode_frame(jpeg_length)) {
      fail(frame_ok ? decode_error : "Camera frame unavailable for pedestrian detection");
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
    auto& results = detector->run(image);
    wake_word::endVisionInference();
    const uint32_t inference_ms = millis() - started - decode_ms;
    vision_service::Status::Box boxes[5] = {};
    uint8_t count = 0;
    int highest = 0;
    for (const auto& result : results) {
      if (result.box.size() < 4 || result.score < kScoreThreshold) continue;
      const int score = std::min(100, static_cast<int>(result.score * 100 + 0.5f));
      highest = std::max(highest, score);
      if (count < 5) boxes[count++] = {result.box[0], result.box[1], result.box[2], result.box[3],
                                       static_cast<uint8_t>(score)};
    }
    portENTER_CRITICAL(&lock);
    const bool accepted = state.enabled && generation == current_generation;
    if (accepted) {
      state.hasResult = true;
      state.personDetected = count > 0;
      state.personScorePercent = static_cast<uint8_t>(highest);
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
    if (accepted) Serial.printf("Vision: capture+decode=%lu ms model=%lu ms total=%lu ms interval=%lu ms people=%u best=%d%%\n",
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
  if (!model_present()) { fail("Pedestrian model partition is missing or blank"); return; }
  jpeg_copy = static_cast<uint8_t*>(heap_caps_malloc(kJpegCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  rgb565 = static_cast<uint8_t*>(heap_caps_aligned_alloc(
      16, kWidth * kHeight * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!jpeg_copy || !rgb565) { fail("Not enough PSRAM for person detection"); return; }
  detector = new PedestrianDetect(PedestrianDetect::PICO_S8_V1, true);
  detector->set_score_thr(kScoreThreshold);
  if (xTaskCreatePinnedToCore(task, "pedestrian", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
    fail("Could not start person detection task"); return;
  }
  portENTER_CRITICAL(&lock);
  state.ready = true;
  state.imageWidth = kWidth;
  state.imageHeight = kHeight;
  state.error[0] = '\0';
  portEXIT_CRITICAL(&lock);
  Serial.println("Vision: ready; press Start person detection to load the model");
}
bool setEnabled(bool enabled) {
  portENTER_CRITICAL(&lock);
  if (!state.ready) { portEXIT_CRITICAL(&lock); return false; }
  state.enabled = enabled;
  state.hasResult = false;
  state.personDetected = false;
  state.boxCount = 0;
  state.intervalMs = 0;
  result_at = 0;
  ++generation;
  wake_word::setVisionEnabled(enabled);
  portEXIT_CRITICAL(&lock);
  return true;
}
Status snapshot() {
  portENTER_CRITICAL(&lock);
  Status copy = state;
  const uint32_t updated = result_at;
  portEXIT_CRITICAL(&lock);
  copy.resultAgeMs = updated ? millis() - updated : UINT32_MAX;
  copy.resultTimestampMs = updated;
  if (copy.resultAgeMs > kStaleMs) { copy.personDetected = false; copy.boxCount = 0; }
  return copy;
}
}  // namespace vision_service
