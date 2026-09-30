#include "photo_store.h"
#include "robot_actions.h"
#include "Arduino.h"
#include <atomic>
#include <cstring>
#include <esp_camera.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
namespace {
constexpr size_t kCapacity = 256 * 1024;
uint8_t* saved = nullptr;
size_t length = 0;
SemaphoreHandle_t gate = nullptr;
QueueHandle_t requests = nullptr;
std::atomic<bool> pending{false}, error{false};
std::atomic<uint32_t> revisionNumber{0}, cancellation{0};
struct Request { uint32_t generation, cancellation; };
void task(void*) {
  Request request;
  for (;;) {
    xQueueReceive(requests, &request, portMAX_DELAY);
    camera_fb_t* frame = esp_camera_fb_get();
    const bool valid = frame && frame->format == PIXFORMAT_JPEG && frame->len && frame->len <= kCapacity;
    xSemaphoreTake(gate, portMAX_DELAY);
    {
      robot_actions::Guard control;
      if (request.generation == robot_actions::generation() && request.cancellation == cancellation.load()) {
        if (valid) { memcpy(saved, frame->buf, frame->len); length = frame->len; ++revisionNumber; }
        error = !valid;
      }
    }
    xSemaphoreGive(gate);
    if (frame) esp_camera_fb_return(frame);
    pending = false;
  }
}
}
namespace photo_store {
void begin() {
  saved = static_cast<uint8_t*>(heap_caps_malloc(kCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  gate = xSemaphoreCreateMutex();
  requests = xQueueCreate(1, sizeof(Request));
  if (!saved || !gate || !requests || xTaskCreate(task, "photo-save", 4096, nullptr, 1, nullptr) != pdPASS) {
    error = true; requests = nullptr;
  }
}
bool capture(uint32_t generation) {
  if (!requests || pending.exchange(true)) return false;
  error = false;
  Request request{generation, cancellation.load()};
  if (xQueueSend(requests, &request, 0) != pdTRUE) { pending = false; return false; }
  return true;
}
void cancel() { ++cancellation; }
bool busy() { return pending.load(); }
bool ready() { return revisionNumber.load() != 0; }
bool failed() { return error.load(); }
uint32_t revision() { return revisionNumber.load(); }
static void sendTask(void* context) {
  auto* request = static_cast<httpd_req_t*>(context);
  xSemaphoreTake(gate, portMAX_DELAY);
  httpd_resp_set_type(request, "image/jpeg");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_send(request, reinterpret_cast<char*>(saved), length);
  xSemaphoreGive(gate);
  httpd_req_async_handler_complete(request);
  vTaskDelete(nullptr);
}
esp_err_t send(httpd_req_t* request) {
  if (!gate || !ready()) {
    httpd_resp_set_status(request, "404 Not Found");
    return httpd_resp_sendstr(request, "No voice photo saved yet");
  }
  httpd_req_t* asyncRequest = nullptr;
  if (httpd_req_async_handler_begin(request, &asyncRequest) != ESP_OK) return ESP_FAIL;
  if (xTaskCreate(sendTask, "photo-http", 4096, asyncRequest, 1, nullptr) != pdPASS) {
    httpd_resp_set_status(asyncRequest,"503 Service Unavailable");
    httpd_resp_sendstr(asyncRequest,"Photo download busy");
    httpd_req_async_handler_complete(asyncRequest);
  }
  return ESP_OK;
}
}
