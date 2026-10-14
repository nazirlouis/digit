// Exercise the actual three-section dashboard against simulated endpoints only.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');
const source = fs.readFileSync(path.join(__dirname, '../src/web_server.cpp'), 'utf8');
const html = source.split('R"HTML(')[1].split(')HTML"')[0];
const header = fs.readFileSync(path.join(__dirname, '../src/gemini_prompt.h'), 'utf8');
const defaults = header.match(/kDefaultSystemInstructions\[\]\s*=\s*R"PROMPT\(([\s\S]*?)\)PROMPT"/)[1].replace(/\r\n?/g,'\n');
const legacy = JSON.parse(source.match(/geminiLegacyInstructions=("(?:\\.|[^"\\])*");/)[1]);
const heading = '## Personality and behavior\n', divider = '\n\n## Base information\n', voiceDivider = '\n\n## Expression and emotions\n';
const at = defaults.indexOf(divider), voiceAt = defaults.indexOf(voiceDivider);
const personality = defaults.slice(heading.length, at), base = defaults.slice(at + divider.length, voiceAt), defaultVoice = defaults.slice(voiceAt + voiceDivider.length);
const combine = (p,b,v='') => heading+p+divider+b+voiceDivider+v;
const folder = path.join(__dirname, '../test-artifacts/instructions');

(async () => {
  fs.mkdirSync(folder, { recursive: true });
  const browser = await chromium.launch({ headless: true, channel: process.env.DIGIT_TEST_BROWSER || 'msedge' });
  try {
    const page = await browser.newPage();
    const errors = [], saves = [];
    let failSave = false, ignoreSave = false, stallSave = false, failRead = false;
    let stalledRoute;
    const settings = {configured:true, model:'gemini-robotics-er-2-streaming-preview', dry_run:false, idle_timeout_sec:30,
      generation:1, system_instructions:defaults.replace(/\n/g,'\r\n'), default_system_instructions:defaults, system_instructions_storage_bytes:0x1d0000};
    page.on('pageerror', error => {errors.push(error.message);console.error('PAGE ERROR:',error.message);});
    await page.route('**/*', async route => {
      const request = route.request(), url = new URL(request.url());
      if (url.pathname === '/') return route.fulfill({ contentType:'text/html', body:html });
      if (url.pathname === '/stream') return route.abort();
      if (url.pathname === '/api/gemini/settings') {
        if (request.method() === 'POST') {
          const body = request.postDataJSON(); saves.push(body);
          if (stallSave) { stalledRoute = route; return; }
          if (failSave) return route.fulfill({status:500, contentType:'application/json', body:JSON.stringify({message:'Could not save settings', generation:settings.generation})});
          assert.equal(body.generation, settings.generation);
          settings.generation++;
          if (body.remove_key) settings.configured = false;
          if (!ignoreSave && typeof body.system_instructions === 'string') settings.system_instructions = body.system_instructions || defaults;
          return route.fulfill({contentType:'application/json', body:JSON.stringify({message:'Gemini settings saved on Digit', generation:settings.generation})});
        }
        if (failRead) return route.fulfill({status:503, contentType:'application/json', body:JSON.stringify({message:'Settings unavailable', generation:settings.generation})});
        return route.fulfill({contentType:'application/json', body:JSON.stringify(settings)});
      }
      return route.fulfill({contentType:'application/json', body:JSON.stringify({generation:settings.generation,
        active:false, task_active:false, action:'Idle', phase:'Stopped', connected:true, ready:true,
        left:0, right:0, fault:'NONE', session_ready:true, local_stop_ready:true,
        live_active:false, live_connected:true, live_probe:false, live_dry_run:false,
        speaker_volume:100, events:[]})});
    });
    const editor = page.locator('#geminiPersonality'), facts = page.locator('#geminiBaseInfo'), voice = page.locator('#geminiEmotionInstructions');
    async function ready() {
      await page.waitForFunction(() => geminiSettingsLoaded && !document.querySelector('#geminiSave').disabled);
      await page.locator('#session .help').evaluate(el => el.open = true);
    }
    async function save() { await page.locator('#geminiSave').click(); await page.waitForFunction(() => !geminiPending); }
    await page.goto('http://digit-preview.test/'); await ready();
    assert.equal(await editor.inputValue(), personality);
    assert.equal(await facts.inputValue(), base);
    assert.equal(await voice.inputValue(), defaultVoice);
    const newlineVariants=[defaults,defaults.replace(/\n/g,'\r\n'),defaults.replace(/\n/g,'\r'),defaults.replace(/\n/g,(line,index)=>index%2?'\r\n':'\n')];
    for(const saved of newlineVariants){
      settings.system_instructions=saved;await page.reload();await ready();
      assert.equal(await editor.inputValue(),personality);
      assert.equal(await facts.inputValue(),base,'Saved Base information loads with every supported line ending');
      assert.equal(await voice.inputValue(),defaultVoice);
      assert.equal(settings.system_instructions,saved,'Loading never rewrites saved instructions');
      assert.equal(saves.length,0,'Loading sections never saves automatically');
    }
    // Earlier loaders put the entire CRLF document into the personality field.
    // Recover that tab draft without discarding any unsaved text it contains.
    const packedDraft=combine('Unsaved personality edit',base,'Unsaved voice edit').replace(/\n/g,'\r\n');
    await page.evaluate(draft=>sessionStorage.setItem(geminiDraftKey,JSON.stringify({personality:draft,base:'',emotion:''})),packedDraft);
    await page.reload();await ready();
    assert.equal(await editor.inputValue(),'Unsaved personality edit');
    assert.equal(await facts.inputValue(),base);
    assert.equal(await voice.inputValue(),'Unsaved voice edit');
    assert.equal(saves.length,0);
    await page.evaluate(()=>sessionStorage.clear());settings.system_instructions=defaults;
    await page.reload();await ready();
    assert.match(await page.locator('#geminiInstructionCount').textContent(), /bytes total across all three sections/);
    for (const field of [editor, facts, voice]) assert.equal(await field.getAttribute('maxlength'), null);
    const custom = 'Speak warmly.\nSay "hello". Robot: 🤖\n<script>window.unwanted = true</script>';
    const customBase = 'I am a robot.\nMy camera takes still frames.\nKeep literal text: <b>facts</b>.';
    const customVoice = 'Use a playful robot voice.\nSpeak slowly; pronounce "Digit" clearly. 🤖\n<script>window.unwanted = true</script>';
    await editor.fill(custom); await facts.fill(customBase); await voice.fill(customVoice);
    await page.evaluate(() => refreshGeminiSettings());
    assert.equal(await editor.inputValue(), custom, 'Refresh preserves unsaved personality');
    assert.equal(await facts.inputValue(), customBase, 'Refresh preserves unsaved base information');
    assert.equal(await voice.inputValue(), customVoice, 'Refresh preserves unsaved emotion instructions');
    const beforeDraftReload = saves.length;
    await page.reload(); await ready();
    assert.equal(await editor.inputValue(), custom, 'Page reload recovers the unsaved personality draft');
    assert.equal(await facts.inputValue(), customBase, 'Page reload recovers the unsaved facts draft');
    assert.equal(await voice.inputValue(), customVoice, 'Page reload recovers the unsaved voice draft');
    assert.equal(saves.length, beforeDraftReload, 'Draft recovery never saves automatically');
    assert.match(await page.locator('#geminiMessage').textContent(), /draft restored/i);
    await save();
    assert.equal(saves.at(-1).system_instructions, combine(custom, customBase, customVoice));
    assert.equal(saves.at(-1).api_key, '', 'Instructions retain the stored key');
    assert.equal(saves.at(-1).dry_run, false, 'Preview mode retained');
    assert.equal(saves.at(-1).idle_timeout_sec, 30, 'Timeout retained');
    await page.reload(); await ready();
    assert.equal(await editor.inputValue(), custom); assert.equal(await facts.inputValue(), customBase);
    assert.equal(await voice.inputValue(), customVoice);
    assert.equal(await page.evaluate(() => sessionStorage.length), 0, 'Verified save clears the draft');
    assert.equal(await page.evaluate(() => window.unwanted), undefined, 'Instructions render as text');
    const beforeReset = saves.length;
    await page.locator('#geminiPersonalityDefault').click();
    assert.equal(await editor.inputValue(), personality); assert.equal(await facts.inputValue(), customBase, 'Personality restore preserves base');
    assert.equal(await voice.inputValue(), customVoice, 'Personality restore preserves voice');
    assert.equal(saves.length, beforeReset, 'Restore waits for Save');
    await editor.fill(custom);
    await page.locator('#geminiBaseDefault').click();
    assert.equal(await facts.inputValue(), base); assert.equal(await editor.inputValue(), custom, 'Base restore preserves personality');
    assert.equal(await voice.inputValue(), customVoice, 'Base restore preserves voice');
    await page.locator('#geminiEmotionDefault').click();
    assert.equal(await voice.inputValue(), defaultVoice);
    assert.equal(await editor.inputValue(), custom, 'Voice restore preserves personality');
    assert.equal(await facts.inputValue(), base, 'Voice restore preserves base');
    assert.equal(saves.length, beforeReset, 'Voice restore waits for Save');
    await voice.fill(customVoice);
    await save(); assert.equal(settings.system_instructions, combine(custom, base, customVoice));
    failSave = true;
    await editor.fill('An unsaved personality'); await facts.fill('Unsaved facts'); await voice.fill('Unsaved voice'); await save();
    assert.equal(await editor.inputValue(), 'An unsaved personality'); assert.equal(await facts.inputValue(), 'Unsaved facts');
    assert.equal(await voice.inputValue(), 'Unsaved voice');
    assert.equal(settings.system_instructions, combine(custom, base, customVoice)); failSave = false;
    await page.reload(); await ready();
    assert.equal(await voice.inputValue(), 'Unsaved voice', 'Failed save survives page refresh');
    ignoreSave = true;
    await save();
    assert.match(await page.locator('#geminiMessage').textContent(), /not confirmed/i, 'Success is checked against saved text');
    assert.equal(await voice.inputValue(), 'Unsaved voice', 'Readback mismatch retains the editor');
    ignoreSave = false;
    failRead = true;
    await voice.fill('Readback interrupted'); await save();
    assert.match(await page.locator('#geminiMessage').textContent(), /unavailable/i);
    assert.equal(await voice.inputValue(), 'Readback interrupted', 'Failed verification retains the draft');
    failRead = false;
    await page.reload(); await ready();
    assert.equal(await voice.inputValue(), 'Readback interrupted', 'A committed save is recovered even when its readback failed');
    assert.equal(await page.evaluate(() => sessionStorage.length), 0, 'Draft matching the saved result is cleared');
    await voice.fill('Unsaved voice');
    // A dropped request gives immediate feedback, releases the form, and retains the draft.
    await page.clock.install();
    stallSave = true;
    await page.locator('#geminiSave').click();
    await page.waitForFunction(() => document.querySelector('#geminiMessage').textContent.includes('Saving'));
    assert.match(await page.locator('#geminiSave').textContent(), /Saving/);
    await page.clock.runFor(10500);
    await page.waitForFunction(() => !geminiPending);
    assert.match(await page.locator('#geminiMessage').textContent(), /timed out/i);
    assert.equal(await voice.inputValue(), 'Unsaved voice');
    if (stalledRoute) await stalledRoute.abort().catch(() => {});
    stallSave = false;
    await save();
    assert.match(await page.locator('#geminiMessage').textContent(), /verified/i);
    await page.reload(); await ready();
    assert.equal(await voice.inputValue(), 'Unsaved voice');
    // The former 4096-byte boundary and substantially longer Unicode documents all save intact.
    const overhead = Buffer.byteLength(combine('', ''));
    await editor.fill('Personality'); await facts.fill('Facts');
    await voice.fill('🤖'.repeat(100)+'x'.repeat(4096-overhead-400-16)); await save();
    assert.equal(Buffer.byteLength(settings.system_instructions),4096);
    await voice.fill((await voice.inputValue())+'x'); await save();
    assert.equal(Buffer.byteLength(settings.system_instructions),4097);
    const longVoice = customVoice.repeat(1500), longFacts = customBase.repeat(300);
    await voice.fill(longVoice); await facts.fill(longFacts); await save();
    assert.equal(settings.system_instructions, combine('Personality', longFacts, longVoice));
    assert.ok(Buffer.byteLength(settings.system_instructions)>128*1024);
    await page.reload(); await ready();
    assert.equal(await voice.inputValue(), longVoice); assert.equal(await facts.inputValue(), longFacts);
    const beforeOversize = saves.length;
    await voice.fill('Voice');
    await editor.fill('Notes'+divider+'Ambiguous section'); await facts.fill('Facts'); await save();
    assert.equal(saves.length, beforeOversize, 'Reserved divider cannot corrupt the section boundary');
    assert.match(await page.locator('#geminiMessage').textContent(), /reserved/);
    await page.reload(); await ready();
    assert.equal(await editor.inputValue(), 'Notes'+divider+'Ambiguous section', 'Invalid section text stays intact in the recovered draft');
    assert.equal(await facts.inputValue(), 'Facts');
    await editor.fill('Notes'+voiceDivider+'Ambiguous voice'); await save();
    assert.equal(saves.length, beforeOversize, 'Personality cannot contain the voice divider');
    await editor.fill('Notes'); await facts.fill('Facts'+voiceDivider+'Ambiguous voice'); await save();
    assert.equal(saves.length, beforeOversize, 'Base cannot contain the voice divider');
    assert.match(await page.locator('#geminiMessage').textContent(), /reserved/);
    await editor.fill(''); await facts.fill(''); await voice.fill(''); await save(); await page.reload(); await ready();
    assert.equal(await editor.inputValue(),''); assert.equal(await facts.inputValue(),''); assert.equal(await voice.inputValue(),'');
    // The existing two-section format loads without rewriting either saved field.
    const oldTwoSections = heading+custom+divider+customBase;
    settings.system_instructions=oldTwoSections; const beforeVoiceMigration=saves.length;
    await page.reload(); await ready();
    assert.equal(await editor.inputValue(),custom); assert.equal(await facts.inputValue(),customBase);
    assert.equal(await voice.inputValue(),''); assert.equal(saves.length,beforeVoiceMigration);
    assert.equal(settings.system_instructions,oldTwoSections);
    await voice.fill(customVoice); await save(); await page.reload(); await ready();
    assert.equal(settings.system_instructions,combine(custom,customBase,customVoice));
    assert.equal(await editor.inputValue(),custom); assert.equal(await facts.inputValue(),customBase);
    assert.equal(await voice.inputValue(),customVoice);
    // Existing stock instructions are reorganized only in the editor until Save.
    settings.system_instructions=legacy; const beforeMigration=saves.length;
    await page.reload(); await ready();
    assert.equal(await editor.inputValue(),personality); assert.equal(await facts.inputValue(),base);
    assert.equal(await voice.inputValue(),defaultVoice);
    assert.equal(saves.length,beforeMigration); assert.equal(settings.system_instructions,legacy);
    await save(); assert.equal(settings.system_instructions,defaults);
    // Never discard or guess the boundary of an existing custom prompt.
    settings.system_instructions=custom;
    await page.reload(); await ready();
    assert.equal(await editor.inputValue(),custom); assert.equal(await facts.inputValue(),'');
    assert.equal(await voice.inputValue(),'');
    assert.equal(await page.locator('#geminiInstructionMigration').isVisible(),true);
    await save(); await page.reload(); await ready();
    assert.equal(await editor.inputValue(),custom); assert.equal(await facts.inputValue(),'');
    assert.equal(await page.locator('#geminiInstructionMigration').isVisible(),false);
    await page.locator('#geminiPersonalityDefault').click(); await page.locator('#geminiBaseDefault').click(); await page.locator('#geminiEmotionDefault').click(); await save();
    for (const width of [1280, 390, 320]) {
      await page.setViewportSize({width, height:1000});
      assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'Editors fit screen');
      await page.locator('#session').scrollIntoViewIfNeeded();
      await page.locator('.top').evaluate(el => el.style.position = 'static');
      await editor.evaluate(el => el.scrollTop = 0); await facts.evaluate(el => el.scrollTop = 0);
      await page.locator('#session .help').screenshot({path:path.join(folder, `instructions-${width}.png`)});
      await page.locator('#geminiEmotionInstructions').scrollIntoViewIfNeeded();
      await page.screenshot({path:path.join(folder, `voice-instructions-${width}.png`)});
    }
    assert.deepEqual(errors, []);
    console.log('PASS: verified save/reload beyond 128 KiB, no editor maxlength, draft recovery, failed/mismatched/stalled saves, independent restore, migrations, Unicode, literal text, key/policy retention and responsive editors');
  } finally { await browser.close(); }
})().catch(error => {console.error(error); process.exitCode = 1;});
