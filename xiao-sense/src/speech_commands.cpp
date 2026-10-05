#include "speech_commands.h"
#include "speech_pipeline.h"
#include "speech_phrases.h"
#include "robot_actions.h"
#include "wake_word.h"
#include "audio_engine.h"
#include "Arduino.h"
#include <cstring>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include <model_path.h>
#include <esp_partition.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

namespace {
constexpr uint32_t kVoiceTailMs = 120, kVoiceWaitTimeoutMs = 2500;
constexpr uint32_t kRecognitionMs = 6000; // Same model-controlled timeout as the demo.
constexpr uint32_t kStallMs = 10000; // Fault watchdog, not the recognition window.
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
speech_commands::Status state{};
esp_mn_iface_t* model = nullptr;
model_iface_data_t* data = nullptr;
constexpr int kModelSamples = 512;
struct Chunk { speech_pipeline::Tag tag; int16_t samples[kModelSamples]; };
QueueHandle_t inputQueue = nullptr, outputQueue = nullptr;
int16_t feedSamples[kModelSamples]{};
uint32_t session = 1, stream = 1, controlGeneration = 0, opensAt = 0;
bool waitingForVoice = false;
int pendingCommand = 0;
uint32_t pendingGeneration = 0;
constexpr size_t kCaptureSamples = 16000 * 7;
SemaphoreHandle_t captureLock = nullptr;
int16_t* capturedAudio = nullptr;
size_t capturedCount = 0;
uint32_t capturedSession = 0;
bool capturedComplete = false;
speech_commands::AudioTestStatus testState{};
int16_t* testPending = nullptr;
size_t testCount = 0;

bool captureInput(const Chunk& chunk, bool first) {
  // Only diagnostic samples may be skipped if an HTTP copy overlaps a wake.
  // Microphone capture, inference, cancellation and dispatch never wait here.
  if (!capturedAudio || xSemaphoreTake(captureLock, 0) != pdTRUE) return false;
  if (first) { capturedSession = chunk.tag.session; capturedCount = 0; capturedComplete = false; }
  const bool fits = capturedSession == chunk.tag.session && capturedCount + kModelSamples <= kCaptureSamples;
  if (fits) {
    memcpy(capturedAudio + capturedCount, chunk.samples, sizeof(chunk.samples));
    capturedCount += kModelSamples;
  }
  xSemaphoreGive(captureLock);
  return fits;
}

void finishCapture(uint32_t expected, bool intact) {
  if (!captureLock || xSemaphoreTake(captureLock, 0) != pdTRUE) return;
  if (capturedSession == expected) capturedComplete = intact;
  xSemaphoreGive(captureLock);
}

void runAudioTest(int16_t* samples, size_t count, uint8_t gain) {
  alignas(16) int16_t input[kModelSamples];
  Chunk discarded{};
  speech_commands::AudioTestStatus result{};
  result.gain = gain;
  wake_word::beginInference();
  model->print_active_speech_commands(data);
  if (model->open_log) model->open_log(data);
  model->clean(data);
  wake_word::endInference();
  for (size_t offset = 0; offset + kModelSamples <= count; offset += kModelSamples) {
    // Keep consuming live idle audio while testing, without starting actions.
    while (xQueueReceive(outputQueue, &discarded, 0) == pdTRUE) {
      portENTER_CRITICAL(&lock); ++state.totalFetches; portEXIT_CRITICAL(&lock);
    }
    for (int i = 0; i < kModelSamples; ++i) {
      input[i] = static_cast<int16_t>(constrain(static_cast<int>(samples[offset+i]) * gain, int(INT16_MIN), int(INT16_MAX)));
    }
    wake_word::beginInference();
    const auto detected = model->detect(data, input);
    ++result.chunks; result.decoderState = detected;
    if (detected != ESP_MN_STATE_DETECTING || offset + 2*kModelSamples > count) {
      const auto* decoded = model->get_results(data);
      if (decoded) {
        snprintf(result.text, sizeof(result.text), "%.127s", decoded->string);
        if (decoded->num) { result.command = decoded->command_id[0]; result.score = decoded->prob[0]; }
      }
    }
    wake_word::endInference();
    if (detected != ESP_MN_STATE_DETECTING) break;
  }
  free(samples);
  wake_word::beginInference(); model->clean(data); wake_word::endInference();
  result.complete = true;
  portENTER_CRITICAL(&lock); testState = result; portEXIT_CRITICAL(&lock);
  Serial.printf("MultiNet saved-audio test: gain=%u chunks=%lu state=%d command=%d score=%.3f text=%s (no actions)\n",
                gain, (unsigned long)result.chunks, result.decoderState, result.command, result.score, result.text);
}

void fail(const char* message) {
  portENTER_CRITICAL(&lock);
  ++session;
  state.ready = state.listening = false;
  pendingCommand = 0;
  snprintf(state.error, sizeof(state.error), "%s", message);
  portEXIT_CRITICAL(&lock);
  Serial.printf("MultiNet: %s\n", message);
}

bool current(const speech_pipeline::Tag& tag) {
  portENTER_CRITICAL(&lock);
  const bool valid = state.ready && speech_pipeline::current(tag, session, state.listening, millis());
  portEXIT_CRITICAL(&lock);
  return valid;
}

void interrupt(uint32_t expected, const char* reason) {
  portENTER_CRITICAL(&lock);
  if (state.listening && expected == session) {
    ++session;
    state.listening = false;
    pendingCommand = 0;
    snprintf(state.result, sizeof(state.result), "%s", reason);
  }
  portEXIT_CRITICAL(&lock);
}

void feedTask(void*) {
  speech_pipeline::Assembler assembler(feedSamples, kModelSamples);
  speech_pipeline::Frame frame{};
  Chunk chunk{};
  for (;;) {
    if (xQueueReceive(inputQueue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    if (!speech_commands::snapshot().ready) continue;
    for (unsigned i = 0; i < frame.count; ++i) {
      if (!assembler.append(frame, i, chunk.tag)) continue;
      memcpy(chunk.samples, feedSamples, sizeof(chunk.samples));
      if (xQueueSend(outputQueue, &chunk, 0) != pdTRUE) {
        interrupt(chunk.tag.session, "Audio queue overflow");
        portENTER_CRITICAL(&lock); ++state.droppedFrames; portEXIT_CRITICAL(&lock);
        continue;
      }
      portENTER_CRITICAL(&lock);
      ++state.totalFeeds;
      if (state.listening && chunk.tag.session == session) ++state.feedChunks;
      portEXIT_CRITICAL(&lock);
    }
  }
}

void publishResult(const speech_pipeline::Tag& tag, esp_mn_state_t result, const esp_mn_results_t* results,
                   uint32_t elapsed, uint16_t peak) {
    portENTER_CRITICAL(&lock);
    if (speech_pipeline::current(tag, session, state.listening, millis())) {
      ++state.fetchChunks;
      ++state.chunks;
      state.inferenceUs = elapsed;
      state.modelPeak = max(state.modelPeak, peak);
      if (results) {
        snprintf(state.decoderText, sizeof(state.decoderText), "%.127s", results->string);
        if (results->num > 0) {
          state.candidateCommand = results->command_id[0];
          state.candidateScore = results->prob[0];
        }
      }
      // Match the demo's acceptance: DETECTED + a nonempty recognized phrase.
      // Keep Digit's command-ID check and dispatch at most one action per wake.
      if (result == ESP_MN_STATE_DETECTED && results &&
          speech_pipeline::accepts(results->num, results->string, results->command_id[0])) {
        pendingCommand = state.lastCommand = results->command_id[0];
        pendingGeneration = controlGeneration;
        state.score = results->prob[0];
        ++state.recognized;
        state.listening = false;
        snprintf(state.result, sizeof(state.result), "Recognized");
      } else if (result == ESP_MN_STATE_TIMEOUT) {
        state.listening = false;
        snprintf(state.result, sizeof(state.result), "No command recognized");
      }
    }
    portEXIT_CRITICAL(&lock);
}

void detectTask(void*) {
  uint32_t processed = 0;
  bool captureIntact = true;
  Chunk chunk{};
  for (;;) {
    portENTER_CRITICAL(&lock);
    auto* probe = testPending; const size_t probeCount = testCount; const uint8_t probeGain = testState.gain;
    testPending = nullptr;
    portEXIT_CRITICAL(&lock);
    if (probe) { runAudioTest(probe, probeCount, probeGain); continue; }
    if (xQueueReceive(outputQueue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    const auto& tag = chunk.tag;
    portENTER_CRITICAL(&lock);
    ++state.totalFetches;
    const bool active = state.listening && tag.session == session;
    portEXIT_CRITICAL(&lock);
    if (!current(tag)) {
      if (active) interrupt(tag.session, "Audio interrupted");
      continue;
    }

    wake_word::beginInference();
    // Stop can arrive while waiting for the wake/vision inference mutex.
    if (!current(tag)) { wake_word::endInference(); continue; }
    const bool first = processed != tag.session;
    if (first) { model->clean(data); processed = tag.session; captureIntact = true; }
    uint16_t peak = 0;
    for (int i = 0; i < kModelSamples; ++i) {
      const int value = chunk.samples[i];
      peak = max(peak, static_cast<uint16_t>(value < 0 ? -value : value));
    }
    captureIntact = captureInput(chunk, first) && captureIntact;
    const uint32_t started = micros();
    const auto result = model->detect(data, chunk.samples);
    const uint32_t elapsed = micros() - started;
    if (result != ESP_MN_STATE_DETECTING) finishCapture(tag.session, captureIntact);
    // Only this task touches the MultiNet decoder, including clean/get_results.
    auto* results = result == ESP_MN_STATE_DETECTING ? nullptr : model->get_results(data);
    publishResult(tag, result, results, elapsed, peak);
    if (results) {
      Serial.printf("MultiNet decoder: state=%d text=%s candidates=%d\n", result, results->string, results->num);
      for (int i = 0; i < results->num && i < ESP_MN_RESULT_MAX_NUM; ++i) {
        Serial.printf("MultiNet candidate: command=%d phrase=%d score=%.3f\n",
                      results->command_id[i], results->phrase_id[i], results->prob[i]);
      }
    }
    wake_word::endInference();
    if (result == ESP_MN_STATE_TIMEOUT) {
      const auto s = speech_commands::snapshot();
      Serial.printf("MultiNet timeout: input=%lu ms feed/fetch=%lu/%lu chunks=%lu mic=%u model=%u inference=%lu us\n",
                    (unsigned long)(s.samples / 16), (unsigned long)s.feedChunks,
                    (unsigned long)s.fetchChunks, (unsigned long)s.chunks, s.micPeak, s.modelPeak,
                    (unsigned long)s.inferenceUs);
    }
  }
}
} // namespace

namespace speech_commands {
void begin() {
  const auto* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
  uint32_t count = 0;
  if (!partition || esp_partition_read(partition, 0, &count, sizeof(count)) != ESP_OK || count == 0 || count > 16) {
    fail("MultiNet model missing; upload the complete firmware and model bundle"); return;
  }
  auto* models = esp_srmodel_init("model");
  char* name = models ? esp_srmodel_filter(models, "mn6", ESP_MN_ENGLISH) : nullptr;
  if (!name || !(model = esp_mn_handle_from_name(name)) || !(data = model->create(name, kRecognitionMs))) {
    fail("English MultiNet 6 initialization failed"); return;
  }
  // The ReSpeaker is the hardware audio front end. Assemble its 10-ms reads
  // into the exact MultiNet frame without a second DSP/VAD/gain-control pass.
  if (model->get_samp_rate(data) != 16000 || model->get_samp_chunksize(data) != kModelSamples) {
    fail("Unsupported MultiNet 6 audio format"); return;
  }
  if (esp_mn_commands_alloc(model, data) != ESP_OK) { fail("Cannot allocate command list"); return; }
  esp_mn_commands_clear();
  // MultiNet 6 registers uppercase command text, as in the demo.
  for (const auto& phrase : speech_phrases::phrases) {
    if (esp_mn_commands_add(phrase.command, phrase.text) != ESP_OK) {
      fail("Cannot register speech command"); return;
    }
  }
  if (esp_mn_commands_update() != nullptr) { fail("MultiNet rejected a command phrase"); return; }
  captureLock = xSemaphoreCreateMutex();
  if (captureLock) capturedAudio = static_cast<int16_t*>(heap_caps_malloc(kCaptureSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!capturedAudio) Serial.println("MultiNet: audio export unavailable (diagnostic allocation failed)");
  static StaticQueue_t inputControl, outputControl;
  auto* inputStorage = static_cast<uint8_t*>(heap_caps_malloc(100 * sizeof(speech_pipeline::Frame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  auto* outputStorage = static_cast<uint8_t*>(heap_caps_malloc(32 * sizeof(Chunk), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (inputStorage) inputQueue = xQueueCreateStatic(100, sizeof(speech_pipeline::Frame), inputStorage, &inputControl);
  if (outputStorage) outputQueue = xQueueCreateStatic(32, sizeof(Chunk), outputStorage, &outputControl);
  if (!inputQueue || !outputQueue) { fail("Cannot allocate speech queues"); return; }
  TaskHandle_t feedTaskHandle = nullptr;
  if (xTaskCreatePinnedToCore(feedTask, "speech-feed", 8192, nullptr, 5, &feedTaskHandle, 0) != pdPASS) {
    fail("Cannot start speech feed task"); return;
  }
  if (xTaskCreatePinnedToCore(detectTask, "speech-detect", 8192, nullptr, 5, nullptr, 1) != pdPASS) {
    vTaskDelete(feedTaskHandle);
    fail("Cannot start speech detection task"); return;
  }
  portENTER_CRITICAL(&lock); state.ready = true; portEXIT_CRITICAL(&lock);
  model->print_active_speech_commands(data);
  Serial.printf("MultiNet 6: ReSpeaker hardware AFE -> mono feed/fetch -> MultiNet; chunk=%d, timeout=%lu ms; free PSRAM=%u\n",
                kModelSamples, (unsigned long)kRecognitionMs, (unsigned)ESP.getFreePsram());
}

void listen(uint32_t generation) {
  bool opened = false;
  portENTER_CRITICAL(&lock);
  if (state.ready && !testState.busy) {
    ++session; controlGeneration = generation; pendingCommand = 0;
    opensAt = millis(); waitingForVoice = true; state.listening = true;
    state.samples = state.chunks = state.inferenceUs = 0; state.micPeak = 0;
    state.feedChunks = state.fetchChunks = 0; state.modelPeak = 0;
    state.candidateCommand = 0; state.candidateScore = 0; state.decoderText[0] = '\0';
    snprintf(state.result, sizeof(state.result), "Wake voice");
    opened = true;
  }
  portEXIT_CRITICAL(&lock);
  if (opened) Serial.println("MultiNet: waiting for Digit voice, then 120 ms echo tail; model timeout 6 seconds");
}

void cancel() {
  portENTER_CRITICAL(&lock);
  if (state.listening) snprintf(state.result, sizeof(state.result), "Cancelled");
  ++session; state.listening = false; waitingForVoice = false; pendingCommand = 0;
  portEXIT_CRITICAL(&lock);
}

void submit(const int32_t* stereo, size_t count) {
  if (!inputQueue || !stereo || !count || count > speech_pipeline::kInputSamples) return;
  speech_pipeline::Frame frame{};
  portENTER_CRITICAL(&lock);
  const bool ready = state.ready;
  const bool active = ready && state.listening && !waitingForVoice && static_cast<int32_t>(millis() - opensAt) >= 0;
  frame.session = active ? session : 0;
  frame.stream = stream;
  portEXIT_CRITICAL(&lock);
  if (!ready) return;
  frame.capturedAt = millis(); frame.count = count;
  uint16_t peak = 0;
  for (size_t i = 0; i < count; ++i) {
    // The XVF hardware owns microphone DSP; convert format without x4 gain.
    const auto value = speech_pipeline::mono(stereo[2*i+1]);
    frame.samples[i] = value;
    peak = max(peak, static_cast<uint16_t>(value < 0 ? -static_cast<int>(value) : value));
  }
  const bool sent = xQueueSend(inputQueue, &frame, 0) == pdTRUE;
  portENTER_CRITICAL(&lock);
  if (sent && active && frame.session == session && state.listening) {
    state.samples += count; state.micPeak = max(state.micPeak, peak);
  }
  if (!sent) {
    ++state.droppedFrames; ++stream;
    if (state.listening) {
      ++session; state.listening = false; pendingCommand = 0;
      snprintf(state.result, sizeof(state.result), "Audio queue overflow");
    }
  }
  portEXIT_CRITICAL(&lock);
}

AudioCapture copyAudio() {
  AudioCapture copy{};
  if (!captureLock || xSemaphoreTake(captureLock, pdMS_TO_TICKS(50)) != pdTRUE) return copy;
  if (capturedComplete && capturedCount) {
    copy.samples = static_cast<int16_t*>(heap_caps_malloc(capturedCount * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy.samples) {
      copy.count = capturedCount; copy.session = capturedSession;
      memcpy(copy.samples, capturedAudio, copy.count * sizeof(int16_t));
    }
  }
  xSemaphoreGive(captureLock);
  return copy;
}
bool testAudio(int16_t* samples, size_t count, uint8_t gain) {
  if (!samples || !count || count > kCaptureSamples || count % kModelSamples || gain < 1 || gain > 8) return false;
  portENTER_CRITICAL(&lock);
  const bool allowed = state.ready && !state.listening && !testState.busy;
  if (allowed) {
    testState = {}; testState.busy = true; testState.gain = gain;
    testCount = count; testPending = samples;
  }
  portEXIT_CRITICAL(&lock);
  return allowed;
}
AudioTestStatus audioTestStatus() {
  portENTER_CRITICAL(&lock); auto copy = testState; portEXIT_CRITICAL(&lock); return copy;
}
void audioFault() { fail("Microphone unavailable; check audio status"); }

void poll() {
  const auto audio = audioEngine.snapshot();
  bool cancelVoice = false;
  portENTER_CRITICAL(&lock);
  if (state.listening && waitingForVoice) {
    if (!audio.cuePlaying && !audio.wakeChimePlaying && audio.state != AudioState::Playing) {
      waitingForVoice = false;
      opensAt = millis() + kVoiceTailMs;
      snprintf(state.result, sizeof(state.result), "Listening");
    } else if (millis() - opensAt >= kVoiceWaitTimeoutMs) {
      waitingForVoice = state.listening = false; ++session; cancelVoice = true;
      snprintf(state.result, sizeof(state.result), "Wake voice interrupted");
    }
  }
  if (state.listening && !waitingForVoice && static_cast<int32_t>(millis() - opensAt) >= static_cast<int32_t>(kStallMs)) {
    state.listening = false; ++session;
    snprintf(state.result, sizeof(state.result), "Audio interrupted");
  }
  const int command = pendingCommand;
  const uint32_t generation = pendingGeneration;
  pendingCommand = 0;
  portEXIT_CRITICAL(&lock);
  if (cancelVoice) audioEngine.cancelOutput();
  if (command) {
    const bool accepted = robot_actions::start(static_cast<robot_actions::Command>(command), generation);
    Serial.printf("MultiNet command %d: %s\n", command, accepted ? "accepted" : "cancelled or unavailable");
  }
}

Status snapshot() {
  portENTER_CRITICAL(&lock); auto copy = state; portEXIT_CRITICAL(&lock); return copy;
}
} // namespace speech_commands
