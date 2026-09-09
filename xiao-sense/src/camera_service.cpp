#include "camera_service.h"

#include <Arduino.h>
#include <esp_camera.h>

namespace {

bool cameraReady = false;
char cameraError[96] = "Camera has not been initialized";

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
  if (!psramFound()) {
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
  if (sensor != nullptr && sensor->id.PID == OV3660_PID) {
    sensor->set_vflip(sensor, 1);
    sensor->set_brightness(sensor, 1);
    sensor->set_saturation(sensor, -2);
  }

  cameraError[0] = '\0';
  cameraReady = true;
  Serial.println("Camera: ready at 640x480 JPEG");
  return true;
}

bool ready() { return cameraReady; }

const char* lastError() { return cameraError; }

}  // namespace camera_service
