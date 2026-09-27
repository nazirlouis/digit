#include "vision_service.h"

#include <Arduino.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_jpg_decode.h>
#include <img_converters.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/schema/schema_generated.h>

#include "camera_service.h"

extern "C" const uint8_t person_model_start[];
extern "C" const uint8_t person_model_end[];

namespace {

constexpr size_t kArenaBytes = 160 * 1024;
constexpr size_t kJpegBytes = 256 * 1024;
constexpr int kDecodedWidth = 160;
constexpr int kDecodedHeight = 120;
constexpr int kInputWidth = 96;
constexpr int kInputHeight = 96;
constexpr uint32_t kSamplePeriodMs = 1000;
constexpr uint32_t kStaleAfterMs = 5000;
constexpr uint8_t kPersonThresholdPercent = 55;

portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
vision_service::Status state = {false, false, false, false, 0, 0, 0,
                                "Vision has not started"};
uint32_t resultAtMs = 0;
uint32_t controlGeneration = 0;
uint8_t positiveCount = 0;
uint8_t negativeCount = 0;

uint8_t* arena = nullptr;
uint8_t* jpegCopy = nullptr;
uint8_t* decoded = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
tflite::MicroMutableOpResolver<5> resolver;

void setError(const char* message) {
  portENTER_CRITICAL(&statusMux);
  snprintf(state.error, sizeof(state.error), "%s", message);
  portEXIT_CRITICAL(&statusMux);
  Serial.printf("Vision: %s\n", message);
}

bool prepareModel() {
  const tflite::Model* model = tflite::GetModel(person_model_start);
  if (person_model_end - person_model_start != 300568 ||
      model->version() != TFLITE_SCHEMA_VERSION) {
    setError("Person model data or schema is invalid");
    return false;
  }
  if (resolver.AddAveragePool2D() != kTfLiteOk ||
      resolver.AddConv2D() != kTfLiteOk ||
      resolver.AddDepthwiseConv2D() != kTfLiteOk ||
      resolver.AddReshape() != kTfLiteOk ||
      resolver.AddSoftmax() != kTfLiteOk) {
    setError("Person model operations are unavailable");
    return false;
  }

  arena = static_cast<uint8_t*>(heap_caps_malloc(kArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  jpegCopy = static_cast<uint8_t*>(heap_caps_malloc(kJpegBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  decoded = static_cast<uint8_t*>(heap_caps_malloc(kDecodedWidth * kDecodedHeight * 2,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!arena || !jpegCopy || !decoded) {
    setError("Not enough PSRAM for person detection");
    return false;
  }

  static tflite::MicroInterpreter personInterpreter(model, resolver, arena,
                                                      kArenaBytes);
  interpreter = &personInterpreter;
  if (interpreter->AllocateTensors() != kTfLiteOk) {
    setError("Person model tensor allocation failed");
    return false;
  }
  const TfLiteTensor* input = interpreter->input(0);
  const TfLiteTensor* output = interpreter->output(0);
  if (input->type != kTfLiteInt8 || input->dims->size != 4 ||
      input->dims->data[0] != 1 || input->dims->data[1] != kInputHeight ||
      input->dims->data[2] != kInputWidth || input->dims->data[3] != 1 ||
      output->type != kTfLiteInt8 || output->dims->size != 2 ||
      output->dims->data[0] != 1 || output->dims->data[1] != 2) {
    setError("Person model has unexpected input or output shape");
    return false;
  }
  Serial.printf("Vision: model ready; arena used=%u bytes, PSRAM free=%u bytes\n",
                static_cast<unsigned>(interpreter->arena_used_bytes()),
                static_cast<unsigned>(ESP.getFreePsram()));
  return true;
}

bool captureAndPrepare() {
  camera_fb_t* frame = esp_camera_fb_get();
  if (!frame) {
    setError("Camera frame unavailable for detection");
    return false;
  }
  const bool valid = frame->format == PIXFORMAT_JPEG && frame->width == 640 &&
                     frame->height == 480 && frame->len > 0 && frame->len <= kJpegBytes;
  const size_t length = valid ? frame->len : 0;
  if (valid) {
    memcpy(jpegCopy, frame->buf, length);
  }
  esp_camera_fb_return(frame);
  if (!valid) {
    setError("Camera frame format or size is unsupported");
    return false;
  }
  if (!jpg2rgb565(jpegCopy, length, decoded, JPG_SCALE_4X)) {
    setError("Could not decode camera frame for detection");
    return false;
  }

  int8_t* input = interpreter->input(0)->data.int8;
  for (int y = 0; y < kInputHeight; ++y) {
    const int sourceY = y * kDecodedHeight / kInputHeight;
    for (int x = 0; x < kInputWidth; ++x) {
      const int sourceX = x * kDecodedWidth / kInputWidth;
      const size_t offset = 2 * (sourceY * kDecodedWidth + sourceX);
      const uint16_t pixel = static_cast<uint16_t>(decoded[offset]) |
                             (static_cast<uint16_t>(decoded[offset + 1]) << 8);
      const int red = ((pixel >> 11) & 0x1f) * 255 / 31;
      const int green = ((pixel >> 5) & 0x3f) * 255 / 63;
      const int blue = (pixel & 0x1f) * 255 / 31;
      const int gray = (77 * red + 150 * green + 29 * blue) >> 8;
      input[y * kInputWidth + x] = static_cast<int8_t>(gray - 128);
    }
  }
  return true;
}

void detectionTask(void*) {
  uint32_t lastGeneration = 0;
  for (;;) {
    portENTER_CRITICAL(&statusMux);
    const bool enabled = state.enabled;
    const uint32_t generation = controlGeneration;
    portEXIT_CRITICAL(&statusMux);
    if (!enabled) {
      positiveCount = negativeCount = 0;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (generation != lastGeneration) {
      positiveCount = negativeCount = 0;
      lastGeneration = generation;
    }
    const uint32_t startedAt = millis();
    if (captureAndPrepare()) {
      const uint32_t inferenceStartedAt = millis();
      const uint32_t preparationMs = inferenceStartedAt - startedAt;
      if (interpreter->Invoke() == kTfLiteOk) {
        const uint32_t inferenceMs = millis() - inferenceStartedAt;
        const TfLiteTensor* output = interpreter->output(0);
        // Class 0 is "no person"; class 1 is "person". Output scale is 1/256
        // with a zero point of -128 for this bundled model.
        const int rawScore = output->data.int8[1];
        const uint8_t score = static_cast<uint8_t>((rawScore + 128) * 100 / 255);
        const uint8_t noPersonScore = static_cast<uint8_t>(
            (static_cast<int>(output->data.int8[0]) + 128) * 100 / 255);
        const bool positive = score >= kPersonThresholdPercent &&
                              output->data.int8[1] > output->data.int8[0];
        positiveCount = positive ? (positiveCount < 2 ? positiveCount + 1 : 2) : 0;
        negativeCount = positive ? 0 : (negativeCount < 2 ? negativeCount + 1 : 2);

        portENTER_CRITICAL(&statusMux);
        const bool stillEnabled = state.enabled && controlGeneration == generation;
        if (stillEnabled) {
          state.hasResult = true;
          state.personScorePercent = score;
          state.inferenceMs = inferenceMs;
          if (positiveCount >= 2) state.personDetected = true;
          if (negativeCount >= 2) state.personDetected = false;
          state.error[0] = '\0';
          resultAtMs = millis();
        }
        const bool badgeVisible = state.personDetected;
        portEXIT_CRITICAL(&statusMux);
        if (!stillEnabled) {
          positiveCount = negativeCount = 0;
          continue;
        }
        Serial.printf(
            "Vision: prep=%lu ms model=%lu ms total=%lu ms "
            "person=%u%% no_person=%u%% badge=%s\n",
            static_cast<unsigned long>(preparationMs),
            static_cast<unsigned long>(inferenceMs),
            static_cast<unsigned long>(millis() - startedAt), score,
            noPersonScore, badgeVisible ? "on" : "off");
        if (inferenceMs > 2500) {
          portENTER_CRITICAL(&statusMux);
          state.ready = false;
          state.enabled = false;
          state.personDetected = false;
          ++controlGeneration;
          portEXIT_CRITICAL(&statusMux);
          setError("Person detection is too slow alongside the dashboard");
          vTaskDelete(nullptr);
        }
      } else {
        positiveCount = negativeCount = 0;
        Serial.printf("Vision: prep=%lu ms model=%lu ms (inference failed)\n",
                      static_cast<unsigned long>(preparationMs),
                      static_cast<unsigned long>(millis() - inferenceStartedAt));
        setError("Person model inference failed");
      }
    } else {
      positiveCount = negativeCount = 0;
    }
    const uint32_t elapsed = millis() - startedAt;
    vTaskDelay(pdMS_TO_TICKS(elapsed < kSamplePeriodMs ? kSamplePeriodMs - elapsed : 1));
  }
}

}  // namespace

namespace vision_service {

void begin() {
  if (!camera_service::ready()) {
    setError("Camera is unavailable for person detection");
    return;
  }
  if (!prepareModel()) return;
  if (xTaskCreatePinnedToCore(detectionTask, "personDetect", 8192, nullptr,
                              1, nullptr, 0) != pdPASS) {
    setError("Could not start person detection task");
    return;
  }
  portENTER_CRITICAL(&statusMux);
  state.ready = true;
  state.error[0] = '\0';
  portEXIT_CRITICAL(&statusMux);
  Serial.println("Vision: ready; detection is off until started from the dashboard");
}

bool setEnabled(bool enabled) {
  portENTER_CRITICAL(&statusMux);
  if (!state.ready) {
    portEXIT_CRITICAL(&statusMux);
    return false;
  }
  if (state.enabled != enabled) {
    state.enabled = enabled;
    ++controlGeneration;
    state.hasResult = false;
    state.personDetected = false;
    state.personScorePercent = 0;
    state.resultAgeMs = 0;
    state.inferenceMs = 0;
    state.error[0] = '\0';
    resultAtMs = 0;
  }
  portEXIT_CRITICAL(&statusMux);
  Serial.printf("Vision: detection %s\n", enabled ? "started" : "stopped");
  return true;
}

Status snapshot() {
  portENTER_CRITICAL(&statusMux);
  Status copy = state;
  const uint32_t lastResult = resultAtMs;
  portEXIT_CRITICAL(&statusMux);
  copy.resultAgeMs = copy.hasResult ? millis() - lastResult : 0;
  if (!copy.enabled || copy.resultAgeMs > kStaleAfterMs ||
      copy.error[0] != '\0') {
    copy.personDetected = false;
  }
  return copy;
}

}  // namespace vision_service
