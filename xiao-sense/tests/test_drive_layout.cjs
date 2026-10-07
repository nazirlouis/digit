// Render the real dashboard against mocked device endpoints; never contact Digit.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');
const source = fs.readFileSync(path.join(__dirname, '../src/web_server.cpp'), 'utf8');
const html = source.split('R"HTML(')[1].split(')HTML"')[0];
const artifacts = path.join(__dirname, '../test-artifacts/drive');

(async () => {
  fs.mkdirSync(artifacts, { recursive: true });
  const browser = await chromium.launch({ headless: true, channel: process.env.DIGIT_TEST_BROWSER || 'msedge' });
  try {
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.route('**/*', async route => {
      const url = new URL(route.request().url());
      if (url.pathname === '/') return route.fulfill({ contentType: 'text/html', body: html });
      if (url.pathname === '/stream') return route.abort();
      return route.fulfill({ contentType: 'application/json', body: JSON.stringify({
        generation: 1, active: false, action: 'Idle', phase: 'Stopped',
        voice_ready: true, drive_ready: true, connected: true, ready: true,
        identify_ready: true, person_action_ready: true, object_action_ready: true,
        fault: 'NONE', last_error: 'NONE', battery_mv: 7600, left: 0, right: 0,
        distance_mm:50, range_valid:true, range_age_ms:10,
        audio_state: 'READY', audio_available: true, camera_ready: true, vision_ready: true,
        vision_enabled: true, person_detected:true, vision_age_ms:10, vision_capture_age_ms:10, follow_enabled: false, follow_state: 'Off',
        captured_ms: 0, max_recording_ms: 10000, psram_free: 4000000,
        ip: '10.0.0.127', volume: 70, available: true, state: 'Listening',
      }) });
    });
    await page.goto('http://digit-preview.test/');
    assert.equal(await page.locator('#driveAcceleration').inputValue(),'6000');
    for (const width of [1280, 980, 390, 320]) {
      await page.setViewportSize({ width, height: 1000 });
      const rects = await page.evaluate(() => {
        const rect = selector => {
          const r = document.querySelector(selector).getBoundingClientRect();
          return { x: r.x, y: r.y, right: r.right, bottom: r.bottom };
        };
        return { camera: rect('.video'), drive: rect('#zumoDrive'),
          distance: rect('#distanceBadge'), person: rect('#personBadge'),
          speed: rect('[for="driveSpeed"]'), acceleration: rect('[for="driveAcceleration"]'),
          scrollWidth: document.documentElement.scrollWidth };
      });
      assert.ok(rects.scrollWidth <= width, `No horizontal overflow at ${width}px`);
      assert.equal(await page.locator('#distanceBadge').textContent(),'ToF: 5.0 cm');
      assert.ok(rects.distance.x>=rects.camera.x && rects.distance.right<=rects.camera.right && rects.distance.bottom<=rects.camera.bottom,'Distance badge stays within the camera');
      assert.ok(rects.distance.x>=rects.person.right || rects.distance.y>=rects.person.bottom,'Distance and person badges do not overlap');
      assert.ok(rects.acceleration.y >= rects.speed.bottom, 'Acceleration is below Drive speed');
      if (width > 900) {
        assert.ok(rects.drive.x >= rects.camera.right, 'Drive is beside camera');
        assert.equal(rects.drive.y, rects.camera.y);
      } else {
        assert.ok(rects.drive.y >= rects.camera.bottom, 'Drive follows camera on phones');
      }
      if (width === 1280 || width === 390) {
        await page.locator('.video').screenshot({path:path.join(artifacts, `camera-distance-${width}.png`)});
        await page.screenshot({ path: path.join(artifacts, `dashboard-${width}.png`) });
        const facePanel = page.locator('.zumo').filter({has:page.locator('#facePreviewTitle')});
        assert.equal(await facePanel.locator('[data-face]').count(),13);
        assert.equal(await page.locator('#faceVoice').isChecked(),true);
        await facePanel.screenshot({path:path.join(artifacts, `face-voice-${width}.png`)});
        const actions = page.locator('.zumo').filter({has:page.locator('#routineState')});
        assert.equal(await actions.locator('[data-routine]').count(),9);
        await actions.screenshot({path:path.join(artifacts, `actions-${width}.png`)});
      }
    }
    assert.deepEqual(errors, [], 'Dashboard loads without script errors');
    console.log('PASS: desktop and phone layouts at 1280, 980, 390, and 320px; 13 face/voice cards; acceleration position; no overflow');
  } finally {
    await browser.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
