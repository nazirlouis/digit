#include <Arduino.h>
#include <WiFi.h>

#include "app_config.h"
#include "audio_engine.h"
#include "camera_service.h"
#include "web_server.h"
#include "wifi_credentials.h"

namespace {

bool connectToWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(app_config::kWifiHostname);
  WiFi.begin(wifi_credentials::kSsid, wifi_credentials::kPassword);

  Serial.print("Wi-Fi: connecting");
  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < app_config::kWifiConnectTimeoutMs) {
    Serial.print('.');
    delay(500);
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi error: connection timed out; check credentials and 2.4 GHz coverage");
    return false;
  }

  Serial.printf("Wi-Fi: connected, open http://%s\n",
                WiFi.localIP().toString().c_str());
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("============================================================");
  Serial.println("Digit Sense starting (diagnostic build)");
  Serial.printf("Reset reason=%d; chip revision=%u; SDK=%s\n",
                static_cast<int>(esp_reset_reason()), ESP.getChipRevision(),
                ESP.getSdkVersion());
  Serial.printf("PSRAM: %u bytes total, %u bytes free\n",
                static_cast<unsigned int>(ESP.getPsramSize()),
                static_cast<unsigned int>(ESP.getFreePsram()));

  // Audio allocates its fixed recording buffer before the camera requests its
  // variable-size frame buffers.
  audioEngine.begin();
  camera_service::begin();

  if (connectToWifi()) {
    web_server::begin();
  }
}

void loop() {
  static uint32_t lastHealthLogMs = 0;
  if (millis() - lastHealthLogMs >= 5000) {
    lastHealthLogMs = millis();
    audioEngine.logDiagnostics();
    Serial.printf("[%10lu] Network: Wi-Fi=%s IP=%s RSSI=%ld dBm\n",
                  static_cast<unsigned long>(millis()),
                  WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
                  WiFi.localIP().toString().c_str(),
                  static_cast<long>(WiFi.RSSI()));
  }
  delay(100);
}
