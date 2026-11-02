// Photo mode's panel helpers (web/src/app/photolook.ts): the focus slider's scale both ways, every crop's kept part
// (the middle of the picture, its own width over height, inside the picture), every filter a full look.
//   node --test web/test/photolook.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const js = (await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/app/photolook.ts')], bundle: true, format: 'esm', write: false })).outputFiles[0].text;
const { CROPS, FILTERS, cropRect, focusM, focusV, FOCUS_MIN, FOCUS_MAX } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

test('the focus slider: its ends, and back and forth', () => {
  assert.equal(focusM(0), FOCUS_MIN);
  assert.ok(Math.abs(focusM(1000) - FOCUS_MAX) < 1e-9);
  for (const m of [0.5, 1, 3.7, 12, 80, 500]) assert.ok(Math.abs(focusM(focusV(m)) / m - 1) < 0.01, String(m));
  assert.equal(focusV(0.01), 0);
  assert.equal(focusV(1e6), 1000);
});

test('each crop keeps the middle of the picture, at its own width over height', () => {
  for (const aspect of [16 / 9, 4 / 3, 21 / 9, 9 / 16, 1.5]) {
    for (const [name, r] of Object.entries(CROPS)) {
      const [l, t, rt, b] = cropRect(r, aspect);
      assert.ok(l >= 0 && t >= 0 && rt <= 1 && b <= 1 && l < rt && t < b, `${name} at ${aspect}`);
      assert.ok(Math.abs((l + rt) / 2 - 0.5) < 1e-9 && Math.abs((t + b) / 2 - 0.5) < 1e-9, `${name} in the middle`);
      if (!r) { assert.deepEqual([l, t, rt, b], [0, 0, 1, 1]); continue; }
      assert.ok(Math.abs((rt - l) * aspect / (b - t) - r) < 1e-9, `${name} at ${aspect}: ${(rt - l) * aspect / (b - t)}`);
      assert.ok(rt - l === 1 || b - t === 1, `${name} as large as it fits`);
    }
  }
});

test('a crop like the picture, or a picture of no size, keeps all of it', () => {
  assert.deepEqual(cropRect(16 / 9, 16 / 9), [0, 0, 1, 1]);
  assert.deepEqual(cropRect(1, 0), [0, 0, 1, 1]);
  assert.deepEqual(cropRect(1, NaN), [0, 0, 1, 1]);
});

test('every filter is a whole look; none leaves the picture as it is', () => {
  for (const [k, f] of Object.entries(FILTERS)) {
    assert.equal(typeof f.contrast, 'number', k); assert.equal(typeof f.saturation, 'number', k);
    assert.equal(f.tint.length, 3, k); assert.ok(f.fade >= 0 && f.fade < 0.5, k);
  }
  assert.deepEqual(FILTERS.none, { contrast: 1, saturation: 1, tint: [1, 1, 1], fade: 0 });
  assert.equal(FILTERS.bw.saturation, 0);
});
