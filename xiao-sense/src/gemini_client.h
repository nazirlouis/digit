#pragma once
#include <cstddef>
#include <cstdint>
#include "gemini_live.h"

namespace gemini_client {
constexpr size_t kMaxApiKeyLength = 511;
struct Settings {
  bool configured;
  char model[64];
  bool dryRun;
  uint32_t idleTimeoutSec;
};
void begin();
Settings settings();
// Changes only after a saved key/model replacement; safe to expose internally.
uint32_t configurationRevision();
void live(const gemini_live::Callbacks& callbacks, bool probe);
// Blank key/model retain the stored value. clearKey removes the saved key.
// Keys are persisted in NVS and are never returned by settings().
bool configure(const char* apiKey, const char* model, bool dryRun,
               bool clearKey, char* error, size_t errorSize,
               uint32_t idleTimeoutSec = 0);
}  // namespace gemini_client
