#include "camera_service.h"

#include <Arduino.h>
#include <esp_camera.h>
#include <freertos/semphr.h>
#include <cstring>

namespace {

bool cameraReady = false;
char cameraError[96] = "Camera has not been initialized";
SemaphoreHandle_t frameMutex = nullptr;
uint8_t* latestJpeg = nullptr;
camera_service::Frame latestFrame;

void captureTask(void*) {
  for (;;) {
    camera_fb_t* frame = esp_camera_fb_get();
    if (frame && frame->format == PIXFORMAT_JPEG && frame->len &&
        frame->len <= camera_service::kMaxJpegBytes &&
        xSemaphoreTake(frameMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      memcpy(latestJpeg, frame->buf, frame->len);
      latestFrame = {frame->len,
        uint32_t(uint64_t(frame->timestamp.tv_sec) * 1000 + frame->timestamp.tv_usec / 1000),
        static_cast<uint16_t>(frame->width), static_cast<uint16_t>(frame->height)};
      xSemaphoreGive(frameMutex);
    }
    if (frame) esp_camera_fb_return(frame);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// XIAO ESP32S3 Sense camera connector pins. These are shared by the OV2640,
// OV3660, and OV5640 camera modules supported by the Sense expansion board.
constexpr int kPinPwdn = -1;
constexpr int kPinReset = -1;
constexpr int kPinXclk = 10;
constexpr int kPinSiod = 40;
constexpr int kPinSioc = 39;
constexpr int kPinY9 = 48;
constexpr int kPinY8 = 11;
constexpr int kPinY7 = 12;
constexpr int kPinY6 = 14;
constexpr int kPinY5 = 16;
constexpr int kPinY4 = 18;
constexpr int kPinY3 = 17;
constexpr int kPinY2 = 15;
constexpr int kPinVsync = 38;
constexpr int kPinHref = 47;
constexpr int kPinPclk = 13;

void fail(const char* message) {
  cameraReady = false;
  snprintf(cameraError, sizeof(cameraError), "%s", message);
  Serial.printf("Camera: %s\n", cameraError);
}

}  // namespace

namespace camera_service {

bool begin() {
  if (cameraReady) return true;
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
    fail("PSRAM was not detected");
    return false;
  }

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = kPinY2;
  config.pin_d1 = kPinY3;
  config.pin_d2 = kPinY4;
  config.pin_d3 = kPinY5;
  config.pin_d4 = kPinY6;
  config.pin_d5 = kPinY7;
  config.pin_d6 = kPinY8;
  config.pin_d7 = kPinY9;
  config.pin_xclk = kPinXclk;
  config.pin_pclk = kPinPclk;
  config.pin_vsync = kPinVsync;
  config.pin_href = kPinHref;
  config.pin_sccb_sda = kPinSiod;
  config.pin_sccb_scl = kPinSioc;
  config.pin_pwdn = kPinPwdn;
  config.pin_reset = kPinReset;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_VGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.jpeg_buffer_size = kMaxJpegBytes;
  config.fb_count = 2;

  const esp_err_t result = esp_camera_init(&config);
  if (result != ESP_OK) {
    snprintf(cameraError, sizeof(cameraError),
             "Camera initialization failed (0x%04x)",
             static_cast<unsigned int>(result));
    cameraReady = false;
    Serial.printf("Camera: %s\n", cameraError);
    return false;
  }

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    if (sensor->id.PID == OV3660_PID) {
      sensor->set_vflip(sensor, 1);
      sensor->set_brightness(sensor, 1);
      sensor->set_saturation(sensor, -2);
    }
    // Turn the current camera orientation upside down for both the live
    // stream and vision frames. The OV3660 correction above is included in
    // the current orientation before applying this 180-degree turn.
    const int vertical = sensor->status.vflip ? 0 : 1;
    const int horizontal = sensor->status.hmirror ? 0 : 1;
    if (sensor->set_vflip(sensor, vertical) != 0 ||
        sensor->set_hmirror(sensor, horizontal) != 0) {
      Serial.println("Camera: could not apply 180-degree rotation");
    } else {
      Serial.println("Camera: image rotated 180 degrees");
    }
  }

  frameMutex = xSemaphoreCreateMutex();
  latestJpeg = static_cast<uint8_t*>(heap_caps_malloc(kMaxJpegBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!frameMutex || !latestJpeg ||
      xTaskCreatePinnedToCore(captureTask, "camera-frames", 4096, nullptr, 1, nullptr, 1) != pdPASS) {
    if (frameMutex) vSemaphoreDelete(frameMutex);
    frameMutex = nullptr;
    heap_caps_free(latestJpeg); latestJpeg = nullptr;
    esp_camera_deinit();
    fail("Could not allocate shared camera frames");
    return false;
  }
  cameraError[0] = '\0';
  cameraReady = true;
  Serial.println("Camera: ready at 640x480 JPEG");
  return true;
}

bool ready() { return cameraReady; }

const char* lastError() { return cameraError; }

bool copyLatestJpeg(uint8_t* destination, size_t capacity, Frame* frame) {
  if (frame) *frame = {};
  if (!cameraReady || !destination || !frame || !frameMutex ||
      xSemaphoreTake(frameMutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
  const bool valid = latestFrame.size && latestFrame.size <= capacity &&
                     millis() - latestFrame.timestampMs <= 500;
  if (valid) {
    memcpy(destination, latestJpeg, latestFrame.size);
    *frame = latestFrame;
  }
  xSemaphoreGive(frameMutex);
  return valid;
}

}  // namespace camera_service
