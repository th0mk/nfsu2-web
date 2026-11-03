// The Cloud saves box (vault-ui.ts VaultPanel): the whole passphrase, shown by the eye, is dotted again by itself 30 s
// later, 30 s after the last key typed in it when the player types (2026-11-03: Saves tab screenshots sent for help had
// the phrase left shown in them).  The page's own VaultPanel on stand-in elements, under node's mock timers.
//   node --test web/test/vault-box.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const js = (await esbuild.build({ stdin: { contents: `export { VaultPanel } from './web/src/app/vault-ui.ts';`, resolveDir: ROOT, loader: 'ts' },
  bundle: true, format: 'esm', write: false, platform: 'neutral', logLevel: 'error' })).outputFiles[0].text;
const store = new Map();
globalThis.localStorage = { getItem: k => store.get(k) ?? null, setItem: (k, v) => store.set(k, String(v)), removeItem: k => store.delete(k) };
const el = () => {
  const on = new Map(), cls = new Set();
  const e = { hidden: false, disabled: false, value: '', type: 'password', readOnly: false, textContent: '', scrollLeft: 0, className: '', dataset: {}, kids: [],
    classList: { toggle: (c, f) => (f ?? !cls.has(c)) ? cls.add(c) : cls.delete(c), add: c => cls.add(c), remove: c => cls.delete(c), contains: c => cls.has(c) },
    setAttribute() {}, append(...k) { e.kids.push(...k); }, replaceChildren(...k) { e.kids = k; }, focus() {}, select() {},
    addEventListener: (ev, f) => on.set(ev, [...on.get(ev) ?? [], f]), fire: ev => { for (const f of on.get(ev) ?? []) f(); } };
  return e;
};
globalThis.document = { createElement: el, createDocumentFragment: el };
const { VaultPanel } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

const PHRASE = 'nfsu2-xyz-ruin-upon-kitty-sushi-fend-slip-math-hug';
/** a panel with a phrase typed in its box; shown(): the whole of it, not dots */
function box() {
  const els = new Map(), $ = s => els.get(s) ?? (els.set(s, el()), els.get(s));
  new VaultPanel({ querySelector: $ }, { saveDir: 'vault-box-test', owner: () => true, playing: () => false, busy: () => false, log() {} });
  const input = $('#vault-input');
  input.value = PHRASE;
  input.fire('input');
  const shown = () => {
    const whole = !$('#vault-box').classList.contains('masked');
    assert.equal(input.type, whole ? 'text' : 'password', 'a password field while dotted');
    if (!whole) assert.equal($('#vault-mask').textContent, 'nfs' + '•'.repeat(PHRASE.length - 3));
    return whole;
  };
  return { eye: () => $('#vault-eye').onclick(), type: () => input.fire('input'), shown };
}

test('the eye shows the passphrase for 30 s, then the dots come back', t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const b = box();
  assert.equal(b.shown(), false, 'dotted at first');
  b.eye();
  assert.equal(b.shown(), true);
  t.mock.timers.tick(29_999);
  assert.equal(b.shown(), true, 'still shown at 29.999 s');
  t.mock.timers.tick(1);
  assert.equal(b.shown(), false, 'dotted again at 30 s');
  b.eye();
  assert.equal(b.shown(), true, 'the eye shows it again');
});

test('typing in the box starts the 30 s again', t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const b = box();
  b.eye();
  t.mock.timers.tick(20_000);
  b.type();
  t.mock.timers.tick(20_000);
  assert.equal(b.shown(), true, '40 s after the eye, 20 s after a key');
  t.mock.timers.tick(10_000);
  assert.equal(b.shown(), false, '30 s after the last key');
  b.type();
  t.mock.timers.tick(60_000);
  assert.equal(b.shown(), false, 'typing while dotted shows nothing');
});

test('hidden by the eye, the old wait does not hide it early once shown again', t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const b = box();
  b.eye();
  t.mock.timers.tick(10_000);
  b.eye();
  assert.equal(b.shown(), false, 'the eye hides it');
  t.mock.timers.tick(10_000);
  b.eye();
  t.mock.timers.tick(15_000);
  assert.equal(b.shown(), true, '35 s after the first show, 15 s after this one');
  t.mock.timers.tick(15_000);
  assert.equal(b.shown(), false);
});
