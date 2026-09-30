#include "audio_engine.h"

#include <Wire.h>
#include <driver/pcnt.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <cstring>

#include "app_config.h"
#include "wake_word.h"
#include "xvf_read.h"

namespace {

SemaphoreHandle_t busMutex = nullptr;
// Includes the XVF command/STOP/delay/read sequence, not just each transfer.
class BusLock {
 public:
  BusLock() { if (busMutex) xSemaphoreTakeRecursive(busMutex, portMAX_DELAY); }
  ~BusLock() { if (busMutex) xSemaphoreGiveRecursive(busMutex); }
};

constexpr uint8_t kGpoServicerResourceId = 20;
constexpr uint8_t kGpoReadCommandId = 0;
constexpr uint8_t kGpoWriteCommandId = 1;
constexpr uint8_t kAmplifierEnablePin = 31;
constexpr uint8_t kAmplifierGpoIndex = 2;
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
constexpr uint32_t kPlaybackTargetPeak = 0x10000000;
constexpr uint32_t kMaximumPlaybackGain = 65536;
constexpr uint32_t kTestToneFrequencyHz = 440;
constexpr uint32_t kTestToneDurationMs = 2000;
constexpr int32_t kTestToneAmplitude = 0x08000000;
constexpr size_t kStreamFrames = wake_word::kFrameSamples;
constexpr uint32_t kChimeFrames = app_config::kAudioSampleRate * 400 / 1000;
int32_t gInput[kStreamFrames * 2] = {};
int32_t gOutput[kStreamFrames * 2] = {};

int32_t toneSample(uint32_t frame, uint32_t total, uint32_t frequency) {
  if (frame >= total) return 0;
  const uint32_t fade = app_config::kAudioSampleRate / 50;
  const float envelope = static_cast<float>(min(min(frame, total - 1 - frame), fade)) / fade;
  const float phase = 6.28318530718f * ((frame * frequency) % app_config::kAudioSampleRate) /
                      app_config::kAudioSampleRate;
  return static_cast<int32_t>(sinf(phase) * envelope * kTestToneAmplitude);
}

int32_t chimeSample(uint32_t frame) {
  const uint32_t noteFrames = kChimeFrames / 2;
  return toneSample(frame % noteFrames, noteFrames, frame < noteFrames ? 660 : 990);
}

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
  BusLock lock;
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
  busMutex = xSemaphoreCreateRecursiveMutex();
  commandQueue_ = xQueueCreate(2, sizeof(Command));
  stopEvents_ = xEventGroupCreate();
  if (mutex_ == nullptr || busMutex == nullptr || commandQueue_ == nullptr || stopEvents_ == nullptr) {
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

  // The old XVF3800 slave image needs XIAO clocks, but Flex firmware may
  // already supply them. Measure WS with both XIAO clock pins as inputs first.
  if (!detectI2sClockSource()) {
    return false;
  }
  if (!beginI2s()) {
    setError("Could not start initial I2S clocks for the ReSpeaker");
    return false;
  }
  delay(250);
  if (!initializeSpeakerPath()) {
    endI2s();
    return false;
  }
  // Keep both directions clocked for the lifetime of the audio task.

  xSemaphoreTake(mutex_, portMAX_DELAY);
  initialized_ = true;
  state_ = AudioState::Ready;
  error_[0] = '\0';
  xSemaphoreGive(mutex_);
  if (xTaskCreatePinnedToCore(taskEntry, "digit-audio", 8192, this, 3, &task_, 1) != pdPASS) {
    endI2s();
    setError("Could not start the audio task");
    return false;
  }

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
  if (currentState != AudioState::Playing && !snapshot().wakeChimePlaying) {
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
  status.wakeChimePlaying = wakeChimePlaying_;
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

bool AudioEngine::playWakeChime() {
  if (!snapshot().available) return false;
  xEventGroupSetBits(stopEvents_, kWakeChimeBit);
  return true;
}

void AudioEngine::taskLoop() {
  enum class Mode { Idle, Record, Play, Tone };
  Mode mode = Mode::Idle;
  size_t recordOffset = 0, playOffset = 0;
  uint32_t recordingPeak = 0, playbackGain = 1, toneFrame = 0;
  uint32_t chimeFrame = kChimeFrames;
  uint32_t emptyReads = 0;
  for (;;) {
    Command command;
    if (xQueueReceive(commandQueue_, &command, 0) == pdTRUE) {
      if (command == Command::Record) {
        recordOffset = 0;
        recordingPeak = 0;
        mode = Mode::Record;
      } else if (command == Command::Play) {
        playOffset = 0;
        mode = Mode::Play;
      } else if (command == Command::Tone) {
        toneFrame = 0;
        mode = Mode::Tone;
      }
    }
    const EventBits_t events = xEventGroupClearBits(stopEvents_, kWakeChimeBit | kStopPlaybackBit);
    if (events & kWakeChimeBit) chimeFrame = 0;
    if (events & kStopPlaybackBit) {
      chimeFrame = kChimeFrames;
      if (mode == Mode::Play || mode == Mode::Tone) {
        mode = Mode::Idle;
        setState(AudioState::Ready);
      }
    }

    const size_t received = i2s_.readBytes(reinterpret_cast<uint8_t*>(gInput), sizeof(gInput));
    if (!received) {
      if (++emptyReads >= 20) {
        endI2s();
        setError("No microphone samples from the ReSpeaker");
        vTaskDelete(nullptr);
        return;
      }
      continue;
    }
    emptyReads = 0;
    if (received % kAudioFrameBytes) {
      endI2s();
      setError("Incomplete stereo microphone frame");
      vTaskDelete(nullptr);
      return;
    }
    const size_t frames = received / kAudioFrameBytes;
    wake_word::submit(gInput, frames);

    if (mode == Mode::Record) {
      const bool stop = (xEventGroupGetBits(stopEvents_) & kStopRecordingBit) != 0;
      if (!stop) {
        const size_t count = min(received, app_config::kAudioBufferBytes - recordOffset);
        memcpy(audioBuffer_ + recordOffset, gInput, count);
        for (size_t i = 0; i < count / sizeof(int32_t); ++i) {
          const int64_t value = gInput[i];
          recordingPeak = max(recordingPeak, static_cast<uint32_t>(value < 0 ? -value : value));
        }
        recordOffset += count;
        updateCapturedBytes(recordOffset);
      }
      if (stop || recordOffset == app_config::kAudioBufferBytes) {
        playbackGain = recordingPeak && recordingPeak < kPlaybackTargetPeak
                           ? min(kMaximumPlaybackGain, kPlaybackTargetPeak / recordingPeak) : 1;
        xSemaphoreTake(mutex_, portMAX_DELAY);
        recordingStartedAtMs_ = 0;
        xSemaphoreGive(mutex_);
        playOffset = 0;
        mode = recordOffset ? Mode::Play : Mode::Idle;
        setState(recordOffset ? AudioState::Playing : AudioState::Ready);
        Serial.printf("Audio: captured %u bytes, peak=%u, playback gain=%u\n",
                      (unsigned)recordOffset, (unsigned)recordingPeak, (unsigned)playbackGain);
      }
    }

    const size_t recordedBytes = snapshot().capturedBytes;
    for (size_t frame = 0; frame < frames; ++frame) {
      const int32_t chime = chimeFrame < kChimeFrames ? chimeSample(chimeFrame++) : 0;
      int32_t tone = 0;
      if (mode == Mode::Tone) {
        const uint32_t total = app_config::kAudioSampleRate * kTestToneDurationMs / 1000;
        tone = toneSample(toneFrame++, total, kTestToneFrequencyHz);
        if (toneFrame >= total) { mode = Mode::Idle; setState(AudioState::Ready); }
      }
      for (size_t channel = 0; channel < 2; ++channel) {
        int64_t sample = tone;
        if (mode == Mode::Play && playOffset < recordedBytes) {
          int32_t original;
          memcpy(&original, audioBuffer_ + playOffset, sizeof(original));
          sample = static_cast<int64_t>(original) * playbackGain;
          playOffset += sizeof(original);
        }
        // Mix the wake chime over playback without stopping microphone capture.
        gOutput[frame * 2 + channel] = static_cast<int32_t>(
            constrain(sample + chime, static_cast<int64_t>(INT32_MIN), static_cast<int64_t>(INT32_MAX)));
      }
      if (mode == Mode::Play && playOffset >= recordedBytes) {
        mode = Mode::Idle;
        setState(AudioState::Ready);
      }
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    wakeChimePlaying_ = chimeFrame < kChimeFrames;
    xSemaphoreGive(mutex_);
    if (i2s_.write(reinterpret_cast<const uint8_t*>(gOutput), received) != received) {
      endI2s();
      setError("The ReSpeaker stopped accepting speaker samples");
      vTaskDelete(nullptr);
      return;
    }
  }
}

bool AudioEngine::detectI2sClockSource() {
  pinMode(app_config::kI2sBitClockPin, INPUT);
  pinMode(app_config::kI2sWordSelectPin, INPUT);
  pcnt_config_t counter = {};
  counter.pulse_gpio_num = app_config::kI2sWordSelectPin;
  counter.ctrl_gpio_num = PCNT_PIN_NOT_USED;
  counter.lctrl_mode = PCNT_MODE_KEEP;
  counter.hctrl_mode = PCNT_MODE_KEEP;
  counter.pos_mode = PCNT_COUNT_INC;
  counter.neg_mode = PCNT_COUNT_DIS;
  counter.counter_h_lim = 30000;
  counter.counter_l_lim = -1;
  counter.unit = PCNT_UNIT_0;
  counter.channel = PCNT_CHANNEL_0;
  if (pcnt_unit_config(&counter) != ESP_OK) {
    setError("Could not measure ReSpeaker I2S clock source");
    return false;
  }
  // Give externally clocked firmware time to start; never drive against any
  // detected clock, even when its rate does not match this application.
  bool clockFound = false;
  uint32_t externalRate = 0;
  for (uint8_t attempt = 0; attempt < 10 && !clockFound; ++attempt) {
    pcnt_counter_pause(PCNT_UNIT_0);
    pcnt_counter_clear(PCNT_UNIT_0);
    const uint32_t started = micros();
    pcnt_counter_resume(PCNT_UNIT_0);
    delay(50);
    pcnt_counter_pause(PCNT_UNIT_0);
    const uint32_t elapsed = micros() - started;
    int16_t edges = 0;
    pcnt_get_counter_value(PCNT_UNIT_0, &edges);
    clockFound = edges > 0;
    if (clockFound) {
      externalRate = static_cast<uint32_t>(
          static_cast<uint64_t>(edges) * 1000000ULL / elapsed);
    }
  }
  pcnt_counter_pause(PCNT_UNIT_0);
  pcnt_set_mode(PCNT_UNIT_0, PCNT_CHANNEL_0, PCNT_COUNT_DIS,
                PCNT_COUNT_DIS, PCNT_MODE_KEEP, PCNT_MODE_KEEP);
  xiaoI2sMaster_ = !clockFound;
  Serial.printf("Audio clock probe: external WS=%lu Hz; clock source=%s\n",
                static_cast<unsigned long>(externalRate),
                xiaoI2sMaster_ ? "XIAO (master)" : "ReSpeaker (XIAO slave)");
  if (clockFound &&
      (externalRate < app_config::kAudioSampleRate * 95 / 100 ||
       externalRate > app_config::kAudioSampleRate * 105 / 100)) {
    setError("External I2S clock is not 16 kHz; check ReSpeaker firmware");
    return false;
  }
  return true;
}

bool AudioEngine::beginI2s() {
  endI2s();
  auto config = i2s_.defaultConfig(RXTX_MODE);
  config.copyFrom(audio_tools::AudioInfo(app_config::kAudioSampleRate,
                                       app_config::kAudioChannels, app_config::kAudioBitsPerSample));
  config.pin_bck = app_config::kI2sBitClockPin;
  config.pin_ws = app_config::kI2sWordSelectPin;
  config.pin_data = app_config::kI2sDataToRespeakerPin;
  config.pin_data_rx = app_config::kI2sDataFromRespeakerPin;
  config.is_master = xiaoI2sMaster_;
  config.auto_clear = true;
  i2sRunning_ = i2s_.begin(config);
  i2s_.setWaitTimeReadMs(100);
  i2s_.setWaitTimeWriteMs(100);
  Serial.printf("Audio: full duplex 16 kHz RX=%d TX=%d, XIAO master=%s, ready=%s\n",
                config.pin_data_rx, config.pin_data, config.is_master ? "yes" : "no",
                i2sRunning_ ? "yes" : "no");
  return i2sRunning_;
}

void AudioEngine::endI2s() {
  if (i2sRunning_) {
    Serial.printf("[%10lu] Audio I2S: stopping\n",
                  static_cast<unsigned long>(millis()));
    i2s_.end();
  }
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
  logCodecDiagnostics();
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
  BusLock lock;
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
  BusLock lock;
  if (!responseStatus) return false;
  const auto result = xvf_read::command(Wire, [](uint32_t ms) { delay(ms); },
      app_config::kXvf3800I2cAddress, resourceId, commandId, payload, payloadSize);
  *responseStatus = result.status;
  if (!result.transportOk) {
    Serial.printf("XVF3800 read transport failed: resource=%u command=%u TX=%u RX=%u/%u SDA=%d SCL=%d\n",
                  resourceId, commandId, result.writeError, (unsigned)result.received,
                  (unsigned)(payloadSize + 1), digitalRead(app_config::kI2cSdaPin),
                  digitalRead(app_config::kI2cSclPin));
  }
  return result.transportOk;
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

void AudioEngine::logCodecDiagnostics() {
  BusLock lock;
  // Page 0 is selected by our volume initialization. Read the live clock,
  // serial format, DAC routing and output settings; do not overwrite them.
  constexpr uint8_t registers[] = {
      0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 37, 40, 43, 44,
      47, 51, 64, 65, 82, 85, 86, 89, 92, 93, 94, 95, 101, 102};
  Serial.println("Audio codec snapshot (register=value):");
  for (uint8_t reg : registers) {
    Wire.beginTransmission(app_config::kAic3104I2cAddress);
    Wire.write(reg);
    const uint8_t result = Wire.endTransmission(false);
    if (result != 0 ||
        Wire.requestFrom(app_config::kAic3104I2cAddress,
                         static_cast<uint8_t>(1)) != 1) {
      Serial.printf("  %02X=READ_FAILED\n", reg);
      continue;
    }
    const uint8_t value = Wire.read();
    Serial.printf("  %02X=%02X\n", reg, value);
    if (reg == 0 && value != 0) {
      Serial.println("Audio codec snapshot stopped: unexpected register page");
      return;
    }
  }
  uint8_t packed = 0;
  uint8_t status = 0xFF;
  if (readXvfCommand(35, 10, &packed, 1, &status)) {
    Serial.printf("Audio XVF3800 I2S_INPUT_PACKED=%u status=%u\n", packed, status);
  }
}

bool AudioEngine::writeCodecRegister(uint8_t reg, uint8_t value) {
  BusLock lock;
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
  BusLock lock;
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
  wake_word::audioFault();
  Serial.printf("Audio error: %s\n", message);
}

void AudioEngine::updateCapturedBytes(size_t bytes) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  capturedBytes_ = bytes;
  xSemaphoreGive(mutex_);
}
