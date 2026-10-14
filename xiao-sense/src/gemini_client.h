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
// Changes after a saved key/model/instructions replacement.
uint32_t configurationRevision();
// Copy effective instructions without exposing credentials or adding large
// returned values to the main/audio task stacks.
bool copySystemInstructions(char* output, size_t capacity);
// Caller owns this exact-sized copy and releases it with free().
char* duplicateSystemInstructions();
// Physical flash capacity for one atomically replaceable instruction document.
size_t systemInstructionsCapacity();
void live(const gemini_live::Callbacks& callbacks, bool probe);
// Blank key/model retain the stored value. clearKey removes the saved key.
// Keys are persisted in NVS and are never returned by settings().
bool configure(const char* apiKey, const char* model, bool dryRun,
               bool clearKey, char* error, size_t errorSize,
               uint32_t idleTimeoutSec = 0, const char* systemInstructions = nullptr);
// Omitted instructions retain the saved text; empty text restores the default.
}  // namespace gemini_client
