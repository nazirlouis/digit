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
assert.deepEqual(routineIds,[1]);
const buttons = routineIds.map(()=>({disabled:false}));
const driveButtons=['forward','back','left','right'].map(drive=>({dataset:{drive},listeners:{},setPointerCapture(){},addEventListener(name,fn){this.listeners[name]=fn;}}));
const windowListeners={},documentListeners={};
let generation=1,active=false,deferNext=false,release,offline=false,deferStop=false,releaseStop;
const calls=[];
let voice={};
function reply(data,ok=true){return {ok,json:async()=>data};}
const context = vm.createContext({
  document:{getElementById:id=>nodes.get(id),querySelectorAll:s=>s==='[data-routine]'?buttons:s==='[data-drive]'?driveButtons:[],addEventListener(name,fn){documentListeners[name]=fn;},hidden:false},
  window:{addEventListener(name,fn){windowListeners[name]=fn;}},location:{hostname:'digit'},
  setInterval(){},clearInterval(){},setTimeout(){},clearTimeout(){},
  async fetch(url,options){
    calls.push({url,options});
    if(offline) throw Error('offline');
    if(url==='/api/actions')return reply({generation,active,action:active?'Say hello':'Idle',phase:active?'Saying hello':'Stopped',voice_ready:true,listening:false,drive_ready:true,...voice});
    if(url.startsWith('/api/zumo/stop')){if(deferStop){deferStop=false;await new Promise(resolve=>releaseStop=resolve);}generation++;active=false;return reply({generation,message:'All actions cancelled; motor stop sent'});}
    if(url.startsWith('/api/actions?')||url.startsWith('/api/zumo/drive?')){
      const requestGeneration=Number(new URL(url,'http://digit').searchParams.get('generation'));
      if(deferNext){deferNext=false;await new Promise(resolve=>release=resolve);}
      if(requestGeneration!==generation)return reply({generation,message:'Request cancelled'},false);
      active=true;generation++;return reply({generation,message:'Action accepted'});
    }
    if(url==='/api/voice/status')return reply({});
    if(url==='/api/zumo/status')return reply({connected:true,ready:true,fault:'NONE',last_error:'NONE'});
    return reply({camera_ready:true,vision_ready:true,audio_state:'READY',max_recording_ms:10000,captured_ms:0});
  },
});
const testTimeout=setTimeout(()=>{console.error('FAIL: dashboard test did not complete');process.exit(1);},5000);
(async()=>{
  vm.runInContext(script,context);
  await new Promise(resolve=>setImmediate(resolve));
  nodes.get('driveSpeed').value='4000';
  nodes.get('driveAcceleration').value='500';
  nodes.get('driveAcceleration').listeners.input({target:{value:'500'}});
  assert.equal(nodes.get('driveAccelerationValue').textContent,'500');
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
  await driveButtons[0].listeners.pointerup({type:'pointerup'});
  assert.equal(stopCalls().at(-1).url,'/api/zumo/stop','Normal release requests fixed deceleration');
  await nodes.get('zumoStop').listeners.click({type:'click'});
  assert.equal(stopCalls().at(-1).url,'/api/zumo/stop','Drive Stop requests fixed deceleration');
  await driveButtons[0].listeners.pointercancel();
  assert.equal(stopCalls().at(-1).url,'/api/zumo/stop?hard=1');
  await windowListeners.blur();
  assert.equal(stopCalls().at(-1).url,'/api/zumo/stop?hard=1');
  vm.runInContext("heldDirection='forward'",context);
  await driveButtons[0].listeners.lostpointercapture();
  await new Promise(resolve=>setImmediate(resolve));
  assert.equal(stopCalls().at(-1).url,'/api/zumo/stop?hard=1');
  deferStop=true;
  const pendingBrake=context.stopZumo();
  await new Promise(resolve=>setImmediate(resolve));
  await context.stopZumo(true);
  assert.equal(stopCalls().at(-1).url,'/api/zumo/stop?hard=1','Safety abort overrides a pending normal stop');
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
  const pending=context.startRoutine(1);
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
  for(let id=2;id<=6;++id)await context.startRoutine(id);
  assert.equal(calls.length,beforeRetired,'Retired actions must not send requests');
  assert.equal(nodes.has('voicePhoto'),false);
  for(const [status,message] of [
    [{listening:true},'Listening for your command…'],
    [{voice_result:'Recognized',last_command:7},'Heard: Stop'],
    [{voice_result:'Recognized',last_command:1},'Heard: Say hello'],
    [{voice_result:'No command recognized'},'No command heard. Say Digit and try again.'],
    [{voice_result:'Audio interrupted'},'Listening interrupted. Say Digit and try again.'],
    [{voice_result:'Audio queue overflow'},'Listening interrupted. Say Digit and try again.'],
    [{voice_ready:false,voice_error:'AFE initialization failed'},'AFE initialization failed'],
  ]) {
    voice=status; await context.refreshActions();
    assert.equal(nodes.get('commandState').textContent,message);
  }
  offline=true;await context.refreshActions();
  assert.ok(buttons.every(b=>b.disabled));
  assert.equal(nodes.get('zumoStop').disabled,false);
  console.log('PASS: acceleration slider and live drive updates, Say hello only, retired actions blocked, Stop during pending start and drive, offline controls');
})().catch(error=>{console.error(error);process.exitCode=1;}).finally(()=>clearTimeout(testTimeout));
