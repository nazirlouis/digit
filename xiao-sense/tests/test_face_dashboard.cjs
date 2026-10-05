const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../src/web_server.cpp'), 'utf8');
const html = source.split('R"HTML(')[1].split(')HTML"')[0];
const script = html.split('<script>')[1].split('</script>')[0];
const names = ['NEUTRAL','HAPPY','SAD','MAD','SERIOUS','CURIOUS','SURPRISED','SLEEPING','CONFUSED','SLEEPY','EXCITED','CRYING','LOVE'];
assert.deepEqual([...html.matchAll(/data-face="([^"]+)"/g)].map(m=>m[1]), names);
assert.match(html, /id="faceVoice"[^>]*checked/);
const nodes = new Map([...html.matchAll(/\bid="([^"]+)"/g)].map(m=>[m[1],{
  textContent:'', value:'70', disabled:false, className:'', checked:m[1]==='faceVoice',
  listeners:{}, addEventListener(name,fn){this.listeners[name]=fn;},
  getContext:()=>({clearRect(){},strokeRect(){},fillText(){}}),
}]));
const buttons=names.map(face=>({dataset:{face},disabled:true,listeners:{},addEventListener(name,fn){this.listeners[name]=fn;}}));
const calls=[];
let generation=1, played=[], stopped=0, defer=false, release, offline=false;
let robot={connected:true,ready:false,left:0,right:0,fault:'NONE',last_error:'NONE'};
let audio={audio_available:true,audio_state:'READY',cue_playing:false};
const response=(data,ok=true)=>({ok,json:async()=>data});
const context=vm.createContext({
  document:{getElementById:id=>nodes.get(id),querySelectorAll:s=>s==='[data-face]'?buttons:[],addEventListener(){},hidden:false},
  window:{addEventListener(){}},location:{hostname:'digit'},
  setInterval(){},clearInterval(){},setTimeout(){},clearTimeout(){},
  async fetch(url, options){
    calls.push({url,options});if(offline)throw Error('offline');
    if(url==='/api/actions')return response({generation,active:false});
    if(url==='/api/zumo/status')return response(robot);
    if(url==='/api/status')return response(audio);
    if(url.startsWith('/api/zumo/face?')){
      const query=new URL(url,'http://digit').searchParams;
      const expected=Number(query.get('generation'));
      if(defer){defer=false;await new Promise(resolve=>release=resolve);}
      if(expected!==generation)return response({generation,message:'Preview cancelled'},false);
      played.push([query.get('value'),query.get('voice')]);generation++;
      return response({generation,message:query.get('voice')==='1'?'Face requested; voice playing once':'Face command accepted'});
    }
    if(url==='/api/zumo/face/stop'||url.startsWith('/api/zumo/stop')){stopped++;generation++;return response({generation,message:'Preview stopped; neutral face requested'});}
    return response({});
  },
});
(async()=>{
  vm.runInContext(script,context);await new Promise(setImmediate);
  await context.refreshActions();await context.refreshZumo();await context.refresh();
  assert.ok(buttons.every(b=>!b.disabled),'Preview must work without floor calibration');
  for(const button of buttons)await button.listeners.click();
  assert.deepEqual(played,names.map(name=>[name,'1']));
  nodes.get('faceVoice').checked=false;nodes.get('faceVoice').listeners.change();
  await context.previewFace('HAPPY');assert.deepEqual(played.at(-1),['HAPPY','0']);
  nodes.get('faceVoice').checked=true;
  for(const busy of [{left:100},{calibrating:true},{look_active:true},{manual_driving:true},{connected:false}]){
    const old={...robot};Object.assign(robot,busy);await context.refreshZumo();
    assert.ok(buttons.every(b=>b.disabled));const count=played.length;
    await context.previewFace('HAPPY');assert.equal(played.length,count);robot=old;
  }
  await context.refreshZumo();audio.audio_state='RECORDING';await context.refresh();
  assert.ok(buttons.every(b=>b.disabled));
  nodes.get('faceVoice').checked=false;context.updateFaceControls();assert.ok(buttons.every(b=>!b.disabled));
  nodes.get('faceVoice').checked=true;audio.audio_state='READY';audio.cue_playing=true;await context.refresh();
  assert.equal(nodes.get('stopPlay').disabled,false);assert.equal(nodes.get('record').disabled,true);
  defer=true;const pending=context.previewFace('LOVE');await new Promise(setImmediate);
  assert.ok(buttons.every(b=>b.disabled));await context.stopFacePreview();
  const count=played.length;release();await pending;
  assert.equal(played.length,count,'Delayed preview cannot restart after Stop preview');
  assert.match(nodes.get('faceMessage').textContent,/Preview stopped/);
  defer=true;const delayed=context.previewFace('SLEEPING');await new Promise(setImmediate);
  await context.stopZumo();release();await delayed;
  assert.equal(played.length,count,'Delayed preview cannot restart after STOP ALL');
  assert.equal(stopped,2);
  assert.ok(calls.filter(c=>c.url.startsWith('/api/zumo/face?')).every(c=>c.options.method==='POST'));
  assert.ok(!calls.some(c=>/\/drive\?|\/look\?|\/wake\/test/.test(c.url)),'Previews never request track motion');
  offline=true;await context.refreshZumo();await context.refresh();assert.ok(buttons.every(b=>b.disabled));
  console.log('PASS: 13 face/voice previews, face-only, uncalibrated preview, busy/offline gates, replacement, stale starts cancelled by both Stop controls');
})().catch(error=>{console.error(error);process.exitCode=1;});
