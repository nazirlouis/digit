#pragma once
#include <cstddef>
#include <cstdint>
#include "camera_frame_limits.h"

namespace camera_service {

bool begin();
bool ready();
const char* lastError();
inline constexpr size_t kMaxJpegBytes = camera_frame_limits::kMaxJpegBytes;
struct Frame {
  size_t size = 0;
  uint32_t timestampMs = 0;
  uint16_t width = 0, height = 0;
};
// Copies the latest complete frame. Never holds a camera buffer or lock while
// a consumer decodes, infers, or writes a network socket. Rejects stale frames.
bool copyLatestJpeg(uint8_t* destination, size_t capacity, Frame* frame);

}  // namespace camera_service
