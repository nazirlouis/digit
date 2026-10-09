// Exercise the actual embedded dashboard against simulated device responses.
// Run: node tests/test_wake_dashboard.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../src/web_server.cpp'), 'utf8');
const html = source.split('R"HTML(')[1].split(')HTML"')[0];
const script = html.split('<script>')[1].split('</script>')[0];
const nodes = new Map([...html.matchAll(/\bid="([^"]+)"/g)].map(match => [match[1], {
  textContent: '', className: '', disabled: false, value: '70',
  addEventListener() {},
  getContext: () => ({ clearRect() {}, strokeRect() {}, fillText() {} }),
}]));
const status = {
  audio_state: 'READY', audio_available: true, captured_ms: 0,
  max_recording_ms: 10000, speaker_volume: 70, camera_ready: true,
  vision_ready: true, vision_enabled: false, follow_enabled: false,
  follow_state: 'Off', follow_target_x: -1, psram_free: 4000000,
  wifi_ip: 'device', wake_ready: true, wake_paused: false,
  wake_reacting: false, wake_detections: 0, wake_score: 0,
  wake_error: '', wake_zumo: 'NONE', wake_chime: false,
  wake_zumo_connected: true,
};
let offline = false;
const voice = {available:true,stable:false,front_set:false,calibrated:false,enabled:false,
  degrees:350,relative:0,turning:false,result:'NONE',drive_ready:false,gyro_ready:true,paused:false,error:''};
const calls = [];
const context = vm.createContext({
  document: {
    getElementById(id) { assert.ok(nodes.has(id), `Missing dashboard element ${id}`); return nodes.get(id); },
    querySelectorAll: () => [], addEventListener() {}, hidden: false,
  },
  window: { addEventListener() {} }, location: { hostname: 'device' },
  setInterval() {}, clearInterval() {}, setTimeout() {}, clearTimeout() {},
  async fetch(url, options) {
    calls.push({url, options});
    if (offline) throw Error('offline');
    const data = url === '/api/status' ? {...status} : url === '/api/voice/status' ? {...voice} : url === '/api/zumo/status'
      ? {connected: true, ready: false, fault: 'NONE', last_error: 'NONE', led_result: 'NONE'}
      : {message: 'Wake reaction requested'};
    return {ok: true, json: async () => data};
  },
});

(async () => {
  vm.runInContext(script, context);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(nodes.get('wakeState').textContent, 'Listening for Digit');
  assert.equal(nodes.get('wakeTest').disabled, false);
  assert.equal(nodes.get('voiceToggle').disabled, false);
  assert.ok(!nodes.has('voiceFront') && !nodes.has('voiceLeft'));
  assert.match(html, /270.*straight ahead.*360.*directly left/);
  voice.available = false;
  voice.error = 'ReSpeaker busy reading speech energy (0x40); retrying';
  await context.refreshVoice();
  assert.equal(nodes.get('voiceState').textContent, voice.error);
  assert.equal(nodes.get('voiceToggle').disabled, false);
  voice.available = true;
  voice.error = '';
  voice.stable = true;
  await context.refreshVoice();
  voice.front_set = true;
  await context.refreshVoice();
  assert.equal(nodes.get('voiceToggle').disabled, false);
  voice.calibrated = voice.drive_ready = true;
  await context.refreshVoice();
  assert.equal(nodes.get('voiceToggle').disabled, false);
  voice.paused = true;
  await context.refreshVoice();
  assert.equal(nodes.get('voiceToggle').disabled, false);
  voice.paused = false;
  voice.enabled = voice.turning = true;
  await context.refreshVoice();
  assert.match(nodes.get('voiceState').textContent, /Turning/);
  assert.equal(nodes.get('voiceToggle').disabled, false); // Off always available.
  await context.voiceAction('off');
  assert.ok(calls.some(c=>c.url==='/api/voice/setup?action=off'&&c.options.method==='POST'));
  voice.turning=false;voice.result='DONE';
  voice.turn_reason='Skipped: Voice direction too old';
  await context.refreshVoice();
  assert.equal(nodes.get('voiceResult').textContent,voice.turn_reason);
  voice.turn_reason='Acquiring voice direction';
  await context.refreshVoice();
  assert.equal(nodes.get('voiceResult').textContent,voice.turn_reason);

  // Media operations must not misreport listening as paused.
  for (const mode of ['RECORDING', 'PLAYING']) {
    status.audio_state = mode;
    await context.refresh();
    assert.equal(nodes.get('wakeState').textContent, 'Listening for Digit');
  }
  status.wake_reacting = status.wake_chime = true;
  status.audio_state = 'READY';
  status.wake_detections = 2;
  status.wake_age_ms = 1100;
  status.wake_score = 0.7;
  status.wake_zumo = 'APPLIED';
  await context.refresh();
  assert.equal(nodes.get('wakeState').textContent, 'Listening · wake reaction');
  assert.equal(nodes.get('wakeTest').disabled, true);
  assert.equal(nodes.get('stopPlay').disabled, false);
  assert.match(nodes.get('wakeLast').textContent, /2 heard.*70%/);

  status.vision_enabled = status.wake_paused = true;
  await context.refresh();
  assert.equal(nodes.get('wakeState').textContent, 'Paused for vision');
  assert.equal(nodes.get('wakeTest').disabled, true);
  status.vision_enabled = status.wake_paused = status.wake_reacting = status.wake_chime = false;
  await context.refresh();
  assert.equal(nodes.get('wakeState').textContent, 'Listening for Digit');
  assert.equal(nodes.get('wakeTest').disabled, false);

  await context.action('/api/wake/test');
  assert.ok(calls.some(c => c.url === '/api/wake/test' && c.options.method === 'POST'));
  assert.ok(!calls.some(c => /\/drive|\/turn|\/calibrate/.test(c.url)));
  status.wake_zumo = 'UNKNOWN_COMMAND';
  await context.refresh();
  assert.match(nodes.get('wakeResult').textContent, /Update the Zumo/);
  status.wake_ready = false;
  status.wake_error = 'Microphone unavailable; check audio status';
  await context.refresh();
  assert.equal(nodes.get('wakeState').textContent, 'Wake unavailable');
  assert.equal(nodes.get('wakeResult').textContent, status.wake_error);
  offline = true;
  await context.refresh();
  await context.refreshVoice();
  assert.equal(nodes.get('voiceToggle').disabled, true);
  assert.equal(nodes.get('wakeState').textContent, 'Unavailable');
  assert.equal(nodes.get('wakeTest').disabled, true);
  console.log('PASS: wake dashboard plus voice setup, floor gate, vision gate, turn cancellation, offline');
})().catch(error => { console.error(error); process.exitCode = 1; });
