#include "Arduino.h"
#include "Wire.h"
#include "WiFi.h"
#include "AudioTools.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "driver/i2c_master.h"
#include "driver/i2s.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"

SerialAdapter Serial;
EspAdapter ESP;
WireAdapter Wire;
WifiAdapter WiFi;
static esp_netif_t* wifi_netif = nullptr;
static int wifi_rssi = 0;
static i2c_master_bus_handle_t audio_i2c_bus = nullptr;
static i2c_master_dev_handle_t audio_i2c_devices[128] = {};
static uint32_t audio_i2c_rate = 100000;

static i2c_master_dev_handle_t audio_i2c_device(uint8_t address) {
  if (!audio_i2c_bus || address >= 128) return nullptr;
  if (!audio_i2c_devices[address]) {
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = address;
    config.scl_speed_hz = audio_i2c_rate;
    if (i2c_master_bus_add_device(audio_i2c_bus, &config, &audio_i2c_devices[address]) != ESP_OK)
      return nullptr;
  }
  return audio_i2c_devices[address];
}

void set_wifi_netif(esp_netif_t* netif) { wifi_netif = netif; }
int WifiAdapter::status() const {
  esp_netif_ip_info_t info = {};
  return wifi_netif && esp_netif_get_ip_info(wifi_netif, &info) == ESP_OK && info.ip.addr ? WL_CONNECTED : 0;
}
std::string WifiIp::toString() const {
  esp_netif_ip_info_t info = {};
  if (!wifi_netif || esp_netif_get_ip_info(wifi_netif, &info) != ESP_OK) return "0.0.0.0";
  char address[16];
  snprintf(address, sizeof(address), IPSTR, IP2STR(&info.ip));
  return address;
}
int WifiAdapter::RSSI() const {
  return wifi_rssi;
}
void refresh_wifi_signal() {
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) wifi_rssi = ap.rssi;
}

void HardwareSerial::begin(uint32_t baud, int, int rx_pin, int tx_pin) {
  const auto port = static_cast<uart_port_t>(port_);
  uart_config_t config = {};
  config.baud_rate = baud;
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_1;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.source_clk = UART_SCLK_DEFAULT;
  ESP_ERROR_CHECK(uart_param_config(port, &config));
  ESP_ERROR_CHECK(uart_set_pin(port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
  ESP_ERROR_CHECK(uart_driver_install(port, 1024, 1024, 0, nullptr, 0));
}
int HardwareSerial::available() {
  size_t count = 0;
  uart_get_buffered_data_len(static_cast<uart_port_t>(port_), &count);
  return static_cast<int>(count);
}
int HardwareSerial::read() {
  uint8_t byte = 0;
  return uart_read_bytes(static_cast<uart_port_t>(port_), &byte, 1, 0) == 1 ? byte : -1;
}
void HardwareSerial::send(const char* data, size_t length) {
  uart_write_bytes(static_cast<uart_port_t>(port_), data, length);
}
void HardwareSerial::print(unsigned value) { char data[12]; const int n = snprintf(data, sizeof(data), "%u", value); send(data, n); }
void HardwareSerial::print(char value) { send(&value, 1); }
void HardwareSerial::println(const char* value) { send(value, strlen(value)); send("\n", 1); }
void HardwareSerial::printf(const char* format, ...) {
  char data[128];
  va_list args;
  va_start(args, format);
  const int count = vsnprintf(data, sizeof(data), format, args);
  va_end(args);
  if (count > 0) send(data, std::min(static_cast<size_t>(count), sizeof(data) - 1));
}

void WireAdapter::begin(int sda, int scl, uint32_t frequency) {
  audio_i2c_rate = frequency;
  i2c_master_bus_config_t config = {};
  config.i2c_port = I2C_NUM_1;
  config.sda_io_num = static_cast<gpio_num_t>(sda);
  config.scl_io_num = static_cast<gpio_num_t>(scl);
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.glitch_ignore_cnt = 7;
  config.flags.enable_internal_pullup = true;
  ESP_ERROR_CHECK(i2c_new_master_bus(&config, &audio_i2c_bus));
}
void WireAdapter::beginTransmission(uint8_t address) {
  address_ = address;
  tx_size_ = 0;
  pending_restart_ = false;
}
size_t WireAdapter::write(uint8_t value) {
  if (tx_size_ == sizeof(tx_)) return 0;
  tx_[tx_size_++] = value;
  return 1;
}
uint8_t WireAdapter::endTransmission(bool send_stop) {
  if (!send_stop) { pending_restart_ = true; return 0; }
  esp_err_t result;
  if (tx_size_ == 0) {
    result = i2c_master_probe(audio_i2c_bus, address_, timeout_ms_);
  } else {
    const auto device = audio_i2c_device(address_);
    result = device ? i2c_master_transmit(device, tx_, tx_size_, timeout_ms_) : ESP_FAIL;
  }
  tx_size_ = 0;
  return result == ESP_OK ? 0 : result == ESP_ERR_TIMEOUT ? 5 : 2;
}
size_t WireAdapter::requestFrom(uint8_t address, size_t count, bool) {
  rx_size_ = rx_offset_ = 0;
  if (count > sizeof(rx_)) return 0;
  esp_err_t result;
  const auto device = audio_i2c_device(address);
  if (!device) return 0;
  if (pending_restart_) {
    result = i2c_master_transmit_receive(device, tx_, tx_size_, rx_, count, timeout_ms_);
  } else {
    result = i2c_master_receive(device, rx_, count, timeout_ms_);
  }
  pending_restart_ = false;
  tx_size_ = 0;
  if (result == ESP_OK) rx_size_ = count;
  return rx_size_;
}
int WireAdapter::read() { return rx_offset_ < rx_size_ ? rx_[rx_offset_++] : -1; }

bool audio_tools::I2SStream::begin(const I2SConfig& config) {
  end();
  i2s_config_t driver = {};
  driver.mode = static_cast<i2s_mode_t>((config.is_master ? I2S_MODE_MASTER : I2S_MODE_SLAVE) |
                                         ((config.rx_tx_mode & RX_MODE) ? I2S_MODE_RX : 0) |
                                         ((config.rx_tx_mode & TX_MODE) ? I2S_MODE_TX : 0));
  driver.sample_rate = config.sample_rate;
  driver.bits_per_sample = static_cast<i2s_bits_per_sample_t>(config.bits);
  driver.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  driver.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  driver.intr_alloc_flags = 0;
  driver.dma_buf_count = 8;
  driver.dma_buf_len = 256;
  driver.use_apll = false;
  driver.tx_desc_auto_clear = config.auto_clear;
  driver.fixed_mclk = 0;
  if (i2s_driver_install(I2S_NUM_0, &driver, 0, nullptr) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = config.pin_bck;
  pins.ws_io_num = config.pin_ws;
  pins.data_out_num = (config.rx_tx_mode & TX_MODE) ? config.pin_data : I2S_PIN_NO_CHANGE;
  pins.data_in_num = config.rx_tx_mode == RXTX_MODE ? config.pin_data_rx :
                    config.rx_tx_mode == RX_MODE ? config.pin_data : I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }
  running_ = true;
  return true;
}
void audio_tools::I2SStream::end() {
  if (running_) i2s_driver_uninstall(I2S_NUM_0);
  running_ = false;
}
size_t audio_tools::I2SStream::readBytes(uint8_t* buffer, size_t length) {
  size_t count = 0;
  if (running_) i2s_read(I2S_NUM_0, buffer, length, &count, pdMS_TO_TICKS(read_timeout_ms_));
  return count;
}
size_t audio_tools::I2SStream::write(const uint8_t* buffer, size_t length) {
  size_t count = 0;
  if (running_) i2s_write(I2S_NUM_0, buffer, length, &count, pdMS_TO_TICKS(write_timeout_ms_));
  return count;
}
