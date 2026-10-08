// Export the actual dashboard with simulated status and no device access.
// Usage: node tools/preview_dashboard.cjs
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../src/web_server.cpp'), 'utf8');
let html = source.split('R"HTML(')[1].split(')HTML"')[0];
const status = {
  generation: 1, active: false, action: 'Idle', phase: 'Stopped',
  voice_ready: true, drive_ready: true, connected: true, ready: true,
  identify_ready: true, person_action_ready: true, object_action_ready: true,
  follow_action_ready: true, chase_action_ready: true,
  fault: 'NONE', last_error: 'NONE', battery_mv: 7600, left: 0, right: 0,
  distance_mm: 250, raw_distance_mm: 250, range_status: 0, range_valid: true, range_age_ms: 10,
  motion_supported: true, motion_state: 'IDLE', motion_result: 'NONE', motion_job: 0, control_max_gap_ms: 8, control_overruns: 0, display_max_slice_us: 1100, target_left_cps: 0, target_right_cps: 0, heading_deci_deg: 0,
  encoder_control: true, encoder_left_cps: 0, encoder_right_cps: 0, reply_age_ms: 20,
  audio_state: 'READY', audio_available: true, camera_ready: true,
  vision_ready: true, vision_enabled: false, follow_enabled: false, follow_state: 'Off',
  captured_ms: 0, max_recording_ms: 10000, psram_free: 4000000,
  wifi_ip: 'Preview only', speaker_volume: 100, available: true,
  battery_ready: true, wake_ready: true, wake_zumo: 'NONE',
  led_result: 'NONE', calibrated: true, front_set: true, enabled: true,
  gyro_ready: true, result: 'NONE',
};
const camera = `<svg xmlns="http://www.w3.org/2000/svg" width="640" height="480" viewBox="0 0 640 480"><rect width="640" height="480" fill="#152422"/><g fill="none" stroke="#638477" stroke-width="2"><rect x="276" y="180" width="88" height="64" rx="14"/><circle cx="320" cy="212" r="17"/><path d="M294 180l7-10h38l7 10"/></g><text x="320" y="283" text-anchor="middle" font-family="Segoe UI, sans-serif" font-size="18" fill="#ecf5ef">Your live camera will appear here</text><text x="320" y="313" text-anchor="middle" font-family="Segoe UI, sans-serif" font-size="13" fill="#abc0b5">Design preview · no device connected</text></svg>`;
html = html.replace('<head>', `<head><meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data:; connect-src 'none';">`);
html = html.replace('Your robot, at a glance', 'Design preview · simulated status');
html = html.replace("$('stream').src=`http://${location.hostname}:81/stream`;", "$('stream').src=" + JSON.stringify('data:image/svg+xml;base64,' + Buffer.from(camera).toString('base64')) + ';');
html = html.replace('<script>', `<script>
const previewStatus=${JSON.stringify(status)};
window.fetch=async (url,options={})=>{
  const request=new URL(url,'http://preview.local');
  if(request.pathname==='/api/actions/debug')return {ok:true,json:async()=>({events:[{id:3,ms:12400,kind:'stopped',action:'Find a way out',reason:'Example: Dashboard STOP ALL button',distance_mm:168,raw_distance_mm:168,range_status:0,range_age_ms:35,range_valid:true,left:3100,right:3300,encoder_left_cps:1550,encoder_right_cps:1540,fault:'NONE',turn:'DONE',connected:true,reply_age_ms:20,generation:4,motion_state:'CANCELLED',motion_result:'STOP',motion_job:3,control_max_gap_ms:8,control_overruns:0,display_max_slice_us:1100}]})};
  if(request.pathname==='/api/camera/photo')return {ok:false,text:async()=>'Preview only. Photos are available on Digit.'};
  if(options.method==='POST'){
    previewStatus.generation++;
    if(request.pathname==='/api/volume')previewStatus.speaker_volume=Number(request.searchParams.get('value'));
  }
  return {ok:true,json:async()=>({...previewStatus,message:'Preview only — no command sent to Digit.'})};
};
`);
const output = path.join(__dirname, '../test-artifacts/ui-overhaul/index.html');
fs.mkdirSync(path.dirname(output), {recursive:true});
fs.writeFileSync(output, html);
console.log(output);
