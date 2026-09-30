const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname,'../src/web_server.cpp'),'utf8');
const html = source.split('R"HTML(')[1].split(')HTML"')[0];
const script = html.split('<script>')[1].split('</script>')[0];
const nodes = new Map([...html.matchAll(/\bid="([^"]+)"/g)].map(m=>[m[1],{
  textContent:'',className:'',disabled:false,value:'1000',hidden:false,
  addEventListener(){},getContext:()=>({clearRect(){},strokeRect(){},fillText(){}}),
}]));
const buttons = Array.from({length:6},()=>({disabled:false}));
let generation=1,active=false,deferNext=false,release,offline=false,photoRevision=0;
const calls=[];
function reply(data,ok=true){return {ok,json:async()=>data};}
const context = vm.createContext({
  document:{getElementById:id=>nodes.get(id),querySelectorAll:s=>s==='[data-routine]'?buttons:[],addEventListener(){},hidden:false},
  window:{addEventListener(){}},location:{hostname:'digit'},
  setInterval(){},clearInterval(){},setTimeout(){},clearTimeout(){},
  async fetch(url,options){
    calls.push({url,options});
    if(offline) throw Error('offline');
    if(url==='/api/actions')return reply({generation,active,action:active?'Explore':'Idle',phase:active?'Exploring':'Stopped',voice_ready:true,listening:false,drive_ready:true,photo_revision:photoRevision});
    if(url==='/api/zumo/stop'){generation++;active=false;return reply({generation,message:'All actions cancelled; motor stop sent'});}
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
(async()=>{
  vm.runInContext(script,context);
  await new Promise(resolve=>setImmediate(resolve));
  for(let id=1;id<=6;id++){
    await context.refreshActions();
    await context.startRoutine(id);
    assert.equal(active,true);
    assert.ok(calls.some(c=>c.url.startsWith('/api/actions?command='+id+'&generation=')&&c.options.method==='POST'));
    await context.stopZumo();
    assert.equal(active,false);
  }
  deferNext=true;
  const pending=context.startRoutine(6);
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
  photoRevision=1;await context.refreshActions();
  assert.equal(nodes.get('voicePhotoPreview').src,'/api/actions/photo?v=1');
  assert.equal(nodes.get('voicePhoto').hidden,false);
  offline=true;await context.refreshActions();
  assert.ok(buttons.every(b=>b.disabled));
  assert.equal(nodes.get('zumoStop').disabled,false);
  console.log('PASS: six actions, Stop during pending start and drive, photo preview, offline controls');
})().catch(error=>{console.error(error);process.exitCode=1;});
