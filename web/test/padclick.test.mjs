// The right stick's click through the poller (web/src/input/gamepad.ts forPage, PadPage.click): pressed in alone it
// goes to the page first (quick chat's picker: quickchat.ts click), and when the page takes it the game never gets
// it; when the page leaves it, the game gets it as before (a car's hydraulics: a tap, or held once the menu's
// shortcut could no longer follow); both sticks are the page's menu and no click; photo mode's capture and the page's
// dialogs keep it from the click.  The stick itself reaches the game unless the page's stick() takes it (the game's
// look round the car: runtime/look.c).
//   node --test web/test/padclick.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const load = async (f) => import('data:text/javascript;base64,' + Buffer.from((await esbuild.build({
  entryPoints: [path.join(ROOT, f)], bundle: true, format: 'esm', write: false })).outputFiles[0].text).toString('base64'));
const P = await load('web/src/input/proto.ts');
const G = await load('web/src/input/gamepad.ts');
const RS = 1 << P.PAD_RS;

/** fn(rig) with a standard pad on navigator.getGamepads(), a clock of the test's own (performance.now) and a page */
async function rig(fn) {
  const nav = Object.getOwnPropertyDescriptor(globalThis, 'navigator'), et = new EventTarget(), pads = [];
  Object.defineProperty(globalThis, 'navigator', { value: { getGamepads: () => pads }, configurable: true });
  const ev = { addEventListener: globalThis.addEventListener, removeEventListener: globalThis.removeEventListener, dispatchEvent: globalThis.dispatchEvent };
  Object.assign(globalThis, { addEventListener: et.addEventListener.bind(et), removeEventListener: et.removeEventListener.bind(et),
    dispatchEvent: et.dispatchEvent.bind(et) });
  const realNow = performance.now;
  let t = 1000;
  performance.now = () => t;
  try {
    const buf = new SharedArrayBuffer(4096), h = new Int32Array(buf), f = new Float32Array(buf);
    const g = { id: 'Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 0b13)', index: 0, mapping: 'standard',
      connected: true, axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })) };
    pads.push(g);
    const page = { clicks: 0, menus: 0, take: true, dialogOpen: false, captures: false, stickTakes: false,
      menu() { this.menus++; }, dialog() { return this.dialogOpen; }, press() {},
      stick() { return this.stickTakes; }, capture() { return this.captures; },
      click() { this.clicks++; return this.take; } };
    const p = new G.PadPoller(h, new Uint8Array(buf), 0, () => {}, () => {}, undefined, page);
    const set = (i, on) => { g.buttons[i] = { pressed: on, value: on ? 1 : 0 }; };
    /** the buttons and the right stick the game got after a read ms later */
    const read = (ms = 16) => { t += ms; p.poll(); return { buttons: h[P.IM_PAD_BUTTONS], rx: f[P.IM_PAD_AXIS + P.PAD_AXIS_RX] }; };
    read();
    await fn({ g, page, set, read });
    p.dispose();
  } finally {
    performance.now = realNow;
    if (nav) Object.defineProperty(globalThis, 'navigator', nav); else delete globalThis.navigator;
    Object.assign(globalThis, ev);
  }
}

test('a click the page takes: never the game\'s, tapped or held', async () => {
  await rig(async ({ page, set, read }) => {
    set(11, true);                                           // the right stick in (the standard mapping's 11)
    assert.equal(read().buttons & RS, 0, 'kept while the left one may follow');
    set(11, false);
    assert.equal(read().buttons & RS, 0, 'let go before: no tap for the game');
    assert.equal(page.clicks, 1);
    for (let i = 0; i < 10; i++) assert.equal(read().buttons & RS, 0);
    set(11, true);
    read();
    for (let i = 0; i < 12; i++) assert.equal(read().buttons & RS, 0, 'held: the game does not get it either');
    assert.equal(page.clicks, 2, 'once per click');
    set(11, false);
    assert.equal(read().buttons & RS, 0);
  });
});

test('a click the page leaves: the game\'s, as before (a car\'s hydraulics)', async () => {
  await rig(async ({ page, set, read }) => {
    page.take = false;
    set(11, true);
    read();
    set(11, false);
    assert.equal(read().buttons & RS, RS, 'let go before: the game gets a tap');
    assert.equal(page.clicks, 1);
    read(200);
    assert.equal(read().buttons & RS, 0, 'for a moment');
    set(11, true);
    read();
    assert.equal(read(150).buttons & RS, RS, 'held past the shortcut\'s wait: the game gets it held');
    assert.equal(page.clicks, 2);
  });
});

test('both sticks are the page\'s menu, no click; photo mode and the page\'s dialogs keep the click', async () => {
  await rig(async ({ page, set, read }) => {
    set(11, true); read(); set(10, true); read();
    assert.equal(page.menus, 1);
    set(10, false); set(11, false); read(200); read();
    assert.equal(page.clicks, 0);
    page.captures = true;
    set(11, true); read(); read(150); set(11, false); read();
    page.captures = false; page.dialogOpen = true;
    set(11, true); read(); read(150); set(11, false); read();
    assert.equal(page.clicks, 0);
  });
});

test('the right stick reaches the game (its look round the car) unless the page\'s stick() takes it', async () => {
  await rig(async ({ g, page, read }) => {
    g.axes[2] = 1;
    assert.equal(read().rx, 1);
    page.stickTakes = true;
    assert.equal(read().rx, 0, 'quick chat\'s picker has it: at rest for the game');
  });
});
