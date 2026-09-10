#include "audio_engine.h"

#include <Wire.h>
#include <esp_heap_caps.h>

#include "app_config.h"

namespace {

constexpr uint8_t kGpoServicerResourceId = 20;
constexpr uint8_t kGpoReadCommandId = 0;
constexpr uint8_t kGpoWriteCommandId = 1;
constexpr uint8_t kAmplifierEnablePin = 31;
constexpr uint8_t kAmplifierGpoIndex = 2;
constexpr uint8_t kXvfReadBit = 0x80;
constexpr uint8_t kVersionResourceId = 48;
constexpr uint8_t kVersionCommandId = 0;
constexpr uint8_t kXvfSuccessStatus = 0;
constexpr uint8_t kXvfRetryStatus = 64;
constexpr uint8_t kI2cAttemptCount = 3;

constexpr uint8_t kCodecPageControlRegister = 0x00;
constexpr uint8_t kCodecLeftDacVolumeRegister = 0x2B;
constexpr uint8_t kCodecRightDacVolumeRegister = 0x2C;
constexpr uint8_t kCodecLeftLineOutputRegister = 0x56;
constexpr uint8_t kCodecRightLineOutputRegister = 0x5D;
constexpr uint8_t kCodecLineOutputEnabled = 0x0B;
constexpr uint8_t kCodecDacMute = 0x80;
constexpr uint8_t kCodecMaximumAttenuation = 0x7F;

constexpr size_t kAudioFrameBytes =
    app_config::kAudioChannels * (app_config::kAudioBitsPerSample / 8);
constexpr uint32_t kNoDataFailureLimit = 100;
constexpr uint32_t kPlaybackTargetPeak = 0x10000000;
constexpr uint32_t kMaximumPlaybackGain = 65536;
constexpr uint32_t kTestToneFrequencyHz = 440;
constexpr uint32_t kTestToneDurationMs = 2000;
constexpr int32_t kTestToneAmplitude = 0x08000000;
constexpr size_t kTestToneFramesPerChunk = 256;

// These buffers must not live on the audio task's stack.  The Wire/I2C driver
// also needs several kilobytes of stack, and combining it with the old 4 KiB
// silence buffer plus 2 KiB tone buffer overflowed the 8 KiB task stack before
// the first tone sample could be written.
uint8_t gPlaybackSilence[app_config::kAudioIoChunkBytes] = {};
int32_t gTestTone[kTestToneFramesPerChunk * app_config::kAudioChannels] = {};

uint8_t volumePercentToCodecValue(uint8_t percent) {
  if (percent == 0) {
    return kCodecDacMute;
  }

  // The AIC3104 uses 0.5 dB attenuation steps: 0x00 is full volume and
  // 0x7F is -63.5 dB. Map 1-100% across that useful logarithmic range.
  return static_cast<uint8_t>(
      ((100U - percent) * kCodecMaximumAttenuation + 49U) / 99U);
}

const char* i2cResultName(uint8_t result) {
  switch (result) {
    case 0:
      return "success";
    case 1:
      return "data-too-long";
    case 2:
      return "address-NACK";
    case 3:
      return "data-NACK";
    case 4:
      return "other-error";
    case 5:
      return "timeout";
    default:
      return "unknown";
  }
}

void printI2cScan() {
  Serial.println("Audio: scanning I2C addresses on GPIO5/GPIO6");
  bool foundAny = false;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf("Audio: I2C device found at 0x%02X\n", address);
      foundAny = true;
    }
  }
  if (!foundAny) {
    Serial.println("Audio: no I2C devices found");
  }
}

}  // namespace

AudioEngine audioEngine;

bool AudioEngine::begin() {
  Serial.printf("[%10lu] Audio: begin; sample=%lu Hz, channels=%u, bits=%u, I2C=%lu Hz\n",
                static_cast<unsigned long>(millis()),
                static_cast<unsigned long>(app_config::kAudioSampleRate),
                app_config::kAudioChannels, app_config::kAudioBitsPerSample,
                static_cast<unsigned long>(app_config::kI2cFrequencyHz));
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
  Serial.printf("[%10lu] Audio: PSRAM buffer allocated at %p (%u bytes); free PSRAM=%u\n",
                static_cast<unsigned long>(millis()), audioBuffer_,
                static_cast<unsigned int>(app_config::kAudioBufferBytes),
                static_cast<unsigned int>(ESP.getFreePsram()));

  Wire.begin(app_config::kI2cSdaPin, app_config::kI2cSclPin,
             app_config::kI2cFrequencyHz);
  Wire.setTimeOut(100);
  Serial.printf("[%10lu] Audio: I2C started on SDA=%d SCL=%d\n",
                static_cast<unsigned long>(millis()),
                app_config::kI2cSdaPin, app_config::kI2cSclPin);

  // The XVF3800 is running the 16 kHz I2S-slave image, so the XIAO owns
  // BCLK/WS. Start a quiet TX stream before probing its control plane, matching
  // Seeed's standalone record/playback initialization order.
  if (!beginI2s(false)) {
    setError("Could not start initial I2S clocks for the ReSpeaker");
    return false;
  }
  delay(250);
  if (!initializeSpeakerPath()) {
    endI2s();
    return false;
  }
  endI2s();

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
  Serial.printf("[%10lu] Audio API: start recording requested in state %s\n",
                static_cast<unsigned long>(millis()), stateName(state_));
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
  Serial.printf("[%10lu] Audio API: stop recording requested in state %s\n",
                static_cast<unsigned long>(millis()), stateName(currentState));

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
  Serial.printf("[%10lu] Audio API: replay requested in state %s (%u bytes captured)\n",
                static_cast<unsigned long>(millis()), stateName(state_),
                static_cast<unsigned int>(capturedBytes_));
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

AudioActionResult AudioEngine::playTestTone() {
  if (mutex_ == nullptr || commandQueue_ == nullptr || stopEvents_ == nullptr) {
    return AudioActionResult::Unavailable;
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  Serial.printf("[%10lu] Audio API: speaker test requested in state %s\n",
                static_cast<unsigned long>(millis()), stateName(state_));
  if (!initialized_ || state_ == AudioState::Error) {
    xSemaphoreGive(mutex_);
    return AudioActionResult::Unavailable;
  }
  if (state_ != AudioState::Ready) {
    xSemaphoreGive(mutex_);
    return AudioActionResult::Conflict;
  }
  state_ = AudioState::Playing;
  xSemaphoreGive(mutex_);

  xEventGroupClearBits(stopEvents_, kStopPlaybackBit);
  const Command command = Command::Tone;
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
  Serial.printf("[%10lu] Audio API: stop playback requested in state %s\n",
                static_cast<unsigned long>(millis()), stateName(currentState));

  if (!available) {
    return AudioActionResult::Unavailable;
  }
  if (currentState != AudioState::Playing) {
    return AudioActionResult::Conflict;
  }
  xEventGroupSetBits(stopEvents_, kStopPlaybackBit);
  return AudioActionResult::Accepted;
}

bool AudioEngine::setSpeakerVolume(uint8_t percent) {
  if (percent > 100 || mutex_ == nullptr) {
    return false;
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool available = initialized_ && state_ != AudioState::Error;
  xSemaphoreGive(mutex_);
  if (!available) {
    return false;
  }

  const uint8_t codecValue = volumePercentToCodecValue(percent);
  Serial.printf(
      "[%10lu] Audio volume: request=%u%% codec DAC value=0x%02X\n",
      static_cast<unsigned long>(millis()), percent, codecValue);
  const bool updated =
      writeCodecRegister(kCodecPageControlRegister, 0x00) &&
      writeCodecRegister(kCodecLeftDacVolumeRegister, codecValue) &&
      writeCodecRegister(kCodecRightDacVolumeRegister, codecValue);
  if (!updated) {
    Serial.println("Audio warning: speaker volume update failed");
    return false;
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  speakerVolumePercent_ = percent;
  xSemaphoreGive(mutex_);
  Serial.printf("[%10lu] Audio volume: now %u%%\n",
                static_cast<unsigned long>(millis()), percent);
  return true;
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
  status.speakerVolumePercent = speakerVolumePercent_;
  snprintf(status.error, sizeof(status.error), "%s", error_);
  xSemaphoreGive(mutex_);
  return status;
}

void AudioEngine::logDiagnostics() {
  const AudioStatus status = snapshot();
  Serial.printf(
      "[%10lu] Health: audio=%s available=%s captured=%u bytes/%lu ms "
      "I2S=%s freePSRAM=%u freeHeap=%u error=\"%s\"\n",
      static_cast<unsigned long>(millis()), stateName(status.state),
      status.available ? "yes" : "no",
      static_cast<unsigned int>(status.capturedBytes),
      static_cast<unsigned long>(status.capturedMilliseconds),
      i2sRunning_ ? "running" : "stopped",
      static_cast<unsigned int>(ESP.getFreePsram()),
      static_cast<unsigned int>(ESP.getFreeHeap()), status.error);
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
    } else if (command == Command::Tone) {
      playDiagnosticTone();
    }
  }
}

bool AudioEngine::captureRecording() {
  Serial.printf("[%10lu] Audio: recording task started\n",
                static_cast<unsigned long>(millis()));
  if (!beginI2s(true)) {
    setError("Could not start the ReSpeaker I2S input");
    return false;
  }

  size_t offset = 0;
  uint32_t emptyReads = 0;
  uint32_t nextProgressBytes = app_config::kAudioBytesPerSecond;
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
    if (offset >= nextProgressBytes) {
      Serial.printf("[%10lu] Audio RX: %u bytes captured (%.1f s)\n",
                    static_cast<unsigned long>(millis()),
                    static_cast<unsigned int>(offset),
                    static_cast<double>(offset) /
                        app_config::kAudioBytesPerSecond);
      nextProgressBytes += app_config::kAudioBytesPerSecond;
    }
  }

  endI2s();
  updateCapturedBytes(offset - (offset % kAudioFrameBytes));
  const size_t sampleCount = offset / sizeof(int32_t);
  const int32_t* samples = reinterpret_cast<const int32_t*>(audioBuffer_);
  uint32_t peak[2] = {};
  size_t nonZero[2] = {};
  for (size_t index = 0; index < sampleCount; ++index) {
    const uint8_t channel = index % app_config::kAudioChannels;
    const int64_t signedSample = samples[index];
    const uint32_t magnitude = static_cast<uint32_t>(
        signedSample < 0 ? -signedSample : signedSample);
    peak[channel] = max(peak[channel], magnitude);
    if (samples[index] != 0) {
      ++nonZero[channel];
    }
  }
  const size_t frames = sampleCount / app_config::kAudioChannels;
  Serial.printf(
      "[%10lu] Audio RX signal: frames=%u ch0_peak=%u ch0_nonzero=%u/%u "
      "ch1_peak=%u ch1_nonzero=%u/%u\n",
      static_cast<unsigned long>(millis()), static_cast<unsigned int>(frames),
      static_cast<unsigned int>(peak[0]),
      static_cast<unsigned int>(nonZero[0]), static_cast<unsigned int>(frames),
      static_cast<unsigned int>(peak[1]),
      static_cast<unsigned int>(nonZero[1]), static_cast<unsigned int>(frames));

  const uint32_t strongestPeak = max(peak[0], peak[1]);
  uint32_t playbackGain = 1;
  if (strongestPeak > 0 && strongestPeak < kPlaybackTargetPeak) {
    playbackGain = min(kMaximumPlaybackGain,
                       kPlaybackTargetPeak / strongestPeak);
  }
  if (playbackGain > 1) {
    int32_t* writableSamples = reinterpret_cast<int32_t*>(audioBuffer_);
    for (size_t index = 0; index < sampleCount; ++index) {
      const int64_t scaled =
          static_cast<int64_t>(writableSamples[index]) * playbackGain;
      writableSamples[index] = static_cast<int32_t>(
          constrain(scaled, static_cast<int64_t>(INT32_MIN),
                    static_cast<int64_t>(INT32_MAX)));
    }
  }
  Serial.printf(
      "[%10lu] Audio playback normalization: source_peak=%u gain=%ux "
      "target_peak=%u\n",
      static_cast<unsigned long>(millis()),
      static_cast<unsigned int>(strongestPeak),
      static_cast<unsigned int>(playbackGain),
      static_cast<unsigned int>(kPlaybackTargetPeak));
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

  // Keep BCLK/WS present while using the XVF3800 I2C control plane. The
  // 16 kHz slave image can NACK or ignore GPO writes when its I2S clocks stop.
  if (!beginI2s(false)) {
    setError("Could not start the ReSpeaker I2S output");
    return false;
  }
  i2s_.write(gPlaybackSilence, sizeof(gPlaybackSilence));
  delay(30);

  // initializeSpeakerPath() already verified that X0D31 booted low (enabled).
  // Avoid I2C traffic here: the XVF3800 and XIAO share this multi-master bus
  // while audio clocks are active, and repeated control writes are unnecessary.
  Serial.printf("[%10lu] Audio: using boot-time speaker amplifier state\n",
                static_cast<unsigned long>(millis()));

  Serial.printf("Audio: playing %u bytes\n",
                static_cast<unsigned int>(bytesToPlay));
  size_t offset = 0;
  uint32_t emptyWrites = 0;
  uint32_t nextProgressBytes = app_config::kAudioBytesPerSecond;
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
        setError("The ReSpeaker stopped accepting playback samples");
        return false;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    emptyWrites = 0;
    offset += written;
    if (offset >= nextProgressBytes) {
      Serial.printf("[%10lu] Audio TX: %u/%u bytes played (%.1f s)\n",
                    static_cast<unsigned long>(millis()),
                    static_cast<unsigned int>(offset),
                    static_cast<unsigned int>(bytesToPlay),
                    static_cast<double>(offset) /
                        app_config::kAudioBytesPerSecond);
      nextProgressBytes += app_config::kAudioBytesPerSecond;
    }
  }

  // A short zero tail prevents the final sample from being held at the output.
  i2s_.write(gPlaybackSilence, sizeof(gPlaybackSilence));
  delay(30);
  endI2s();

  setState(AudioState::Ready);
  Serial.printf("Audio: playback ended at %u bytes\n",
                static_cast<unsigned int>(offset));
  return true;
}

bool AudioEngine::playDiagnosticTone() {
  if (!beginI2s(false)) {
    setError("Could not start I2S for the speaker test");
    return false;
  }

  i2s_.write(gPlaybackSilence, sizeof(gPlaybackSilence));
  delay(30);
  Serial.printf(
      "[%10lu] Audio speaker test: amplifier was enabled during boot; "
      "skipping playback-time I2C control\n",
      static_cast<unsigned long>(millis()));

  const uint32_t totalFrames =
      (app_config::kAudioSampleRate * kTestToneDurationMs) / 1000;
  const uint32_t halfPeriodFrames =
      app_config::kAudioSampleRate / (kTestToneFrequencyHz * 2);
  uint32_t framesWritten = 0;
  Serial.printf(
      "[%10lu] Audio speaker test: %lu Hz square wave, %lu ms, "
      "amplitude=%ld\n",
      static_cast<unsigned long>(millis()),
      static_cast<unsigned long>(kTestToneFrequencyHz),
      static_cast<unsigned long>(kTestToneDurationMs),
      static_cast<long>(kTestToneAmplitude));

  while (framesWritten < totalFrames) {
    if ((xEventGroupGetBits(stopEvents_) & kStopPlaybackBit) != 0) {
      break;
    }
    const size_t chunkFrames = min(
        kTestToneFramesPerChunk,
        static_cast<size_t>(totalFrames - framesWritten));
    for (size_t frame = 0; frame < chunkFrames; ++frame) {
      const int32_t sample =
          ((framesWritten + frame) / halfPeriodFrames) % 2 == 0
              ? kTestToneAmplitude
              : -kTestToneAmplitude;
      for (uint8_t channel = 0; channel < app_config::kAudioChannels;
           ++channel) {
        gTestTone[frame * app_config::kAudioChannels + channel] = sample;
      }
    }
    const size_t bytesRequested =
        chunkFrames * app_config::kAudioChannels * sizeof(int32_t);
    const size_t bytesWritten = i2s_.write(
        reinterpret_cast<const uint8_t*>(gTestTone), bytesRequested);
    if (bytesWritten != bytesRequested) {
      endI2s();
      setError("The ReSpeaker stopped accepting the speaker test tone");
      return false;
    }
    framesWritten += chunkFrames;
  }

  i2s_.write(gPlaybackSilence, sizeof(gPlaybackSilence));
  delay(30);
  endI2s();
  setState(AudioState::Ready);
  Serial.printf("[%10lu] Audio speaker test ended after %lu frames\n",
                static_cast<unsigned long>(millis()),
                static_cast<unsigned long>(framesWritten));
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
  // AudioTools 1.2.5's legacy ESP32 driver uses pin_data (not pin_data_rx)
  // as the physical input when configured for RX_MODE.  Leaving GPIO44 here
  // records the XIAO's outgoing data line instead of the XVF3800 microphone.
  config.pin_data = receive ? app_config::kI2sDataFromRespeakerPin
                            : app_config::kI2sDataToRespeakerPin;
  config.pin_data_rx = app_config::kI2sDataFromRespeakerPin;
  config.is_master = true;
  Serial.printf(
      "[%10lu] Audio I2S: begin %s, master=yes, BCLK=%d WS=%d "
      "activeData=%d (TX=%d RX=%d)\n",
      static_cast<unsigned long>(millis()), receive ? "RX" : "TX",
      config.pin_bck, config.pin_ws, config.pin_data,
      app_config::kI2sDataToRespeakerPin,
      app_config::kI2sDataFromRespeakerPin);
  i2sRunning_ = i2s_.begin(config);
  Serial.printf("[%10lu] Audio I2S: begin result=%s\n",
                static_cast<unsigned long>(millis()),
                i2sRunning_ ? "success" : "FAILED");
  return i2sRunning_;
}

void AudioEngine::endI2s() {
  if (i2sRunning_) {
    Serial.printf("[%10lu] Audio I2S: stopping\n",
                  static_cast<unsigned long>(millis()));
  }
  i2s_.end();
  i2sRunning_ = false;
}

bool AudioEngine::initializeSpeakerPath() {
  speakerVolumePercent_ = app_config::kDefaultSpeakerVolumePercent;
  Serial.printf("[%10lu] Audio: probing XVF3800 at 0x%02X\n",
                static_cast<unsigned long>(millis()),
                app_config::kXvf3800I2cAddress);
  bool xvfFound = false;
  constexpr uint8_t kProbeAttempts = 20;
  for (uint8_t attempt = 1; attempt <= kProbeAttempts; ++attempt) {
    if (probeI2cAddress(app_config::kXvf3800I2cAddress)) {
      Serial.printf("[%10lu] Audio: XVF3800 detected on attempt %u/%u\n",
                    static_cast<unsigned long>(millis()), attempt,
                    kProbeAttempts);
      xvfFound = true;
      break;
    }
    if (attempt < kProbeAttempts) {
      delay(250);
    }
  }
  if (!xvfFound) {
    Serial.println("Audio: XVF3800 did not answer at I2C address 0x2C");
    printI2cScan();
    setError("XVF3800 not found at 0x2C; check its firmware and soldered connection");
    return false;
  }
  readXvfVersion();

  // Do not force X0D31 high here. The XVF3800 I2S firmware normally boots
  // with its speaker path enabled, and Seeed's standalone record/playback
  // example leaves that GPO under firmware control.
  uint8_t initialGpo[5] = {};
  uint8_t initialGpoStatus = 0xFF;
  if (readXvfGpoValues(initialGpo, &initialGpoStatus)) {
    Serial.printf(
        "[%10lu] XVF3800 initial GPO: status=0x%02X X0D11=%u X0D30=%u "
        "X0D31=%u X0D33=%u X0D39=%u\n",
        static_cast<unsigned long>(millis()), initialGpoStatus, initialGpo[0],
        initialGpo[1], initialGpo[2], initialGpo[3], initialGpo[4]);
  } else {
    Serial.printf(
        "[%10lu] Audio warning: initial GPO read was busy; continuing with "
        "the I2S firmware defaults\n",
        static_cast<unsigned long>(millis()));
  }

  if (!probeI2cAddress(app_config::kAic3104I2cAddress)) {
    Serial.println(
        "Audio warning: AIC3104 was not visible at 0x18; using the XVF3800 firmware's codec configuration");
    return true;
  }

  const uint8_t initialCodecVolume =
      volumePercentToCodecValue(speakerVolumePercent_);
  const bool codecConfigured =
      writeCodecRegister(kCodecPageControlRegister, 0x00) &&
      writeCodecRegister(kCodecLeftDacVolumeRegister, initialCodecVolume) &&
      writeCodecRegister(kCodecRightDacVolumeRegister, initialCodecVolume) &&
      writeCodecRegister(kCodecLeftLineOutputRegister,
                         kCodecLineOutputEnabled) &&
      writeCodecRegister(kCodecRightLineOutputRegister,
                         kCodecLineOutputEnabled);
  if (!codecConfigured) {
    Serial.println(
        "Audio warning: codec volume setup failed; using its current volume configuration");
  }
  Serial.printf("[%10lu] Audio volume: initialized at %u%%\n",
                static_cast<unsigned long>(millis()),
                speakerVolumePercent_);
  return true;
}

bool AudioEngine::setAmplifierEnabled(bool enabled) {
  const uint8_t requestedLevel = enabled ? 0 : 1;
  Serial.printf("[%10lu] XVF3800: amplifier request enabled=%s, X0D31=%u\n",
                static_cast<unsigned long>(millis()), enabled ? "yes" : "no",
                requestedLevel);
  return writeXvfGpo(kAmplifierEnablePin, requestedLevel);
}

bool AudioEngine::writeXvfGpo(uint8_t pin, uint8_t value) {
  for (uint8_t attempt = 1; attempt <= kI2cAttemptCount; ++attempt) {
    Wire.beginTransmission(app_config::kXvf3800I2cAddress);
    Wire.write(kGpoServicerResourceId);
    Wire.write(kGpoWriteCommandId);
    Wire.write(static_cast<uint8_t>(2));
    Wire.write(pin);
    Wire.write(value);
    const uint8_t result = Wire.endTransmission();
    Serial.printf(
        "[%10lu] XVF3800 I2C TX attempt %u/%u: [14 01 02 %02X %02X] "
        "result=%u (%s), SDA=%d SCL=%d\n",
        static_cast<unsigned long>(millis()), attempt, kI2cAttemptCount, pin,
        value, result, i2cResultName(result),
        digitalRead(app_config::kI2cSdaPin),
        digitalRead(app_config::kI2cSclPin));

    if (result == 0) {
      delay(100);
      uint8_t values[5] = {};
      uint8_t status = 0xFF;
      if (readXvfGpoValues(values, &status)) {
        const bool matches = status == kXvfSuccessStatus &&
                             values[kAmplifierGpoIndex] == value;
        Serial.printf(
            "[%10lu] XVF3800 GPO read-back: status=0x%02X "
            "X0D11=%u X0D30=%u X0D31=%u X0D33=%u X0D39=%u match=%s\n",
            static_cast<unsigned long>(millis()), status, values[0], values[1],
            values[2], values[3], values[4], matches ? "yes" : "NO");
        if (matches) {
          return true;
        }
        Serial.printf(
            "[%10lu] XVF3800: GPO write was acknowledged but has not taken "
            "effect yet%s\n",
            static_cast<unsigned long>(millis()),
            status == kXvfRetryStatus ? " (device requested retry)" : "");
      }
    }
    delay(15 * attempt);
  }
  Serial.printf("[%10lu] XVF3800: GPO write failed after %u attempts\n",
                static_cast<unsigned long>(millis()), kI2cAttemptCount);
  return false;
}

bool AudioEngine::readXvfCommand(uint8_t resourceId, uint8_t commandId,
                                 uint8_t* payload, size_t payloadSize,
                                 uint8_t* responseStatus) {
  const size_t responseSize = payloadSize + 1;
  Wire.beginTransmission(app_config::kXvf3800I2cAddress);
  Wire.write(resourceId);
  Wire.write(commandId | kXvfReadBit);
  Wire.write(static_cast<uint8_t>(responseSize));
  // Seeed's current Arduino reference ends the command with a STOP before
  // requesting the response. This also avoids wedging the ESP32 I2C driver
  // when the XVF3800 is busy as a second bus master for its codec.
  const uint8_t txResult = Wire.endTransmission();
  if (txResult != 0) {
    Serial.printf(
        "[%10lu] XVF3800 I2C read command failed: resource=%u command=%u "
        "result=%u (%s), SDA=%d SCL=%d\n",
        static_cast<unsigned long>(millis()), resourceId, commandId, txResult,
        i2cResultName(txResult), digitalRead(app_config::kI2cSdaPin),
        digitalRead(app_config::kI2cSclPin));
    return false;
  }

  delay(2);
  const size_t received = Wire.requestFrom(
      app_config::kXvf3800I2cAddress, responseSize, true);
  if (received != responseSize || Wire.available() < responseSize) {
    Serial.printf(
        "[%10lu] XVF3800 I2C short response: resource=%u command=%u "
        "wanted=%u received=%u available=%u\n",
        static_cast<unsigned long>(millis()), resourceId, commandId,
        static_cast<unsigned int>(responseSize),
        static_cast<unsigned int>(received),
        static_cast<unsigned int>(Wire.available()));
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }

  *responseStatus = Wire.read();
  for (size_t index = 0; index < payloadSize; ++index) {
    payload[index] = Wire.read();
  }
  return true;
}

bool AudioEngine::readXvfGpoValues(uint8_t values[5],
                                    uint8_t* responseStatus) {
  return readXvfCommand(kGpoServicerResourceId, kGpoReadCommandId, values, 5,
                        responseStatus);
}

bool AudioEngine::readXvfVersion() {
  uint8_t version[3] = {};
  uint8_t status = 0xFF;
  if (!readXvfCommand(kVersionResourceId, kVersionCommandId, version,
                      sizeof(version), &status)) {
    Serial.printf("[%10lu] XVF3800 warning: VERSION read transport failed\n",
                  static_cast<unsigned long>(millis()));
    return false;
  }
  Serial.printf("[%10lu] XVF3800 VERSION: status=0x%02X firmware=%u.%u.%u\n",
                static_cast<unsigned long>(millis()), status, version[0],
                version[1], version[2]);
  return status == kXvfSuccessStatus;
}

bool AudioEngine::writeCodecRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(app_config::kAic3104I2cAddress);
  Wire.write(reg);
  Wire.write(value);
  const uint8_t result = Wire.endTransmission();
  Serial.printf(
      "[%10lu] Codec I2C TX: address=0x%02X register=0x%02X value=0x%02X "
      "result=%u (%s)\n",
      static_cast<unsigned long>(millis()), app_config::kAic3104I2cAddress,
      reg, value, result, i2cResultName(result));
  return result == 0;
}

bool AudioEngine::probeI2cAddress(uint8_t address) {
  Wire.beginTransmission(address);
  const uint8_t result = Wire.endTransmission();
  Serial.printf("[%10lu] I2C probe: address=0x%02X result=%u (%s)\n",
                static_cast<unsigned long>(millis()), address, result,
                i2cResultName(result));
  return result == 0;
}

void AudioEngine::setState(AudioState state) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const AudioState previous = state_;
  state_ = state;
  xSemaphoreGive(mutex_);
  if (previous != state) {
    Serial.printf("[%10lu] Audio state: %s -> %s\n",
                  static_cast<unsigned long>(millis()), stateName(previous),
                  stateName(state));
  }
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
