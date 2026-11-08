// 8BitDo controllers without the standard mapping (web/src/input/hidpad.ts, gamepad.ts): which are 8BitDo's and how
// their face buttons are labelled, their axes as each browser places them, the pad they make, and one through the
// poller (a player's report: the controls did not work with an 8BitDo controller).
//   node --test web/test/hidpad.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const load = async (f) => import('data:text/javascript;base64,' + Buffer.from((await esbuild.build({
  entryPoints: [path.join(ROOT, f)], bundle: true, format: 'esm', write: false })).outputFiles[0].text).toString('base64'));
const D = await load('web/src/input/hidpad.ts');
const P = await load('web/src/input/proto.ts');
const G = await load('web/src/input/gamepad.ts');
const bit = (...b) => b.reduce((m, x) => m | 1 << x, 0);

const PRO2 = '8BitDo Pro 2 (Vendor: 2dc8 Product: 6003)';                 // labelled as Nintendo's
const ULTIMATE = '8BitDo Ultimate Wireless Controller (Vendor: 2dc8 Product: 3012)';   // as Xbox's
const H = 9 / 7, c = 0.004;                                     // a hat switch let go, read as one axis; a stick at rest
// the Pro 2 under Chrome on Windows and macOS (axes at their usage): the sticks X Y and Z Rz on 0 1 2 5, R2 and L2
// let go at -1 in the places left free (3, 4), the hat switch let go on 9
const PRO2_CHROME = [c, c, c, -1, -1, c, 0, 0, 0, H];
const press = (n, ...on) => Array.from({ length: n }, (_, i) => on.includes(i));

test('8BitDo\'s controllers by vendor, in Chrome\'s and Firefox\'s ids, and how their face buttons are labelled', () => {
  assert.equal(D.eightBitDoFace(PRO2), 'nintendo');
  assert.equal(D.eightBitDoFace(ULTIMATE), 'xbox');
  assert.equal(D.eightBitDoFace('2dc8-6101-8BitDo SN30 Pro'), 'nintendo');
  assert.equal(D.eightBitDoFace('2dc8-301b-8BitDo Ultimate 2C Wireless'), 'xbox');
  assert.equal(D.eightBitDoFace('8BitDo Ultimate 2C (STANDARD GAMEPAD Vendor: 2dc8 Product: 310a)'), 'xbox');
  assert.equal(D.eightBitDoFace('Xbox 360 Controller (XInput STANDARD GAMEPAD)'), null);      // its X-input mode
  assert.equal(D.eightBitDoFace('Pro Controller (STANDARD GAMEPAD Vendor: 057e Product: 2009)'), null);   // Switch mode
  const ZIKWAY = 'Zikway HID gamepad (Vendor: 3537 Product: 1041)';             // another maker's, 8BitDo's report
  assert.equal(D.eightBitDoFace(ZIKWAY), null);
  assert.deepEqual(D.knownPad(ZIKWAY), { face: 'xbox', maker: 'Zikway', ltFirst: false });
  assert.equal(D.knownPad('3537-1041-Zikway HID gamepad')?.maker, 'Zikway');      // Firefox's id
  assert.equal(D.knownPad('3537-1042-Some other pad'), null);                     // that maker's others: not known
  assert.equal(G.familyOf(ZIKWAY).family, P.PADF_XBOX);
  assert.match(new D.HidPadReader('xbox', Array(10).fill(0), false, 'Zikway').describe(), /^a Zikway controller/);
  assert.ok(G.mayDrive({ id: ZIKWAY, mapping: '' }));
  assert.equal(G.familyOf(PRO2).family, P.PADF_NINTENDO);
  assert.equal(G.familyOf(ULTIMATE).family, P.PADF_XBOX);
  assert.equal(G.familyOf(PRO2).name, '8BitDo Pro 2');
  assert.ok(G.mayDrive({ id: PRO2, mapping: '' }));               // may take the pad from another controller
});

test('an 8BitDo pad\'s axes as each browser places them', () => {
  // by usage (Chrome on Windows and macOS, Firefox on Windows); Chrome on macOS before the first report the same
  const usage = { lx: 0, ly: 1, rx: 2, ry: 5, rt: 3, lt: 4, hat: 9 };
  assert.deepEqual(D.placeHidAxes(PRO2_CHROME), usage);
  assert.deepEqual(D.placeHidAxes(Array(10).fill(0)), usage);
  // Linux (Chrome): one after another, ABS_GAS and ABS_BRAKE, the hat switch two axes last
  assert.deepEqual(D.placeHidAxes([c, c, c, c, -1, -1, 0, 0]), { lx: 0, ly: 1, rx: 2, ry: 3, rt: 4, lt: 5, hat: -1, dpad: [6, 7] });
  // an SN30 Pro there: no analog triggers
  assert.deepEqual(D.placeHidAxes([c, c, c, c, 0, 0]), { lx: 0, ly: 1, rx: 2, ry: 3, rt: -1, lt: -1, hat: -1, dpad: [4, 5] });
  // Firefox on macOS: the descriptor's order, the hat switch one axis wherever it is
  assert.deepEqual(D.placeHidAxes([H, c, c, c, c, -1, -1]), { lx: 1, ly: 2, rx: 3, ry: 4, rt: 5, lt: 6, hat: 0 });
  // Safari: no hat switch
  assert.deepEqual(D.placeHidAxes([c, c, c, c, -1, -1]), { lx: 0, ly: 1, rx: 2, ry: 3, rt: 4, lt: 5, hat: -1 });
  assert.equal(D.placeHidAxes(Array(8).fill(0)), null);            // not read yet
});

test('a Pro 2 as the pad: B, the bottom button, is the pad\'s A; shoulders, triggers, Start and the D-pad where they belong', () => {
  const r = new D.HidPadReader('nintendo', PRO2_CHROME);
  assert.deepEqual(r.read(PRO2_CHROME, press(17)), { buttons: 0, axes: [c, c, c, c, 0, 0] });
  assert.equal(r.read(PRO2_CHROME, press(17, 1)).buttons, bit(P.PAD_A));          // B (bottom)
  assert.equal(r.read(PRO2_CHROME, press(17, 0)).buttons, bit(P.PAD_B));          // A (right)
  assert.equal(r.read(PRO2_CHROME, press(17, 4)).buttons, bit(P.PAD_X));          // Y (left)
  assert.equal(r.read(PRO2_CHROME, press(17, 3)).buttons, bit(P.PAD_Y));          // X (top)
  assert.equal(r.read(PRO2_CHROME, press(17, 6, 7, 10, 11, 12, 13, 14)).buttons,
    bit(P.PAD_LB, P.PAD_RB, P.PAD_BACK, P.PAD_START, P.PAD_HOME, P.PAD_LS, P.PAD_RS));
  assert.equal(r.read(PRO2_CHROME, press(17, 2, 5)).buttons, 0);                  // the back paddles: nothing
  const on = [-0.5, 0.25, 1, 1, 0, -1, 0, 0, 0, -1];                              // R2 down, L2 half, the right stick, up
  assert.deepEqual(r.read(on, press(17)), { buttons: bit(P.PAD_DUP), axes: [-0.5, 0.25, 1, -1, 0.5, 1] });
  assert.match(r.describe(), /nintendo buttons\), sticks on axes 0 1 and 2 5, triggers L2 4 R2 3 .* D-pad axis 9$/);
});

test('an Ultimate as the pad: labelled as Xbox\'s, A is A', () => {
  const r = new D.HidPadReader('xbox', PRO2_CHROME);
  assert.equal(r.read(PRO2_CHROME, press(17, 0)).buttons, bit(P.PAD_A));
  assert.equal(r.read(PRO2_CHROME, press(17, 1)).buttons, bit(P.PAD_B));
  assert.equal(r.read(PRO2_CHROME, press(17, 3)).buttons, bit(P.PAD_X));
  assert.equal(r.read(PRO2_CHROME, press(17, 4)).buttons, bit(P.PAD_Y));
});

test('without analog triggers (Firefox, an SN30 Pro): their buttons; Linux\'s D-pad on two axes', () => {
  const ff = [c, c, c, 0, 0, c, 0, 0, 0, H], r = new D.HidPadReader('nintendo', ff);   // the places left free read 0
  assert.deepEqual(r.read(ff, press(15, 9)).axes.slice(4), [0, 1]);
  assert.deepEqual(r.read(ff, press(15, 8)).axes.slice(4), [1, 0]);
  const linux = [c, c, c, c, 0, 0], l = new D.HidPadReader('nintendo', linux);
  assert.equal(l.read([c, c, c, c, -1, 1], press(15)).buttons, bit(P.PAD_DLEFT, P.PAD_DDOWN));
  assert.deepEqual(l.read(linux, press(15, 8, 9)).axes.slice(4), [1, 1]);
});

test('an 8BitDo pad through the poller: its own layout, the Nintendo family, and it takes the pad with a press', async () => {
  const nav = Object.getOwnPropertyDescriptor(globalThis, 'navigator'), et = new EventTarget(), pads = [];
  Object.defineProperty(globalThis, 'navigator', { value: { getGamepads: () => pads }, configurable: true });
  const ev = { addEventListener: globalThis.addEventListener, removeEventListener: globalThis.removeEventListener, dispatchEvent: globalThis.dispatchEvent };
  Object.assign(globalThis, { addEventListener: et.addEventListener.bind(et), removeEventListener: et.removeEventListener.bind(et),
    dispatchEvent: et.dispatchEvent.bind(et) });
  try {
    const buf = new SharedArrayBuffer(P.IM_BYTES), h = new Int32Array(buf), f = new Float32Array(buf), logs = [];
    const pad = (id, index, mapping, axes, n) => ({ id, index, mapping, connected: true, axes: [...axes],
      buttons: Array.from({ length: n }, () => ({ pressed: false, value: 0 })) });
    const xbox = pad('Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 0b13)', 0, 'standard', [0, 0, 0, 0], 17);
    const pro2 = pad(PRO2, 1, '', PRO2_CHROME, 17);
    pads.push(xbox, pro2);
    const p = new G.PadPoller(h, new Uint8Array(buf), 0, () => {}, s => logs.push(s));
    p.poll();
    assert.equal(G.drivingPad(), xbox);
    pro2.buttons[1] = { pressed: true, value: 1 }; pro2.axes[3] = 1; p.poll();    // B and R2 on the Pro 2: it drives
    assert.equal(G.drivingPad(), pro2);
    assert.deepEqual([h[P.IM_PAD_BUTTONS], h[P.IM_PAD_FAMILY], f[P.IM_PAD_AXIS + P.PAD_AXIS_RT]], [bit(P.PAD_A), P.PADF_NINTENDO, 1]);
    assert.ok(logs.some(s => /read as an 8BitDo controller \(nintendo buttons\), sticks on axes 0 1 and 2 5/.test(s)));
    p.dispose();
  } finally {
    if (nav) Object.defineProperty(globalThis, 'navigator', nav); else delete globalThis.navigator;
    Object.assign(globalThis, ev);
  }
});

/** fn(pads) with navigator.getGamepads() giving pads, the browser's user agent and touch points, and the window's
 *  events on a target of its own */
async function onBrowser(userAgent, maxTouchPoints, fn) {
  const nav = Object.getOwnPropertyDescriptor(globalThis, 'navigator'), et = new EventTarget(), pads = [];
  Object.defineProperty(globalThis, 'navigator', { value: { getGamepads: () => pads, userAgent, maxTouchPoints }, configurable: true });
  const ev = { addEventListener: globalThis.addEventListener, removeEventListener: globalThis.removeEventListener, dispatchEvent: globalThis.dispatchEvent };
  Object.assign(globalThis, { addEventListener: et.addEventListener.bind(et), removeEventListener: et.removeEventListener.bind(et),
    dispatchEvent: et.dispatchEvent.bind(et) });
  try { await fn(pads); } finally {
    if (nav) Object.defineProperty(globalThis, 'navigator', nav); else delete globalThis.navigator;
    Object.assign(globalThis, ev);
  }
}
const ANDROID_UA = 'Mozilla/5.0 (Linux; Android 14; SM-X826B) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36';
const DESKTOP_SITE_UA = 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36';
// an Ultimate 2 under Chrome on Android: Android's key codes put it in the standard mapping's places (sticks on 0-3,
// the triggers buttons 6 and 7, the D-pad 12-15, Home 16), but Chrome knows no mapping of its own for it
const ULTIMATE2_ANDROID = '8BitDo Ultimate 2 Wireless (Vendor: 2dc8 Product: 6012)';

/** an Ultimate 2 on a browser through the poller: what the pad gets for its buttons and the log */
async function ultimate2On(ua, touch) {
  const G2 = await load('web/src/input/gamepad.ts');
  const out = { logs: [] };
  await onBrowser(ua, touch, async (pads) => {
    const buf = new SharedArrayBuffer(P.IM_BYTES), h = new Int32Array(buf), f = new Float32Array(buf);
    const g = { id: ULTIMATE2_ANDROID, index: 0, mapping: '', connected: true, axes: [0, 0, 0, 0],
      buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })) };
    pads.push(g);
    const p = new G2.PadPoller(h, new Uint8Array(buf), 0, () => {}, s => out.logs.push(s));
    const on = (...b) => { g.buttons.forEach((_, i) => { g.buttons[i] = { pressed: b.includes(i), value: b.includes(i) ? 1 : 0 }; }); p.poll(); return h[P.IM_PAD_BUTTONS]; };
    out.mayDrive = G2.mayDrive(g);
    out.face = [on(0), on(1), on(2), on(3)];
    out.shoulders = on(4, 5);
    out.dpad = [on(12), on(13), on(14), on(15)];
    out.rest = on(8, 9, 10, 11);
    on(); g.buttons[7] = { pressed: true, value: 0.75 }; g.axes[0] = -0.5; p.poll();
    out.gas = +f[P.IM_PAD_AXIS + P.PAD_AXIS_RT].toFixed(2); out.lx = f[P.IM_PAD_AXIS + P.PAD_AXIS_LX];
    p.dispose();
  });
  return out;
}

test('an 8BitDo pad under Chrome on Android: Android\'s places as they come, not its HID report\'s (a report: nothing worked)', async () => {
  const want = { face: [bit(P.PAD_A), bit(P.PAD_B), bit(P.PAD_X), bit(P.PAD_Y)], shoulders: bit(P.PAD_LB, P.PAD_RB),
    dpad: [bit(P.PAD_DUP), bit(P.PAD_DDOWN), bit(P.PAD_DLEFT), bit(P.PAD_DRIGHT)],
    rest: bit(P.PAD_BACK, P.PAD_START, P.PAD_LS, P.PAD_RS), gas: 0.75, lx: -0.5 };
  for (const [ua, touch] of [[ANDROID_UA, 5], [DESKTOP_SITE_UA, 5]]) {   // a phone, and a big tablet asking for desktop sites
    const r = await ultimate2On(ua, touch);
    assert.ok(r.mayDrive);
    assert.deepEqual({ face: r.face, shoulders: r.shoulders, dpad: r.dpad, rest: r.rest, gas: r.gas, lx: r.lx }, want, ua);
    assert.ok(r.logs.some(s => /read as it is: Android put it in the standard mapping's places/.test(s)));
    assert.ok(!r.logs.some(s => /8BitDo controller/.test(s)));
  }
  // Chrome on a Linux desktop (no touch screen): the report's order, through the 8BitDo layout as before
  const linux = await ultimate2On(DESKTOP_SITE_UA, 0);
  assert.ok(linux.logs.some(s => /read as an 8BitDo controller \(xbox buttons\)/.test(s)));
  assert.equal(linux.face[2], 0);                                  // its index 2 is a gap there
});

test('Xbox\'s Bluetooth controllers in Firefox on a Mac: their own layout, LT before RT (a Mac player\'s: the triggers did nothing)', () => {
  const MAC = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:153.0) Gecko/20100101 Firefox/153.0';
  const WIN = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:153.0) Gecko/20100101 Firefox/153.0';
  const ONE_S = '45e-b20-Xbox Wireless Controller';                  // Firefox's id on macOS (no leading zeros)
  assert.deepEqual(D.knownPad(ONE_S, MAC), { face: 'xbox', maker: 'Xbox', ltFirst: true });
  assert.equal(D.knownPad('45e-b22-Xbox Elite Wireless Controller Series 2', MAC)?.maker, 'Xbox');
  assert.equal(D.knownPad(ONE_S, WIN), null);                        // Windows: no triggers in such a pad, read as before
  assert.equal(D.knownPad(ONE_S, 'Mozilla/5.0 (X11; Linux x86_64; rv:153.0) Gecko/20100101 Firefox/153.0'), null);
  assert.equal(D.knownPad('45e-2e0-Xbox Wireless Controller', MAC), null);   // the older firmware: Firefox maps it
  assert.equal(D.knownPad(ULTIMATE, WIN)?.maker, '8BitDo');
  // the report's order: the sticks X Y and Z Rz, the Brake (LT) and the Accelerator (RT) let go at -1, the hat switch
  const ff = [c, c, c, c, -1, -1, H], k = D.knownPad(ONE_S, MAC);
  const r = new D.HidPadReader(k.face, ff, k.ltFirst, k.maker);
  assert.deepEqual(r.read(ff, press(15)), { buttons: 0, axes: [c, c, c, c, 0, 0] });
  assert.deepEqual(r.read([c, c, c, c, -1, 1, H], press(15)).axes.slice(4), [0, 1]);   // RT all the way: throttle
  assert.deepEqual(r.read([c, c, c, c, 0, -1, H], press(15)).axes.slice(4), [0.5, 0]); // LT half way: brake
  assert.equal(r.read(ff, press(15, 0)).buttons, bit(P.PAD_A));
  assert.equal(r.read(ff, press(15, 3)).buttons, bit(P.PAD_X));
  assert.equal(r.read(ff, press(15, 4)).buttons, bit(P.PAD_Y));
  assert.equal(r.read(ff, press(15, 6, 7, 10, 11, 13, 14)).buttons, bit(P.PAD_LB, P.PAD_RB, P.PAD_BACK, P.PAD_START, P.PAD_LS, P.PAD_RS));
  assert.match(r.describe(), /^an Xbox controller \(xbox buttons\), sticks on axes 0 1 and 2 3, triggers L2 4 R2 5/);
  // where Chrome places them by usage (no mapping of its own): LT on 3, RT on 4, as Chromium's MapperXboxBluetooth
  assert.deepEqual(D.placeHidAxes([c, c, c, -1, -1, c, 0, 0, 0, H], true), { lx: 0, ly: 1, rx: 2, ry: 5, lt: 3, rt: 4, hat: 9 });
});

// Xbox's One S with its 2016 firmware's id (045e 02e0) under Chrome on Android: Android's own key layout for the id
// (Vendor_045e_Product_02e0.kl) gives the usual key codes, and Chrome's XboxOneS2016FirmwareMappings (GamepadMappings.java)
// maps those as if they were the raw ones.  chromeOneS2016() is that mapping, over what the pad sends through the key
// layout: keys by Android's names (A B X Y L1 R1 SELECT START THUMBL THUMBR MODE), the sticks x y and z rz, the
// triggers lt rt, the hat hx hy.
const ONE_S_2016 = 'Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 02e0)';
function chromeOneS2016() {
  let lt = false, rt = false;                      // Chrome's "activated": AXIS_Z, AXIS_RZ read anything but 0
  return ({ keys = [], x = 0, y = 0, z = 0, rz = 0, hx = 0, hy = 0 } = {}) => {
    const k = n => keys.includes(n) ? 1 : 0, b = Array(16).fill(0);
    if (z !== 0) lt = true;
    if (rz !== 0) rt = true;
    b[0] = k('A'); b[1] = k('B'); b[2] = k('C'); b[3] = k('X'); b[4] = k('Y'); b[5] = k('Z');
    b[8] = k('L1'); b[9] = k('R1'); b[10] = k('L2'); b[11] = k('R2');
    b[6] = lt ? (z + 1) / 2 : 0; b[7] = rt ? (rz + 1) / 2 : 0;
    b[14] = hx < -0.5 ? 1 : 0; b[15] = hx > 0.5 ? 1 : 0; b[12] = hy < -0.5 ? 1 : 0; b[13] = hy > 0.5 ? 1 : 0;
    // AXIS_RX and RY: nothing of the pad's goes there through the key layout
    return { axes: [x, y, 0, 0], buttons: b.map(v => ({ pressed: v > 0.12, value: v })) };
  };
}

test('Xbox\'s One S 2016 id under Chrome on Android: which pads are read so', () => {
  const g = (id, mapping = 'standard', n = 16) => ({ id, mapping, buttons: Array(n).fill({ pressed: false, value: 0 }) });
  assert.ok(D.oneS2016Android(g(ONE_S_2016), ANDROID_UA, 5));
  assert.ok(D.oneS2016Android(g(ONE_S_2016), DESKTOP_SITE_UA, 5));          // a big tablet asking for desktop sites
  assert.ok(!D.oneS2016Android(g(ONE_S_2016), DESKTOP_SITE_UA, 0));         // a Linux computer: Chrome's own mapping there
  assert.ok(!D.oneS2016Android(g(ONE_S_2016, 'standard', 17), ANDROID_UA, 5)); // not that mapping (it has no Home)
  assert.ok(!D.oneS2016Android(g(ONE_S_2016, ''), ANDROID_UA, 5));
  assert.ok(!D.oneS2016Android(g(ONE_S_2016), 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/157.0.0.0 Safari/537.36', 0));
  assert.ok(!D.oneS2016Android(g('45e-2e0-Xbox Wireless Controller'), ANDROID_UA, 5));   // Firefox's id
  for (const p of ['0b13', '0b20', '02fd'])                                   // the other Xbox ids: Chrome maps them right
    assert.ok(!D.oneS2016Android(g(`Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: ${p})`), ANDROID_UA, 5), p);
});

test('Xbox\'s One S 2016 id under Chrome on Android: its buttons where they belong, the right stick a stick (a report: LT and RT dead, the right stick drove)', async () => {
  const G2 = await load('web/src/input/gamepad.ts');
  await onBrowser(ANDROID_UA, 5, async (pads) => {
    const buf = new SharedArrayBuffer(P.IM_BYTES), h = new Int32Array(buf), f = new Float32Array(buf), logs = [];
    const chrome = chromeOneS2016(), g = { id: ONE_S_2016, index: 0, mapping: 'standard', connected: true, ...chrome() };
    pads.push(g);
    const p = new G2.PadPoller(h, new Uint8Array(buf), 0, () => {}, s => logs.push(s));
    const send = (raw) => { Object.assign(g, chrome(raw)); p.poll(); };
    const pad = () => ({ buttons: h[P.IM_PAD_BUTTONS], axes: Array.from({ length: 6 }, (_, i) => +f[P.IM_PAD_AXIS + i].toFixed(2)) });
    const press = (...keys) => { send({ keys }); return pad().buttons; };
    // read as the standard mapping it was the report: X was Y, Y LB, LB View, RB Menu
    const bare = (raw) => { const c = chromeOneS2016(); return G2.mapStandard({ ...g, ...c(raw) }); };
    assert.equal(bare({ keys: ['X'] }).buttons, bit(P.PAD_Y));
    assert.equal(bare({ keys: ['Y'] }).buttons, bit(P.PAD_LB));
    assert.equal(bare({ keys: ['R1'] }).buttons, bit(P.PAD_START));
    assert.equal(bare({ rz: 1 }).axes[P.PAD_AXIS_RT], 1);                     // the right stick down: the throttle
    // now
    assert.deepEqual([press('A'), press('B'), press('X'), press('Y')], [bit(P.PAD_A), bit(P.PAD_B), bit(P.PAD_X), bit(P.PAD_Y)]);
    assert.equal(press('L1', 'R1'), bit(P.PAD_LB, P.PAD_RB));
    send({ hx: 1 }); assert.equal(pad().buttons, bit(P.PAD_DRIGHT));
    send({ hy: -1 }); assert.equal(pad().buttons, bit(P.PAD_DUP));
    // what Chrome never reads: View, Menu, the sticks' buttons, Xbox
    assert.equal(press('SELECT', 'START', 'THUMBL', 'THUMBR', 'MODE'), 0);
    // the right stick: down and right, back to rest, all the way left and up; never a trigger
    const axes = (rx, ry) => { const a = [0, 0, 0, 0, 0, 0]; a[P.PAD_AXIS_RX] = rx; a[P.PAD_AXIS_RY] = ry; return a; };
    send({ z: 0.5, rz: 1 }); assert.deepEqual(pad().axes, axes(0.5, 1));
    send({}); assert.deepEqual(pad().axes, axes(0, 0));
    send({ z: -1, rz: -1 }); assert.deepEqual(pad().axes, axes(-1, -1));
    // the left stick as it is
    send({ x: -0.5, y: 0.25 });
    assert.equal(pad().axes[P.PAD_AXIS_LX], -0.5); assert.equal(pad().axes[P.PAD_AXIS_LY], 0.25);
    assert.equal(h[P.IM_PAD_FAMILY], P.PADF_XBOX);
    assert.ok(logs.some(s => /read as Xbox's One S with its 2016 firmware under Chrome on Android/.test(s)));
    p.dispose();
  });
});

test('Xbox\'s One S 2016 id: before Chrome saw the right stick move, its trigger places at 0 are the stick at rest', () => {
  const r = new D.OneS2016Reader(), chrome = chromeOneS2016(), none = { pressed: false, value: 0 };
  const read = (raw) => { const c = chrome(raw); return r.read(c.axes, c.buttons).axes; };
  assert.deepEqual(read({}), [0, 0, 0, 0, 0, 0]);
  assert.deepEqual(read({ rz: 0.5 }), [0, 0, 0, 0.5, 0, 0]);               // Y moved: its place counts, X's not yet
  assert.deepEqual(read({ z: -1, rz: 0.5 }), [0, 0, 0, 0.5, 0, 0]);       // X all the way left first: 0, read as rest
  assert.deepEqual(read({ z: -0.5, rz: 0.5 }), [0, 0, -0.5, 0.5, 0, 0]);  // until it moves
  assert.deepEqual(r.read([0, 0], [none]).axes, [0, 0, 0, 0, 0, 0]);       // fewer buttons than it should have: no throw
});

test('a phone controller with a touch pad for its right stick under Chrome on Android: a still stick off centre reads as let go (a report: one tap turned the view for good)', async () => {
  const G2 = await load('web/src/input/gamepad.ts');
  const realNow = performance.now;
  let t = 1000;
  performance.now = () => t;
  try {
    await onBrowser(ANDROID_UA, 5, async (pads) => {
      const buf = new SharedArrayBuffer(P.IM_BYTES), h = new Int32Array(buf), f = new Float32Array(buf), logs = [];
      // the player's tester screen: 17 buttons, 9 axes, no mapping; after a tap axes 2 and 3 stayed at -0.97
      const g = { id: 'GT NX Controller-T (Vendor: 4131 Product: 0009)', index: 0, mapping: '', connected: true,
        axes: [0, 0, 0, 0, 0, 0, 0, 0, -1], buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })) };
      pads.push(g);
      const p = new G2.PadPoller(h, new Uint8Array(buf), 0, () => {}, s => logs.push(s));
      const rs = () => [+f[P.IM_PAD_AXIS + P.PAD_AXIS_RX].toFixed(2), +f[P.IM_PAD_AXIS + P.PAD_AXIS_RY].toFixed(2)];
      const at = (ms, x, y) => { t += ms; if (x !== undefined) { g.axes[2] = x; g.axes[3] = y; } p.poll(); return rs(); };
      at(0);
      assert.deepEqual(at(16, -0.4, 0.2), [-0.4, 0.2]);           // a finger on the pad
      assert.deepEqual(at(16, -0.9671, -0.9685), [-0.97, -0.97]);  // its last place, the finger off
      assert.deepEqual(at(1900), [-0.97, -0.97]);                   // still read for 2 s: a stick held there
      assert.deepEqual(at(200), [0, 0]);                            // then as let go
      assert.ok(logs.some(s => /right stick stayed at \[-0\.97 -0\.97\] for 2 s/.test(s)));
      assert.deepEqual(at(16, -0.9671, -0.9684), [-0.97, -0.97]);  // it moves: read again
      assert.deepEqual(at(16, 0, 0), [0, 0]);
      assert.deepEqual(at(5000), [0, 0]);                           // at rest: nothing to do
      p.dispose();
    });
  } finally { performance.now = realNow; }
});

test('a standard-mapping controller\'s right stick held still is read as it is', async () => {
  const G2 = await load('web/src/input/gamepad.ts');
  const realNow = performance.now;
  let t = 1000;
  performance.now = () => t;
  try {
    await onBrowser(ANDROID_UA, 5, async (pads) => {
      const buf = new SharedArrayBuffer(P.IM_BYTES), h = new Int32Array(buf), f = new Float32Array(buf);
      const g = { id: 'Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 0b13)', index: 0, mapping: 'standard',
        connected: true, axes: [0, 0, 0, 1], buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })) };
      pads.push(g);
      const p = new G2.PadPoller(h, new Uint8Array(buf), 0, () => {}, () => {});
      p.poll(); t += 5000; p.poll();
      assert.equal(f[P.IM_PAD_AXIS + P.PAD_AXIS_RY], 1);          // looking back all the while
      p.dispose();
    });
  } finally { performance.now = realNow; }
});
