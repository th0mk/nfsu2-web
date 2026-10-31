// A phone's graphics (web/src/app/phonegraphics.ts): a new phone's registry is the built-in seed with six options
// off and the mark; a phone from before gets them once while every graphics option still has the seed's value, and
// only the mark when one was changed; a marked registry is left alone.  Most Wanted's set (web/src/games/mw/registry.ts,
// TRACKER M6-58) has only options its seed holds, so its phones get no line the game never reads.
//   node --test web/test/phonegraphics.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const js = (await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/app/phonegraphics.ts')], bundle: true, format: 'esm', write: false })).outputFiles[0].text;
const { PHONE_GRAPHICS, PHONE_MARK, phoneSeed, phoneUpgrade, withDwords } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
const SEED = fs.readFileSync(path.join(ROOT, 'app/registry-defaults.txt'), 'utf8');

const values = text => Object.fromEntries([...text.matchAll(/^d ([^=\n]+)=(\d+)\r?$/gm)].map(m => [m[1], +m[2]]));
const OFF = Object.keys(PHONE_GRAPHICS);

test('the six options are in the seed, and on there', () => {
  const v = values(SEED);
  assert.equal(OFF.length, 6);
  for (const k of OFF) assert.ok(v[k] > 0, k);
});

test("a new phone's registry: the seed with the six off and the mark", () => {
  const out = phoneSeed(SEED), v = values(out), s = values(SEED);
  for (const k of OFF) assert.equal(v[k], 0, k);
  assert.equal(v[PHONE_MARK], 1);
  for (const k of Object.keys(s)) if (!OFF.includes(k)) assert.equal(v[k], s[k], k);
  assert.equal(out.split('\n').length, SEED.split('\n').length + 1);
  assert.equal(phoneUpgrade(out, SEED), null);
});

test("a phone from before, graphics untouched: lowered once, the game's other lines kept", () => {
  const before = withDwords(SEED, { g_RacingResolution: 1, HornVolume: 70 }) + 's DI_Keyboard_0=12\n';
  const up = phoneUpgrade(before, SEED);
  assert.equal(up.lowered, true);
  const v = values(up.text);
  for (const k of OFF) assert.equal(v[k], 0, k);
  assert.equal(v[PHONE_MARK], 1);
  assert.equal(v.g_RacingResolution, 1);
  assert.equal(v.HornVolume, 70);
  assert.match(up.text, /^s DI_Keyboard_0=12$/m);
  assert.equal(phoneUpgrade(up.text, SEED), null);
});

test('a phone from before where a graphics option was changed: only the mark', () => {
  const before = withDwords(SEED, { g_LightStreaksEnable: 0 });
  const up = phoneUpgrade(before, SEED);
  assert.equal(up.lowered, false);
  assert.deepEqual(values(up.text), { ...values(before), [PHONE_MARK]: 1 });
  // set back to the seed's values later: still the player's
  assert.equal(phoneUpgrade(withDwords(up.text, { g_LightStreaksEnable: 1 }), SEED), null);
});

test('a registry the game made itself (seed_registry = off) is not the seed', () => {
  const own = 'd NotFirstTime=1\nd g_CarEnvironmentMapEnable=1\nd g_RoadReflectionEnable=2\nd g_PerformanceLevel=3\n';
  assert.equal(phoneUpgrade(own, SEED).lowered, false);
  assert.equal(phoneUpgrade('', SEED).text, `d ${PHONE_MARK}=1\n`);
});

test('CRLF lines keep their line ends', () => {
  const crlf = SEED.replace(/\n/g, '\r\n'), out = phoneSeed(crlf);
  assert.doesNotMatch(out.replace(/\r\n/g, ''), /\n/);
  assert.equal(values(out).g_RoadReflectionEnable, 0);
});

// Most Wanted's registry module: its seed and its phone graphics
const mw = (await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/games/mw/registry.ts')], bundle: true, format: 'esm',
  write: false, loader: { '.txt': 'text' } })).outputFiles[0].text;
const MW = await import('data:text/javascript;base64,' + Buffer.from(mw).toString('base64'));

test("Most Wanted's phone graphics: options its seed holds, on there; its phones get those off and nothing else", () => {
  const s = values(MW.REGISTRY_DEFAULTS), off = Object.keys(MW.PHONE_GRAPHICS);
  assert.ok(off.length > 0);
  for (const k of off) { assert.ok(s[k] > 0, k); assert.equal(MW.PHONE_GRAPHICS[k], 0, k); }
  const out = phoneSeed(MW.REGISTRY_DEFAULTS, MW.PHONE_GRAPHICS), v = values(out);
  assert.deepEqual(Object.keys(v).sort(), [...Object.keys(s), PHONE_MARK].sort());   // no key the game does not read
  for (const k of Object.keys(s)) assert.equal(v[k], off.includes(k) ? 0 : s[k], k);
  for (const k of OFF.filter(k => !(k in s))) assert.equal(v[k], undefined, k);       // Underground 2's others
  assert.equal(phoneUpgrade(out, MW.REGISTRY_DEFAULTS, MW.PHONE_GRAPHICS), null);
  const up = phoneUpgrade(withDwords(MW.REGISTRY_DEFAULTS, { g_RacingResolution: 3 }), MW.REGISTRY_DEFAULTS, MW.PHONE_GRAPHICS);
  assert.equal(up.lowered, true);
  assert.deepEqual(values(up.text), { ...s, ...MW.PHONE_GRAPHICS, g_RacingResolution: 3, [PHONE_MARK]: 1 });
  const changed = phoneUpgrade(withDwords(MW.REGISTRY_DEFAULTS, { g_ShadowDetail: 1 }), MW.REGISTRY_DEFAULTS, MW.PHONE_GRAPHICS);
  assert.equal(changed.lowered, false);
});
