#include "web_server.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_http_server.h>

#include "app_config.h"
#include "audio_engine.h"
#include "camera_service.h"

namespace {

httpd_handle_t controlServer = nullptr;
httpd_handle_t streamServer = nullptr;

constexpr char kStreamContentType[] =
    "multipart/x-mixed-replace;boundary=frame";
constexpr char kStreamBoundary[] = "\r\n--frame\r\n";
constexpr char kStreamPartHeader[] =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

const char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Digit Sense</title>
  <style>
    :root{color-scheme:dark;--cyan:#36e4ff;--ink:#071014;--panel:#101d23;--muted:#8da4ae;--red:#ff5470}
    *{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at top,#17343f,var(--ink) 58%);color:#eefcff;font:16px system-ui,sans-serif;min-height:100vh}
    main{width:min(960px,100%);margin:auto;padding:22px}.top{display:flex;justify-content:space-between;gap:15px;align-items:center;margin-bottom:16px}
    h1{font-size:clamp(24px,5vw,40px);margin:0;letter-spacing:.03em}.pill{border:1px solid #35515d;border-radius:999px;padding:8px 13px;color:var(--muted)}
    .panel{background:rgba(16,29,35,.9);border:1px solid #28434e;border-radius:20px;overflow:hidden;box-shadow:0 22px 70px #0008}
    .video{aspect-ratio:4/3;background:#020506;display:grid;place-items:center;position:relative}.video img{width:100%;height:100%;object-fit:contain}
    .video span{position:absolute;bottom:12px;left:12px;background:#071014cc;border-radius:999px;padding:7px 11px;font-size:13px}
    .controls{padding:18px;display:grid;grid-template-columns:repeat(4,1fr);gap:10px}button{border:0;border-radius:13px;padding:14px 10px;font-weight:750;cursor:pointer;background:#233740;color:#fff}
    button.primary{background:var(--cyan);color:#00151c}button.danger{background:#4a1d29;color:#ffdbe2}button:disabled{opacity:.35;cursor:not-allowed}
    .details{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;padding:0 18px 18px}.metric{background:#0a151a;border-radius:12px;padding:12px}.metric b{display:block;font-size:20px;margin-top:3px}.metric small{color:var(--muted)}
    #message{min-height:24px;padding:0 18px 18px;color:var(--muted)}#message.error{color:var(--red)}
    @media(max-width:650px){.controls{grid-template-columns:1fr 1fr}.details{grid-template-columns:1fr}.top{align-items:flex-start;flex-direction:column}}
  </style>
</head>
<body>
<main>
  <div class="top"><div><h1>Digit Sense</h1><div style="color:var(--muted)">Live camera + ReSpeaker recorder</div></div><div class="pill" id="state">CONNECTING</div></div>
  <section class="panel">
    <div class="video"><img id="stream" alt="Digit camera stream"><span id="camera">Camera starting...</span></div>
    <div class="controls">
      <button class="primary" id="record" onclick="action('/api/record/start')">Record</button>
      <button class="danger" id="stopRecord" onclick="action('/api/record/stop')">Stop &amp; play</button>
      <button id="replay" onclick="action('/api/play')">Replay</button>
      <button id="stopPlay" onclick="action('/api/play/stop')">Stop playback</button>
    </div>
    <div class="details">
      <div class="metric"><small>Recording</small><b id="duration">0.0 s</b></div>
      <div class="metric"><small>PSRAM free</small><b id="memory">--</b></div>
      <div class="metric"><small>Wi-Fi clients</small><b id="clients">--</b></div>
    </div>
    <div id="message">Waiting for device status...</div>
  </section>
</main>
<script>
const $=id=>document.getElementById(id);
$('stream').src=`http://${location.hostname}:81/stream`;
async function action(path){
  try{const response=await fetch(path,{method:'POST'});const data=await response.json();$('message').textContent=data.message||'';$('message').className=response.ok?'':'error';await refresh();}
  catch(error){$('message').textContent=error.message;$('message').className='error';}
}
function formatBytes(bytes){return bytes>1048576?(bytes/1048576).toFixed(1)+' MB':Math.round(bytes/1024)+' KB'}
async function refresh(){
  try{
    const response=await fetch('/api/status',{cache:'no-store'});const s=await response.json();
    $('state').textContent=s.audio_state;$('camera').textContent=s.camera_ready?'LIVE · 640×480':s.camera_error;
    const elapsed=s.audio_state==='RECORDING'?s.recording_elapsed_ms:s.captured_ms;
    $('duration').textContent=(Math.min(elapsed,s.max_recording_ms)/1000).toFixed(1)+' s';
    $('memory').textContent=formatBytes(s.psram_free);$('clients').textContent=s.wifi_clients;
    $('record').disabled=s.audio_state!=='READY';$('stopRecord').disabled=s.audio_state!=='RECORDING';
    $('replay').disabled=s.audio_state!=='READY'||s.captured_ms===0;$('stopPlay').disabled=s.audio_state!=='PLAYING';
    if(s.audio_error){$('message').textContent=s.audio_error;$('message').className='error';}
    else if($('message').className==='error'){$('message').textContent='Ready';$('message').className='';}
  }catch(error){$('state').textContent='OFFLINE';$('message').textContent='Device status unavailable';$('message').className='error';}
}
refresh();setInterval(refresh,350);
</script>
</body>
</html>
)HTML";

void setNoCache(httpd_req_t* request) {
  httpd_resp_set_hdr(request, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(request, "Pragma", "no-cache");
}

esp_err_t sendJson(httpd_req_t* request, const char* json) {
  httpd_resp_set_type(request, "application/json");
  setNoCache(request);
  return httpd_resp_sendstr(request, json);
}

esp_err_t rootHandler(httpd_req_t* request) {
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  setNoCache(request);
  return httpd_resp_send(request, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

void escapeJsonString(const char* input, char* output, size_t outputSize) {
  if (outputSize == 0) {
    return;
  }
  size_t writeIndex = 0;
  for (size_t readIndex = 0; input[readIndex] != '\0' &&
                             writeIndex + 1 < outputSize;
       ++readIndex) {
    const char character = input[readIndex];
    if ((character == '"' || character == '\\') &&
        writeIndex + 2 < outputSize) {
      output[writeIndex++] = '\\';
    }
    output[writeIndex++] = character;
  }
  output[writeIndex] = '\0';
}

esp_err_t statusHandler(httpd_req_t* request) {
  const AudioStatus audio = audioEngine.snapshot();
  char audioError[192];
  char cameraError[192];
  escapeJsonString(audio.error, audioError, sizeof(audioError));
  escapeJsonString(camera_service::lastError(), cameraError,
                   sizeof(cameraError));

  char json[640];
  snprintf(
      json, sizeof(json),
      "{\"audio_state\":\"%s\",\"audio_available\":%s,"
      "\"audio_error\":\"%s\",\"captured_ms\":%lu,"
      "\"recording_elapsed_ms\":%lu,\"max_recording_ms\":%lu,"
      "\"camera_ready\":%s,\"camera_error\":\"%s\","
      "\"psram_free\":%u,\"wifi_clients\":%u}",
      AudioEngine::stateName(audio.state), audio.available ? "true" : "false",
      audioError, static_cast<unsigned long>(audio.capturedMilliseconds),
      static_cast<unsigned long>(audio.recordingElapsedMilliseconds),
      static_cast<unsigned long>(app_config::kMaximumRecordingSeconds * 1000UL),
      camera_service::ready() ? "true" : "false", cameraError,
      static_cast<unsigned int>(ESP.getFreePsram()),
      static_cast<unsigned int>(WiFi.softAPgetStationNum()));
  return sendJson(request, json);
}

esp_err_t sendActionResult(httpd_req_t* request, AudioActionResult result,
                           const char* acceptedMessage) {
  const char* status = "200 OK";
  const char* message = acceptedMessage;
  switch (result) {
    case AudioActionResult::Accepted:
      break;
    case AudioActionResult::Conflict:
      status = "409 Conflict";
      message = "That action is not allowed in the current audio state";
      break;
    case AudioActionResult::NoRecording:
      status = "409 Conflict";
      message = "There is no recording to replay yet";
      break;
    case AudioActionResult::Unavailable:
    default:
      status = "503 Service Unavailable";
      message = "The ReSpeaker audio engine is unavailable";
      break;
  }

  char json[180];
  snprintf(json, sizeof(json), "{\"message\":\"%s\"}", message);
  httpd_resp_set_status(request, status);
  return sendJson(request, json);
}

esp_err_t startRecordingHandler(httpd_req_t* request) {
  return sendActionResult(request, audioEngine.startRecording(),
                          "Recording started");
}

esp_err_t stopRecordingHandler(httpd_req_t* request) {
  return sendActionResult(request, audioEngine.stopRecording(),
                          "Recording will stop and play automatically");
}

esp_err_t playHandler(httpd_req_t* request) {
  return sendActionResult(request, audioEngine.playLatest(),
                          "Playback started");
}

esp_err_t stopPlaybackHandler(httpd_req_t* request) {
  return sendActionResult(request, audioEngine.stopPlayback(),
                          "Playback will stop");
}

esp_err_t streamHandler(httpd_req_t* request) {
  if (!camera_service::ready()) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_sendstr(request, camera_service::lastError());
  }

  httpd_resp_set_type(request, kStreamContentType);
  setNoCache(request);
  const TickType_t framePeriod =
      pdMS_TO_TICKS(1000 / app_config::kTargetCameraFps);

  while (true) {
    const TickType_t frameStartedAt = xTaskGetTickCount();
    camera_fb_t* frame = esp_camera_fb_get();
    if (frame == nullptr) {
      Serial.println("Camera: failed to capture a frame");
      return ESP_FAIL;
    }

    char partHeader[72];
    const int headerLength = snprintf(partHeader, sizeof(partHeader),
                                      kStreamPartHeader,
                                      static_cast<unsigned int>(frame->len));
    esp_err_t result = httpd_resp_send_chunk(request, kStreamBoundary,
                                             strlen(kStreamBoundary));
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(request, partHeader, headerLength);
    }
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(
          request, reinterpret_cast<const char*>(frame->buf), frame->len);
    }
    esp_camera_fb_return(frame);

    if (result != ESP_OK) {
      return result;
    }

    const TickType_t elapsed = xTaskGetTickCount() - frameStartedAt;
    if (elapsed < framePeriod) {
      vTaskDelay(framePeriod - elapsed);
    }
  }
}

bool registerUri(httpd_handle_t server, const char* uri,
                 httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
  httpd_uri_t definition = {};
  definition.uri = uri;
  definition.method = method;
  definition.handler = handler;
  definition.user_ctx = nullptr;
  return httpd_register_uri_handler(server, &definition) == ESP_OK;
}

}  // namespace

namespace web_server {

bool begin() {
  httpd_config_t controlConfig = HTTPD_DEFAULT_CONFIG();
  controlConfig.server_port = app_config::kControlServerPort;
  controlConfig.ctrl_port = 32768;
  controlConfig.max_uri_handlers = 8;
  controlConfig.lru_purge_enable = true;

  if (httpd_start(&controlServer, &controlConfig) != ESP_OK) {
    Serial.println("Web: could not start the control server");
    return false;
  }

  const bool controlRoutesReady =
      registerUri(controlServer, "/", HTTP_GET, rootHandler) &&
      registerUri(controlServer, "/api/status", HTTP_GET, statusHandler) &&
      registerUri(controlServer, "/api/record/start", HTTP_POST,
                  startRecordingHandler) &&
      registerUri(controlServer, "/api/record/stop", HTTP_POST,
                  stopRecordingHandler) &&
      registerUri(controlServer, "/api/play", HTTP_POST, playHandler) &&
      registerUri(controlServer, "/api/play/stop", HTTP_POST,
                  stopPlaybackHandler);
  if (!controlRoutesReady) {
    Serial.println("Web: could not register all control routes");
    return false;
  }

  httpd_config_t streamConfig = HTTPD_DEFAULT_CONFIG();
  streamConfig.server_port = app_config::kStreamServerPort;
  streamConfig.ctrl_port = 32769;
  streamConfig.lru_purge_enable = true;
  if (httpd_start(&streamServer, &streamConfig) != ESP_OK ||
      !registerUri(streamServer, "/stream", HTTP_GET, streamHandler)) {
    Serial.println("Web: could not start the camera stream server");
    return false;
  }

  Serial.println("Web: controls on port 80, camera stream on port 81");
  return true;
}

}  // namespace web_server

