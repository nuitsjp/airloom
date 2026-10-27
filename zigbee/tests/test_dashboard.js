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
const context = vm.createContext({
  console, AbortController, Date, Math, Number, String, Array, Error, Promise,
  document: {
    getElementById: element,
    createElement: () => new Element(),
    createDocumentFragment: () => new Element(),
  },
  window: {devicePixelRatio: 1, addEventListener() {}},
  setTimeout: () => 1,
  clearTimeout() {},
  fetch: async url => {
    requests.push(url);
    if (hold) await new Promise(resolve => { pendingResolve = resolve; });
    if (fail) throw Error('offline');
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
  console.log('dashboard tests passed: pagination, offline retention, unknown freshness, absent readings, sequential polling, text-only sensor rendering');
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
