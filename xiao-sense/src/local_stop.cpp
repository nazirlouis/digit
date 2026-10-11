#include "local_stop.h"
#include "local_stop_gate.h"
#include "Arduino.h"
#include "app_config.h"
#include "robot_actions.h"
#include "speech_commands.h"
#include "wake_word.h"
#include "zumo_link.h"
#include <cstring>
#include <frontend.h>
#include <frontend_util.h>
#include <freertos/queue.h>
#include <freertos/idf_additions.h>
#include <tensorflow/lite/micro/micro_allocator.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_resource_variable.h>
#include <tensorflow/lite/schema/schema_generated.h>

extern "C" const uint8_t stop_model_start[];
extern "C" const uint8_t stop_model_end[];

namespace {
constexpr size_t kFeatures = 40, kArenaBytes = 64 * 1024, kWindow = 5;
constexpr uint8_t kCutoff = 128;  // Upstream stop.json: 0.5, five scores.
constexpr size_t kQueueFrames = 70;
struct Frame {
  local_stop::Tag tag;
  uint16_t count;
  int16_t samples[wake_word::kFrameSamples];
};
portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;
local_stop::Status state{};
local_stop::Gate gate;
QueueHandle_t framesQueue = nullptr;
FrontendConfig frontendConfig{};
FrontendState frontendState{};
tflite::MicroMutableOpResolver<21> resolver;
tflite::MicroInterpreter* interpreter = nullptr;
uint8_t* arena = nullptr;
alignas(16) uint8_t variableArena[4096];
uint8_t featureStride = 0, strideStep = 0, probabilityIndex = 0;
uint8_t probabilities[kWindow]{};
uint16_t warmupFeatures = 100;
bool latched = false, pending = false;
local_stop::Tag pendingTag{};
float pendingScore = 0;

void fail(const char* message) {
  portENTER_CRITICAL(&stateLock);
  state.ready = false;
  state.active = false;
  pending = false;
  gate.invalidate();
  snprintf(state.error, sizeof(state.error), "%s", message);
  portEXIT_CRITICAL(&stateLock);
  Serial.printf("Local Stop error: %s\n", message);
}

bool addOps() {
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
  arena = static_cast<uint8_t*>(heap_caps_aligned_alloc(
      16, kArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!arena || stop_model_end - stop_model_start != 45744 || !addOps()) return false;
  const tflite::Model* model = tflite::GetModel(stop_model_start);
  if (model->version() != TFLITE_SCHEMA_VERSION) return false;
  auto* allocator = tflite::MicroAllocator::Create(variableArena, sizeof(variableArena));
  if (!allocator) return false;
  auto* variables = tflite::MicroResourceVariables::Create(allocator, 20);
  if (!variables) return false;
  static tflite::MicroInterpreter stopInterpreter(model, resolver, arena, kArenaBytes, variables);
  interpreter = &stopInterpreter;
  if (interpreter->AllocateTensors() != kTfLiteOk) return false;
  const auto* input = interpreter->input(0);
  const auto* output = interpreter->output(0);
  if (input->type != kTfLiteInt8 || input->dims->size != 3 ||
      input->dims->data[0] != 1 || input->dims->data[2] != kFeatures ||
      input->dims->data[1] < 1 || input->dims->data[1] > 255 ||
      output->type != kTfLiteUInt8 || output->dims->size != 2 ||
      output->dims->data[0] != 1 || output->dims->data[1] != 1) return false;
  featureStride = input->dims->data[1];
  Serial.printf("Local Stop: model=45744 bytes, stride=%u, arena=%u bytes\n",
                featureStride, static_cast<unsigned>(interpreter->arena_used_bytes()));
  return true;
}

bool beginFrontend() {
  frontendConfig.window.size_ms = 30;
  frontendConfig.window.step_size_ms = 10;
  frontendConfig.filterbank.num_channels = kFeatures;
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
  return FrontendPopulateState(&frontendConfig, &frontendState, app_config::kAudioSampleRate);
}

bool current(local_stop::Tag tag) {
  portENTER_CRITICAL(&stateLock);
  const bool allowed = state.ready && gate.accepts(tag, millis()) &&
      (gate.manual() || gate.generation() == robot_actions::generation());
  portEXIT_CRITICAL(&stateLock);
  return allowed;
}

void feature(const int8_t data[kFeatures], local_stop::Tag tag) {
  memcpy(interpreter->input(0)->data.int8 + strideStep * kFeatures, data, kFeatures);
  if (++strideStep < featureStride) return;
  strideStep = 0;
  const uint32_t started = micros();
  if (interpreter->Invoke() != kTfLiteOk) { fail("Stop inference failed"); return; }
  const uint32_t elapsed = micros() - started;
  probabilities[probabilityIndex] = interpreter->output(0)->data.uint8[0];
  probabilityIndex = (probabilityIndex + 1) % kWindow;
  portENTER_CRITICAL(&stateLock);
  state.inferenceUs = elapsed;
  portEXIT_CRITICAL(&stateLock);
  if (warmupFeatures) {
    warmupFeatures = warmupFeatures > featureStride ? warmupFeatures - featureStride : 0;
    return;
  }
  uint16_t sum = 0;
  for (uint8_t value : probabilities) sum += value;
  if (sum <= kCutoff * kWindow) { latched = false; return; }
  if (latched) return;
  portENTER_CRITICAL(&stateLock);
  const bool allowed = state.ready && gate.accepts(tag, millis()) &&
      (gate.manual() || gate.generation() == robot_actions::generation());
  if (allowed) {
    pending = true;
    pendingTag = tag;
    pendingScore = sum / (255.0f * kWindow);
    latched = true;
  }
  portEXIT_CRITICAL(&stateLock);
}

void task(void*) {
  Frame frame{};
  uint32_t processedEpoch = UINT32_MAX;
  for (;;) {
    if (xQueueReceive(framesQueue, &frame, portMAX_DELAY) != pdTRUE) continue;
    // Stop uses its own frontend and interpreter, sharing only compute access.
    wake_word::beginInference();
    if (current(frame.tag)) {
      if (processedEpoch != frame.tag.epoch) {
        FrontendReset(&frontendState);
        if (interpreter->Reset() != kTfLiteOk) fail("Could not reset Stop model");
        memset(probabilities, 0, sizeof(probabilities));
        strideStep = probabilityIndex = 0;
        warmupFeatures = 100;
        latched = false;
        processedEpoch = frame.tag.epoch;
      }
      size_t offset = 0;
      while (offset < frame.count && current(frame.tag)) {
        size_t consumed = 0;
        const auto output = FrontendProcessSamples(&frontendState, frame.samples + offset,
                                                   frame.count - offset, &consumed);
        if (!consumed) { fail("Stop audio frontend stalled"); break; }
        offset += consumed;
        if (!output.size) continue;
        if (output.size != kFeatures) { fail("Unexpected Stop feature count"); break; }
        int8_t features[kFeatures];
        for (size_t i = 0; i < kFeatures; ++i) {
          const int32_t value = (output.values[i] * 256 + 333) / 666 - 128;
          features[i] = static_cast<int8_t>(constrain<int32_t>(value, -128, 127));
        }
        feature(features, frame.tag);
      }
    } else processedEpoch = UINT32_MAX;
    wake_word::endInference();
  }
}
}  // namespace

namespace local_stop {
void begin() {
  static StaticQueue_t queueControl;
  auto* storage = static_cast<uint8_t*>(heap_caps_malloc(
      kQueueFrames * sizeof(Frame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (storage) framesQueue = xQueueCreateStatic(kQueueFrames, sizeof(Frame), storage, &queueControl);
  if (!framesQueue || !beginModel() || !beginFrontend()) {
    fail("Could not initialize local Stop model/frontend"); return;
  }
  portENTER_CRITICAL(&stateLock);
  state.ready = true;
  portEXIT_CRITICAL(&stateLock);
  // No flash/NVS/network operations may be added to this compute-only worker.
  if (xTaskCreatePinnedToCoreWithCaps(task, "digit-stop", 8192, nullptr, 3, nullptr, 0,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    fail("Could not start local Stop listener"); return;
  }
}

void submit(const int32_t* stereo, size_t count) {
  if (!framesQueue || !stereo || !count || count > wake_word::kFrameSamples) return;
  Frame frame{};
  portENTER_CRITICAL(&stateLock);
  const bool allowed = state.ready && gate.active();
  frame.tag = gate.tag(millis());
  portEXIT_CRITICAL(&stateLock);
  if (!allowed) return;
  frame.count = count;
  // The ReSpeaker's right ASR beam already has its configured hardware AEC.
  // Keep listening during robot cues; software suppression would lose Stop.
  for (size_t i = 0; i < count; ++i) frame.samples[i] = static_cast<int16_t>(stereo[2 * i + 1] >> 16);
  if (xQueueSend(framesQueue, &frame, 0) != pdTRUE) {
    portENTER_CRITICAL(&stateLock);
    ++state.droppedFrames;
    gate.invalidate();
    pending = false;
    portEXIT_CRITICAL(&stateLock);
  }
}

void poll() {
  robot_actions::Guard control;
  const auto actions = robot_actions::snapshot();
  const auto speech = speech_commands::activity();
  const auto zumo = zumo_link::snapshot();
  const auto wake = wake_word::snapshot();
  const bool manual = zumo.manualDriving;
  // Recognition ends listening before its command reaches the main loop.
  // Preserve a detected Stop through that handoff so it cancels dispatch first.
  const bool active = actions.active || speech.liveActive || speech.listening || speech.pendingAction ||
                      manual || wake.reacting || zumo.lookActive || zumo.motionActive;
  portENTER_CRITICAL(&stateLock);
  if (gate.update(active, manual, actions.generation)) pending = false;
  state.active = state.ready && active;
  const bool stop = state.ready && pending && gate.accepts(pendingTag, millis());
  if (stop) { ++state.detections; state.lastScore = pendingScore; }
  pending = false;
  portEXIT_CRITICAL(&stateLock);
  if (stop) robot_actions::stop("Local voice Stop");
}

void cancel() {
  portENTER_CRITICAL(&stateLock);
  gate.invalidate();
  pending = false;
  portEXIT_CRITICAL(&stateLock);
}
void audioFault() { fail("Microphone unavailable; local Stop disabled"); }
Status snapshot() {
  portENTER_CRITICAL(&stateLock);
  const Status copy = state;
  portEXIT_CRITICAL(&stateLock);
  return copy;
}
}  // namespace local_stop
