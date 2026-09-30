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
#include "person_follow.h"
#include "web_server.h"
#include "wifi_credentials.h"
#include "zumo_link.h"
#include "wake_word.h"
#include "voice_direction.h"
#include "robot_actions.h"
#include "speech_commands.h"

namespace {
constexpr char kTag[] = "digit";
void wifi_event(void*, esp_event_base_t base, int32_t id, void*) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    robot_actions::stop("Wi-Fi disconnected");
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
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs);
  ESP_LOGI(kTag, "Digit ESP-IDF 5.5 starting; PSRAM=%u bytes", (unsigned)ESP.getFreePsram());
  wake_word::begin();
  speech_commands::begin();
  robot_actions::begin();
  audioEngine.begin();
  camera_service::begin();
  zumo_link::begin();
  voice_direction::begin();
  vision_service::begin();
  start_wifi();
  bool web_started = false;
  uint32_t last_health = 0;
  for (;;) {
    {
      robot_actions::Guard control;
      zumo_link::poll();
      person_follow::poll();
      speech_commands::poll();
      robot_actions::poll();
      wake_word::poll();
    }
    if (!web_started && WiFi.status() == WL_CONNECTED) web_started = web_server::begin();
    if (millis() - last_health >= 5000) {
      last_health = millis();
      refresh_wifi_signal();
      audioEngine.logDiagnostics();
      const auto wake = wake_word::snapshot();
      ESP_LOGI(kTag, "Wake=%s detections=%lu mic_peak=%u inference=%lu us dropped=%lu",
               !wake.ready ? "error" : wake.pausedForVision ? "paused-for-vision" : "listening",
               (unsigned long)wake.detections, wake.micPeak, (unsigned long)wake.inferenceUs,
               (unsigned long)wake.droppedFrames);
      ESP_LOGI(kTag, "Wi-Fi=%s IP=%s RSSI=%d dBm", WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
               WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    delay(5);
  }
}
