#pragma once
#include <cstddef>

namespace camera_frame_limits {
// One bound for VGA JPEG capture, consumer copies, and Gemini encoding/preview.
// Keep the camera driver's explicit framebuffer size and all consumers in sync.
inline constexpr size_t kMaxJpegBytes = 64 * 1024;
}  // namespace camera_frame_limits
