#include "speech_commands.h"
#include "robot_actions.h"
#include "wake_word.h"
#include "Arduino.h"
#include <cstring>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include <model_path.h>
#include <esp_partition.h>
#include <freertos/queue.h>
namespace {
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
speech_commands::Status state{};
esp_mn_iface_t* model = nullptr;
model_iface_data_t* data = nullptr;
QueueHandle_t queue = nullptr;
int16_t* samples = nullptr;
uint32_t session = 0, controlGeneration = 0, opensAt = 0, closesAt = 0;
int pendingCommand = 0;
uint32_t pendingGeneration = 0;
struct Frame { uint32_t session, capturedAt; uint16_t count; int16_t samples[160]; };
void fail(const char* message) {
  portENTER_CRITICAL(&lock);
  state.ready = state.listening = false;
  snprintf(state.error, sizeof(state.error), "%s", message);
  portEXIT_CRITICAL(&lock);
  Serial.printf("MultiNet: %s\n", message);
}
void task(void*) {
  const int chunk = model->get_samp_chunksize(data);
  Frame frame{};
  uint32_t processed = UINT32_MAX;
  int filled = 0;
  for (;;) {
    if (xQueueReceive(queue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    portENTER_CRITICAL(&lock);
    const bool valid = state.listening && frame.session == session &&
        static_cast<int32_t>(millis() - closesAt) < 0 && millis() - frame.capturedAt <= 1000;
    portEXIT_CRITICAL(&lock);
    if (!valid) { processed = UINT32_MAX; filled = 0; continue; }
    wake_word::beginInference();
    if (processed != frame.session) { model->clean(data); filled = 0; processed = frame.session; }
    for (unsigned offset = 0; offset < frame.count; ++offset) {
      samples[filled++] = frame.samples[offset];
      if (filled != chunk) continue;
      filled = 0;
      const auto result = model->detect(data, samples);
      if (result == ESP_MN_STATE_DETECTED) {
        auto* results = model->get_results(data);
        portENTER_CRITICAL(&lock);
        if (state.listening && frame.session == session && results && results->num > 0 &&
            results->command_id[0] >= 1 && results->command_id[0] <= 7 && results->prob[0] >= 0.70f) {
          pendingCommand = state.lastCommand = results->command_id[0];
          pendingGeneration = controlGeneration;
          state.score = results->prob[0];
          ++state.recognized;
          state.listening = false;
        }
        portEXIT_CRITICAL(&lock);
      }
    }
    wake_word::endInference();
  }
}
}
namespace speech_commands {
void begin() {
  const auto* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
  uint32_t count = 0;
  if (!partition || esp_partition_read(partition, 0, &count, sizeof(count)) != ESP_OK || count == 0 || count > 16) {
    fail("MultiNet model missing; upload the complete firmware and model bundle"); return;
  }
  auto* models = esp_srmodel_init("model");
  char* name = models ? esp_srmodel_filter(models, "mn7", "en") : nullptr;
  if (!name || !(model = esp_mn_handle_from_name(name)) || !(data = model->create(name, 5000))) {
    fail("English MultiNet model initialization failed"); return;
  }
  if (model->get_samp_rate(data) != 16000 || model->get_samp_chunksize(data) < 1 || model->get_samp_chunksize(data) > 2048) {
    fail("Unsupported MultiNet audio format"); return;
  }
  samples = static_cast<int16_t*>(heap_caps_malloc(model->get_samp_chunksize(data) * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!samples) { fail("No memory for command audio"); return; }
  if (esp_mn_commands_alloc(model, data) != ESP_OK) { fail("Cannot allocate command list"); return; }
  const char* phrases[] = {"say hello", "follow me", "do a dance", "look around", "take my picture", "explore", "stop moving"};
  for (int i = 0; i < 7; ++i) {
    if (esp_mn_commands_add(i + 1, phrases[i]) != ESP_OK) { fail("Cannot register speech command"); return; }
  }
  if (esp_mn_commands_add(7, "stop") != ESP_OK || esp_mn_commands_update() != nullptr) {
    fail("MultiNet rejected a command phrase"); return;
  }
  static StaticQueue_t queueControl;
  auto* storage = static_cast<uint8_t*>(heap_caps_malloc(100 * sizeof(Frame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (storage) queue = xQueueCreateStatic(100, sizeof(Frame), storage, &queueControl);
  if (!queue || xTaskCreatePinnedToCore(task, "multinet", 8192, nullptr, 2, nullptr, 0) != pdPASS) {
    fail("Cannot start MultiNet listener"); return;
  }
  portENTER_CRITICAL(&lock); state.ready = true; portEXIT_CRITICAL(&lock);
  model->print_active_speech_commands(data);
  Serial.println("MultiNet: ready; say Digit, wait for chime, then a command");
}
void listen(uint32_t generation) {
  portENTER_CRITICAL(&lock);
  if (state.ready) {
    ++session; controlGeneration = generation; pendingCommand = 0;
    opensAt = millis() + 500; closesAt = opensAt + 5000; state.listening = true;
  }
  portEXIT_CRITICAL(&lock);
}
void cancel() {
  portENTER_CRITICAL(&lock);
  ++session; state.listening = false; pendingCommand = 0;
  portEXIT_CRITICAL(&lock);
}
void submit(const int32_t* stereo, size_t count) {
  if (!queue || !count || count > 160) return;
  Frame frame{};
  portENTER_CRITICAL(&lock);
  const bool active = state.ready && state.listening && static_cast<int32_t>(millis() - opensAt) >= 0;
  frame.session = session;
  portEXIT_CRITICAL(&lock);
  if (!active) return;
  frame.capturedAt = millis(); frame.count = count;
  // ReSpeaker's processed right channel is already beamformed/noise reduced.
  for (size_t i = 0; i < count; ++i) frame.samples[i] = static_cast<int16_t>(
      constrain<int32_t>((stereo[2*i+1] >> 16) * 4, INT16_MIN, INT16_MAX));
  if (xQueueSend(queue, &frame, 0) != pdTRUE) cancel(); // Never splice discontinuous speech.
}
void poll() {
  portENTER_CRITICAL(&lock);
  if (state.listening && static_cast<int32_t>(millis() - closesAt) >= 0) { state.listening = false; ++session; }
  const int command = pendingCommand;
  const uint32_t generation = pendingGeneration;
  pendingCommand = 0;
  portEXIT_CRITICAL(&lock);
  if (command) {
    const bool accepted = robot_actions::start(static_cast<robot_actions::Command>(command), generation);
    Serial.printf("MultiNet command %d: %s\n", command, accepted ? "accepted" : "cancelled or unavailable");
  }
}
Status snapshot() {
  portENTER_CRITICAL(&lock); auto copy = state; portEXIT_CRITICAL(&lock); return copy;
}
}
