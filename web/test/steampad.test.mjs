// Valve's controllers through WebHID (web/src/input/steampad.ts): the 2015 Steam Controller's, the Steam Deck's and the
// 2026 Steam Controller's reports as SDL lays them out, the commands that turn their lizard mode off and on, and a
// controller through the poller: it drives as a standard-mapping pad of the Xbox family, lizard mode follows the page's
// focus, a dongle's controller comes and goes, the Deck's motors rumble.
//   node --test web/test/steampad.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
/** steampad.ts and gamepad.ts in one bundle; tag: a fresh module instance (a new import URL) */
const load = async (tag = '') => import('data:text/javascript;base64,' + Buffer.from((await esbuild.build({
  stdin: { contents: "export * from './steampad'; export { PadPoller, drivingPad, familyOf, mayDrive } from './gamepad';",
    resolveDir: path.join(ROOT, 'web/src/input'), loader: 'ts' }, bundle: true, format: 'esm', write: false }))
  .outputFiles[0].text + `\n//${tag}`).toString('base64'));
const S = await load();
const P = await import('data:text/javascript;base64,' + Buffer.from((await esbuild.build({
  entryPoints: [path.join(ROOT, 'web/src/input/proto.ts')], bundle: true, format: 'esm', write: false })).outputFiles[0].text).toString('base64'));
const B = S.SB;
const bit = (...b) => b.reduce((m, x) => m | 1 << x, 0);

/** a 2015 controller's state report (64 bytes, no id) */
function d0g({ packet = 1, m = 0, lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0 } = {}) {
  const d = new DataView(new ArrayBuffer(64));
  d.setUint16(0, 1, true); d.setUint8(2, 1); d.setUint8(3, 60); d.setUint32(4, packet, true);
  d.setUint8(8, m & 0xff); d.setUint8(9, m >> 8 & 0xff); d.setUint8(10, m >> 16 & 0xff);
  d.setUint8(11, lt); d.setUint8(12, rt);
  d.setInt16(16, lx, true); d.setInt16(18, ly, true); d.setInt16(20, rx, true); d.setInt16(22, ry, true);
  return d;
}
/** a 2015 dongle's link report: 1 its controller went, 2 came */
function link(event) {
  const d = new DataView(new ArrayBuffer(64));
  d.setUint16(0, 1, true); d.setUint8(2, 3); d.setUint8(3, 1); d.setUint8(4, event);
  return d;
}
/** a Steam Deck's report (64 bytes, no id, type 9) */
function deck({ low = 0, high = 0, lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0 } = {}) {
  const d = new DataView(new ArrayBuffer(64));
  d.setUint16(0, 1, true); d.setUint8(2, 9); d.setUint8(3, 64); d.setUint32(4, 7, true);
  d.setUint32(8, low, true); d.setUint32(12, high, true);
  d.setUint16(44, lt, true); d.setUint16(46, rt, true);
  d.setInt16(48, lx, true); d.setInt16(50, ly, true); d.setInt16(52, rx, true); d.setInt16(54, ry, true);
  return d;
}
/** a 2026 controller's state (report 0x42's data, its id left out) */
function triton({ m = 0, lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0 } = {}) {
  const d = new DataView(new ArrayBuffer(45));
  d.setUint8(0, 3); d.setUint32(1, m, true);
  d.setInt16(5, lt, true); d.setInt16(7, rt, true);
  d.setInt16(9, lx, true); d.setInt16(11, ly, true); d.setInt16(13, rx, true); d.setInt16(15, ry, true);
  return d;
}
const near = (a, b, eps = 0.01) => Math.abs(a - b) <= eps;

test('the 2015 controller: buttons where the standard mapping has them, the stick +y down, the triggers\' 7 bits', () => {
  const r = new S.D0GReader();
  const s = r.read(d0g({ m: 0x80 | 0x1000 | 0x4000 | 0x2000, lx: 32767, ly: 32767, lt: 255, rt: 101 }));
  assert.equal(s.buttons, bit(B.A, B.BACK, B.START, B.HOME, B.LT, B.RT));
  assert.deepEqual([s.lx, s.ly], [1, -1]);
  assert.equal(s.lt, 1);
  assert.ok(near(s.rt, 13029 / 26000, 1e-6));
  assert.equal(r.read(d0g({ m: 0x20 })), s, 'the same packet number: the last state');
  assert.equal(r.read(d0g({ packet: 2, m: 0x20 | 0x40 | 0x10 | 0x8 | 0x4 })).buttons, bit(B.B, B.X, B.Y, B.LB, B.RB));
  assert.equal(r.read(d0g({ packet: 3, m: 0x8000 | 0x10000 })).buttons, bit(B.L4, B.R4), 'the grips');
  assert.equal(r.read(d0g({ packet: 4, m: 0x2 })).lt, 1, 'the trigger clicked all the way');
});

test('the 2015 controller: the left pad\'s clicks are the D-pad, its stick and pad take turns in a report\'s fields', () => {
  const r = new S.D0GReader();
  // a thumb on the pad, clicked up: the D-pad, and the stick reads nothing then
  let s = r.read(d0g({ packet: 1, m: 0x80000 | 0x20000 | 0x100, lx: 0, ly: 30000 }));
  assert.equal(s.buttons, bit(B.UP));
  assert.deepEqual([s.lx, s.ly], [0, -0]);
  // the stick pushed right, then the pad touched too: the reports take turns, the stick stays where it was
  s = r.read(d0g({ packet: 2, lx: 16000, ly: 0 }));
  assert.ok(near(s.lx, 16000 / 32767));
  s = r.read(d0g({ packet: 3, m: 0x80000 | 0x800000, lx: -5000, ly: 5000 }));
  assert.ok(near(s.lx, 16000 / 32767), 'a pad report while both are in use keeps the stick');
  // an older controller: the pad's click with no thumb on the pad is the stick's
  assert.equal(r.read(d0g({ packet: 4, m: 0x20000 })).buttons, bit(B.LS));
  assert.equal(r.read(d0g({ packet: 5, m: 0x400000 })).buttons, bit(B.LS));
  // a click on the pad that came without its direction: from where it was clicked
  assert.equal(r.read(d0g({ packet: 6, m: 0x80000 | 0x20000, lx: 0, ly: 30000 })).buttons, bit(B.UP));
  assert.equal(r.read(d0g({ packet: 7, m: 0x80000 | 0x20000, lx: -30000, ly: 0 })).buttons & bit(B.LEFT), bit(B.LEFT));
  assert.equal(r.read(d0g({ packet: 8, m: 0x80000 | 0x20000, lx: 100, ly: 200 })).buttons, 0, 'the middle: none');
});

test('the 2015 controller: the right pad is the right stick (turned 15 degrees, as SDL has it), its click the stick\'s', () => {
  const r = new S.D0GReader();
  const s = r.read(d0g({ m: 0x100000 | 0x40000, rx: 0, ry: 20000 }));
  assert.equal(s.buttons, bit(B.RS));
  assert.ok(s.ry < -0.6 && Math.abs(s.rx) < 0.2, `up on the pad is up: ${s.rx} ${s.ry}`);
  assert.deepEqual([r.read(d0g({ packet: 2 })).rx, r.read(d0g({ packet: 3 })).ry], [0, -0], 'let go: centred');
});

test('the 2015 dongle\'s link reports, and what is not a state', () => {
  const r = new S.D0GReader();
  assert.equal(r.read(link(1)), 'off');
  assert.equal(r.read(link(2)), 'on');
  assert.equal(r.read(link(3)), 'on', 'newly paired');
  const status = d0g(); status.setUint8(2, 4);
  assert.equal(r.read(status), null, 'a battery report');
  const other = d0g(); other.setUint16(0, 2, true);
  assert.equal(r.read(other), null, 'another version');
});

test('the Steam Deck: both button words, View is Back and the menu button Start, triggers and sticks', () => {
  const s = S.readDeck(deck({ low: 0x80 | 0x1000 | 0x4000 | 0x2000 | 0x4000000 | 0x8000, high: 0x200 | 0x40000,
    lt: 32767, rt: 16384, lx: -32768, ly: -32767, rx: 32767 }));
  assert.equal(s.buttons, bit(B.A, B.BACK, B.START, B.HOME, B.RS, B.L5, B.L4, B.QAM, B.LT, B.RT));
  assert.deepEqual([s.lx, s.ly, s.rx, s.ry, s.lt], [-1, 1, 1, -0, 1]);
  assert.ok(near(s.rt, 0.5));
  assert.equal(S.readDeck(deck({ low: 0x20 | 0x40 | 0x10 | 0x8 | 0x4 | 0x400000 | 0x100 | 0x800 | 0x400 | 0x200 })).buttons,
    bit(B.B, B.X, B.Y, B.LB, B.RB, B.LS, B.UP, B.DOWN, B.LEFT, B.RIGHT));
  const pressed = S.readDeck(deck({ low: 0x2 }));
  assert.equal(pressed.buttons, bit(B.LT), 'L2 clicked');
  const lizard = deck(); lizard.setUint8(2, 1);
  assert.equal(S.readDeck(lizard), null, 'not a Deck state');
});

test('the 2026 controller: SDL\'s buttons (its MENU bit is Back, VIEW Start), sticks, a dongle\'s link', () => {
  const s = S.readTriton(0x42, triton({ m: 0x1 | 0x4000 | 0x40 | 0x10000 | 0x80000 | 0x200 | 0x2000, lt: 32767, ly: 32767, rx: -32767 }));
  assert.equal(s.buttons, bit(B.A, B.BACK, B.START, B.HOME, B.LB, B.RB, B.UP, B.LT));
  assert.deepEqual([s.lx, s.ly, s.rx, s.lt, s.rt], [0, -1, -1, 1, 0]);
  assert.equal(S.readTriton(0x47, triton({ m: 0x8000 | 0x20 | 0x20000 | 0x80 | 0x40000 | 0x100 | 0x10 })).buttons,
    bit(B.LS, B.RS, B.L4, B.R4, B.L5, B.R5, B.QAM));
  assert.equal(S.readTriton(0x45, triton({ m: 0x800000 })).buttons, bit(B.RT), 'over Bluetooth, R2 clicked');
  const one = (v) => new DataView(Uint8Array.of(v).buffer);
  assert.equal(S.readTriton(0x79, one(1)), 'off');
  assert.equal(S.readTriton(0x46, one(2)), 'on');
  assert.equal(S.readTriton(0x43, triton()), null, 'its battery');
  assert.equal(S.readTriton(0x42, new DataView(new ArrayBuffer(8))), null, 'too short');
});

test('the commands that turn lizard mode off and on, byte for byte as SDL sends them', () => {
  const L = S.LIZARD;
  assert.deepEqual(L.d0gOff, [[0x81, 0], [0x8e, 0], [0x87, 12, 49, 2, 0, 7, 7, 0, 8, 7, 0, 24, 0, 0]]);
  assert.deepEqual(L.d0gOn, [[0x85, 0], [0x8e, 0], [0x87, 3, 8, 0, 0]]);
  assert.deepEqual(L.deckOff, [[0x81, 0], [0x87, 15, 24, 0, 0, 7, 7, 0, 8, 7, 0, 52, 255, 255, 53, 255, 255]]);
  assert.deepEqual(L.deckKeep, [[0x81, 0], [0x87, 3, 8, 7, 0]]);
  assert.deepEqual(L.tritonOff, [[0x87, 3, 9, 0, 0]]);
  assert.deepEqual(L.tritonOn, [[0x87, 3, 9, 1, 0]]);
  assert.equal(S.steamPadId(0x1142), 'Steam Controller (STANDARD GAMEPAD Vendor: 28de Product: 1142)');
  assert.equal(S.steamPadId(0x1205), 'Steam Deck (STANDARD GAMEPAD Vendor: 28de Product: 1205)');
});

/** a WebHID device of Valve's that records what the page sends it */
class FakeHid extends EventTarget {
  constructor(productId, collections = [{ usagePage: 0xff00, usage: 1 }]) {
    super();
    Object.assign(this, { vendorId: 0x28de, productId, productName: 'Valve', collections, opened: false, sent: [], out: [], reads: 0 });
  }
  async open() { this.opened = true; }
  async close() { this.opened = false; }
  async sendFeatureReport(id, data) { this.sent.push([id, data.length, ...Array.from(data.slice(0, Math.max(2, data[1] + 2)))]); }
  async sendReport(id, data) { this.out.push([id, ...data]); }
  async receiveFeatureReport() { this.reads++; return new DataView(new ArrayBuffer(64)); }
  report(id, data) { const e = new Event('inputreport'); e.reportId = id; e.data = data; this.dispatchEvent(e); }
}
const settle = (ms = 0) => new Promise(r => setTimeout(r, ms));

/** fn(env) on a page with navigator.hid giving devices, a document that is visible and focused (env.focus(on)), and the
 *  window's events on a target of its own; M: a fresh steampad + gamepad instance */
async function onPage(devices, fn) {
  const nav = Object.getOwnPropertyDescriptor(globalThis, 'navigator'), doc = Object.getOwnPropertyDescriptor(globalThis, 'document');
  const et = new EventTarget(), hidEt = new EventTarget(), docEt = new EventTarget();
  const hid = Object.assign(hidEt, { getDevices: async () => devices, requestDevice: async () => devices });
  let focused = true;
  Object.defineProperty(globalThis, 'navigator', { value: { getGamepads: () => [], hid }, configurable: true });
  Object.defineProperty(globalThis, 'document', { value: Object.assign(docEt, { visibilityState: 'visible', hidden: false,
    hasFocus: () => focused }), configurable: true });
  const ev = { addEventListener: globalThis.addEventListener, removeEventListener: globalThis.removeEventListener, dispatchEvent: globalThis.dispatchEvent };
  Object.assign(globalThis, { addEventListener: et.addEventListener.bind(et), removeEventListener: et.removeEventListener.bind(et),
    dispatchEvent: et.dispatchEvent.bind(et) });
  try {
    const M = await load(String(Math.random()));
    const events = [];
    et.addEventListener('gamepadconnected', e => events.push(['on', e.gamepad.id]));
    et.addEventListener('gamepaddisconnected', e => events.push(['off', e.gamepad.id]));
    await fn({ M, events, hid, focus(on) { focused = on; et.dispatchEvent(new Event(on ? 'focus' : 'blur')); } });
  } finally {
    if (nav) Object.defineProperty(globalThis, 'navigator', nav); else delete globalThis.navigator;
    if (doc) Object.defineProperty(globalThis, 'document', doc); else delete globalThis.document;
    Object.assign(globalThis, ev);
  }
}
function poller(M) {
  const buf = new SharedArrayBuffer(P.IM_BYTES), logs = [];
  const p = new M.PadPoller(new Int32Array(buf), new Uint8Array(buf), 0, () => {}, s => logs.push(s));
  return { p, h: new Int32Array(buf), u8: new Uint8Array(buf), f: new Float32Array(buf), logs };
}
const nameAt = (u8) => String.fromCharCode(...u8.slice(P.IM_OFF_PAD_NAME, P.IM_OFF_PAD_NAME + 16)).replace(/\0.*/, '');

test('a wired 2015 controller through the poller: it drives as an Xbox-family pad, lizard mode off while the page has the focus', async () => {
  const dev = new FakeHid(0x1102);
  await onPage([dev], async ({ M, events, focus }) => {
    const { p, h, u8, f, logs } = poller(M);
    await settle();
    assert.ok(dev.opened, 'the picked controller opened with the game');
    assert.equal(M.allPads().length, 0, 'not a pad until it reports');
    dev.report(0, d0g({ m: 0x80, lt: 255 }));
    await settle();
    assert.deepEqual(events, [['on', 'Steam Controller (STANDARD GAMEPAD Vendor: 28de Product: 1102)']]);
    p.poll();
    const g = M.drivingPad();
    assert.equal(g?.index, 16);
    assert.equal(g.mapping, 'standard');
    assert.ok(M.mayDrive(g));
    assert.deepEqual([h[P.IM_PAD_BUTTONS], h[P.IM_PAD_FAMILY], f[P.IM_PAD_AXIS + P.PAD_AXIS_LT]], [bit(P.PAD_A), P.PADF_XBOX, 1]);
    assert.equal(nameAt(u8), 'Steam Controller');
    assert.equal(h[P.IM_PAD_RUMBLE], 0, 'the 2015 controller has no motors');
    assert.deepEqual(dev.sent, [[0, 64, 0x81, 0], [0, 64, 0x8e, 0], [0, 64, 0x87, 12, 49, 2, 0, 7, 7, 0, 8, 7, 0, 24, 0, 0]],
      'lizard mode off: the 64-byte feature reports of report 0');
    assert.ok(logs.some(s => /Steam Controller \(28de:1102\) is controller 16 \(WebHID\)/.test(s)));
    assert.ok(logs.some(s => /first report, id 0, 64 bytes: 01 00 01 3c/.test(s)));
    dev.sent.length = 0;
    focus(false);
    await settle();
    assert.deepEqual(dev.sent.map(r => r[2]), [0x85, 0x8e, 0x87], 'the page lost the focus: lizard mode back');
    dev.sent.length = 0;
    focus(true);
    await settle();
    assert.deepEqual(dev.sent.map(r => r[2]), [0x81, 0x8e, 0x87], 'and off again');
    dev.sent.length = 0;
    p.dispose();
    await settle();
    assert.deepEqual(dev.sent.map(r => r[2]), [0x85, 0x8e, 0x87], 'the game ended: lizard mode back');
    assert.ok(!dev.opened, 'then closed');
    assert.equal(M.allPads().length, 0);
  });
});

test('a 2015 dongle: asked for its controller, which comes, drives, goes and comes back (lizard mode off again)', async () => {
  const slot = new FakeHid(0x1142), kbd = new FakeHid(0x1142, []);
  await onPage([slot, kbd], async ({ M, events }) => {
    const { p, h } = poller(M);
    await settle();
    assert.deepEqual(slot.sent, [[0, 64, 0xb4, 0]], 'the dongle asked whether its controller is there');
    assert.deepEqual(kbd.sent, [], 'not its other interfaces');
    slot.report(0, link(2));
    await settle();
    assert.deepEqual(events, [], 'linked, no state yet');
    slot.report(0, d0g({ m: 0x20 }));
    await settle();
    p.poll();
    assert.equal(h[P.IM_PAD_BUTTONS], bit(P.PAD_B));
    assert.equal(slot.sent.at(-1)[2], 0x87, 'lizard mode off once it is there');
    slot.sent.length = 0;
    slot.report(0, link(1));
    await settle();
    p.poll();
    assert.equal(h[P.IM_PAD_CONNECTED], 0);
    assert.equal(M.allPads().length, 0);
    assert.deepEqual(slot.sent, [], 'nothing sent to a controller that went');
    slot.report(0, d0g({ packet: 9 }));
    await settle();
    assert.deepEqual(slot.sent.map(r => r[2]), [0x81, 0x8e, 0x87], 'back: its settings are the default again, lizard mode off');
    assert.deepEqual(events.map(e => e[0]), ['on', 'off', 'on']);
    p.dispose();
  });
});

test('a Steam Deck: lizard mode kept off (its watchdog), the game\'s rumble on its motors and stopped after', async () => {
  const dev = new FakeHid(0x1205);
  await onPage([dev], async ({ M }) => {
    const { p, h } = poller(M);
    await settle();
    dev.report(0, deck({ low: 0x80 }));
    await settle();
    p.poll();
    assert.equal(h[P.IM_PAD_BUTTONS], bit(P.PAD_A));
    assert.equal(h[P.IM_PAD_RUMBLE], 1, 'it can rumble');
    assert.deepEqual(dev.sent, [[0, 64, 0x81, 0], [0, 64, 0x87, 15, 24, 0, 0, 7, 7, 0, 8, 7, 0, 52, 255, 255, 53, 255, 255]]);
    assert.equal(dev.reads, 1, 'the report a change of settings leaves, read and dropped');
    dev.sent.length = 0;
    await M.drivingPad().vibrationActuator.playEffect('dual-rumble', { duration: 60, strongMagnitude: 1, weakMagnitude: 0.5 });
    await settle();
    assert.deepEqual(dev.sent, [[0, 64, 0xeb, 0]]);
    dev.sent.length = 0;
    await settle(130);
    assert.deepEqual(dev.sent, [[0, 64, 0xeb, 0]], 'stopped once the effect ran out');
    p.dispose();
  });
});

test('the Deck\'s rumble report: SDL\'s simple rumble, the motors\' speeds 16 bits', async () => {
  const dev = new FakeHid(0x1205);
  dev.sendFeatureReport = async (id, data) => { dev.sent.push([id, ...data.slice(0, 11)]); };
  await onPage([dev], async ({ M }) => {
    const { p } = poller(M);
    await settle();
    dev.report(0, deck());
    await settle();
    p.poll();
    dev.sent.length = 0;
    await M.drivingPad().vibrationActuator.playEffect('dual-rumble', { duration: 500, strongMagnitude: 1, weakMagnitude: 0.5 });
    await settle();
    assert.deepEqual(dev.sent, [[0, 0xeb, 0, 0, 0, 0, 0xff, 0xff, 0x00, 0x80, 2, 0]]);
    await M.drivingPad().vibrationActuator.reset();
    await settle();
    assert.deepEqual(dev.sent.at(-1), [0, 0xeb, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0]);
    p.dispose();
  });
});

test('a 2026 controller: its reports with ids, lizard mode off through feature report 1, rumble on output report 0x80', async () => {
  const dev = new FakeHid(0x1302);
  await onPage([dev], async ({ M }) => {
    const { p, h, f } = poller(M);
    await settle();
    dev.report(0x43, triton());                    // its battery: no controller yet
    await settle();
    assert.equal(M.allPads().length, 0);
    dev.report(0x42, triton({ m: 0x4 | 0x40, rx: 32767 }));
    await settle();
    p.poll();
    assert.equal(h[P.IM_PAD_BUTTONS], bit(P.PAD_X, P.PAD_START));
    assert.equal(f[P.IM_PAD_AXIS + P.PAD_AXIS_RX], 1);
    assert.deepEqual(dev.sent, [[1, 63, 0x87, 3, 9, 0, 0]], 'feature report 1 is 63 bytes');
    await M.drivingPad().vibrationActuator.playEffect('dual-rumble', { duration: 120, strongMagnitude: 0.5, weakMagnitude: 1 });
    await settle(100);
    assert.ok(dev.out.length >= 2, 'renewed while it runs (the motors stop by themselves after ~50 ms)');
    assert.deepEqual(dev.out[0], [0x80, 0, 0, 0, 0x00, 0x80, 0, 0xff, 0xff, 0]);
    await settle(80);
    assert.deepEqual(dev.out.at(-1), [0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0], 'then stopped');
    dev.sent.length = 0;
    p.dispose();
    await settle();
    assert.deepEqual(dev.sent, [[1, 63, 0x87, 3, 9, 1, 0]], 'lizard mode back when the game ends');
  });
});

test('no WebHID (Firefox, Safari, phones): nothing to open, the browser\'s pads as they are', async () => {
  const nav = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
  const xbox = { id: 'Xbox 360 Controller (XInput STANDARD GAMEPAD)', index: 0, connected: true, mapping: 'standard', axes: [], buttons: [] };
  Object.defineProperty(globalThis, 'navigator', { value: { getGamepads: () => [xbox, null] }, configurable: true });
  try {
    const M = await load('nohid');
    assert.equal(M.steamPadsPossible(), false);
    assert.deepEqual(await M.connectSteamPad(), []);
    assert.deepEqual(M.allPads(), [xbox, null]);
    M.useSteamPads(() => {})();
  } finally {
    if (nav) Object.defineProperty(globalThis, 'navigator', nav); else delete globalThis.navigator;
  }
  assert.equal(S.familyOf('Steam Virtual Gamepad (STANDARD GAMEPAD Vendor: 28de Product: 11ff)').family, P.PADF_XBOX);
});
