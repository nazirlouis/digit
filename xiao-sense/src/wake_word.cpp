#include "wake_word.h"
#include "Arduino.h"
#include "app_config.h"
#include "audio_engine.h"
#include "zumo_link.h"
#include "voice_direction.h"
#include <cstring>
#include <frontend.h>
#include <frontend_util.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <tensorflow/lite/micro/micro_allocator.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_resource_variable.h>
#include <tensorflow/lite/schema/schema_generated.h>

extern "C" const uint8_t digit_model_start[];
extern "C" const uint8_t digit_model_end[];

namespace {
constexpr size_t kFeatureCount = 40;
constexpr size_t kModelArenaBytes = 64 * 1024;
constexpr size_t kProbabilityWindow = 5;
constexpr uint8_t kProbabilityCutoff = 128;
constexpr uint32_t kReactionMs = 2600;
uint8_t* modelArena = nullptr;
alignas(16) uint8_t variableArena[4096];
FrontendConfig frontendConfig = {};
FrontendState frontendState = {};
tflite::MicroMutableOpResolver<21> resolver;
tflite::MicroInterpreter* interpreter = nullptr;
uint8_t featureStride = 0, strideStep = 0, probabilityIndex = 0;
uint8_t recentProbabilities[kProbabilityWindow] = {};
uint16_t warmupFeatures = 100;
struct Frame {
  uint32_t epoch;
  uint32_t capturedAt;
  uint16_t count;
  int16_t samples[wake_word::kFrameSamples];
};
QueueHandle_t framesQueue = nullptr;
SemaphoreHandle_t modelGate = nullptr;
portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;
wake_word::Status state = {};
bool visionEnabled = false, visionRunning = false, reactionPending = false;
uint32_t epoch = 0, lastReaction = 0;
bool hasReaction = false;
bool detectionLatched = false;
bool turning = false, pendingBearing = false;
int pendingDegrees = 0;
uint32_t bearingGeneration = 0, controlGeneration = 0;

void fail(const char* error) {
  portENTER_CRITICAL(&stateLock);
  state.ready = false;
  reactionPending = false;
  snprintf(state.error, sizeof(state.error), "%s", error);
  portEXIT_CRITICAL(&stateLock);
  Serial.printf("Wake error: %s\n", error);
}

bool addModelOps() {
  return resolver.AddCallOnce() == kTfLiteOk &&
         resolver.AddVarHandle() == kTfLiteOk &&
         resolver.AddReshape() == kTfLiteOk &&
         resolver.AddReadVariable() == kTfLiteOk &&
         resolver.AddStridedSlice() == kTfLiteOk &&
         resolver.AddConcatenation() == kTfLiteOk &&
         resolver.AddAssignVariable() == kTfLiteOk &&
         resolver.AddConv2D() == kTfLiteOk &&
         resolver.AddMul() == kTfLiteOk &&
         resolver.AddAdd() == kTfLiteOk &&
         resolver.AddMean() == kTfLiteOk &&
         resolver.AddFullyConnected() == kTfLiteOk &&
         resolver.AddLogistic() == kTfLiteOk &&
         resolver.AddQuantize() == kTfLiteOk &&
         resolver.AddDepthwiseConv2D() == kTfLiteOk &&
         resolver.AddAveragePool2D() == kTfLiteOk &&
         resolver.AddMaxPool2D() == kTfLiteOk &&
         resolver.AddPad() == kTfLiteOk &&
         resolver.AddPack() == kTfLiteOk &&
         resolver.AddSplitV() == kTfLiteOk;
}

bool beginModel() {
  modelArena = static_cast<uint8_t*>(heap_caps_aligned_alloc(
      16, kModelArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!modelArena) return false;
  const size_t modelBytes = digit_model_end - digit_model_start;
  const tflite::Model* model = tflite::GetModel(digit_model_start);
  if (modelBytes != 60840 || model->version() != TFLITE_SCHEMA_VERSION ||
      !addModelOps()) {
    Serial.println("Wake model: invalid data, schema, or operations");
    return false;
  }

  auto* allocator = tflite::MicroAllocator::Create(
      variableArena, sizeof(variableArena));
  if (allocator == nullptr) {
    Serial.println("Wake model: variable allocator failed");
    return false;
  }
  auto* variables = tflite::MicroResourceVariables::Create(allocator, 20);
  if (variables == nullptr) {
    Serial.println("Wake model: state allocation failed");
    return false;
  }
  static tflite::MicroInterpreter digitInterpreter(
      model, resolver, modelArena, kModelArenaBytes, variables);
  interpreter = &digitInterpreter;
  if (interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("Wake model: tensor allocation failed");
    return false;
  }
  TfLiteTensor* input = interpreter->input(0);
  TfLiteTensor* output = interpreter->output(0);
  if (input->type != kTfLiteInt8 || input->dims->size != 3 ||
      input->dims->data[0] != 1 || input->dims->data[2] != kFeatureCount ||
      input->dims->data[1] < 1 || input->dims->data[1] > 255 ||
      output->type != kTfLiteUInt8 || output->dims->size != 2 ||
      output->dims->data[0] != 1 || output->dims->data[1] != 1) {
    Serial.println("Wake model: unexpected input or output tensor");
    return false;
  }
  featureStride = input->dims->data[1];
  Serial.printf("Wake model: %u bytes, stride=%u, arena=%u bytes\n",
                static_cast<unsigned>(modelBytes), featureStride,
                static_cast<unsigned>(interpreter->arena_used_bytes()));
  return true;
}

bool beginFrontend() {
  frontendConfig.window.size_ms = 30;
  frontendConfig.window.step_size_ms = 10;
  frontendConfig.filterbank.num_channels = kFeatureCount;
  frontendConfig.filterbank.lower_band_limit = 125.0f;
  frontendConfig.filterbank.upper_band_limit = 7500.0f;
  frontendConfig.noise_reduction.smoothing_bits = 10;
  frontendConfig.noise_reduction.even_smoothing = 0.025f;
  frontendConfig.noise_reduction.odd_smoothing = 0.06f;
  frontendConfig.noise_reduction.min_signal_remaining = 0.05f;
  frontendConfig.pcan_gain_control.enable_pcan = true;
  frontendConfig.pcan_gain_control.strength = 0.95f;
  frontendConfig.pcan_gain_control.offset = 80.0f;
  frontendConfig.pcan_gain_control.gain_bits = 21;
  frontendConfig.log_scale.enable_log = true;
  frontendConfig.log_scale.scale_shift = 6;
  if (!FrontendPopulateState(&frontendConfig, &frontendState,
                             app_config::kAudioSampleRate)) {
    Serial.println("Wake model: audio feature setup failed");
    return false;
  }
  return true;
}


void processFeature(const int8_t features[kFeatureCount], uint32_t frameEpoch) {
  memcpy(interpreter->input(0)->data.int8 + strideStep * kFeatureCount, features, kFeatureCount);
  if (++strideStep < featureStride) return;
  strideStep = 0;
  const uint32_t started = micros();
  if (interpreter->Invoke() != kTfLiteOk) { fail("Digit model inference failed"); return; }
  const uint32_t elapsed = micros() - started;
  recentProbabilities[probabilityIndex] = interpreter->output(0)->data.uint8[0];
  probabilityIndex = (probabilityIndex + 1) % kProbabilityWindow;
  portENTER_CRITICAL(&stateLock);
  state.inferenceUs = elapsed;
  portEXIT_CRITICAL(&stateLock);
  if (warmupFeatures) {
    warmupFeatures = warmupFeatures > featureStride ? warmupFeatures - featureStride : 0;
    return;
  }
  uint16_t total = 0;
  for (uint8_t p : recentProbabilities) total += p;
  if (total <= kProbabilityCutoff * kProbabilityWindow) { detectionLatched = false; return; }
  if (detectionLatched) return;
  const uint32_t now = millis();
  const float score = total / (255.0f * kProbabilityWindow);
  int degrees = 0;
  uint32_t directionGeneration = 0;
  const bool bearing = voice_direction::target(degrees, directionGeneration);
  const auto zumo = zumo_link::snapshot();
  portENTER_CRITICAL(&stateLock);
  const bool accept = state.ready && frameEpoch == epoch && !visionEnabled && !visionRunning && !turning &&
                      (!hasReaction || now - lastReaction >= kReactionMs);
  if (accept) {
    ++state.detections;
    state.lastDetectionMs = now;
    state.lastScore = score;
    lastReaction = now;
    hasReaction = true;
    reactionPending = true;
    pendingBearing = bearing;
    pendingDegrees = degrees;
    bearingGeneration = directionGeneration;
    controlGeneration = zumo.controlGeneration;
  }
  portEXIT_CRITICAL(&stateLock);
  if (accept) {
    detectionLatched = true;
    memset(recentProbabilities, 0, sizeof(recentProbabilities));
    Serial.printf("DIGIT HEARD (score=%.2f, inference=%lu ms)\n", score, (unsigned long)(elapsed / 1000));
  }
}

void task(void*) {
  Frame frame;
  uint32_t processedEpoch = UINT32_MAX;
  for (;;) {
    if (xQueueReceive(framesQueue, &frame, portMAX_DELAY) != pdTRUE) continue;
    xSemaphoreTake(modelGate, portMAX_DELAY);
    portENTER_CRITICAL(&stateLock);
    const bool allowed = state.ready && !visionEnabled && !visionRunning && frame.epoch == epoch;
    portEXIT_CRITICAL(&stateLock);
    if (allowed && millis() - frame.capturedAt <= 200) {
      if (processedEpoch != frame.epoch) {
        FrontendReset(&frontendState);
        if (interpreter->Reset() != kTfLiteOk) fail("Could not reset Digit model");
        memset(recentProbabilities, 0, sizeof(recentProbabilities));
        strideStep = probabilityIndex = 0;
        warmupFeatures = 100;
        detectionLatched = false;
        processedEpoch = frame.epoch;
      }
      size_t offset = 0;
      while (offset < frame.count && wake_word::snapshot().ready) {
        size_t consumed = 0;
        const auto output = FrontendProcessSamples(&frontendState, frame.samples + offset,
                                                   frame.count - offset, &consumed);
        if (!consumed) { fail("Digit audio frontend stalled"); break; }
        offset += consumed;
        if (!output.size) continue;
        if (output.size != kFeatureCount) { fail("Unexpected Digit feature count"); break; }
        int8_t features[kFeatureCount];
        for (size_t i = 0; i < kFeatureCount; ++i) {
          const int32_t value = (output.values[i] * 256 + 333) / 666 - 128;
          features[i] = static_cast<int8_t>(constrain<int32_t>(value, -128, 127));
        }
        processFeature(features, frame.epoch);
      }
    } else {
      processedEpoch = UINT32_MAX;  // Never join audio across a pause or a gap.
    }
    xSemaphoreGive(modelGate);
  }
}

bool react() {
  const bool chime = audioEngine.playWakeChime();
  const bool zumo = zumo_link::wake();
  Serial.printf("Wake reaction: chime=%s Zumo=%s\n", chime ? "queued" : "unavailable",
                zumo ? "queued" : "busy/disconnected");
  return chime || zumo;
}
}  // namespace

namespace wake_word {
void begin() {
  modelGate = xSemaphoreCreateMutex();
  framesQueue = xQueueCreate(20, sizeof(Frame));
  if (!modelGate || !framesQueue) { fail("Could not allocate wake synchronization"); return; }
  if (!beginModel() || !beginFrontend()) { fail("Could not initialize Digit model/frontend"); return; }
  portENTER_CRITICAL(&stateLock);
  state.ready = true;
  portEXIT_CRITICAL(&stateLock);
  if (xTaskCreatePinnedToCore(task, "digit-wake", 8192, nullptr, 2, nullptr, 0) != pdPASS) {
    fail("Could not start Digit listener"); return;
  }
  Serial.println("Wake: listening for Digit; pauses only for vision");
}

void submit(const int32_t* stereo, size_t count) {
  if (!framesQueue || !count || count > kFrameSamples) return;
  Frame frame;
  portENTER_CRITICAL(&stateLock);
  const bool active = state.ready && !visionEnabled && !visionRunning;
  frame.epoch = epoch;
  portEXIT_CRITICAL(&stateLock);
  if (!active) return;
  frame.capturedAt = millis();
  frame.count = count;
  uint16_t peak = 0;
  for (size_t i = 0; i < count; ++i) {
    const int32_t sample = constrain((stereo[2 * i + 1] >> 16) * 4, (int32_t)INT16_MIN, (int32_t)INT16_MAX);
    frame.samples[i] = static_cast<int16_t>(sample);
    peak = max(peak, static_cast<uint16_t>(sample < 0 ? -sample : sample));
  }
  const bool dropped = xQueueSend(framesQueue, &frame, 0) != pdTRUE;
  portENTER_CRITICAL(&stateLock);
  state.micPeak = peak;
  if (dropped) { ++state.droppedFrames; ++epoch; }
  portEXIT_CRITICAL(&stateLock);
}
void audioFault() { fail("Microphone unavailable; check audio status"); }
void setVisionEnabled(bool enabled) {
  portENTER_CRITICAL(&stateLock);
  if (visionEnabled != enabled) { visionEnabled = enabled; ++epoch; reactionPending = false; }
  portEXIT_CRITICAL(&stateLock);
}
void beginVisionInference() {
  if (modelGate) xSemaphoreTake(modelGate, portMAX_DELAY);
  portENTER_CRITICAL(&stateLock);
  visionRunning = true;
  ++epoch;
  reactionPending = false;
  portEXIT_CRITICAL(&stateLock);
}
void endVisionInference() {
  portENTER_CRITICAL(&stateLock);
  visionRunning = false;
  ++epoch;
  portEXIT_CRITICAL(&stateLock);
  if (modelGate) xSemaphoreGive(modelGate);
}
void poll() {
  portENTER_CRITICAL(&stateLock);
  const bool wasTurning = turning;
  const bool paused = visionEnabled || visionRunning || !state.ready;
  const bool hasBearing = pendingBearing;
  const int degrees = pendingDegrees;
  const uint32_t directionGeneration = bearingGeneration;
  const uint32_t manualGeneration = controlGeneration;
  const bool pending = reactionPending && !visionEnabled && !visionRunning && state.ready &&
                       millis() - lastReaction < 500;
  reactionPending = false;
  portEXIT_CRITICAL(&stateLock);
  if (wasTurning) {
    const bool allowed = !paused && voice_direction::accepts(directionGeneration);
    if (!allowed) zumo_link::cancelLook();
    const auto zumo = zumo_link::snapshot();
    if (zumo.lookActive) return;
    portENTER_CRITICAL(&stateLock);
    turning = false;
    lastReaction = millis();  // Also ignore the end-of-turn chime as a new wake.
    portEXIT_CRITICAL(&stateLock);
    if (allowed && zumo.controlGeneration == manualGeneration && !strcmp(zumo.lookResult, "DONE")) react();
    else Serial.printf("Voice turn ended: %s\n", zumo.lookResult);
    return;
  }
  if (pending) {
    if (hasBearing && voice_direction::accepts(directionGeneration) &&
        zumo_link::look(degrees, manualGeneration)) {
      portENTER_CRITICAL(&stateLock);
      turning = true;
      portEXIT_CRITICAL(&stateLock);
      Serial.printf("Voice turn: saved bearing %d degrees (positive=left)\n", degrees);
    } else {
      Serial.println("Voice turn skipped: disabled, uncertain direction, or Zumo unavailable/busy");
      react();
    }
  }
}
bool testReaction() {
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateLock);
  const bool allowed = !visionEnabled && !visionRunning && !turning &&
                       (!hasReaction || now - lastReaction >= kReactionMs);
  if (allowed) { lastReaction = now; hasReaction = true; }
  portEXIT_CRITICAL(&stateLock);
  return allowed && react();
}
Status snapshot() {
  portENTER_CRITICAL(&stateLock);
  Status copy = state;
  copy.pausedForVision = visionEnabled || visionRunning;
  copy.reacting = turning || (hasReaction && millis() - lastReaction < kReactionMs);
  portEXIT_CRITICAL(&stateLock);
  return copy;
}
}  // namespace wake_word
