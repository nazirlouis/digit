// Execute the real dashboard script with deterministic delayed network replies.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../src/web_server.cpp'), 'utf8');
const capacity = Number(source.match(/controlConfig\.max_uri_handlers\s*=\s*(\d+)/)[1]);
const routes = source.split('const bool controlRoutesReady =')[1].split('if (!controlRoutesReady)')[0];
assert.ok([...routes.matchAll(/registerUri\(controlServer,/g)].length <= capacity,
          'The control server must have room to register the battery route');
const html = source.split('R"HTML(')[1].split(')HTML"')[0];
const script = html.split('<script>')[1].split('</script>')[0];
const painted = [];
const nodes = new Map([...html.matchAll(/\bid="([^"]+)"/g)].map(m => [m[1], {
  textContent:'',value:'70',disabled:false,className:'',addEventListener(){},
  getContext:()=>({clearRect(){},strokeRect(){},fillText(text){painted.push(text);}}),
}]));
let generation=1, enabled=false, defer=false, release, offline=false;
const calls=[];
const response=(data,ok=true)=>({ok,json:async()=>data});
const context=vm.createContext({
  document:{getElementById:id=>nodes.get(id),querySelectorAll:()=>[],addEventListener(){},hidden:false},
  window:{addEventListener(){}},location:{hostname:'digit'},
  setInterval(){},clearInterval(){},setTimeout(){},clearTimeout(){},
  async fetch(url,options){
    calls.push({url,options});
    if(offline)throw Error('offline');
    if(url==='/api/actions')return response({generation,active:false,drive_ready:true});
    if(url.startsWith('/api/zumo/stop')){enabled=false;++generation;return response({generation});}
    if(url.startsWith('/api/vision/objects?')){
      const query=new URL(url,'http://digit').searchParams;
      if(defer){defer=false;await new Promise(resolve=>release=resolve);}
      if(query.get('enabled')==='1'&&Number(query.get('generation'))!==generation)
        return response({generation,message:'Object test cancelled'},false);
      enabled=query.get('enabled')==='1';++generation;
      return response({generation,message:enabled?'Object test started':'Object test stopped'});
    }
    if(url==='/api/status')return response({
      camera_ready:true,vision_ready:true,battery_ready:true,vision_enabled:enabled,battery_mode:enabled,
      battery_detected:enabled,battery_score_percent:91,person_detected:false,
      vision_age_ms:10,vision_inference_ms:140,vision_width:320,vision_height:240,
      object_classes:['Battery','Marker','Screwdriver'],
      boxes:enabled?[
        {x1:20,y1:30,x2:50,y2:100,score:91,class_id:0,label:'Battery'},
        {x1:70,y1:30,x2:90,y2:140,score:88,class_id:1,label:'Marker'},
        {x1:130,y1:30,x2:145,y2:160,score:94,class_id:2,label:'Screwdriver'},
      ]:[],
      follow_enabled:false,audio_state:'READY',psram_free:4000000,
    });
    if(url==='/api/zumo/status')return response({connected:true,ready:true,fault:'NONE',last_error:'NONE'});
    return response({});
  },
});
(async()=>{
  vm.runInContext(script,context);
  await new Promise(setImmediate);
  await context.refreshActions();await context.refresh();
  assert.equal(nodes.get('batteryToggle').disabled,false);
  await context.toggleBattery();
  assert.equal(enabled,true);
  assert.equal(nodes.get('batteryToggle').textContent,'Stop object test');
  assert.equal(nodes.get('followToggle').disabled,true,'Battery boxes cannot enable person tracking');
  assert.ok(painted.includes('Battery 91%'));
  assert.ok(painted.includes('Marker 88%'));
  assert.ok(painted.includes('Screwdriver 94%'));
  assert.match(nodes.get('batteryState').textContent,/91%/);
  assert.match(nodes.get('batteryState').textContent,/Screwdriver 94%/);
  assert.equal(calls.find(c=>c.url.startsWith('/api/vision/objects?')).options.method,'POST');
  await context.toggleBattery();assert.equal(enabled,false);
  defer=true;const pending=context.toggleBattery();
  await new Promise(setImmediate);await context.stopZumo();release();await pending;
  assert.equal(enabled,false,'A delayed test request must not restart after Stop');
  assert.match(nodes.get('batteryState').textContent,/cancelled/);
  offline=true;await context.refresh();
  assert.equal(nodes.get('batteryToggle').disabled,true);
  assert.equal(nodes.get('personBadge').hidden,true);
  console.log('PASS: object button, all three labels/confidences, no person tracking, delayed start cancelled by Stop, offline');
})().catch(error=>{console.error(error);process.exitCode=1;});
