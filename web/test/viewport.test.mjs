import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
const code = (await esbuild.build({ entryPoints: [fileURLToPath(new URL('../src/gpu/viewport.ts', import.meta.url))],
  bundle: true, format: 'esm', write: false })).outputFiles[0].text;
const { drawViewport, drawScissor } = await import('data:text/javascript;base64,' + Buffer.from(code).toString('base64'));
test('six viewport regions retain their offsets and depth ranges', () => {
  for (let i = 0; i < 6; i++) {
    const v = [i % 3 * 640, Math.floor(i / 3) * 540, 640, 540, 0.1, 0.9];
    assert.deepEqual(drawViewport(v, 0, 1920, 1080), v);
  }
});
test('reset viewport fills target; invalid values cannot poison the GPU frame', () => {
  assert.deepEqual(drawViewport([0, 0, 0, 0, 0, 0], 0, 320, 240), [0, 0, 320, 240, 0, 1]);
  for (const v of [[-1, 0, 640, 540, 0, 1], [1281, 540, 640, 540, 0, 1],
    [0, 0, 640, NaN, 0, 1], [0, 0, 640, 540, 1, 0], [0, 0, 640, 540, 0, 2]]) {
    assert.equal(drawViewport(v, 0, 1920, 1080), null);
  }
});
test('effect clips follow centered third-player region on full and reduced targets', () => {
  const lo = 16384 | (32768 << 16), hi = 49151 | (65535 << 16);
  for (const [w, h] of [[1280, 720], [640, 360], [320, 180]]) {
    assert.deepEqual(drawScissor([0, 0, w, h, 0, 1], w, h, 0, lo, hi), [w/4, h/2, w/2, h/2]);
  }
});
test('camera scissor intersects UI bands and keeps disjoint clips empty', () => {
  assert.deepEqual(drawScissor([640, 0, 640, 360, 0, 1], 1280, 720, 50 | (670 << 16)), [640, 50, 640, 310]);
  assert.deepEqual(drawScissor([0, 0, 640, 360, 0, 1], 1280, 720, 400 | (720 << 16)), [0, 400, 640, 0]);
});
