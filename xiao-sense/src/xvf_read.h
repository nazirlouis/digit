#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// The XVF control servicer can need more than one transaction to return a
// value. Finish retrying the SAME command before another command is issued.
// Caller holds the shared bus lock for this entire sequence. No audio reads.
namespace xvf_read {
constexpr uint8_t kSuccess = 0;
constexpr uint8_t kRetry = 0x40;
constexpr uint8_t kAttempts = 8;

struct Result {
  bool transportOk = false;
  uint8_t status = 0xFF;
  uint8_t attempts = 0;
  uint8_t writeError = 0;
  size_t received = 0;
};

template <typename Bus, typename Pause>
Result command(Bus& bus, Pause pause, uint8_t address, uint8_t resource,
               uint8_t id, uint8_t* payload, size_t size) {
  Result result;
  if (!payload || size == 0 || size > 30) return result;
  const size_t expected = size + 1;
  uint8_t response[31];
  for (uint8_t attempt = 1; attempt <= kAttempts; ++attempt) {
    if (attempt > 1) pause(10);
    result.attempts = attempt;
    result.transportOk = false;
    result.status = 0xFF;
    bus.beginTransmission(address);
    bus.write(resource);
    bus.write(id | 0x80);
    bus.write(static_cast<uint8_t>(expected));
    result.writeError = bus.endTransmission();  // STOP: keep the working Flex transport.
    if (result.writeError) return result;
    pause(2);
    result.received = bus.requestFrom(address, expected, true);
    if (result.received != expected || bus.available() < expected) {
      while (bus.available()) bus.read();
      return result;
    }
    // Drain each response, including busy/error payloads. Never expose those
    // bytes as measurements or replace the caller's last good payload.
    for (size_t i = 0; i < expected; ++i) response[i] = static_cast<uint8_t>(bus.read());
    result.transportOk = true;
    result.status = response[0];
    if (result.status == kSuccess) {
      std::memcpy(payload, response + 1, size);
      return result;
    }
    if (result.status != kRetry) return result;
  }
  return result;  // Bounded busy timeout; caller must check status == 0.
}
}
