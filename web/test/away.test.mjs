// The app's away check (web/src/app/away.ts) on a fake clock: no input for a while gets a warning and then the
// place on the server goes back; a tab in the background gives it back without one; input brings it back.
//   node --test web/test/away.test.mjs
import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { sourceKeys } from '../tools/i18n-keys.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const js = (await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/app/away.ts')], bundle: true, format: 'esm', write: false })).outputFiles[0].text;
const { AwayCheck } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
// the two texts as away.ts writes them (the English is the key the page translates), so new wording needs no change
// here: the warning counts down in {n}, the other is for when the place went back
const TEXTS = sourceKeys(fs.readFileSync(path.join(ROOT, 'web/src/app/away.ts'), 'utf8')).keys.map(k => k.key);
const WARN = TEXTS.find(k => k.includes('{n}')), AWAY = TEXTS.find(k => !k.includes('{n}'));
assert.ok(TEXTS.length === 2 && WARN && AWAY, `away.ts's texts: ${JSON.stringify(TEXTS)}`);
const warn = n => WARN.replace('{n}', String(n));
const isWarn = s => !!s && s.startsWith(WARN.slice(0, WARN.indexOf('{n}')));

const L = { inputMs: 20 * 60_000, hiddenMs: 10 * 60_000, warnMs: 60_000 };
const MIN = 60_000;

// the page: a clock, a document that may be hidden, the game's input and network as the app sees them
function setup({ holding = true, input = true } = {}) {
  mock.timers.enable({ apis: ['setInterval'] });
  let t = 1000;
  Object.defineProperty(globalThis, 'performance', { value: { now: () => t }, configurable: true });
  const vis = new Set();
  globalThis.document = { hidden: false, addEventListener: (k, f) => vis.add(f), removeEventListener: (k, f) => vis.delete(f) };
  const g = { last: t, holding, calls: [], shown: [], logs: [] };
  const s = {
    input: { lastInput: () => (input ? g.last : null) },
    net: { holding: () => g.holding, away: on => { g.calls.push(on); g.holding = !on && g.holding; } },
  };
  const check = new AwayCheck(s, L, text => g.shown.push(text), l => g.logs.push(l));
  const tick = ms => { for (let k = 0; k < ms; k += 1000) { t += 1000; mock.timers.tick(1000); } };
  const use = () => { g.last = t; };
  const hide = on => { globalThis.document.hidden = on; for (const f of vis) f(); };
  return { g, check, tick, use, hide, now: () => t };
}
const done = (x) => { x.check.stop(); mock.timers.reset(); };

test('no input for 20 min: a warning that counts down 60 s, then the place goes back; input brings it back', () => {
  const x = setup();
  try {
    x.tick(20 * MIN - 1000);
    assert.deepEqual(x.g.shown, [], 'nothing before 20 min');
    x.tick(1000);
    assert.equal(x.g.shown.at(-1), warn(60));
    x.tick(30_000);
    assert.equal(x.g.shown.at(-1), warn(30));
    assert.deepEqual(x.g.calls, []);
    x.tick(30_000);
    assert.deepEqual(x.g.calls, [true], 'the place went back');
    assert.equal(x.g.shown.at(-1), AWAY);
    x.tick(10 * MIN);
    assert.deepEqual(x.g.calls, [true], 'once');
    x.use(); x.tick(1000);
    assert.deepEqual(x.g.calls, [true, false], 'back');
    assert.equal(x.g.shown.at(-1), null, 'the message is gone');
    assert.ok(x.g.logs.some(l => /no input for 21 min/.test(l)));
  } finally { done(x); }
});

test('input during the warning: it goes and the place stays', () => {
  const x = setup();
  try {
    x.tick(20 * MIN + 10_000);
    assert.ok(isWarn(x.g.shown.at(-1)), 'the warning');
    x.use(); x.tick(1000);
    assert.equal(x.g.shown.at(-1), null);
    x.tick(5 * MIN);
    assert.deepEqual(x.g.calls, []);
    x.tick(15 * MIN);                        // 20 min after that input: the warning again
    assert.ok(isWarn(x.g.shown.at(-1)), 'the warning');
  } finally { done(x); }
});

test('a tab in the background for 10 min: the place goes back at once, no warning first', () => {
  const x = setup();
  try {
    x.tick(5 * MIN); x.use();
    x.hide(true);
    x.tick(10 * MIN - 1000);
    assert.deepEqual(x.g.calls, []);
    x.tick(1000);
    assert.deepEqual(x.g.calls, [true]);
    assert.ok(!x.g.shown.some(isWarn), 'no warning');
    assert.equal(x.g.shown.at(-1), AWAY, 'the message for when they come back');
    x.hide(false); x.tick(5000);
    assert.deepEqual(x.g.calls, [true], 'seeing the tab again is not input');
    x.use(); x.tick(1000);
    assert.deepEqual(x.g.calls, [true, false]);
  } finally { done(x); }
});

test('a tab in the background for a while and back: the 10 min start again', () => {
  const x = setup();
  try {
    x.hide(true); x.tick(8 * MIN); x.hide(false); x.use(); x.tick(1000);
    x.hide(true); x.tick(8 * MIN);
    assert.deepEqual(x.g.calls, []);
  } finally { done(x); }
});

test('nothing while the game holds no place, or the page cannot see the input', () => {
  for (const o of [{ holding: false }, { input: false }]) {
    const x = setup(o);
    try {
      x.tick(30 * MIN); x.hide(true); x.tick(15 * MIN);
      assert.deepEqual(x.g.calls, [], JSON.stringify(o));
      assert.deepEqual(x.g.shown.filter(s => s !== null), []);
    } finally { done(x); }
  }
});

test('the place given up some other way during the warning (the game left free roam): the warning goes', () => {
  const x = setup();
  try {
    x.tick(20 * MIN + 5000);
    assert.ok(isWarn(x.g.shown.at(-1)), 'the warning');
    x.g.holding = false; x.tick(1000);
    assert.equal(x.g.shown.at(-1), null);
    x.tick(5 * MIN);
    assert.deepEqual(x.g.calls, []);
  } finally { done(x); }
});
