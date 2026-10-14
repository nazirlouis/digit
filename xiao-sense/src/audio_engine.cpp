#include "audio_engine.h"

#include <Wire.h>
#include <driver/pcnt.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <cmath>
#include <cstring>

#include "app_config.h"
#include "robot_session.h"
#include "wake_word.h"
#include "local_stop.h"
#include "xvf_read.h"
#include "gesture_chirp.h"
#include "zumo_link.h"

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
constexpr size_t kStreamFrames = 160;
int32_t gInput[kStreamFrames * 2] = {};
int32_t gOutput[kStreamFrames * 2] = {};
int16_t gLiveOutput[kStreamFrames] = {};

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
  if (mutex_ == nullptr || busMutex == nullptr) {
    setError("Could not create audio synchronization objects");
    return false;
  }

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
  readProcessingStatus();

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

  Serial.println("Audio: ready; local wake/Stop, microphone input and R2-D2 emotion sounds");
  return true;
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
  status.speakerVolumePercent = speakerVolumePercent_;
  status.wakeChimePlaying = wakeChimePlaying_;
  status.cuePlaying = cuePlaying_;
  snprintf(status.error, sizeof(status.error), "%s", error_);
  xSemaphoreGive(mutex_);
  return status;
}

void AudioEngine::logDiagnostics() {
  const AudioStatus status = snapshot();
  Serial.printf(
      "[%10lu] Health: audio=%s available=%s captured=%u bytes/%lu ms "
      "I2S=%s freePSRAM=%u freeHeap=%u internal=%u largestInternal=%u error=\"%s\"\n",
      static_cast<unsigned long>(millis()), stateName(status.state),
      status.available ? "yes" : "no",
      static_cast<unsigned int>(status.capturedBytes),
      static_cast<unsigned long>(status.capturedMilliseconds),
      i2sRunning_ ? "running" : "stopped",
      static_cast<unsigned int>(ESP.getFreePsram()),
      static_cast<unsigned int>(ESP.getFreeHeap()),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), status.error);
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

bool AudioEngine::cuePlaying() {
  if (!mutex_) return false;
  xSemaphoreTake(mutex_,portMAX_DELAY);
  const bool playing = cuePlaying_;
  xSemaphoreGive(mutex_);
  return playing;
}
void AudioEngine::cancelOutput() {
  if (!mutex_) return;
  xSemaphoreTake(mutex_,portMAX_DELAY); ++outputGeneration_; xSemaphoreGive(mutex_);
}
void AudioEngine::taskLoop() {
  gesture_chirp::Player chirp;
  uint32_t lastChirpAt=0;
  uint32_t emptyReads=0;
  bool chirpTail=false, wakeTail=false;
  for (;;) {
    xSemaphoreTake(mutex_,portMAX_DELAY); const uint32_t epoch=outputGeneration_; xSemaphoreGive(mutex_);
    const size_t received=i2s_.readBytes(reinterpret_cast<uint8_t*>(gInput),sizeof(gInput));
    if (!received) {
      if (++emptyReads>=20) { endI2s(); setError("No microphone samples from the ReSpeaker"); vTaskDelete(nullptr); return; }
      continue;
    }
    emptyReads=0;
    if (received%kAudioFrameBytes) { endI2s(); setError("Incomplete stereo microphone frame"); vTaskDelete(nullptr); return; }
    const size_t frames=received/kAudioFrameBytes;
    local_stop::submit(gInput,frames);
    wake_word::submit(gInput,frames);
    gesture_chirp::Event event;
    zumo_link::gestureAudio(event.generation,event.expression);
    const auto activity=robot_session::activity();
    const size_t cue=chirp.render(event,activity.liveProbe || (activity.liveActive && activity.dryRun),
                                 epoch,gLiveOutput,frames);
    const size_t rendered=cue;
    for (size_t i=0;i<frames;++i) gOutput[2*i]=gOutput[2*i+1]=i<rendered ? int32_t(gLiveOutput[i])*65536 : 0;
    // Recheck cancellation after rendering, before handing samples to I2S.
    gesture_chirp::Event current;
    zumo_link::gestureAudio(current.generation,current.expression);
    bool cueWritten=cue && current.generation==event.generation && current.expression;
    xSemaphoreTake(mutex_,portMAX_DELAY);
    if (epoch!=outputGeneration_ || (cue && !cueWritten)) {
      memset(gOutput,0,received); cueWritten=false;
    }
    const uint32_t now=millis();
    if (cueWritten) { lastChirpAt=now; chirpTail=true; wakeTail=event.expression==gesture_chirp::kWakeExpression; }
    if (chirpTail && now-lastChirpAt>=180) { chirpTail=false; wakeTail=false; }
    cuePlaying_=chirpTail;
    wakeChimePlaying_=chirpTail && wakeTail;
    xSemaphoreGive(mutex_);
    if (!chirpTail) robot_session::submit(gInput,frames);
    if (i2s_.write(reinterpret_cast<const uint8_t*>(gOutput),received)!=received) {
      endI2s(); setError("The ReSpeaker stopped accepting speaker samples"); vTaskDelete(nullptr); return;
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
  i2s_.setWaitTimeWriteMs(100);
  Serial.printf("Audio: microphone and emotion output 16 kHz RX=%d TX=%d, XIAO master=%s, ready=%s\n",
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

void AudioEngine::readProcessingStatus() {
  auto read = [&](uint8_t resource, uint8_t command, void* payload, size_t bytes) {
    uint8_t status = 0xFF;
    const bool ok = readXvfCommand(resource, command, static_cast<uint8_t*>(payload), bytes, &status) && status == 0;
    if (!ok) Serial.printf("ReSpeaker audit: resource=%u command=%u unavailable (status=%u)\n", resource, command, status);
    return ok;
  };
  auto integer = [&](uint8_t resource, uint8_t command, int& output) {
    int32_t value;
    if (read(resource, command, &value, sizeof(value))) output = value;
  };
  auto scalar = [&](uint8_t resource, uint8_t command, float& output) {
    float value;
    if (read(resource, command, &value, sizeof(value)) && std::isfinite(value)) output = value;
  };
  uint8_t pair[2];
  if (read(35, 15, pair, 2)) { processing_.leftCategory = pair[0]; processing_.leftSource = pair[1]; }
  if (read(35, 19, pair, 2)) { processing_.rightCategory = pair[0]; processing_.rightSource = pair[1]; }
  if (read(35, 13, pair, 2)) { processing_.packedLeft = pair[0]; processing_.packedRight = pair[1]; }
  integer(33, 35, processing_.asrOutput);
  scalar(33, 36, processing_.asrGain);
  uint8_t bypass;
  if (read(33, 70, &bypass, 1)) processing_.aecBypass = bypass;
  integer(17, 10, processing_.agc);
  scalar(17, 13, processing_.agcGain);
  integer(17, 19, processing_.limiter);
  scalar(17, 21, processing_.stationaryNoiseFloor);
  scalar(17, 22, processing_.speechNoiseFloor);
  integer(17, 23, processing_.echoSuppression);
  integer(17, 27, processing_.nonlinearEcho);
  Serial.printf("ReSpeaker audit: L=%d/%d R=%d/%d packed=%d/%d ASR=%d gain=%.3f AEC_bypass=%d PP_AGC=%d gain=%.3f NS=%.3f/%.3f echo=%d/%d limiter=%d\n",
                processing_.leftCategory, processing_.leftSource, processing_.rightCategory, processing_.rightSource,
                processing_.packedLeft, processing_.packedRight, processing_.asrOutput, processing_.asrGain,
                processing_.aecBypass, processing_.agc, processing_.agcGain, processing_.stationaryNoiseFloor,
                processing_.speechNoiseFloor, processing_.echoSuppression, processing_.nonlinearEcho, processing_.limiter);
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
  Serial.printf("Audio error: %s\n", message);
  wake_word::audioFault();
  local_stop::audioFault();
  robot_session::audioFault();
}
