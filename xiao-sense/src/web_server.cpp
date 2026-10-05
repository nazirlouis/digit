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
#include "object_model_config.h"
#include "zumo_link.h"
#include "wake_word.h"
#include "voice_direction.h"
#include "robot_actions.h"
#include "speech_commands.h"
#include "photo_store.h"

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
    main{width:min(1280px,100%);margin:auto;padding:22px}.top{display:flex;justify-content:space-between;gap:15px;align-items:center;margin-bottom:16px}
    h1{font-size:clamp(24px,5vw,40px);margin:0;letter-spacing:.03em}.pill{border:1px solid #35515d;border-radius:999px;padding:8px 13px;color:var(--muted)}
    .panel{background:rgba(16,29,35,.9);border:1px solid #28434e;border-radius:20px;overflow:hidden;box-shadow:0 22px 70px #0008}
    .drive-view{display:grid;grid-template-columns:minmax(0,1fr) 400px;align-items:start}.drive-view .zumo{border-top:0;border-left:1px solid #28434e}.drive-view .video{min-width:0}
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
    .drive-pad{display:grid;grid-template-columns:repeat(3,minmax(0,110px));gap:8px;justify-content:center;touch-action:none;user-select:none}
    .drive-pad button{min-height:64px;touch-action:none}.drive-pad .forward{grid-column:2}.drive-pad .left{grid-column:1}.drive-pad .stop{grid-column:2}.drive-pad .right{grid-column:3}.drive-pad .back{grid-column:2}
    .zumo-actions{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px}.zumo-actions button{flex:1 1 120px}
    .speed{display:flex;align-items:center;gap:12px;margin:14px 0}.speed input{flex:1;min-width:0;accent-color:var(--cyan)}.speed b{min-width:48px;text-align:right}.acceleration-hint{font-size:13px}
    .led-controls{display:flex;flex-wrap:wrap;align-items:center;gap:10px;margin-top:16px;padding:14px;background:#0a151a;border-radius:14px}.led-controls label{display:flex;align-items:center;gap:8px}.led-controls select,.led-controls input[type=color]{background:#233740;color:#fff;border:1px solid #35515d;border-radius:9px;padding:5px}.led-controls input[type=range]{width:110px;accent-color:var(--cyan)}.led-controls button{padding:10px 16px}
    #message{min-height:24px;padding:0 18px 18px;color:var(--muted)}#message.error{color:var(--red)}
    @media(max-width:900px){.drive-view{grid-template-columns:1fr}.drive-view .zumo{border-left:0;border-top:1px solid #28434e}}
    @media(max-width:650px){.controls{grid-template-columns:1fr 1fr}.details{grid-template-columns:1fr}.top{align-items:flex-start;flex-direction:column}}
  </style>
</head>
<body>
<main>
  <div class="top"><div><h1>Digit Sense</h1><div style="color:var(--muted)">Live camera + ReSpeaker recorder</div></div><div class="pill" id="state">CONNECTING</div></div>
  <section class="panel">
    <div class="drive-view">
    <div class="video"><img id="stream" alt="Digit camera stream"><canvas id="boxes" width="640" height="480" aria-hidden="true"></canvas><strong class="person-badge" id="personBadge" hidden>Naz Detected</strong><span id="camera">Camera starting...</span></div>
    <aside class="zumo" id="zumoDrive" aria-label="Zumo drive controls">
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
      <label class="speed" for="driveAcceleration">Acceleration <input id="driveAcceleration" type="range" min="500" max="6000" step="100" value="6000" aria-describedby="accelerationHint"><b id="driveAccelerationValue">6000</b></label>
      <p class="acceleration-hint" id="accelerationHint">Speed units per second. Lower is gentler; higher reaches drive speed sooner. Normal stops use a fixed deceleration rate.</p>
      <div class="zumo-actions"><button id="zumoCalibrate">Calibrate floor</button></div>
      <div id="zumoMessage" role="status">Connecting to Zumo...</div>
    </aside>
    </div>
    <div class="controls">
      <button class="primary" id="record" onclick="action('/api/record/start')">Record</button>
      <button class="danger" id="stopRecord" onclick="action('/api/record/stop')">Stop &amp; play</button>
      <button id="replay" onclick="action('/api/play')">Replay</button>
      <button id="tone" onclick="action('/api/tone')">Test speaker</button>
      <button id="stopPlay" onclick="action('/api/play/stop')">Stop playback</button>
      <button id="takePhoto" onclick="takePhoto()" disabled>Take picture</button>
      <button id="visionToggle" onclick="toggleVision()" disabled>Start person detection</button>
      <button id="batteryToggle" onclick="toggleBattery()" disabled>Test object model</button>
      <button id="followToggle" onclick="toggleFollow()" disabled>Turn with person</button>
    </div>
    <div class="volume">
      <p id="batteryState" role="status">Object model: checking...</p>
      <label for="volume"><span>Speaker volume</span><b><span id="volumeValue">70</span>%</b></label>
      <input id="volume" type="range" min="0" max="100" step="1" value="70" oninput="queueVolume(this.value)" aria-label="Speaker volume">
    </div>
    <div class="details">
      <div class="metric"><small>Recording</small><b id="duration">0.0 s</b></div>
      <div class="metric"><small>PSRAM free</small><b id="memory">--</b></div>
      <div class="metric"><small>Network address</small><b id="network">--</b></div>
    </div>
    <div class="zumo">
      <h2>Digit wake word</h2>
      <p>Say “Digit”, wait for the chime, then give a command within six seconds. Person detection briefly yields while listening for a command.</p>
      <p><strong id="wakeState">Starting...</strong> <span id="wakeLast"></span></p>
      <p id="wakeResult"></p>
      <button id="wakeTest" onclick="action('/api/wake/test')" disabled>Test wake reaction</button>
      <h3>Turn toward my voice</h3>
      <p>Fasten the circular microphone array to Digit so they turn together. With Digit still, speak from directly in front and press Set front. Then speak from Digit's left side and press Set left. Say “hello” during setup.</p>
      <p><strong id="voiceState">Checking direction...</strong> <span id="voiceAngle"></span></p>
      <div class="zumo-actions">
        <button id="voiceFront" onclick="voiceAction('front')" disabled>1. Set front</button>
        <button id="voiceLeft" onclick="voiceAction('left')" disabled>2. Set left</button>
        <button id="voiceToggle" onclick="voiceAction(voiceEnabled?'off':'on')" disabled>Enable voice turning</button>
        <button onclick="voiceAction('reset')">Reset direction setup</button>
      </div>
      <p id="voiceMessage">Turning starts off after every XIAO restart. Calibrate the floor before enabling it. Setup does not move the tracks.</p>
      <p id="voiceResult"></p>
      <p>Once enabled, “Digit” requests one turn, then the lights, face and chime. If the direction is unclear, Digit only reacts. The reaction test above never turns the tracks.</p>
    </div>
    <div id="message">Waiting for device status...</div>
    <div class="zumo">
      <h2>Digit actions</h2>
      <p id="commandState">Checking voice commands...</p>
      <p><strong id="routineState">Idle</strong></p>
      <div class="zumo-actions">
        <button data-routine="1" onclick="startRoutine(1)">Say hello</button>
        <button class="danger" onclick="stopZumo()">STOP ALL</button>
      </div>
      <p>Say hello requires floor calibration. STOP cancels movement, speech, and pending commands.</p>
      <p id="routineMessage" role="status"></p>
    </div>
    <div class="zumo" id="followPanel">Person turning: <strong id="followState">Off</strong><p>Tracks rotate in place toward the largest person box. Stops when centered or the person is lost.</p></div>
    <div class="zumo">
      <h2>Digit face</h2><p>Choose an expression while stopped. Sleeping stays on until you choose another face.</p>
      <div class="zumo-actions" id="faceButtons">
        <button data-face="NEUTRAL" disabled>Neutral / wake up</button>
        <button data-face="HAPPY" disabled>Happy</button><button data-face="SAD" disabled>Sad</button>
        <button data-face="MAD" disabled>Mad</button><button data-face="SERIOUS" disabled>Serious</button>
        <button data-face="CURIOUS" disabled>Curious</button><button data-face="SURPRISED" disabled>Surprised</button>
        <button data-face="SLEEPING" disabled>Sleeping</button><button data-face="CONFUSED" disabled>Confused</button>
        <button data-face="SLEEPY" disabled>Sleepy</button><button data-face="EXCITED" disabled>Excited</button>
        <button data-face="CRYING" disabled>Crying</button><button data-face="LOVE" disabled>Love / heart eyes</button>
      </div>
      <div id="faceMessage" role="status">Expressions are available when Digit is connected and stopped.</div>
      <div class="led-controls">
        <strong>RGB LEDs</strong>
        <label for="ledTarget">LED <select id="ledTarget"><option value="ALL">All six</option><option value="0">1</option><option value="1">2</option><option value="2">3</option><option value="3">4</option><option value="4">5</option><option value="5">6</option></select></label>
        <label for="ledColor">Color <input id="ledColor" type="color" value="#36e4ff"></label>
        <label for="ledBrightness">Brightness <input id="ledBrightness" type="range" min="1" max="31" value="5"><span id="ledBrightnessValue">5</span></label>
        <button id="ledApply">Apply</button><button id="ledOff">Off</button><span id="ledResult">--</span>
      </div>
    </div>
  </section>
</main>
<script>
const $=id=>document.getElementById(id);
$('stream').src=`http://${location.hostname}:81/stream`;
let heldDirection=null,driveTimer=null,driveInFlight=false,zumoReady=false,cameraReady=false,photoPending=false,visionEnabled=false,visionPending=false,followEnabled=false,followPending=false,followHeartbeatPending=false;
let voiceEnabled=false,voicePending=false,batteryEnabled=false,batteryPending=false,batteryNotice='';
let controlGeneration=0,routinePending=false,stopPending=false,hardStopPending=false,stopRequests=0,actionActive=false;
const routineButtons=[...document.querySelectorAll('[data-routine]')];
async function refreshActions(){try{const response=await fetch('/api/actions',{cache:'no-store'});if(!response.ok)throw Error('Action status unavailable');const s=await response.json();controlGeneration=Math.max(controlGeneration,s.generation);actionActive=s.active;$('routineState').textContent=s.action+' · '+s.phase;$('commandState').textContent=s.listening?'Listening for your command…':s.voice_ready?(s.voice_result==='No command recognized'?'No command heard. Say Digit and try again.':(s.voice_result==='Audio queue overflow'||s.voice_result==='Audio interrupted')?'Listening interrupted. Say Digit and try again.':s.voice_result==='Recognized'?'Heard: '+(({1:'Say hello',7:'Stop'}[s.last_command])||'command'):'Say Digit, then a command'):s.voice_error||'Voice commands unavailable';for(const b of routineButtons)b.disabled=!s.drive_ready||routinePending||stopPending;}catch(error){$('commandState').textContent=error.message;for(const b of routineButtons)b.disabled=true}}
async function startRoutine(id){if(id!==1||routinePending||stopPending||!controlGeneration)return;routinePending=true;try{const response=await fetch('/api/actions?command='+id+'&generation='+controlGeneration,{method:'POST'});const s=await response.json();if(!response.ok)throw Error(s.message);controlGeneration=Math.max(controlGeneration,s.generation);$('routineMessage').textContent=s.message}catch(error){$('routineMessage').textContent=error.message}finally{routinePending=false;await refreshActions()}}
async function voiceAction(value){if(voicePending)return;voicePending=true;try{const response=await fetch('/api/voice/setup?action='+value,{method:'POST'});const data=await response.json();if(!response.ok)throw Error(data.message);$('voiceMessage').textContent=data.message}catch(error){$('voiceMessage').textContent=error.message}finally{voicePending=false;await refreshVoice()}}
async function refreshVoice(){try{const response=await fetch('/api/voice/status',{cache:'no-store'});if(!response.ok)throw Error('Unavailable');const s=await response.json();voiceEnabled=s.enabled;$('voiceState').textContent=s.turning?'Turning toward saved voice direction':s.paused?'Paused for vision':s.error||(!s.available?'Direction unavailable':!s.front_set?'Set the front direction':!s.calibrated?'Now set Digit’s left side':s.enabled?'Voice turning enabled':'Direction ready · turning off');$('voiceAngle').textContent=s.stable?` · ${Math.round(s.degrees)}° at array`+(s.calibrated?` · ${Math.round(Math.abs(s.relative))}° ${s.relative>=0?'left':'right'}`:''):' · waiting for clear speech';const setup=s.stable&&!s.paused&&!s.turning&&!voicePending;$('voiceFront').disabled=!setup;$('voiceLeft').disabled=!setup||!s.front_set;$('voiceToggle').textContent=s.enabled?'Disable voice turning':'Enable voice turning';$('voiceToggle').disabled=voicePending||(!s.enabled&&(!s.calibrated||!s.available||!s.drive_ready||!s.gyro_ready||s.paused));$('voiceResult').textContent=s.result==='NONE'?'':s.result==='DONE'?'Last turn completed':`Last turn: ${s.result}`;if(!s.gyro_ready)$('voiceResult').textContent+=' · Zumo turn support unavailable; check updated files and gyro'}catch(error){$('voiceState').textContent='Direction unavailable';$('voiceFront').disabled=$('voiceLeft').disabled=$('voiceToggle').disabled=true}}
const driveButtons=[...document.querySelectorAll('[data-drive]')];
const faceButtons=[...document.querySelectorAll('[data-face]')];
function speeds(direction){const value=Number($('driveSpeed').value);return {forward:[value,value],back:[-value,-value],left:[-value,value],right:[value,-value]}[direction]||[0,0]}
async function postZumo(path){const response=await fetch(path,{method:'POST',cache:'no-store'});const data=await response.json();if(!response.ok)throw Error(data.message||'Zumo command failed');return data}
async function stopZumo(hard=false){hard=hard===true;heldDirection=null;if(driveTimer!==null){clearInterval(driveTimer);driveTimer=null}if((stopPending&&!hard)||hardStopPending)return;stopPending=true;if(hard)hardStopPending=true;++stopRequests;try{const s=await postZumo('/api/zumo/stop'+(hard?'?hard=1':''));controlGeneration=Math.max(controlGeneration,s.generation);$('routineMessage').textContent=s.message}catch(error){$('zumoMessage').textContent=error.message}finally{--stopRequests;stopPending=stopRequests>0;if(hard)hardStopPending=false;await refreshActions()}}
async function sendDrive(){if(!heldDirection||driveInFlight||stopPending)return;driveInFlight=true;const [left,right]=speeds(heldDirection);try{const s=await postZumo(`/api/zumo/drive?left=${left}&right=${right}&acceleration=${Number($('driveAcceleration').value)}&generation=${controlGeneration}`);if(s.generation)controlGeneration=Math.max(controlGeneration,s.generation)}catch(error){$('zumoMessage').textContent=error.message;await stopZumo(true)}finally{driveInFlight=false}}
function beginDrive(direction,event){if(!zumoReady||heldDirection||stopPending||!controlGeneration)return;event.preventDefault();event.currentTarget.setPointerCapture(event.pointerId);heldDirection=direction;sendDrive();driveTimer=setInterval(sendDrive,100)}
for(const button of driveButtons){button.addEventListener('pointerdown',event=>beginDrive(button.dataset.drive,event));button.addEventListener('pointerup',stopZumo);button.addEventListener('pointercancel',()=>stopZumo(true));button.addEventListener('lostpointercapture',()=>{if(heldDirection)stopZumo(true)})}
$('zumoStop').addEventListener('click',stopZumo);
async function takePhoto(){if(photoPending)return;photoPending=true;const button=$('takePhoto');button.disabled=true;try{const response=await fetch('/api/camera/photo',{cache:'no-store'});if(!response.ok)throw Error((await response.text())||'Photo capture failed');const blob=await response.blob();if(blob.type!=='image/jpeg'||blob.size===0)throw Error('Camera returned an invalid photo');const url=URL.createObjectURL(blob);const link=document.createElement('a');link.href=url;link.download='digit-'+new Date().toISOString().replace(/[:.]/g,'-')+'.jpg';document.body.append(link);link.click();link.remove();setTimeout(()=>URL.revokeObjectURL(url),10000);$('message').textContent='Picture saved';$('message').className=''}catch(error){$('message').textContent=error.message;$('message').className='error'}finally{photoPending=false;button.disabled=!cameraReady}}
async function toggleVision(){if(visionPending)return;visionPending=true;$('visionToggle').disabled=true;try{const response=await fetch('/api/vision?enabled='+(visionEnabled?'0':'1'),{method:'POST',cache:'no-store'});const data=await response.json();if(!response.ok)throw Error(data.message||'Could not change person detection');$('message').textContent=data.message;$('message').className=''}catch(error){$('message').textContent=error.message;$('message').className='error'}finally{visionPending=false;await refresh()}}
async function toggleBattery(){if(batteryPending||stopPending||!Number.isFinite(controlGeneration)||!controlGeneration)return;batteryNotice='';batteryPending=true;$('batteryToggle').disabled=true;try{const response=await fetch('/api/vision/objects?enabled='+(batteryEnabled?'0':'1')+'&generation='+controlGeneration,{method:'POST',cache:'no-store'});const data=await response.json();if(Number.isFinite(data.generation))controlGeneration=Math.max(controlGeneration,data.generation);if(!response.ok)throw Error(data.message||'Could not change object detection');$('message').textContent=data.message;$('message').className=''}catch(error){batteryNotice=error.message;$('message').textContent=error.message;$('message').className='error'}finally{batteryPending=false;await refresh()}}
async function toggleFollow(){if(followPending)return;followPending=true;$('followToggle').disabled=true;try{const response=await fetch('/api/follow?enabled='+(followEnabled?'0':'1'),{method:'POST',cache:'no-store'});const data=await response.json();if(!response.ok)throw Error(data.message||'Could not change person turning');$('message').textContent=data.message;$('message').className=''}catch(error){$('message').textContent=error.message;$('message').className='error'}finally{followPending=false;await refresh()}}
async function followHeartbeat(){if(!followEnabled||followHeartbeatPending||document.hidden)return;followHeartbeatPending=true;try{const response=await fetch('/api/follow/heartbeat',{method:'POST',cache:'no-store'});if(!response.ok)followEnabled=false}catch(error){followEnabled=false}finally{followHeartbeatPending=false}}
setInterval(followHeartbeat,500);
$('driveSpeed').addEventListener('input',event=>{$('driveSpeedValue').textContent=event.target.value});
$('driveAcceleration').addEventListener('input',event=>{$('driveAccelerationValue').textContent=event.target.value});
$('zumoCalibrate').addEventListener('click',async()=>{try{await postZumo('/api/zumo/calibrate');$('zumoMessage').textContent='Calibrating floor...'}catch(error){$('zumoMessage').textContent=error.message}});
for(const button of faceButtons){button.addEventListener('click',async()=>{try{await postZumo('/api/zumo/face?value='+encodeURIComponent(button.dataset.face));$('faceMessage').textContent=button.textContent+' requested'}catch(error){$('faceMessage').textContent=error.message}})}
function ledPath(off){const hex=off?'000000':$('ledColor').value.slice(1);const target=$('ledTarget').value;const brightness=off?0:$('ledBrightness').value;return `/api/zumo/led?target=${target}&r=${parseInt(hex.slice(0,2),16)}&g=${parseInt(hex.slice(2,4),16)}&b=${parseInt(hex.slice(4,6),16)}&brightness=${brightness}`}
$('ledBrightness').addEventListener('input',event=>{$('ledBrightnessValue').textContent=event.target.value});
for(const [id,off] of [['ledApply',false],['ledOff',true]]){$(id).addEventListener('click',async()=>{try{await postZumo(ledPath(off));$('ledResult').textContent='Sending...'}catch(error){$('ledResult').textContent=error.message}})}
window.addEventListener('blur',()=>stopZumo(true));document.addEventListener('visibilitychange',()=>{if(document.hidden)stopZumo(true)});
async function refreshZumo(){try{const response=await fetch('/api/zumo/status',{cache:'no-store'});if(!response.ok)throw Error('Status unavailable');const s=await response.json();zumoReady=s.connected&&s.ready&&!s.calibrating;$('zumoLink').textContent=s.connected?'Zumo: connected':'Zumo: disconnected';$('zumoReady').textContent=s.calibrating?'Drive: calibrating':zumoReady?'Drive: ready':'Drive: locked';$('zumoBattery').textContent=s.battery_mv>=0?`Battery: ${(s.battery_mv/1000).toFixed(2)} V`:'Battery: --';$('zumoFault').textContent='Fault: '+s.fault;$('ledApply').disabled=$('ledOff').disabled=!s.connected;$('ledResult').textContent=s.connected?(s.led_result==='APPLIED'?'Zumo acknowledged':s.led_result==='SENDING'?'Sending...':s.led_result==='NONE'?'--':s.led_result):'Disconnected';for(const button of driveButtons)button.disabled=!zumoReady||followEnabled;$('zumoCalibrate').disabled=!s.connected||s.calibrating||!!heldDirection||followEnabled;for(const button of faceButtons)button.disabled=!s.connected||s.calibrating||!!s.left||!!s.right||!!heldDirection||followEnabled;if(!zumoReady&&heldDirection)stopZumo(true);let message='Waiting for Zumo connection...';if(s.connected){if(s.calibrating)message='Calibrating floor...';else if(s.fault!=='NONE')message='Zumo fault: '+s.fault;else if(s.last_error!=='NONE')message=s.last_error;else if(zumoReady)message='Hold a direction to drive.';else message='Connected. Calibrate floor on a safe surface to enable driving.'}$('zumoMessage').textContent=message}catch(error){zumoReady=false;for(const button of [...driveButtons,...faceButtons])button.disabled=true;$('ledApply').disabled=$('ledOff').disabled=true;$('ledResult').textContent='Unavailable';$('zumoLink').textContent='Zumo: unavailable';$('zumoMessage').textContent='Zumo status unavailable';if(heldDirection)stopZumo(true)}}
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
function drawBoxes(s){const ctx=$('boxes').getContext('2d');ctx.clearRect(0,0,640,480);if(!s.vision_enabled||!s.boxes)return;const sx=640/(s.vision_width||320),sy=480/(s.vision_height||240);ctx.lineWidth=4;ctx.font='bold 18px system-ui';for(const b of s.boxes){const label=s.battery_mode?(b.label||s.object_classes?.[b.class_id]||'Object'):'Person';ctx.strokeStyle=s.battery_mode?(['#ffcf45','#38e4ff','#ff74d4'][b.class_id]||'#ffcf45'):'#36e4ff';ctx.fillStyle=ctx.strokeStyle;const x=b.x1*sx,y=b.y1*sy,w=(b.x2-b.x1)*sx,h=(b.y2-b.y1)*sy;ctx.strokeRect(x,y,w,h);ctx.fillText(`${label} ${b.score}%`,x+4,Math.max(22,y-7))}}
let refreshPending=false;
async function refresh(){
  if(refreshPending)return;
  refreshPending=true;
  try{
    const response=await fetch('/api/status',{cache:'no-store'});if(!response.ok)throw new Error('Status HTTP '+response.status);const s=await response.json();
    cameraReady=s.camera_ready;visionEnabled=s.vision_enabled&&!s.battery_mode;batteryEnabled=s.vision_enabled&&!!s.battery_mode;followEnabled=s.follow_enabled;$('takePhoto').disabled=!cameraReady||photoPending;$('visionToggle').disabled=!s.vision_ready||visionPending||batteryPending;$('visionToggle').textContent=visionEnabled?'Stop person detection':'Start person detection';$('followToggle').disabled=!followEnabled&&(!visionEnabled||!zumoReady)||followPending;$('followToggle').textContent=followEnabled?'Stop turning':'Turn with person';$('followState').textContent=s.follow_state+(s.follow_target_x>=0?` · target ${s.follow_target_x}/${s.vision_width}`:'');$('state').textContent=s.audio_state;const rate=s.vision_interval_ms>0&&s.vision_age_ms<2000?` · ${(1000/s.vision_interval_ms).toFixed(1)} fps`:' · measuring';$('camera').textContent=cameraReady?((s.vision_ready||s.battery_ready)?(s.vision_enabled?'LIVE · 640×480 · '+(s.battery_mode?'Object':'Person')+' detection'+rate:'LIVE · 640×480 · Vision off'):'LIVE · 640×480 · Vision unavailable'):s.camera_error;$('personBadge').hidden=!visionEnabled||!s.person_detected;drawBoxes(s);
    $('batteryToggle').disabled=!s.battery_ready||batteryPending||visionPending||stopPending;$('batteryToggle').textContent=batteryEnabled?'Stop object test':'Test object model';const objectSummary=(s.boxes||[]).map(b=>(b.label||s.object_classes?.[b.class_id]||'Object')+' '+b.score+'%').join(' · ');$('batteryState').textContent=batteryNotice||(!s.battery_ready?'Object model unavailable':!batteryEnabled?'Battery, Marker, Screwdriver · camera test only':s.vision_error||((s.vision_age_ms<2000?(objectSummary||'No objects detected'):'Waiting for object result')+' · '+s.vision_inference_ms+' ms'));
    $('wakeState').textContent=!s.wake_ready?'Wake unavailable':s.wake_paused?'Paused for vision':s.wake_reacting?'Listening · wake reaction':'Listening for Digit';
    $('wakeLast').textContent=s.wake_detections?` · ${s.wake_detections} heard · last ${Math.floor(s.wake_age_ms/1000)}s ago (${Math.round(s.wake_score*100)}%)`:'';
    $('wakeResult').textContent=s.wake_error||({'APPLIED':'Zumo acknowledged the reaction','SENDING':'Sending reaction to Zumo...','NONE':'','DISCONNECTED':'Zumo disconnected · speaker reaction remains available','BUSY':'Zumo busy · face and lights skipped','CANCELLED':'Zumo reaction cancelled','NO_REPLY':'No wake acknowledgement from Zumo · check its updated files','UNKNOWN_COMMAND':'Update the Zumo controller for wake reactions'}[s.wake_zumo]||s.wake_zumo);
    $('wakeTest').disabled=s.wake_paused||s.wake_reacting||(!s.audio_available&&!s.wake_zumo_connected);
    const elapsed=s.audio_state==='RECORDING'?s.recording_elapsed_ms:s.captured_ms;
    $('duration').textContent=(Math.min(elapsed,s.max_recording_ms)/1000).toFixed(1)+' s';
    $('memory').textContent=formatBytes(s.psram_free);$('network').textContent=s.wifi_ip;
    if(volumeTimer===null&&!volumeSending){$('volume').value=s.speaker_volume;$('volumeValue').textContent=s.speaker_volume;}
    $('volume').disabled=!s.audio_available;
    $('record').disabled=s.audio_state!=='READY';$('stopRecord').disabled=s.audio_state!=='RECORDING';
    $('replay').disabled=s.audio_state!=='READY'||s.captured_ms===0;$('tone').disabled=s.audio_state!=='READY';$('stopPlay').disabled=s.audio_state!=='PLAYING'&&!s.wake_chime;
    if(s.audio_error){$('message').textContent=s.audio_error;$('message').className='error';}
    else if($('message').className==='error'||$('message').textContent==='Waiting for device status...'){$('message').textContent='Ready';$('message').className='';}
  }catch(error){$('wakeState').textContent='Unavailable';$('wakeTest').disabled=true;cameraReady=false;$('takePhoto').disabled=true;$('visionToggle').disabled=true;$('batteryToggle').disabled=true;$('batteryState').textContent='Object test unavailable · device offline';$('personBadge').hidden=true;$('state').textContent='OFFLINE';$('message').textContent='Device status unavailable';$('message').className='error';}
  finally{refreshPending=false;}
}
refresh();setInterval(refresh,250);
refreshVoice();setInterval(refreshVoice,750);
refreshActions();setInterval(refreshActions,500);
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
  const auto wake = wake_word::snapshot();
  const auto zumo = zumo_link::snapshot();
  char wakeError[192], wakeZumo[48];
  escapeJsonString(wake.error, wakeError, sizeof(wakeError));
  escapeJsonString(zumo.wakeResult, wakeZumo, sizeof(wakeZumo));
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

  char json[2400];
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
                         ",\"battery_ready\":%s,\"battery_mode\":%s,\"battery_detected\":%s,"
                         "\"battery_score_percent\":%u,\"object_classes\":[\"Battery\",\"Marker\",\"Screwdriver\"],\"vision_width\":%u,\"vision_height\":%u,\"boxes\":[",
                         vision.batteryReady ? "true" : "false", vision.batteryMode ? "true" : "false",
                         vision.batteryDetected ? "true" : "false", vision.batteryScorePercent,
                         vision.imageWidth, vision.imageHeight);
  used += written > 0 ? static_cast<size_t>(written) : 0;
  for (uint8_t index = 0; index < vision.boxCount && used < sizeof(json) - 100; ++index) {
    const auto& box = vision.boxes[index];
    written = snprintf(json + used, sizeof(json) - used,
                       "%s{\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d,\"score\":%u,\"class_id\":%u,\"label\":\"%s\"}",
                       index ? "," : "", box.x1, box.y1, box.x2, box.y2, box.scorePercent,
                       box.classId, vision.batteryMode ? object_model::className(box.classId) : "Person");
    used += written > 0 ? static_cast<size_t>(written) : 0;
  }
  snprintf(json + used, sizeof(json) - used,
           "],\"follow_enabled\":%s,\"follow_state\":\"%s\","
           "\"follow_target_x\":%d,\"follow_error_px\":%d,"
           "\"follow_score_percent\":%u,"
           "\"wake_ready\":%s,\"wake_paused\":%s,\"wake_reacting\":%s,"
           "\"wake_detections\":%lu,\"wake_age_ms\":%lu,\"wake_score\":%.3f,"
           "\"wake_dropped_frames\":%lu,\"wake_inference_us\":%lu,\"wake_mic_peak\":%u,"
           "\"wake_error\":\"%s\",\"wake_zumo\":\"%s\",\"wake_chime\":%s,"
           "\"wake_zumo_connected\":%s}",
           follow.enabled ? "true" : "false", follow.phase,
           follow.targetX, follow.errorPixels, follow.scorePercent,
           wake.ready ? "true" : "false", wake.pausedForVision ? "true" : "false",
           wake.reacting ? "true" : "false", (unsigned long)wake.detections,
           (unsigned long)(wake.detections ? millis() - wake.lastDetectionMs : 0), wake.lastScore,
           (unsigned long)wake.droppedFrames, (unsigned long)wake.inferenceUs, wake.micPeak,
           wakeError, wakeZumo, audio.wakeChimePlaying ? "true" : "false",
           zumo.connected ? "true" : "false");
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

bool currentGeneration(httpd_req_t* request) {
  char value[16];
  if (!queryValue(request, "generation", value, sizeof(value)) || !value[0]) return false;
  uint64_t parsed = 0;
  for (const char* p = value; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    parsed = parsed * 10 + *p - '0';
    if (parsed > UINT32_MAX) return false;
  }
  return parsed == robot_actions::generation();
}
esp_err_t controlReply(httpd_req_t* request, const char* message) {
  char escaped[240], json[320];
  escapeJsonString(message, escaped, sizeof(escaped));
  snprintf(json, sizeof(json), "{\"message\":\"%s\",\"generation\":%lu}", escaped,
           (unsigned long)robot_actions::generation());
  return sendJson(request,json);
}
esp_err_t actionsStatusHandler(httpd_req_t* request) {
  const auto action = robot_actions::snapshot();
  const auto speech = speech_commands::snapshot();
  const auto z = zumo_link::snapshot();
  char error[192], phase[192], result[64], decoder[768], json[2200];
  escapeJsonString(speech.error,error,sizeof(error));
  escapeJsonString(action.phase,phase,sizeof(phase));
  escapeJsonString(speech.result,result,sizeof(result));
  escapeJsonString(speech.decoderText,decoder,sizeof(decoder));
  snprintf(json,sizeof(json),
      "{\"action\":\"%s\",\"phase\":\"%s\",\"active\":%s,\"generation\":%lu,"
      "\"voice_ready\":%s,\"listening\":%s,\"voice_error\":\"%s\",\"recognized\":%lu,"
      "\"last_command\":%d,\"score\":%.3f,\"drive_ready\":%s,\"range_valid\":%s,"
      "\"distance_mm\":%d,\"photo_revision\":%lu,\"voice_result\":\"%s\","
      "\"command_audio_ms\":%lu,\"command_chunks\":%lu,\"command_mic_peak\":%u,"
      "\"command_inference_us\":%lu,\"candidate_command\":%d,\"candidate_score\":%.3f,"
      "\"command_feed_chunks\":%lu,\"command_fetch_chunks\":%lu,\"command_model_peak\":%u,"
      "\"audio_total_feeds\":%lu,\"audio_total_fetches\":%lu,"
      "\"command_dropped_frames\":%lu,\"command_decoder_text\":\"%s\"}",
      robot_actions::name(action.command),phase,action.active?"true":"false",(unsigned long)action.generation,
      speech.ready?"true":"false",speech.listening?"true":"false",error,(unsigned long)speech.recognized,
      speech.lastCommand,speech.score,z.connected&&z.ready&&!z.calibrating&&z.actionCapable?"true":"false",
      z.rangeValid?"true":"false",z.distanceMm,(unsigned long)photo_store::revision(),result,
      (unsigned long)(speech.samples/16),(unsigned long)speech.chunks,speech.micPeak,
      (unsigned long)speech.inferenceUs,speech.candidateCommand,speech.candidateScore,
      (unsigned long)speech.feedChunks,(unsigned long)speech.fetchChunks,speech.modelPeak,
      (unsigned long)speech.totalFeeds,
      (unsigned long)speech.totalFetches,(unsigned long)speech.droppedFrames,decoder);
  return sendJson(request,json);
}
esp_err_t audioProcessingHandler(httpd_req_t* request) {
  const auto& p = audioEngine.processing();
  char json[800];
  snprintf(json, sizeof(json),
      "{\"left_category\":%d,\"left_source\":%d,\"right_category\":%d,\"right_source\":%d,"
      "\"packed_left\":%d,\"packed_right\":%d,\"asr_output\":%d,\"asr_gain\":%.4f,\"aec_bypass\":%d,"
      "\"post_agc\":%d,\"post_agc_gain\":%.4f,\"post_limiter\":%d,\"post_noise_floor\":%.4f,"
      "\"post_speech_noise_floor\":%.4f,\"post_echo\":%d,\"post_nonlinear_echo\":%d}",
      p.leftCategory,p.leftSource,p.rightCategory,p.rightSource,p.packedLeft,p.packedRight,p.asrOutput,p.asrGain,
      p.aecBypass,p.agc,p.agcGain,p.limiter,p.stationaryNoiseFloor,p.speechNoiseFloor,p.echoSuppression,p.nonlinearEcho);
  return sendJson(request,json);
}
esp_err_t commandAudioHandler(httpd_req_t* request) {
  const auto capture = speech_commands::copyAudio();
  if (!capture.samples) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"No complete command recording available yet\"}");
  }
  // ESP32 is little-endian: lossless 16 kHz signed PCM from detect(), no gain.
  uint8_t header[44]{};
  memcpy(header, "RIFF", 4); memcpy(header + 8, "WAVEfmt ", 8); memcpy(header + 36, "data", 4);
  const uint32_t bytes = capture.count * sizeof(int16_t);
  const auto put32 = [&](size_t offset, uint32_t value) { memcpy(header + offset, &value, 4); };
  const auto put16 = [&](size_t offset, uint16_t value) { memcpy(header + offset, &value, 2); };
  put32(4, 36 + bytes); put32(16, 16); put16(20, 1); put16(22, 1);
  put32(24, 16000); put32(28, 32000); put16(32, 2); put16(34, 16); put32(40, bytes);
  char session[16]; snprintf(session, sizeof(session), "%lu", (unsigned long)capture.session);
  httpd_resp_set_type(request, "audio/wav");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "X-Command-Session", session);
  httpd_resp_set_hdr(request, "Content-Disposition", "attachment; filename=command-input.wav");
  esp_err_t result = httpd_resp_send_chunk(request, reinterpret_cast<const char*>(header), sizeof(header));
  const char* pcm = reinterpret_cast<const char*>(capture.samples);
  for (size_t offset = 0; result == ESP_OK && offset < bytes; offset += 4096) {
    result = httpd_resp_send_chunk(request, pcm + offset, min(size_t(4096), size_t(bytes) - offset));
  }
  free(capture.samples);
  if (result == ESP_OK) result = httpd_resp_send_chunk(request, nullptr, 0);
  return result;
}
esp_err_t commandAudioTestHandler(httpd_req_t* request) {
  if (request->method == HTTP_POST) {
    char gainText[4];
    if (!queryValue(request, "gain", gainText, sizeof(gainText)) || gainText[0] < '1' || gainText[0] > '8' || gainText[1] ||
        request->content_len <= 0 || request->content_len > 16000*7*2 || request->content_len % (512*2)) {
      httpd_resp_set_status(request, "400 Bad Request");
      return sendJson(request, "{\"message\":\"Send 16 kHz mono signed PCM in complete 512-sample frames, gain 1 through 8\"}");
    }
    auto* pcm = static_cast<int16_t*>(heap_caps_aligned_alloc(16, request->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pcm) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "No diagnostic memory");
    size_t received = 0;
    while (received < request->content_len) {
      const int count = httpd_req_recv(request, reinterpret_cast<char*>(pcm) + received, request->content_len - received);
      if (count <= 0) { free(pcm); return ESP_FAIL; }
      received += count;
    }
    if (!speech_commands::testAudio(pcm, received / sizeof(int16_t), gainText[0] - '0')) {
      free(pcm); httpd_resp_set_status(request, "409 Conflict");
      return sendJson(request, "{\"message\":\"Decoder busy or unavailable\"}");
    }
    return sendJson(request, "{\"message\":\"Decoder test queued; no robot actions\"}");
  }
  const auto probe = speech_commands::audioTestStatus();
  char escaped[768], json[1000];
  escapeJsonString(probe.text, escaped, sizeof(escaped));
  snprintf(json, sizeof(json), "{\"busy\":%s,\"complete\":%s,\"gain\":%u,\"chunks\":%lu,\"state\":%d,\"command\":%d,\"score\":%.3f,\"text\":\"%s\"}",
           probe.busy?"true":"false", probe.complete?"true":"false", probe.gain, (unsigned long)probe.chunks,
           probe.decoderState, probe.command, probe.score, escaped);
  return sendJson(request, json);
}
esp_err_t actionsHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  char value[4];
  if (!queryValue(request,"command",value,sizeof(value)) || (value[0]!='1' && value[0]!='7') || value[1]) {
    httpd_resp_set_status(request,"400 Bad Request");
    return controlReply(request,"Choose Say hello or Stop");
  }
  if (!currentGeneration(request)) {
    httpd_resp_set_status(request,"409 Conflict");
    return controlReply(request,"This request was cancelled by Stop or another action. Try again.");
  }
  if (!robot_actions::start(static_cast<robot_actions::Command>(value[0]-'0'),robot_actions::generation())) {
    httpd_resp_set_status(request,"409 Conflict");
    return controlReply(request,robot_actions::snapshot().phase);
  }
  return controlReply(request,"Action accepted");
}

esp_err_t wakeTestHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  if (!wake_word::testReaction()) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"Wake reaction unavailable during a turn, cooldown, or disconnected hardware\"}");
  }
  return sendJson(request, "{\"message\":\"Wake reaction requested\"}");
}

esp_err_t voiceStatusHandler(httpd_req_t* request) {
  const auto direction = voice_direction::snapshot();
  const auto zumo = zumo_link::snapshot();
  const auto wake = wake_word::snapshot();
  char error[160], result[48], json[720];
  escapeJsonString(direction.error, error, sizeof(error));
  escapeJsonString(zumo.lookResult, result, sizeof(result));
  snprintf(json, sizeof(json),
           "{\"available\":%s,\"stable\":%s,\"front_set\":%s,\"calibrated\":%s,"
           "\"enabled\":%s,\"degrees\":%.1f,\"relative\":%.1f,\"age_ms\":%lu,"
           "\"source\":\"%s\",\"read_resource\":%u,\"read_command\":%u,\"read_status\":%u,"
           "\"error\":\"%s\",\"turning\":%s,\"result\":\"%s\","
           "\"drive_ready\":%s,\"gyro_ready\":%s,\"paused\":%s}",
           direction.available ? "true" : "false", direction.stable ? "true" : "false",
           direction.frontSet ? "true" : "false", direction.calibrated ? "true" : "false",
           direction.enabled ? "true" : "false", direction.degrees, direction.relative,
           (unsigned long)direction.ageMs, direction.source,
           direction.readResource, direction.readCommand, direction.readStatus, error,
           zumo.lookActive ? "true" : "false", result,
           zumo.connected && zumo.ready && !zumo.calibrating ? "true" : "false",
           zumo.connected && zumo.voiceCapable ? "true" : "false",
           wake.pausedForVision ? "true" : "false");
  return sendJson(request, json);
}

esp_err_t voiceSetupHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  char action[8];
  if (!queryValue(request, "action", action, sizeof(action)) ||
      (strcmp(action, "front") && strcmp(action, "left") && strcmp(action, "on") &&
       strcmp(action, "off") && strcmp(action, "reset"))) {
    httpd_resp_set_status(request, "400 Bad Request");
    return sendJson(request, "{\"message\":\"Choose front, left, on, off, or reset\"}");
  }
  if (!voice_direction::configure(action)) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"Keep Digit still with vision off. Setup needs fresh clear speech from the chosen side; left must be about 90 degrees from front. Enabling also needs floor calibration and the updated Zumo gyro controller.\"}");
  }
  const char* message = !strcmp(action, "front") ? "Front saved. Now speak from Digit's left and press Set left." :
      !strcmp(action, "left") ? "Left saved. Verify the displayed directions, calibrate the floor, then enable voice turning." :
      !strcmp(action, "on") ? "Voice turning enabled. Say Digit for one turn toward your voice." :
      !strcmp(action, "off") ? "Voice turning disabled. Wake reactions still work." : "Direction setup reset; turning is off.";
  char json[240];
  snprintf(json, sizeof(json), "{\"message\":\"%s\"}", message);
  return sendJson(request, json);
}

esp_err_t visionHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  char value[4];
  if (!queryValue(request, "enabled", value, sizeof(value)) ||
      (strcmp(value, "0") != 0 && strcmp(value, "1") != 0)) {
    httpd_resp_set_status(request, "400 Bad Request");
    return sendJson(request, "{\"message\":\"Use enabled=0 or enabled=1\"}");
  }
  const bool enabled = strcmp(value, "1") == 0;
  robot_actions::manualOverride();
  if (!enabled) person_follow::setEnabled(false);
  if (!vision_service::setEnabled(enabled)) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    return sendJson(request, "{\"message\":\"Person detection is unavailable\"}");
  }
  return sendJson(request, enabled
                               ? "{\"message\":\"Person detection started\"}"
                               : "{\"message\":\"Person detection stopped\"}");
}

esp_err_t batteryVisionHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  char value[4];
  if (!queryValue(request, "enabled", value, sizeof(value)) ||
      (strcmp(value, "0") && strcmp(value, "1"))) {
    httpd_resp_set_status(request, "400 Bad Request");
    return controlReply(request, "Use enabled=0 or enabled=1");
  }
  const bool enabled = strcmp(value, "1") == 0;
  if (enabled && !currentGeneration(request)) {
    httpd_resp_set_status(request, "409 Conflict");
    return controlReply(request, "Object test cancelled; refresh the dashboard and try again");
  }
  if (enabled && !vision_service::snapshot().batteryReady) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    return controlReply(request, "Object model unavailable; flash the complete project bundle");
  }
  // Cancel motion and queued voice/routine work before changing camera modes.
  robot_actions::stop(enabled ? "Object test started" : "Object test stopped");
  if (!vision_service::setBatteryEnabled(enabled)) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    return controlReply(request, "Object detection is unavailable");
  }
  return controlReply(request, enabled ? "Object detection started; camera test only"
                                      : "Object detection stopped");
}

esp_err_t followHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  char value[4];
  if (!queryValue(request, "enabled", value, sizeof(value)) ||
      (strcmp(value, "0") != 0 && strcmp(value, "1") != 0)) {
    httpd_resp_set_status(request, "400 Bad Request");
    return sendJson(request, "{\"message\":\"Use enabled=0 or enabled=1\"}");
  }
  const bool enabled = strcmp(value, "1") == 0;
  robot_actions::manualOverride();
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
  char json[420];
  snprintf(json, sizeof(json),
           "{\"connected\":%s,\"ready\":%s,\"calibrating\":%s,"
           "\"edge\":%s,\"left\":%d,\"right\":%d,\"battery_mv\":%d,"
           "\"reply_age_ms\":%lu,\"fault\":\"%s\",\"last_error\":\"%s\",\"led_result\":\"%s\",\"brake_supported\":%s}",
           zumo.connected ? "true" : "false", zumo.ready ? "true" : "false",
           zumo.calibrating ? "true" : "false", zumo.edge ? "true" : "false",
           zumo.left, zumo.right, zumo.batteryMillivolts,
           static_cast<unsigned long>(zumo.replyAgeMs), fault, lastError,
           zumo.ledResult, zumo.brakeCapable ? "true" : "false");
  return sendJson(request, json);
}

esp_err_t zumoDriveHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  if (!currentGeneration(request)) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"Drive request cancelled by Stop or another action; release and try again\"}");
  }
  char leftText[12], rightText[12], accelerationText[80];
  int left = 0, right = 0, acceleration = 6000;
  if (!queryValue(request, "left", leftText, sizeof(leftText)) ||
      !queryValue(request, "right", rightText, sizeof(rightText)) ||
      !parseSpeed(leftText, &left) || !parseSpeed(rightText, &right)) {
    httpd_resp_set_status(request, "400 Bad Request");
    return sendJson(request, "{\"message\":\"Use left and right speeds from -6000 to 6000\"}");
  }
  if (queryValue(request, "acceleration", accelerationText, sizeof(accelerationText)) &&
      (!parseSpeed(accelerationText, &acceleration) || acceleration < 500)) {
    httpd_resp_set_status(request, "400 Bad Request");
    return sendJson(request, "{\"message\":\"Use acceleration from 500 to 6000 speed units per second\"}");
  }
  person_follow::setEnabled(false);
  robot_actions::manualOverride();
  if (!zumo_link::drive(left, right, acceleration)) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"Zumo is disconnected or not calibrated\"}");
  }
  return controlReply(request, "Drive command accepted");
}

esp_err_t zumoStopHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  char hardText[80];
  const bool hasHard = queryValue(request, "hard", hardText, sizeof(hardText));
  if (hasHard && strcmp(hardText, "0") && strcmp(hardText, "1")) {
    httpd_resp_set_status(request, "400 Bad Request");
    return sendJson(request, "{\"message\":\"Use hard=0 or hard=1\"}");
  }
  const bool smooth = !hasHard || strcmp(hardText, "1");
  const bool supported = zumo_link::snapshot().brakeCapable;
  robot_actions::stop("Dashboard Stop", smooth);
  zumo_link::poll(); // Flush cancellation/braking before acknowledging.
  return controlReply(request, !smooth ? "All actions cancelled; motor stop sent" :
      supported ? "All actions cancelled; slowing to a stop" :
      "All actions cancelled; smooth stopping needs the updated Zumo file");
}

esp_err_t zumoCalibrateHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  robot_actions::manualOverride();
  person_follow::setEnabled(false);
  if (!zumo_link::calibrate()) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"Zumo must be connected and stopped\"}");
  }
  return sendJson(request, "{\"message\":\"Floor calibration requested\"}");
}

esp_err_t zumoFaceHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  char value[12];
  if (!queryValue(request, "value", value, sizeof(value)) ||
      !zumo_link::isFaceName(value)) {
    httpd_resp_set_status(request, "400 Bad Request");
    return sendJson(request, "{\"message\":\"Unknown face expression\"}");
  }
  person_follow::setEnabled(false);
  robot_actions::manualOverride();
  if (!zumo_link::face(value)) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"Zumo must be connected and stopped\"}");
  }
  return sendJson(request, "{\"message\":\"Face command accepted\"}");
}

bool parseLedValue(httpd_req_t* request, const char* key, int maximum,
                   int* result) {
  char value[8];
  if (!queryValue(request, key, value, sizeof(value)) || !*value) return false;
  int number = 0;
  for (const char* cursor = value; *cursor; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
    number = number * 10 + (*cursor - '0');
    if (number > maximum) return false;
  }
  *result = number;
  return true;
}

esp_err_t zumoLedHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  char targetText[8];
  int target = -1, red, green, blue, brightness;
  if (!queryValue(request, "target", targetText, sizeof(targetText)) ||
      (strcmp(targetText, "ALL") != 0 &&
       !(targetText[0] >= '0' && targetText[0] <= '5' && targetText[1] == '\0')) ||
      !parseLedValue(request, "r", 255, &red) ||
      !parseLedValue(request, "g", 255, &green) ||
      !parseLedValue(request, "b", 255, &blue) ||
      !parseLedValue(request, "brightness", 31, &brightness)) {
    httpd_resp_set_status(request, "400 Bad Request");
    return sendJson(request, "{\"message\":\"Use target ALL or 0-5, RGB 0-255, and brightness 0-31\"}");
  }
  if (strcmp(targetText, "ALL") != 0) target = targetText[0] - '0';
  if (!zumo_link::led(target, red, green, blue, brightness)) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"Zumo is disconnected\"}");
  }
  return sendJson(request, "{\"message\":\"LED command sent\"}");
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
  controlConfig.max_uri_handlers = 32;
  controlConfig.stack_size = 8192;
  controlConfig.lru_purge_enable = true;

  if (httpd_start(&controlServer, &controlConfig) != ESP_OK) {
    Serial.println("Web: could not start the control server");
    return false;
  }

  const bool controlRoutesReady =
      registerUri(controlServer, "/", HTTP_GET, rootHandler) &&
      registerUri(controlServer, "/api/status", HTTP_GET, statusHandler) &&
      registerUri(controlServer, "/api/actions", HTTP_GET, actionsStatusHandler) &&
      registerUri(controlServer, "/api/audio/processing", HTTP_GET, audioProcessingHandler) &&
      registerUri(controlServer, "/api/actions/audio.wav", HTTP_GET, commandAudioHandler) &&
      registerUri(controlServer, "/api/actions/audio/test", HTTP_POST, commandAudioTestHandler) &&
      registerUri(controlServer, "/api/actions/audio/test", HTTP_GET, commandAudioTestHandler) &&
      registerUri(controlServer, "/api/actions", HTTP_POST, actionsHandler) &&
      registerUri(controlServer, "/api/actions/photo", HTTP_GET, photo_store::send) &&
      registerUri(controlServer, "/api/wake/test", HTTP_POST, wakeTestHandler) &&
      registerUri(controlServer, "/api/voice/status", HTTP_GET, voiceStatusHandler) &&
      registerUri(controlServer, "/api/voice/setup", HTTP_POST, voiceSetupHandler) &&
      registerUri(controlServer, "/api/vision", HTTP_POST, visionHandler) &&
      registerUri(controlServer, "/api/vision/battery", HTTP_POST, batteryVisionHandler) &&
      registerUri(controlServer, "/api/vision/objects", HTTP_POST, batteryVisionHandler) &&
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
                  zumoFaceHandler) &&
      registerUri(controlServer, "/api/zumo/led", HTTP_POST,
                  zumoLedHandler);
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
