#pragma once
#include <cstdint>
#include <esp_http_server.h>
namespace photo_store {
void begin();
bool capture(uint32_t generation);
void cancel();
bool busy();
bool ready();
bool failed();
uint32_t revision();
esp_err_t send(httpd_req_t* request);
}
