// The daily treasure hunt's page side (web/src/app/treasure.ts; runtime/treasure.c packs what the game says):
//   node --test web/test/treasure.test.mjs
// - th_state, th_aim and th_screen unpack as treasure.c packs them (its shifts are the ones in the C)
// - a hunt's time reads m:ss, h:mm:ss from an hour
// - the totals keep the quickest hunt, once a day
// The modules are bundled from web/src with esbuild, as web/test/achievements.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const load = async f => {
  const js = (await esbuild.build({ stdin: { contents: `export * from './${f}';`, resolveDir: ROOT, loader: 'ts' },
    bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
  return import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
};
const T = await load('web/src/app/treasure.ts'), A = await load('web/src/app/achievements.ts');

test('th_state, th_aim and th_screen unpack as treasure.c packs them', () => {
  const c = fs.readFileSync(path.join(ROOT, 'runtime/treasure.c'), 'utf8');
  for (const s of ['T.found << 16', '(on ? 1 : 0) << 31', '0xffffu) << 32', '(locked & 0xfu) << 48', '(uint64_t)deg << 16',
    '(uint64_t)best << 25', '0xfffu) << 29', '(T.gold + 1) << 42', '1ull << 41', '2048.f) << 15', '1ull << 30',
    '"th_state"', '"th_aim"', '"th_screen"']) assert.ok(c.includes(s), s);
  const state = 20762 + 0b101 * 2 ** 16 + 1 * 2 ** 31 + 80 * 2 ** 32 + 3 * 2 ** 48;
  const aim = 1234 + 270 * 2 ** 16 + 7 * 2 ** 25 + 160 * 2 ** 29 + 2 ** 41 + (4 + 1) * 2 ** 42;
  assert.deepEqual(T.unpackHunt(state, aim), { day: 20762, found: 0b101, on: true, paid: 800, locked: 3, meters: 1234, bearing: 270,
    which: 7, prize: 1600, aimed: true, gold: 4 });
  assert.equal(T.unpackHunt(0, 0).gold, -1, 'no golden one');
  const at = (x, y) => Math.round((x + 8) * 2048) + Math.round((y + 8) * 2048) * 2 ** 15 + 2 ** 30;
  assert.deepEqual(T.unpackScreen(at(-1.5, 0.25)), { x: -1.5, y: 0.25 });
  assert.equal(T.unpackScreen(0), null, 'behind the camera');
});

test('a hunt\'s time', () => {
  assert.equal(A.huntClock(0), '0:00');
  assert.equal(A.huntClock(61_400), '1:01');
  assert.equal(A.huntClock(3_725_000), '1:02:05');
});

test('the totals keep the quickest hunt, once a day', () => {
  let t = A.addHunt(A.emptyTotals(), 100, 600_000);
  t = A.addHunt(t, 101, 480_000);
  t = A.addHunt(t, 101, 100_000);
  assert.equal(t.bestHuntMs, 480_000, 'a day counts once');
  t = A.addHunt(t, 102, 0);
  assert.equal(t.bestHuntMs, 480_000, 'a hunt not timed keeps the best');
  assert.equal(t.hunts, 3); assert.equal(t.huntStreak, 3);
});
