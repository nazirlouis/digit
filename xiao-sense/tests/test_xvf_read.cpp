// Native regression tests for the exact transaction helper used by firmware.
#include "../src/xvf_read.h"
#include <cassert>
#include <cstdio>
#include <vector>

struct Wire {
  unsigned time = 0, began = 0, pendingSince = 0;
  int pending = -1;
  bool asynchronous = false;
  unsigned readCount = 0, writeCount = 0;
  uint8_t writeFailure = 0;
  int shortAtRead = -1;
  size_t offset = 0;
  std::vector<uint8_t> tx, rx, statuses;
  std::vector<unsigned> commands;

  void beginTransmission(uint8_t address) { assert(address == 0x2C); tx.clear(); }
  size_t write(uint8_t byte) { tx.push_back(byte); return 1; }
  uint8_t endTransmission() {
    ++writeCount;
    assert(tx.size() == 3 && (tx[1] & 0x80));
    const int command = tx[0] * 256 + (tx[1] & 0x7F);
    commands.push_back(command);
    if (pending != command) { pending = command; pendingSince = time; }
    return writeFailure;
  }
  size_t requestFrom(uint8_t address, size_t count, bool stop) {
    assert(address == 0x2C && stop && count == tx[2]);
    uint8_t status = asynchronous ? (time - pendingSince < 15 ? 0x40 : 0) :
        statuses.empty() ? 0 : statuses[readCount < statuses.size() ? readCount : statuses.size()-1];
    const bool shortRead = static_cast<int>(readCount) == shortAtRead;
    ++readCount;
    rx.assign(shortRead ? count - 1 : count, 0xAB);
    rx[0] = status;
    offset = 0;
    return rx.size();
  }
  size_t available() const { return rx.size() - offset; }
  int read() { assert(available()); return rx[offset++]; }
  void pause(unsigned ms) { time += ms; }
};

static xvf_read::Result read(Wire& wire, uint8_t cmd, uint8_t* payload, size_t length) {
  return xvf_read::command(wire, [&](unsigned ms) { wire.pause(ms); }, 0x2C, 33, cmd, payload, length);
}

// Reproduce the previous caller's one-shot request, including its 50 ms poll.
static bool oldOnce(Wire& wire, uint8_t cmd) {
  wire.beginTransmission(0x2C); wire.write(33); wire.write(cmd | 0x80); wire.write(17);
  wire.endTransmission(); wire.pause(2); wire.requestFrom(0x2C, 17, true);
  const bool ok = wire.read() == 0;
  while (wire.available()) wire.read();
  return ok;
}

int main() {
  {
    Wire wire;
    uint8_t payload[16] = {};
    const auto r = read(wire, 75, payload, sizeof(payload));
    assert(r.transportOk && r.status == 0 && r.attempts == 1);
    for (uint8_t byte : payload) assert(byte == 0xAB);
  }
  {
    Wire old; old.asynchronous = true;
    bool paired = false;
    for (int i = 0; i < 20; ++i) {
      paired |= oldOnce(old, 75) && oldOnce(old, 80);
      old.pause(50);
    }
    assert(!paired);  // Alternating unfinished DSP commands starves the pair.
    Wire fixed; fixed.asynchronous = true;
    uint8_t angles[16] = {}, energy[16] = {};
    const auto a = read(fixed, 75, angles, 16);
    const auto e = read(fixed, 80, energy, 16);
    assert(a.transportOk && a.status == 0 && e.transportOk && e.status == 0);
    assert(a.attempts == 3 && e.attempts == 3);
    assert((fixed.commands == std::vector<unsigned>{8523,8523,8523,8528,8528,8528}));
    assert(fixed.time < 100);
  }
  {
    Wire wire; wire.statuses = {0x40};
    uint8_t payload[4] = {1,2,3,4};
    const auto r = read(wire, 75, payload, 4);
    assert(r.transportOk && r.status == 0x40 && r.attempts == xvf_read::kAttempts);
    assert(wire.time == 86 && wire.readCount == 8 && wire.available() == 0);
    assert(payload[0] == 1 && payload[3] == 4);
  }
  {
    Wire wire; wire.statuses = {0x41};
    uint8_t payload[4] = {1,2,3,4};
    const auto r = read(wire, 75, payload, 4);
    assert(r.transportOk && r.status == 0x41 && r.attempts == 1);
    assert(payload[0] == 1 && payload[3] == 4 && wire.available() == 0);
  }
  {
    Wire wire; wire.writeFailure = 2;
    uint8_t payload[4] = {1,2,3,4};
    const auto r = read(wire, 75, payload, 4);
    assert(!r.transportOk && r.status == 0xFF && r.writeError == 2);
    assert(wire.readCount == 0 && payload[0] == 1);
  }
  for (int failedAttempt : {0,1}) {
    Wire wire; wire.statuses = {0x40,0}; wire.shortAtRead = failedAttempt;
    uint8_t payload[4] = {1,2,3,4};
    const auto r = read(wire, 75, payload, 4);
    assert(!r.transportOk && r.status == 0xFF && wire.available() == 0);
    assert(payload[0] == 1 && payload[3] == 4);
  }
  {
    Wire wire; wire.statuses = {64,64,64,64,64,64,64,0};
    uint8_t payload[4] = {};
    const auto r = read(wire, 75, payload, 4);
    assert(r.transportOk && r.status == 0 && r.attempts == 8 && payload[0] == 0xAB);
  }
  {
    Wire wire; uint8_t oversized[31] = {};
    assert(!read(wire, 75, nullptr, 4).transportOk);
    assert(!read(wire, 75, oversized, 31).transportOk);
    assert(!read(wire, 75, oversized, 0).transportOk);
    assert(wire.writeCount == 0);
  }
  std::puts("PASS: XVF same-command retries, command-pair starvation regression, bounded busy, errors, short reads, payload validation");
}
