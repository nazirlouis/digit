// Actual credential validation and persistence, with only ESP-IDF I/O mocked.
#include "../src/gemini_client.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <new>
#include <cstdlib>
#include <algorithm>

using esp_err_t = int;
using nvs_handle_t = int;
constexpr int ESP_OK = 0, NVS_READONLY = 0, NVS_READWRITE = 1;
constexpr int ESP_LOG_WARN = 2, ESP_LOG_NONE = 0, portMAX_DELAY = -1;
struct StaticSemaphore_t { int depth = 0; };
using SemaphoreHandle_t = StaticSemaphore_t*;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) { return storage; }
void xSemaphoreTake(SemaphoreHandle_t handle, int) { assert(handle->depth++ == 0); }
void xSemaphoreGive(SemaphoreHandle_t handle) { assert(--handle->depth == 0); }
void mbedtls_platform_zeroize(void* data, size_t size) { std::memset(data, 0, size); }
void esp_log_level_set(const char*, int) {}

std::vector<unsigned char> persistent, staged;
bool failWrite = false, failCommit = false;
unsigned writes = 0;
int nvs_open(const char* name, int mode, nvs_handle_t* handle) {
  assert(std::strcmp(name, "digit_gemini") == 0);
  *handle = mode;
  return ESP_OK;
}
int nvs_get_blob(nvs_handle_t, const char* name, void* output, size_t* size) {
  assert(std::strcmp(name, "settings") == 0);
  if (persistent.empty()) return -1;
  const size_t capacity = *size;
  *size = persistent.size();
  if (!output) return ESP_OK;
  if (capacity < persistent.size()) return -1;
  std::memcpy(output, persistent.data(), persistent.size());
  return ESP_OK;
}
int nvs_set_blob(nvs_handle_t, const char*, const void* data, size_t size) {
  ++writes;
  if (failWrite) return -1;
  const auto* bytes = static_cast<const unsigned char*>(data);
  staged.assign(bytes, bytes + size);
  return ESP_OK;
}
int nvs_commit(nvs_handle_t) {
  if (failCommit) return -1;
  persistent = staged;
  return ESP_OK;
}
void nvs_close(nvs_handle_t) { staged.clear(); }

constexpr int ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_SUBTYPE_ANY = 0xff;
struct esp_partition_t { size_t size = 0x3a0000; } instructionFlash;
std::vector<unsigned char> flash(instructionFlash.size, 0xff);
bool failErase = false, failFlashWrite = false, failFlashRead = false;
unsigned flashWrites = 0;
const esp_partition_t* esp_partition_find_first(int type, int subtype, const char* label) {
  assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_ANY && !strcmp(label, "gemini_data"));
  return &instructionFlash;
}
int esp_partition_erase_range(const esp_partition_t* part, size_t offset, size_t size) {
  assert(part == &instructionFlash && offset % 4096 == 0 && size % 4096 == 0 && offset + size <= flash.size());
  if (failErase) return -1;
  std::fill(flash.begin() + offset, flash.begin() + offset + size, static_cast<unsigned char>(0xff)); return ESP_OK;
}
int esp_partition_write(const esp_partition_t*, size_t offset, const void* data, size_t size) {
  assert(offset + size <= flash.size()); ++flashWrites;
  // A failed write can leave partially programmed bytes in the inactive slot.
  std::memcpy(flash.data() + offset, data, failFlashWrite ? size / 2 : size);
  return failFlashWrite ? -1 : ESP_OK;
}
int esp_partition_read(const esp_partition_t*, size_t offset, void* data, size_t size) {
  assert(offset + size <= flash.size());
  if (failFlashRead) return -1;
  std::memcpy(data, flash.data() + offset, size); return ESP_OK;
}
uint32_t esp_crc32_le(uint32_t crc, const uint8_t* data, uint32_t length) {
  crc = ~crc;
  for (uint32_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
  }
  return ~crc;
}

// Extracted unchanged by run_gemini.ps1.
#include "gemini_settings_private.inc"
#include "gemini_settings_public.inc"

void reboot() { saved = SavedSettings{}; savedInstructions.clear(); instructionsReady = true; loaded = false; revision = 1; }
template<typename T>
void install(const T& value) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
  persistent.assign(bytes, bytes + sizeof(value));
  reboot();
}

int main() {
  char error[128]{};
  assert(!gemini_client::settings().configured);
  assert(gemini_client::settings().dryRun);

  LegacySettings legacy;
  std::strcpy(legacy.apiKey, "AIza_FAKE_LEGACY_KEY_123456");
  std::strcpy(legacy.model, "gemini-test-model");
  legacy.dryRun = 0;
  install(legacy);
  assert(gemini_client::settings().configured && !gemini_client::settings().dryRun);
  assert(!std::strcmp(gemini_client::settings().model, "gemini-robotics-er-2-streaming-preview"));
  assert(!std::strcmp(saved.apiKey, legacy.apiKey) && saved.version == 5);
  assert(gemini_client::settings().idleTimeoutSec == 30);
  assert(writes == 0 && persistent.size() == sizeof(LegacySettings));
  assert(gemini_client::configure("", "", true, false, error, sizeof(error)));
  assert(persistent.size() == sizeof(SavedSettings));
  reboot();
  assert(gemini_client::settings().configured && gemini_client::settings().dryRun);
  assert(!std::strcmp(saved.apiKey, legacy.apiKey));

  const std::vector<std::string> keys = {"AIza_FAKE_KEY_123456", "AQ.FAKE_KEY_123456",
      "AQ." + std::string(317, 'x'), "AQ." + std::string(gemini_client::kMaxApiKeyLength - 3, 'x')};
  for (const auto& key : keys) {
    assert(gemini_client::configure(key.c_str(), "gemini-robotics-er-2-streaming-preview", false, false, error, sizeof(error), 25));
    assert(!error[0]);
    reboot();
    assert(gemini_client::settings().configured && !gemini_client::settings().dryRun);
    assert(saved.apiKey == key);  // No truncation in RAM or across a reboot.
    assert(gemini_client::settings().idleTimeoutSec == 25);
    assert(gemini_client::configure("", "", true, false, error, sizeof(error)));
    assert(saved.apiKey == key && gemini_client::settings().dryRun);
  }

  const auto prior = persistent;
  const std::string priorKey = saved.apiKey;
  const auto writesBefore = writes;
  for (uint32_t timeout : {1u, 9u, 301u, 0xffffffffu}) {
    assert(!gemini_client::configure("", "", true, false, error, sizeof(error), timeout));
    assert(persistent == prior && writes == writesBefore);
  }
  for (const char* model : {"gemini-3.8-flash", "gemini-test-model", "models/gemini-robotics-er-2-streaming-preview", "gemini-3.1-flash-live-preview"})
    assert(!gemini_client::configure("", model, true, false, error, sizeof(error)));
  const std::vector<std::string> badKeys = {"AQ.short", "AQ.FAKE KEY_123456", "\"AQ.FAKE_KEY_123456\"",
      "AQ.FAKE_KEY_123456\r\nHeader:value", "AQ.FAKE_KEY_123456/", std::string(512, 'x')};
  for (const auto& key : badKeys) {
    assert(!gemini_client::configure(key.c_str(), "", true, false, error, sizeof(error)));
    assert(error[0] && saved.apiKey == priorKey && persistent == prior && writes == writesBefore);
  }
  assert(!gemini_client::configure("AQ.FAKE_KEY_123456", "", true, true, error, sizeof(error)));
  failWrite = true;
  assert(!gemini_client::configure("AQ.NEW_FAKE_KEY_123456", "", true, false, error, sizeof(error)));
  failWrite = false; failCommit = true;
  assert(!gemini_client::configure("AQ.NEW_FAKE_KEY_123456", "", true, false, error, sizeof(error)));
  failCommit = false;
  assert(saved.apiKey == priorKey && persistent == prior);
  reboot();
  assert(gemini_client::settings().configured && saved.apiKey == priorKey);
  assert(gemini_client::configure("", "", true, true, error, sizeof(error)));
  reboot();
  assert(!gemini_client::settings().configured);

  PreviousSettings previous;
  std::strcpy(previous.apiKey, "AQ.FAKE_PREVIOUS_KEY_123456");
  std::strcpy(previous.model, "gemini-3.8-flash");
  previous.dryRun = 0;
  install(previous);
  assert(gemini_client::settings().configured && !gemini_client::settings().dryRun);
  assert(!std::strcmp(gemini_client::settings().model,"gemini-robotics-er-2-streaming-preview"));
  assert(gemini_client::settings().idleTimeoutSec == 30);
  for (uint32_t timeout : {10u,300u}) {
    assert(gemini_client::configure("", "", false, false, error, sizeof(error), timeout));
    reboot(); assert(gemini_client::settings().idleTimeoutSec == timeout);
  }

  auto corrupt = legacy; corrupt.version = 99;
  install(corrupt); assert(!gemini_client::settings().configured);
  corrupt = legacy; std::memset(corrupt.apiKey, 'x', sizeof(corrupt.apiKey));
  install(corrupt); assert(!gemini_client::settings().configured);
  corrupt = legacy; std::memset(corrupt.model, 'x', sizeof(corrupt.model));
  install(corrupt); assert(!gemini_client::settings().configured);
  SavedSettings newCorrupt; newCorrupt.version = 1;
  std::strcpy(newCorrupt.apiKey, "AQ.FAKE_KEY_123456");
  install(newCorrupt); assert(!gemini_client::settings().configured);
  SavedSettings corruptTimeout; corruptTimeout.idleTimeoutSec=301;
  std::strcpy(corruptTimeout.apiKey, "AQ.FAKE_KEY_123456");
  install(corruptTimeout); assert(!gemini_client::settings().configured);
  const auto emptyRevision = gemini_client::configurationRevision();
  assert(gemini_client::configure("AQ.FAKE_KEY_123456", "", true, false, error, sizeof(error)));
  const auto keyRevision = gemini_client::configurationRevision();
  assert(keyRevision != emptyRevision);
  assert(gemini_client::configure("AQ.FAKE_KEY_123456", "", false, false, error, sizeof(error), 60));
  assert(gemini_client::configurationRevision() == keyRevision);
  failCommit = true;
  assert(!gemini_client::configure("AQ.NEW_FAKE_KEY_123456", "", false, false, error, sizeof(error)));
  failCommit = false;
  assert(gemini_client::configurationRevision() == keyRevision);
  assert(gemini_client::configure("", "", false, true, error, sizeof(error)));
  assert(gemini_client::configurationRevision() != keyRevision && !gemini_client::settings().configured);
  PreviousIdleSettings v3;
  std::strcpy(v3.apiKey, "AQ.FAKE_V3_KEY_123456"); v3.dryRun = 0; v3.idleTimeoutSec = 75;
  install(v3);
  char instructions[128 * 1024 + 1];
  assert(gemini_client::settings().configured && saved.apiKey == std::string(v3.apiKey));
  assert(!gemini_client::settings().dryRun && gemini_client::settings().idleTimeoutSec == 75);
  assert(gemini_client::copySystemInstructions(instructions, sizeof(instructions)));
  assert(!strcmp(instructions, gemini_live::kDefaultSystemInstructions));
  assert(!gemini_client::copySystemInstructions(instructions, 8));
  const std::string custom = "## Personality and behavior\nBe friendly.\n\n## Base information\nYou are Digit.\n\n## Expression and emotions\nSpeak warmly.\nSay \"hello\". Keep emojis: \xf0\x9f\xa4\x96";
  const auto beforePrompt = gemini_client::configurationRevision();
  assert(gemini_client::configure("", "", false, false, error, sizeof(error), 0, custom.c_str()));
  assert(saved.apiKey == std::string(v3.apiKey) && gemini_client::settings().idleTimeoutSec == 75);
  const auto promptRevision = gemini_client::configurationRevision();
  assert(promptRevision != beforePrompt);
  assert(gemini_client::configure("", "", true, false, error, sizeof(error), 0, custom.c_str()));
  assert(gemini_client::configurationRevision() == promptRevision);
  failCommit = true;
  assert(!gemini_client::configure("", "", false, false, error, sizeof(error), 0, "A different instruction"));
  failCommit = false;
  assert(gemini_client::configurationRevision() == promptRevision);
  reboot();
  assert(gemini_client::copySystemInstructions(instructions, sizeof(instructions)) && instructions == custom);
  assert(gemini_client::configure("", "", true, false, error, sizeof(error)));
  assert(gemini_client::copySystemInstructions(instructions, sizeof(instructions)) && instructions == custom);
  const std::string full(128 * 1024, 'x');
  assert(gemini_client::configure("", "", true, false, error, sizeof(error), 0, full.c_str()));
  reboot();
  assert(gemini_client::copySystemInstructions(instructions, sizeof(instructions)) && instructions == full);
  const auto promptWrites = writes;
  for (const auto& bad : {std::string(gemini_client::systemInstructionsCapacity()+1, 'x'), std::string("bad\x01"), std::string("bad\xc0\xaf"), std::string("bad\xed\xa0\x80"), std::string("bad\xf4\x90\x80\x80"), std::string("bad\xe2\x82")})
    assert(!gemini_client::configure("", "", true, false, error, sizeof(error), 0, bad.c_str()));
  assert(writes == promptWrites && savedInstructions.text() == full);
  const auto priorFlashWrites = flashWrites;
  assert(gemini_client::configure("", "", true, false, error, sizeof(error), 0, full.c_str()));
  assert(flashWrites == priorFlashWrites);  // Unchanged documents are not erased and rewritten.
  for (bool* failure : {&failErase, &failFlashWrite, &failFlashRead, &failWrite, &failCommit}) {
    const auto priorSettings = persistent;
    *failure = true;
    assert(!gemini_client::configure("", "", false, false, error, sizeof(error), 0, custom.c_str()));
    *failure = false;
    assert(persistent == priorSettings);
    reboot();
    assert(gemini_client::copySystemInstructions(instructions, sizeof(instructions)) && instructions == full);
  }
  // Integrity failures retain credentials and refuse to silently substitute defaults.
  const auto activeOffset = saved.promptSlot * gemini_client::systemInstructionsCapacity();
  flash[activeOffset] ^= 1;
  reboot();
  assert(gemini_client::settings().configured);
  assert(!gemini_client::copySystemInstructions(instructions, sizeof(instructions)));
  assert(gemini_client::duplicateSystemInstructions() == nullptr);
  assert(!gemini_client::configure("", "", true, false, error, sizeof(error)));
  assert(gemini_client::configure("", "", true, false, error, sizeof(error), 0, custom.c_str()));
  reboot();
  char* dynamicInstructions = gemini_client::duplicateSystemInstructions();
  assert(dynamicInstructions && dynamicInstructions == custom); std::free(dynamicInstructions);
  assert(gemini_client::configure("", "", true, false, error, sizeof(error), 0, ""));
  reboot();
  assert(gemini_client::copySystemInstructions(instructions, sizeof(instructions)) && !strcmp(instructions, gemini_live::kDefaultSystemInstructions));
  assert(gemini_client::configure("", "", true, false, error, sizeof(error), 0, gemini_live::kDefaultSystemInstructions));
  assert(!savedInstructions.length);
  PreviousInstructionSettings v4;
  std::strcpy(v4.apiKey, "AQ.FAKE_V4_KEY_123456"); v4.idleTimeoutSec = 90;
  std::strcpy(v4.systemInstructions, custom.c_str()); install(v4);
  assert(gemini_client::settings().configured && saved.version == 5 && saved.idleTimeoutSec == 90);
  assert(gemini_client::copySystemInstructions(instructions, sizeof(instructions)) && instructions == custom);
  assert(persistent.size() == sizeof(v4));
  assert(gemini_client::configure("", "", false, false, error, sizeof(error)));
  reboot();
  assert(gemini_client::copySystemInstructions(instructions, sizeof(instructions)) && instructions == custom);
  assert(saved.apiKey == std::string(v4.apiKey) && persistent.size() == sizeof(SavedSettings));
  PreviousInstructionSettings brokenPrompt;
  std::strcpy(brokenPrompt.apiKey, "AQ.FAKE_KEY_123456");
  std::memset(brokenPrompt.systemInstructions, 'x', sizeof(brokenPrompt.systemInstructions));
  install(brokenPrompt); assert(!gemini_client::settings().configured);
  // The installed v5 voice model must migrate without losing its saved key
  // or custom personality/facts. Loading never writes flash; Save commits the
  // new emotion section through the existing verified alternating slots.
  for(const std::string newline : {std::string("\n"),std::string("\r\n")}) {
  const std::string prefix="## Personality and behavior"+newline+"My custom playful personality."+newline+newline+
      "## Base information"+newline+"My custom robot facts."+newline+newline;
  const std::string suffix=newline+newline+"## Additional facts"+newline+"Preserve this custom section exactly.";
  const std::string oldPrompt=prefix+"## Voice instructions"+newline+"Speak warmly."+suffix;
  assert(gemini_client::configure("FAKE_ER2_MIGRATION_KEY_123456", "", false, false, error, sizeof(error), 0, oldPrompt.c_str()));
  SavedSettings oldModel;memcpy(&oldModel,persistent.data(),sizeof(oldModel));
  strcpy(oldModel.model,"gemini-3.8-live");
  const std::string keyBefore=saved.apiKey;const unsigned voiceMigrationWritesBefore=writes;
  install(oldModel);assert(gemini_client::settings().configured);
  assert(!strcmp(gemini_client::settings().model,"gemini-robotics-er-2-streaming-preview"));
  assert(saved.apiKey==keyBefore&&writes==voiceMigrationWritesBefore);
  char* migrated=gemini_client::duplicateSystemInstructions();assert(migrated);
  assert(strstr(migrated,"My custom playful personality.")&&strstr(migrated,"My custom robot facts."));
  assert(strstr(migrated,"## Expression and emotions")&&!strstr(migrated,"## Voice instructions"));
  assert(std::string(migrated).compare(0,prefix.size(),prefix)==0);
  assert(std::string(migrated).substr(strlen(migrated)-suffix.size())==suffix);
  assert(gemini_client::configure("", "", false, false,error,sizeof(error),0,migrated));free(migrated);
  reboot();assert(gemini_client::settings().configured&&!strcmp(saved.apiKey,keyBefore.c_str()));
  migrated=gemini_client::duplicateSystemInstructions();assert(migrated&&strstr(migrated,"## Expression and emotions"));free(migrated);
  }
  std::puts("PASS: v1-v4 migration, dynamic 128 KiB instructions, verified alternating flash slots, integrity checks, key retention and failed-save recovery across reboot");
  std::puts("PASS: key/prompt revision reconnects; no-op/policy edits and failed saves retain revision");
}
