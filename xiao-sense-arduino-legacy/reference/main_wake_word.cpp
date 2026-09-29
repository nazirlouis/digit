#include <Arduino.h>
#include <AudioTools.h>
#include <Wire.h>
#include <driver/pcnt.h>

#include <algorithm>
#include <cstring>

#include <frontend.h>
#include <frontend_util.h>
#include <tensorflow/lite/micro/micro_allocator.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_resource_variable.h>
#include <tensorflow/lite/schema/schema_generated.h>

#include "app_config.h"

extern "C" const uint8_t digit_model_start[];
extern "C" const uint8_t digit_model_end[];

namespace {

constexpr size_t kFeatureCount = 40;
constexpr size_t kSamplesPerRead = 160;  // 10 ms at 16 kHz.
constexpr size_t kModelArenaBytes = 64 * 1024;
constexpr size_t kVariableArenaBytes = 4096;
constexpr size_t kProbabilityWindow = 5;  // digit.json
constexpr uint8_t kProbabilityCutoff = 128;  // 0.5 in digit.json
constexpr uint16_t kCooldownFeatures = 100;  // One second after detection.
constexpr uint8_t kAsrChannel = 1;
constexpr int32_t kMicGain = 4;

alignas(16) uint8_t modelArena[kModelArenaBytes];
alignas(16) uint8_t variableArena[kVariableArenaBytes];
int32_t stereoSamples[kSamplesPerRead * 2];
int16_t monoSamples[kSamplesPerRead];
uint8_t recentProbabilities[kProbabilityWindow] = {};

audio_tools::I2SStream i2s;
FrontendConfig frontendConfig = {};
FrontendState frontendState = {};
tflite::MicroMutableOpResolver<21> resolver;
tflite::MicroInterpreter* interpreter = nullptr;
uint8_t featureStride = 0;
uint8_t strideStep = 0;
uint8_t probabilityIndex = 0;
uint16_t cooldownFeatures = kCooldownFeatures;
uint32_t featureCount = 0;
uint32_t lastHealthMs = 0;
uint16_t peakSinceHealth = 0;
uint8_t highestProbability = 0;
bool xiaoIsClockMaster = true;
bool listening = false;

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
      model, resolver, modelArena, sizeof(modelArena), variables);
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

bool detectClockSource() {
  pinMode(app_config::kI2sBitClockPin, INPUT);
  pinMode(app_config::kI2sWordSelectPin, INPUT);
  pcnt_config_t counter = {};
  counter.pulse_gpio_num = app_config::kI2sWordSelectPin;
  counter.ctrl_gpio_num = PCNT_PIN_NOT_USED;
  counter.lctrl_mode = PCNT_MODE_KEEP;
  counter.hctrl_mode = PCNT_MODE_KEEP;
  counter.pos_mode = PCNT_COUNT_INC;
  counter.neg_mode = PCNT_COUNT_DIS;
  counter.counter_h_lim = 30000;
  counter.counter_l_lim = -1;
  counter.unit = PCNT_UNIT_0;
  counter.channel = PCNT_CHANNEL_0;
  if (pcnt_unit_config(&counter) != ESP_OK) {
    Serial.println("I2S: clock measurement failed");
    return false;
  }
  uint32_t externalRate = 0;
  for (uint8_t attempt = 0; attempt < 10 && externalRate == 0; ++attempt) {
    pcnt_counter_pause(PCNT_UNIT_0);
    pcnt_counter_clear(PCNT_UNIT_0);
    const uint32_t started = micros();
    pcnt_counter_resume(PCNT_UNIT_0);
    delay(50);
    pcnt_counter_pause(PCNT_UNIT_0);
    int16_t edges = 0;
    pcnt_get_counter_value(PCNT_UNIT_0, &edges);
    if (edges > 0) {
      externalRate = static_cast<uint32_t>(
          static_cast<uint64_t>(edges) * 1000000ULL /
          (micros() - started));
    }
  }
  pcnt_set_mode(PCNT_UNIT_0, PCNT_CHANNEL_0, PCNT_COUNT_DIS,
                PCNT_COUNT_DIS, PCNT_MODE_KEEP, PCNT_MODE_KEEP);
  xiaoIsClockMaster = externalRate == 0;
  Serial.printf("I2S: external WS=%lu Hz, XIAO master=%s\n",
                static_cast<unsigned long>(externalRate),
                xiaoIsClockMaster ? "yes" : "no");
  return externalRate == 0 ||
         (externalRate >= 15200 && externalRate <= 16800);
}

bool beginI2s(bool receive) {
  auto config = i2s.defaultConfig(receive ? RX_MODE : TX_MODE);
  config.copyFrom(audio_tools::AudioInfo(app_config::kAudioSampleRate,
                                        app_config::kAudioChannels,
                                        app_config::kAudioBitsPerSample));
  config.pin_bck = app_config::kI2sBitClockPin;
  config.pin_ws = app_config::kI2sWordSelectPin;
  config.pin_data = receive ? app_config::kI2sDataFromRespeakerPin
                            : app_config::kI2sDataToRespeakerPin;
  config.pin_data_rx = app_config::kI2sDataFromRespeakerPin;
  config.is_master = xiaoIsClockMaster;
  config.i2s_format = audio_tools::I2S_STD_FORMAT;
  config.auto_clear = true;
  if (!i2s.begin(config)) {
    return false;
  }
  i2s.driver()->setWaitTimeReadMs(100);
  i2s.driver()->setWaitTimeWriteMs(100);
  return true;
}

bool beginMicrophone() {
  if (!detectClockSource()) {
    Serial.println("I2S: expected a 16 kHz external clock");
    return false;
  }
  if (!beginI2s(false)) {
    Serial.println("I2S: could not start initial clocks");
    return false;
  }
  static const uint8_t silence[1024] = {};
  i2s.write(silence, sizeof(silence));
  delay(250);
  Wire.begin(app_config::kI2cSdaPin, app_config::kI2cSclPin,
             app_config::kI2cFrequencyHz);
  Wire.setTimeOut(100);
  Wire.beginTransmission(app_config::kXvf3800I2cAddress);
  const uint8_t probeResult = Wire.endTransmission();
  i2s.end();
  if (probeResult != 0) {
    Serial.printf("XVF3800: I2C probe failed (%u)\n", probeResult);
    return false;
  }
  if (!beginI2s(true)) {
    Serial.println("I2S: could not start microphone input");
    return false;
  }
  Serial.println("XVF3800: microphone streaming at 16 kHz");
  return true;
}

void processFeature(const int8_t features[kFeatureCount]) {
  TfLiteTensor* input = interpreter->input(0);
  memcpy(input->data.int8 + strideStep * kFeatureCount, features,
         kFeatureCount);
  ++featureCount;
  if (++strideStep < featureStride) {
    return;
  }
  strideStep = 0;
  const uint32_t started = micros();
  if (interpreter->Invoke() != kTfLiteOk) {
    Serial.println("Wake model: inference failed");
    listening = false;
    return;
  }
  const uint32_t inferenceUs = micros() - started;
  const uint8_t probability = interpreter->output(0)->data.uint8[0];
  highestProbability = std::max(highestProbability, probability);
  recentProbabilities[probabilityIndex] = probability;
  probabilityIndex = (probabilityIndex + 1) % kProbabilityWindow;
  if (cooldownFeatures > 0) {
    cooldownFeatures = cooldownFeatures > featureStride
                           ? cooldownFeatures - featureStride
                           : 0;
    return;
  }
  uint16_t total = 0;
  for (uint8_t value : recentProbabilities) {
    total += value;
  }
  if (total > kProbabilityCutoff * kProbabilityWindow) {
    Serial.printf("DIGIT HEARD (score=%.2f, inference=%lu ms)\n",
                  static_cast<float>(total) /
                      (255.0f * kProbabilityWindow),
                  static_cast<unsigned long>(inferenceUs / 1000));
    memset(recentProbabilities, 0, sizeof(recentProbabilities));
    cooldownFeatures = kCooldownFeatures;
  }
}

void processAudio(const int16_t* samples, size_t count) {
  size_t offset = 0;
  while (offset < count && listening) {
    size_t consumed = 0;
    const FrontendOutput result = FrontendProcessSamples(
        &frontendState, samples + offset, count - offset, &consumed);
    if (consumed == 0) {
      Serial.println("Wake model: audio feature processing stalled");
      listening = false;
      return;
    }
    offset += consumed;
    if (result.size == 0) {
      continue;
    }
    if (result.size != kFeatureCount) {
      Serial.println("Wake model: unexpected feature count");
      listening = false;
      return;
    }
    int8_t features[kFeatureCount];
    for (size_t i = 0; i < kFeatureCount; ++i) {
      int32_t scaled = (result.values[i] * 256 + 333) / 666 - 128;
      features[i] = static_cast<int8_t>(
          std::max<int32_t>(-128, std::min<int32_t>(127, scaled)));
    }
    processFeature(features);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("Digit wake-word listener starting");
  if (!beginModel() || !beginFrontend() || !beginMicrophone()) {
    Serial.println("Digit wake-word listener stopped during setup");
    return;
  }
  listening = true;
  lastHealthMs = millis();
  Serial.println("Listening for Digit...");
}

void loop() {
  if (!listening) {
    delay(1000);
    return;
  }
  const size_t received = i2s.readBytes(
      reinterpret_cast<uint8_t*>(stereoSamples), sizeof(stereoSamples));
  if (received == 0) {
    Serial.println("I2S: no microphone samples received");
    delay(100);
    return;
  }
  if (received % (2 * sizeof(int32_t)) != 0) {
    Serial.println("I2S: incomplete stereo frame");
    listening = false;
    return;
  }
  const size_t frames = received / (2 * sizeof(int32_t));
  for (size_t i = 0; i < frames; ++i) {
    const int32_t raw = stereoSamples[2 * i + kAsrChannel] >> 16;
    const int32_t amplified = std::max<int32_t>(
        INT16_MIN, std::min<int32_t>(INT16_MAX, raw * kMicGain));
    monoSamples[i] = static_cast<int16_t>(amplified);
    peakSinceHealth = std::max<uint16_t>(
        peakSinceHealth, amplified < 0 ? -amplified : amplified);
  }
  processAudio(monoSamples, frames);
  if (millis() - lastHealthMs >= 5000) {
    lastHealthMs = millis();
    Serial.printf("Listening: features=%lu, mic peak=%u, max score=%.2f\n",
                  static_cast<unsigned long>(featureCount), peakSinceHealth,
                  highestProbability / 255.0f);
    peakSinceHealth = 0;
    highestProbability = 0;
  }
}
