#pragma once

#include <Arduino.h>

namespace app_config {

static constexpr char kWifiHostname[] = "digit-sense";
static constexpr uint32_t kWifiConnectTimeoutMs = 30000;

static constexpr uint32_t kAudioSampleRate = 16000;
static constexpr uint8_t kAudioChannels = 2;
static constexpr uint8_t kAudioBitsPerSample = 32;
static constexpr uint32_t kMaximumRecordingSeconds = 10;
static constexpr size_t kAudioBytesPerSecond =
    kAudioSampleRate * kAudioChannels * (kAudioBitsPerSample / 8);
static constexpr size_t kAudioBufferBytes =
    kAudioBytesPerSecond * kMaximumRecordingSeconds;
static constexpr size_t kAudioIoChunkBytes = 4096;

// ReSpeaker XVF3800 embedded-mode buses.
static constexpr int kI2sBitClockPin = 8;
static constexpr int kI2sWordSelectPin = 7;
static constexpr int kI2sDataToRespeakerPin = 44;
static constexpr int kI2sDataFromRespeakerPin = 43;
static constexpr int kI2cSdaPin = 5;
static constexpr int kI2cSclPin = 6;
static constexpr uint32_t kI2cFrequencyHz = 100000;
static constexpr uint8_t kXvf3800I2cAddress = 0x2C;
static constexpr uint8_t kAic3104I2cAddress = 0x18;

// Start at full speaker volume. The dashboard can change this from 0-100%.
static constexpr uint8_t kDefaultSpeakerVolumePercent = 100;

static constexpr uint16_t kControlServerPort = 80;
static constexpr uint16_t kStreamServerPort = 81;
static constexpr uint8_t kTargetCameraFps = 10;

static_assert(kAudioBitsPerSample % 8 == 0,
              "Audio samples must occupy a whole number of bytes");
}  // namespace app_config
