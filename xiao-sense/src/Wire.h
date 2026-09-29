#pragma once
#include <cstddef>
#include <cstdint>

class WireAdapter {
 public:
  void begin(int sda, int scl, uint32_t frequency);
  void setTimeOut(uint32_t timeout_ms) { timeout_ms_ = timeout_ms; }
  void beginTransmission(uint8_t address);
  size_t write(uint8_t value);
  uint8_t endTransmission(bool send_stop = true);
  size_t requestFrom(uint8_t address, size_t count, bool send_stop = true);
  size_t available() const { return rx_size_ - rx_offset_; }
  int read();

 private:
  uint8_t address_ = 0;
  uint8_t tx_[32] = {};
  size_t tx_size_ = 0;
  uint8_t rx_[32] = {};
  size_t rx_size_ = 0;
  size_t rx_offset_ = 0;
  bool pending_restart_ = false;
  uint32_t timeout_ms_ = 100;
};
extern WireAdapter Wire;
