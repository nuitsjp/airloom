// Run from the repository root: node zigbee/tests/test_dashboard.js
// Executes the production script with a minimal DOM; browser layout is not tested.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

class Element {
  constructor() {
    this.textContent = '';
    this.disabled = false;
    this.value = '';
    this.files = [];
    this.children = [];
    this.handlers = {};
    this.classes = new Set();
    this.classList = {
      toggle: (name, enabled) => enabled ? this.classes.add(name) : this.classes.delete(name),
      add: name => this.classes.add(name),
    };
  }
  append(child) { this.children.push(child); }
  replaceChildren(...items) { this.children = items.flatMap(item => item.children); }
  addEventListener(name, fn) { this.handlers[name] = fn; }
  getBoundingClientRect() { return {width: 300, height: 190}; }
  getContext() {
    return {scale() {}, fillText() {}, beginPath() {}, moveTo() {}, lineTo() {}, stroke() {}, arc() {}, fill() {}};
  }
}

const elements = new Map();
function element(id) {
  if (!elements.has(id)) elements.set(id, new Element());
  return elements.get(id);
}
const status = {
  boot_id: 'boot-new', uptime_seconds: 30, storage_ok: true,
  history_count: 70, history_capacity: 4096, time_synchronized: true,
  latest: {
    sensor_id: '<img src=x onerror=alert(1)>',
    temperature_c: 23.45, humidity_percent: 65.78,
    temperature_age_seconds: 4, humidity_age_seconds: 8, from_previous_boot: false,
  },
};
const records = Array.from({length: 70}, (_, i) => ({
  sensor_id: 'one', boot_id: 'boot-new', received_uptime_seconds: i,
  received_at: i < 20 ? null : 1700000000 + i,
  temperature_c: 20 + i / 100, humidity_percent: 50 + i / 100,
  kind: i % 2 ? 'temperature' : 'humidity',
}));
const requests = [];
let fail = false, hold = false, pendingResolve;
const firmware = {version: 'test-v1', elf_sha256: 'a'.repeat(64), running_partition: 'ota_0', update_capacity_bytes: 1507328, updating: false, password_configured: true};
let firmwareFail = false, firmwareReplies = [], confirmUpdate = true, fakeNow = 1700000000000;
let uploadMode = 200, uploadReply = {ok: true, rebooting: true, elf_sha256: 'a'.repeat(64)};
const uploads = [];
class Clock extends Date { static now() { return fakeNow; } }
class MockUpload {
  constructor() { this.upload = {}; this.headers = {}; uploads.push(this); }
  open(method, url) { this.method = method; this.url = url; }
  setRequestHeader(key, value) { this.headers[key] = value; }
  send(file) {
    this.file = file;
    if (uploadMode === 'manual') return;
    queueMicrotask(() => {
      if (uploadMode === 'network') this.onerror();
      else { this.status = uploadMode; this.responseText = JSON.stringify(uploadReply); this.onload(); }
    });
  }
}
const context = vm.createContext({
  console, AbortController, Date: Clock, Math, Number, String, Array, Error, Promise,
  XMLHttpRequest: MockUpload,
  document: {
    getElementById: element,
    createElement: () => new Element(),
    createDocumentFragment: () => new Element(),
  },
  window: {devicePixelRatio: 1, addEventListener() {}, confirm: () => confirmUpdate},
  setTimeout: (fn, delay) => {
    if (delay <= 3000 && !String(fn).includes('controller.abort')) {
      fakeNow += delay;
      queueMicrotask(fn);
    }
    return 1;
  },
  clearTimeout() {},
  fetch: async url => {
    requests.push(url);
    if (hold) await new Promise(resolve => { pendingResolve = resolve; });
    if (fail) throw Error('offline');
    if (url === '/api/firmware') {
      if (firmwareFail) throw Error('firmware unavailable');
      const next = firmwareReplies.length ? firmwareReplies.shift() : firmware;
      if (next instanceof Error) throw next;
      return {ok: true, json: async () => structuredClone(next)};
    }
    if (url === '/api/status') return {ok: true, json: async () => structuredClone(status)};
    const query = new URL(url, 'http://example').searchParams;
    const end = records.length - Number(query.get('offset'));
    return {ok: true, json: async () => records.slice(Math.max(0, end - Number(query.get('limit'))), end)};
  },
});

const html = fs.readFileSync(path.join(__dirname, '../main/dashboard.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/);
assert.ok(script, 'dashboard must contain its inline application script');
vm.runInContext(script[1], context, {filename: 'dashboard.html'});

async function settle() {
  for (let i = 0; i < 8; i++) await new Promise(setImmediate);
}

(async () => {
  await settle();
  assert.equal(element('temperature').textContent, '23.45');
  assert.equal(element('humidity').textContent, '65.78');
  assert.equal(element('sensor').textContent, 'センサー <img src=x onerror=alert(1)>');
  assert.equal(element('history').children.length, 32);
  assert.equal(element('older').disabled, false);
  assert.equal(element('newer').disabled, true);
  assert.equal(element('firmware-version').textContent, '現在のバージョン: test-v1');
  assert.equal(element('firmware-update').disabled, false);

  element('older').handlers.click();
  await settle();
  assert.match(requests.at(-1), /offset=32$/);
  assert.equal(element('history').children.length, 32);
  element('older').handlers.click();
  await settle();
  assert.match(requests.at(-1), /offset=64$/);
  assert.equal(element('history').children.length, 6);
  assert.equal(element('older').disabled, true);
  element('newer').handlers.click();
  await settle();
  assert.match(requests.at(-1), /offset=32$/);

  fail = true;
  await vm.runInContext('refresh()', context);
  assert.equal(element('temperature').textContent, '23.45');
  assert.equal(element('connection').textContent, '通信断');
  assert.equal(element('temperature-age').textContent, '通信断のため現在の鮮度は未確認');

  fail = false;
  status.latest.from_previous_boot = true;
  status.latest.temperature_age_seconds = null;
  await vm.runInContext('refresh()', context);
  assert.equal(element('temperature-age').textContent, '以前の起動で受信 · 現在の鮮度は不明');
  assert.equal(element('humidity-age').textContent, '最終受信から 8秒',
    'a current humidity report must retain its known age when the temperature is from an earlier boot');
  status.latest.temperature_age_seconds = 3605;
  await vm.runInContext('refresh()', context);
  assert.equal(element('temperature-age').textContent, '最終受信から 1時間0分',
    'a known age takes precedence even when a value was received during an earlier boot');
  status.latest = null;
  await vm.runInContext('refresh()', context);
  assert.equal(element('temperature').textContent, '—');
  assert.equal(element('temperature-age').textContent, '受信待ち');

  hold = true;
  const first = vm.runInContext('refresh()', context);
  const count = requests.length;
  await vm.runInContext('refresh()', context);
  assert.equal(requests.length, count, 'an in-flight refresh must prevent another request');
  hold = false;
  pendingResolve();
  await first;

  firmwareFail = true;
  await vm.runInContext('loadFirmwareInfo()', context);
  assert.equal(element('firmware-update').disabled, true);
  assert.equal(element('connection').textContent, '接続済み', 'firmware endpoint failures must not change sensor connection status');
  firmwareFail = false;
  await vm.runInContext('loadFirmwareInfo()', context);
  assert.equal(element('firmware-update').disabled, false);
  firmwareReplies = [{...firmware, password_configured: false}];
  await vm.runInContext('loadFirmwareInfo()', context);
  assert.equal(element('firmware-update').disabled, true);
  assert.match(element('firmware-message').textContent, /管理パスワードが未設定/);
  const missingPasswordState = {...firmware};
  delete missingPasswordState.password_configured;
  firmwareReplies = [missingPasswordState];
  await vm.runInContext('loadFirmwareInfo()', context);
  assert.equal(element('firmware-update').disabled, true, 'missing password state must never enable firmware upload');
  await vm.runInContext('loadFirmwareInfo()', context);

  const runUpdate = () => vm.runInContext('startFirmwareUpdate()', context);
  const selectFile = (name = 'airloom_zigbee.bin', size = 123456) => {
    element('firmware-file').files = [{name, size}];
    element('firmware-password').value = 'test-password-only';
  };
  selectFile('wrong.txt');
  await runUpdate();
  assert.equal(uploads.length, 0);
  selectFile('empty.bin', 0);
  await runUpdate();
  assert.equal(uploads.length, 0);
  selectFile('large.bin', firmware.update_capacity_bytes + 1);
  await runUpdate();
  assert.match(element('firmware-message').textContent, /容量/);
  assert.equal(uploads.length, 0);
  selectFile();
  element('firmware-password').value = '';
  await runUpdate();
  assert.match(element('firmware-message').textContent, /パスワード/);
  assert.equal(uploads.length, 0);
  selectFile(); confirmUpdate = false;
  await runUpdate();
  assert.equal(uploads.length, 0, 'cancelling confirmation must not send the firmware');
  confirmUpdate = true;

  selectFile(); uploadMode = 401;
  await runUpdate();
  assert.match(element('firmware-message').textContent, /パスワードが正しくありません/);
  assert.equal(element('firmware-password').value, '');
  assert.equal(uploads.length, 1, 'an authentication failure must not retry');
  assert.equal(uploads[0].headers['X-Airloom-Update-Password'], 'test-password-only');
  assert.equal(uploads[0].headers['Content-Type'], 'application/octet-stream');
  assert.equal(uploads[0].method, 'POST');
  assert.equal(uploads[0].url, '/api/firmware');

  selectFile(); uploadMode = 400;
  await runUpdate();
  assert.match(element('firmware-message').textContent, /更新ファイルが正しくありません/);
  selectFile(); uploadMode = 'network';
  await runUpdate();
  assert.match(element('firmware-message').textContent, /自動で再送は行いません/);
  assert.equal(uploads.length, 3);

  selectFile(); uploadMode = 'manual';
  const updating = runUpdate();
  await settle();
  const upload = uploads.at(-1);
  assert.equal(element('firmware-update').disabled, true);
  assert.equal(element('firmware-password').value, '', 'clear the password field after sending');
  const requestsDuringUpload = requests.length;
  await vm.runInContext('refresh()', context);
  assert.equal(requests.length, requestsDuringUpload, 'sensor polling must pause during an update');
  await runUpdate();
  assert.equal(uploads.length, 4, 'a second click must not start another upload');
  upload.upload.onprogress({lengthComputable: true, loaded: 50, total: 100});
  assert.equal(element('firmware-progress').value, 50);
  assert.match(element('firmware-message').textContent, /50%/);
  firmwareReplies = [
    {...firmware, running_partition: 'ota_1', elf_sha256: 'b'.repeat(64)},
    {...firmware, running_partition: 'ota_1'},
  ];
  upload.status = 200; upload.responseText = JSON.stringify(uploadReply); upload.onload();
  await updating;
  assert.match(element('firmware-message').textContent, /更新が完了/);
  assert.equal(firmwareReplies.length, 0, 'a changed slot with the wrong image hash must not count as success');
  assert.equal(element('firmware-update').disabled, false);
  assert.equal(element('firmware-file').value, '');
  assert.equal(element('firmware-password').value, '');

  await vm.runInContext('loadFirmwareInfo()', context);
  selectFile(); uploadMode = 200;
  const beforeTimeout = fakeNow;
  await runUpdate();
  assert.match(element('firmware-message').textContent, /起動を確認できません/);
  assert.ok(fakeNow - beforeTimeout >= 90000, 'wait for the bounded reboot confirmation window');
  assert.equal(uploads.length, 5, 'a failed reboot confirmation must not resend the image');
  console.log('dashboard tests passed: history, freshness, OTA validation, authentication failures, progress, paused polling, image verification, bounded restart confirmation');
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
