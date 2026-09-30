#pragma once
#include <cstddef>
#include <cstdint>

constexpr int RX_MODE = 1;
constexpr int TX_MODE = 2;
constexpr int RXTX_MODE = RX_MODE | TX_MODE;
namespace audio_tools {
constexpr int I2S_STD_FORMAT = 0;
struct AudioInfo {
  uint32_t sample_rate;
  uint8_t channels;
  uint8_t bits;
  AudioInfo(uint32_t rate, uint8_t channel_count, uint8_t bit_count)
      : sample_rate(rate), channels(channel_count), bits(bit_count) {}
};
struct I2SConfig {
  uint32_t sample_rate = 16000;
  uint8_t channels = 2;
  uint8_t bits = 32;
  int rx_tx_mode = RX_MODE;
  int pin_bck = -1;
  int pin_ws = -1;
  int pin_data = -1;
  int pin_data_rx = -1;
  bool is_master = true;
  int i2s_format = I2S_STD_FORMAT;
  bool auto_clear = true;
  void copyFrom(const AudioInfo& info) {
    sample_rate = info.sample_rate; channels = info.channels; bits = info.bits;
  }
};
class I2SStream {
 public:
  I2SConfig defaultConfig(int mode) const { I2SConfig result; result.rx_tx_mode = mode; return result; }
  bool begin(const I2SConfig& config);
  void end();
  size_t readBytes(uint8_t* buffer, size_t length);
  size_t write(const uint8_t* buffer, size_t length);
  I2SStream* driver() { return this; }
  void setWaitTimeReadMs(uint32_t ms) { read_timeout_ms_ = ms; }
  void setWaitTimeWriteMs(uint32_t ms) { write_timeout_ms_ = ms; }

 private:
  bool running_ = false;
  uint32_t read_timeout_ms_ = 500;
  uint32_t write_timeout_ms_ = 500;
};
}  // namespace audio_tools
