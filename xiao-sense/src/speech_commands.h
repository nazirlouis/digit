#pragma once
#include <cstddef>
#include <cstdint>
namespace speech_commands {
struct Status { bool ready; bool listening; uint32_t recognized; int lastCommand; float score; char error[96]; };
void begin();
void listen(uint32_t generation);
void cancel();
void submit(const int32_t* stereo, size_t frames);
void poll();
Status snapshot();
}
