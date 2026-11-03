import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as esbuild from 'esbuild';
const load = async path => import('data:text/javascript;base64,' + Buffer.from((await esbuild.build({
  entryPoints: [new URL(path, import.meta.url).pathname.replace(/^\/([A-Z]:)/, '$1')],
  bundle: true, format: 'esm', write: false,
})).outputFiles[0].text).toString('base64'));
const { LocalSeats, LocalPadWriter } = await load('../src/input/local-pads.ts');
const P = await load('../src/input/proto.ts');
const { mapStandard } = await load('../src/input/gamepad.ts');
const pad = (index, id = `Pad ${index}`) => ({ index, id, connected: true, mapping: 'standard',
  axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })) });
const start = (g, on) => { g.buttons[9] = { pressed: on, value: +on }; };

test('six seats require START, suppress the join press and never steal one another', () => {
  const seats = new LocalSeats(), pads = Array.from({ length: 7 }, (_, i) => pad(i));
  assert.deepEqual(seats.poll(pads), Array(6).fill(null));
  pads.forEach(g => start(g, true));
  assert.deepEqual(seats.poll(pads), Array(6).fill(null));
  pads.forEach(g => start(g, false));
  assert.deepEqual(seats.poll(pads).map(g => g.index), [0, 1, 2, 3, 4, 5]);
  start(pads[3], true); start(pads[6], true);
  assert.deepEqual(seats.poll(pads).map(g => g.index), [0, 1, 2, 3, 4, 5]);
});

test('disconnected seats are neutral; replacement requires a fresh START press', () => {
  const seats = new LocalSeats(), a = pad(2), b = pad(7), replacement = pad(9);
  start(a, true); start(b, true); seats.poll([a, b]);
  start(a, false); start(b, false); seats.poll([a, b]);
  a.connected = false;
  assert.deepEqual(seats.poll([a, b, replacement]).map(g => g?.index ?? null), [null, 7, null, null, null, null]);
  start(replacement, true); seats.poll([b, replacement]);
  start(replacement, false);
  assert.deepEqual(seats.poll([b, replacement]).map(g => g?.index ?? null), [9, 7, null, null, null, null]);
});

test('START join counter survives the release gate for the native join popup', () => {
  const seats = new LocalSeats(), a = pad(1), b = pad(4);
  seats.poll([a, b]);
  start(a, true);
  assert.equal(seats.poll([a, b])[0], null);
  assert.deepEqual(seats.joins, [1, 0, 0, 0, 0, 0]);
  seats.poll([a, b]);
  assert.equal(seats.joins[0], 1, 'held START is not a second join');
  start(a, false); seats.poll([a, b]);
  start(b, true); seats.poll([a, b]);
  start(b, false); seats.poll([a, b]);
  assert.deepEqual(seats.joins, [1, 1, 0, 0, 0, 0]);
  start(a, true); seats.poll([a, b]);
  assert.equal(seats.joins[0], 2, 'an already assigned pad can rejoin a new round');
});

test('all six states occupy independent records; disconnect and page dialogs clear held input', () => {
  const base = 64, buffer = new SharedArrayBuffer(base + P.IM_BYTES + 64);
  const writer = new LocalPadWriter(buffer, base), words = new Int32Array(buffer);
  words.fill(0x12345678);
  const pads = Array.from({ length: 6 }, (_, i) => pad(i));
  pads.forEach(g => start(g, true)); writer.poll(pads, mapStandard);
  pads.forEach((g, i) => { start(g, false); g.axes[0] = i / 10; }); writer.poll(pads, mapStandard);
  pads.forEach((g, i) => { g.buttons[i] = { pressed: true, value: 1 }; }); writer.poll(pads, mapStandard);
  const offset = (seat, field) => (base + P.IM_OFF_LOCAL_PADS) / 4 + seat * P.IM_LOCAL_WORDS + field;
  for (let seat = 0; seat < 6; seat++) {
    assert.equal(words[offset(seat, P.IM_LOCAL_CONNECTED)], 1);
    assert.equal(words[offset(seat, P.IM_LOCAL_BUTTONS)], 1 << seat);
    assert.equal(words[offset(seat, P.IM_LOCAL_SEQ)] & 1, 0);
  }
  assert.equal(words[base / 4], 0x12345678, 'legacy mailbox untouched');
  assert.equal(words[(base + P.IM_BYTES) / 4], 0x12345678, 'no buffer overrun');
  pads[4].connected = false; writer.poll(pads, mapStandard);
  assert.equal(words[offset(4, P.IM_LOCAL_BUTTONS)], 0);
  assert.equal(words[offset(4, P.IM_LOCAL_CONNECTED)], 0);
  writer.poll(pads, mapStandard, true);
  for (let seat = 0; seat < 6; seat++) assert.equal(words[offset(seat, P.IM_LOCAL_BUTTONS)], 0);
});
