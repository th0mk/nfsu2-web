// The field for a phone's keyboard (web/src/app/textentry.ts): it keeps what the game's text entry takes
// (0x4FF310: letters and digits, more by kind), and every change of it reaches the game as backspaces back to
// where the two texts part, then the rest, so the game's text ends up the field's.
//   node --test web/test/textentry.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
// a game's page: its '@game' (web/tsconfig.json maps it to Underground 2's, as tsc does)
const load = async game => {
  const js = (await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/app/textentry.ts')], bundle: true, format: 'esm', write: false,
    alias: { '@game': path.join(ROOT, `web/src/games/${game}`) } })).outputFiles[0].text;
  return import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
};
const { takes, fit, edits } = await load('ug2');
const mw = await load('mw');

// the game's side: 0x551270 on each WM_CHAR (8 deletes the last character, anything else is added)
const apply = (text, chars) => chars.reduce((t, c) => c === 8 ? t.slice(0, -1) : t + String.fromCharCode(c), text);

test('a profile name takes letters and digits, 7 of them', () => {
  assert.equal(fit('Thom M4 Pro!', 1, 7), 'ThomM4P');
  assert.equal(fit('Zoë_99', 1, 7), 'Zo99');
  assert.equal(fit('😀abc', 1, 7), 'abc');
});

test('the kinds with more characters', () => {
  assert.ok(takes(4, '@') && takes(4, '.') && takes(4, '_') && !takes(4, ' '));
  assert.ok(takes(5, ' ') && takes(5, ',') && !takes(5, '_'));
  assert.ok(takes(0, 'é') && takes(3, '~') && takes(0, ' ') && !takes(0, '€') && !takes(2, 'é'));
});

test("Most Wanted's kinds 0 and 3 also take the inverted question and exclamation marks (0x8F53F0)", () => {
  assert.ok(mw.takes(0, '¿') && mw.takes(3, '¡') && !mw.takes(1, '¿'));
  assert.ok(!takes(0, '¿') && !takes(3, '¡'));
  assert.equal(mw.fit('Zoë_99', 1, 7), 'Zo99');
  assert.ok(mw.takes(4, '@') && mw.takes(5, ' ') && !mw.takes(4, ' ') && mw.takes(0, 'é') && !mw.takes(0, '€'));
});

test('the game ends up with the field\'s text', () => {
  const cases = [['NAME', 'T'], ['', 'THOM'], ['THOM', 'THO'], ['THOM', 'THAM'], ['THOM', 'THOM'], ['ABC', ''], ['Zoe', 'Zoé']];
  for (const [from, to] of cases) assert.equal(apply(from, edits(from, to)), to, `${from} -> ${to}`);
  assert.deepEqual(edits('NAME', 'T'), [8, 8, 8, 8, 0x54]);
  assert.deepEqual(edits('THOM', 'THOMAS'), [0x41, 0x53]);
  assert.deepEqual(edits('Zoe', 'Zoé'), [8, 0xE9]);
});
