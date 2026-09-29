#include "web_server.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_http_server.h>

#include "app_config.h"
#include "audio_engine.h"
#include "camera_service.h"
#include "person_follow.h"
#include "vision_service.h"
#include "zumo_link.h"

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
    .video canvas{position:absolute;inset:0;width:100%;height:100%;pointer-events:none}
    .video span{position:absolute;bottom:12px;left:12px;background:#071014cc;border-radius:999px;padding:7px 11px;font-size:13px}
    .person-badge{position:absolute;top:12px;left:12px;padding:9px 14px;border-radius:999px;background:#36e4ff;color:#00151c;font-size:15px;box-shadow:0 4px 20px #0009}
    [hidden]{display:none!important}
    .controls{padding:18px;display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}button{border:0;border-radius:13px;padding:14px 10px;font-weight:750;cursor:pointer;background:#233740;color:#fff}
    button.primary{background:var(--cyan);color:#00151c}button.danger{background:#4a1d29;color:#ffdbe2}button:disabled{opacity:.35;cursor:not-allowed}
    .volume{margin:0 18px 18px;padding:14px 16px;background:#0a151a;border-radius:14px}.volume label{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px}.volume label span{color:var(--muted)}
    .volume input{width:100%;accent-color:var(--cyan);cursor:pointer}.volume input:disabled{opacity:.35;cursor:not-allowed}
    .details{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;padding:0 18px 18px}.metric{background:#0a151a;border-radius:12px;padding:12px}.metric b{display:block;font-size:20px;margin-top:3px}.metric small{color:var(--muted)}
    .zumo{padding:18px;border-top:1px solid #28434e}.zumo h2{margin:0 0 8px;font-size:22px}.zumo p{color:var(--muted);margin:0 0 14px}
    .zumo-status{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:14px}.zumo-status span{background:#0a151a;border-radius:10px;padding:8px 10px}
    .drive-pad{display:grid;grid-template-columns:repeat(3,minmax(80px,110px));gap:8px;justify-content:center;touch-action:none;user-select:none}
    .drive-pad button{min-height:64px;touch-action:none}.drive-pad .forward{grid-column:2}.drive-pad .left{grid-column:1}.drive-pad .stop{grid-column:2}.drive-pad .right{grid-column:3}.drive-pad .back{grid-column:2}
    .zumo-actions{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px}.zumo-actions button{flex:1 1 120px}
    .speed{display:flex;align-items:center;gap:12px;margin:14px 0}.speed input{flex:1;accent-color:var(--cyan)}
    #message{min-height:24px;padding:0 18px 18px;color:var(--muted)}#message.error{color:var(--red)}
    @media(max-width:650px){.controls{grid-template-columns:1fr 1fr}.details{grid-template-columns:1fr}.top{align-items:flex-start;flex-direction:column}}
  </style>
</head>
<body>
<main>
  <div class="top"><div><h1>Digit Sense</h1><div style="color:var(--muted)">Live camera + ReSpeaker recorder</div></div><div class="pill" id="state">CONNECTING</div></div>
  <section class="panel">
    <div class="video"><img id="stream" alt="Digit camera stream"><canvas id="boxes" width="640" height="480" aria-hidden="true"></canvas><strong class="person-badge" id="personBadge" hidden>Naz Detected</strong><span id="camera">Camera starting...</span></div>
    <div class="controls">
      <button class="primary" id="record" onclick="action('/api/record/start')">Record</button>
      <button class="danger" id="stopRecord" onclick="action('/api/record/stop')">Stop &amp; play</button>
      <button id="replay" onclick="action('/api/play')">Replay</button>
      <button id="tone" onclick="action('/api/tone')">Test speaker</button>
      <button id="stopPlay" onclick="action('/api/play/stop')">Stop playback</button>
      <button id="takePhoto" onclick="takePhoto()" disabled>Take picture</button>
      <button id="visionToggle" onclick="toggleVision()" disabled>Start person detection</button>
      <button id="followToggle" onclick="toggleFollow()" disabled>Turn with person</button>
    </div>
    <div class="volume">
      <label for="volume"><span>Speaker volume</span><b><span id="volumeValue">70</span>%</b></label>
      <input id="volume" type="range" min="0" max="100" step="1" value="70" oninput="queueVolume(this.value)" aria-label="Speaker volume">
    </div>
    <div class="details">
      <div class="metric"><small>Recording</small><b id="duration">0.0 s</b></div>
      <div class="metric"><small>PSRAM free</small><b id="memory">--</b></div>
      <div class="metric"><small>Network address</small><b id="network">--</b></div>
    </div>
    <div id="message">Waiting for device status...</div>
    <div class="zumo" id="followPanel">Person turning: <strong id="followState">Off</strong><p>Tracks rotate in place toward the largest person box. Stops when centered or the person is lost.</p></div>
    <div class="zumo">
      <h2>Zumo drive</h2><p>Hold a direction to move. Release to stop. Calibrate on safe floor first.</p>
      <div class="zumo-status"><span id="zumoLink">Zumo: connecting</span><span id="zumoReady">Drive: locked</span><span id="zumoBattery">Battery: --</span><span id="zumoFault">Fault: --</span></div>
      <div class="drive-pad" id="drivePad">
        <button class="forward" data-drive="forward" aria-label="Hold to drive forward">▲</button>
        <button class="left" data-drive="left" aria-label="Hold to turn left">◀</button>
        <button class="stop danger" id="zumoStop" aria-label="Stop Zumo">STOP</button>
        <button class="right" data-drive="right" aria-label="Hold to turn right">▶</button>
        <button class="back" data-drive="back" aria-label="Hold to reverse">▼</button>
      </div>
      <label class="speed" for="driveSpeed">Drive speed <input id="driveSpeed" type="range" min="500" max="6000" step="100" value="4000"><b id="driveSpeedValue">4000</b></label>
      <div class="zumo-actions"><button id="zumoCalibrate">Calibrate floor</button><button id="zumoHappy">Happy face</button><button id="zumoSad">Sad face</button></div>
      <div id="zumoMessage">Connecting to Zumo...</div>
    </div>
  </section>
</main>
<script>
const $=id=>document.getElementById(id);
$('stream').src=`http://${location.hostname}:81/stream`;
let heldDirection=null,driveTimer=null,driveInFlight=false,zumoReady=false,cameraReady=false,photoPending=false,visionEnabled=false,visionPending=false,followEnabled=false,followPending=false,followHeartbeatPending=false;
const driveButtons=[...document.querySelectorAll('[data-drive]')];
function speeds(direction){const value=Number($('driveSpeed').value);return {forward:[value,value],back:[-value,-value],left:[-value,value],right:[value,-value]}[direction]||[0,0]}
async function postZumo(path){const response=await fetch(path,{method:'POST',cache:'no-store'});const data=await response.json();if(!response.ok)throw Error(data.message||'Zumo command failed');return data}
async function stopZumo(){heldDirection=null;if(driveTimer!==null){clearInterval(driveTimer);driveTimer=null}try{await postZumo('/api/zumo/stop')}catch(error){$('zumoMessage').textContent=error.message}}
async function sendDrive(){if(!heldDirection||driveInFlight)return;driveInFlight=true;const [left,right]=speeds(heldDirection);try{await postZumo(`/api/zumo/drive?left=${left}&right=${right}`)}catch(error){$('zumoMessage').textContent=error.message;await stopZumo()}finally{driveInFlight=false}}
function beginDrive(direction,event){if(!zumoReady||heldDirection)return;event.preventDefault();event.currentTarget.setPointerCapture(event.pointerId);heldDirection=direction;sendDrive();driveTimer=setInterval(sendDrive,100)}
for(const button of driveButtons){button.addEventListener('pointerdown',event=>beginDrive(button.dataset.drive,event));button.addEventListener('pointerup',stopZumo);button.addEventListener('pointercancel',stopZumo);button.addEventListener('lostpointercapture',()=>{if(heldDirection)stopZumo()})}
$('zumoStop').addEventListener('click',stopZumo);
async function takePhoto(){if(photoPending)return;photoPending=true;const button=$('takePhoto');button.disabled=true;try{const response=await fetch('/api/camera/photo',{cache:'no-store'});if(!response.ok)throw Error((await response.text())||'Photo capture failed');const blob=await response.blob();if(blob.type!=='image/jpeg'||blob.size===0)throw Error('Camera returned an invalid photo');const url=URL.createObjectURL(blob);const link=document.createElement('a');link.href=url;link.download='digit-'+new Date().toISOString().replace(/[:.]/g,'-')+'.jpg';document.body.append(link);link.click();link.remove();setTimeout(()=>URL.revokeObjectURL(url),10000);$('message').textContent='Picture saved';$('message').className=''}catch(error){$('message').textContent=error.message;$('message').className='error'}finally{photoPending=false;button.disabled=!cameraReady}}
async function toggleVision(){if(visionPending)return;visionPending=true;$('visionToggle').disabled=true;try{const response=await fetch('/api/vision?enabled='+(visionEnabled?'0':'1'),{method:'POST',cache:'no-store'});const data=await response.json();if(!response.ok)throw Error(data.message||'Could not change person detection');$('message').textContent=data.message;$('message').className=''}catch(error){$('message').textContent=error.message;$('message').className='error'}finally{visionPending=false;await refresh()}}
async function toggleFollow(){if(followPending)return;followPending=true;$('followToggle').disabled=true;try{const response=await fetch('/api/follow?enabled='+(followEnabled?'0':'1'),{method:'POST',cache:'no-store'});const data=await response.json();if(!response.ok)throw Error(data.message||'Could not change person turning');$('message').textContent=data.message;$('message').className=''}catch(error){$('message').textContent=error.message;$('message').className='error'}finally{followPending=false;await refresh()}}
async function followHeartbeat(){if(!followEnabled||followHeartbeatPending||document.hidden)return;followHeartbeatPending=true;try{const response=await fetch('/api/follow/heartbeat',{method:'POST',cache:'no-store'});if(!response.ok)followEnabled=false}catch(error){followEnabled=false}finally{followHeartbeatPending=false}}
setInterval(followHeartbeat,500);
$('driveSpeed').addEventListener('input',event=>{$('driveSpeedValue').textContent=event.target.value});
$('zumoCalibrate').addEventListener('click',async()=>{try{await postZumo('/api/zumo/calibrate');$('zumoMessage').textContent='Calibrating floor...'}catch(error){$('zumoMessage').textContent=error.message}});
for(const [id,value] of [['zumoHappy','HAPPY'],['zumoSad','SAD']]){$(id).addEventListener('click',async()=>{try{await postZumo('/api/zumo/face?value='+value)}catch(error){$('zumoMessage').textContent=error.message}})}
window.addEventListener('blur',stopZumo);document.addEventListener('visibilitychange',()=>{if(document.hidden)stopZumo()});
async function refreshZumo(){try{const response=await fetch('/api/zumo/status',{cache:'no-store'});if(!response.ok)throw Error('Status unavailable');const s=await response.json();zumoReady=s.connected&&s.ready&&!s.calibrating;$('zumoLink').textContent=s.connected?'Zumo: connected':'Zumo: disconnected';$('zumoReady').textContent=s.calibrating?'Drive: calibrating':zumoReady?'Drive: ready':'Drive: locked';$('zumoBattery').textContent=s.battery_mv>=0?`Battery: ${(s.battery_mv/1000).toFixed(2)} V`:'Battery: --';$('zumoFault').textContent='Fault: '+s.fault;for(const button of driveButtons)button.disabled=!zumoReady||followEnabled;$('zumoCalibrate').disabled=!s.connected||s.calibrating||!!heldDirection||followEnabled;$('zumoHappy').disabled=$('zumoSad').disabled=!s.connected||!!heldDirection||followEnabled;if(!zumoReady&&heldDirection)stopZumo();let message='Waiting for Zumo connection...';if(s.connected){if(s.calibrating)message='Calibrating floor...';else if(s.fault!=='NONE')message='Zumo fault: '+s.fault;else if(s.last_error!=='NONE')message=s.last_error;else if(zumoReady)message='Hold a direction to drive.';else message='Connected. Calibrate floor on a safe surface to enable driving.'}$('zumoMessage').textContent=message}catch(error){zumoReady=false;for(const button of driveButtons)button.disabled=true;$('zumoLink').textContent='Zumo: unavailable';$('zumoMessage').textContent='Zumo status unavailable';if(heldDirection)stopZumo()}}
refreshZumo();setInterval(refreshZumo,500);
async function action(path){
  try{const response=await fetch(path,{method:'POST'});const data=await response.json();$('message').textContent=data.message||'';$('message').className=response.ok?'':'error';await refresh();}
  catch(error){$('message').textContent=error.message;$('message').className='error';}
}
let volumeTimer=null,volumeSending=false;
function queueVolume(value){
  $('volumeValue').textContent=value;
  if(volumeTimer!==null)clearTimeout(volumeTimer);
  volumeTimer=setTimeout(()=>sendVolume(value),150);
}
async function sendVolume(value){
  volumeTimer=null;volumeSending=true;
  try{const response=await fetch('/api/volume?value='+encodeURIComponent(value),{method:'POST'});const data=await response.json();$('message').textContent=data.message||'';$('message').className=response.ok?'':'error';}
  catch(error){$('message').textContent=error.message;$('message').className='error';}
  finally{volumeSending=false;await refresh();}
}
function formatBytes(bytes){return bytes>1048576?(bytes/1048576).toFixed(1)+' MB':Math.round(bytes/1024)+' KB'}
function drawBoxes(s){const ctx=$('boxes').getContext('2d');ctx.clearRect(0,0,640,480);if(!s.vision_enabled||!s.boxes)return;const sx=640/(s.vision_width||320),sy=480/(s.vision_height||240);ctx.strokeStyle='#36e4ff';ctx.fillStyle='#36e4ff';ctx.lineWidth=4;ctx.font='bold 18px system-ui';for(const b of s.boxes){const x=b.x1*sx,y=b.y1*sy,w=(b.x2-b.x1)*sx,h=(b.y2-b.y1)*sy;ctx.strokeRect(x,y,w,h);ctx.fillText(`Person ${b.score}%`,x+4,Math.max(22,y-7))}}
let refreshPending=false;
async function refresh(){
  if(refreshPending)return;
  refreshPending=true;
  try{
    const response=await fetch('/api/status',{cache:'no-store'});if(!response.ok)throw new Error('Status HTTP '+response.status);const s=await response.json();
    cameraReady=s.camera_ready;visionEnabled=s.vision_enabled;followEnabled=s.follow_enabled;$('takePhoto').disabled=!cameraReady||photoPending;$('visionToggle').disabled=!s.vision_ready||visionPending;$('visionToggle').textContent=visionEnabled?'Stop person detection':'Start person detection';$('followToggle').disabled=!followEnabled&&(!visionEnabled||!zumoReady)||followPending;$('followToggle').textContent=followEnabled?'Stop turning':'Turn with person';$('followState').textContent=s.follow_state+(s.follow_target_x>=0?` · target ${s.follow_target_x}/${s.vision_width}`:'');$('state').textContent=s.audio_state;const rate=s.vision_interval_ms>0&&s.vision_age_ms<2000?` · ${(1000/s.vision_interval_ms).toFixed(1)} fps`:' · measuring';$('camera').textContent=cameraReady?(s.vision_ready?(visionEnabled?'LIVE · 640×480 · Vision on'+rate:'LIVE · 640×480 · Vision off'):'LIVE · 640×480 · Vision unavailable'):s.camera_error;$('personBadge').hidden=!visionEnabled||!s.person_detected;drawBoxes(s);
    const elapsed=s.audio_state==='RECORDING'?s.recording_elapsed_ms:s.captured_ms;
    $('duration').textContent=(Math.min(elapsed,s.max_recording_ms)/1000).toFixed(1)+' s';
    $('memory').textContent=formatBytes(s.psram_free);$('network').textContent=s.wifi_ip;
    if(volumeTimer===null&&!volumeSending){$('volume').value=s.speaker_volume;$('volumeValue').textContent=s.speaker_volume;}
    $('volume').disabled=!s.audio_available;
    $('record').disabled=s.audio_state!=='READY';$('stopRecord').disabled=s.audio_state!=='RECORDING';
    $('replay').disabled=s.audio_state!=='READY'||s.captured_ms===0;$('tone').disabled=s.audio_state!=='READY';$('stopPlay').disabled=s.audio_state!=='PLAYING';
    if(s.audio_error){$('message').textContent=s.audio_error;$('message').className='error';}
    else if($('message').className==='error'||$('message').textContent==='Waiting for device status...'){$('message').textContent='Ready';$('message').className='';}
  }catch(error){cameraReady=false;$('takePhoto').disabled=true;$('visionToggle').disabled=true;$('personBadge').hidden=true;$('state').textContent='OFFLINE';$('message').textContent='Device status unavailable';$('message').className='error';}
  finally{refreshPending=false;}
}
refresh();setInterval(refresh,250);
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
  Serial.printf("[%10lu] Web: GET /\n", static_cast<unsigned long>(millis()));
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
  static uint32_t lastStatusLogMs = 0;
  const AudioStatus audio = audioEngine.snapshot();
  const vision_service::Status vision = vision_service::snapshot();
  const person_follow::Status follow = person_follow::snapshot();
  if (lastStatusLogMs == 0 || millis() - lastStatusLogMs >= 5000) {
    lastStatusLogMs = millis();
    Serial.printf("[%10lu] Web: GET /api/status -> audio=%s captured=%lu ms\n",
                  static_cast<unsigned long>(millis()),
                  AudioEngine::stateName(audio.state),
                  static_cast<unsigned long>(audio.capturedMilliseconds));
  }
  char audioError[192];
  char cameraError[192];
  char visionError[96];
  escapeJsonString(audio.error, audioError, sizeof(audioError));
  escapeJsonString(camera_service::lastError(), cameraError,
                   sizeof(cameraError));
  escapeJsonString(vision.error, visionError, sizeof(visionError));

  char json[1800];
  const String wifiIp = WiFi.localIP().toString();
  snprintf(
      json, sizeof(json),
      "{\"audio_state\":\"%s\",\"audio_available\":%s,"
      "\"audio_error\":\"%s\",\"captured_ms\":%lu,"
      "\"recording_elapsed_ms\":%lu,\"max_recording_ms\":%lu,"
      "\"speaker_volume\":%u,"
      "\"camera_ready\":%s,\"camera_error\":\"%s\","
      "\"vision_ready\":%s,\"vision_enabled\":%s,\"person_detected\":%s,"
      "\"person_score_percent\":%u,\"vision_age_ms\":%lu,"
      "\"vision_inference_ms\":%lu,\"vision_decode_ms\":%lu,"
      "\"vision_total_ms\":%lu,\"vision_interval_ms\":%lu,"
      "\"vision_error\":\"%s\","
      "\"psram_free\":%u,\"wifi_connected\":%s,"
      "\"wifi_ip\":\"%s\",\"wifi_rssi\":%ld}",
      AudioEngine::stateName(audio.state), audio.available ? "true" : "false",
      audioError, static_cast<unsigned long>(audio.capturedMilliseconds),
      static_cast<unsigned long>(audio.recordingElapsedMilliseconds),
      static_cast<unsigned long>(app_config::kMaximumRecordingSeconds * 1000UL),
      audio.speakerVolumePercent,
      camera_service::ready() ? "true" : "false", cameraError,
      vision.ready ? "true" : "false",
      vision.enabled ? "true" : "false",
      vision.personDetected ? "true" : "false", vision.personScorePercent,
      static_cast<unsigned long>(vision.resultAgeMs),
      static_cast<unsigned long>(vision.inferenceMs),
      static_cast<unsigned long>(vision.decodeMs),
      static_cast<unsigned long>(vision.totalMs),
      static_cast<unsigned long>(vision.intervalMs), visionError,
      static_cast<unsigned int>(ESP.getFreePsram()),
      WiFi.status() == WL_CONNECTED ? "true" : "false", wifiIp.c_str(),
      static_cast<long>(WiFi.RSSI()));
  size_t used = strlen(json);
  if (used && json[used - 1] == '}') json[--used] = '\0';
  int written = snprintf(json + used, sizeof(json) - used,
                         ",\"vision_width\":%u,\"vision_height\":%u,\"boxes\":[",
                         vision.imageWidth, vision.imageHeight);
  used += written > 0 ? static_cast<size_t>(written) : 0;
  for (uint8_t index = 0; index < vision.boxCount && used < sizeof(json) - 100; ++index) {
    const auto& box = vision.boxes[index];
    written = snprintf(json + used, sizeof(json) - used,
                       "%s{\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d,\"score\":%u}",
                       index ? "," : "", box.x1, box.y1, box.x2, box.y2, box.scorePercent);
    used += written > 0 ? static_cast<size_t>(written) : 0;
  }
  snprintf(json + used, sizeof(json) - used,
           "],\"follow_enabled\":%s,\"follow_state\":\"%s\","
           "\"follow_target_x\":%d,\"follow_error_px\":%d,"
           "\"follow_score_percent\":%u}",
           follow.enabled ? "true" : "false", follow.phase,
           follow.targetX, follow.errorPixels, follow.scorePercent);
  return sendJson(request, json);
}

bool queryValue(httpd_req_t* request, const char* key, char* value,
                size_t valueSize) {
  const size_t queryLength = httpd_req_get_url_query_len(request);
  if (queryLength == 0 || queryLength >= 80) {
    return false;
  }
  char query[80];
  if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) {
    return false;
  }
  return httpd_query_key_value(query, key, value, valueSize) == ESP_OK;
}

esp_err_t visionHandler(httpd_req_t* request) {
  char value[4];
  if (!queryValue(request, "enabled", value, sizeof(value)) ||
      (strcmp(value, "0") != 0 && strcmp(value, "1") != 0)) {
    httpd_resp_set_status(request, "400 Bad Request");
    return sendJson(request, "{\"message\":\"Use enabled=0 or enabled=1\"}");
  }
  const bool enabled = strcmp(value, "1") == 0;
  if (!enabled) person_follow::setEnabled(false);
  if (!vision_service::setEnabled(enabled)) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    return sendJson(request, "{\"message\":\"Person detection is unavailable\"}");
  }
  return sendJson(request, enabled
                               ? "{\"message\":\"Person detection started\"}"
                               : "{\"message\":\"Person detection stopped\"}");
}

esp_err_t followHandler(httpd_req_t* request) {
  char value[4];
  if (!queryValue(request, "enabled", value, sizeof(value)) ||
      (strcmp(value, "0") != 0 && strcmp(value, "1") != 0)) {
    httpd_resp_set_status(request, "400 Bad Request");
    return sendJson(request, "{\"message\":\"Use enabled=0 or enabled=1\"}");
  }
  const bool enabled = strcmp(value, "1") == 0;
  if (!person_follow::setEnabled(enabled)) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"Start person detection and calibrate the connected Zumo first\"}");
  }
  return sendJson(request, enabled
                               ? "{\"message\":\"Turn-only person tracking started\"}"
                               : "{\"message\":\"Person turning stopped\"}");
}

esp_err_t followHeartbeatHandler(httpd_req_t* request) {
  if (!person_follow::heartbeat()) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"Person turning is off\"}");
  }
  return sendJson(request, "{\"message\":\"OK\"}");
}

bool parseSpeed(const char* value, int* result) {
  const bool negative = *value == '-';
  if (negative) {
    ++value;
  }
  if (*value == '\0') {
    return false;
  }
  int number = 0;
  for (const char* cursor = value; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') {
      return false;
    }
    number = number * 10 + (*cursor - '0');
    if (number > 6000) {
      return false;
    }
  }
  *result = negative ? -number : number;
  return true;
}

esp_err_t zumoStatusHandler(httpd_req_t* request) {
  const zumo_link::Status zumo = zumo_link::snapshot();
  char fault[48], lastError[64];
  escapeJsonString(zumo.fault, fault, sizeof(fault));
  escapeJsonString(zumo.lastError, lastError, sizeof(lastError));
  char json[320];
  snprintf(json, sizeof(json),
           "{\"connected\":%s,\"ready\":%s,\"calibrating\":%s,"
           "\"edge\":%s,\"left\":%d,\"right\":%d,\"battery_mv\":%d,"
           "\"reply_age_ms\":%lu,\"fault\":\"%s\",\"last_error\":\"%s\"}",
           zumo.connected ? "true" : "false", zumo.ready ? "true" : "false",
           zumo.calibrating ? "true" : "false", zumo.edge ? "true" : "false",
           zumo.left, zumo.right, zumo.batteryMillivolts,
           static_cast<unsigned long>(zumo.replyAgeMs), fault, lastError);
  return sendJson(request, json);
}

esp_err_t zumoDriveHandler(httpd_req_t* request) {
  char leftText[12], rightText[12];
  int left = 0, right = 0;
  if (!queryValue(request, "left", leftText, sizeof(leftText)) ||
      !queryValue(request, "right", rightText, sizeof(rightText)) ||
      !parseSpeed(leftText, &left) || !parseSpeed(rightText, &right)) {
    httpd_resp_set_status(request, "400 Bad Request");
    return sendJson(request, "{\"message\":\"Use left and right speeds from -6000 to 6000\"}");
  }
  person_follow::setEnabled(false);
  if (!zumo_link::drive(left, right)) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"Zumo is disconnected or not calibrated\"}");
  }
  return sendJson(request, "{\"message\":\"Drive command accepted\"}");
}

esp_err_t zumoStopHandler(httpd_req_t* request) {
  person_follow::setEnabled(false);
  zumo_link::stop();
  return sendJson(request, "{\"message\":\"Zumo stop requested\"}");
}

esp_err_t zumoCalibrateHandler(httpd_req_t* request) {
  person_follow::setEnabled(false);
  if (!zumo_link::calibrate()) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"Zumo must be connected and stopped\"}");
  }
  return sendJson(request, "{\"message\":\"Floor calibration requested\"}");
}

esp_err_t zumoFaceHandler(httpd_req_t* request) {
  char value[12];
  if (!queryValue(request, "value", value, sizeof(value)) ||
      (strcmp(value, "HAPPY") != 0 && strcmp(value, "SAD") != 0)) {
    httpd_resp_set_status(request, "400 Bad Request");
    return sendJson(request, "{\"message\":\"Use HAPPY or SAD\"}");
  }
  person_follow::setEnabled(false);
  if (!zumo_link::face(strcmp(value, "HAPPY") == 0)) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"Zumo must be connected and stopped\"}");
  }
  return sendJson(request, "{\"message\":\"Face command accepted\"}");
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
  Serial.printf("[%10lu] Web action: result=%d HTTP=%s message=\"%s\"\n",
                static_cast<unsigned long>(millis()),
                static_cast<int>(result), status, message);
  return sendJson(request, json);
}

esp_err_t startRecordingHandler(httpd_req_t* request) {
  Serial.printf("[%10lu] Web: POST /api/record/start\n",
                static_cast<unsigned long>(millis()));
  return sendActionResult(request, audioEngine.startRecording(),
                          "Recording started");
}

esp_err_t stopRecordingHandler(httpd_req_t* request) {
  Serial.printf("[%10lu] Web: POST /api/record/stop\n",
                static_cast<unsigned long>(millis()));
  return sendActionResult(request, audioEngine.stopRecording(),
                          "Recording will stop and play automatically");
}

esp_err_t playHandler(httpd_req_t* request) {
  Serial.printf("[%10lu] Web: POST /api/play\n",
                static_cast<unsigned long>(millis()));
  return sendActionResult(request, audioEngine.playLatest(),
                          "Playback started");
}

esp_err_t toneHandler(httpd_req_t* request) {
  Serial.printf("[%10lu] Web: POST /api/tone\n",
                static_cast<unsigned long>(millis()));
  return sendActionResult(request, audioEngine.playTestTone(),
                          "Playing a two-second speaker test tone");
}

esp_err_t stopPlaybackHandler(httpd_req_t* request) {
  Serial.printf("[%10lu] Web: POST /api/play/stop\n",
                static_cast<unsigned long>(millis()));
  return sendActionResult(request, audioEngine.stopPlayback(),
                          "Playback will stop");
}

esp_err_t volumeHandler(httpd_req_t* request) {
  char query[48] = {};
  char valueText[8] = {};
  if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "value", valueText, sizeof(valueText)) !=
          ESP_OK) {
    httpd_resp_set_status(request, "400 Bad Request");
    return sendJson(request, "{\"message\":\"Volume value is required\"}");
  }

  char* end = nullptr;
  const long value = strtol(valueText, &end, 10);
  if (end == valueText || *end != '\0' || value < 0 || value > 100) {
    httpd_resp_set_status(request, "400 Bad Request");
    return sendJson(request,
                    "{\"message\":\"Volume must be between 0 and 100\"}");
  }

  Serial.printf("[%10lu] Web: POST /api/volume value=%ld\n",
                static_cast<unsigned long>(millis()), value);
  if (!audioEngine.setSpeakerVolume(static_cast<uint8_t>(value))) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    return sendJson(
        request,
        "{\"message\":\"Could not update the ReSpeaker volume\"}");
  }

  char json[96];
  snprintf(json, sizeof(json),
           "{\"message\":\"Speaker volume set to %ld%%\"}", value);
  return sendJson(request, json);
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

esp_err_t photoHandler(httpd_req_t* request) {
  if (!camera_service::ready()) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_sendstr(request, camera_service::lastError());
  }

  camera_fb_t* frame = esp_camera_fb_get();
  if (frame == nullptr) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_sendstr(request, "Photo capture failed");
  }

  if (frame->format != PIXFORMAT_JPEG || frame->len == 0) {
    esp_camera_fb_return(frame);
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_sendstr(request, "Camera returned an invalid photo");
  }

  httpd_resp_set_type(request, "image/jpeg");
  setNoCache(request);
  const esp_err_t result = httpd_resp_send(
      request, reinterpret_cast<const char*>(frame->buf), frame->len);
  esp_camera_fb_return(frame);
  return result;
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
  controlConfig.max_uri_handlers = 18;
  controlConfig.stack_size = 8192;
  controlConfig.lru_purge_enable = true;

  if (httpd_start(&controlServer, &controlConfig) != ESP_OK) {
    Serial.println("Web: could not start the control server");
    return false;
  }

  const bool controlRoutesReady =
      registerUri(controlServer, "/", HTTP_GET, rootHandler) &&
      registerUri(controlServer, "/api/status", HTTP_GET, statusHandler) &&
      registerUri(controlServer, "/api/vision", HTTP_POST, visionHandler) &&
      registerUri(controlServer, "/api/follow", HTTP_POST, followHandler) &&
      registerUri(controlServer, "/api/follow/heartbeat", HTTP_POST, followHeartbeatHandler) &&
      registerUri(controlServer, "/api/camera/photo", HTTP_GET, photoHandler) &&
      registerUri(controlServer, "/api/record/start", HTTP_POST,
                  startRecordingHandler) &&
      registerUri(controlServer, "/api/record/stop", HTTP_POST,
                  stopRecordingHandler) &&
      registerUri(controlServer, "/api/play", HTTP_POST, playHandler) &&
      registerUri(controlServer, "/api/tone", HTTP_POST, toneHandler) &&
      registerUri(controlServer, "/api/play/stop", HTTP_POST,
                  stopPlaybackHandler) &&
      registerUri(controlServer, "/api/volume", HTTP_POST, volumeHandler) &&
      registerUri(controlServer, "/api/zumo/status", HTTP_GET,
                  zumoStatusHandler) &&
      registerUri(controlServer, "/api/zumo/drive", HTTP_POST,
                  zumoDriveHandler) &&
      registerUri(controlServer, "/api/zumo/stop", HTTP_POST,
                  zumoStopHandler) &&
      registerUri(controlServer, "/api/zumo/calibrate", HTTP_POST,
                  zumoCalibrateHandler) &&
      registerUri(controlServer, "/api/zumo/face", HTTP_POST,
                  zumoFaceHandler);
  if (!controlRoutesReady) {
    Serial.println("Web: could not register all control routes");
    return false;
  }

  httpd_config_t streamConfig = HTTPD_DEFAULT_CONFIG();
  streamConfig.server_port = app_config::kStreamServerPort;
  streamConfig.stack_size = 8192;
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
