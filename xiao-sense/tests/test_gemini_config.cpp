// Exercise actual dashboard handlers against ESP-IDF cJSON without device I/O.
#include "cJSON.h"
#include "../src/gemini_client.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

using esp_err_t = int;
constexpr int ESP_OK = 0, HTTP_GET = 0, HTTP_POST = 1;
constexpr int HTTPD_500_INTERNAL_SERVER_ERROR = 500;
struct httpd_req_t {
  int method = HTTP_POST;
  size_t content_len = 0, read = 0;
  const char* uri = "/api/gemini/settings";
  std::string body, status = "200 OK", response;
  std::unordered_map<std::string,std::string> headers{{"Content-Type","application/json"},{"Host","digit"},{"Origin","http://digit"}};
};
namespace mock {
unsigned saves = 0, stops = 0, controlDepth = 0, starts = 0;
uint32_t generation = 1;
bool probe = false, testOnly = false, busy = false;
bool mission = false, wake = false, follow = false, manual = false, look = false, motion = false;
int left = 0, right = 0;
std::string apiKey;
}
namespace gemini_client {
Settings saved{true, "gemini-3.8-live", true, 30};
Settings settings() { return saved; }
bool configure(const char* apiKey, const char* model, bool dryRun, bool clearKey, char*, size_t, uint32_t timeout) {
  assert(mock::controlDepth > 0);
  ++mock::saves; mock::apiKey = apiKey;
  if (clearKey) saved.configured = false;
  else if (*apiKey) saved.configured = true;
  if (*model) std::snprintf(saved.model, sizeof(saved.model), "%s", model);
  saved.dryRun = dryRun;
  if (timeout) saved.idleTimeoutSec = timeout;
  return true;
}
}
namespace robot_actions {
Guard::Guard() { ++mock::controlDepth; }
Guard::~Guard() { --mock::controlDepth; }
Status snapshot() { assert(mock::controlDepth > 0); return {Command::None,"Idle",mock::mission,mock::generation}; }
uint32_t generation() { return mock::generation; }
void stop(const char*,bool) { assert(mock::controlDepth > 0); ++mock::stops; ++mock::generation; }
}
namespace wake_word {
struct Status { bool reacting; };
Status snapshot() { assert(mock::controlDepth > 0); return {mock::wake}; }
}
namespace person_follow {
struct Status { bool enabled; };
Status snapshot() { assert(mock::controlDepth > 0); return {mock::follow}; }
}
namespace zumo_link {
struct Status { bool manualDriving, lookActive, motionActive; int left, right; };
Status snapshot() { assert(mock::controlDepth > 0); return {mock::manual, mock::look, mock::motion, mock::left, mock::right}; }
}
namespace speech_commands {
bool startLive(uint32_t generation, bool probe, bool testOnly) {
  assert(mock::controlDepth > 0 && generation == mock::generation);
  if (mock::busy) return false;
  ++mock::starts; mock::probe = probe; mock::testOnly = testOnly;
  return true;
}
}
size_t httpd_req_get_hdr_value_len(httpd_req_t* request, const char* key) {
  const auto found=request->headers.find(key);
  return found==request->headers.end()?0:found->second.size();
}
int httpd_req_get_hdr_value_str(httpd_req_t* request, const char* key, char* output, size_t capacity) {
  const auto found=request->headers.find(key);
  if(found==request->headers.end()||found->second.size()>=capacity)return -1;
  std::memcpy(output,found->second.c_str(),found->second.size()+1); return ESP_OK;
}
int httpd_req_recv(httpd_req_t* request, char* output, size_t capacity) {
  const size_t count=std::min(capacity,request->body.size()-request->read);
  std::memcpy(output,request->body.data()+request->read,count);request->read+=count;
  return static_cast<int>(count);
}
void httpd_resp_set_status(httpd_req_t* request, const char* status) { request->status=status; }
esp_err_t sendJson(httpd_req_t* request, const char* json) { request->response=json;return ESP_OK; }
esp_err_t httpd_resp_send_err(httpd_req_t* request,int code,const char* error) {request->status=std::to_string(code);request->response=error;return ESP_OK;}
esp_err_t controlReply(httpd_req_t* request,const char* message) {
  request->response=std::string("{\"message\":\"")+message+"\",\"generation\":"+std::to_string(mock::generation)+"}";
  return ESP_OK;
}
#include "gemini_config_handlers.inc"

std::string object(const std::string& fields="") {
  return "{\"generation\":"+std::to_string(mock::generation)+(fields.empty()?"":","+fields)+"}";
}
httpd_req_t request(const std::string& body,const char* uri="/api/gemini/settings",const char* type="application/json",const char* origin="http://digit") {
  httpd_req_t result;result.body=body;result.content_len=body.size();result.uri=uri;
  result.headers["Content-Type"]=type;
  if(origin)result.headers["Origin"]=origin;else result.headers.erase("Origin");
  const int code=std::strstr(uri,"/live/")?geminiLiveHandler(&result):geminiSettingsHandler(&result);
  assert(code==ESP_OK&&mock::controlDepth==0);
  return result;
}
void reject(const std::string& body,const char* status="400",const char* uri="/api/gemini/settings",const char* type="application/json",const char* origin="http://digit") {
  const auto saves=mock::saves,stops=mock::stops,starts=mock::starts;
  const auto result=request(body,uri,type,origin);
  assert(result.status.find(status)==0);
  assert(mock::saves==saves&&mock::stops==stops&&mock::starts==starts);
}
int main() {
  httpd_req_t get;get.method=HTTP_GET;
  assert(geminiSettingsHandler(&get)==ESP_OK&&mock::saves==0&&mock::stops==0);
  assert(get.response.find("api_key")==std::string::npos&&get.response.find("\"configured\":true")!=std::string::npos);
  assert(get.response.find("\"idle_timeout_sec\":30")!=std::string::npos);
  for(const char* type:{"","text/plain","application/x-www-form-urlencoded","application/jsonp","application/json trailing"})
    reject(object("\"api_key\":\"FAKE_TEST_KEY_123456\""),"415","/api/gemini/settings",type);
  reject(object(),"403","/api/gemini/settings","application/json","http://attacker.example");
  reject(object(),"403","/api/gemini/settings","application/json","null");
  httpd_req_t cross;cross.body=object();cross.content_len=cross.body.size();cross.headers.erase("Origin");cross.headers["Sec-Fetch-Site"]="cross-site";
  assert(geminiSettingsHandler(&cross)==ESP_OK&&cross.status.find("403")==0);
  auto saved=request(object("\"api_key\":\"FAKE_TEST_KEY_123456\",\"model\":\"gemini-3.8-live\",\"dry_run\":false,\"idle_timeout_sec\":45"),"/api/gemini/settings","Application/JSON; charset=utf-8");
  assert(saved.status=="200 OK"&&mock::saves==1&&mock::stops==1);
  assert(mock::apiKey=="FAKE_TEST_KEY_123456"&&!gemini_client::saved.dryRun&&gemini_client::saved.idleTimeoutSec==45);
  assert(saved.response.find(mock::apiKey)==std::string::npos);
  const std::string longKey="AQ."+std::string(gemini_client::kMaxApiKeyLength-3,'x');
  assert(request(object("\"api_key\":\""+longKey+"\""),"/api/gemini/settings","application/json",nullptr).status=="200 OK");
  assert(mock::apiKey==longKey);
  for(const char* fields:{
    "\"dry_run\":true,\"dry_run\":false", "\"extra\":1", "\"remove_key\":true,\"remove_key\":false",
    "\"generation\":1", "\"api_key\":null", "\"api_key\":\"short\"", "\"api_key\":\"FAKE_TEST KEY_123456\"",
    "\"model\":\"gemini-other\"", "\"model\":123", "\"dry_run\":\"false\"", "\"remove_key\":1",
    "\"idle_timeout_sec\":9", "\"idle_timeout_sec\":301", "\"idle_timeout_sec\":30.5", "\"idle_timeout_sec\":\"30\"",
    "\"api_key\":\"FAKE_TEST_KEY_123456\\u0000suffix\"", "\"model\":\"gemini-3.8-live\\u0000suffix\"",
    "\"remove_key\":true,\"api_key\":\"FAKE_TEST_KEY_123456\"", "\"dry_run\":[true]", "\"model\":{\"nested\":1}",
    "\"mo\\u0064el\":\"gemini-3.8-live\",\"model\":\"gemini-3.8-live\""}) reject(object(fields));
  reject(object("\"api_key\":\""+std::string(gemini_client::kMaxApiKeyLength+1,'x')+"\""));
  reject(object()+" extra");reject(object()+'\0'+"ignored");reject("[]");reject("null");reject("");reject(std::string(1025,' '));
  reject(std::string("{\"generation\":1,\"model\":")+std::string(450,'[')+"0"+std::string(450,']')+"}");
  reject("{\"generation\":0}","409");reject("{\"generation\":4294967296}","409");reject("{\"generation\":1.5}","409");reject("{\"generation\":\"1\"}","409");reject("{}","409");
  const uint32_t stale=mock::generation;
  assert(request(object("\"remove_key\":true")).status=="200 OK"&&!gemini_client::saved.configured);
  reject("{\"generation\":"+std::to_string(stale)+"}","409");
  reject(object(),"409","/api/gemini/live/test");
  gemini_client::saved.configured=true;
  for (bool* activity : {&mock::mission, &mock::wake, &mock::manual, &mock::look, &mock::motion}) {
    *activity = true;
    reject(object(), "409", "/api/gemini/live/test");
    reject(object("\"mode\":\"test_only\""), "409", "/api/gemini/live/start");
    reject(object("\"mode\":\"conversation\""), "409", "/api/gemini/live/start");
    *activity = false;
  }
  for (int* motor : {&mock::left, &mock::right}) {
    *motor = 100;
    reject(object(), "409", "/api/gemini/live/test");
    reject(object("\"mode\":\"test_only\""), "409", "/api/gemini/live/start");
    *motor = 0;
  }
  assert(request(object(),"/api/gemini/live/test").status=="200 OK"&&mock::probe&&!mock::testOnly);
  reject(object("\"mode\":\"conversation\""),"400","/api/gemini/live/test");
  reject(object(),"400","/api/gemini/live/start");
  reject(object("\"mode\":\"arbitrary\""),"400","/api/gemini/live/start");
  assert(request(object("\"mode\":\"test_only\""),"/api/gemini/live/start").status=="200 OK"&&!mock::probe&&mock::testOnly);
  assert(request(object("\"mode\":\"conversation\""),"/api/gemini/live/start?source=dashboard").status=="200 OK"&&!mock::probe&&!mock::testOnly);
  mock::busy=true;reject(object("\"mode\":\"conversation\""),"409","/api/gemini/live/start");mock::busy=false;
  const auto beforeStops=mock::stops;
  assert(request(object(),"/api/gemini/live/stop").status=="200 OK"&&mock::stops==beforeStops+1);
  std::puts("PASS: sanitized key settings, same-origin bounded strict JSON, duplicate/NUL/nested input, stale generation, and isolated Live controls");
}
