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
      version: '1.1.0', csrf_token: 'initial-token', uptime_ms: 125000,
      free_heap: 29400, reset_reason: 'Power on', storage_ok: true,
      ups: {
        state: 'off', phase: 'finished', result: 'none', busy: false,
        relay: false, mains: true, online: false, battery: false,
        inputs_valid: true, auto_hold: false, cycle_handled: false,
        attempts: 0, remaining_ms: 0
      },
      battery: {
        enabled: false, valid: false, raw_adc: null, voltage_mv: null,
        low: false, saturated: false, state: 'disabled',
        full_scale_mv: 62600, low_warning_mv: 0
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
      battery: { enabled: false, full_scale_mv: 62600, low_warning_mv: 0 },
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
        if (body.battery) Object.assign(status.battery, body.battery, {
          valid: false, raw_adc: null, voltage_mv: null, low: false,
          saturated: false, state: body.battery.enabled ? 'warming_up' : 'disabled'
        });
        if (body.network) {
          status.network.mode = body.network.mode;
          if (Object.hasOwn(body.network, 'password')) {
            config.network.has_password = !!body.network.password;
            delete config.network.password;
          }
        }
      }
      if (pathname === '/api/battery/calibrate') {
        config.battery.full_scale_mv = Math.round(body.reference_mv * 1023 / status.battery.raw_adc);
        Object.assign(status.battery, {
          full_scale_mv: config.battery.full_scale_mv, valid: false,
          raw_adc: null, voltage_mv: null, low: false, state: 'warming_up'
        });
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
        status.version = '1.1.1'; status.csrf_token = 'after-update'; status.uptime_ms = 500;
      }
      return reply(data);
    });
    await page.goto('http://ups.test/');
    await page.waitForFunction(() => document.querySelector('#connection_text').textContent === 'На зв’язку' && !document.querySelector('[data-action=start]').disabled);
    assert.equal(await page.locator('#log_list img').count(), 0);
    assert.equal(await page.evaluate(() => window.injected), undefined);
    assert.equal(await page.locator('#battery_voltage').textContent(), '—');
    assert.equal(await page.locator('#battery_state_badge').textContent(), 'Вимкнено');
    assert.equal(await page.locator('#battery_calibrate_button').isDisabled(), true);
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
    await page.locator('#automation_form details').evaluate(element => { element.open = true; });
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

    // Battery settings are independent from digital BATTERY and preserve edits
    // in the automation form, including after calibration refreshes config.
    await page.locator('#tab_system').click();
    await page.locator('#battery_enabled').check();
    await page.locator('#battery_warning_enabled').check();
    await page.locator('#battery_low_warning').fill('11.6');
    await page.locator('#battery_advanced').evaluate(element => { element.open = true; });
    await page.locator('#battery_preset').selectOption('19000');
    assert.equal(await page.locator('#battery_full_scale').inputValue(), '19');
    await page.locator('#battery_preset').selectOption('62600');
    assert.equal(await page.locator('#battery_full_scale').inputValue(), '62.6');
    await page.locator('#battery_full_scale').fill('63.1');
    assert.equal(await page.locator('#battery_preset').inputValue(), 'custom');
    await page.waitForTimeout(2200);
    assert.equal(await page.locator('#battery_full_scale').inputValue(), '63.1', 'Polling preserves unsaved battery scale');
    assert.equal(await page.locator('#battery_calibrate_button').isDisabled(), true, 'Unsaved settings block calibration');
    await page.locator('#battery_form button[type=submit]').click();
    await page.waitForFunction(() => !document.querySelector('#battery_form button[type=submit]').disabled);
    assert.deepEqual(writes.filter(x => x.path === '/api/config').at(-1).body, {
      battery: { enabled: true, full_scale_mv: 63100, low_warning_mv: 11600 }
    });
    assert.equal(await page.locator('#battery_voltage').textContent(), '—', 'Warm-up must not display null as zero volts');
    assert.equal(await page.locator('#battery_adc').textContent(), '—');
    assert.equal(await page.locator('#start_delay_ms').inputValue(), '24', 'Battery refresh preserves unrelated edits');
    Object.assign(status.battery, { valid: true, raw_adc: 206, voltage_mv: 12707, state: 'ok' });
    await page.evaluate(data => renderStatus(data), status);
    assert.equal(await page.locator('#battery_voltage').textContent(), '12,7 В');
    assert.equal(await page.locator('#battery_adc').textContent(), '206 / 1023');
    assert.equal(await page.locator('#battery_calibrate_button').isDisabled(), false);
    assert.equal(await page.locator('#battery_value').textContent(), 'Немає сигналу', 'Analog measurement does not alter digital BATTERY state');
    assert.doesNotMatch(await page.locator('#battery_card').textContent(), /%/);
    const validBattery = { ...status.battery };
    for (const fault of [
      { valid: false, raw_adc: 0, voltage_mv: null, state: 'no_signal', saturated: false },
      { valid: false, raw_adc: 1023, voltage_mv: null, state: 'overrange', saturated: true },
      { valid: false, raw_adc: null, voltage_mv: null, state: 'stale', saturated: false }
    ]) {
      Object.assign(status.battery, fault);
      await page.evaluate(data => renderStatus(data), status);
      assert.equal(await page.locator('#battery_voltage').textContent(), '—', fault.state + ' hides unreliable voltage');
      assert.equal(await page.locator('#battery_calibrate_button').isDisabled(), true);
    }
    Object.assign(status.battery, validBattery, { raw_adc: 180, voltage_mv: 11100, low: true });
    await page.evaluate(data => renderStatus(data), status);
    assert.equal(await page.locator('#battery_voltage').textContent(), '11,1 В');
    assert.equal(await page.locator('#battery_low_notice').isVisible(), false, 'Overview warning is inside the hidden overview tab');
    assert.equal(await page.locator('#battery_low_notice').getAttribute('hidden'), null);
    assert.match(await page.locator('#battery_low_notice').textContent(), /не вимикає UPS/);
    Object.assign(status.battery, validBattery);
    await page.evaluate(data => renderStatus(data), status);
    assert.notEqual(await page.locator('#battery_low_notice').getAttribute('hidden'), null);
    status.ups.busy = true;
    await page.evaluate(data => renderStatus(data), status);
    assert.equal(await page.locator('#battery_calibrate_button').isDisabled(), true);
    status.ups.busy = false;
    await page.evaluate(data => renderStatus(data), status);
    await page.locator('#battery_reference').fill('12.65');
    nextError = { path: '/api/battery/calibrate', status: 422, message: 'Масштаб поза межами' };
    await page.locator('#battery_calibrate_button').click();
    await page.waitForFunction(() => document.querySelector('#feedback_text').textContent === 'Масштаб поза межами');
    assert.equal(config.battery.full_scale_mv, 63100, 'Failed calibration leaves saved scale intact');
    await page.locator('#battery_calibrate_button').click();
    await page.waitForFunction(() => !document.querySelector('#battery_form button[type=submit]').disabled);
    assert.deepEqual(writes.filter(x => x.path === '/api/battery/calibrate').at(-1).body, { reference_mv: 12650 });
    assert.equal(config.battery.full_scale_mv, Math.round(12650 * 1023 / 206));
    assert.equal(await page.locator('#battery_full_scale').inputValue(), String(config.battery.full_scale_mv / 1000));
    assert.equal(await page.locator('#start_delay_ms').inputValue(), '24');
    assert.equal(await page.locator('#battery_calibrate_button').isDisabled(), true, 'Calibration restarts the averaging window');
    const beforeInvalidBattery = writes.length;
    await page.locator('#battery_low_warning').fill('7.5');
    await page.locator('#battery_form button[type=submit]').click();
    assert.equal(writes.length, beforeInvalidBattery, 'Out-of-range warning is not posted');
    await page.locator('#battery_low_warning').fill('11.6');
    await page.locator('#battery_enabled').uncheck();
    await page.locator('#battery_form button[type=submit]').click();
    await page.waitForFunction(() => !document.querySelector('#battery_form button[type=submit]').disabled);
    assert.equal(config.battery.enabled, false);
    assert.equal(await page.locator('#battery_voltage').textContent(), '—');
    const legacyStatus = { ...status }; delete legacyStatus.battery;
    await page.evaluate(data => renderStatus(data), legacyStatus);
    assert.equal(await page.locator('#battery_state_badge').textContent(), 'Вимкнено', 'Older status responses degrade gracefully');
    await page.evaluate(data => renderStatus(data), status);

    if (screenshots) {
      config.battery.enabled = true;
      Object.assign(status.battery, validBattery, { enabled: true, voltage_mv: 12650 });
      await page.evaluate(data => fillConfig(data, 'battery'), config);
      await page.evaluate(data => renderStatus(data), status);
      await page.locator('#tab_overview').click();
      await page.screenshot({ path: path.join(screenshots, 'desktop-battery.png'), fullPage: true });
      await page.setViewportSize({ width: 390, height: 844 });
      await page.locator('#tab_system').click();
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true, 'Mobile battery settings fit');
      await page.screenshot({ path: path.join(screenshots, 'mobile-battery-settings.png'), fullPage: true });
      await page.setViewportSize({ width: 1280, height: 900 });
      config.battery.enabled = false;
      Object.assign(status.battery, { enabled: false, valid: false, raw_adc: null, voltage_mv: null, state: 'disabled' });
      await page.evaluate(data => fillConfig(data, 'battery'), config);
    }

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
    assert.equal(status.version, '1.1.0');
    await page.locator('#update_form button[type=submit]').click();
    await page.waitForFunction(() => document.querySelector('#firmware_version').textContent === '1.1.1');
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
    console.log('Web smoke checks passed: responsive layout, commands/guards, token refresh, settings, battery readings/faults/calibration, validation/error handling, Wi-Fi secrets/scan, escaping, OTA failure/reconnect, offline recovery.');
  } finally {
    await browser.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
