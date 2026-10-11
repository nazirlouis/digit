#include "gemini_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
template<size_t KeyCapacity, uint32_t Version>
struct StoredSettings {
  uint32_t version = Version;
  char apiKey[KeyCapacity] = {};
  char model[64] = "gemini-3.8-live";
  uint8_t dryRun = 1;
  uint8_t reserved[3] = {};
};
using LegacySettings = StoredSettings<160, 1>;
using PreviousSettings = StoredSettings<gemini_client::kMaxApiKeyLength + 1, 2>;
struct SavedSettings : StoredSettings<gemini_client::kMaxApiKeyLength + 1, 3> {
  uint32_t idleTimeoutSec = 30;
};
SavedSettings saved;
bool loaded = false;
uint32_t revision = 1;
SemaphoreHandle_t settingsMutex() {
  static StaticSemaphore_t storage;
  static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&storage);
  return handle;
}
class SettingsGuard {
 public:
  SettingsGuard() { xSemaphoreTake(settingsMutex(), portMAX_DELAY); }
  ~SettingsGuard() { xSemaphoreGive(settingsMutex()); }
};
SemaphoreHandle_t configurationMutex() {
  static StaticSemaphore_t storage;
  static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&storage);
  return handle;
}
class ConfigurationGuard {
 public:
  ConfigurationGuard() { xSemaphoreTake(configurationMutex(), portMAX_DELAY); }
  ~ConfigurationGuard() { xSemaphoreGive(configurationMutex()); }
};
template<typename T>
struct PrivateValue {
  T value;
  ~PrivateValue() { mbedtls_platform_zeroize(&value, sizeof(value)); }
};
using PrivateSettings = PrivateValue<SavedSettings>;
bool validToken(const char* value, size_t capacity) {
  if (!value) return false;
  const size_t length = strnlen(value, capacity);
  if (length < 16 || length >= capacity) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = value[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
  }
  return true;
}
bool validModel(const char* model) {
  return model && strnlen(model, 64) < 64 && !strcmp(model, "gemini-3.8-live");
}
template<typename T>
bool validStored(const T& value, uint32_t version) {
  return value.version == version && value.dryRun <= 1 &&
      strnlen(value.model, sizeof(value.model)) < sizeof(value.model) &&
      (!value.apiKey[0] || validToken(value.apiKey, sizeof(value.apiKey)));
}
template<typename T>
void migrate(const T& previous) {
  strcpy(saved.apiKey, previous.apiKey);
  // Unsupported old REST models are never allowed onto Live.
  strcpy(saved.model, validModel(previous.model) ? previous.model : "gemini-3.8-live");
  saved.dryRun = previous.dryRun;
}
void loadLocked() {
  if (loaded) return;
  loaded = true;
  nvs_handle_t handle;
  if (nvs_open("digit_gemini", NVS_READONLY, &handle) != ESP_OK) return;
  size_t size = 0;
  if (nvs_get_blob(handle, "settings", nullptr, &size) == ESP_OK) {
    if (size == sizeof(SavedSettings)) {
      PrivateSettings candidate;
      if (nvs_get_blob(handle, "settings", &candidate.value, &size) == ESP_OK &&
          size == sizeof(SavedSettings) && validStored(candidate.value, 3) &&
          validModel(candidate.value.model) && candidate.value.idleTimeoutSec >= 10 &&
          candidate.value.idleTimeoutSec <= 300) saved = candidate.value;
    } else if (size == sizeof(PreviousSettings)) {
      PrivateValue<PreviousSettings> candidate;
      if (nvs_get_blob(handle, "settings", &candidate.value, &size) == ESP_OK &&
          size == sizeof(PreviousSettings) && validStored(candidate.value, 2)) migrate(candidate.value);
    } else if (size == sizeof(LegacySettings)) {
      PrivateValue<LegacySettings> candidate;
      if (nvs_get_blob(handle, "settings", &candidate.value, &size) == ESP_OK &&
          size == sizeof(LegacySettings) && validStored(candidate.value, 1)) migrate(candidate.value);
    }
  }
  nvs_close(handle);
}
uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
bool connected() {
  wifi_ap_record_t ap{};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip{};
  return netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr;
}
bool ensureClock(const gemini_live::Callbacks& cb) {
  static bool started = false;
  if (!started) {
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.google.com");
    if (esp_netif_sntp_init(&config) != ESP_OK) return false;
    started = true;
  }
  const uint32_t began = nowMs();
  while (std::time(nullptr) < 1735689600) {
    if (cb.cancelled(cb.context) || !connected() || nowMs() - began >= 10000) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return true;
}
}  // namespace

namespace gemini_client {
void begin() {
  SettingsGuard guard;
  loadLocked();
  esp_log_level_set("transport_ws", ESP_LOG_NONE);
  esp_log_level_set("transport_base", ESP_LOG_WARN);
}
Settings settings() {
  SettingsGuard guard;
  loadLocked();
  Settings result{};
  result.configured = saved.apiKey[0] != '\0';
  result.dryRun = saved.dryRun != 0;
  result.idleTimeoutSec = saved.idleTimeoutSec;
  strcpy(result.model, saved.model);
  return result;
}
uint32_t configurationRevision() { SettingsGuard guard; loadLocked(); return revision; }
bool configure(const char* apiKey, const char* model, bool dryRun, bool clearKey,
               char* error, size_t errorSize, uint32_t idleTimeoutSec) {
  if (error && errorSize) error[0] = '\0';
  const auto fail = [&](const char* message) {
    if (error && errorSize) snprintf(error, errorSize, "%s", message);
    return false;
  };
  if (clearKey && apiKey && *apiKey) return fail("Choose either a new key or remove key");
  if (apiKey && *apiKey && !validToken(apiKey, sizeof(saved.apiKey)))
    return fail("Paste only the API key: 16-511 letters, digits, periods, underscores or hyphens");
  if (model && *model && !validModel(model)) return fail("Supported Live model: gemini-3.8-live");
  if (idleTimeoutSec && (idleTimeoutSec < 10 || idleTimeoutSec > 300))
    return fail("Idle timeout must be 10-300 seconds");
  ConfigurationGuard writer;
  PrivateSettings next;
  { SettingsGuard guard; loadLocked(); next.value = saved; }
  if (clearKey) mbedtls_platform_zeroize(next.value.apiKey, sizeof(next.value.apiKey));
  else if (apiKey && *apiKey) {
    mbedtls_platform_zeroize(next.value.apiKey, sizeof(next.value.apiKey));
    strcpy(next.value.apiKey, apiKey);
  }
  if (model && *model) strcpy(next.value.model, model);
  next.value.dryRun = dryRun ? 1 : 0;
  if (idleTimeoutSec) next.value.idleTimeoutSec = idleTimeoutSec;
  nvs_handle_t handle;
  esp_err_t result = nvs_open("digit_gemini", NVS_READWRITE, &handle);
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, "settings", &next.value, sizeof(next.value));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
  }
  if (result != ESP_OK) return fail("Could not save Gemini settings; active settings retained");
  { SettingsGuard guard;
    if (strcmp(saved.apiKey, next.value.apiKey) || strcmp(saved.model, next.value.model)) {
      if (!++revision) ++revision;
    }
    mbedtls_platform_zeroize(&saved, sizeof(saved)); saved = next.value;
  }
  return true;
}
void live(const gemini_live::Callbacks& cb, bool probe) {
  if (!cb.cancelled || !cb.event) return;
  PrivateSettings config;
  { SettingsGuard guard; loadLocked(); config.value = saved; }
  const auto error = [&](const char* message) {
    if (!cb.cancelled(cb.context)) cb.event(gemini_live::Event::Error, message, cb.context);
  };
  if (!config.value.apiKey[0]) { error("Add a Gemini API key in voice settings"); return; }
  if (!connected()) { error("Wi-Fi is not connected"); return; }
  if (!ensureClock(cb)) { error("Waiting for clock sync; check internet access"); return; }
  if (!cb.cancelled(cb.context)) gemini_live::run(config.value.apiKey, config.value.model, cb, probe);
}
}  // namespace gemini_client
