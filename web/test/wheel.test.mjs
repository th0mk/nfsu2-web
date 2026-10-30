// Steering wheels through the Gamepad API (web/src/input/wheel.ts, gamepad.ts): which controllers are wheels, a known
// model's layout as each browser places its axes, the layout guessed from where a wheel's axes sat when it showed
// up, the pad they make, the shifter's gears, the set-up's steps, and which of a wheel and a controller drives the pad.
//   node --test web/test/wheel.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
// tag: another instance of the module (its own module state)
const load = async (f, tag = '') => import('data:text/javascript;base64,' + Buffer.from((await esbuild.build({
  entryPoints: [path.join(ROOT, f)], bundle: true, format: 'esm', write: false })).outputFiles[0].text + tag).toString('base64'));
const W = await load('web/src/input/wheel.ts');
const P = await load('web/src/input/proto.ts');
const G = await load('web/src/input/gamepad.ts');
const bit = (...b) => b.reduce((m, x) => m | 1 << x, 0);

const G29 = 'G29 Driving Force Racing Wheel (Vendor: 046d Product: c24f)';
const G920 = 'Logitech G920 Driving Force Racing Wheel (Vendor: 046d Product: c262)';
const H = 9 / 7;                                                // a hat switch let go, read as one axis
// a G29 under Chrome on Windows and macOS (axes at their usage): X the wheel, Y the clutch, Z the gas, Rz the brake
// (the pedals let go at +1), the hat switch let go on axis 9
const G29_REST = [0.002, 1, 1, 0, 0, 1, 0, 0, 0, H];

test('wheels by their USB ids, in Chrome\'s and Firefox\'s ids, and by name', () => {
  assert.equal(W.wheelOf(G29).layout, 'ps');
  assert.equal(W.wheelOf(G29).degrees, 900);
  assert.deepEqual(W.wheelOf(G29).axes.axes, ['hat', 'x', 'z', 'rz', 'y']);
  assert.equal(W.wheelOf('046d-c24f-G29 Driving Force Racing Wheel').layout, 'ps');
  assert.equal(W.wheelOf('46d-c262-Logitech G920 Driving Force Racing Wheel').layout, 'xbox');
  assert.equal(W.wheelOf('Thrustmaster T300RS Racing wheel (Vendor: 044f Product: b66e)').layout, 'generic');
  assert.equal(W.wheelOf('Thrustmaster T300RS Racing wheel (Vendor: 044f Product: b66e)').axes, undefined);
  assert.equal(W.wheelOf('FANATEC CSL Elite Wheel Base (Vendor: 0eb7 Product: 0e03)').degrees, 0);
  assert.equal(W.wheelOf('Some USB Wheel (Vendor: 1234 Product: 5678)').layout, 'generic');
  assert.equal(W.wheelOf('Logitech Gamepad F310 (STANDARD GAMEPAD Vendor: 046d Product: c216)'), null);
  assert.equal(W.wheelOf('Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 0b13)'), null);
  assert.equal(W.wheelOf('DualSense Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 0ce6)'), null);
});

test('a G29 let go makes a pad at rest: no stick held down (the report: "down" pressed all the time)', () => {
  const m = W.guessMap(W.wheelOf(G29), G29_REST, 25);
  const hat = W.findHat(G29_REST);
  assert.equal(hat, 9);
  const r = W.readWheel(G29_REST, Array(25).fill(false), m, hat);
  assert.equal(r.buttons, 0);
  assert.deepEqual(r.axes.map(v => +v.toFixed(3)), [0.007, 0, 0, 0, 0, 0]);
});

test('the G29\'s layout from its descriptor: the wheel, Z the gas, Rz the brake, its buttons', () => {
  const m = W.guessMap(W.wheelOf(G29), G29_REST, 25);
  assert.equal(m.placed, 'usage');
  assert.equal(m.steer, 0);
  assert.equal(+m.lock.toFixed(3), 0.3);                      // 270 of its 900 degrees
  assert.deepEqual(m.gas, { axis: 2, rest: 1, full: -1 });
  assert.deepEqual(m.brake, { axis: 5, rest: 1, full: -1 });
  assert.equal(m.dpad, undefined);
  assert.match(W.describeMap(m, 9),
    /gas axis 2 .* brake axis 5 .* D-pad axis 9, shifter buttons 12 13 14 15 16 17 18, the model's own \(axes placed by usage\)$/);
  const press = (...i) => W.readWheel(G29_REST, Array.from({ length: 25 }, (_, j) => i.includes(j)), m, 9).buttons;
  assert.equal(press(0), bit(P.PAD_A));                       // Cross
  assert.equal(press(1), bit(P.PAD_X));                       // Square
  assert.equal(press(2), bit(P.PAD_B));                       // Circle
  assert.equal(press(3), bit(P.PAD_Y));                       // Triangle
  assert.equal(press(4, 5), bit(P.PAD_RB, P.PAD_LB));         // the paddles
  assert.equal(press(8, 9), bit(P.PAD_BACK, P.PAD_START));    // Share, Options
  assert.equal(press(10, 11), bit(P.PAD_RS, P.PAD_LS));       // R3, L3
  assert.equal(press(12, 13, 18), 0);                         // the shifter: none of the pad's buttons (Shifter)
  assert.deepEqual(m.gears, [12, 13, 14, 15, 16, 17, 18]);
  assert.equal(press(21), bit(P.PAD_DDOWN));                  // the dial
  assert.equal(press(23), bit(P.PAD_A));                      // Enter
  assert.equal(press(24), bit(P.PAD_HOME));                   // PS: the page's menu
});

test('other Logitech wheels\' buttons and shifters: the G25, G27 and Driving Force GT in the G29\'s order, the G920\'s', () => {
  const map = (product, axes, n) => W.guessMap(W.wheelOf(`Logitech wheel (Vendor: 046d Product: ${product})`), axes, n);
  const press = (m, ...i) => W.readWheel([], Array.from({ length: m.buttons.length }, (_, j) => i.includes(j)), m, -1).buttons;
  for (const [product, n, gears] of [['c299', 19, [12, 13, 14, 15, 16, 17, 18]], ['c29b', 23, [12, 13, 14, 15, 16, 17, 22]]]) {
    const m = map(product, [0, 1, 1, 0, 0, 1, 0, 0, 0, H], n);
    assert.equal(press(m, 4), bit(P.PAD_RB), product);                  // the right paddle shifts up
    assert.equal(press(m, 5), bit(P.PAD_LB), product);
    assert.equal(press(m, ...gears), 0, product);                       // the gears were the D-pad and the page's menu
    assert.deepEqual(m.gears, gears, product);
  }
  assert.deepEqual(map('c29b', [0, 1, 1, 0, 0, 1, 0, 0, 0, H], 22).gears, [12, 13, 14, 15, 16, 17, -1]);   // no reverse shown
  const d = map('c29a', [0, 1, 1, 0, 0, 0, 0, 0, 0, H], 21);
  assert.equal(W.wheelOf('Driving Force GT (Vendor: 046d Product: c29a)').layout, 'ps');
  assert.deepEqual([press(d, 12), press(d, 13)], [bit(P.PAD_RB), bit(P.PAD_LB)]);   // its shifter pulled back, pushed
  assert.deepEqual([press(d, 4), press(d, 15), press(d, 18)], [bit(P.PAD_RB), bit(P.PAD_RB), bit(P.PAD_LB)]);
  assert.deepEqual([press(d, 14), press(d, 16), press(d, 19), press(d, 20)],
    [bit(P.PAD_A), bit(P.PAD_DDOWN), bit(P.PAD_LS), bit(P.PAD_HOME)]);   // Enter, the dial, the horn, PS
  assert.equal(d.gears, undefined);
  // the G920's shifter one button further on than the G29's (Logitech's table); a G920 showing 11 buttons has none
  assert.deepEqual(W.guessMap(W.wheelOf(G920), [0, 1, 1, 0, 0, 1, 0, 0, 0, H], 20).gears, [13, 14, 15, 16, 17, 18, 19]);
  assert.equal(W.guessMap(W.wheelOf(G920), [0, 1, 1, 0, 0, 1, 0, 0, 0, H], 11).gears, undefined);
  assert.equal(map('c26e', [0, 1, 1, 0, 0, 1, 0, 0, 0, H], 30).gears, undefined);   // the G923 for Xbox: not known
});

test('the shifter: one shift a move into a gear, reverse the handbrake', () => {
  const RB = bit(P.PAD_RB), LB = bit(P.PAD_LB), X = bit(P.PAD_X), ms = W.SHIFT_PRESS_MS;
  const run = (mode, moves, start = 0) => {
    const s = new W.Shifter();
    s.buttons(start, mode, 0);
    let t = 1000;
    return moves.map(g => { const b = s.buttons(g, mode, t); const after = s.buttons(g, mode, t + ms); t += 2 * ms; return [b, after]; });
  };
  // push up: forward (1, 3, 5) up, back (2, 4, 6) down; a press of SHIFT_PRESS_MS, then nothing while in the gear
  assert.deepEqual(run('push-up', [1, 0, 2, 0, 3, 5, 0, 7, 0]),
    [[RB, 0], [0, 0], [LB, 0], [0, 0], [RB, 0], [RB, 0], [0, 0], [X, X], [0, 0]]);
  assert.deepEqual(run('pull-up', [1, 2, 7]), [[LB, 0], [RB, 0], [X, X]]);
  assert.deepEqual(run('handbrake', [1, 0, 4, 7, 0]), [[X, X], [0, 0], [X, X], [X, X], [0, 0]]);
  assert.deepEqual(run('off', [1, 2, 7]), [[0, 0], [0, 0], [0, 0]]);
  // a lever in a gear when the wheel showed up: no shift; in reverse, no handbrake until it has been in neutral
  assert.deepEqual(run('push-up', [3, 0, 3], 3), [[0, 0], [0, 0], [RB, 0]]);
  assert.deepEqual(run('push-up', [7, 0, 7], 7), [[0, 0], [0, 0], [X, X]]);
  // the lever's position from the buttons; the first gear found wins
  assert.equal(W.shifterGear([], [12, 13, 14, 15, 16, 17, 18]), 0);
  assert.equal(W.shifterGear(Object.assign(Array(25).fill(false), { 14: true }), [12, 13, 14, 15, 16, 17, 18]), 3);
  assert.equal(W.shifterGear(Object.assign(Array(25).fill(false), { 18: true }), [12, 13, 14, 15, 16, 17, 18]), 7);
  assert.equal(W.shifterGear(Object.assign(Array(25).fill(false), { 18: true }), [12, 13, 14, 15, 16, 17, -1]), 0);
});

test('a set-up kept from before: the model\'s buttons and shifter under the player\'s wheel and pedals', () => {
  const guess = W.guessMap(W.wheelOf('Logitech G27 (Vendor: 046d Product: c29b)'), [0, 1, 1, 0, 0, 1, 0, 0, 0, H], 23);
  const old = { steer: 0, centre: 0.01, lock: -0.3, gas: { axis: 2, rest: 1, full: -1 }, brake: { axis: 5, rest: 1, full: -1 },
    buttons: [P.PAD_A, P.PAD_B, P.PAD_X, P.PAD_Y, P.PAD_LB, P.PAD_RB, -1, -1, 6, 7, 8, 9, P.PAD_DUP, P.PAD_DDOWN], learned: true };
  const m = W.withSetUp(guess, old);
  assert.deepEqual([m.steer, m.centre, m.lock, m.gas, m.brake, m.learned, m.placed], [0, 0.01, -0.3, old.gas, old.brake, true, undefined]);
  assert.deepEqual([m.buttons[4], m.buttons[12], m.gears], [P.PAD_RB, -1, [12, 13, 14, 15, 16, 17, 22]]);
});

test('the wheel, the pedals and the D-pad as the pad', () => {
  const m = W.guessMap(W.wheelOf(G29), G29_REST, 25);
  const at = (over) => { const a = G29_REST.slice(); for (const [i, v] of Object.entries(over)) a[+i] = v; return W.readWheel(a, [], m, 9); };
  assert.equal(+at({ 0: 0.15 }).axes[0].toFixed(3), 0.5);       // half lock: 67.5 degrees right
  assert.equal(at({ 0: -0.45 }).axes[0], -1);                   // past full lock left
  assert.equal(at({ 2: -1 }).axes[5], 1);                       // gas floored
  assert.equal(at({ 2: 0 }).axes[5], 0.5);                      // half
  assert.equal(at({ 5: -1 }).axes[4], 1);                       // brake floored
  assert.equal(at({ 1: -1 }).axes.slice(4).join(), '0,0');      // the clutch: nothing (the issue: it was the gas)
  const dirs = [-1, -5 / 7, -3 / 7, -1 / 7, 1 / 7, 3 / 7, 5 / 7, 1].map(v => at({ 9: v }).buttons);
  const U = bit(P.PAD_DUP), D = bit(P.PAD_DDOWN), L = bit(P.PAD_DLEFT), R = bit(P.PAD_DRIGHT);
  assert.deepEqual(dirs, [U, U | R, R, D | R, D, D | L, L, U | L]);
  assert.equal(at({ 9: 0 }).buttons, 0);                        // no report yet
  assert.equal(at({ 9: 3.2857 }).buttons, 0);                   // let go (another hat's range)
});

test('a known wheel\'s axes as each browser places them', () => {
  const U = bit(P.PAD_DUP), D = bit(P.PAD_DDOWN), L = bit(P.PAD_DLEFT);
  const pick = (m) => [m.placed, m.steer, m.gas?.axis, m.brake?.axis];
  // Linux (joydev, evdev): X, Y the clutch, Z the gas, Rz the brake, the hat switch's x and y
  const l = W.guessMap(W.wheelOf(G29), [0, 1, 1, 1, 0, 0], 25);
  assert.deepEqual([...pick(l), l.dpad], ['kernel', 0, 2, 3, [4, 5]]);
  assert.equal(W.readWheel([0, 1, 1, 1, -1, 0], [], l, -1).buttons, L);
  assert.equal(W.readWheel([0, 1, 1, 1, 0, 1], [], l, -1).buttons, D);
  assert.equal(W.readWheel([0, -1, 1, 1, 0, 0], [], l, -1).axes.slice(4).join(), '0,0');  // the clutch
  assert.equal(W.readWheel([0, 1, -1, 1, 0, 0], [], l, -1).axes.slice(4).join(), '0,1');  // the gas
  // Firefox on macOS: the descriptor's order, the hat switch first
  const f = W.guessMap(W.wheelOf('46d-c24f-G29 Driving Force Racing Wheel'), [H, 0, 1, 1, 1], 25);
  assert.deepEqual(pick(f), ['listed', 1, 2, 3]);
  assert.equal(W.readWheel([-1, 0, 1, 1, 1], [], f, W.findHat([H, 0, 1, 1, 1])).buttons, U);
  // Safari: the descriptor's order without the hat switch
  assert.deepEqual(pick(W.guessMap(W.wheelOf('46d-c24f-G29 Driving Force Racing Wheel'), [0, 1, 1, 1], 25)), ['tree', 0, 1, 2]);
  // Chrome on macOS before the wheel's first report: every axis 0, the pedals taken to rest at +1
  const z = W.guessMap(W.wheelOf(G29), Array(10).fill(0), 25);
  assert.deepEqual([...pick(z), z.gas.rest, z.brake.full], ['usage', 0, 2, 5, 1, -1]);
  // the G920: Y the gas, Z the brake, Rz the clutch
  assert.deepEqual(pick(W.guessMap(W.wheelOf(G920), [0, 1, 1, 0, 0, 1, 0, 0, 0, H], 11)), ['usage', 0, 1, 2]);
  const g = W.guessMap(W.wheelOf(G920), [0, 1, 1, 1, 0, 0], 11);
  assert.deepEqual([...pick(g), g.dpad], ['kernel', 0, 1, 2, [4, 5]]);
  // the Driving Force GT: Y the gas, Z the brake
  assert.deepEqual(pick(W.guessMap(W.wheelOf('Driving Force GT (Vendor: 046d Product: c29a)'), [0, 1, 1, 0, 0, 0, 0, 0, 0, H], 21)),
    ['usage', 0, 1, 2]);
});

test('guessed layouts: other wheels, and known ones the descriptor does not explain', () => {
  // G HUB's combined pedals: nothing rests at an end, Y in the middle, the gas pulls it up, the brake down
  const c = W.guessMap(W.wheelOf(G29), [0, 0, 0, 0, 0, 0, 0, 0, 0, H], 25);
  assert.equal(c.placed, undefined);
  assert.deepEqual([c.gas, c.brake], [{ axis: 1, rest: 0, full: -1 }, { axis: 1, rest: 0, full: 1 }]);
  assert.deepEqual(W.readWheel([0, -0.6], [], c, -1).axes.slice(4), [0, 0.6]);
  assert.deepEqual(W.readWheel([0, 0.8], [], c, -1).axes.slice(4), [0.8, 0]);
  assert.match(W.describeMap(c, 9), /, guessed$/);
  // a G920 where the browser lists its axes in the descriptor's order, which is not known here
  const n = W.guessMap(W.wheelOf(G920), [0, 1, 1, 1, H], 11);
  assert.deepEqual([n.placed, n.steer, n.gas.axis, n.brake.axis], [undefined, 0, 1, 3]);
  assert.deepEqual(n.buttons.slice(0, 8), [P.PAD_A, P.PAD_B, P.PAD_X, P.PAD_Y, P.PAD_RB, P.PAD_LB, P.PAD_START, P.PAD_BACK]);
  // a listing whose wheel axis reads as a hat switch: not the descriptor's order after all
  const x = W.guessMap(W.wheelOf('46d-c24f-G29 Driving Force Racing Wheel'), [0, H, 1, 1, 1], 25);
  assert.deepEqual([x.placed, x.steer, x.gas.axis, x.brake.axis], [undefined, 0, 2, 4]);
  // another wheel: pedals resting at -1; one with its hat switch first
  const o = W.guessMap(W.wheelOf('Some USB Wheel'), [0, -1, -1], 12);
  assert.deepEqual([o.steer, o.gas, o.brake], [0, { axis: 1, rest: -1, full: 1 }, { axis: 2, rest: -1, full: 1 }]);
  const h = W.guessMap(W.wheelOf('Some USB Wheel'), [H, 0.01, 1, 1], 12);
  assert.deepEqual([h.steer, h.gas.axis, h.brake.axis], [1, 2, 3]);
  // a wheel of unknown rotation: all of it is full lock
  assert.equal(W.guessMap(W.wheelOf('Some USB Wheel'), [0, 1, 1], 12).lock, 1);
});

test('the set-up: let go, turn right to full lock, the gas, the brake', () => {
  const base = W.guessMap(W.wheelOf(G29), G29_REST, 25);
  const L = new W.WheelLearn(base);
  let t = 0;
  const hold = (axes, ms = 800) => { for (let e = 0; e <= ms; e += 16) L.feed(axes, t += 16); };
  const a = (over) => { const x = [-0.01, 1, 1, 0, 0, 1, 0, 0, 0, H]; for (const [i, v] of Object.entries(over)) x[+i] = v; return x; };
  hold(a({}));
  assert.equal(L.step, 'steer');
  hold(a({ 9: -1 }));                                           // the D-pad is no wheel
  assert.equal(L.step, 'steer');
  for (let v = 0; v <= 0.2; v += 0.02) L.feed(a({ 0: v }), t += 16);   // turning: not held yet
  assert.equal(L.step, 'steer');
  hold(a({ 0: 0.19 }));
  assert.equal(L.step, 'gas');
  hold(a({ 0: 0.19, 1: -1 }));                                  // the clutch, pressed first, is taken as the gas
  assert.equal(L.step, 'brake');
  const L2 = new W.WheelLearn(base);
  t = 0;
  const hold2 = (axes) => { for (let e = 0; e <= 800; e += 16) L2.feed(axes, t += 16); };
  hold2(a({})); hold2(a({ 0: -0.21 })); hold2(a({ 2: -1 })); hold2(a({ 5: -0.98 }));
  assert.equal(L2.step, 'done');
  const m = L2.map();
  assert.equal(m.steer, 0);
  assert.equal(+m.centre.toFixed(2), -0.01);
  assert.equal(+m.lock.toFixed(2), -0.2);                       // this wheel's axis runs the other way
  assert.deepEqual(m.gas, { axis: 2, rest: 1, full: -1 });
  assert.deepEqual(m.brake, { axis: 5, rest: 1, full: -0.98 });
  assert.equal(m.learned, true);
  assert.equal(m.placed, undefined);
  assert.deepEqual(m.buttons, base.buttons);
  assert.equal(W.readWheel(a({ 0: -0.21 }), [], m, 9).axes[0], 1);   // right, as turned in the set-up
});

test('the set-up with combined pedals: the brake is the gas\'s axis the other way', () => {
  const L = new W.WheelLearn(W.guessMap(W.wheelOf(G29), [0, 0], 25));
  let t = 0;
  const hold = (axes) => { for (let e = 0; e <= 800; e += 16) L.feed(axes, t += 16); };
  hold([0, 0]); hold([0.3, 0]); hold([0, -1]);
  assert.equal(L.step, 'brake');
  hold([0, -1]);                                                // still the gas: no brake
  assert.equal(L.step, 'brake');
  hold([0, 0.9]);
  assert.deepEqual([L.map().gas, L.map().brake], [{ axis: 1, rest: 0, full: -1 }, { axis: 1, rest: 0, full: 0.9 }]);
});

test('controller families from Chrome\'s and Firefox\'s ids, a wheel\'s from its buttons', () => {
  assert.deepEqual(G.familyOf('54c-ce6-DualSense Wireless Controller'), { family: P.PADF_PLAYSTATION, name: 'DualSense Wireless Controller' });
  assert.deepEqual(G.familyOf('054c-0ce6-DualSense Wireless Controller'), { family: P.PADF_PLAYSTATION, name: 'DualSense Wireless Controller' });
  assert.deepEqual(G.familyOf(G29), { family: P.PADF_PLAYSTATION, name: 'G29 Driving Force Racing Wheel' });
  assert.equal(G.familyOf('46d-c262-G920 Driving Force Racing Wheel').family, P.PADF_XBOX);
  assert.equal(G.familyOf('Xbox 360 Controller (XInput STANDARD GAMEPAD)').family, P.PADF_XBOX);
});

test('a controller without the standard mapping: an axis that rested at an end is no stick', () => {
  const g = { id: 'Generic USB Joystick (Vendor: 0079 Product: 0006)', index: 0, mapping: '', axes: [0, 1, 0, -1],
    buttons: Array.from({ length: 12 }, () => ({ pressed: false, value: 0 })) };
  const s = G.mapStandard(g, new Set([1, 3]));
  assert.deepEqual(s.axes, [0, 0, 0, 0, 0, 0]);
  assert.equal(s.buttons, 0);
  assert.deepEqual(G.mapStandard(g).axes.slice(0, 4), [0, 1, 0, -1]);    // as before, without
});

test('a wheel and a controller: the one a button was last pressed on drives the pad', () => {
  const nav = Object.getOwnPropertyDescriptor(globalThis, 'navigator'), et = new EventTarget(), pads = [];
  Object.defineProperty(globalThis, 'navigator', { value: { getGamepads: () => pads }, configurable: true });
  const ev = { addEventListener: globalThis.addEventListener, removeEventListener: globalThis.removeEventListener, dispatchEvent: globalThis.dispatchEvent };
  Object.assign(globalThis, { addEventListener: et.addEventListener.bind(et), removeEventListener: et.removeEventListener.bind(et),
    dispatchEvent: et.dispatchEvent.bind(et) });
  try {
    const buf = new SharedArrayBuffer(4096), h = new Int32Array(buf), u8 = new Uint8Array(buf), logs = [];
    const pad = (id, index, mapping, axes, n) => ({ id, index, mapping, connected: true, axes: [...axes],
      buttons: Array.from({ length: n }, () => ({ pressed: false, value: 0 })) });
    const press = (g, i, on = true) => { g.buttons[i] = { pressed: on, value: on ? 1 : 0 }; };
    const name = () => String.fromCharCode(...u8.subarray(P.IM_OFF_PAD_NAME, P.IM_OFF_PAD_NAME + 64)).replace(/\0+$/, '');
    const wheel = pad(G29, 0, '', G29_REST, 25);
    const ds = pad('DualSense Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 0ce6)', 1, 'standard', [0, 0, 0, 0], 17);
    let changes = 0;
    et.addEventListener(G.PAD_DRIVER_EVENT, () => changes++);
    pads.push(wheel, ds);
    let p = new G.PadPoller(h, u8, 0, () => {}, s => logs.push(s));
    p.poll();
    assert.equal(h[P.IM_PAD_WHEEL], 1);                             // the first connected, before any press
    assert.equal(G.drivingPad(), wheel);
    press(ds, 0); p.poll();                                         // Cross on the controller: it takes over, Cross too
    assert.deepEqual([h[P.IM_PAD_WHEEL], name(), h[P.IM_PAD_BUTTONS]], [0, 'DualSense Wireless Controller', bit(P.PAD_A)]);
    assert.equal(G.drivingPad(), ds);
    assert.match(logs.at(-1), /DualSense.* drives now \(a button pressed on it\)/);
    press(ds, 0, false); wheel.axes[0] = -1; p.poll();              // the wheel turned: no press, the controller stays
    assert.equal(h[P.IM_PAD_WHEEL], 0);
    press(wheel, 3); p.poll();                                      // a button on the wheel takes it back
    assert.equal(h[P.IM_PAD_WHEEL], 1);
    press(wheel, 3, false); press(ds, 7); p.poll();                 // the controller's trigger is a press too
    assert.equal(h[P.IM_PAD_WHEEL], 0);
    pads[1] = null; p.poll();                                       // the controller off: the wheel again
    assert.equal(h[P.IM_PAD_WHEEL], 1);
    pads[1] = ds; p.poll();                                         // back, its trigger still held (Chrome shows it on a press)
    assert.equal(h[P.IM_PAD_WHEEL], 0);
    assert.equal(changes, 6);
    p.dispose();
    assert.equal(G.drivingPad(), null);
    p = new G.PadPoller(h, u8, 0, () => {}, () => {});              // a button held at the first poll is no press
    p.poll();
    assert.equal(h[P.IM_PAD_WHEEL], 1);
    press(ds, 7, false); p.poll(); press(ds, 1); p.poll();
    assert.equal(h[P.IM_PAD_WHEEL], 0);
    p.dispose();
  } finally {
    if (nav) Object.defineProperty(globalThis, 'navigator', nav); else delete globalThis.navigator;
    Object.assign(globalThis, ev);
  }
});

/** fn with navigator.getGamepads() giving pads and the window's events on a target of its own */
async function withPads(fn) {
  const nav = Object.getOwnPropertyDescriptor(globalThis, 'navigator'), et = new EventTarget(), pads = [];
  Object.defineProperty(globalThis, 'navigator', { value: { getGamepads: () => pads }, configurable: true });
  const ev = { addEventListener: globalThis.addEventListener, removeEventListener: globalThis.removeEventListener, dispatchEvent: globalThis.dispatchEvent };
  Object.assign(globalThis, { addEventListener: et.addEventListener.bind(et), removeEventListener: et.removeEventListener.bind(et),
    dispatchEvent: et.dispatchEvent.bind(et) });
  try { await fn(pads); } finally {
    if (nav) Object.defineProperty(globalThis, 'navigator', nav); else delete globalThis.navigator;
    Object.assign(globalThis, ev);
  }
}
const fakePad = (id, index, mapping, axes, n) => ({ id, index, mapping, connected: true, axes: [...axes],
  buttons: Array.from({ length: n }, () => ({ pressed: false, value: 0 })) });
const pressOn = (g, i, on = true) => { g.buttons[i] = { pressed: on, value: on ? 1 : 0 }; };

test('a wheel taking the pad back with the gas down keeps its pedals (a report: the gas died until a reload)', async () => {
  const G2 = await load('web/src/input/gamepad.ts', '\n// a page of its own');
  await withPads(async (pads) => {
    const buf = new SharedArrayBuffer(4096), h = new Int32Array(buf), f = new Float32Array(buf), logs = [];
    const wheel = fakePad(G29, 0, '', G29_REST, 25);
    const ds = fakePad('DualSense Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 0ce6)', 1, 'standard', [0, 0, 0, 0], 17);
    pads.push(wheel, ds);
    const p = new G2.PadPoller(h, new Uint8Array(buf), 0, () => {}, s => logs.push(s));
    const gas = () => +f[P.IM_PAD_AXIS + P.PAD_AXIS_RT].toFixed(2);
    p.poll();
    wheel.axes[2] = 0; p.poll();                                    // the gas half down
    assert.deepEqual([h[P.IM_PAD_WHEEL], gas()], [1, 0.5]);
    wheel.axes[2] = 1; pressOn(ds, 0); p.poll(); pressOn(ds, 0, false); p.poll();   // the controller takes over
    assert.equal(h[P.IM_PAD_WHEEL], 0);
    wheel.axes[2] = -1; pressOn(wheel, 4); p.poll();                // on the gas, the right paddle: the wheel's again
    assert.deepEqual([h[P.IM_PAD_WHEEL], gas()], [1, 1]);
    wheel.axes[1] = -1; wheel.axes[2] = 1; p.poll();                // the clutch is still no gas
    assert.equal(gas(), 0);
    assert.match(logs.filter(s => /a steering wheel/.test(s)).at(-1), /gas axis 2 .* the model's own/);
    p.dispose();
  });
});

test('the shifter through the poller: a move into third, the right paddle for a moment; the pick per model', async () => {
  const G2 = await load('web/src/input/gamepad.ts', '\n// another page');
  const W2 = await load('web/src/input/wheel.ts', '\n// another page');
  await withPads(async (pads) => {
    const buf = new SharedArrayBuffer(4096), h = new Int32Array(buf), logs = [];
    const wheel = fakePad(G29, 0, '', G29_REST, 25);
    pads.push(wheel);
    const p = new G2.PadPoller(h, new Uint8Array(buf), 0, () => {}, s => logs.push(s));
    p.poll();
    assert.match(logs.at(-1), /shifter buttons 12 .* 18, .*; the shifter: push-up$/);
    pressOn(wheel, 14); p.poll();                                   // third gear
    assert.equal(h[P.IM_PAD_BUTTONS], bit(P.PAD_RB));
    await new Promise(r => setTimeout(r, W.SHIFT_PRESS_MS + 20));
    p.poll();
    assert.equal(h[P.IM_PAD_BUTTONS], 0);                           // the lever left in third holds nothing
    pressOn(wheel, 14, false); pressOn(wheel, 18); p.poll();        // reverse: the handbrake
    assert.equal(h[P.IM_PAD_BUTTONS], bit(P.PAD_X));
    W2.saveShifter(G29, 'off');                                     // this page's wheel.ts is G2's own: no event to it
    assert.equal(W2.loadShifter(G29), 'off');
    assert.equal(W2.loadShifter('046d-c24f-G29 Driving Force Racing Wheel'), 'off');   // the same model in Firefox
    assert.equal(W2.loadShifter(G920), 'push-up');
    p.dispose();
  });
});

test('a controller first seen with its left stick pushed (Chrome shows one then) steers: the left stick is always one', async () => {
  const G2 = await load('web/src/input/gamepad.ts', '\n// a page for the left stick');
  await withPads(async (pads) => {
    const buf = new SharedArrayBuffer(4096), h = new Int32Array(buf), f = new Float32Array(buf), logs = [];
    const lx = () => f[P.IM_PAD_AXIS + P.PAD_AXIS_LX], rx = () => f[P.IM_PAD_AXIS + P.PAD_AXIS_RX];
    // a USB pad by usage: the left stick X Y pushed right, a trigger on Rx (3) let go at -1, the hat switch on 9
    const pad = fakePad('USB Gamepad (Vendor: 0079 Product: 0011)', 0, '', [1, 0, 0, -1, 0, 0, 0, 0, 0, H], 12);
    pads.push(pad);
    const p = new G2.PadPoller(h, new Uint8Array(buf), 0, () => {}, s => logs.push(s));
    p.poll();
    assert.equal(lx(), 1);
    pad.axes[0] = -0.5; p.poll();
    assert.equal(lx(), -0.5);
    pad.axes[2] = 0.25; p.poll();
    assert.deepEqual([rx(), f[P.IM_PAD_AXIS + P.PAD_AXIS_RY]], [0.25, 0]);   // the trigger at an end: still no stick
    assert.match(logs.at(-1), /read as if it had one, but for axes 3 \(at an end: no stick\), the D-pad on axis 9$/);
    p.dispose();
  });
  // Chrome on Android, a controller it has no mapping for (a player's report: the left stick did not steer)
  await withPads(async (pads) => {
    navigator.userAgent = 'Mozilla/5.0 (Linux; Android 15; 24117RK2CG) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Mobile Safari/537.36';
    const buf = new SharedArrayBuffer(4096), h = new Int32Array(buf), f = new Float32Array(buf), logs = [];
    const std = fakePad('Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 0b13)', 0, 'standard', [0, 0, 0, 0], 17);
    const pad = fakePad('Gamepad (Vendor: 3537 Product: 1053)', 1, '', [0, 0, 0, 0], 16);
    pads.push(std, pad);
    const p = new G2.PadPoller(h, new Uint8Array(buf), 0, () => {}, s => logs.push(s));
    p.poll();
    assert.equal(G2.drivingPad(), std);
    pad.axes[0] = -1; pad.axes[3] = 1; pressOn(pad, 2); p.poll();   // X on it, the sticks pushed: it drives, as it comes
    assert.equal(G2.drivingPad(), pad);
    assert.deepEqual([h[P.IM_PAD_BUTTONS], f[P.IM_PAD_AXIS + P.PAD_AXIS_LX], f[P.IM_PAD_AXIS + P.PAD_AXIS_RY]], [bit(P.PAD_X), -1, 1]);
    pressOn(pad, 2, false); pad.axes[0] = 0.5; pad.axes[3] = 0; pressOn(pad, 12); p.poll();
    assert.deepEqual([h[P.IM_PAD_BUTTONS], f[P.IM_PAD_AXIS + P.PAD_AXIS_LX]], [bit(P.PAD_DUP), 0.5]);
    assert.ok(logs.some(s => /read as it is: Android put it/.test(s)));
    p.dispose();
  });
});

// An RC radio (EdgeTX's USB joystick) under Chrome on Windows: CH1 to CH8 on axes 0 to 7 (AETR: the sticks centred,
// the throttle down; CH5 to CH8 switches), channels past the eighth as buttons
const T8L = 'EdgeTX RadioMaster T8L Joystick (Vendor: 1209 Product: 4f54)';
const RADIO_REST = [0, 0, -1, 0, -1, -1, 0, 1];
const radioAt = (over) => { const a = RADIO_REST.slice(); for (const [i, v] of Object.entries(over)) a[+i] = v; return a; };

test('RC radios by EdgeTX\'s ids or their firmware\'s and makers\' names, and nothing else', () => {
  for (const id of [T8L, '1209-4f54-EdgeTX RadioMaster T8L Joystick', 'FrSky Taranis Joystick (Vendor: 0483 Product: 5710)',
    'OpenTX Jumper T16 Joystick (Vendor: 0483 Product: 5710)', 'RadioMaster Pocket Joystick'])
    assert.equal(W.wheelOf(id)?.layout, 'radio', id);
  // STMicro's demo ids without a radio's name, another pid.codes device, wheels and pads: as before
  assert.equal(W.wheelOf('STM32 Joystick (Vendor: 0483 Product: 5710)'), null);
  assert.equal(W.wheelOf('Some Game Controller (Vendor: 1209 Product: 4f55)'), null);
  assert.equal(W.wheelOf('Wheel Base (Vendor: 1209 Product: ffb0)').layout, 'generic');
  assert.equal(W.wheelOf(G29).layout, 'ps');
  assert.equal(W.wheelOf('Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 0b13)'), null);
  assert.deepEqual(G.familyOf(T8L), { family: P.PADF_GENERIC, name: 'EdgeTX RadioMaster T8L Joystick' });
});

test('a radio\'s layout: the aileron steers, the throttle is the gas, the elevator pulled back the brake', () => {
  const m = W.guessMap(W.wheelOf(T8L), RADIO_REST, 24);
  assert.deepEqual([m.steer, m.centre, m.lock, m.gas, m.brake], [0, 0, 1, { axis: 2, rest: -1, full: 1 }, { axis: 1, rest: 0, full: -1 }]);
  assert.ok(m.buttons.every(b => b === -1));
  assert.deepEqual([m.switches, m.gears, m.placed], [[], undefined, undefined]);
  const at = (over) => { const r = W.readWheel(radioAt(over), [], m, -1); return { ...r, axes: r.axes.map(v => v || 0) }; };
  assert.deepEqual(at({}).axes, [0, 0, 0, 0, 0, 0]);                   // at rest: nothing, whatever the switches
  assert.equal(at({}).buttons, 0);
  assert.deepEqual(at({ 0: 0.5 }).axes, [0.5, 0, 0, 0, 0, 0]);        // half right
  assert.deepEqual(at({ 2: 0 }).axes, [0, 0, 0, 0, 0, 0.5]);          // half throttle
  assert.deepEqual(at({ 2: 1, 1: -1 }).axes, [0, 0, 0, 0, 1, 1]);     // full throttle, the right stick pulled back
  assert.deepEqual(at({ 1: 1, 3: -1 }).axes, [0, 0, 0, 0, 0, 0]);     // pushed forward, the rudder: nothing
  assert.match(W.describeMap(m, -1), /^steering axis 0 .* gas axis 2 \(-1\.00 to 1\.00\), brake axis 1 \(0\.00 to -1\.00\), switches none, guessed$/);
  // TAER: the throttle first; a radio sending a stick's up as -1 (its throttle rests at +1): pulled back is +1
  const taer = W.guessMap(W.wheelOf(T8L), [-1, 0, 0, 0, 1, 1, 1, 1], 24);
  assert.deepEqual([taer.steer, taer.gas.axis, taer.brake.axis], [1, 0, 2]);
  const up = W.guessMap(W.wheelOf(T8L), [0, 0, 1, 0, -1, -1, -1, -1], 24);
  assert.deepEqual([up.gas, up.brake], [{ axis: 2, rest: 1, full: -1 }, { axis: 1, rest: 0, full: 1 }]);
  // the throttle not at an end, or not read yet: AETR, a stick's up +1
  const pick = (m) => [m.steer, m.gas.axis, m.gas.rest, m.brake.axis, m.brake.full];
  for (const axes of [[0, 0, 0, 0, -1, -1, 0, 0], Array(8).fill(0)])
    assert.deepEqual(pick(W.guessMap(W.wheelOf(T8L), axes, 24)), [0, 2, -1, 1, -1]);
});

test('a radio\'s set-up: the sticks, then a switch for nitrous, the handbrake and a respawn, or skipped', () => {
  const base = W.guessMap(W.wheelOf(T8L), RADIO_REST, 24);
  const L = new W.WheelLearn(base, true);
  let t = 0, pressed = [];
  const hold = (over, l = L) => { const a = radioAt(over); for (let e = 0; e <= 800; e += 16) l.feed(a, t += 16, pressed); };
  hold({});
  hold({ 0: 0.98 });                                                    // the right stick right
  hold({ 2: 1 });                                                       // the throttle up
  assert.equal(L.step, 'brake');
  hold({ 2: 1, 1: -1 });                                                // the elevator back, the throttle still up
  assert.equal(L.step, 'nitrous');
  hold({});                                                             // nothing flipped
  assert.deepEqual([L.step, L.progress], ['nitrous', 0]);
  hold({ 3: 0.4 });                                                     // the rudder a little: no switch
  assert.equal(L.step, 'nitrous');
  hold({ 7: -1 });                                                      // CH8's switch from up to down
  assert.equal(L.step, 'handbrake');
  hold({ 7: -1 });                                                      // that switch still there: taken
  assert.equal(L.step, 'handbrake');
  hold({ 7: 0 });                                                       // the same switch's middle: free
  assert.equal(L.step, 'respawn');
  pressed = Object.assign(Array(24).fill(false), { 1: true });          // CH10, a button
  hold({ 7: 0 });
  assert.equal(L.step, 'done');
  const m = L.map();
  assert.deepEqual(m.switches, [{ pad: P.PAD_A, axis: 7, on: -1 }, { pad: P.PAD_X, axis: 7, on: 0 }, { pad: P.PAD_BACK, button: 1 }]);
  assert.deepEqual([m.steer, +m.lock.toFixed(2), m.gas, m.brake], [0, 0.98, { axis: 2, rest: -1, full: 1 }, { axis: 1, rest: 0, full: -1 }]);
  const read = (over, b = []) => W.readWheel(radioAt(over), b, m, -1).buttons;
  assert.equal(read({}), 0);
  assert.equal(read({ 7: -0.98 }), bit(P.PAD_A));
  assert.equal(read({ 7: 0.02 }), bit(P.PAD_X));
  assert.equal(read({}, Object.assign(Array(24).fill(false), { 1: true })), bit(P.PAD_BACK));
  assert.match(W.describeMap(m, -1), /switches nitrous axis 7 at -1\.00, handbrake axis 7 at 0\.00, respawn button 1, set up by the player$/);
  // skipped: none
  const S = new W.WheelLearn(base, true);
  t = 0; pressed = [];
  for (const over of [{}, { 0: 1 }, { 2: 1 }, { 1: -1 }]) hold(over, S);
  assert.equal(S.step, 'nitrous');
  assert.deepEqual([S.skip(), S.skip(), S.skip(), S.step, S.skip(), S.map().switches], [true, true, true, 'done', false, []]);
  // a wheel's set-up has no switch steps, and a wheel's map no switches, kept or not
  const wb = W.guessMap(W.wheelOf(G29), G29_REST, 25), wl = new W.WheelLearn(wb);
  assert.equal(wl.skip(), false);
  assert.equal('switches' in wb, false);
  assert.deepEqual(W.withSetUp(base, m).switches, m.switches);
  assert.equal('switches' in W.withSetUp(wb, m), false);
});

test('a radio drives as a wheel, and a stick or a switch moved half way takes it back from a controller', async () => {
  const G2 = await load('web/src/input/gamepad.ts', '\n// a page for a radio');
  await withPads(async (pads) => {
    const buf = new SharedArrayBuffer(4096), h = new Int32Array(buf), f = new Float32Array(buf), logs = [];
    const radio = fakePad(T8L, 0, '', RADIO_REST, 24);
    pads.push(radio);
    const p = new G2.PadPoller(h, new Uint8Array(buf), 0, () => {}, s => logs.push(s));
    const ax = (a) => +f[P.IM_PAD_AXIS + a].toFixed(2);
    p.poll();
    assert.deepEqual([h[P.IM_PAD_WHEEL], ax(P.PAD_AXIS_LX), ax(P.PAD_AXIS_RT), ax(P.PAD_AXIS_LT), h[P.IM_PAD_BUTTONS]], [1, 0, 0, 0, 0]);
    assert.match(logs.at(-1), /T8L Joystick .*: an RC radio, steering axis 0 .* switches none, guessed$/);
    radio.axes[0] = -0.5; radio.axes[2] = 1; p.poll();
    assert.deepEqual([ax(P.PAD_AXIS_LX), ax(P.PAD_AXIS_RT)], [-0.5, 1]);
    const ds = fakePad('DualSense Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 0ce6)', 1, 'standard', [0, 0, 0, 0], 17);
    pads.push(ds); p.poll();
    pressOn(ds, 0); p.poll(); pressOn(ds, 0, false); p.poll();     // Cross: the controller drives
    assert.equal(G2.drivingPad(), ds);
    radio.axes[0] = -0.1; radio.axes[2] = 0.6; p.poll();           // less than half way from where it was: no
    assert.equal(G2.drivingPad(), ds);
    radio.axes[2] = -1; p.poll();                                   // the throttle pulled down: the radio again
    assert.deepEqual([G2.drivingPad(), h[P.IM_PAD_WHEEL], ax(P.PAD_AXIS_LX)], [radio, 1, -0.1]);
    radio.axes[0] = 0.4; p.poll(); radio.axes[0] = 0.8; p.poll();  // driving: the stick moves on as it likes
    assert.equal(G2.drivingPad(), radio);
    pressOn(ds, 1); p.poll();
    assert.equal(G2.drivingPad(), ds);
    radio.axes[7] = -1; p.poll();                                   // a switch flipped
    assert.equal(G2.drivingPad(), radio);
    p.dispose();
  });
});
