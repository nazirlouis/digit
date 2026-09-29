#pragma once
#include <cstddef>
#include <cstdint>

class HardwareSerial {
 public:
  explicit HardwareSerial(int port) : port_(port) {}
  void begin(uint32_t baud, int config, int rx_pin, int tx_pin);
  int available();
  int read();
  void print(unsigned value);
  void print(uint16_t value) { print(static_cast<unsigned>(value)); }
  void print(char value);
  void println(const char* value);
  void printf(const char* format, ...);

 private:
  int port_;
  void send(const char* data, size_t length);
};
