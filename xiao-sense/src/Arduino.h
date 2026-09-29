#pragma once

// Small adapters for the former Arduino application services. The firmware is
// built entirely with ESP-IDF; this header does not include the Arduino core.
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "HardwareSerial.h"

#define PROGMEM
#define INPUT 0
#define SERIAL_8N1 0

inline uint32_t millis() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
inline uint32_t micros() { return static_cast<uint32_t>(esp_timer_get_time()); }
inline void delay(uint32_t ms) {
  const TickType_t ticks = pdMS_TO_TICKS(ms ? ms : 1);
  vTaskDelay(ticks ? ticks : 1);
}
inline void pinMode(int pin, int) { gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_INPUT); }
inline int digitalRead(int pin) { return gpio_get_level(static_cast<gpio_num_t>(pin)); }
template <typename T> inline T constrain(T value, T lo, T hi) { return std::max(lo, std::min(value, hi)); }
using std::min;
using std::max;

struct SerialAdapter {
  void begin(int) {}
  void print(const char* value) { fputs(value, stdout); }
  void print(char value) { putchar(value); }
  void print(unsigned value) { printf("%u", value); }
  void println(const char* value = "") { puts(value); }
  template <typename... Args> void printf(const char* format, Args... args) { ::printf(format, args...); }
};
extern SerialAdapter Serial;

struct EspAdapter {
  size_t getFreePsram() const { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
  size_t getFreeHeap() const { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }
};
extern EspAdapter ESP;

using String = std::string;
