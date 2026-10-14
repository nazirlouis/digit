// Verify the actual dashboard with simulated endpoints and no device access.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');
const source = fs.readFileSync(path.join(__dirname, '../src/web_server.cpp'), 'utf8');
const html = source.split('R"HTML(')[1].split(')HTML"')[0];
const folder = path.join(__dirname, '../test-artifacts/chat');
const camera = '<svg xmlns="http://www.w3.org/2000/svg" width="640" height="480"><rect width="640" height="480" fill="#152422"/><text x="320" y="210" text-anchor="middle" fill="#c3d5cc" font-family="Segoe UI" font-size="20">Camera preview</text><text x="320" y="240" text-anchor="middle" fill="#c3d5cc" font-family="Segoe UI" font-size="14">Simulated image · no device connected</text></svg>';

(async () => {
  fs.mkdirSync(folder, {recursive:true});
  const browser = await chromium.launch({headless:true, channel:process.env.DIGIT_TEST_BROWSER || 'msedge'});
  try {
    const page = await browser.newPage(), errors = [], messages = [], images = [], stops = [];
    let failChat = false, failImage = false, deferChat = null;
    const status = {generation:10, active:false, live_active:false, live_connected:true,
      live_probe:false, live_text_only:false, live_text_pending:false, live_thinking:false,
      live_speaking:false, live_response_id:0, live_response:'', last_image_id:0,
      last_image_age_ms:1000, phase:'Stopped', connected:true, ready:true, calibrated:true,
      session_ready:true, camera_ready:true, audio_available:true, fault:'NONE',
      range_valid:true, range_age_ms:10, distance_mm:250, events:[], speaker_volume:100};
    page.on('pageerror', error => {errors.push(error.message);console.error('PAGE ERROR:',error.message);});
    await page.route('**/*', async route => {
      const request = route.request(), url = new URL(request.url());
      const reply = (data, code=200) => route.fulfill({status:code,contentType:'application/json',body:JSON.stringify(data)});
      if (url.pathname === '/') return route.fulfill({contentType:'text/html',body:html});
      if (url.pathname === '/stream') return route.fulfill({contentType:'image/svg+xml',body:camera});
      if (url.pathname === '/api/gemini/settings') return reply({configured:true,model:'gemini-robotics-er-2-streaming-preview',dry_run:true,idle_timeout_sec:30,generation:status.generation,system_instructions:'You are Digit.',default_system_instructions:'You are Digit.',max_system_instructions_bytes:4096});
      if (url.pathname === '/api/gemini/last-image') {
        images.push(url.searchParams.get('frame'));
        if (failImage) return reply({message:'Image unavailable'},503);
        return route.fulfill({contentType:'image/svg+xml',body:camera.replace('Camera preview','Sent frame '+url.searchParams.get('frame'))});
      }
      if (url.pathname === '/api/gemini/chat') {
        messages.push(request.postDataJSON());
        if (deferChat) await deferChat;
        if (failChat) return reply({generation:status.generation,message:'Digit is busy'},409);
        status.live_active=true; status.live_text_only=true;
        return reply({generation:status.generation,message:'Message queued on Digit'});
      }
      if (url.pathname === '/api/zumo/stop') {
        stops.push(url.searchParams);
        status.generation++;status.live_active=false;status.live_thinking=false;
        if(url.searchParams.get('source')==='stop-all') {
          status.live_connected=false;status.live_connecting=true;
          status.live_connection_status='Opening fresh Gemini connection';
        }
        return reply({generation:status.generation,message:'All actions cancelled; stopping motors and opening a fresh Gemini connection'});
      }
      return reply(status);
    });
    await page.goto('http://digit-preview.test/');
    await page.waitForFunction(() => geminiConfigured && geminiStatusAvailable).catch(async e=>{console.error(await page.locator('#geminiKeyState').textContent());console.error(errors);throw e;});
    status.live_connected=false;status.live_connecting=true;status.live_attempts=3;
    status.live_connection_status='Sending Gemini instructions and tools';status.live_attempt_age_ms=2300;
    status.session_error='Could not send Live setup';
    await page.evaluate(() => refreshActions());
    assert.equal(await page.locator('#geminiState').textContent(),'Connecting');
    assert.match(await page.locator('#geminiActivityDetail').textContent(),/Sending Gemini instructions and tools/);
    assert.match(await page.locator('#geminiDetail').textContent(),/attempt 3.*2 s.*Last issue: Could not send Live setup/);
    assert.equal(await page.locator('#commandState').textContent(),'Sending Gemini instructions and tools');
    await page.locator('#session .status-line').screenshot({path:path.join(folder,'connection-progress.png')});
    status.live_probe=true;status.live_attempt_age_ms=0;status.live_connection_status='Retrying Gemini in 2 s';
    await page.evaluate(() => refreshActions());
    assert.equal(await page.locator('#geminiState').textContent(),'Connecting');
    assert.match(await page.locator('#geminiActivityDetail').textContent(),/Retrying Gemini in 2 s/);
    status.live_probe=false;status.live_connected=true;status.live_connecting=false;status.session_error='';
    status.live_connection_status='Ready';status.live_last_error='Could not send Live setup';
    await page.evaluate(() => refreshActions());
    assert.equal(await page.locator('#geminiState').textContent(),'Ready');
    assert.doesNotMatch(await page.locator('#geminiDetail').textContent(),/Last issue/);
    assert.equal(await page.locator('#geminiImage').isVisible(),false);
    assert.match(await page.locator('#geminiImageDetail').textContent(),/No image sent yet/);
    assert.equal(images.length,0);
    assert.equal(await page.locator('#chatSend').isEnabled(),false);
    const text='Hi Digit! <script>window.unwanted=true</script> 🤖';
    await page.locator('#chatInput').fill(text);
    await page.locator('#chatInput').press('Enter');
    await page.waitForFunction(() => !chatPending);
    assert.deepEqual(messages,[{text,generation:10}]);
    assert.equal(await page.locator('#chatInput').inputValue(),'');
    assert.equal(await page.locator('#chatLog .user span').textContent(),text);
    assert.equal(await page.evaluate(() => window.unwanted),undefined);
    status.live_response='Hello Naz';status.live_response_id=1;
    await page.evaluate(() => refreshActions());
    status.live_response+='!';
    await page.evaluate(() => refreshActions());
    assert.equal(await page.locator('#chatLog .digit').count(),1);
    assert.equal(await page.locator('#chatLog .digit span').textContent(),'Hello Naz!');
    status.live_response_id=2;await page.evaluate(() => refreshActions());
    assert.equal(await page.locator('#chatLog .digit').count(),2,'Same text on a new turn remains a separate reply');
    await page.locator('#chatInput').fill('line one');
    await page.locator('#chatInput').press('Shift+Enter');
    await page.locator('#chatInput').type('line two');
    assert.equal(await page.locator('#chatInput').inputValue(),'line one\nline two');
    assert.equal(messages.length,1);
    failChat=true;await page.locator('#chatSend').click();await page.waitForFunction(() => !chatPending);
    assert.equal(await page.locator('#chatInput').inputValue(),'line one\nline two');
    assert.equal(await page.locator('#chatLog .user').count(),1);
    failChat=false;
    await page.locator('#chatInput').fill('🤖'.repeat(300));
    const beforeLimit=messages.length;
    await page.locator('#chatSend').click();await page.waitForFunction(() => !chatPending);
    assert.equal(messages.length,beforeLimit,'Decoded UTF-8 byte limit applies before HTTP');
    status.live_text_pending=true;await page.evaluate(() => refreshActions());
    assert.equal(await page.locator('#chatSend').isEnabled(),false);
    status.live_text_pending=false;
    status.last_image_id=1;await page.evaluate(() => refreshActions());
    await page.waitForFunction(() => lastImageId===1);
    assert.equal(await page.locator('#geminiImage').isVisible(),true);
    assert.deepEqual(images,['1']);
    await page.evaluate(() => refreshActions());assert.deepEqual(images,['1'],'Unchanged frame never fetches again');
    await page.locator('#globalStop').click();
    await page.waitForFunction(() => !stopPending);
    assert.equal(stops.at(-1).get('source'),'stop-all');
    assert.equal(stops.at(-1).get('hard'),'1');
    await page.evaluate(() => refreshActions());
    assert.equal(await page.locator('#geminiState').textContent(),'Reconnecting');
    assert.match(await page.locator('#geminiActivityDetail').textContent(),/Opening fresh Gemini connection/);
    status.live_connected=true;status.live_connecting=false;status.live_connection_status='Ready';
    await page.evaluate(() => refreshActions());
    assert.equal(await page.locator('#geminiState').textContent(),'Ready');
    assert.equal(status.live_active,false);
    assert.equal(await page.locator('#geminiImage').isVisible(),true,'End/Stop retains last sent photo');
    status.last_image_id=2;failImage=true;await page.evaluate(() => refreshActions());
    await page.waitForFunction(() => !imagePending);
    assert.equal(await page.evaluate(() => lastImageId),1,'Failed fetch retains exact earlier frame');
    failImage=false;await page.evaluate(() => refreshActions());
    await page.waitForFunction(() => lastImageId===2);
    status.task_active=true;status.task_moves=12;status.task_max_moves=50;status.task_phase='Moving';
    await page.evaluate(() => refreshActions());
    assert.match(await page.locator('#taskProgress').textContent(),/12 of 50 moves/);
    // Steering can queue while the active task moves or replies; one pending
    // message still blocks a second send, and unrelated motion stays blocked.
    status.active=true;status.live_thinking=true;status.live_speaking=true;
    await page.locator('#chatInput').fill('Try the left side');
    await page.evaluate(() => refreshActions());
    assert.equal(await page.locator('#chatSend').isEnabled(),true);
    status.live_text_pending=true;await page.evaluate(() => refreshActions());
    assert.equal(await page.locator('#chatSend').isEnabled(),false);
    status.live_text_pending=false;status.task_active=false;
    await page.evaluate(() => refreshActions());
    assert.equal(await page.locator('#chatSend').isEnabled(),false);
    status.active=status.live_thinking=status.live_speaking=false;
    status.task_active=false;
    // Keep the final layout screenshots readable after the escaping assertions.
    await page.evaluate(() => {$('chatLog').replaceChildren();chatEntry('user','Hi Digit!');chatResponseNode=chatEntry('digit','Hello Naz!');});
    await page.locator('#chatInput').fill('What do you see?');
    for(const width of [1280,980,390,320]) {
      await page.setViewportSize({width,height:1400});
      await page.evaluate(() => scrollTo(0,0));
      const rects=await page.evaluate(() => {
        const rect = selector => {const r=document.querySelector(selector).getBoundingClientRect();return {x:r.x,y:r.y,right:r.right,bottom:r.bottom};};
        return {camera:rect('.video'),inset:rect('.gemini-image'),cameraLabel:rect('#camera'),chat:rect('.digit-chat'),input:rect('#chatInput'),send:rect('#chatSend'),scroll:document.documentElement.scrollWidth};
      });
      assert.ok(rects.scroll<=width,'No horizontal overflow at '+width);
      assert.ok(rects.inset.x>=rects.camera.x && rects.inset.right<=rects.camera.right && rects.inset.bottom<=rects.camera.bottom,'Inset stays inside camera');
      assert.ok(rects.inset.x<rects.camera.x+25 && rects.inset.bottom<=rects.cameraLabel.y,'Inset is bottom-left above camera status');
      assert.ok(rects.chat.y>=rects.camera.bottom && rects.send.right<=rects.chat.right,'Chat fits beneath camera');
      if(width===1280 || width===390) await page.screenshot({path:path.join(folder,'chat-'+width+'.png'),fullPage:false});
    }
    assert.deepEqual(errors,[]);
    console.log('PASS: typed send/Enter/newline/UTF-8/draft/XSS, incremental replies, busy and Stop, exact versioned photo/retention/retry, responsive camera and chat');
  } finally {await browser.close();}
})().catch(error => {console.error(error);process.exitCode=1;});
