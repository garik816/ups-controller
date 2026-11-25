// Optional browser smoke test. Requires Playwright on the development machine.
// All HTTP requests are intercepted locally; no UPS or external server is used.
'use strict';
const { chromium } = require('playwright');
const fs = require('node:fs');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const assert = require('node:assert/strict');

async function launchBrowser() {
  // A normal Playwright browser is the default. These optional overrides also
  // allow a locally installed Chromium or a serverless Chromium distribution.
  if (process.env.UPS_CHROMIUM_MODULE) {
    const { default: binary } = await import(pathToFileURL(process.env.UPS_CHROMIUM_MODULE).href);
    return chromium.launch({ headless: true, executablePath: await binary.executablePath(), args: binary.args });
  }
  return chromium.launch({
    headless: true,
    ...(process.env.UPS_CHROMIUM_EXECUTABLE ? { executablePath: process.env.UPS_CHROMIUM_EXECUTABLE } : {})
  });
}

(async () => {
  const html = fs.readFileSync(path.join(__dirname, '../web/index.html'), 'utf8');
  const screenshots = process.env.UPS_SCREENSHOT_DIR;
  if (screenshots) fs.mkdirSync(screenshots, { recursive: true });
  const browser = await launchBrowser();
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
    const status = {
      version: '1.0.0', csrf_token: 'initial-token', uptime_ms: 125000,
      free_heap: 29400, reset_reason: 'Power on', storage_ok: true,
      ups: {
        state: 'off', phase: 'finished', result: 'none', busy: false,
        relay: false, mains: true, online: false, battery: false,
        inputs_valid: true, auto_hold: false, cycle_handled: false,
        attempts: 0, remaining_ms: 0
      },
      network: {
        mode: 'standalone', ap_active: true, ap_ssid: 'UPS_wifi',
        ap_ip: '192.168.7.1', ap_clients: 1, sta_connected: false,
        sta_ssid: '', sta_ip: '0.0.0.0', rssi: 0, fallback: false,
        subnet_conflict: false
      }
    };
    const config = {
      network: {
        mode: 'standalone', ssid: 'home', has_password: true,
        ap_ssid: 'UPS_wifi', ap_ip: '192.168.7.1', fallback_seconds: 30
      },
      ups: {
        auto_mode: 'on_mains', start_delay_ms: 10000, press_ms: 2000,
        stop_press_ms: 2000, verify_timeout_ms: 15000,
        standby_filter_ms: 300, indicator_filter_ms: 300,
        off_confirm_ms: 2000, restart_delay_ms: 10000,
        retry_delay_ms: 60000, max_attempts: 1,
        auto_on_boot: true, allow_battery_start: false
      },
      inputs: { online_active_high: true, battery_active_high: true, standby_active_high: true },
      security: { username: 'admin' }
    };
    const writes = [], errors = [];
    let dialogs = 0, rejectedTokens = 0, offline = false, uploadReceived = false;
    let nextError = null;
    page.on('pageerror', error => errors.push(error.message));
    page.on('dialog', async dialog => { dialogs++; await dialog.accept(); });
    await page.route('http://ups.test/**', async route => {
      const request = route.request(), pathname = new URL(request.url()).pathname;
      if (pathname === '/') return route.fulfill({ contentType: 'text/html', body: html });
      if (offline) return route.abort('internetdisconnected');
      const reply = (body, code = 200) => route.fulfill({ status: code, contentType: 'application/json', body: JSON.stringify(body) });
      let body = {};
      if (request.method() === 'POST') {
        if (request.headers()['x-ups-token'] !== status.csrf_token) {
          rejectedTokens++;
          return reply({ ok: false, error: 'Сесію оновлено' }, 403);
        }
        if (pathname !== '/api/update') body = JSON.parse(request.postData() || '{}');
        else uploadReceived = request.postDataBuffer().toString().includes('name="firmware"');
        if (nextError && nextError.path === pathname) {
          const error = nextError; nextError = null;
          return reply({ ok: false, error: error.message }, error.status);
        }
        writes.push({ path: pathname, body });
      }
      let data = { ok: true, message: 'Готово' };
      if (pathname === '/api/status') data = status;
      if (pathname === '/api/config' && request.method() === 'GET') data = config;
      if (pathname === '/api/config' && request.method() === 'POST') {
        for (const key of Object.keys(body)) config[key] = { ...config[key], ...body[key] };
        if (body.network) {
          status.network.mode = body.network.mode;
          if (Object.hasOwn(body.network, 'password')) {
            config.network.has_password = !!body.network.password;
            delete config.network.password;
          }
        }
      }
      if (pathname === '/api/logs') data = { events: [
        { id: 1, uptime_ms: 5000, level: 'info', message: '<img src=x onerror="window.injected=true">' },
        { id: 2, uptime_ms: 15000, level: 'warning', message: 'Мережа зникла' }
      ] };
      if (pathname === '/api/action') {
        if (body.action === 'start') Object.assign(status.ups, { state: 'online', online: true, result: 'started' });
        if (body.action === 'stop') Object.assign(status.ups, { state: 'off', online: false, result: 'stopped', auto_hold: true });
        if (body.action === 'resume') Object.assign(status.ups, { auto_hold: false, cycle_handled: false });
        if (body.action === 'cancel') Object.assign(status.ups, { busy: false, relay: false, phase: 'finished', result: 'cancelled' });
      }
      if (pathname === '/api/scan' && request.method() === 'GET') data = { running: false, networks: [
        { ssid: '<script>alert(1)</script>', rssi: -40, secure: true },
        { ssid: 'Open', rssi: -70, secure: false }
      ] };
      if (pathname === '/api/update') {
        status.version = '1.0.1'; status.csrf_token = 'after-update'; status.uptime_ms = 500;
      }
      return reply(data);
    });
    await page.goto('http://ups.test/');
    await page.waitForFunction(() => document.querySelector('#connection_text').textContent === 'На зв’язку' && !document.querySelector('[data-action=start]').disabled);
    assert.equal(await page.locator('#log_list img').count(), 0);
    assert.equal(await page.evaluate(() => window.injected), undefined);
    if (screenshots) await page.screenshot({ path: path.join(screenshots, 'desktop.png'), fullPage: true });
    await page.setViewportSize({ width: 390, height: 844 });
    for (const tab of ['overview', 'automation', 'network', 'system']) {
      await page.locator('#tab_' + tab).click();
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true, 'Mobile overflow in ' + tab);
      if (screenshots && (tab === 'overview' || tab === 'network')) await page.screenshot({ path: path.join(screenshots, 'mobile-' + tab + '.png'), fullPage: true });
    }
    await page.setViewportSize({ width: 1280, height: 900 });
    await page.locator('#tab_overview').click();
    await page.locator('[data-action=start]').click();
    await page.waitForFunction(() => document.querySelector('#ups_title').textContent === 'Працює від мережі');
    await page.locator('[data-action=stop]').click();
    await page.waitForFunction(() => !document.querySelector('#hold_notice').hidden);
    assert.equal(dialogs, 1, 'Stop must require confirmation');

    // A token changed by an independent ESP reboot is refreshed before retry;
    // the successful command must still be sent exactly once.
    await page.evaluate(() => { polling = true; });
    status.csrf_token = 'refreshed-token';
    await page.locator('[data-action=resume]').click();
    await page.waitForFunction(() => document.querySelector('#hold_notice').hidden);
    await page.evaluate(() => { polling = false; });
    assert.equal(rejectedTokens, 1);
    assert.equal(writes.filter(x => x.path === '/api/action' && x.body.action === 'resume').length, 1);

    await page.locator('#tab_automation').click();
    await page.locator('#start_delay_ms').fill('23.5');
    await page.waitForTimeout(2200);
    assert.equal(await page.locator('#start_delay_ms').inputValue(), '23.5', 'Polling must not overwrite edits');
    await page.locator('#automation_form button[type=submit]').click();
    await page.waitForFunction(() => document.querySelector('#feedback_text').textContent === 'Готово');
    assert.equal(writes.find(x => x.path === '/api/config').body.ups.start_delay_ms, 23500);
    const beforeInvalid = writes.length;
    await page.locator('#verify_timeout_ms').fill('1');
    await page.locator('#automation_form button[type=submit]').click();
    assert.match(await page.locator('#feedback_text').textContent(), /має бути довшим/);
    assert.equal(writes.length, beforeInvalid);
    await page.locator('#verify_timeout_ms').fill('15');
    await page.locator('details').evaluate(element => { element.open = true; });
    await page.locator('#online_active_high').selectOption('false');
    await page.locator('#automation_form button[type=submit]').click();
    assert.match(await page.locator('#feedback_text').textContent(), /Для зміни полярності/);
    assert.equal(writes.length, beforeInvalid);
    await page.locator('#online_active_high').selectOption('true');
    nextError = { path: '/api/config', status: 500, message: 'Не вдалося записати налаштування' };
    await page.locator('#start_delay_ms').fill('24');
    await page.locator('#automation_form button[type=submit]').click();
    await page.waitForFunction(() => document.querySelector('#feedback_text').textContent === 'Не вдалося записати налаштування');
    assert.equal(config.ups.start_delay_ms, 23500, 'Failed save must not be presented as committed');

    await page.locator('#tab_network').click();
    await page.locator('#network_mode').selectOption('both');
    await page.locator('#ssid').fill('new router');
    await page.locator('#network_form button[type=submit]').click();
    await page.waitForFunction(() => !document.querySelector('#network_form button[type=submit]').disabled);
    const networkWrite = writes.filter(x => x.path === '/api/config').at(-1).body.network;
    assert.equal(networkWrite.ssid, 'new router');
    assert.equal(Object.hasOwn(networkWrite, 'password'), false, 'Blank password preserves the stored secret');
    await page.locator('#scan_button').click();
    await page.waitForFunction(() => document.querySelector('#scan_results').options.length === 3);
    assert.equal(await page.locator('#scan_results script').count(), 0);
    await page.locator('#scan_results').selectOption('Open');
    assert.equal(await page.locator('#open_network').isChecked(), true);
    await page.locator('#network_form button[type=submit]').click();
    await page.waitForFunction(() => !document.querySelector('#network_form button[type=submit]').disabled);
    assert.equal(writes.filter(x => x.path === '/api/config').at(-1).body.network.password, '');

    await page.locator('#tab_overview').click();
    Object.assign(status.ups, { busy: true, relay: true, phase: 'press_start' });
    await page.evaluate(data => renderStatus(data), status);
    for (const action of ['start', 'stop', 'restart']) assert.equal(await page.locator('[data-action=' + action + ']').isDisabled(), true);
    assert.equal(await page.locator('[data-action=cancel]').isDisabled(), false);
    await page.locator('[data-action=cancel]').click();
    await page.waitForFunction(() => document.querySelector('#relay_badge').textContent === 'Кнопку відпущено');

    // Rejected firmware must leave the UI usable and must not announce a reboot.
    await page.locator('#tab_system').click();
    nextError = { path: '/api/update', status: 400, message: 'Перевірка прошивки не пройдена' };
    await page.locator('#firmware_file').setInputFiles({ name: 'esp8266.bin', mimeType: 'application/octet-stream', buffer: Buffer.from([0xe9, 0, 0, 0]) });
    await page.locator('#update_form button[type=submit]').click();
    await page.waitForFunction(() => document.querySelector('#feedback_text').textContent === 'Перевірка прошивки не пройдена');
    assert.equal(uploadReceived, true);
    assert.equal(await page.locator('#firmware_file').isDisabled(), false);
    assert.equal(status.version, '1.0.0');
    await page.locator('#update_form button[type=submit]').click();
    await page.waitForFunction(() => document.querySelector('#firmware_version').textContent === '1.0.1');
    await page.waitForFunction(() => document.querySelector('#feedback_text').textContent.includes('знову на зв’язку'));

    await page.locator('#tab_automation').click();
    await page.locator('#start_delay_ms').fill('42.5');
    offline = true;
    await page.waitForFunction(() => document.querySelector('#connection_text').textContent === 'Немає зв’язку');
    assert.equal(await page.locator('#automation_form button[type=submit]').isDisabled(), true);
    offline = false;
    await page.waitForFunction(() => document.querySelector('#connection_text').textContent === 'На зв’язку');
    assert.equal(await page.locator('#start_delay_ms').inputValue(), '42.5');
    assert.deepEqual(errors, []);
    console.log('Web smoke checks passed: responsive layout, commands/guards, token refresh, settings, validation/error handling, Wi-Fi secrets/scan, escaping, OTA failure/reconnect, offline recovery.');
  } finally {
    await browser.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
