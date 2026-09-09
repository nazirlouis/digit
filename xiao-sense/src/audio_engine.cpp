#include "audio_engine.h"

#include <Wire.h>
#include <esp_heap_caps.h>

#include "app_config.h"

namespace {

constexpr uint8_t kGpoServicerResourceId = 20;
constexpr uint8_t kGpoWriteCommandId = 1;
constexpr uint8_t kAmplifierEnablePin = 31;

constexpr uint8_t kCodecPageControlRegister = 0x00;
constexpr uint8_t kCodecLeftDacVolumeRegister = 0x2B;
constexpr uint8_t kCodecRightDacVolumeRegister = 0x2C;
constexpr uint8_t kCodecLeftLineOutputRegister = 0x56;
constexpr uint8_t kCodecRightLineOutputRegister = 0x5D;
constexpr uint8_t kCodecLineOutputEnabled = 0x0B;

constexpr size_t kAudioFrameBytes =
    app_config::kAudioChannels * (app_config::kAudioBitsPerSample / 8);
constexpr uint32_t kNoDataFailureLimit = 100;

}  // namespace

AudioEngine audioEngine;

bool AudioEngine::begin() {
  mutex_ = xSemaphoreCreateMutex();
  commandQueue_ = xQueueCreate(2, sizeof(Command));
  stopEvents_ = xEventGroupCreate();
  if (mutex_ == nullptr || commandQueue_ == nullptr || stopEvents_ == nullptr) {
    setError("Could not create audio synchronization objects");
    return false;
  }

  audioBuffer_ = static_cast<uint8_t*>(heap_caps_malloc(
      app_config::kAudioBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (audioBuffer_ == nullptr) {
    setError("Could not allocate the 10-second PSRAM audio buffer");
    return false;
  }

  Wire.begin(app_config::kI2cSdaPin, app_config::kI2cSclPin,
             app_config::kI2cFrequencyHz);
  delay(250);
  if (!initializeSpeakerPath()) {
    setError("ReSpeaker I2C or speaker initialization failed");
    return false;
  }

  const BaseType_t taskResult = xTaskCreatePinnedToCore(
      taskEntry, "digit-audio", 8192, this, 3, &task_, 1);
  if (taskResult != pdPASS) {
    setError("Could not start the audio task");
    return false;
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  initialized_ = true;
  state_ = AudioState::Ready;
  error_[0] = '\0';
  xSemaphoreGive(mutex_);

  Serial.printf("Audio: ready, %u-byte PSRAM buffer\n",
                static_cast<unsigned int>(app_config::kAudioBufferBytes));
  return true;
}

AudioActionResult AudioEngine::startRecording() {
  if (mutex_ == nullptr || commandQueue_ == nullptr || stopEvents_ == nullptr) {
    return AudioActionResult::Unavailable;
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (!initialized_ || state_ == AudioState::Error) {
    xSemaphoreGive(mutex_);
    return AudioActionResult::Unavailable;
  }
  if (state_ != AudioState::Ready) {
    xSemaphoreGive(mutex_);
    return AudioActionResult::Conflict;
  }

  capturedBytes_ = 0;
  recordingStartedAtMs_ = millis();
  error_[0] = '\0';
  state_ = AudioState::Recording;
  xSemaphoreGive(mutex_);

  xEventGroupClearBits(stopEvents_, kStopRecordingBit | kStopPlaybackBit);
  const Command command = Command::Record;
  if (xQueueSend(commandQueue_, &command, 0) != pdTRUE) {
    setError("The audio command queue is full");
    return AudioActionResult::Unavailable;
  }
  return AudioActionResult::Accepted;
}

AudioActionResult AudioEngine::stopRecording() {
  if (mutex_ == nullptr || stopEvents_ == nullptr) {
    return AudioActionResult::Unavailable;
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  const AudioState currentState = state_;
  const bool available = initialized_ && state_ != AudioState::Error;
  xSemaphoreGive(mutex_);

  if (!available) {
    return AudioActionResult::Unavailable;
  }
  if (currentState != AudioState::Recording) {
    return AudioActionResult::Conflict;
  }
  xEventGroupSetBits(stopEvents_, kStopRecordingBit);
  return AudioActionResult::Accepted;
}

AudioActionResult AudioEngine::playLatest() {
  if (mutex_ == nullptr || commandQueue_ == nullptr || stopEvents_ == nullptr) {
    return AudioActionResult::Unavailable;
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (!initialized_ || state_ == AudioState::Error) {
    xSemaphoreGive(mutex_);
    return AudioActionResult::Unavailable;
  }
  if (state_ != AudioState::Ready) {
    xSemaphoreGive(mutex_);
    return AudioActionResult::Conflict;
  }
  if (capturedBytes_ == 0) {
    xSemaphoreGive(mutex_);
    return AudioActionResult::NoRecording;
  }
  state_ = AudioState::Playing;
  xSemaphoreGive(mutex_);

  xEventGroupClearBits(stopEvents_, kStopPlaybackBit);
  const Command command = Command::Play;
  if (xQueueSend(commandQueue_, &command, 0) != pdTRUE) {
    setError("The audio command queue is full");
    return AudioActionResult::Unavailable;
  }
  return AudioActionResult::Accepted;
}

AudioActionResult AudioEngine::stopPlayback() {
  if (mutex_ == nullptr || stopEvents_ == nullptr) {
    return AudioActionResult::Unavailable;
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  const AudioState currentState = state_;
  const bool available = initialized_ && state_ != AudioState::Error;
  xSemaphoreGive(mutex_);

  if (!available) {
    return AudioActionResult::Unavailable;
  }
  if (currentState != AudioState::Playing) {
    return AudioActionResult::Conflict;
  }
  xEventGroupSetBits(stopEvents_, kStopPlaybackBit);
  return AudioActionResult::Accepted;
}

AudioStatus AudioEngine::snapshot() {
  AudioStatus status = {};
  if (mutex_ == nullptr) {
    status.state = AudioState::Unavailable;
    status.available = false;
    snprintf(status.error, sizeof(status.error), "%s", error_);
    return status;
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  status.state = state_;
  status.available = initialized_ && state_ != AudioState::Error;
  status.capturedBytes = capturedBytes_;
  status.capturedMilliseconds = static_cast<uint32_t>(
      (capturedBytes_ * 1000ULL) / app_config::kAudioBytesPerSecond);
  if (state_ == AudioState::Recording && recordingStartedAtMs_ != 0) {
    status.recordingElapsedMilliseconds = millis() - recordingStartedAtMs_;
  } else {
    status.recordingElapsedMilliseconds = 0;
  }
  snprintf(status.error, sizeof(status.error), "%s", error_);
  xSemaphoreGive(mutex_);
  return status;
}

const char* AudioEngine::stateName(AudioState state) {
  switch (state) {
    case AudioState::Ready:
      return "READY";
    case AudioState::Recording:
      return "RECORDING";
    case AudioState::Playing:
      return "PLAYING";
    case AudioState::Error:
      return "ERROR";
    case AudioState::Unavailable:
    default:
      return "UNAVAILABLE";
  }
}

void AudioEngine::taskEntry(void* context) {
  static_cast<AudioEngine*>(context)->taskLoop();
}

void AudioEngine::taskLoop() {
  for (;;) {
    Command command;
    if (xQueueReceive(commandQueue_, &command, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (command == Command::Record) {
      if (!captureRecording()) {
        continue;
      }

      AudioStatus status = snapshot();
      if (status.capturedBytes == 0) {
        setState(AudioState::Ready);
        continue;
      }

      setState(AudioState::Playing);
      xEventGroupClearBits(stopEvents_, kStopPlaybackBit);
      playRecording();
    } else if (command == Command::Play) {
      playRecording();
    }
  }
}

bool AudioEngine::captureRecording() {
  Serial.println("Audio: recording started");
  if (!beginI2s(true)) {
    setError("Could not start the ReSpeaker I2S input");
    return false;
  }

  size_t offset = 0;
  uint32_t emptyReads = 0;
  while (offset < app_config::kAudioBufferBytes) {
    if ((xEventGroupGetBits(stopEvents_) & kStopRecordingBit) != 0) {
      break;
    }

    const size_t remaining = app_config::kAudioBufferBytes - offset;
    const size_t requested =
        min(remaining, static_cast<size_t>(app_config::kAudioIoChunkBytes));
    const size_t received = i2s_.readBytes(audioBuffer_ + offset, requested);
    if (received == 0) {
      if (++emptyReads >= kNoDataFailureLimit) {
        endI2s();
        setError("No audio samples arrived from the ReSpeaker");
        return false;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    emptyReads = 0;
    offset += received;
    updateCapturedBytes(offset - (offset % kAudioFrameBytes));
  }

  endI2s();
  updateCapturedBytes(offset - (offset % kAudioFrameBytes));
  xSemaphoreTake(mutex_, portMAX_DELAY);
  recordingStartedAtMs_ = 0;
  xSemaphoreGive(mutex_);
  Serial.printf("Audio: recording stopped at %u bytes\n",
                static_cast<unsigned int>(offset));
  return true;
}

bool AudioEngine::playRecording() {
  AudioStatus status = snapshot();
  const size_t bytesToPlay = status.capturedBytes;
  if (bytesToPlay == 0) {
    setState(AudioState::Ready);
    return true;
  }

  if (!setAmplifierEnabled(true)) {
    setError("Could not enable the ReSpeaker speaker amplifier");
    return false;
  }
  delay(30);

  if (!beginI2s(false)) {
    setAmplifierEnabled(false);
    setError("Could not start the ReSpeaker I2S output");
    return false;
  }

  Serial.printf("Audio: playing %u bytes\n",
                static_cast<unsigned int>(bytesToPlay));
  size_t offset = 0;
  uint32_t emptyWrites = 0;
  while (offset < bytesToPlay) {
    if ((xEventGroupGetBits(stopEvents_) & kStopPlaybackBit) != 0) {
      break;
    }

    const size_t remaining = bytesToPlay - offset;
    const size_t requested =
        min(remaining, static_cast<size_t>(app_config::kAudioIoChunkBytes));
    const size_t written = i2s_.write(audioBuffer_ + offset, requested);
    if (written == 0) {
      if (++emptyWrites >= kNoDataFailureLimit) {
        endI2s();
        setAmplifierEnabled(false);
        setError("The ReSpeaker stopped accepting playback samples");
        return false;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    emptyWrites = 0;
    offset += written;
  }

  // A short zero tail prevents the final sample from being held at the output.
  uint8_t silence[app_config::kAudioIoChunkBytes] = {};
  i2s_.write(silence, sizeof(silence));
  endI2s();
  delay(30);

  if (!setAmplifierEnabled(false)) {
    setError("Playback ended, but the speaker amplifier did not disable");
    return false;
  }

  setState(AudioState::Ready);
  Serial.printf("Audio: playback ended at %u bytes\n",
                static_cast<unsigned int>(offset));
  return true;
}

bool AudioEngine::beginI2s(bool receive) {
  endI2s();

  const audio_tools::AudioInfo info(app_config::kAudioSampleRate,
                                    app_config::kAudioChannels,
                                    app_config::kAudioBitsPerSample);
  audio_tools::I2SConfig config =
      i2s_.defaultConfig(receive ? RX_MODE : TX_MODE);
  config.copyFrom(info);
  config.rx_tx_mode = receive ? RX_MODE : TX_MODE;
  config.pin_bck = app_config::kI2sBitClockPin;
  config.pin_ws = app_config::kI2sWordSelectPin;
  config.pin_data = app_config::kI2sDataToRespeakerPin;
  config.pin_data_rx = app_config::kI2sDataFromRespeakerPin;
  config.is_master = true;
  return i2s_.begin(config);
}

void AudioEngine::endI2s() { i2s_.end(); }

bool AudioEngine::initializeSpeakerPath() {
  if (!probeI2cAddress(app_config::kXvf3800I2cAddress)) {
    Serial.println("Audio: XVF3800 did not answer at I2C address 0x2C");
    return false;
  }
  if (!probeI2cAddress(app_config::kAic3104I2cAddress)) {
    Serial.println("Audio: AIC3104 did not answer at I2C address 0x18");
    return false;
  }

  // Disable the active-low amplifier while configuring the codec.
  if (!setAmplifierEnabled(false)) {
    return false;
  }

  return writeCodecRegister(kCodecPageControlRegister, 0x00) &&
         writeCodecRegister(kCodecLeftDacVolumeRegister,
                            app_config::kCodecDacAttenuation) &&
         writeCodecRegister(kCodecRightDacVolumeRegister,
                            app_config::kCodecDacAttenuation) &&
         writeCodecRegister(kCodecLeftLineOutputRegister,
                            kCodecLineOutputEnabled) &&
         writeCodecRegister(kCodecRightLineOutputRegister,
                            kCodecLineOutputEnabled);
}

bool AudioEngine::setAmplifierEnabled(bool enabled) {
  return writeXvfGpo(kAmplifierEnablePin, enabled ? 0 : 1);
}

bool AudioEngine::writeXvfGpo(uint8_t pin, uint8_t value) {
  Wire.beginTransmission(app_config::kXvf3800I2cAddress);
  Wire.write(kGpoServicerResourceId);
  Wire.write(kGpoWriteCommandId);
  Wire.write(static_cast<uint8_t>(2));
  Wire.write(pin);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool AudioEngine::writeCodecRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(app_config::kAic3104I2cAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool AudioEngine::probeI2cAddress(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void AudioEngine::setState(AudioState state) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  state_ = state;
  xSemaphoreGive(mutex_);
}

void AudioEngine::setError(const char* message) {
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  initialized_ = false;
  state_ = AudioState::Error;
  snprintf(error_, sizeof(error_), "%s", message);
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
  Serial.printf("Audio error: %s\n", message);
}

void AudioEngine::updateCapturedBytes(size_t bytes) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  capturedBytes_ = bytes;
  xSemaphoreGive(mutex_);
}

