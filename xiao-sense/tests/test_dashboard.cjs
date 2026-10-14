// Exercise the current dashboard in a real browser with every request mocked.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {chromium} = require('playwright');
const html = fs.readFileSync(path.join(__dirname, '../src/web_server.cpp'), 'utf8')
  .split('R"HTML(')[1].split(')HTML"')[0];

(async () => {
  const browser = await chromium.launch({headless:true, channel:process.env.DIGIT_TEST_BROWSER || 'msedge'});
  try {
    const page = await browser.newPage(), errors = [], faces = [], drives = [];
    const status = {generation:1, active:false, connected:true, ready:true, calibrated:false,
      fault:'NONE', last_error:'NONE', left:0, right:0, battery_mv:7600,
      calibrating:false, look_active:false, manual_driving:false,
      camera_ready:true, vision_ready:true, battery_ready:true, audio_available:true,
      vision_age_ms:10, vision_inference_ms:140, vision_width:320, vision_height:240,
      audio_state:'READY', local_stop_ready:true, session_ready:true, live_connected:true,
      range_valid:true, range_age_ms:10, distance_mm:50, speaker_volume:100, events:[]};
    let enabled = false, offline = false, deferredPath, stalled, reached;
    let consoleData={available:true,boot_id:123,revision:1,oldest_id:1,newest_id:1,events:[{id:1,revision:1,ms:1000,updated_ms:1000,session:2,source:'Gemini',call_id:'face-1',function:'set_expression',arguments:'expression=CURIOUS, duration_ms=4000',action_reason:'I noticed <an unfamiliar> object.',state:'Queued',result:'Awaiting local checks',category:'Emotes'}]};
    async function waitForStalled(waiting) {
      let timer; const expected = deferredPath;
      try {await Promise.race([waiting, new Promise((_,reject) => {timer=setTimeout(() => reject(Error('Expected a delayed request: '+expected)), 5000);})]);}
      finally {clearTimeout(timer);}
    }
    const reply = (route, data, code=200) => route.fulfill({status:code, contentType:'application/json', body:JSON.stringify(data)});
    async function command(route) {
      const url = new URL(route.request().url());
      if (url.pathname === '/api/zumo/stop' || url.pathname === '/api/zumo/face/stop') {
        enabled = false; status.generation++;
        return reply(route, {...status, message:'Stopped'});
      }
      if (Number(url.searchParams.get('generation')) !== status.generation)
        return reply(route, {...status, message:'Request cancelled'}, 409);
      if (url.pathname === '/api/zumo/face') faces.push(url.searchParams.get('value'));
      if (url.pathname === '/api/vision/objects') enabled = url.searchParams.get('enabled') === '1';
      if (url.pathname === '/api/zumo/drive') drives.push(url.searchParams);
      status.generation++;
      return reply(route, {...status, message:'Applied'});
    }
    page.on('pageerror', error => errors.push(error.message));
    await page.route('**/*', async route => {
      const url = new URL(route.request().url());
      if (url.pathname === '/') return route.fulfill({contentType:'text/html', body:html});
      if (url.pathname === '/stream') return route.abort();
      if (url.pathname === deferredPath) {deferredPath = null; stalled = route; reached(); return;}
      if (route.request().method() === 'POST') return command(route);
      if (offline) return reply(route, {message:'Offline'}, 503);
      if (url.pathname === '/api/actions/console') {
        const after=Number(url.searchParams.get('after')||0),sameBoot=!url.searchParams.has('boot')||Number(url.searchParams.get('boot'))===consoleData.boot_id;
        return reply(route,{...consoleData,events:consoleData.events.filter(e=>!sameBoot||after>consoleData.revision||e.revision>after)});
      }
      if (url.pathname === '/api/gemini/settings') return reply(route, {
        configured:true, model:'gemini-robotics-er-2-streaming-preview', dry_run:true,
        idle_timeout_sec:30, generation:status.generation,
        system_instructions:'You are Digit.', default_system_instructions:'You are Digit.'});
      return reply(route, {...status, vision_enabled:enabled, battery_mode:enabled,
        battery_detected:enabled, battery_score_percent:91,
        boxes:enabled ? ['Battery','Marker','Screwdriver'].map((label,class_id) =>
          ({label,class_id,x1:20+class_id*60,y1:30,x2:50+class_id*60,y2:100,score:91})) : []});
    });
    const refresh = () => page.evaluate(async () => {await refreshActions(); await refreshZumo(); await refresh();});
    await page.goto('http://digit-preview.test/');
    await page.waitForFunction(() => geminiSettingsLoaded && controlGeneration > 0);
    await refresh();

    await page.waitForFunction(()=>consoleEntries.size===1);
    assert.match(await page.locator('#consoleLog').textContent(),/CURIOUS face/);
    assert.match(await page.locator('#consoleLog').textContent(),/I noticed <an unfamiliar> object/);
    await page.locator('#consoleLog summary').click();
    consoleData.revision=2;Object.assign(consoleData.events[0],{revision:2,state:'Applied',result:'Face confirmed; sound unconfirmed'});
    await page.evaluate(()=>refreshConsole());
    assert.equal(await page.locator('.console-entry').count(),1);
    assert.equal(await page.locator('#consoleLog details').getAttribute('open'),'');
    assert.match(await page.locator('#consoleLog').textContent(),/Applied/);
    consoleData.revision=3;consoleData.newest_id=2;consoleData.events.push({id:2,revision:3,ms:1100,updated_ms:1100,session:2,source:'Gemini',call_id:'move-2',function:'move_forward',arguments:'distance_cm=10',action_reason:'',state:'Blocked',result:'No fresh observation',category:'Movement'});
    await page.evaluate(()=>refreshConsole());
    assert.match(await page.locator('#consoleLog').textContent(),/No reason provided/);
    await page.selectOption('#consoleFilter','Emotes');assert.equal(await page.locator('.console-entry').count(),1);
    await page.selectOption('#consoleFilter','Movement');assert.match(await page.locator('#consoleLog').textContent(),/No fresh observation/);
    await page.selectOption('#consoleFilter','All actions');
    await page.locator('#consoleFollow').click();assert.equal(await page.locator('#consoleFollow').textContent(),'Resume scrolling');
    const exported=JSON.parse(await page.evaluate(()=>consoleExport()));assert.equal(exported.events.length,2);
    const download=page.waitForEvent('download');await page.locator('#consoleDownload').click();assert.equal((await download).suggestedFilename(),'digit-actions.json');
    consoleData={available:true,boot_id:456,revision:3,oldest_id:1,newest_id:1,events:[{...consoleData.events[0],id:1,revision:1,state:'Applied',action_reason:'New session after reboot'}]};
    await page.evaluate(()=>refreshConsole());assert.equal(await page.locator('.console-entry').count(),1);
    assert.match(await page.locator('#consoleMessage').textContent(),/restarted/);
    offline=true;await page.evaluate(()=>refreshConsole());assert.equal(await page.locator('.console-entry').count(),1);
    assert.match(await page.locator('#consoleMessage').textContent(),/last history retained/);offline=false;
    consoleData.revision=4;consoleData.oldest_id=2;consoleData.newest_id=2;consoleData.events=[{...consoleData.events[0],id:2,revision:4}];
    await page.evaluate(()=>refreshConsole());assert.match(await page.locator('#consoleMessage').textContent(),/Older actions expired/);

    // Broad expression coverage, including EDGE without motor readiness.
    const names = ['NEUTRAL','HAPPY','SAD','MAD','SERIOUS','CURIOUS','SURPRISED','SLEEPING','CONFUSED','SLEEPY','EXCITED','CRYING','LOVE'];
    assert.equal(await page.locator('[data-face]').count(), names.length);
    for (const name of names) {
      await page.locator(`[data-face="${name}"]`).click();
      await page.waitForFunction(() => !facePending);
    }
    assert.deepEqual(faces, names);
    Object.assign(status, {ready:false, edge:true, fault:'EDGE', edge_reverse_allowed:true});
    await refresh();
    await page.locator('[data-face="HAPPY"]').click();
    await page.waitForFunction(() => !facePending);
    assert.equal(faces.at(-1), 'HAPPY');
    assert.equal(await page.locator('[data-drive="back"]').isEnabled(), true);
    for (const direction of ['forward','left','right'])
      assert.equal(await page.locator(`[data-drive="${direction}"]`).isEnabled(), false);
    await page.evaluate(() => beginDrive('back', {preventDefault(){}, currentTarget:{setPointerCapture(){}}, pointerId:1}));
    await page.waitForFunction(() => !driveInFlight);
    assert.ok(drives.length > 0 && drives.every(query => Number(query.get('left')) < 0 && query.get('left') === query.get('right')));
    await page.evaluate(() => stopZumo());
    Object.assign(status, {ready:true, edge:false, fault:'NONE'});
    for (const busy of [{left:100}, {calibrating:true}, {look_active:true}, {manual_driving:true}, {connected:false}]) {
      const previous = {...status}; Object.assign(status, busy); await refresh();
      assert.equal(await page.locator('[data-face="HAPPY"]').isEnabled(), false);
      Object.assign(status, previous);
    }
    await refresh();

    // Stop must invalidate requests that were already in flight.
    for (const stop of ['stopFacePreview', 'stopZumo']) {
      await page.waitForFunction(() => faceAvailable && !facePending && !faceStopping && !stopPending);
      deferredPath = '/api/zumo/face'; const waiting = new Promise(resolve => reached = resolve);
      const pending = page.evaluate(() => previewFace('LOVE'));
      await waitForStalled(waiting); const count = faces.length;
      await page.evaluate(name => window[name](), stop);
      await command(stalled); await pending;
      assert.equal(faces.length, count);
    }
    await page.locator('#batteryToggle').click();
    await page.waitForFunction(() => !batteryPending);
    assert.equal(enabled, true);
    for (const label of ['Battery','Marker','Screwdriver'])
      assert.match(await page.locator('#batteryState').textContent(), new RegExp(label));
    await page.locator('#batteryToggle').click();
    await page.waitForFunction(() => !batteryPending);
    assert.equal(enabled, false);
    deferredPath = '/api/vision/objects'; const waiting = new Promise(resolve => reached = resolve);
    const pending = page.evaluate(() => toggleBattery());
    await waitForStalled(waiting); await page.evaluate(() => stopZumo(true, 'stop-all'));
    await command(stalled); await pending;
    assert.equal(enabled, false);
    offline = true; await refresh();
    assert.equal(await page.locator('[data-face="HAPPY"]').isEnabled(), false);
    assert.equal(await page.locator('#batteryToggle').isEnabled(), false);
    assert.deepEqual(errors, []);
    console.log('PASS: action console live updates, reasons, filters, pause/export, reboot/retention/offline recovery; expressions, EDGE/busy gates, reverse-only drive and Stop cancellation');
  } finally {await browser.close();}
})().catch(error => {console.error(error); process.exitCode = 1;});
