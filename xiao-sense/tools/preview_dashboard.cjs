// Export the actual dashboard with simulated status and no device access.
// Usage: node tools/preview_dashboard.cjs
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../src/web_server.cpp'), 'utf8');
const promptHeader = fs.readFileSync(path.join(__dirname, '../src/gemini_prompt.h'), 'utf8');
const defaultInstructions = promptHeader.split('R"PROMPT(')[1].split(')PROMPT"')[0].replace(/\r\n?/g,'\n');
let html = source.split('R"HTML(')[1].split(')HTML"')[0];
const status = {
  generation: 1, active: false, action: 'Idle', phase: 'Stopped',
  session_ready: true, drive_ready: true, connected: true, ready: true,
  listening: false, last_command: 8, score: 0.96, candidate_score: 0.96,
  live_active: true, live_connected: true, live_connecting: false, live_connections: 1,
  live_speaking: false, live_probe: false, live_dry_run: true,
  live_text_only:false,live_text_pending:false,live_thinking:false,live_response_id:1,
  waiting_for_reply:false,waiting_for_voice:false,tool_pending:false,reply_wait_ms:0,
  live_video_frames:120,live_video_fps:0.99,current_emotion:'CURIOUS',live_response:'I spotted something interesting.',last_image_id:1,last_image_age_ms:2100,
  wake_ready:true, local_stop_ready: true, local_stop_active: false, local_stop_detections: 0,
  command_decoder_text: 'COME SAY HELLO', candidate_command: 8,
  command_audio_ms: 2000, command_mic_peak: 18000, session_result: 'Recognized',
  identify_ready: true, person_action_ready: true, object_action_ready: true, circle_ready: true,
  battery_action_ready: true, battery_action_reason: '', battery_target_mm: 50, battery_spin_supported: true,
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
html = html.replace('<head>', `<head><meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data: blob:; connect-src 'none';">`);
html = html.replace('Your robot, at a glance', 'Design preview · simulated status');
html = html.replace("$('stream').src=`http://${location.hostname}:81/stream`;", "$('stream').src=" + JSON.stringify('data:image/svg+xml;base64,' + Buffer.from(camera).toString('base64')) + ';');
html = html.replace("$('stream').src=", "chatEntry('user','Hi Digit!');$('stream').src=");
html = html.replace('<script>', `<script>
const previewStatus=${JSON.stringify(status)};
const previewGeminiSettings={configured:true,model:'gemini-robotics-er-2-streaming-preview',dry_run:true,idle_timeout_sec:30,system_instructions:${JSON.stringify(defaultInstructions)},default_system_instructions:${JSON.stringify(defaultInstructions)},system_instructions_storage_bytes:0x1d0000};
window.fetch=async (url,options={})=>{
  const request=new URL(url,'http://preview.local');
  if(request.pathname==='/api/gemini/last-image')return {ok:true,blob:async()=>new Blob([${JSON.stringify(camera)}],{type:'image/svg+xml'})};
  if(request.pathname==='/api/gemini/chat'){
    const fields=JSON.parse(options.body||'{}');previewStatus.live_active=true;previewStatus.live_text_only=true;
    previewStatus.live_response_id++;previewStatus.live_response='This is a design preview. Your message will go directly to Digit on the real dashboard.';
    previewStatus.live_transcript=fields.text;
    return {ok:true,json:async()=>({generation:previewStatus.generation,message:'Preview only — no message sent to Digit.'})};
  }
  if(request.pathname==='/api/gemini/settings'){
    if(options.method==='POST'){
      const fields=JSON.parse(options.body||'{}');previewStatus.generation++;
      if(fields.remove_key)previewGeminiSettings.configured=false;
      else {previewGeminiSettings.configured=true;previewGeminiSettings.model=fields.model||previewGeminiSettings.model;previewGeminiSettings.dry_run=fields.dry_run??previewGeminiSettings.dry_run;previewGeminiSettings.idle_timeout_sec=fields.idle_timeout_sec??previewGeminiSettings.idle_timeout_sec;if(typeof fields.system_instructions==='string')previewGeminiSettings.system_instructions=fields.system_instructions||previewGeminiSettings.default_system_instructions;}
      previewStatus.live_active=false;previewStatus.live_connected=previewGeminiSettings.configured;
      previewStatus.session_ready=previewGeminiSettings.configured;previewStatus.live_reason='';
    }
    return {ok:true,json:async()=>({...previewGeminiSettings,generation:previewStatus.generation,message:'Preview only — no settings saved on Digit.'})};
  }
  if(request.pathname==='/api/actions/debug')return {ok:true,json:async()=>({events:[{id:3,ms:12400,kind:'stopped',action:'Find a way out',reason:'Example: Dashboard STOP ALL button',distance_mm:168,raw_distance_mm:168,range_status:0,range_age_ms:35,range_valid:true,left:3100,right:3300,encoder_left_cps:1550,encoder_right_cps:1540,fault:'NONE',turn:'DONE',connected:true,reply_age_ms:20,generation:4,motion_state:'CANCELLED',motion_result:'STOP',motion_job:3,control_max_gap_ms:8,control_overruns:0,display_max_slice_us:1100}]})};
  if(request.pathname==='/api/actions/console')return {ok:true,json:async()=>({available:true,boot_id:123,revision:4,oldest_id:1,newest_id:4,events:[
    {id:1,revision:1,ms:12400,updated_ms:12600,session:2,source:'Gemini',call_id:'face-1',function:'set_expression',arguments:'expression=CURIOUS, duration_ms=4000',action_reason:'I noticed an unfamiliar object beside the chair.',state:'Applied',result:'Face confirmed by Zumo; gesture and sound are not confirmed by this acknowledgement',category:'Emotes'},
    {id:2,revision:2,ms:14700,updated_ms:15000,session:2,source:'Gemini',call_id:'observe-2',function:'observe',arguments:'',action_reason:'I want a fresh view before deciding where to go.',state:'Completed',result:'Fresh camera frame and sensor status delivered',category:'Other'},
    {id:3,revision:3,ms:16200,updated_ms:16400,session:2,source:'Gemini',call_id:'face-3',function:'set_expression',arguments:'expression=SURPRISED, duration_ms=3000',action_reason:'Something just moved into view.',state:'Blocked',result:'Expression cooldown; keep the current emotion',category:'Emotes'},
    {id:4,revision:4,ms:18500,updated_ms:18500,session:2,source:'Gemini',call_id:'turn-4',function:'turn_right',arguments:'degrees=20',action_reason:'The object is to my right; I want to face it.',state:'Running',result:'Waiting for the actual local movement result',category:'Movement'}
  ].filter(e=>e.revision>Number(request.searchParams.get('after')||0))})};
  if(options.method==='POST'){
    previewStatus.generation++;
    if(request.pathname==='/api/volume')previewStatus.speaker_volume=Number(request.searchParams.get('value'));
    if(request.pathname==='/api/gemini/live/start'){
      const fields=JSON.parse(options.body||'{}');previewStatus.live_active=true;
      previewStatus.live_dry_run=fields.mode==='test_only'||previewGeminiSettings.dry_run;
      previewStatus.live_reason='Observing at 1 FPS';
    }
    if(request.pathname==='/api/gemini/live/stop'||request.pathname==='/api/zumo/stop'){
      previewStatus.live_active=false;previewStatus.live_reason='';
    }
  }
  return {ok:true,json:async()=>({...previewStatus,message:'Preview only — no command sent to Digit.'})};
};
`);
const output = path.join(__dirname, '../test-artifacts/er2-preview/index.html');
fs.mkdirSync(path.dirname(output), {recursive:true});
fs.writeFileSync(output, html);
console.log(output);
