#pragma once

#include <Arduino.h>

namespace app_config {

// Change this password before using Digit in a public place. WPA2 requires at
// least eight characters.
static constexpr char kAccessPointName[] = "Digit-Sense";
static constexpr char kAccessPointPassword[] = "digit-sense";
static constexpr uint8_t kAccessPointChannel = 6;
static constexpr uint8_t kMaximumWifiClients = 4;

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

// A conservative starting attenuation for the speaker path. The web UI does
// not expose volume in v1; adjust this only after a low-volume hardware test.
static constexpr uint8_t kCodecDacAttenuation = 0x24;

static constexpr uint16_t kControlServerPort = 80;
static constexpr uint16_t kStreamServerPort = 81;
static constexpr uint8_t kTargetCameraFps = 10;

static_assert(kAudioBitsPerSample % 8 == 0,
              "Audio samples must occupy a whole number of bytes");
static_assert(sizeof(kAccessPointPassword) - 1 >= 8,
              "The Wi-Fi access point password must be at least 8 characters");

}  // namespace app_config

