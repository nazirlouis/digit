#include <Arduino.h>
#include <WiFi.h>

#include "app_config.h"
#include "audio_engine.h"
#include "camera_service.h"
#include "web_server.h"

namespace {

const IPAddress kAccessPointIp(192, 168, 4, 1);
const IPAddress kAccessPointGateway(192, 168, 4, 1);
const IPAddress kAccessPointSubnet(255, 255, 255, 0);

bool startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  if (!WiFi.softAPConfig(kAccessPointIp, kAccessPointGateway,
                         kAccessPointSubnet)) {
    Serial.println("Wi-Fi: could not configure 192.168.4.1");
    return false;
  }

  if (!WiFi.softAP(app_config::kAccessPointName,
                   app_config::kAccessPointPassword,
                   app_config::kAccessPointChannel, false,
                   app_config::kMaximumWifiClients)) {
    Serial.println("Wi-Fi: could not start the access point");
    return false;
  }

  Serial.printf("Wi-Fi: join %s and open http://%s\n",
                app_config::kAccessPointName,
                WiFi.softAPIP().toString().c_str());
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("Digit Sense starting");
  Serial.printf("PSRAM: %u bytes total, %u bytes free\n",
                static_cast<unsigned int>(ESP.getPsramSize()),
                static_cast<unsigned int>(ESP.getFreePsram()));

  // Audio allocates its fixed recording buffer before the camera requests its
  // variable-size frame buffers.
  audioEngine.begin();
  camera_service::begin();

  if (startAccessPoint()) {
    web_server::begin();
  }
}

void loop() { delay(1000); }

