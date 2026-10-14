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
#include "esp_partition.h"
#include "esp_crc.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <string>

namespace {
template<size_t KeyCapacity, uint32_t Version>
struct StoredSettings {
  uint32_t version = Version;
  char apiKey[KeyCapacity] = {};
  char model[64] = "gemini-robotics-er-2-streaming-preview";
  uint8_t dryRun = 1;
  uint8_t reserved[3] = {};
};
using LegacySettings = StoredSettings<160, 1>;
using PreviousSettings = StoredSettings<gemini_client::kMaxApiKeyLength + 1, 2>;
struct PreviousIdleSettings : StoredSettings<gemini_client::kMaxApiKeyLength + 1, 3> {
  uint32_t idleTimeoutSec = 30;
};
struct PreviousInstructionSettings : StoredSettings<gemini_client::kMaxApiKeyLength + 1, 4> {
  uint32_t idleTimeoutSec = 30;
  char systemInstructions[4097] = {};  // Preserve the exact legacy on-flash layout.
};
struct SavedSettings : StoredSettings<gemini_client::kMaxApiKeyLength + 1, 5> {
  uint32_t idleTimeoutSec = 30;
  uint32_t promptSlot = UINT32_MAX;
  uint32_t promptLength = 0;
  uint32_t promptCrc = 0;
};
struct PrivateText {
  char* data = nullptr;
  size_t length = 0;
  ~PrivateText() { clear(); }
  PrivateText() = default;
  PrivateText(const PrivateText&) = delete;
  PrivateText& operator=(const PrivateText&) = delete;
  void clear() {
    if (data) { mbedtls_platform_zeroize(data, length + 1); std::free(data); }
    data = nullptr; length = 0;
  }
  bool allocate(size_t size) {
    clear();
    if (size == SIZE_MAX) return false;
    data = static_cast<char*>(std::malloc(size + 1));
    if (!data) return false;
    length = size; data[size] = '\0'; return true;
  }
  bool copy(const char* text) {
    const size_t size = strlen(text);
    if (!size) { clear(); return true; }
    if (!allocate(size)) return false;
    memcpy(data, text, size); return true;
  }
  const char* text() const { return data ? data : ""; }
  void swap(PrivateText& other) {
    char* previous = data; data = other.data; other.data = previous;
    const size_t previousLength = length; length = other.length; other.length = previousLength;
  }
};
SavedSettings saved;
PrivateText savedInstructions;
bool instructionsReady = true;
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
struct PrivateSettings {
  SavedSettings* value = new (std::nothrow) SavedSettings;
  ~PrivateSettings() {
    if (value) { mbedtls_platform_zeroize(value, sizeof(*value)); delete value; }
  }
  PrivateSettings() = default;
  PrivateSettings(const PrivateSettings&) = delete;
  PrivateSettings& operator=(const PrivateSettings&) = delete;
};
const esp_partition_t* instructionPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "gemini_data");
}
size_t instructionSlotSize(const esp_partition_t* partition) {
  return partition ? (partition->size / 2 / 4096) * 4096 : 0;
}
bool readInstructions(const SavedSettings& metadata, PrivateText& output) {
  if (!metadata.promptLength) { output.clear(); return true; }
  const auto* partition = instructionPartition();
  const size_t slotSize = instructionSlotSize(partition);
  if (metadata.promptSlot > 1 || metadata.promptLength > slotSize || !output.allocate(metadata.promptLength)) return false;
  uint8_t chunk[1024];
  uint32_t crc = 0;
  for (size_t at = 0; at < metadata.promptLength;) {
    const size_t count = metadata.promptLength - at < sizeof(chunk) ? metadata.promptLength - at : sizeof(chunk);
    if (esp_partition_read(partition, metadata.promptSlot * slotSize + at, chunk, count) != ESP_OK) return false;
    memcpy(output.data + at, chunk, count); crc = esp_crc32_le(crc, chunk, static_cast<uint32_t>(count)); at += count;
  }
  return crc == metadata.promptCrc && strlen(output.data) == metadata.promptLength &&
      gemini_live::validSystemInstructions(output.data);
}
bool writeInstructions(SavedSettings& metadata, const PrivateText& text) {
  const auto* partition = instructionPartition();
  const size_t slotSize = instructionSlotSize(partition);
  if (!text.length || text.length > slotSize) return false;
  const uint32_t slot = metadata.promptSlot == 0 ? 1 : 0;
  const size_t offset = slot * slotSize;
  const size_t eraseSize = (text.length + 4095) / 4096 * 4096;
  if (esp_partition_erase_range(partition, offset, eraseSize) != ESP_OK) return false;
  uint8_t chunk[1024]; uint32_t crc = 0;
  for (size_t at = 0; at < text.length;) {
    const size_t count = text.length - at < sizeof(chunk) ? text.length - at : sizeof(chunk);
    // Keep flash I/O buffers in internal RAM even for a large PSRAM document.
    memcpy(chunk, text.data + at, count);
    if (esp_partition_write(partition, offset + at, chunk, count) != ESP_OK) return false;
    if (esp_partition_read(partition, offset + at, chunk, count) != ESP_OK || memcmp(chunk, text.data + at, count)) return false;
    crc = esp_crc32_le(crc, chunk, static_cast<uint32_t>(count)); at += count;
  }
  metadata.promptSlot = slot; metadata.promptLength = static_cast<uint32_t>(text.length); metadata.promptCrc = crc;
  return true;
}
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
  return model && strnlen(model, 64) < 64 && !strcmp(model, "gemini-robotics-er-2-streaming-preview");
}
bool loadableModel(const char* model) { return validModel(model) || !strcmp(model,"gemini-3.8-live"); }

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
  strcpy(saved.model, validModel(previous.model) ? previous.model : "gemini-robotics-er-2-streaming-preview");
  saved.dryRun = previous.dryRun;
}
void migrateVoiceInstructions() {
  if (!instructionsReady || !savedInstructions.length) return;
  std::string value=savedInstructions.text();
  // Saved dashboard documents can use LF or CRLF. Replace only the voice
  // section, retaining custom personality/facts and their exact line endings.
  size_t voice=0;
  if(value.rfind("## Voice instructions",0)!=0) {
    const auto line=value.find("\n## Voice instructions");
    if(line==std::string::npos) return;
    voice=line+1;
  }
  const size_t headerEnd=voice+strlen("## Voice instructions");
  if(headerEnd<value.size() && value[headerEnd]!='\n' && value[headerEnd]!='\r') return;
  const char* emotions=strstr(gemini_live::kDefaultSystemInstructions,"## Expression and emotions\n");
  const auto next=value.find("\n## ",headerEnd);
  size_t end=next==std::string::npos ? value.size() : next+1;
  while(end>headerEnd && (value[end-1]=='\n'||value[end-1]=='\r')) --end;
  value.replace(voice,end-voice,emotions);
  // Keep customized personality and robot facts verbatim. Runtime capability
  // policy overrides older demands for speech.
  if(savedInstructions.copy(value.c_str())) {
    saved.promptSlot=UINT32_MAX; // First Save writes and verifies the migrated document.
    saved.promptLength=static_cast<uint32_t>(savedInstructions.length);
  } else instructionsReady=false;
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
      if (!candidate.value) { loaded = false; nvs_close(handle); return; }
      if (candidate.value && nvs_get_blob(handle, "settings", candidate.value, &size) == ESP_OK &&
          size == sizeof(SavedSettings) && validStored(*candidate.value, 5) &&
          loadableModel(candidate.value->model) && candidate.value->idleTimeoutSec >= 10 &&
          candidate.value->idleTimeoutSec <= 300) {
        saved = *candidate.value;
        strcpy(saved.model,"gemini-robotics-er-2-streaming-preview");
        instructionsReady = readInstructions(saved, savedInstructions);
      }
    } else if (size == sizeof(PreviousInstructionSettings)) {
      auto* candidate = new (std::nothrow) PreviousInstructionSettings;
      if (!candidate) { loaded = false; nvs_close(handle); return; }
      if (nvs_get_blob(handle, "settings", candidate, &size) == ESP_OK &&
          size == sizeof(*candidate) && validStored(*candidate, 4) && loadableModel(candidate->model) &&
          candidate->idleTimeoutSec >= 10 && candidate->idleTimeoutSec <= 300 &&
          strnlen(candidate->systemInstructions, sizeof(candidate->systemInstructions)) < sizeof(candidate->systemInstructions) &&
          gemini_live::validSystemInstructions(candidate->systemInstructions)) {
        migrate(*candidate); saved.idleTimeoutSec = candidate->idleTimeoutSec;
        instructionsReady = savedInstructions.copy(candidate->systemInstructions);
        // No flash writes on load. The first successful save migrates the document.
        saved.promptLength = static_cast<uint32_t>(savedInstructions.length);
      }
      mbedtls_platform_zeroize(candidate, sizeof(*candidate)); delete candidate;
    } else if (size == sizeof(PreviousIdleSettings)) {
      PrivateValue<PreviousIdleSettings> candidate;
      if (nvs_get_blob(handle, "settings", &candidate.value, &size) == ESP_OK &&
          size == sizeof(PreviousIdleSettings) && validStored(candidate.value, 3) &&
          loadableModel(candidate.value.model) && candidate.value.idleTimeoutSec >= 10 &&
          candidate.value.idleTimeoutSec <= 300) {
        migrate(candidate.value);
        saved.idleTimeoutSec = candidate.value.idleTimeoutSec;
      }
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
  migrateVoiceInstructions();
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
  if (std::time(nullptr) < 1735689600 && !cb.cancelled(cb.context))
    cb.event(gemini_live::Event::ConnectionProgress, "Waiting for internet clock sync", cb.context);
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
bool copySystemInstructions(char* output, size_t capacity) {
  SettingsGuard guard; loadLocked();
  const char* value = savedInstructions.length ? savedInstructions.text() : gemini_live::kDefaultSystemInstructions;
  if (!instructionsReady || !output || capacity <= strlen(value)) return false;
  strcpy(output, value);
  return true;
}
char* duplicateSystemInstructions() {
  SettingsGuard guard; loadLocked();
  if (!instructionsReady) return nullptr;
  PrivateText copy;
  if (!copy.copy(savedInstructions.length ? savedInstructions.text() : gemini_live::kDefaultSystemInstructions)) return nullptr;
  char* result = copy.data; copy.data = nullptr; copy.length = 0; return result;
}
size_t systemInstructionsCapacity() { return instructionSlotSize(instructionPartition()); }
bool configure(const char* apiKey, const char* model, bool dryRun, bool clearKey,
               char* error, size_t errorSize, uint32_t idleTimeoutSec, const char* systemInstructions) {
  if (error && errorSize) error[0] = '\0';
  const auto fail = [&](const char* message) {
    if (error && errorSize) snprintf(error, errorSize, "%s", message);
    return false;
  };
  if (clearKey && apiKey && *apiKey) return fail("Choose either a new key or remove key");
  if (apiKey && *apiKey && !validToken(apiKey, sizeof(saved.apiKey)))
    return fail("Paste only the API key: 16-511 letters, digits, periods, underscores or hyphens");
  if (model && *model && !validModel(model)) return fail("Supported model: gemini-robotics-er-2-streaming-preview");
  if (idleTimeoutSec && (idleTimeoutSec < 10 || idleTimeoutSec > 300))
    return fail("Response timeout must be 10-300 seconds");
  if (systemInstructions && !gemini_live::validSystemInstructions(systemInstructions))
    return fail("System instructions contain invalid text");
  ConfigurationGuard writer;
  PrivateSettings next;
  PrivateText nextInstructions;
  bool promptChanged = false;
  if (!next.value) return fail("Not enough memory to save Gemini settings");
  { SettingsGuard guard; loadLocked(); *next.value = saved;
    if (!instructionsReady && !systemInstructions) return fail("Saved instructions could not be read; re-enter them before saving");
    const char* desired = systemInstructions ? systemInstructions : savedInstructions.text();
    if (!strcmp(desired, gemini_live::kDefaultSystemInstructions)) desired = "";
    if (*desired && strlen(desired) > systemInstructionsCapacity()) return fail("Digit's instruction storage is full; previous settings retained");
    if (!nextInstructions.copy(desired)) return fail("Not enough free memory to save instructions; previous settings retained");
    promptChanged = !instructionsReady || strcmp(savedInstructions.text(), desired);
  }
  if (clearKey) mbedtls_platform_zeroize(next.value->apiKey, sizeof(next.value->apiKey));
  else if (apiKey && *apiKey) {
    mbedtls_platform_zeroize(next.value->apiKey, sizeof(next.value->apiKey));
    strcpy(next.value->apiKey, apiKey);
  }
  if (model && *model) strcpy(next.value->model, model);
  next.value->dryRun = dryRun ? 1 : 0;
  if (idleTimeoutSec) next.value->idleTimeoutSec = idleTimeoutSec;
  if (!nextInstructions.length) {
    next.value->promptSlot = UINT32_MAX; next.value->promptLength = 0; next.value->promptCrc = 0;
  } else if (promptChanged || next.value->promptSlot > 1) {
    // The previous slot remains intact until NVS commits the new selector.
    if (!writeInstructions(*next.value, nextInstructions))
      return fail("Could not store instructions on Digit; previous settings retained");
  }
  nvs_handle_t handle;
  esp_err_t result = nvs_open("digit_gemini", NVS_READWRITE, &handle);
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, "settings", next.value, sizeof(*next.value));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
  }
  if (result != ESP_OK) return fail("Could not save Gemini settings; active settings retained");
  { SettingsGuard guard;
    if (strcmp(saved.apiKey, next.value->apiKey) || strcmp(saved.model, next.value->model) ||
        promptChanged) {
      if (!++revision) ++revision;
    }
    mbedtls_platform_zeroize(&saved, sizeof(saved)); saved = *next.value;
    savedInstructions.swap(nextInstructions); instructionsReady = true;
  }
  return true;
}
void live(const gemini_live::Callbacks& cb, bool probe) {
  if (!cb.cancelled || !cb.event) return;
  PrivateSettings config;
  PrivateText instructions;
  const auto error = [&](const char* message) {
    if (!cb.cancelled(cb.context)) cb.event(gemini_live::Event::Error, message, cb.context);
  };
  if (!config.value) { error("Not enough memory for Gemini settings"); return; }
  bool haveInstructions;
  { SettingsGuard guard; loadLocked(); *config.value = saved;
    haveInstructions = instructionsReady && instructions.copy(savedInstructions.text()); }
  if (!haveInstructions) { error("Could not load saved Gemini instructions"); return; }
  if (!config.value->apiKey[0]) { error("Add a Gemini API key in session settings"); return; }
  if (!connected() && !cb.cancelled(cb.context))
    cb.event(gemini_live::Event::ConnectionProgress, "Waiting for Wi-Fi connection", cb.context);
  while (!connected()) {
    if (cb.cancelled(cb.context)) return;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (!ensureClock(cb)) { error("Waiting for clock sync; check internet access"); return; }
  if (!cb.cancelled(cb.context)) gemini_live::run(config.value->apiKey, config.value->model, cb, probe,
      instructions.length ? instructions.text() : gemini_live::kDefaultSystemInstructions);
}
}  // namespace gemini_client
