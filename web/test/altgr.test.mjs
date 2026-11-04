// AltGr and the game's keys (web/src/input/keymap.ts altGrControl, index.ts): a layout's third level arrives as the
// left Control and the right Alt together, and only the Alt is the game's (DirectInput on Windows sees no Control).
//   node --test web/test/altgr.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const out = await esbuild.build({ entryPoints: [path.join(HERE, '../src/input/keymap.ts')], bundle: true, format: 'esm',
  write: false, platform: 'neutral' });
const K = await import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));

/** the KeyboardEvent the browser makes for ControlLeft while AltGr is held, or for an ordinary Ctrl */
const ev = (altGraph, code = 'ControlLeft', key = 'Control') =>
  ({ code, key, ctrlKey: true, altKey: altGraph, metaKey: false, getModifierState: k => k === 'AltGraph' && altGraph });

test('the Control that AltGr presses is not the game\'s', () => {
  assert.equal(K.altGrControl('ControlLeft', ev(true)), true, 'AltGr holds the Control down');
  assert.equal(K.altGrControl('ControlLeft', ev(false)), false, 'a Control of the player\'s own stays');
});

test('other keys are untouched', () => {
  assert.equal(K.altGrControl('ControlRight', ev(true)), false, 'only the left Control comes with AltGr');
  assert.equal(K.altGrControl('AltRight', ev(true)), false, 'the Alt itself is the game\'s (nitrous)');
  assert.equal(K.altGrControl('KeyA', ev(true)), false, 'the key AltGr types is still a key');
  assert.equal(K.altGrControl('ShiftLeft', ev(true)), false);
});

test('a real Ctrl+Alt (no AltGraph) is not AltGr', () => {
  const e = { code: 'ControlLeft', key: 'Control', ctrlKey: true, altKey: true, metaKey: false,
    getModifierState: () => false };
  assert.equal(K.altGrControl(e.code, e), false, 'Ctrl+Alt is the player pressing both');
});
