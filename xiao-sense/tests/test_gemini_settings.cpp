// Actual credential validation and persistence, with only ESP-IDF I/O mocked.
#include "../src/gemini_client.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

// Extracted unchanged by run_gemini.ps1.
#include "gemini_settings_private.inc"
#include "gemini_settings_public.inc"

void reboot() { saved = SavedSettings{}; loaded = false; revision = 1; }
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
  assert(!std::strcmp(gemini_client::settings().model, "gemini-3.8-live"));
  assert(!std::strcmp(saved.apiKey, legacy.apiKey) && saved.version == 3);
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
    assert(gemini_client::configure(key.c_str(), "gemini-3.8-live", false, false, error, sizeof(error), 25));
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
  for (const char* model : {"gemini-3.8-flash", "gemini-test-model", "models/gemini-3.8-live", "gemini-3.1-flash-live-preview"})
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
  assert(!std::strcmp(gemini_client::settings().model,"gemini-3.8-live"));
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
  std::puts("PASS: v1/v2 migration, Live model allowlist, full-length key persistence, timeout bounds/reboot, retain/clear, corruption, failed-save retention");
  std::puts("PASS: key-only configuration revision, no-op/policy edits retain connection, failed-save revision unchanged, key removal invalidates worker");
}
