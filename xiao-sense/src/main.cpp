#include <cstring>
#include "Arduino.h"
#include "WiFi.h"
#include "app_config.h"
#include "audio_engine.h"
#include "camera_service.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "vision_service.h"
#include "web_server.h"
#include "wifi_credentials.h"
#include "zumo_link.h"
#include "robot_actions.h"
#include "robot_session.h"
#include "wake_word.h"
#include "local_stop.h"
#include "voice_direction.h"

namespace {
constexpr char kTag[] = "digit";
void motion_link_task(void*) {
  TickType_t scheduled = xTaskGetTickCount();
  for (;;) {
    // Never acquire the mission/UI mutex here. UART service and its watchdog
    // remain independent of camera, audio, HTTP and high-level decisions.
    zumo_link::poll();
    vTaskDelayUntil(&scheduled, std::max<TickType_t>(1, pdMS_TO_TICKS(5)));
  }
}
void wifi_event(void*, esp_event_base_t base, int32_t id, void*) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    // Speech recovery immediately stops its owned job while preserving only
    // valid high-level task authority. Manual driving always loses its lease.
    if (zumo_link::snapshot().manualDriving) robot_actions::stop("Wi-Fi disconnected during manual drive");
    robot_session::networkDisconnected();
    ESP_LOGW(kTag, "Wi-Fi disconnected; reconnecting");
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ESP_LOGI(kTag, "Wi-Fi connected: http://%s", WiFi.localIP().toString().c_str());
  }
}
bool start_wifi() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t* netif = esp_netif_create_default_wifi_sta();
  if (!netif) return false;
  set_wifi_netif(netif);
  esp_netif_set_hostname(netif, app_config::kWifiHostname);
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  wifi_config_t wifi = {};
  strncpy(reinterpret_cast<char*>(wifi.sta.ssid), wifi_credentials::kSsid, sizeof(wifi.sta.ssid) - 1);
  strncpy(reinterpret_cast<char*>(wifi.sta.password), wifi_credentials::kPassword, sizeof(wifi.sta.password) - 1);
  wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  ESP_ERROR_CHECK(esp_wifi_start());
  return true;
}
}  // namespace

extern "C" void app_main() {
  static_assert(CONFIG_ESP_MAIN_TASK_STACK_SIZE >= 8192,
                "Digit tool dispatch requires an 8192-byte main task stack; refresh sdkconfig");
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs);
  // Keep normal model/work buffers in PSRAM rather than spending the scarce
  // internal heap on allocations up to IDF's default 16 KB threshold. DMA,
  // task stacks and explicitly internal allocations keep their required caps.
  heap_caps_malloc_extmem_enable(1024);
  ESP_LOGI(kTag, "Digit ESP-IDF 5.5 starting; PSRAM=%u bytes", (unsigned)ESP.getFreePsram());

  wake_word::begin();
  local_stop::begin();
  robot_session::begin();
  robot_actions::begin();

  // Bring up the independent motor link before optional peripheral startup.
  // Audio initialization must never prevent Stop/status service from running.
  zumo_link::begin();
  configASSERT(xTaskCreatePinnedToCore(motion_link_task, "motion_link", 4096,
                                     nullptr, 4, nullptr, 1) == pdPASS);
  audioEngine.begin();
  camera_service::begin();

  voice_direction::begin();
  vision_service::begin();
  start_wifi();
  bool web_started = false;
  uint32_t last_web_attempt = 0;
  uint32_t last_health = 0;
  for (;;) {
    {
      robot_actions::Guard control;

      local_stop::poll();
      robot_session::poll();
      robot_actions::poll();
      wake_word::poll();


    }
    if (!web_started && WiFi.status() == WL_CONNECTED &&
        (!last_web_attempt || millis()-last_web_attempt >= 1000)) {
      last_web_attempt=millis();
      web_started=web_server::begin();
    }
    if (millis() - last_health >= 5000) {
      last_health = millis();
      refresh_wifi_signal();
      audioEngine.logDiagnostics();
      ESP_LOGI(kTag, "Main task stack minimum free=%u bytes",
               static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
      ESP_LOGI(kTag, "Wi-Fi=%s IP=%s RSSI=%d dBm", WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
               WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    delay(5);
  }
}
