#pragma once

#include <Arduino.h>
#include <AudioTools.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

enum class AudioState : uint8_t {
  Unavailable,
  Ready,
  Recording,
  Playing,
  Error,
};

enum class AudioActionResult : uint8_t {
  Accepted,
  Conflict,
  NoRecording,
  Unavailable,
};

struct AudioStatus {
  AudioState state;
  bool available;
  size_t capturedBytes;
  uint32_t capturedMilliseconds;
  uint32_t recordingElapsedMilliseconds;
  uint8_t speakerVolumePercent;
  char error[96];
};

class AudioEngine {
 public:
  bool begin();

  AudioActionResult startRecording();
  AudioActionResult stopRecording();
  AudioActionResult playLatest();
  AudioActionResult playTestTone();
  AudioActionResult stopPlayback();
  bool setSpeakerVolume(uint8_t percent);

  AudioStatus snapshot();
  void logDiagnostics();
  static const char* stateName(AudioState state);

 private:
  enum class Command : uint8_t {
    Record,
    Play,
    Tone,
  };

  static constexpr EventBits_t kStopRecordingBit = BIT0;
  static constexpr EventBits_t kStopPlaybackBit = BIT1;

  static void taskEntry(void* context);
  void taskLoop();
  bool captureRecording();
  bool playRecording();
  bool playDiagnosticTone();
  bool detectI2sClockSource();
  bool beginI2s(bool receive);
  void endI2s();

  bool initializeSpeakerPath();
  bool setAmplifierEnabled(bool enabled);
  bool writeXvfGpo(uint8_t pin, uint8_t value);
  bool readXvfCommand(uint8_t resourceId, uint8_t commandId,
                      uint8_t* payload, size_t payloadSize,
                      uint8_t* responseStatus);
  bool readXvfGpoValues(uint8_t values[5], uint8_t* responseStatus);
  bool readXvfVersion();
  void logCodecDiagnostics();
  bool writeCodecRegister(uint8_t reg, uint8_t value);
  bool probeI2cAddress(uint8_t address);

  void setState(AudioState state);
  void setError(const char* message);
  void updateCapturedBytes(size_t bytes);

  SemaphoreHandle_t mutex_ = nullptr;
  QueueHandle_t commandQueue_ = nullptr;
  EventGroupHandle_t stopEvents_ = nullptr;
  TaskHandle_t task_ = nullptr;
  uint8_t* audioBuffer_ = nullptr;
  size_t capturedBytes_ = 0;
  uint32_t recordingStartedAtMs_ = 0;
  AudioState state_ = AudioState::Unavailable;
  bool initialized_ = false;
  bool i2sRunning_ = false;
  bool xiaoI2sMaster_ = true;
  uint8_t speakerVolumePercent_ = 0;
  char error_[96] = "Audio has not been initialized";
  audio_tools::I2SStream i2s_;
};

extern AudioEngine audioEngine;
