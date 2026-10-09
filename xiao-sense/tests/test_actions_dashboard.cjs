const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname,'../src/web_server.cpp'),'utf8');
const html = source.split('R"HTML(')[1].split(')HTML"')[0];
const script = html.split('<script>')[1].split('</script>')[0];
const nodes = new Map([...html.matchAll(/\bid="([^"]+)"/g)].map(m=>[m[1],{
  textContent:'',className:'',disabled:false,value:'1000',hidden:false,
  listeners:{},addEventListener(name,fn){this.listeners[name]=fn;},getContext:()=>({clearRect(){},strokeRect(){},fillText(){}}),
}]));
const routineIds = [...html.matchAll(/data-routine="(\d+)"/g)].map(m=>Number(m[1]));
assert.deepEqual(routineIds,[8,9,16,14]);
assert.ok(html.includes('>Come say hello<small>'));
assert.ok(html.includes('place a battery at the exit'));
assert.ok(html.includes('about 10 cm before celebrating'));
assert.ok(html.includes('more than 5 cm ahead'));
assert.ok(html.includes('90° right without checking behind him'));
assert.ok(html.includes('id="driveSpeed" type="range" min="3000"'));
assert.ok(!html.includes('exit marker'));
assert.ok(!html.includes('Bump me'));
const buttons = routineIds.map(id=>({disabled:false,dataset:{routine:String(id)}}));
const driveButtons=['forward','back','left','right'].map(drive=>({dataset:{drive},listeners:{},setPointerCapture(){},addEventListener(name,fn){this.listeners[name]=fn;}}));
const windowListeners={},documentListeners={};
let generation=1,active=false,deferNext=false,release,offline=false,deferStop=false,releaseStop;
const calls=[];
let voice={};
let rangeStatus={range_valid:true,distance_mm:50,range_age_ms:20};
const timers=new Map();let nextTimer=1;
function reply(data,ok=true){return {ok,json:async()=>data};}
const context = vm.createContext({
  document:{getElementById:id=>nodes.get(id),querySelectorAll:s=>s==='[data-routine]'?buttons:s==='[data-drive]'?driveButtons:[],addEventListener(name,fn){documentListeners[name]=fn;},hidden:false},
  window:{addEventListener(name,fn){windowListeners[name]=fn;}},location:{hostname:'digit'},
  setInterval(){},clearInterval(){},setTimeout(fn,delay){const id=nextTimer++;timers.set(id,{fn,delay});return id;},clearTimeout(id){timers.delete(id);},
  async fetch(url,options){
    calls.push({url,options});
    if(offline) throw Error('offline');
    if(url==='/api/actions/debug')return reply({events:[{id:1,ms:1000,kind:'stopped',action:'Find a way out',reason:'Manual drive stopped: browser lost focus',distance_mm:168,raw_distance_mm:168,range_status:0,range_age_ms:30,range_valid:true,left:3100,right:3200,encoder_left_cps:1550,encoder_right_cps:1560,fault:'NONE',turn:'DONE',connected:true,reply_age_ms:25,generation:4}]});
    if(url==='/api/actions')return reply({generation,active,action:active?'Say hello':'Idle',phase:active?'Saying hello':'Stopped',voice_ready:true,listening:false,drive_ready:true,circle_ready:true,identify_ready:true,person_action_ready:true,object_action_ready:true,follow_action_ready:true,chase_action_ready:true,...voice});
    if(url.startsWith('/api/zumo/stop')){if(deferStop){deferStop=false;await new Promise(resolve=>releaseStop=resolve);}generation++;active=false;return reply({generation,message:'All actions cancelled; motor stop sent'});}
    if(url.startsWith('/api/actions?')||url.startsWith('/api/zumo/drive?')){
      const requestGeneration=Number(new URL(url,'http://digit').searchParams.get('generation'));
      if(deferNext){deferNext=false;await new Promise(resolve=>release=resolve);}
      if(requestGeneration!==generation)return reply({generation,message:'Request cancelled'},false);
      active=true;generation++;return reply({generation,message:'Action accepted'});
    }
    if(url==='/api/voice/status')return reply({});
    if(url==='/api/zumo/status')return reply({connected:true,ready:true,fault:'NONE',last_error:'NONE',...rangeStatus});
    return reply({camera_ready:true,vision_ready:true,audio_state:'READY',max_recording_ms:10000,captured_ms:0});
  },
});
const testTimeout=setTimeout(()=>{console.error('FAIL: dashboard test did not complete');process.exit(1);},5000);
(async()=>{
  vm.runInContext(script,context);
  await new Promise(resolve=>setImmediate(resolve));
  nodes.get('actionDebug').open=true;await context.refreshDebug();
  assert.match(nodes.get('debugLog').textContent,/browser lost focus/);
  assert.match(nodes.get('debugLog').textContent,/168 mm/);
  assert.match(context.motionDebug({motion_supported:true,motion_state:'DONE',motion_result:'WALL_STOPPED',motion_job:42,control_max_gap_ms:9}),/Zumo job 42: DONE.*WALL_STOPPED/);
  assert.match(context.motionDebug({motion_supported:true,control_max_gap_ms:9}),/control gap: 9 ms/);
  nodes.get('actionDebug').open=false;
  assert.equal(nodes.get('distanceBadge').textContent,'ToF: 5.0 cm');
  rangeStatus={range_valid:true,distance_mm:13,range_age_ms:100,ready:false,fault:'EDGE'};
  await context.refreshZumo();
  assert.equal(nodes.get('distanceBadge').textContent,'ToF: 1.3 cm','Range remains visible while drive is locked');
  rangeStatus={range_valid:false,distance_mm:-1,range_age_ms:20};
  await context.refreshZumo();assert.equal(nodes.get('distanceBadge').textContent,'ToF: unavailable');
  rangeStatus={range_valid:false,range_no_return:true,distance_mm:-1,range_age_ms:20};
  await context.refreshZumo();assert.equal(nodes.get('distanceBadge').textContent,'ToF: no return');
  const noReturnExpiry=[...timers.values()].find(t=>t.delay===1500);
  assert.ok(noReturnExpiry);noReturnExpiry.fn();
  assert.equal(nodes.get('distanceBadge').textContent,'ToF: stale');
  rangeStatus={range_valid:false,range_no_return:true,distance_mm:-1,range_age_ms:250};
  await context.refreshZumo();assert.equal(nodes.get('distanceBadge').textContent,'ToF: stale');
  rangeStatus={range_valid:true,distance_mm:50,range_age_ms:250};
  await context.refreshZumo();assert.equal(nodes.get('distanceBadge').textContent,'ToF: stale');
  rangeStatus={connected:false,range_valid:true,distance_mm:50,range_age_ms:10};
  await context.refreshZumo();assert.equal(nodes.get('distanceBadge').textContent,'ToF: offline');
  rangeStatus={range_valid:true,distance_mm:50,range_age_ms:10};
  await context.refreshZumo();
  const expiry=[...timers.values()].find(t=>t.delay===1500);
  assert.ok(expiry);expiry.fn();
  assert.equal(nodes.get('distanceBadge').textContent,'ToF: stale','Frozen requests must expire the visible number');
  offline=true;await context.refreshZumo();
  assert.equal(nodes.get('distanceBadge').textContent,'ToF: offline');
  offline=false;await context.refreshZumo();
  nodes.get('driveSpeed').value='4000';
  nodes.get('driveSpeed').listeners.input({target:{value:'4000'}});
  await nodes.get('driveSpeed').listeners.change();
  assert.ok(calls.some(c=>c.url==='/api/zumo/speed?value=4000'&&c.options.method==='POST'));
  await context.startRoutine(16);
  const danceParams=new URL(calls.findLast(c=>c.url.startsWith('/api/actions?')).url,'http://digit').searchParams;
  assert.equal(danceParams.get('command'),'16');assert.equal(danceParams.get('speed'),'4000');
  await context.stopZumo(true);
  nodes.get('driveAcceleration').value='500';
  nodes.get('driveAcceleration').listeners.input({target:{value:'500'}});
  assert.equal(nodes.get('driveAccelerationValue').textContent,'500');
  await nodes.get('driveAcceleration').listeners.change();
  assert.ok(calls.some(c=>c.url==='/api/zumo/acceleration?value=500'&&c.options.method==='POST'));
  vm.runInContext("heldDirection='forward'",context);
  await context.sendDrive();
  let params=new URL(calls.findLast(c=>c.url.startsWith('/api/zumo/drive?')).url,'http://digit').searchParams;
  assert.equal(params.get('left'),'4000');assert.equal(params.get('right'),'4000');
  assert.equal(params.get('acceleration'),'500');
  nodes.get('driveAcceleration').value='6000';
  await context.sendDrive();
  params=new URL(calls.findLast(c=>c.url.startsWith('/api/zumo/drive?')).url,'http://digit').searchParams;
  assert.equal(params.get('acceleration'),'6000','Changing acceleration must affect the next held-drive request');
  await context.stopZumo();
  const stopCalls=()=>calls.filter(c=>c.url.startsWith('/api/zumo/stop'));
  let before=stopCalls().length;
  active=true;
  await windowListeners.blur();
  context.document.hidden=true; await documentListeners.visibilitychange();
  await driveButtons[0].listeners.pointerup({type:'pointerup'});
  await driveButtons[0].listeners.pointercancel();
  assert.equal(stopCalls().length,before,'An idle dashboard tab must not cancel an autonomous action');
  assert.equal(active,true);
  context.document.hidden=false;
  vm.runInContext("heldDirection='forward'",context);
  await driveButtons[0].listeners.pointerup({type:'pointerup'});
  assert.equal(stopCalls().at(-1).url,'/api/zumo/stop?source=release');
  await nodes.get('zumoStop').listeners.click({type:'click'});
  assert.equal(stopCalls().at(-1).url,'/api/zumo/stop?source=stop-button');
  for(const [handler,source] of [[driveButtons[0].listeners.pointercancel,'pointer-cancel'],[windowListeners.blur,'blur'],[driveButtons[0].listeners.lostpointercapture,'capture-lost']]){
    vm.runInContext("heldDirection='forward'",context);await handler();
    assert.equal(stopCalls().at(-1).url,'/api/zumo/stop?source='+source+'&hard=1');
  }
  vm.runInContext("heldDirection='forward'",context);context.document.hidden=true;
  await documentListeners.visibilitychange();
  assert.equal(stopCalls().at(-1).url,'/api/zumo/stop?source=hidden&hard=1');
  context.document.hidden=false;
  deferStop=true;
  const pendingBrake=context.stopZumo();
  await new Promise(resolve=>setImmediate(resolve));
  await context.stopZumo(true);
  assert.equal(stopCalls().at(-1).url,'/api/zumo/stop?source=stop-button&hard=1','Safety abort overrides a pending normal stop');
  assert.equal(vm.runInContext('stopPending',context),true);
  releaseStop();await pendingBrake;
  assert.equal(vm.runInContext('stopPending',context),false);
  assert.equal(vm.runInContext('Number.isFinite(controlGeneration)',context),true);
  for(const id of routineIds){
    await context.refreshActions();
    await context.startRoutine(id);
    assert.equal(active,true);
    assert.ok(calls.some(c=>c.url.startsWith('/api/actions?command='+id+'&generation=')&&c.options.method==='POST'));
    await context.stopZumo();
    assert.equal(active,false);
  }
  deferNext=true;
  const pending=context.startRoutine(8);
  await new Promise(resolve=>setImmediate(resolve));
  await context.stopZumo();
  release();await pending;
  assert.equal(active,false,'A delayed action must not restart after Stop');
  assert.match(nodes.get('routineMessage').textContent,/cancelled/);
  await context.refreshActions();
  vm.runInContext("heldDirection='forward'",context);
  deferNext=true;
  const drive=context.sendDrive();
  await new Promise(resolve=>setImmediate(resolve));
  await context.stopZumo();release();await drive;
  assert.equal(active,false,'A delayed hold-to-drive request must not restart');
  const beforeRetired=calls.length;
  for(const id of [1,2,3,4,5,6,10,11,12,13,15,17])await context.startRoutine(id);
  assert.equal(calls.length,beforeRetired,'Retired actions must not send requests');
  assert.equal(nodes.has('voicePhoto'),false);
  for(const [status,message] of [
    [{listening:true},'Listening for your command…'],
    [{voice_result:'Recognized',last_command:7},'Heard: Stop'],
    [{voice_result:'Recognized',last_command:8},'Heard: Come say hello'],
    [{voice_result:'No command recognized'},'No command heard. Say Digit and try again.'],
    [{voice_result:'Audio interrupted'},'Listening interrupted. Say Digit and try again.'],
    [{voice_result:'Audio queue overflow'},'Listening interrupted. Say Digit and try again.'],
    [{voice_ready:false,voice_error:'AFE initialization failed'},'AFE initialization failed'],
  ]) {
    voice=status; await context.refreshActions();
    assert.equal(nodes.get('commandState').textContent,message);
  }
  voice={person_action_ready:false};
  await context.refreshActions();
  assert.equal(buttons.find(b=>b.dataset.routine==='8').disabled,true);
  assert.ok(buttons.filter(b=>b.dataset.routine!=='8').every(b=>!b.disabled));
  voice={object_action_ready:false};
  await context.refreshActions();
  assert.equal(buttons.find(b=>b.dataset.routine==='14').disabled,true);
  assert.ok(buttons.filter(b=>b.dataset.routine!=='14').every(b=>!b.disabled));
  voice={follow_action_ready:false,drive_ready:false,person_action_ready:false,object_action_ready:false,chase_action_ready:false,circle_ready:false,identify_ready:true,movement_reason:'Edge stop: place Digit fully on the surface, then recalibrate'};
  await context.refreshActions();
  assert.equal(nodes.get('actionAvailability').textContent,voice.movement_reason);
  assert.equal(context.freshVision({vision_age_ms:20,vision_capture_age_ms:500}),true);
  assert.equal(context.freshVision({vision_age_ms:20,vision_capture_age_ms:1201}),false);
  assert.equal(context.freshVision({vision_age_ms:1201,vision_capture_age_ms:500}),false);
  assert.equal(buttons.find(b=>b.dataset.routine==='9').disabled,false);
  assert.ok(buttons.filter(b=>b.dataset.routine!=='9').every(b=>b.disabled));
  offline=true;await context.refreshActions();
  assert.ok(buttons.every(b=>b.disabled));
  assert.equal(nodes.get('zumoStop').disabled,false);
  console.log('PASS: all approved actions, stationary identification, acceleration, retired actions blocked, Stop during pending start and drive, offline controls');
})().catch(error=>{console.error(error);process.exitCode=1;}).finally(()=>clearTimeout(testTimeout));
