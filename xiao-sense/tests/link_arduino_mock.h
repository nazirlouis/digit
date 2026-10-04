#pragma once
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <sstream>
#include <string>
inline uint32_t nowMs = 1000;
inline uint32_t millis() { return nowMs; }
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
constexpr int SERIAL_8N1 = 0;
struct HardwareSerial {
  std::string incoming, outgoing;
  explicit HardwareSerial(int) {}
  void begin(uint32_t, int, int, int) {}
  size_t available() { return incoming.size(); }
  int read() { int c=incoming.front(); incoming.erase(0,1); return c; }
  template<class T> void print(T value) { std::ostringstream s; s<<value; outgoing+=s.str(); }
  void println(const char* value) { outgoing+=value; outgoing+='\n'; }
  void printf(const char* format, ...) {
    char line[256]; va_list args; va_start(args,format);
    std::vsnprintf(line,sizeof(line),format,args); va_end(args); outgoing+=line;
  }
};
struct SerialLog { void println(const char*) {} };
inline SerialLog Serial;
