#include "voice_direction.h"
#include "audio_engine.h"
#include "wake_word.h"
#include "zumo_link.h"
#include <cmath>
#include <cstring>

namespace {
constexpr float kPi = 3.14159265359f;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
voice_direction::Status state = {};
float front = 0;
int orientation = 1;
uint32_t sampledAt = 0;
bool hasSample = false;

float wrap(float value) {
  while (value >= 180) value -= 360;
  while (value < -180) value += 360;
  return value;
}

// Use the auto-selected speech beam and its matching energy. Fall back only
// for firmware that implements DOA_VALUE instead of the raw AEC commands.
bool readBearing(float& degrees, bool& speech, int& mode, uint8_t& code,
                 uint8_t& resource, uint8_t& command) {
  auto read = [&](uint8_t r, uint8_t c, uint8_t* data, size_t size) {
    resource = r; command = c;
    return audioEngine.readXvfCommand(r, c, data, size, &code) && code == 0;
  };
  if (mode != 2) {
    float azimuths[4], energy[4];
    if (read(33, 75, reinterpret_cast<uint8_t*>(azimuths), 16) &&
        read(33, 80, reinterpret_cast<uint8_t*>(energy), 16)) {
      mode = 1;
      speech = std::isfinite(energy[3]) && energy[3] > 0 && std::isfinite(azimuths[3]) &&
               std::fabs(azimuths[3]) <= 2 * kPi;
      degrees = speech ? azimuths[3] * 180 / kPi : 0;
      return true;
    }
    if (mode == 1 || code == 64 || code == 255) return false;
  }
  uint8_t data[4];
  if (!read(20, 18, data, 4)) return false;
  const unsigned angle = data[0] | (data[1] << 8);
  const unsigned vad = data[2] | (data[3] << 8);
  if (angle > 359 || vad > 1) return false;
  mode = 2;
  degrees = angle;
  speech = vad == 1;
  return true;
}

void task(void*) {
  float samples[5] = {};
  uint32_t sampleTimes[5] = {};
  uint32_t lastFailureLog = 0;
  bool wasOk = false;
  unsigned count = 0, index = 0;
  int mode = 0;
  for (;;) {
    const auto audio = audioEngine.snapshot();
    const auto zumo = zumo_link::snapshot();
    const bool quiet = audio.available && audio.state != AudioState::Playing && !audio.wakeChimePlaying &&
                       !zumo.left && !zumo.right && !zumo.manualDriving && !zumo.lookActive &&
                       !wake_word::snapshot().pausedForVision;
    float degrees = 0;
    bool speech = false;
    uint8_t code = 255, resource = 0, command = 0;
    const uint32_t started = millis();
    const bool ok = quiet && readBearing(degrees, speech, mode, code, resource, command);
    const uint32_t now = millis();
    if (quiet && !ok && (!lastFailureLog || now - lastFailureLog >= 5000)) {
      lastFailureLog = now;
      Serial.printf("Voice direction: resource=%u command=%u status=0x%02X after retries (%lu ms)\n",
                    resource, command, code, (unsigned long)(now - started));
    }
    if (ok && !wasOk) Serial.printf("Voice direction: %s reads ready\n", mode == 1 ? "AEC" : "DOA_VALUE");
    wasOk = ok;
    portENTER_CRITICAL(&lock);
    if (quiet) {
      state.available = ok;
      state.readResource = resource;
      state.readCommand = command;
      state.readStatus = code;
      snprintf(state.source, sizeof(state.source), "%s", mode == 1 ? "AEC" : mode == 2 ? "DOA_VALUE" : "--");
      const char* stage = command == 75 ? "angle" : command == 80 ? "speech energy" : "direction";
      if (!ok && code == 64)
        snprintf(state.error, sizeof(state.error), "ReSpeaker busy reading %s (0x40); retrying", stage);
      else if (!ok)
        snprintf(state.error, sizeof(state.error), "Direction read unavailable (%u/%u, status 0x%02X)", resource, command, code);
      else state.error[0] = '\0';
    }
    if (ok && speech && now - started <= 200) {
      // Reject scattered directions, but average across 359/0 correctly.
      if (count && std::fabs(wrap(degrees - samples[(index + 4) % 5])) > 25) count = 0;
      samples[index] = degrees;
      sampleTimes[index] = started;
      index = (index + 1) % 5;
      if (count < 5) ++count;
      // Retrying consumes time. Do not average old directions into a fresh one.
      while (count && now - sampleTimes[(index + 5 - count) % 5] > 300) --count;
      float x = 0, y = 0;
      for (unsigned i = 0; i < count; ++i) {
        const float angle = samples[(index + 4 - i) % 5] * kPi / 180;
        x += std::cos(angle); y += std::sin(angle);
      }
      state.degrees = std::atan2(y, x) * 180 / kPi;
      if (state.degrees < 0) state.degrees += 360;
      state.stable = count >= 3 && std::sqrt(x*x + y*y) / count > 0.98f;
      sampledAt = started;
      hasSample = true;
    } else {
      count = 0;
      // Silence may briefly follow a spoken wake. A bus error, motor movement,
      // vision, or playback must invalidate the previous direction immediately.
      if (!ok || now - started > 200) { state.stable = false; hasSample = false; }
    }
    portEXIT_CRITICAL(&lock);
    delay(ok || !quiet || code == 64 ? 50 : 1000);
  }
}
}

namespace voice_direction {
void begin() {
  if (xTaskCreatePinnedToCore(task, "voice-direction", 4096, nullptr, 1, nullptr, 1) != pdPASS) {
    portENTER_CRITICAL(&lock);
    snprintf(state.error, sizeof(state.error), "Could not start direction reader");
    portEXIT_CRITICAL(&lock);
  }
}
Status snapshot() {
  portENTER_CRITICAL(&lock);
  Status copy = state;
  copy.ageMs = hasSample ? millis() - sampledAt : UINT32_MAX;
  copy.stable = copy.stable && copy.ageMs <= 300;
  copy.relative = wrap(state.degrees - front) * orientation;
  portEXIT_CRITICAL(&lock);
  return copy;
}
bool configure(const char* action) {
  const auto zumo = zumo_link::snapshot();
  const auto audio = audioEngine.snapshot();
  const bool quiet = !zumo.left && !zumo.right && !zumo.manualDriving && !zumo.lookActive && !zumo.calibrating &&
                     !wake_word::snapshot().pausedForVision && audio.state != AudioState::Playing && !audio.wakeChimePlaying;
  bool accepted = false;
  portENTER_CRITICAL(&lock);
  const bool fresh = quiet && state.available && state.stable && hasSample && millis() - sampledAt <= 300;
  if (!strcmp(action, "off") || !strcmp(action, "reset")) {
    state.enabled = false;
    if (!strcmp(action, "reset")) state.frontSet = state.calibrated = false;
    accepted = true;
  } else if (!strcmp(action, "front") && fresh) {
    front = state.degrees;
    state.frontSet = true;
    state.calibrated = state.enabled = false;
    accepted = true;
  } else if (!strcmp(action, "left") && fresh && state.frontSet) {
    const float difference = wrap(state.degrees - front);
    if (std::fabs(difference) >= 45 && std::fabs(difference) <= 135) {
      orientation = difference > 0 ? 1 : -1;
      state.calibrated = true;
      state.enabled = false;
      accepted = true;
    }
  } else if (!strcmp(action, "on") && quiet && state.calibrated && state.available &&
             zumo.connected && zumo.ready && zumo.voiceCapable) {
    state.enabled = true;
    accepted = true;
  }
  if (accepted) ++state.generation;
  portEXIT_CRITICAL(&lock);
  if (accepted) zumo_link::cancelLook();
  return accepted;
}
bool target(int& degrees, uint32_t& generation) {
  const auto s = snapshot();
  const auto audio = audioEngine.snapshot();
  const auto zumo = zumo_link::snapshot();
  if (!audio.available || audio.state == AudioState::Playing || audio.wakeChimePlaying ||
      zumo.manualDriving || zumo.lookActive || zumo.left || zumo.right) return false;
  if (!s.enabled || !s.calibrated || !s.available || !s.stable) return false;
  degrees = static_cast<int>(std::lround(s.relative));
  generation = s.generation;
  return true;
}
bool accepts(uint32_t generation) {
  portENTER_CRITICAL(&lock);
  const bool accepted = state.enabled && state.calibrated && state.generation == generation;
  portEXIT_CRITICAL(&lock);
  return accepted;
}
}
