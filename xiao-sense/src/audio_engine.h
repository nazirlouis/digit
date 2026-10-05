#pragma once

#include <Arduino.h>
#include <AudioTools.h>
#include "audio_cues.h"
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
  bool wakeChimePlaying;
  char error[96];
};

// Cached read-only XVF audit, collected at startup outside audio capture.
// Unknown or unsupported values stay -1 rather than being treated as enabled.
struct AudioProcessingStatus {
  int leftCategory = -1, leftSource = -1, rightCategory = -1, rightSource = -1;
  int packedLeft = -1, packedRight = -1, asrOutput = -1, aecBypass = -1;
  int agc = -1, limiter = -1, echoSuppression = -1, nonlinearEcho = -1;
  float asrGain = -1, agcGain = -1, stationaryNoiseFloor = -1, speechNoiseFloor = -1;
};

class AudioEngine {
 public:
  bool begin();

  AudioActionResult startRecording();
  AudioActionResult stopRecording();
  AudioActionResult playLatest();
  AudioActionResult playTestTone();
  bool playWakeChime();
  bool playCue(AudioCue cue);
  void cancelOutput();
  bool cuePlaying();
  AudioActionResult stopPlayback();
  bool setSpeakerVolume(uint8_t percent);

  AudioStatus snapshot();
  const AudioProcessingStatus& processing() const { return processing_; }
  void logDiagnostics();
  // Shared I2C access with bounded retries of status 0x40. Bool is transport
  // success; payload changes only on status 0. Never call from audio capture.
  bool readXvfCommand(uint8_t resourceId, uint8_t commandId,
                      uint8_t* payload, size_t payloadSize,
                      uint8_t* responseStatus);
  static const char* stateName(AudioState state);

 private:
  enum class Command : uint8_t {
    Record,
    Play,
    Tone,
  };

  static constexpr EventBits_t kStopRecordingBit = BIT0;
  static constexpr EventBits_t kStopPlaybackBit = BIT1;
  static constexpr EventBits_t kWakeChimeBit = BIT2;

  static void taskEntry(void* context);
  void taskLoop();
  bool detectI2sClockSource();
  bool beginI2s();
  void endI2s();

  bool initializeSpeakerPath();
  bool setAmplifierEnabled(bool enabled);
  bool writeXvfGpo(uint8_t pin, uint8_t value);
  bool readXvfGpoValues(uint8_t values[5], uint8_t* responseStatus);
  bool readXvfVersion();
  void readProcessingStatus();
  void logCodecDiagnostics();
  bool writeCodecRegister(uint8_t reg, uint8_t value);
  bool probeI2cAddress(uint8_t address);

  void setState(AudioState state);
  void setError(const char* message);
  void updateCapturedBytes(size_t bytes);

  SemaphoreHandle_t mutex_ = nullptr;
  AudioProcessingStatus processing_{};
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
  bool wakeChimePlaying_ = false;
  AudioCue pendingCue_ = AudioCue::None;
  bool cuePlaying_ = false;
  uint32_t outputGeneration_ = 0;
  uint8_t speakerVolumePercent_ = 0;
  char error_[96] = "Audio has not been initialized";
  audio_tools::I2SStream i2s_;
};

extern AudioEngine audioEngine;
