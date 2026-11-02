// The touch controls' Pressurize and Bounce (web/src/app/touch.ts) on a fake page and clock: the one on holds its key
// (H, J), let go and pressed again HOLD_MS after the car gets slow enough (the game's status "hydraulics" 2), and a
// tap off always lets go of it in the game, however the taps, the status and that press again fall.
//   node --test web/test/touch.test.mjs
import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

// the page as touch.ts uses it: elements that keep their attributes, classes and listeners, a window's events, a
// touch screen, the frames and the clock driven by the tests
class El {
  constructor() {
    this.dataset = {}; this.attrs = new Map(); this.listeners = {}; this.hidden = false; this.innerHTML = ''; this.textContent = '';
    this.style = { setProperty() {}, removeProperty() {} };
    const cls = this.cls = new Set();
    this.classList = {
      add: (...c) => c.forEach(x => cls.add(x)), remove: (...c) => c.forEach(x => cls.delete(x)),
      toggle: (c, on = !cls.has(c)) => { if (on) cls.add(c); else cls.delete(c); return on; }, contains: c => cls.has(c),
    };
  }
  setAttribute(k, v) { this.attrs.set(k, String(v)); }
  hasAttribute(k) { return this.attrs.has(k); }
  toggleAttribute(k, on = !this.attrs.has(k)) { if (on) this.attrs.set(k, ''); else this.attrs.delete(k); return on; }
  append() {}
  replaceChildren() {}
  addEventListener(type, f) { (this.listeners[type] ??= []).push(f); }
  getBoundingClientRect() { return { left: 0, top: 0, width: 0, height: 0 }; }
  closest(sel) { return sel === '[data-ctl]' && this.dataset.ctl ? this : null; }
}
const win = new EventTarget();
let now = 1000, raf = null;
Object.assign(globalThis, {
  document: { createElement: () => new El(), documentElement: new El(), addEventListener() {}, elementFromPoint: () => null },
  getComputedStyle: () => ({ paddingLeft: '12px', paddingRight: '12px', paddingTop: '12px', paddingBottom: '12px' }),
  innerWidth: 1280, innerHeight: 720,
  addEventListener: win.addEventListener.bind(win), removeEventListener: win.removeEventListener.bind(win),
  dispatchEvent: win.dispatchEvent.bind(win),
  localStorage: { getItem: () => null, setItem() {}, removeItem() {} },
  matchMedia: () => ({ matches: true, addEventListener() {} }),
  requestAnimationFrame: f => { raf = f; return 1; },
  cancelAnimationFrame: () => { raf = null; },
});
Object.defineProperty(globalThis, 'performance', { value: { now: () => now }, configurable: true });

const js = (await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/app/touch.ts')], bundle: true, format: 'esm', write: false })).outputFiles[0].text;
const { TouchControls } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

const H = 'KeyH', J = 'KeyJ';

// a game in free roam (drive 2) with a car with hydraulics, too fast to start them (1) until the test says
function setup() {
  mock.timers.enable({ apis: ['setTimeout'] });
  const status = new Map([['drive', 2], ['frames', 100], ['hydraulics', 1]]);
  const sent = [];
  const session = () => ({
    status: { read: () => status },
    input: { key: (code, down) => sent.push([code, down]), touchPad() {}, menuScale() {}, padPressed: () => 0, viewRoll() {} },
  });
  const tc = new TouchControls(new El(), { toast() {}, log() {} });
  tc.start(session(), false);
  // a frame: the timers run as the page's clock goes (late: they lag behind it, as on a busy page)
  const frame = (late = false) => { now += 17; if (!late) mock.timers.tick(17); raf?.(); };
  const run = ms => { for (let k = 0; k < ms; k += 17) frame(); };
  // frames until the controls have what the game says (they read it every 100 ms)
  const until = (fn, late = false) => {
    for (let k = 0; !fn(); k += 17) { assert.ok(k < 1000, 'the controls read the status'); frame(late); }
  };
  // the game's status changes ('drive', 'hydraulics'), and the controls have read it: anything next happens within
  // HOLD_MS of that
  const set = (k, v, late = false) => { status.set(k, v); until(() => tc.state()[k] === v, late); };
  // a tap on a control: a finger down and up on it
  const tap = id => {
    const target = tc.ctls.get(id).el;
    for (const type of ['pointerdown', 'pointerup']) {
      const e = { type, target, pointerId: 7, pointerType: 'touch', isPrimary: true, clientX: 0, clientY: 0, preventDefault() {} };
      for (const f of tc.root.listeners[type]) f(e);
    }
  };
  // the key down in the game (its last press or release), and down as the controls count it
  const held = code => sent.findLast(([c]) => c === code)?.[1] === true;
  const counted = code => tc.state().keys.includes(code);
  until(() => tc.state().shown);
  return { tc, status, sent, session, run, until, set, tap, held, counted };
}
const done = (x) => { x.tc.stop(); mock.timers.reset(); };

test('the key is pressed again once the car is slow enough, and a tap off lets go even while that press waits', () => {
  const x = setup();
  try {
    x.tap('lift');
    assert.ok(x.held(H), 'Pressurize on holds H');
    x.set('hydraulics', 2);
    assert.ok(!x.held(H), 'let go as the car got slow enough');
    x.run(200);
    assert.ok(x.held(H), 'and pressed again HOLD_MS later');
    assert.deepEqual(x.sent.filter(([c]) => c === H).map(([, d]) => d), [true, false, true]);

    // off and on again within HOLD_MS of the status going to 2: the press again waiting is dropped
    x.set('hydraulics', 1);
    x.set('hydraulics', 2);
    assert.ok(!x.held(H), 'let go, to be pressed again');
    x.tap('lift'); x.tap('lift');
    x.run(500);
    assert.ok(x.held(H), 'on again');
    x.tap('lift');
    assert.ok(!x.held(H) && !x.counted(H), 'a tap off lets go of H');
    x.run(500);
    assert.ok(!x.held(H) && !x.counted(H), 'and nothing presses it again');

    // Bounce on in Pressurize's place while its press again waits: J held, H not pressed again
    x.tap('lift');
    x.set('hydraulics', 1);
    x.set('hydraulics', 2);
    x.tap('bounce');
    x.run(500);
    assert.ok(x.held(J) && !x.held(H) && !x.counted(H), 'Bounce holds J, H stays up');
    x.tap('bounce');
    assert.ok(!x.held(J) && !x.counted(J), 'a tap off lets go of J');
  } finally { done(x); }
});

test('the status back at 2 before the press again ran (a busy page) holds the key once', () => {
  const x = setup();
  try {
    x.tap('lift');
    x.set('hydraulics', 2, true);
    x.set('hydraulics', 1, true);
    x.set('hydraulics', 2, true);
    x.run(500);
    assert.ok(x.held(H), 'H pressed again');
    x.tap('lift');
    assert.ok(!x.held(H) && !x.counted(H), 'a tap off lets go of H');
  } finally { done(x); }
});

// each and back before the press again ran (a busy page), then Pressurize on and off again
const LEAVE = {
  'a menu (the driving controls go)': x => { x.set('drive', 0, true); x.set('drive', 2, true); },
  'a car without hydraulics (the buttons hide)': x => { x.set('hydraulics', 0, true); x.set('hydraulics', 2, true); },
  'the game ending (stop)': x => { x.tc.stop(); x.tc.start(x.session(), false); x.until(() => x.tc.state().shown, true); },
};
for (const [way, leave] of Object.entries(LEAVE)) {
  test(`${way} drops the press again waiting`, () => {
    const x = setup();
    try {
      x.tap('lift');
      x.set('hydraulics', 2, true);           // H let go, its press again waiting
      leave(x);
      assert.equal(x.tc.state().hyd, '', 'Pressurize off');
      assert.equal(x.tc.ctls.get('lift').el.classList.contains('lit'), false, 'Pressurize not lit');
      x.tap('lift');
      x.run(500);
      assert.ok(x.held(H), 'Pressurize on again holds H');
      x.tap('lift');
      assert.ok(!x.held(H) && !x.counted(H), 'a tap off lets go of H');
    } finally { done(x); }
  });
}
