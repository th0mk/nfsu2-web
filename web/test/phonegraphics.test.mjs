// A phone's graphics (web/src/app/phonegraphics.ts): a new phone's registry is the built-in seed with six options
// off and the mark; a phone from before gets them once while every graphics option still has the seed's value, and
// only the mark when one was changed; a marked registry is left alone.  A new computer's registry has Full Scene
// Anti-Aliasing on; a tablet's is the seed.  A registry from before it (-1) is asked about it once.
//   node --test web/test/phonegraphics.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const js = (await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/app/phonegraphics.ts')], bundle: true, format: 'esm', write: false })).outputFiles[0].text;
const { PHONE_GRAPHICS, PHONE_MARK, COMPUTER_GRAPHICS, FSAA_MARK, deviceSeed, fsaaAnswer, fsaaAsk, phoneSeed, phoneUpgrade, withDwords } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
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

test("a new computer's registry: the seed with Full Scene Anti-Aliasing on; a tablet's the seed; a phone's its own", () => {
  const s = values(SEED), pc = deviceSeed('pc', SEED), v = values(pc);
  assert.equal(s.g_FSAALevel, 4294967295, "the seed's: off (-1), as the Mac app and tablets start");
  assert.deepEqual(COMPUTER_GRAPHICS, { g_FSAALevel: 1 });
  assert.equal(v.g_FSAALevel, 1);
  for (const k of Object.keys(s)) if (k !== 'g_FSAALevel') assert.equal(v[k], s[k], k);
  assert.equal(pc.split('\n').length, SEED.split('\n').length, 'the line replaced where it is');
  assert.equal(deviceSeed('tablet', SEED), SEED);
  assert.equal(deviceSeed('phone', SEED), phoneSeed(SEED));
});

test('a registry from before FSAA (-1) is asked once: on or kept off, marked either way; others are not asked', () => {
  const before = withDwords(SEED, { g_RacingResolution: 1 }) + 's DI_Keyboard_0=12\n';
  assert.equal(fsaaAsk(before), true);
  const on = fsaaAnswer(before, true), off = fsaaAnswer(before, false);
  assert.equal(values(on).g_FSAALevel, 1); assert.equal(values(on)[FSAA_MARK], 1);
  assert.equal(values(off).g_FSAALevel, 4294967295); assert.equal(values(off)[FSAA_MARK], 1);
  assert.ok(on.includes('s DI_Keyboard_0=12') && off.includes('s DI_Keyboard_0=12'), "the game's other lines kept");
  assert.equal(fsaaAsk(on), false); assert.equal(fsaaAsk(off), false);
  assert.equal(fsaaAsk(deviceSeed('pc', SEED)), false, "a new computer's: on already");
  assert.equal(fsaaAsk(withDwords(SEED, { g_FSAALevel: 0 })), false, 'turned off in the game: its choice');
});
