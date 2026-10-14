// The runner extracts the actual streamHandler function into stream_handler.cpp.
#include "stream_memory_mocks.h"
#include "stream_handler.cpp"

void reset() {
  assert(!mock::frameHeld && mock::allocations.empty());
  mock::ready = true;
  mock::allocationFails = false;
  mock::captureLimit = 1;
  mock::captures = mock::returns = mock::allocationsCalled = 0;
  mock::sendCalls = mock::failSendAt = mock::bodySends = mock::errorResponses = 0;
  mock::failAllocationAt = 0;
  mock::frameLength = 32;
  mock::frameFormat = PIXFORMAT_JPEG;
  mock::status.clear();
  mock::contentType.clear();
  mock::message.clear();
}
void noLeaks() { assert(!mock::frameHeld && mock::allocations.empty()); }
int main() {
  httpd_req_t request;
  reset();
  mock::captureLimit = 2;
  assert(streamHandler(&request) == ESP_FAIL);
  noLeaks();
  assert(mock::returns == 2 && mock::allocationsCalled == 1 && mock::bodySends == 2);
  assert(mock::errorResponses == 0); // Cannot send a new 503 after multipart started.

  for (unsigned failAt = 1; failAt <= 3; ++failAt) {
    reset();
    mock::failSendAt = failAt;
    assert(streamHandler(&request) == -9);
    noLeaks();
    assert(mock::returns == 1 && mock::sendCalls == failAt);
  }
  reset();
  mock::allocationFails = true;
  assert(streamHandler(&request) == ESP_OK); // A complete HTTP 503 response was sent.
  noLeaks();
  assert(mock::captures == 0 && mock::sendCalls == 0 && mock::status == "503 Service Unavailable");
  assert(mock::contentType == "text/plain" && mock::errorResponses == 1);

  reset();
  mock::frameLength = camera_service::kMaxJpegBytes;
  assert(streamHandler(&request) == ESP_FAIL); // Full frame sent, then no fresh frame.
  noLeaks();
  assert(mock::bodySends == 1 && mock::allocationsCalled == 1 && mock::errorResponses == 0);

  for (size_t length : {size_t(0), camera_service::kMaxJpegBytes + 1}) {
    reset();
    mock::frameLength = length;
    assert(streamHandler(&request) == ESP_OK);
    noLeaks();
    assert(mock::captures == 1 && mock::allocationsCalled == 1 && mock::sendCalls == 0);
  }
  reset();
  mock::frameFormat = 999;
  assert(streamHandler(&request) == ESP_OK);
  noLeaks();
  assert(mock::captures == 1 && mock::allocationsCalled == 1);
  reset();
  mock::captureLimit = 0;
  assert(streamHandler(&request) == ESP_OK);
  noLeaks();
  assert(mock::errorResponses == 1 && mock::captures == 0);
  reset();
  mock::ready = false;
  assert(streamHandler(&request) == ESP_OK);
  noLeaks();
  assert(mock::captures == 0 && mock::errorResponses == 1);

  std::puts("Camera stream ownership tests passed");
}
