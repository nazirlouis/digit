#pragma once
#include <string>
#include "esp_wifi.h"
#include "esp_netif.h"

constexpr int WL_CONNECTED = 3;
struct WifiIp {
  std::string toString() const;
};
struct WifiAdapter {
  int status() const;
  WifiIp localIP() const { return {}; }
  int RSSI() const;
};
extern WifiAdapter WiFi;
void set_wifi_netif(esp_netif_t* netif);
void refresh_wifi_signal();
