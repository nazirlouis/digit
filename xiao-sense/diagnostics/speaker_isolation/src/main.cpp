#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <math.h>

// Isolate the XIAO -> Flex speaker path: native I2S, continuous clocks,
// no camera, Wi-Fi, microphone, recording, or AudioTools conversion.
// Requires the Flex circular 16 kHz I2S firmware and XIAO-generated clocks.
namespace {
constexpr uint32_t kRate = 16000;
constexpr uint32_t kToneFrames = kRate * 2;
constexpr size_t kChunkFrames = 256;
constexpr int32_t kAmplitude = 0x08000000;
int32_t samples[kChunkFrames * 2] = {};
uint32_t toneFrame = kToneFrames;
bool ready = false;

bool writeCodec(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(0x18);
  Wire.write(reg);
  Wire.write(value);
  const uint8_t result = Wire.endTransmission();
  Serial.printf("Codec %02X=%02X result=%u\n", reg, value, result);
  return result == 0;
}

bool check(esp_err_t result, const char* operation) {
  if (result == ESP_OK) return true;
  Serial.printf("FAILED %s: %s\n", operation, esp_err_to_name(result));
  return false;
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("SPEAKER ISOLATION: native I2S, no Wi-Fi/camera, continuous clocks");
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = kRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.bits_per_chan = I2S_BITS_PER_CHAN_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.dma_buf_count = 6;
  config.dma_buf_len = kChunkFrames;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;
  config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = 8;
  pins.ws_io_num = 7;
  pins.data_out_num = 44;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (!check(i2s_driver_install(I2S_NUM_0, &config, 0, nullptr), "install") ||
      !check(i2s_set_pin(I2S_NUM_0, &pins), "pins") ||
      !check(i2s_zero_dma_buffer(I2S_NUM_0), "zero DMA")) return;

  delay(250);
  Wire.begin(5, 6, 100000);
  Wire.setTimeOut(100);
  // Same 50% DAC attenuation and 0 dB line output as the dashboard test.
  if (!writeCodec(0x00, 0x00) || !writeCodec(0x2B, 0x40) ||
      !writeCodec(0x2C, 0x40) || !writeCodec(0x56, 0x0B) ||
      !writeCodec(0x5D, 0x0B)) return;
  ready = true;
  Serial.printf("READY: %.1f Hz, 32-bit stereo; t = 2-second sine, s = silence\n",
                i2s_get_clk(I2S_NUM_0));
}

void loop() {
  if (!ready) { delay(100); return; }
  while (Serial.available()) {
    const char command = Serial.read();
    if (command == 't') {
      toneFrame = 0;
      Serial.println("TONE START: 440 Hz, 2 seconds, DAC volume 50%");
    } else if (command == 's') {
      toneFrame = kToneFrames;
      Serial.println("SILENCE: clocks remain running");
    }
  }
  const bool wasPlaying = toneFrame < kToneFrames;
  for (size_t frame = 0; frame < kChunkFrames; ++frame) {
    int32_t sample = 0;
    if (toneFrame < kToneFrames) {
      const uint32_t edge = min(toneFrame, kToneFrames - 1 - toneFrame);
      const float envelope = static_cast<float>(min(edge, kRate / 50)) / (kRate / 50);
      const float phase = 6.28318530718f * ((toneFrame * 440) % kRate) / kRate;
      sample = static_cast<int32_t>(sinf(phase) * envelope * kAmplitude);
      ++toneFrame;
    }
    samples[frame * 2] = sample;
    samples[frame * 2 + 1] = sample;
  }
  size_t written = 0;
  if (!check(i2s_write(I2S_NUM_0, samples, sizeof(samples), &written,
                       pdMS_TO_TICKS(500)), "write") || written != sizeof(samples)) {
    Serial.printf("Audio write incomplete: %u/%u bytes\n", written, sizeof(samples));
    i2s_zero_dma_buffer(I2S_NUM_0);
    ready = false;
  }
  if (wasPlaying && toneFrame == kToneFrames) Serial.println("TONE QUEUED; returning to silence");
}
