#include "web_server.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <cmath>
#include <cctype>

#include "app_config.h"
#include "audio_engine.h"
#include "reaction_player.h"
#include "camera_service.h"
#include "vision_service.h"
#include "object_model_config.h"
#include "zumo_link.h"
#include "wake_word.h"
#include "voice_direction.h"
#include "voice_settings.h"
#include "robot_actions.h"
#include "speech_commands.h"
#include "local_stop.h"
#include "gemini_client.h"

namespace {

httpd_handle_t controlServer = nullptr;
httpd_handle_t streamServer = nullptr;

constexpr char kStreamContentType[] =
    "multipart/x-mixed-replace;boundary=frame";
constexpr char kStreamBoundary[] = "\r\n--frame\r\n";
constexpr char kStreamPartHeader[] =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

extern "C" {
extern const unsigned char dashboard_html_gz_start[];
extern const unsigned char dashboard_html_gz_end[];
}

// Editable dashboard source. The build packs this literal as gzip; only the
// compressed asset is linked into firmware. Preview and tests read it directly.
#if 0
const char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Digit Sense</title>
  <style>
    :root{color-scheme:light;--cyan:#126b60;--ink:#182b2a;--panel:#fff;--muted:#526560;--red:#b52d36;--line:#dce4df;--soft:#f3f6f3}
    *{box-sizing:border-box}
    html{scroll-padding-top:150px}
    body{margin:0;background:#f3f5f1;color:var(--ink);font:16px/1.55 "Segoe UI",system-ui,sans-serif}
    main{width:min(1400px,100%);margin:auto;padding:0 32px 40px}
    h1,h2,h3,p{margin-top:0}h1{font-size:30px;line-height:1.15;letter-spacing:-1px;margin-bottom:3px}h2{font-size:23px;letter-spacing:-.5px;line-height:1.3;margin-bottom:8px}h3{font-size:17px;margin-bottom:8px}
    p{margin-bottom:16px;color:var(--muted)}button,input,select{font:inherit}button,a,input,select,summary{-webkit-tap-highlight-color:transparent}
    button{border:1px solid var(--line);border-radius:10px;padding:12px 16px;min-height:46px;font-size:15px;font-weight:650;cursor:pointer;background:var(--soft);color:var(--ink);line-height:1.3}
    button:hover:not(:disabled){background:#e5eeea;border-color:#9cb9ad}button.primary{background:var(--cyan);border-color:var(--cyan);color:#fff}button.primary:hover:not(:disabled){background:#0c574d}
    button.danger{background:#fff0f0;border-color:#ebc2c4;color:#a0242d}button.danger:hover:not(:disabled){background:#fbdede}
    button:disabled{color:#64716b;background:#eef1ee;border-color:#e1e6e1;cursor:not-allowed}
    :focus-visible{outline:3px solid #127467;outline-offset:3px}a{color:var(--cyan)}[hidden]{display:none!important}
    .top{position:sticky;top:0;z-index:10;display:flex;justify-content:space-between;align-items:center;gap:16px;background:#f3f5f1;min-height:108px;padding:20px 0 16px;border-bottom:1px solid var(--line)}
    .brand{display:flex;align-items:center;gap:13px}.brand-mark{width:48px;height:48px;background:var(--cyan);border-radius:15px;display:flex;align-items:center;justify-content:center;gap:6px}.brand-mark i{display:block;width:7px;height:17px;border-radius:5px;background:#fff}
    .subtitle{font-size:14px;color:var(--muted)}.top-actions{display:flex;align-items:center;gap:12px}.pill{border:1px solid #bfd3c8;border-radius:999px;padding:7px 13px;color:#36594a;background:#eaf2eb;font-size:13px;font-weight:700}
    .top .global-stop{background:#b52d36;border-color:#b52d36;color:#fff;min-width:126px}.top .global-stop:hover{background:#94212b}
    .section-nav{display:flex;gap:6px;flex-wrap:wrap;padding:16px 0 22px}.section-nav a{padding:7px 13px;border-radius:8px;text-decoration:none;font-size:14px;color:var(--muted);font-weight:650}.section-nav a:hover{background:#e4ebe5;color:var(--ink)}
    .workspace{display:grid;gap:22px}.panel{background:var(--panel);border:1px solid var(--line);border-radius:18px;overflow:hidden;min-width:0;box-shadow:0 4px 16px #243c2e04}
    .section-heading{display:flex;justify-content:space-between;align-items:center;gap:16px;padding:22px 24px 18px}.section-heading h2{margin:0}.eyebrow{display:block;font-size:11px;font-weight:750;letter-spacing:1.6px;text-transform:uppercase;color:var(--cyan);margin-bottom:6px}.section-note{font-size:13px;color:var(--muted)}
    .drive-view{display:grid;grid-template-columns:minmax(0,1fr) 400px;align-items:start;border-top:1px solid var(--line)}.drive-view .zumo{border-left:1px solid var(--line);border-radius:0;padding:24px}.drive-view .video{min-width:0}
    .video{aspect-ratio:4/3;background:#152422;display:grid;place-items:center;position:relative;color:#e9f5ef}.video img{width:100%;height:100%;object-fit:contain;color:#c3d5cc;font-size:14px}.video canvas{position:absolute;inset:0;width:100%;height:100%;pointer-events:none}
    .video>span{position:absolute;bottom:16px;left:16px;right:16px;width:fit-content;background:#152422e8;border:1px solid #51665e;border-radius:8px;padding:7px 11px;font-size:12px;pointer-events:none}
    .camera-readouts{position:absolute;top:16px;left:16px;right:16px;display:flex;flex-wrap:wrap;align-items:flex-start;gap:8px;pointer-events:none}.person-badge{padding:8px 12px;border-radius:8px;background:#d8f6e8;color:#154b38;font-size:14px}.distance-badge{margin-left:auto;padding:8px 12px;border:1px solid #9ab7a9;border-radius:8px;background:#f6fbf7;color:#234538;font-size:14px;font-weight:700;font-variant-numeric:tabular-nums}.distance-badge.unavailable{color:#4d6258}
    .zumo{padding:24px;min-width:0}.zumo>p{max-width:90ch}.zumo h2{margin-bottom:8px}.zumo-status{display:grid;grid-template-columns:1fr 1fr;gap:7px;margin:16px 0}.zumo-status span{background:var(--soft);border-radius:7px;padding:7px 9px;font-size:13px;overflow-wrap:anywhere}
    .drive-pad{display:grid;grid-template-columns:repeat(3,minmax(0,96px));gap:8px;justify-content:center;touch-action:none;user-select:none;margin:20px 0}.drive-pad button{min-height:58px;touch-action:none;font-size:19px;background:#e9f0eb}.drive-pad .forward{grid-column:2}.drive-pad .left{grid-column:1}.drive-pad .stop{grid-column:2;font-size:14px;background:#fff0f0}.drive-pad .right{grid-column:3}.drive-pad .back{grid-column:2}
    .speed{display:flex;align-items:center;gap:12px;margin:14px 0;font-size:14px}.speed input{flex:1;min-width:0}.speed b{min-width:43px;text-align:right;font-variant-numeric:tabular-nums}input[type=range]{accent-color:var(--cyan);cursor:pointer;min-height:26px}.acceleration-hint{font-size:13px}
    .zumo-actions{display:flex;flex-wrap:wrap;gap:10px;margin:16px 0}.zumo-actions button{flex:1 1 140px}#actions .zumo-actions{display:grid;grid-template-columns:repeat(3,minmax(0,1fr))}#actions .zumo-actions button{min-height:100px;text-align:left;padding:18px}#actions .zumo-actions small{display:block;font-size:13px;font-weight:400;line-height:1.5;color:var(--muted);margin-top:8px}
    .two-column{display:grid;grid-template-columns:1fr 1fr;gap:22px;align-items:start}.controls{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px;margin:18px 0}.vision-controls{grid-template-columns:1fr 1fr}
    .volume{padding:16px 18px;background:var(--soft);border:1px solid var(--line);border-radius:12px;margin:18px 0}.volume label{display:flex;justify-content:space-between;align-items:center;gap:12px;margin-bottom:6px}.volume label>b{font-size:22px;color:var(--cyan)}.volume input{width:100%}.volume input:disabled{cursor:not-allowed}.volume-note{font-size:12px;margin:4px 0 0}
    .details{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px}.metric{background:var(--soft);border:1px solid var(--line);border-radius:10px;padding:14px}.metric b{display:block;font-size:18px;margin-top:4px;overflow-wrap:anywhere;font-variant-numeric:tabular-nums}.metric small{color:var(--muted);font-size:13px}
    .status-line{background:var(--soft);border-left:3px solid #7fa694;padding:12px 14px;border-radius:0 8px 8px 0;margin:16px 0}#routineState{font-size:18px}.help{border-top:1px solid var(--line);margin-top:18px;padding-top:14px}.help summary{cursor:pointer;font-size:14px;font-weight:650;color:var(--cyan);padding:4px 0}.help[open] summary{margin-bottom:12px}.help p{font-size:14px}.help p:last-child{margin-bottom:0}
    #followPanel{margin-top:18px;padding:16px;background:var(--soft);border-radius:10px;font-size:14px}#followPanel p{margin:6px 0 0}#batteryState{font-size:14px;margin-bottom:0}
    .face-previews{display:grid;grid-template-columns:repeat(5,minmax(0,1fr));gap:10px;margin:20px 0}.face-previews button{text-align:left;padding:18px 16px;min-height:94px;background:#f6f8f5}.face-previews small{display:block;color:var(--muted);font-size:13px;font-weight:400;margin-top:7px;line-height:1.45}.face-options{display:flex;align-items:center;flex-wrap:wrap;gap:16px}.face-options label{display:flex;align-items:center;gap:9px;font-size:14px}.face-options input{accent-color:var(--cyan);width:19px;height:19px}
    #geminiKey,#geminiModel{display:block;width:100%;max-width:480px;border:1px solid #becdc3;border-radius:7px;padding:8px}#geminiIdle{width:90px;max-width:100%}
    .led-controls{display:flex;flex-wrap:wrap;align-items:center;gap:14px;margin-top:22px;padding:18px;background:var(--soft);border-radius:12px}.led-controls label{display:flex;align-items:center;gap:8px;font-size:14px}.led-controls select,.led-controls input[type=color]{background:#fff;color:var(--ink);border:1px solid #becdc3;border-radius:7px;padding:5px;min-height:36px}.led-controls input[type=range]{width:100px}.led-controls button{padding:10px 16px}#ledResult{font-size:13px;color:var(--muted)}
    #message,#zumoMessage,#faceMessage,#routineMessage,#voiceResult{font-size:14px;overflow-wrap:anywhere}#message{min-height:24px;color:var(--muted);margin-top:16px}#message.error{color:var(--red)}#faceMessage{color:var(--muted)}#voiceSetup{margin-top:24px;padding-top:24px;border-top:1px solid var(--line)}.footer-note{font-size:12px;color:var(--muted);text-align:center;margin:24px 0 0}
    @media(max-width:1100px){main{padding-inline:24px}.drive-view{grid-template-columns:minmax(0,1fr) 360px}.drive-view .zumo{padding:20px}#actions .zumo-actions{grid-template-columns:repeat(3,minmax(0,1fr))}.face-previews{grid-template-columns:repeat(4,minmax(0,1fr))}}
    @media(max-width:900px){.drive-view,.two-column{grid-template-columns:1fr}.drive-view .zumo{border-left:0;border-top:1px solid var(--line)}.drive-pad{grid-template-columns:repeat(3,minmax(0,110px))}.section-note{display:none}.face-previews{grid-template-columns:repeat(3,minmax(0,1fr))}}
    @media(max-width:600px){html{scroll-padding-top:116px}main{padding:0 14px 24px}.top{min-height:90px;gap:8px;padding:16px 0}.brand{gap:8px}.brand-mark{width:36px;height:38px;border-radius:11px}.brand-mark i{width:5px;height:14px}h1{font-size:25px}.subtitle{font-size:12px}.top-actions{gap:7px}.pill{font-size:10px;padding:6px 8px}.top .global-stop{min-width:0;padding:12px 10px;font-size:12px}.section-nav{gap:2px;padding:10px 0 16px}.section-nav a{font-size:13px;padding:7px 9px}.workspace,.two-column{gap:16px}.panel{border-radius:14px}.zumo,.section-heading{padding:20px 16px}h2{font-size:21px}.controls{grid-template-columns:1fr 1fr}#actions .zumo-actions{grid-template-columns:1fr}.face-previews{grid-template-columns:1fr 1fr}.face-previews button{padding:14px 12px}.details{grid-template-columns:1fr}.led-controls{padding:14px;gap:12px}.video>span{font-size:11px;left:10px;right:10px;bottom:10px}.camera-readouts{top:10px;left:10px;right:10px}.person-badge,.distance-badge{font-size:12px;padding:7px 9px}}
    @media(prefers-reduced-motion:no-preference){button,a{transition:background .15s,border-color .15s}}
  </style>
</head>
<body>
<main>
  <header class="top">
    <div class="brand"><span class="brand-mark" aria-hidden="true"><i></i><i></i></span><div><h1>Digit</h1><div class="subtitle">Your robot, at a glance</div></div></div>
    <div class="top-actions"><div class="pill" id="state" aria-label="Audio status">CONNECTING</div><button class="danger global-stop" id="globalStop" onclick="stopZumo(true,'stop-all')" aria-label="Stop all movement, audio and actions">■ STOP ALL</button></div>
  </header>
  <nav class="section-nav" aria-label="Dashboard sections"><a href="#drive">Drive</a><a href="#actions">Task</a><a href="#audio">Audio</a><a href="#expressions">Face &amp; lights</a><a href="#voice">Voice setup</a></nav>
  <div class="workspace">
    <section class="panel" id="drive" aria-labelledby="driveTitle">
      <div class="section-heading"><div><span class="eyebrow">01 / Live view</span><h2 id="driveTitle">See &amp; drive</h2></div><span class="section-note">Hold to move · Release to stop</span></div>
    <div class="drive-view">
    <div class="video"><img id="stream" alt="Digit camera stream"><canvas id="boxes" width="640" height="480" aria-hidden="true"></canvas><div class="camera-readouts"><strong class="person-badge" id="personBadge" hidden>Naz Detected</strong><output class="distance-badge unavailable" id="distanceBadge" title="Distance from the front ToF sensor">ToF: connecting</output></div><span id="camera">Camera starting...</span></div>
    <aside class="zumo" id="zumoDrive" aria-label="Zumo drive controls">
      <h3>Manual drive</h3><p>Hold a direction to move. Release to stop. Calibrate on safe surface first.</p>
      <div class="zumo-status"><span id="zumoLink">Zumo: connecting</span><span id="zumoReady">Drive: locked</span><span id="zumoBattery">Battery: --</span><span id="zumoFault">Fault: --</span></div>
      <div class="drive-pad" id="drivePad">
        <button class="forward" data-drive="forward" aria-label="Hold to drive forward">▲</button>
        <button class="left" data-drive="left" aria-label="Hold to turn left">◀</button>
        <button class="stop danger" id="zumoStop" aria-label="Stop Zumo">STOP</button>
        <button class="right" data-drive="right" aria-label="Hold to turn right">▶</button>
        <button class="back" data-drive="back" aria-label="Hold to reverse">▼</button>
      </div>
      <label class="speed" for="driveSpeed">Drive speed <input id="driveSpeed" type="range" min="3000" max="6000" step="100" value="4000"><b id="driveSpeedValue">4000</b></label>
      <label class="speed" for="driveAcceleration">Acceleration <input id="driveAcceleration" type="range" min="500" max="6000" step="100" value="6000" aria-describedby="accelerationHint"><b id="driveAccelerationValue">6000</b></label>
      <p class="acceleration-hint" id="accelerationHint">Lower acceleration feels gentler. Applies to driving and voice turns; Gemini travel uses a fixed gentle speed.</p>
      <div class="zumo-actions"><button id="zumoCalibrate">Calibrate surface</button></div>
      <div class="controls vision-controls"><button id="visionToggle" onclick="toggleVision()" disabled>Person detection</button><button id="batteryToggle" onclick="toggleBattery()" disabled>Object detection</button></div>
      <p id="batteryState" role="status">Object model: checking...</p>
      <div id="zumoMessage" role="status">Connecting to Zumo...</div>
    </aside>
    </div>

    </section>
    <section class="panel zumo" id="actions" aria-labelledby="actionsTitle"><span class="eyebrow">02 / Current goal</span><h2 id="actionsTitle">Gemini task</h2>

      <p id="commandState" role="status" aria-live="polite">Checking voice commands...</p>
      <p class="status-line"><strong id="routineState">Idle</strong></p>
      <p id="actionAvailability" role="status"></p>
      <p id="taskGoal">Ask Digit for a goal after saying “Digit”.</p>
      <p id="taskProgress" role="status"></p><p id="taskResult" role="status"></p>
      <p>Digit looks when needed and after each move. One step is limited to 30 cm forward, 10 cm backward, or a 90° turn.</p>
      <p id="routineMessage" role="status"></p>
      <details class="help" id="actionDebug"><summary>Why did Digit stop? · Debug log</summary>
        <p>Recent events are saved on Digit until he restarts, including stops from other dashboard tabs. Times are seconds since startup; newest events come first.</p>
        <p id="debugSensors">Waiting for controller readings…</p>
        <p id="debugMessage" role="status"></p>
        <button onclick="copyDebugLog()">Copy debug log</button>
        <pre id="debugLog" style="white-space:pre-wrap;overflow-wrap:anywhere;max-height:360px;overflow:auto;font-size:14px;line-height:1.6">Open this panel to load recent events.</pre>
      </details>

    </section>
    <div class="two-column">
      <section class="panel zumo" id="audio" aria-labelledby="audioTitle"><span class="eyebrow">03 / Sound</span><h2 id="audioTitle">Speaker</h2><p>Adjust Digit’s speaking volume.</p>
        <div class="volume">
      <label for="volume"><span>Speaker volume</span><b><span id="volumeValue">100</span>%</b></label>
      <input id="volume" type="range" min="0" max="100" step="1" value="100" oninput="queueVolume(this.value)" aria-label="Speaker volume">
      <p class="volume-note">Starts at 100% when Digit powers on.</p>
    </div>

        <div id="message" role="status">Waiting for device status...</div>
      </section>

    </div>
    <section class="panel zumo" id="expressions" aria-labelledby="facePreviewTitle"><span class="eyebrow">05 / Personality</span><h2 id="facePreviewTitle">Digit expressions</h2><p>Tap an expression to see Digit’s animated eyes. Gemini can use these silently during a conversation.</p>
      <div class="face-options">
        <button class="danger" id="faceStop" onclick="stopFacePreview()">Neutral eyes</button>
      </div>
      <div class="face-previews" id="faceButtons">
        <button data-face="NEUTRAL" disabled>Neutral<small>Soft acknowledgment</small></button>
        <button data-face="HAPPY" disabled>Happy<small>A bright, bouncing greeting</small></button><button data-face="SAD" disabled>Sad<small>A long, falling sigh</small></button>
        <button data-face="MAD" disabled>Mad<small>Three rumbling grumbles</small></button><button data-face="SERIOUS" disabled>Serious<small>A firm, steady hum</small></button>
        <button data-face="CURIOUS" disabled>Curious<small>A searching, rising whistle</small></button><button data-face="SURPRISED" disabled>Surprised<small>A gasp, then a soft recovery</small></button>
        <button data-face="SLEEPING" disabled>Sleeping<small>One slow, sleepy breath</small></button><button data-face="CONFUSED" disabled>Confused<small>A wavering, puzzled question</small></button>
        <button data-face="SLEEPY" disabled>Sleepy<small>A long yawn and drowsy sigh</small></button><button data-face="EXCITED" disabled>Excited<small>A bouncing series of trills</small></button>
        <button data-face="CRYING" disabled>Crying<small>Three trembling sobs</small></button><button data-face="LOVE" disabled>Love<small>A warm, pulsing two-note coo</small></button>
      </div>
      <p>Adjust loudness with the speaker slider. Gemini expressions return to neutral after their requested duration.</p>
      <div id="faceMessage" role="status">Connect Digit and stop movement to preview. Floor calibration is not required.</div>
      <div class="led-controls">
        <strong>RGB LEDs</strong>
        <label for="ledTarget">LED <select id="ledTarget"><option value="ALL">All six</option><option value="0">1</option><option value="1">2</option><option value="2">3</option><option value="3">4</option><option value="4">5</option><option value="5">6</option></select></label>
        <label for="ledColor">Color <input id="ledColor" type="color" value="#36e4ff"></label>
        <label for="ledBrightness">Brightness <input id="ledBrightness" type="range" min="1" max="31" value="5"><span id="ledBrightnessValue">5</span></label>
        <button id="ledApply">Apply</button><button id="ledOff">Off</button><span id="ledResult">--</span>
      </div>

    </section>
    <section class="panel zumo" id="voice" aria-labelledby="voiceTitle"><span class="eyebrow">06 / Listening</span><h2 id="voiceTitle">Voice &amp; wake word</h2>
      <p>Digit keeps a Gemini connection ready after startup. Say “Digit”, wait for his answering voice, then talk naturally. Gemini Live receives microphone audio and requested camera frames only after wake or a dashboard start. After inactivity or Stop, upload and playback stop while the connection stays ready. Digit's wake word and Stop listener stay local.</p>
      <div class="status-line"><strong id="geminiState">Checking Gemini Live...</strong><p id="geminiDetail"></p><p id="geminiTranscript"></p><p id="geminiResponse"></p></div>
      <div class="zumo-actions"><button id="geminiBegin" onclick="liveAction('conversation')" disabled>Begin conversation</button><button id="geminiTestOnly" onclick="liveAction('test_only')" disabled>Test-only stream</button><button id="geminiEnd" class="danger" onclick="endLive()">End conversation</button></div>
      <p>Test-only stream sends audio and requested camera frames and plays the reply with robot actions disabled. Begin conversation uses the action setting below. End conversation and STOP ALL cancel streaming, speaker audio, and pending robot work, then leave Gemini ready for the next wake.</p>
      <details class="help"><summary>Gemini Live setup</summary>
        <p id="geminiKeyState">Checking saved key...</p>
        <label for="geminiKey">Gemini API key <input id="geminiKey" type="password" maxlength="511" autocomplete="new-password" placeholder="Enter a key to save on Digit"></label>
        <p><label for="geminiModel">Live model <input id="geminiModel" type="text" maxlength="63" value="gemini-3.8-live" autocomplete="off"></label></p>
        <p><label for="geminiIdle">End after silence (seconds) <input id="geminiIdle" type="number" min="10" max="300" step="1" value="30"></label></p>
        <p><label><input id="geminiDryRun" type="checkbox" checked> Preview robot actions only</label><br><small>Leave this on while testing. Turn it off to let Gemini request Digit's existing actions, subject to local readiness and safety checks.</small></p>
        <div class="zumo-actions"><button id="geminiSave" onclick="saveGemini()">Save settings</button><button id="geminiRemove" onclick="saveGemini(true)" class="danger">Remove saved key</button><button id="geminiTest" onclick="liveAction('probe')" disabled>Test connection</button></div>
        <p>Test connection checks the ready service connection or reconnects if needed without sending microphone or camera data, playing audio, or running actions. Changing the saved key reconnects with upload off. The key is saved on the XIAO and is never sent back to this page. This dashboard uses HTTP: save keys only on your trusted local network.</p>
        <p id="geminiMessage" role="status"></p>
      </details>
      <p><strong id="wakeState">Starting...</strong> <span id="wakeLast"></span></p>
      <p id="wakeResult"></p>
      <button id="wakeTest" onclick="action('/api/wake/test')" disabled>Test wake reaction</button>
      <p id="localStopState" role="status">Local Stop: checking...</p>
      <div id="voiceSetup"><h3>Turn toward my voice</h3>
      <p>Fixed microphone orientation: 270° is straight ahead; 0°/360° is directly left. Keep the array fastened in this position so it turns with Digit. No voice direction calibration is needed.</p>
      <p><strong id="voiceState">Checking direction...</strong> <span id="voiceAngle"></span></p>
      <div class="zumo-actions">
        <button id="voiceToggle" onclick="voiceAction(voiceEnabled?'off':'on')" disabled>Enable voice turning</button>
      </div>
      <p id="voiceMessage">Voice turning starts on by default. Your on/off choice is saved. Surface calibration is still required before movement.</p>
      <p id="voiceResult"></p>
      <p>Once enabled, “Digit” requests one turn, then the lights and face before listening. If the direction is unclear, Digit only reacts. The reaction test above never turns the tracks.</p>
    </div>

    </section>
    <section class="panel zumo" aria-labelledby="deviceTitle"><h2 id="deviceTitle">Device details</h2><div class="details">
      <div class="metric"><small>PSRAM free</small><b id="memory">--</b></div>
      <div class="metric"><small>Network address</small><b id="network">--</b></div>
    </div>

    </section>
  </div>
  <p class="footer-note">Digit Sense · XIAO camera &amp; audio + Zumo drive controller</p>
</main>
<script>
const $=id=>document.getElementById(id);
$('stream').src=`http://${location.hostname}:81/stream`;
let heldDirection=null,driveTimer=null,driveInFlight=false,zumoReady=false,cameraReady=false,visionEnabled=false,visionPending=false,unusedDriveFlag=false;
let voiceEnabled=false,voicePending=false,batteryEnabled=false,batteryPending=false,batteryNotice='';
let controlGeneration=0,stopPending=false,hardStopPending=false,stopRequests=0,actionActive=false;
function voiceCommandMessage(s){
   if(s.live_active)return s.live_reason||'Gemini Live conversation active';
   if(s.listening)return s.voice_result==='Wake voice'?'Digit is answering…':'Connecting to Gemini Live…';
  if(!s.voice_ready)return s.voice_error||'Voice commands unavailable';
   if(s.live_probe)return 'Checking Gemini connection…';
   if(s.live_connected)return 'Ready · say Digit';
   if(s.live_connecting)return 'Connecting to Gemini Live…';
   if(s.voice_result==='Audio queue overflow'||s.voice_result==='Audio interrupted')return 'Conversation interrupted. Say Digit and try again.';
   return s.voice_result&&s.voice_result!=='Idle'?s.voice_result:'Say Digit to begin a conversation';
}
function localStopMessage(s){return 'Local Stop: '+(!s.local_stop_ready?'unavailable'+(s.local_stop_error?' · '+s.local_stop_error:''):s.local_stop_active?'listening on Digit':'ready · listens during robot activity')+(s.local_stop_detections>0?' · '+s.local_stop_detections+' heard':'');}
async function refreshActions(){try{const r=await fetch('/api/actions',{cache:'no-store'});if(!r.ok)throw Error('Task status unavailable');const s=await r.json();controlGeneration=Math.max(controlGeneration,s.generation);actionActive=s.active;if(Number.isFinite(s.drive_speed)&&!driveSpeedDirty){$('driveSpeed').value=s.drive_speed;$('driveSpeedValue').textContent=s.drive_speed;}updateGeminiStatus(s);$('routineState').textContent=s.phase;$('commandState').textContent=voiceCommandMessage(s);$('localStopState').textContent=localStopMessage(s);$('actionAvailability').textContent=s.movement_reason||'';$('taskGoal').textContent=s.task_goal||'Ask Digit for a goal after saying “Digit”.';$('taskProgress').textContent=s.task_active?s.task_phase+' · '+s.task_moves+' of 12 moves':'';$('taskResult').textContent=s.task_result||'';}catch(error){geminiStatusAvailable=false;$('geminiState').textContent='Gemini status unavailable';updateGeminiControls();$('commandState').textContent=error.message;$('localStopState').textContent='Local Stop: status unavailable';}}
 let geminiConfigured=false,geminiPending=false,geminiActive=false,geminiSettingsLoaded=false,geminiStatusAvailable=false,geminiLastStatus=null;
 function updateGeminiControls(){for(const id of ['geminiBegin','geminiTestOnly','geminiTest'])$(id).disabled=!geminiConfigured||!geminiStatusAvailable||geminiActive||geminiPending||stopPending||!controlGeneration;for(const id of ['geminiSave','geminiRemove'])$(id).disabled=geminiPending||stopPending||!controlGeneration;}
 function updateGeminiStatus(s){geminiLastStatus=s;geminiStatusAvailable=true;const mediaActive=!!s.live_active,probe=!!s.live_probe;geminiActive=mediaActive||probe;$('geminiState').textContent=mediaActive?s.live_reason||s.voice_error||'Gemini Live conversation active':probe?'Checking Gemini connection…':!geminiConfigured?s.voice_error||'Add an API key to begin':s.live_connected?'Ready · say Digit':s.voice_error||'Connecting to Gemini Live…';const videoAge=s.live_video_capture_age_ms;const mode=s.live_dry_run?'Preview actions':'Robot actions enabled';$('geminiDetail').textContent=mediaActive?mode+' · '+(s.live_connected?'connected':'connecting')+' · '+(s.live_video_frames||0)+' requested camera frames'+(Number.isFinite(videoAge)&&videoAge<10000?' · latest '+(videoAge/1000).toFixed(1)+' s ago':'')+(s.live_idle_remaining_ms>0?' · idle timeout '+Math.ceil(s.live_idle_remaining_ms/1000)+' s':''):geminiConfigured?(s.live_connected?'Connected · ':'')+'Microphone and camera upload off':'';$('geminiTranscript').textContent=s.live_transcript?'You: '+s.live_transcript:'';$('geminiResponse').textContent=s.live_response?'Digit: '+s.live_response:'';updateGeminiControls();}
 async function refreshGeminiSettings(){try{const r=await fetch('/api/gemini/settings',{cache:'no-store'});const s=await r.json();if(!r.ok||typeof s.configured!=='boolean'||typeof s.model!=='string'||typeof s.dry_run!=='boolean'||!Number.isFinite(s.generation)||!Number.isFinite(s.idle_timeout_sec))throw Error(s.message||'Gemini settings unavailable');geminiConfigured=!!s.configured;controlGeneration=Math.max(controlGeneration,s.generation);$('geminiKeyState').textContent=geminiConfigured?'API key saved on Digit':'No API key saved';if(!geminiSettingsLoaded){$('geminiModel').value=s.model;$('geminiDryRun').checked=!!s.dry_run;$('geminiIdle').value=s.idle_timeout_sec;geminiSettingsLoaded=true;}if(geminiLastStatus)updateGeminiStatus(geminiLastStatus);else updateGeminiControls();}catch(error){geminiConfigured=false;$('geminiKeyState').textContent=error.message;updateGeminiControls();}}
 async function geminiPost(url,body={}){const r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({...body,generation:controlGeneration})});const s=await r.json();if(Number.isFinite(s.generation))controlGeneration=Math.max(controlGeneration,s.generation);if(!r.ok)throw Error(s.message||'Gemini request failed');return s;}
 async function saveGemini(remove=false){if(geminiPending||stopPending||!controlGeneration)return;geminiPending=true;updateGeminiControls();try{const body=remove?{remove_key:true}:{api_key:$('geminiKey').value,model:$('geminiModel').value,dry_run:$('geminiDryRun').checked,idle_timeout_sec:Number($('geminiIdle').value)};const s=await geminiPost('/api/gemini/settings',body);$('geminiKey').value='';$('geminiMessage').textContent=s.message;geminiSettingsLoaded=false;await refreshGeminiSettings();}catch(error){$('geminiMessage').textContent=error.message;}finally{geminiPending=false;updateGeminiControls();await refreshActions();}}
 async function liveAction(mode){if(geminiPending||stopPending||!controlGeneration)return;geminiPending=true;updateGeminiControls();try{const s=await geminiPost(mode==='probe'?'/api/gemini/live/test':'/api/gemini/live/start',mode==='probe'?{}:{mode});$('geminiMessage').textContent=s.message;}catch(error){$('geminiMessage').textContent=error.message;}finally{geminiPending=false;updateGeminiControls();await refreshActions();}}
 async function endLive(){try{await refreshActions();const s=await geminiPost('/api/gemini/live/stop');$('geminiMessage').textContent=s.message;}catch(error){$('geminiMessage').textContent=error.message;}finally{await refreshActions();}}
let debugData=null,debugPending=false;
function motionDebug(s){return !s.motion_supported&&!s.motion_state?'Motion jobs: update both controllers':`Zumo job ${s.motion_job??0}: ${s.motion_state||'IDLE'} · ${s.motion_result||'NONE'} | Target L/R: ${s.target_left_cps??0}/${s.target_right_cps??0} counts/s | Heading: ${((s.heading_deci_deg??0)/10).toFixed(1)}° | Longest control gap: ${s.control_max_gap_ms??0} ms · overruns ${s.control_overruns??0} · longest display slice ${((s.display_max_slice_us??0)/1000).toFixed(1)} ms`;}
async function refreshDebug(){if(!$('actionDebug').open||debugPending)return;debugPending=true;try{const r=await fetch('/api/actions/debug',{cache:'no-store'});if(!r.ok)throw Error('Debug log unavailable');const d=await r.json();debugData=d;$('debugLog').textContent=d.events.length?d.events.map(e=>`#${e.id} · ${(e.ms/1000).toFixed(1)}s · ${e.kind} · ${e.action}\n${e.reason}\nDistance ${e.range_valid?e.distance_mm+' mm':'unavailable'} (raw ${e.raw_distance_mm}, status ${e.range_status}, age ${e.range_age_ms} ms) | Motor power ${e.left}/${e.right} | Encoders ${e.encoder_left_cps}/${e.encoder_right_cps} counts/s\nFault ${e.fault} | Link ${e.connected?'connected':'lost'}, reply ${e.reply_age_ms} ms | Turn ${e.turn} | Generation ${e.generation}\n${motionDebug(e)}`).join('\n\n'):'No events yet. Start an action to record its progress.';$('debugMessage').textContent='Log connected · '+d.events.length+' recent events'}catch(error){$('debugMessage').textContent=error.message+'; last log retained'}finally{debugPending=false}}
async function copyDebugLog(){if(!debugData)return;try{const text=JSON.stringify(debugData,null,2);if(navigator.clipboard&&window.isSecureContext){await navigator.clipboard.writeText(text)}else{const field=document.createElement('textarea');field.value=text;field.style.cssText='position:fixed;left:-10000px';document.body.appendChild(field);field.select();let copied=false;try{copied=document.execCommand('copy')}finally{field.remove()}if(!copied)throw Error('Copy unavailable')}$('debugMessage').textContent='Debug log copied'}catch(error){$('debugMessage').textContent='Clipboard unavailable. Select and copy the log below.'}}
$('actionDebug').addEventListener('toggle',refreshDebug);setInterval(refreshDebug,1000);
async function voiceAction(value){if(voicePending)return;voicePending=true;try{const response=await fetch('/api/voice/setup?action='+value,{method:'POST'});const data=await response.json();if(!response.ok)throw Error(data.message);$('voiceMessage').textContent=data.message}catch(error){$('voiceMessage').textContent=error.message}finally{voicePending=false;await refreshVoice()}}
async function refreshVoice(){try{const response=await fetch('/api/voice/status',{cache:'no-store'});if(!response.ok)throw Error('Unavailable');const s=await response.json();voiceEnabled=s.enabled;$('voiceState').textContent=s.turning?'Turning toward saved voice direction':s.paused?'Paused for vision':s.error||(!s.available?'Direction unavailable':s.enabled?(!s.gyro_ready?'Voice turning on · update Zumo turn files':!s.drive_ready?'Voice turning on · calibrate the surface':'Voice turning enabled'):'Direction ready · turning off');$('voiceAngle').textContent=s.stable?` · ${Math.round(s.degrees)}° at array`+(s.calibrated?` · ${Math.round(Math.abs(s.relative))}° ${s.relative>=0?'left':'right'}`:''):' · waiting for clear speech';$('voiceToggle').textContent=s.enabled?'Disable voice turning':'Enable voice turning';$('voiceToggle').disabled=voicePending;$('voiceResult').textContent=s.turn_reason||(s.result==='NONE'?'':s.result==='DONE'?'Last turn completed':`Last turn: ${s.result}`);if(!s.gyro_ready)$('voiceResult').textContent+=' · Check Zumo connection, updated turn files, and gyro'}catch(error){$('voiceState').textContent='Direction unavailable';$('voiceToggle').disabled=true}}
const driveButtons=[...document.querySelectorAll('[data-drive]')];
const faceButtons=[...document.querySelectorAll('[data-face]')];
let accelerationDirty=false,accelerationSending=false;
async function saveAcceleration(){
  accelerationDirty=true;if(accelerationSending)return;accelerationSending=true;
  try{while(accelerationDirty){accelerationDirty=false;await postZumo('/api/zumo/acceleration?value='+Number($('driveAcceleration').value));}$('zumoMessage').textContent='Acceleration saved for driving and voice turns';}
  catch(error){$('zumoMessage').textContent=error.message;}
  finally{accelerationSending=false;}
}
let facePending=false,faceStopping=false,faceAvailable=false,faceRequest=0;
function updateFaceControls(){for(const button of faceButtons)button.disabled=!faceAvailable||facePending||faceStopping||stopPending||!controlGeneration;$('faceStop').disabled=faceStopping||stopPending;}
async function previewFace(name){
  if(facePending||faceStopping||stopPending||!faceAvailable||!controlGeneration)return;
  const request=++faceRequest;facePending=true;updateFaceControls();
  try{const data=await postZumo('/api/zumo/face?value='+encodeURIComponent(name)+'&generation='+controlGeneration);if(Number.isFinite(data.generation))controlGeneration=Math.max(controlGeneration,data.generation);if(request===faceRequest)$('faceMessage').textContent=name.charAt(0)+name.slice(1).toLowerCase()+': '+data.message;}
  catch(error){if(request===faceRequest)$('faceMessage').textContent=error.message;}
  finally{facePending=false;await refreshActions();updateFaceControls();}
}
async function stopFacePreview(){
  if(faceStopping)return;++faceRequest;faceStopping=true;updateFaceControls();
  try{const data=await postZumo('/api/zumo/face/stop');if(Number.isFinite(data.generation))controlGeneration=Math.max(controlGeneration,data.generation);$('faceMessage').textContent=data.message;}
  catch(error){$('faceMessage').textContent=error.message;}
  finally{faceStopping=false;await refreshActions();await refreshZumo();updateFaceControls();}
}
function speeds(direction){const value=Number($('driveSpeed').value);return {forward:[value,value],back:[-value,-value],left:[-value,value],right:[value,-value]}[direction]||[0,0]}
async function postZumo(path){const response=await fetch(path,{method:'POST',cache:'no-store'});const data=await response.json();if(!response.ok)throw Error(data.message||'Zumo command failed');return data}
async function stopZumo(hard=false,source='stop-button'){hard=hard===true;++faceRequest;heldDirection=null;if(driveTimer!==null){clearInterval(driveTimer);driveTimer=null}if((stopPending&&!hard)||hardStopPending)return;stopPending=true;if(hard)hardStopPending=true;++stopRequests;try{const s=await postZumo('/api/zumo/stop?source='+encodeURIComponent(source)+(hard?'&hard=1':''));controlGeneration=Math.max(controlGeneration,s.generation);$('routineMessage').textContent=s.message}catch(error){$('zumoMessage').textContent=error.message}finally{--stopRequests;stopPending=stopRequests>0;if(hard)hardStopPending=false;await refreshActions()}}
async function sendDrive(){if(!heldDirection||driveInFlight||stopPending)return;driveInFlight=true;const [left,right]=speeds(heldDirection);try{const s=await postZumo(`/api/zumo/drive?left=${left}&right=${right}&acceleration=${Number($('driveAcceleration').value)}&generation=${controlGeneration}`);if(s.generation)controlGeneration=Math.max(controlGeneration,s.generation)}catch(error){$('zumoMessage').textContent=error.message;await stopZumo(true,'drive-error')}finally{driveInFlight=false}}
function beginDrive(direction,event){if(!zumoReady||heldDirection||stopPending||!controlGeneration)return;event.preventDefault();event.currentTarget.setPointerCapture(event.pointerId);heldDirection=direction;sendDrive();driveTimer=setInterval(sendDrive,100)}
for(const button of driveButtons){button.addEventListener('pointerdown',event=>beginDrive(button.dataset.drive,event));button.addEventListener('pointerup',()=>{if(heldDirection)return stopZumo(false,'release')});button.addEventListener('pointercancel',()=>{if(heldDirection)return stopZumo(true,'pointer-cancel')});button.addEventListener('lostpointercapture',()=>{if(heldDirection)return stopZumo(true,'capture-lost')})}
$('zumoStop').addEventListener('click',stopZumo);
async function toggleVision(){if(visionPending)return;visionPending=true;$('visionToggle').disabled=true;try{const response=await fetch('/api/vision?enabled='+(visionEnabled?'0':'1'),{method:'POST',cache:'no-store'});const data=await response.json();if(!response.ok)throw Error(data.message||'Could not change person detection');$('message').textContent=data.message;$('message').className=''}catch(error){$('message').textContent=error.message;$('message').className='error'}finally{visionPending=false;await refresh()}}
async function toggleBattery(){if(batteryPending||stopPending||!Number.isFinite(controlGeneration)||!controlGeneration)return;batteryNotice='';batteryPending=true;$('batteryToggle').disabled=true;try{const response=await fetch('/api/vision/objects?enabled='+(batteryEnabled?'0':'1')+'&generation='+controlGeneration,{method:'POST',cache:'no-store'});const data=await response.json();if(Number.isFinite(data.generation))controlGeneration=Math.max(controlGeneration,data.generation);if(!response.ok)throw Error(data.message||'Could not change object detection');$('message').textContent=data.message;$('message').className=''}catch(error){batteryNotice=error.message;$('message').textContent=error.message;$('message').className='error'}finally{batteryPending=false;await refresh()}}
let driveSpeedDirty=false;
$('driveSpeed').addEventListener('input',event=>{driveSpeedDirty=true;$('driveSpeedValue').textContent=event.target.value});
$('driveSpeed').addEventListener('change',async()=>{try{const response=await fetch('/api/zumo/speed?value='+Number($('driveSpeed').value),{method:'POST'});if(!response.ok)throw Error('Could not save drive speed');driveSpeedDirty=false}catch(error){$('zumoMessage').textContent=error.message}});
$('driveAcceleration').addEventListener('input',event=>{$('driveAccelerationValue').textContent=event.target.value;accelerationDirty=true;});
$('driveAcceleration').addEventListener('change',saveAcceleration);
$('zumoCalibrate').addEventListener('click',async()=>{try{await postZumo('/api/zumo/calibrate');$('zumoMessage').textContent='Calibrating surface...'}catch(error){$('zumoMessage').textContent=error.message}});
for(const button of faceButtons){button.addEventListener('click',()=>previewFace(button.dataset.face))}
function ledPath(off){const hex=off?'000000':$('ledColor').value.slice(1);const target=$('ledTarget').value;const brightness=off?0:$('ledBrightness').value;return `/api/zumo/led?target=${target}&r=${parseInt(hex.slice(0,2),16)}&g=${parseInt(hex.slice(2,4),16)}&b=${parseInt(hex.slice(4,6),16)}&brightness=${brightness}`}
$('ledBrightness').addEventListener('input',event=>{$('ledBrightnessValue').textContent=event.target.value});
for(const [id,off] of [['ledApply',false],['ledOff',true]]){$(id).addEventListener('click',async()=>{try{await postZumo(ledPath(off));$('ledResult').textContent='Sending...'}catch(error){$('ledResult').textContent=error.message}})}
window.addEventListener('blur',()=>{if(heldDirection)return stopZumo(true,'blur')});document.addEventListener('visibilitychange',()=>{if(document.hidden&&heldDirection)return stopZumo(true,'hidden')});
let distanceTimer=null,zumoRefreshPending=false;
function updateDistance(s){
  if(distanceTimer!==null){clearTimeout(distanceTimer);distanceTimer=null;}
  const badge=$('distanceBadge');
  const fresh=s?.connected&&s.range_valid&&Number.isFinite(s.distance_mm)&&s.distance_mm>0&&Number.isFinite(s.range_age_ms)&&s.range_age_ms<250;
  const noReturn=s?.connected&&s.range_no_return&&Number.isFinite(s.range_age_ms)&&s.range_age_ms<250;
  badge.className='distance-badge'+(fresh||noReturn?'':' unavailable');
  badge.textContent=fresh?`ToF: ${(s.distance_mm/10).toFixed(1)} cm`:noReturn?'ToF: no return':!s?.connected?'ToF: offline':s.range_age_ms>=250?'ToF: stale':'ToF: unavailable';
  // A stalled request must not leave the last number looking live indefinitely.
  if(fresh||noReturn)distanceTimer=setTimeout(()=>{badge.textContent='ToF: stale';badge.className='distance-badge unavailable';distanceTimer=null;},1500);
}
async function refreshZumo(){if(zumoRefreshPending)return;zumoRefreshPending=true;try{const response=await fetch('/api/zumo/status',{cache:'no-store'});if(!response.ok)throw Error('Status unavailable');const s=await response.json();updateDistance(s);$('debugSensors').textContent=`Distance: ${s.range_valid?s.distance_mm+' mm':'unavailable'} · raw ${s.raw_distance_mm??'—'} mm · sensor status ${s.range_status??'—'} · age ${s.range_age_ms} ms | Encoders L/R: ${s.encoder_left_cps??0}/${s.encoder_right_cps??0} counts/s · feedback ${s.encoder_control?'on':'off'} | Controller: ${s.fault} · reply ${s.reply_age_ms} ms\n${motionDebug(s)}`;zumoReady=s.connected&&s.ready&&!s.calibrating;if(Number.isFinite(s.acceleration)&&!accelerationDirty&&!accelerationSending){$('driveAcceleration').value=s.acceleration;$('driveAccelerationValue').textContent=s.acceleration;}$('zumoLink').textContent=s.connected?'Zumo: connected':'Zumo: disconnected';$('zumoReady').textContent=s.calibrating?'Drive: calibrating':zumoReady?'Drive: ready':'Drive: locked';$('zumoBattery').textContent=s.battery_mv>=0?`Battery: ${(s.battery_mv/1000).toFixed(2)} V`:'Battery: --';$('zumoFault').textContent='Fault: '+s.fault;$('ledApply').disabled=$('ledOff').disabled=!s.connected;$('ledResult').textContent=s.connected?(s.led_result==='APPLIED'?'Zumo acknowledged':s.led_result==='SENDING'?'Sending...':s.led_result==='NONE'?'--':s.led_result):'Disconnected';for(const button of driveButtons)button.disabled=!zumoReady;$('zumoCalibrate').disabled=!s.connected||s.calibrating||!!heldDirection;faceAvailable=s.connected&&!s.calibrating&&!s.left&&!s.right&&!s.look_active&&!s.manual_driving&&!heldDirection&&!actionActive;updateFaceControls();if(!zumoReady&&heldDirection)stopZumo(true,'status-locked');let message='Waiting for Zumo connection...';if(s.connected){if(s.calibrating)message='Calibrating surface...';else if(s.fault!=='NONE')message='Zumo fault: '+s.fault;else if(s.last_error!=='NONE')message=s.last_error;else if(zumoReady)message='Hold a direction to drive.';else message='Connected. Calibrate surface on a safe surface to enable driving.'}$('zumoMessage').textContent=message}catch(error){updateDistance(null);zumoReady=false;faceAvailable=false;updateFaceControls();for(const button of [...driveButtons,...faceButtons])button.disabled=true;$('ledApply').disabled=$('ledOff').disabled=true;$('ledResult').textContent='Unavailable';$('zumoLink').textContent='Zumo: unavailable';$('zumoMessage').textContent='Zumo status unavailable';if(heldDirection)stopZumo(true,'status-offline')}finally{zumoRefreshPending=false}}
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
function freshVision(s){return (s.vision_age_ms??0)<=1200&&(s.vision_capture_age_ms??0)<=1200}
function drawBoxes(v){
  const ctx=$('boxes').getContext('2d');ctx.clearRect(0,0,640,480);ctx.setLineDash?.([]);ctx.lineWidth=4;ctx.font='bold 18px system-ui';
  if(!v?.vision_enabled||!v.boxes||!freshVision(v))return;const sx=640/(v.vision_width||320),sy=480/(v.vision_height||240);for(const b of v.boxes){const label=v.battery_mode?(b.label||v.object_classes?.[b.class_id]||'Object'):'Person';ctx.strokeStyle=v.battery_mode?(['#ffcf45','#38e4ff','#ff74d4'][b.class_id]||'#ffcf45'):'#36e4ff';ctx.fillStyle=ctx.strokeStyle;const x=b.x1*sx,y=b.y1*sy,w=(b.x2-b.x1)*sx,h=(b.y2-b.y1)*sy;ctx.strokeRect(x,y,w,h);ctx.fillText(`${label} ${b.score}%`,x+4,Math.max(22,y-7))}
}
let refreshPending=false;
async function refresh(){
  if(refreshPending)return;
  refreshPending=true;
  try{
    const response=await fetch('/api/status',{cache:'no-store'});if(!response.ok)throw new Error('Status HTTP '+response.status);const s=await response.json();
    cameraReady=s.camera_ready;visionEnabled=s.vision_enabled&&!s.battery_mode;batteryEnabled=s.vision_enabled&&!!s.battery_mode;$('visionToggle').disabled=!s.vision_ready||visionPending||batteryPending;$('visionToggle').textContent=visionEnabled?'Stop person detection':'Start person detection';$('state').textContent=s.audio_state;const rate=s.vision_interval_ms>0&&s.vision_age_ms<2000?` · ${(1000/s.vision_interval_ms).toFixed(1)} fps`:' · measuring';$('camera').textContent=cameraReady?((s.vision_ready||s.battery_ready)?(s.vision_enabled?'LIVE · 640×480 · '+(s.battery_mode?'Object':'Person')+' detection'+rate:'LIVE · 640×480 · Vision off'):'LIVE · 640×480 · Vision unavailable'):s.camera_error;$('personBadge').hidden=!visionEnabled||!s.person_detected||!freshVision(s);drawBoxes(s);
    $('batteryToggle').disabled=!s.battery_ready||batteryPending||visionPending||stopPending;$('batteryToggle').textContent=batteryEnabled?'Stop object test':'Test object model';const objectSummary=(s.boxes||[]).map(b=>(b.label||s.object_classes?.[b.class_id]||'Object')+' '+b.score+'%').join(' · ');$('batteryState').textContent=batteryNotice||(!s.battery_ready?'Object model unavailable':!batteryEnabled?'Battery, Marker, Screwdriver · camera test only':s.vision_error||((s.vision_age_ms<2000?(objectSummary||'No objects detected'):'Waiting for object result')+' · '+s.vision_inference_ms+' ms'));
    $('wakeState').textContent=!s.wake_ready?'Wake unavailable':s.wake_paused?'Paused for vision':s.wake_reacting?'Listening · wake reaction':'Listening for Digit';
    $('wakeLast').textContent=s.wake_detections?` · ${s.wake_detections} heard · last ${Math.floor(s.wake_age_ms/1000)}s ago (${Math.round(s.wake_score*100)}%)`:'';
    $('wakeResult').textContent=s.wake_error||(s.wake_zumo==='NONE'?'':({'APPLIED':'Zumo acknowledged the reaction','SENDING':'Sending reaction to Zumo...','NONE':'','DISCONNECTED':'Zumo disconnected · speaker reaction remains available','BUSY':'Zumo busy · face and lights skipped','CANCELLED':'Zumo reaction cancelled','NO_REPLY':'No wake acknowledgement from Zumo · check its updated files','UNKNOWN_COMMAND':'Update the Zumo controller for wake reactions'}[s.wake_zumo]||s.wake_zumo||''));
    $('wakeTest').disabled=s.wake_paused||s.wake_reacting||(!s.audio_available&&!s.wake_zumo_connected);
    $('memory').textContent=formatBytes(s.psram_free);$('network').textContent=s.wifi_ip;
    if(volumeTimer===null&&!volumeSending){$('volume').value=s.speaker_volume;$('volumeValue').textContent=s.speaker_volume;}
    $('volume').disabled=!s.audio_available;
    updateFaceControls();
    if(s.audio_error){$('message').textContent=s.audio_error;$('message').className='error';}
    else if($('message').className==='error'||$('message').textContent==='Waiting for device status...'){$('message').textContent='Ready';$('message').className='';}
  }catch(error){updateFaceControls();$('wakeState').textContent='Unavailable';$('wakeTest').disabled=true;cameraReady=false;$('visionToggle').disabled=true;$('batteryToggle').disabled=true;$('batteryState').textContent='Object test unavailable · device offline';$('personBadge').hidden=true;$('state').textContent='OFFLINE';$('message').textContent='Device status unavailable';$('message').className='error';}
  finally{refreshPending=false;}
}
refresh();setInterval(refresh,250);
refreshVoice();setInterval(refreshVoice,750);
refreshActions();setInterval(refreshActions,500);
refreshGeminiSettings();setInterval(refreshGeminiSettings,5000);
</script>
</body>
</html>
)HTML";
#endif

void setNoCache(httpd_req_t* request) {
  httpd_resp_set_hdr(request, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(request, "Pragma", "no-cache");
}

esp_err_t sendJson(httpd_req_t* request, const char* json) {
  httpd_resp_set_type(request, "application/json");
  setNoCache(request);
  return httpd_resp_sendstr(request, json);
}

// cJSON owns escaping for action and local recognition status.
// This helper consumes the object on every path.
esp_err_t sendJsonObject(httpd_req_t* request, cJSON* object) {
  if (!object) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "No response memory");
  char* json = cJSON_PrintUnformatted(object);
  cJSON_Delete(object);
  if (!json) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "No response memory");
  const esp_err_t result = sendJson(request, json);
  cJSON_free(json);
  return result;
}

esp_err_t rootHandler(httpd_req_t* request) {
  Serial.printf("[%10lu] Web: GET /\n", static_cast<unsigned long>(millis()));
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
  setNoCache(request);
  return httpd_resp_send(request,
      reinterpret_cast<const char*>(dashboard_html_gz_start),
      dashboard_html_gz_end - dashboard_html_gz_start);
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
      "\"speaker_volume\":%u,\"cue_playing\":%s,"
      "\"camera_ready\":%s,\"camera_error\":\"%s\","
      "\"vision_ready\":%s,\"vision_enabled\":%s,\"person_detected\":%s,"
      "\"person_score_percent\":%u,\"vision_age_ms\":%lu,\"vision_capture_age_ms\":%lu,"
      "\"vision_inference_ms\":%lu,\"vision_decode_ms\":%lu,"
      "\"vision_total_ms\":%lu,\"vision_interval_ms\":%lu,"
      "\"vision_error\":\"%s\","
      "\"psram_free\":%u,\"wifi_connected\":%s,"
      "\"wifi_ip\":\"%s\",\"wifi_rssi\":%ld}",
      AudioEngine::stateName(audio.state), audio.available ? "true" : "false",
      audioError, static_cast<unsigned long>(audio.capturedMilliseconds),
      static_cast<unsigned long>(audio.recordingElapsedMilliseconds),
      static_cast<unsigned long>(app_config::kMaximumRecordingSeconds * 1000UL),
      audio.speakerVolumePercent, audio.cuePlaying ? "true" : "false",
      camera_service::ready() ? "true" : "false", cameraError,
      vision.ready ? "true" : "false",
      vision.enabled ? "true" : "false",
      vision.personDetected ? "true" : "false", vision.personScorePercent,
      static_cast<unsigned long>(vision.resultAgeMs),
      static_cast<unsigned long>(vision.hasResult ? millis()-vision.captureTimestampMs : UINT32_MAX),
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
           "],"
           "\"wake_ready\":%s,\"wake_paused\":%s,\"wake_reacting\":%s,"
           "\"wake_detections\":%lu,\"wake_age_ms\":%lu,\"wake_score\":%.3f,"
           "\"wake_dropped_frames\":%lu,\"wake_inference_us\":%lu,\"wake_mic_peak\":%u,"
           "\"wake_error\":\"%s\",\"wake_zumo\":\"%s\",\"wake_chime\":%s,"
           "\"wake_zumo_connected\":%s}",
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
bool readAcceleration(httpd_req_t* request, const char* key, int& value) {
  char text[8];
  if (!queryValue(request, key, text, sizeof(text)) || !text[0]) return false;
  value = 0;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    value = value * 10 + *p - '0';
    if (value > 6000) return false;
  }
  return value >= 500;
}
esp_err_t controlReply(httpd_req_t* request, const char* message) {
  char escaped[240], json[320];
  escapeJsonString(message, escaped, sizeof(escaped));
  snprintf(json, sizeof(json), "{\"message\":\"%s\",\"generation\":%lu}", escaped,
           (unsigned long)robot_actions::generation());
  return sendJson(request,json);
}

bool geminiRequestOrigin(httpd_req_t* request) {
  char site[24] = {}, origin[128] = {}, host[96] = {}, expected[128] = {};
  const size_t siteLength = httpd_req_get_hdr_value_len(request, "Sec-Fetch-Site");
  if (siteLength >= sizeof(site)) return false;
  if (siteLength && (httpd_req_get_hdr_value_str(request, "Sec-Fetch-Site", site, sizeof(site)) != ESP_OK ||
                     !strcmp(site, "cross-site"))) return false;
  const size_t originLength = httpd_req_get_hdr_value_len(request, "Origin");
  // Non-browser clients may omit Origin; browsers still need JSON and generation.
  if (!originLength) return true;
  if (originLength >= sizeof(origin) || httpd_req_get_hdr_value_len(request, "Host") >= sizeof(host) ||
      httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK ||
      httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) return false;
  snprintf(expected, sizeof(expected), "http://%s", host);
  return !strcmp(origin, expected);
}

esp_err_t geminiError(httpd_req_t* request, const char* status, const char* message) {
  httpd_resp_set_status(request, status);
  return controlReply(request, message);
}

bool geminiFlatObjectBody(const char* body, size_t size) {
  bool quoted = false, escaped = false, started = false, ended = false;
  unsigned depth = 0;
  for (size_t index = 0; index < size; ++index) {
    const unsigned char value = static_cast<unsigned char>(body[index]);
    if (quoted) {
      if (escaped) escaped = false;
      else if (value == '\\') escaped = true;
      else if (value == '"') quoted = false;
      continue;
    }
    if (value == '"') { if (depth != 1) return false; quoted = true; }
    else if (value == '{') { if (depth || started) return false; depth = 1; started = true; }
    else if (value == '}') { if (depth != 1) return false; depth = 0; ended = true; }
    else if (value == '[' || value == ']') return false;
    else if (!depth && !std::isspace(value)) return false;
  }
  return started && ended && !depth && !quoted;
}

cJSON* geminiRequestJson(httpd_req_t* request) {
  if (!geminiRequestOrigin(request)) {
    geminiError(request, "403 Forbidden", "Use the dashboard on Digit's own address"); return nullptr;
  }
  char contentType[64] = {};
  if (httpd_req_get_hdr_value_len(request, "Content-Type") >= sizeof(contentType) ||
      httpd_req_get_hdr_value_str(request, "Content-Type", contentType, sizeof(contentType)) != ESP_OK) {
    geminiError(request, "415 Unsupported Media Type", "Send application/json"); return nullptr;
  }
  for (char* value = contentType; *value; ++value) *value = static_cast<char>(std::tolower(static_cast<unsigned char>(*value)));
  if (strncmp(contentType, "application/json", 16) || (contentType[16] && contentType[16] != ';')) {
    geminiError(request, "415 Unsupported Media Type", "Send application/json"); return nullptr;
  }
  constexpr size_t kMaxBody = 1024;
  if (request->content_len <= 0 || request->content_len > kMaxBody) {
    geminiError(request, "400 Bad Request", "Send a JSON object of at most 1024 bytes"); return nullptr;
  }
  char body[kMaxBody + 1] = {};
  size_t received = 0;
  while (received < request->content_len) {
    const int count = httpd_req_recv(request, body + received, request->content_len - received);
    if (count <= 0) { geminiError(request, "400 Bad Request", "Incomplete JSON request"); return nullptr; }
    received += count;
  }
  if (strlen(body) != received || strstr(body, "\\u0000") || !geminiFlatObjectBody(body, received)) {
    geminiError(request, "400 Bad Request", "Invalid JSON request"); return nullptr;
  }
  const char* end = nullptr;
  cJSON* json = cJSON_ParseWithLengthOpts(body, received + 1, &end, true);
  if (!cJSON_IsObject(json)) {
    cJSON_Delete(json); geminiError(request, "400 Bad Request", "Send one JSON object"); return nullptr;
  }
  return json;
}

bool geminiFields(cJSON* json, const char* const* allowed, size_t count) {
  for (cJSON* field = json->child; field; field = field->next) {
    bool known = false;
    for (size_t index = 0; index < count; ++index) if (!strcmp(field->string, allowed[index])) known = true;
    if (!known) return false;
    for (cJSON* earlier = json->child; earlier != field; earlier = earlier->next)
      if (!strcmp(earlier->string, field->string)) return false;
  }
  return true;
}

bool geminiGeneration(cJSON* json) {
  const cJSON* value = cJSON_GetObjectItemCaseSensitive(json, "generation");
  if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < 1 ||
      value->valuedouble > UINT32_MAX || value->valuedouble != std::floor(value->valuedouble)) return false;
  return static_cast<uint32_t>(value->valuedouble) == robot_actions::generation();
}

bool geminiKeyField(const cJSON* key) {
  if (!key) return true;
  if (!cJSON_IsString(key)) return false;
  const size_t size = strlen(key->valuestring);
  if (!size) return true;
  if (size < 16 || size > gemini_client::kMaxApiKeyLength) return false;
  for (const char* value = key->valuestring; *value; ++value) {
    const unsigned char character = static_cast<unsigned char>(*value);
    if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.')) return false;
  }
  return true;
}

esp_err_t geminiSettingsHandler(httpd_req_t* request) {
  if (request->method == HTTP_GET) {
    const auto settings = gemini_client::settings();
    cJSON* json = cJSON_CreateObject();
    if (json) {
      cJSON_AddBoolToObject(json, "configured", settings.configured);
      cJSON_AddStringToObject(json, "model", settings.model);
      cJSON_AddBoolToObject(json, "dry_run", settings.dryRun);
      cJSON_AddNumberToObject(json, "idle_timeout_sec", settings.idleTimeoutSec);
      cJSON_AddNumberToObject(json, "generation", robot_actions::generation());
    }
    return sendJsonObject(request, json);
  }
  cJSON* json = geminiRequestJson(request);
  if (!json) return ESP_OK;
  constexpr const char* fields[] = {"generation", "api_key", "model", "dry_run", "remove_key", "idle_timeout_sec"};
  const cJSON* key = cJSON_GetObjectItemCaseSensitive(json, "api_key");
  const cJSON* model = cJSON_GetObjectItemCaseSensitive(json, "model");
  const cJSON* dryRun = cJSON_GetObjectItemCaseSensitive(json, "dry_run");
  const cJSON* remove = cJSON_GetObjectItemCaseSensitive(json, "remove_key");
  const cJSON* timeout = cJSON_GetObjectItemCaseSensitive(json, "idle_timeout_sec");
  const bool valid = geminiFields(json, fields, sizeof(fields) / sizeof(fields[0])) &&
      geminiKeyField(key) &&
      (!model || (cJSON_IsString(model) && (!model->valuestring[0] || !strcmp(model->valuestring, "gemini-3.8-live")))) &&
      (!dryRun || cJSON_IsBool(dryRun)) && (!remove || cJSON_IsBool(remove)) &&
      (!timeout || (cJSON_IsNumber(timeout) && std::isfinite(timeout->valuedouble) &&
                    timeout->valuedouble >= 10 && timeout->valuedouble <= 300 &&
                    timeout->valuedouble == std::floor(timeout->valuedouble))) &&
      !(cJSON_IsTrue(remove) && key && key->valuestring[0]);
  if (!valid) { cJSON_Delete(json); return geminiError(request, "400 Bad Request", "Invalid settings: use the Live model and an idle timeout of 10 to 300 seconds"); }
  robot_actions::Guard control;
  if (!geminiGeneration(json)) { cJSON_Delete(json); return geminiError(request, "409 Conflict", "Settings request cancelled by newer control"); }
  const auto settings = gemini_client::settings();
  robot_actions::stop("Gemini settings changed");
  char error[96] = {};
  const bool saved = gemini_client::configure(key ? key->valuestring : "", model ? model->valuestring : "",
      dryRun ? cJSON_IsTrue(dryRun) : settings.dryRun, cJSON_IsTrue(remove), error, sizeof(error),
      timeout ? static_cast<uint32_t>(timeout->valuedouble) : 0);
  const bool removed = cJSON_IsTrue(remove);
  cJSON_Delete(json);
  if (!saved) return geminiError(request, "400 Bad Request", error[0] ? error : "Could not save Gemini settings");
  return controlReply(request, removed ? "Saved API key removed" : "Gemini settings saved on Digit");
}

esp_err_t geminiLiveHandler(httpd_req_t* request) {
  cJSON* json = geminiRequestJson(request);
  if (!json) return ESP_OK;
  const bool start = !strncmp(request->uri, "/api/gemini/live/start", 22) && (request->uri[22] == 0 || request->uri[22] == '?');
  const bool probe = !strncmp(request->uri, "/api/gemini/live/test", 21) && (request->uri[21] == 0 || request->uri[21] == '?');
  constexpr const char* startFields[] = {"generation", "mode"};
  constexpr const char* controlFields[] = {"generation"};
  const cJSON* mode = cJSON_GetObjectItemCaseSensitive(json, "mode");
  if (!geminiFields(json, start ? startFields : controlFields, start ? 2 : 1) ||
      (start && (!cJSON_IsString(mode) || (strcmp(mode->valuestring, "conversation") && strcmp(mode->valuestring, "test_only"))))) {
    cJSON_Delete(json); return geminiError(request, "400 Bad Request", "Invalid Live request");
  }
  const bool testOnly = start && !strcmp(mode->valuestring, "test_only");
  robot_actions::Guard control;
  if (!geminiGeneration(json)) { cJSON_Delete(json); return geminiError(request, "409 Conflict", "Live request cancelled by newer control"); }
  cJSON_Delete(json);
  if (!start && !probe) {
    robot_actions::stop("Gemini conversation ended");
    return controlReply(request, "Conversation, streaming, audio and pending actions stopped");
  }
  if (!gemini_client::settings().configured) return geminiError(request, "409 Conflict", "Save a Gemini API key first");
  const auto zumo = zumo_link::snapshot();
  if (robot_actions::snapshot().active || wake_word::snapshot().reacting ||
      zumo.manualDriving || zumo.lookActive ||
      zumo.motionActive || zumo.left || zumo.right)
    return geminiError(request, "409 Conflict", "Stop existing robot activity before starting Gemini Live");
  if (!speech_commands::startLive(robot_actions::generation(), probe, testOnly))
    return geminiError(request, "409 Conflict", "Live session busy or unavailable; end the current session first");
  return controlReply(request, probe ? "Connection check requested; microphone/video upload, audio output and actions remain off" :
      testOnly ? "Test-only stream started; robot actions disabled" : "Gemini Live conversation started");
}

esp_err_t actionsDebugHandler(httpd_req_t* request) {
  httpd_resp_set_type(request,"application/json");
  httpd_resp_set_hdr(request,"Cache-Control","no-store");
  if (httpd_resp_send_chunk(request,"{\"events\":[",HTTPD_RESP_USE_STRLEN)!=ESP_OK) return ESP_FAIL;
  const uint32_t newest=robot_actions::latestDebugId();
  bool first=true;
  for (unsigned index=0; index<24 && index<newest; ++index) {
    robot_actions::DebugEvent e;
    if (!robot_actions::debugEvent(newest-index,&e)) continue;
    char reason[256], fault[64], turn[64], motionState[40], motionResult[64], json[1600];
    escapeJsonString(e.reason,reason,sizeof(reason));
    escapeJsonString(e.fault,fault,sizeof(fault));
    escapeJsonString(e.turn,turn,sizeof(turn));
    escapeJsonString(e.motionState,motionState,sizeof(motionState));
    escapeJsonString(e.motionResult,motionResult,sizeof(motionResult));
    snprintf(json,sizeof(json),
      "%s{\"id\":%lu,\"ms\":%lu,\"generation\":%lu,\"kind\":\"%s\",\"action\":\"%s\",\"reason\":\"%s\","
      "\"distance_mm\":%d,\"raw_distance_mm\":%d,\"range_status\":%d,\"range_age_ms\":%lu,\"range_valid\":%s,"
      "\"left\":%d,\"right\":%d,\"encoder_left_cps\":%d,\"encoder_right_cps\":%d,\"encoder_control\":%s,"
      "\"fault\":\"%s\",\"turn\":\"%s\",\"connected\":%s,\"ready\":%s,\"reply_age_ms\":%lu,"
      "\"motion_job\":%lu,\"motion_session\":%lu,\"motion_state\":\"%s\",\"motion_result\":\"%s\","
      "\"target_left_cps\":%d,\"target_right_cps\":%d,\"heading_deci_deg\":%d,"
      "\"control_max_gap_ms\":%lu,\"control_overruns\":%lu,\"display_max_slice_us\":%lu}",
      first?"":",",(unsigned long)e.id,(unsigned long)e.timestampMs,(unsigned long)e.generation,e.kind,
      robot_actions::name(e.command),reason,e.distanceMm,e.rawDistanceMm,e.rangeStatus,(unsigned long)e.rangeAgeMs,e.rangeValid?"true":"false",
      e.left,e.right,e.encoderLeft,e.encoderRight,e.encoderControl?"true":"false",fault,turn,
      e.connected?"true":"false",e.ready?"true":"false",(unsigned long)e.replyAgeMs,
      (unsigned long)e.motionJob,(unsigned long)e.motionSession,motionState,motionResult,
      e.targetLeftCps,e.targetRightCps,e.headingDeciDeg,
      (unsigned long)e.controlMaxGapMs,(unsigned long)e.controlOverruns,(unsigned long)e.displayMaxSliceUs);
    if (httpd_resp_send_chunk(request,json,HTTPD_RESP_USE_STRLEN)!=ESP_OK) return ESP_FAIL;
    first=false;
  }
  if (httpd_resp_send_chunk(request,"]}",HTTPD_RESP_USE_STRLEN)!=ESP_OK) return ESP_FAIL;
  return httpd_resp_send_chunk(request,nullptr,0);
}

esp_err_t actionsStatusHandler(httpd_req_t* request) {
  const auto action = robot_actions::snapshot();
  const auto speech = speech_commands::snapshot();
  const auto localStop = local_stop::snapshot();
  const auto z = zumo_link::snapshot();
  const char* movementReason=robot_actions::unavailableReason(robot_actions::Command::Forward);
  const bool driveReady=!movementReason[0];
  cJSON* json = cJSON_CreateObject();
  if (!json) return sendJsonObject(request, json);
  cJSON_AddStringToObject(json, "action", robot_actions::name(action.command));
  cJSON_AddStringToObject(json, "phase", action.phase);
  cJSON_AddBoolToObject(json, "active", action.active);
  cJSON_AddNumberToObject(json, "generation", action.generation);
  cJSON_AddBoolToObject(json, "voice_ready", speech.ready);
  cJSON_AddBoolToObject(json, "listening", speech.listening);
  cJSON_AddStringToObject(json, "voice_error", speech.error);
  cJSON_AddNumberToObject(json, "recognized", speech.recognized);
  cJSON_AddNumberToObject(json, "last_command", speech.lastCommand);
  cJSON_AddNumberToObject(json, "score", speech.score);
  cJSON_AddBoolToObject(json, "drive_ready", driveReady);
  cJSON_AddBoolToObject(json, "range_valid", z.rangeValid);
  cJSON_AddNumberToObject(json,"drive_speed",robot_actions::driveSpeed());
  cJSON_AddStringToObject(json,"movement_reason",movementReason);
  cJSON_AddBoolToObject(json,"distance_calibrated",z.distanceCalibrated);
  cJSON_AddBoolToObject(json,"task_active",speech.taskActive);
  cJSON_AddNumberToObject(json,"task_moves",speech.taskMoves);
  cJSON_AddStringToObject(json,"task_goal",speech.taskGoal);
  cJSON_AddStringToObject(json,"task_phase",speech.taskPhase);
  cJSON_AddStringToObject(json,"task_result",speech.taskResult);
  cJSON_AddNumberToObject(json, "distance_mm", z.distanceMm);
  cJSON_AddStringToObject(json, "voice_result", speech.result);
  cJSON_AddBoolToObject(json, "live_active", speech.liveActive);
  cJSON_AddBoolToObject(json, "live_connected", speech.liveConnected);
  cJSON_AddBoolToObject(json, "live_connecting", speech.liveConnecting);
  cJSON_AddNumberToObject(json, "live_connections", speech.liveConnectionCount);
  cJSON_AddBoolToObject(json, "live_speaking", speech.liveSpeaking);
  cJSON_AddBoolToObject(json, "live_probe", speech.liveProbe);
  cJSON_AddBoolToObject(json, "live_dry_run", speech.dryRun);
  cJSON_AddNumberToObject(json, "live_latency_ms", speech.latencyMs);
  cJSON_AddNumberToObject(json, "live_sent_samples", speech.liveSentSamples);
  cJSON_AddNumberToObject(json, "live_received_samples", speech.liveReceivedSamples);
  cJSON_AddNumberToObject(json, "live_played_samples", speech.livePlayedSamples);
  cJSON_AddNumberToObject(json, "live_video_frames", speech.liveVideoFrames);
  cJSON_AddNumberToObject(json, "live_video_bytes", speech.liveVideoBytes);
  cJSON_AddNumberToObject(json, "live_video_capture_age_ms", speech.liveVideoCapturedAt ? millis() - speech.liveVideoCapturedAt : UINT32_MAX);
  cJSON_AddNumberToObject(json, "live_idle_remaining_ms", speech.idleRemainingMs);
  cJSON_AddStringToObject(json, "live_transcript", speech.transcript);
  cJSON_AddStringToObject(json, "live_response", speech.liveResponse);
  cJSON_AddStringToObject(json, "live_reason", speech.reason);
  cJSON_AddBoolToObject(json, "local_stop_ready", localStop.ready);
  cJSON_AddBoolToObject(json, "local_stop_active", localStop.active);
  cJSON_AddNumberToObject(json, "local_stop_detections", localStop.detections);
  cJSON_AddStringToObject(json, "local_stop_error", localStop.error);
  cJSON_AddNumberToObject(json, "command_audio_ms", speech.samples / 16);
  cJSON_AddNumberToObject(json, "command_chunks", speech.chunks);
  cJSON_AddNumberToObject(json, "command_mic_peak", speech.micPeak);
  cJSON_AddNumberToObject(json, "command_inference_us", speech.inferenceUs);
  cJSON_AddNumberToObject(json, "candidate_command", speech.candidateCommand);
  cJSON_AddNumberToObject(json, "candidate_score", speech.candidateScore);
  cJSON_AddNumberToObject(json, "command_feed_chunks", speech.feedChunks);
  cJSON_AddNumberToObject(json, "command_fetch_chunks", speech.fetchChunks);
  cJSON_AddNumberToObject(json, "command_model_peak", speech.modelPeak);
  cJSON_AddNumberToObject(json, "audio_total_feeds", speech.totalFeeds);
  cJSON_AddNumberToObject(json, "audio_total_fetches", speech.totalFetches);
  cJSON_AddNumberToObject(json, "command_dropped_frames", speech.droppedFrames);
  cJSON_AddStringToObject(json, "command_decoder_text", speech.decoderText);
  return sendJsonObject(request, json);
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
bool readDriveSpeed(httpd_req_t* request, const char* key, int& speed) {
  char text[12];
  if (!queryValue(request,key,text,sizeof(text)) || !text[0] || strspn(text,"0123456789")!=strlen(text) || strlen(text)>4) return false;
  speed=atoi(text);
  return speed>=3000 && speed<=6000;
}
esp_err_t driveSpeedHandler(httpd_req_t* request) {
  int speed;
  if (!readDriveSpeed(request,"value",speed)) {
    httpd_resp_set_status(request,"400 Bad Request");
    return controlReply(request,"Choose a drive speed from 3000 to 6000");
  }
  robot_actions::setDriveSpeed(speed);
  return controlReply(request,"Manual drive speed saved");
}

esp_err_t actionsHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  char value[8];
  if (!queryValue(request,"command",value,sizeof(value)) || strcmp(value,"7")) {
    httpd_resp_set_status(request,"410 Gone"); return controlReply(request,"Fixed routines retired; ask Gemini for a task");
  }
  robot_actions::stop("Dashboard Stop"); return controlReply(request,"Stopped");
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
  char error[160], result[48], turnReason[200], json[1024];
  escapeJsonString(direction.error, error, sizeof(error));
  escapeJsonString(zumo.lookResult, result, sizeof(result));
  escapeJsonString(wake.turnReason, turnReason, sizeof(turnReason));
  snprintf(json, sizeof(json),
           "{\"fixed_orientation\":true,\"front_degrees\":%.0f,\"left_degrees\":%.0f,"
           "\"available\":%s,\"stable\":%s,\"front_set\":%s,\"calibrated\":%s,"
           "\"enabled\":%s,\"degrees\":%.1f,\"relative\":%.1f,\"age_ms\":%lu,"
           "\"source\":\"%s\",\"read_resource\":%u,\"read_command\":%u,\"read_status\":%u,"
           "\"error\":\"%s\",\"turning\":%s,\"result\":\"%s\","
           "\"drive_ready\":%s,\"gyro_ready\":%s,\"paused\":%s,\"turn_reason\":\"%s\"}",
           voice_mount::frontDegrees, voice_mount::leftDegrees,
           direction.available ? "true" : "false", direction.stable ? "true" : "false",
           direction.frontSet ? "true" : "false", direction.calibrated ? "true" : "false",
           direction.enabled ? "true" : "false", direction.degrees, direction.relative,
           (unsigned long)direction.ageMs, direction.source,
           direction.readResource, direction.readCommand, direction.readStatus, error,
           zumo.lookActive ? "true" : "false", result,
           zumo.connected && zumo.ready && !zumo.calibrating ? "true" : "false",
           zumo.connected && zumo.voiceCapable && zumo.rampedLookCapable ? "true" : "false",
           wake.pausedForVision ? "true" : "false", turnReason);
  return sendJson(request, json);
}

esp_err_t voiceSetupHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  char action[8];
  if (!queryValue(request, "action", action, sizeof(action)) ||
      (strcmp(action, "on") &&
       strcmp(action, "off") && strcmp(action, "reset"))) {
    httpd_resp_set_status(request, "400 Bad Request");
    return sendJson(request, "{\"message\":\"Choose on, off, or reset. Direction is fixed at front 270 degrees, left 0 degrees.\"}");
  }
  if (!voice_direction::configure(action)) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"Voice turning setting unavailable.\"}");
  }
  const char* message = !strcmp(action, "on") ? "Voice turning enabled and saved. Front is 270 degrees; left is 0/360 degrees. Calibrate the surface before movement." :
      !strcmp(action, "off") ? "Voice turning disabled and saved. Wake reactions still work." : "Fixed direction restored: front 270 degrees, left 0/360 degrees. Your on/off choice is unchanged.";
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
  char fault[48], lastError[64], motionState[40], motionResult[64];
  escapeJsonString(zumo.fault, fault, sizeof(fault));
  escapeJsonString(zumo.lastError, lastError, sizeof(lastError));
  escapeJsonString(zumo.motionState, motionState, sizeof(motionState));
  escapeJsonString(zumo.motionResult, motionResult, sizeof(motionResult));
  char json[1900];
  snprintf(json, sizeof(json),
           "{\"connected\":%s,\"ready\":%s,\"calibrating\":%s,"
           "\"edge\":%s,\"left\":%d,\"right\":%d,\"battery_mv\":%d,"
           "\"reply_age_ms\":%lu,\"fault\":\"%s\",\"last_error\":\"%s\",\"led_result\":\"%s\",\"brake_supported\":%s,"
           "\"look_active\":%s,\"manual_driving\":%s,\"turn_ramp_supported\":%s,\"wiggle_supported\":%s,\"acceleration\":%d,"
           "\"scene_supported\":%s,\"auto_ramp_supported\":%s,\"range_age_ms\":%lu,\"close_approach_supported\":%s,\"chase_close_supported\":%s,"
           "\"distance_mm\":%d,\"range_valid\":%s,\"range_no_return\":%s,\"range_status\":%d,\"raw_distance_mm\":%d,"
           "\"encoder_control\":%s,\"encoder_left_cps\":%d,\"encoder_right_cps\":%d,"
           "\"motion_supported\":%s,\"motion_armed\":%s,\"motion_active\":%s,"
           "\"motion_boot\":%lu,\"motion_session\":%lu,\"motion_job\":%lu,"
           "\"motion_state\":\"%s\",\"motion_result\":\"%s\",\"motion_distance_mm\":%d,"
           "\"motion_range_age_ms\":%lu,\"motion_sample\":%lu,\"heading_deci_deg\":%d,"
           "\"target_left_cps\":%d,\"target_right_cps\":%d,\"control_max_gap_ms\":%lu,"
           "\"control_overruns\":%lu,\"display_max_slice_us\":%lu,\"distance_calibrated\":%s,\"travelled_mm\":%d,\"encoder_left_count\":%d,\"encoder_right_count\":%d,\"expression_supported\":%s}",
           zumo.connected ? "true" : "false", zumo.ready ? "true" : "false",
           zumo.calibrating ? "true" : "false", zumo.edge ? "true" : "false",
           zumo.left, zumo.right, zumo.batteryMillivolts,
           static_cast<unsigned long>(zumo.replyAgeMs), fault, lastError,
           zumo.ledResult, zumo.brakeCapable ? "true" : "false",
           zumo.lookActive ? "true" : "false", zumo.manualDriving ? "true" : "false",
           zumo.rampedLookCapable ? "true" : "false", zumo.wiggleCapable ? "true" : "false", zumo_link::acceleration(),
           zumo.sceneCapable ? "true" : "false", zumo.autoRampCapable ? "true" : "false", (unsigned long)zumo.rangeAgeMs,
           zumo.closeApproachCapable ? "true" : "false", zumo.chaseCloseCapable ? "true" : "false",
           zumo.distanceMm, zumo.rangeValid ? "true" : "false", zumo.rangeNoReturn ? "true" : "false", zumo.rangeStatus, zumo.rawDistanceMm,
           zumo.encoderControl ? "true" : "false", zumo.encoderLeftCps, zumo.encoderRightCps,
           zumo.motionCapable ? "true" : "false",zumo.motionArmed ? "true" : "false",zumo.motionActive ? "true" : "false",
           (unsigned long)zumo.motionBoot,(unsigned long)zumo.motionSession,(unsigned long)zumo.motionJob,
           motionState,motionResult,zumo.motionDistanceMm,(unsigned long)zumo.motionRangeAgeMs,
           (unsigned long)zumo.motionSample,zumo.motionHeadingDeciDeg,zumo.targetLeftCps,zumo.targetRightCps,
           (unsigned long)zumo.controlMaxGapMs,(unsigned long)zumo.controlOverruns,(unsigned long)zumo.displayMaxSliceUs,
           zumo.distanceCalibrated?"true":"false",zumo.motionTravelledMm,zumo.encoderLeftCount,zumo.encoderRightCount,zumo.expressionCapable?"true":"false");
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
  robot_actions::manualOverride();
  if (!zumo_link::drive(left, right, acceleration)) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"Zumo is disconnected or not calibrated\"}");
  }
  return controlReply(request, "Drive command accepted");
}

esp_err_t accelerationHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  int value;
  if (!readAcceleration(request, "value", value) || !zumo_link::setAcceleration(value)) {
    httpd_resp_set_status(request, "400 Bad Request");
    return controlReply(request, "Use acceleration from 500 to 6000");
  }
  return controlReply(request, "Acceleration saved for driving and voice turns");
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
  char source[48] = {};
  queryValue(request,"source",source,sizeof(source));
  const char* reason = "Dashboard Stop (legacy or unspecified request)";
  if (!strcmp(source,"stop-all")) reason="Dashboard STOP ALL button";
  else if (!strcmp(source,"stop-button")) reason="Dashboard Stop button";
  else if (!strcmp(source,"release")) reason="Manual drive button released";
  else if (!strcmp(source,"blur")) reason="Manual drive stopped: browser lost focus";
  else if (!strcmp(source,"hidden")) reason="Manual drive stopped: dashboard hidden";
  else if (!strcmp(source,"pointer-cancel")) reason="Manual drive stopped: pointer cancelled";
  else if (!strcmp(source,"capture-lost")) reason="Manual drive stopped: pointer capture lost";
  else if (!strcmp(source,"drive-error")) reason="Manual drive stopped: request failed";
  else if (!strcmp(source,"status-locked")) reason="Manual drive stopped: controller locked";
  else if (!strcmp(source,"status-offline")) reason="Manual drive stopped: status unavailable";
  robot_actions::stop(reason, smooth);
  // The dedicated UART task dispatches cancellation on its next tick.
  // HTTP must never race that task while receiving or writing a packet.
  return controlReply(request, !smooth ? "All actions cancelled; stopping motors" :
      supported ? "All actions cancelled; slowing to a stop" :
      "All actions cancelled; smooth stopping needs the updated Zumo file");
}

esp_err_t zumoCalibrateHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  robot_actions::manualOverride();
  if (!zumo_link::calibrate()) {
    httpd_resp_set_status(request, "409 Conflict");
    return sendJson(request, "{\"message\":\"Zumo must be connected and stopped\"}");
  }
  return sendJson(request, "{\"message\":\"Floor calibration requested\"}");
}

esp_err_t zumoFaceHandler(httpd_req_t* request) {
  robot_actions::Guard control; char value[12];
  if (!queryValue(request,"value",value,sizeof(value)) || !zumo_link::isFaceName(value)) return geminiError(request,"400 Bad Request","Unknown expression");
  if (!currentGeneration(request)) return geminiError(request,"409 Conflict","Expression request cancelled by Stop or manual control");
  robot_actions::manualOverride();
  if (!robot_actions::setExpression(value,3000)) return geminiError(request,"409 Conflict","Face unavailable or safety stop has priority");
  return controlReply(request,"Silent expression requested");
}

esp_err_t zumoFaceStopHandler(httpd_req_t* request) {
  robot_actions::Guard control;
  robot_actions::stop("Expression preview stopped");
  const bool reset = zumo_link::face("NEUTRAL");
  return controlReply(request, reset ? "Preview stopped; neutral face requested" :
                                     "Audio stopped; neutral face needs Digit connected and stopped");
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
  const TickType_t framePeriod = pdMS_TO_TICKS(1000 / app_config::kTargetCameraFps);
  bool streamStarted = false;
  uint8_t* jpeg = static_cast<uint8_t*>(heap_caps_malloc(
      camera_service::kMaxJpegBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  const auto failStream = [request, &streamStarted, &jpeg](const char* message, esp_err_t error) {
    if (jpeg) { heap_caps_free(jpeg); jpeg = nullptr; }
    Serial.println(message);
    if (streamStarted) return error;
    httpd_resp_set_status(request, "503 Service Unavailable");
    httpd_resp_set_type(request, "text/plain");
    return httpd_resp_sendstr(request, message);
  };
  if (!jpeg) return failStream("Camera: insufficient PSRAM for streaming", ESP_ERR_NO_MEM);
  while (true) {
    const TickType_t frameStartedAt = xTaskGetTickCount();
    camera_service::Frame frame;
    if (!camera_service::copyLatestJpeg(jpeg, camera_service::kMaxJpegBytes, &frame) ||
        !frame.size || frame.size > camera_service::kMaxJpegBytes) {
      return failStream("Camera: fresh streaming frame unavailable", ESP_FAIL);
    }
    // The broker lock and camera framebuffer are released before socket writes.
    char partHeader[72];
    const int headerLength = snprintf(partHeader, sizeof(partHeader),
        kStreamPartHeader, static_cast<unsigned int>(frame.size));
    esp_err_t result = httpd_resp_send_chunk(request, kStreamBoundary, strlen(kStreamBoundary));
    if (result == ESP_OK) {
      streamStarted = true;
      result = httpd_resp_send_chunk(request, partHeader, headerLength);
    }
    if (result == ESP_OK) result = httpd_resp_send_chunk(request,
        reinterpret_cast<const char*>(jpeg), frame.size);
    if (result != ESP_OK) { heap_caps_free(jpeg); return result; }
    const TickType_t elapsed = xTaskGetTickCount() - frameStartedAt;
    if (elapsed < framePeriod) vTaskDelay(framePeriod - elapsed);
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
  if (controlServer && streamServer) return true;
  if (!controlServer) {
  httpd_config_t controlConfig = HTTPD_DEFAULT_CONFIG();
  controlConfig.server_port = app_config::kControlServerPort;
  controlConfig.ctrl_port = 32768;
  controlConfig.max_uri_handlers = 44;
  controlConfig.stack_size = 8192;
  controlConfig.lru_purge_enable = true;

  const esp_err_t controlError = httpd_start(&controlServer, &controlConfig);
  if (controlError != ESP_OK) {
    controlServer = nullptr;
    Serial.printf("Web: control server failed: %s\n", esp_err_to_name(controlError));
    return false;
  }

  const bool controlRoutesReady =
      registerUri(controlServer, "/", HTTP_GET, rootHandler) &&
      registerUri(controlServer, "/api/status", HTTP_GET, statusHandler) &&
      registerUri(controlServer, "/api/actions", HTTP_GET, actionsStatusHandler) &&
      registerUri(controlServer, "/api/gemini/settings", HTTP_GET, geminiSettingsHandler) &&
      registerUri(controlServer, "/api/gemini/settings", HTTP_POST, geminiSettingsHandler) &&
      registerUri(controlServer, "/api/gemini/live/start", HTTP_POST, geminiLiveHandler) &&
      registerUri(controlServer, "/api/gemini/live/test", HTTP_POST, geminiLiveHandler) &&
      registerUri(controlServer, "/api/gemini/live/stop", HTTP_POST, geminiLiveHandler) &&
      registerUri(controlServer, "/api/actions/debug", HTTP_GET, actionsDebugHandler) &&
      registerUri(controlServer, "/api/audio/processing", HTTP_GET, audioProcessingHandler) &&
      registerUri(controlServer, "/api/actions", HTTP_POST, actionsHandler) &&
      registerUri(controlServer, "/api/zumo/speed", HTTP_POST, driveSpeedHandler) &&
      registerUri(controlServer, "/api/wake/test", HTTP_POST, wakeTestHandler) &&
      registerUri(controlServer, "/api/voice/status", HTTP_GET, voiceStatusHandler) &&
      registerUri(controlServer, "/api/voice/setup", HTTP_POST, voiceSetupHandler) &&
      registerUri(controlServer, "/api/vision", HTTP_POST, visionHandler) &&
      registerUri(controlServer, "/api/vision/battery", HTTP_POST, batteryVisionHandler) &&
      registerUri(controlServer, "/api/vision/objects", HTTP_POST, batteryVisionHandler) &&
      registerUri(controlServer, "/api/volume", HTTP_POST, volumeHandler) &&
      registerUri(controlServer, "/api/zumo/status", HTTP_GET,
                  zumoStatusHandler) &&
      registerUri(controlServer, "/api/zumo/drive", HTTP_POST,
                  zumoDriveHandler) &&
      registerUri(controlServer, "/api/zumo/acceleration", HTTP_POST,
                  accelerationHandler) &&
      registerUri(controlServer, "/api/zumo/stop", HTTP_POST,
                  zumoStopHandler) &&
      registerUri(controlServer, "/api/zumo/calibrate", HTTP_POST,
                  zumoCalibrateHandler) &&
      registerUri(controlServer, "/api/zumo/face", HTTP_POST,
                  zumoFaceHandler) &&
      registerUri(controlServer, "/api/zumo/face/stop", HTTP_POST,
                  zumoFaceStopHandler) &&
      registerUri(controlServer, "/api/zumo/led", HTTP_POST,
                  zumoLedHandler);
  if (!controlRoutesReady) {
    Serial.println("Web: could not register all control routes");
    httpd_stop(controlServer);
    controlServer = nullptr;
    return false;
  }
  }

  httpd_config_t streamConfig = HTTPD_DEFAULT_CONFIG();
  streamConfig.server_port = app_config::kStreamServerPort;
  // The stream handler sends temporary JPEG copies and has no large JSON locals.
  // Reserve the larger stack for controls; leave internal RAM for motion/audio.
  streamConfig.stack_size = 4096;
  streamConfig.ctrl_port = 32769;
  streamConfig.lru_purge_enable = true;
  const esp_err_t streamError = httpd_start(&streamServer, &streamConfig);
  if (streamError != ESP_OK) {
    streamServer = nullptr;
    Serial.printf("Web: stream server failed: %s; internal free=%u largest=%u\n",
                  esp_err_to_name(streamError),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return false; // Retain the working dashboard while stream startup retries.
  }
  if (!registerUri(streamServer, "/stream", HTTP_GET, streamHandler)) {
    httpd_stop(streamServer);
    streamServer = nullptr;
    Serial.println("Web: could not register the camera stream route");
    return false;
  }

  Serial.println("Web: controls on port 80, camera stream on port 81");
  return true;
}

}  // namespace web_server
