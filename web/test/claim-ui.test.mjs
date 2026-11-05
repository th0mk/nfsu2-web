// One name (web/src/app/claim-ui.ts), in a real browser (web/tools/browser.mjs):
//   node --test web/test/claim-ui.test.mjs
// - Play: a passphrase with a claimed name plays as that name, whatever Your name says (another free name, one
//   another player claimed, NAME, the profile's name with the box empty, a name the page refuses), and never moves
//   the claim; with a passphrase but no claim yet a free name is claimed at Play, and without a passphrase the name
//   plays as typed (2026-11-05: a player who typed another name after claiming played under one nobody held, which
//   nobody could add as a friend)
// - the line under Your name: the claimed name is the one played, with Change name (or the day it may change); an
//   empty box shows the claimed name; the typed name itself, claimed, says so; without a claim, as before
// The module is bundled from web/src with esbuild into a blank page; /names is a stand-in that records what is asked.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { chromium } from 'playwright';
import { pickBrowser } from '../tools/browser.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const js = (await esbuild.build({
  stdin: { contents: `export * from './web/src/app/claim-ui.ts';`, resolveDir: ROOT, loader: 'ts' },
  bundle: true, format: 'iife', globalName: 'U', write: false, platform: 'browser',
})).outputFiles[0].text;

const browser = await chromium.launch({ ...pickBrowser(), headless: true });
test.after(() => browser.close());

const DAY = 86_400_000;

/** a blank page with the module and a stand-in /names: `mine` the passphrase's claim (null: none), `states` what
 *  each name is (lower case; anything else is free) */
async function page() {
  const p = await browser.newPage();
  // (localhost: a secure context, as the page's own, for crypto.subtle's digits)
  await p.route('http://localhost/', r => r.fulfill({ contentType: 'text/html', body: '<div class="set"><div class="set-control"><input id="name" placeholder="profile name"></div></div>' }));
  await p.goto('http://localhost/');
  await p.addScriptTag({ content: js });
  await p.evaluate(() => {
    const w = /** @type {any} */ (window);
    w.mine = null; w.states = {}; w.linked = true; w.asked = []; w.claimed = []; w.saved = []; w.kept = undefined; w.questions = 0;
    w.down = false; w.heard = null;
    w.claims = {
      here: true,
      async check(name) {
        w.asked.push(name);
        if (w.down) return null;
        const state = name ? w.states[name.toLowerCase()] ?? 'free' : 'invalid';
        return { name: name || null, state, ...(state === 'old' ? { until: Date.now() + 5 * 86_400_000 } : {}), ...(w.linked && w.mine ? { mine: w.mine } : {}) };
      },
      async claim(name) {
        w.claimed.push(name);
        w.mine = { name, until: Date.now() + 730 * 86_400_000, next: Date.now() + 7 * 86_400_000 };
        return { ok: true, mine: w.mine };
      },
    };
    w.play = typed => w.U.settleName({
      claims: w.claims, seed: () => 'seed', linked: () => w.linked, log: () => {},
      choose: async () => { w.questions++; return 'cancel'; }, save: async n => { w.saved.push(n); }, claimed: () => w.heard,
    }, typed, '/save', async () => null);
    w.row = new w.U.NameRow({
      claims: w.claims, seed: () => 'seed', linked: () => w.linked, enable: async () => null, showPassphrase: async () => {},
      profileName: async () => 'Profile1', save: async n => { w.saved.push(n); document.getElementById('name').value = n; },
      writable: () => true, kept: n => { w.kept = n; }, say: () => {}, log: () => {},
    });
    const input = document.getElementById('name');
    w.row.attach(input.closest('.set'), input, input.parentElement);
  });
  return p;
}

/** Your name typed, and the line once the server answered */
async function typed(p, name) {
  return p.evaluate(async n => {
    const w = /** @type {any} */ (window), input = document.getElementById('name');
    input.value = n;
    await w.row.ask();
    const line = document.getElementById('set-roam_name-claim'), btn = input.parentElement.querySelector('button.btn');
    return { text: line.hidden ? null : line.textContent, cls: line.className.replace('set-state ', ''), button: btn.hidden ? null : btn.textContent, placeholder: input.placeholder };
  }, name);
}
const set = (p, o) => p.evaluate(x => Object.assign(window, x), o);
const take = (p, k) => p.evaluate(x => /** @type {any} */ (window)[x].splice(0), k);

test('Play: a claimed name is the one played, whatever Your name says, and the claim never moves', async () => {
  const p = await page();
  await set(p, { mine: { name: 'Speedy', until: Date.now() + 700 * DAY, next: 0 }, states: { taken1: 'taken', name: 'default', speedy: 'yours', older: 'old' } });
  await take(p, 'asked');
  for (const typed of ['Racer_X_99', 'Taken1', 'NAME', 'Older', '', 'Hitler']) {
    assert.equal(await p.evaluate(t => /** @type {any} */ (window).play(t), typed), 'Speedy', JSON.stringify(typed));
  }
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).play('SPEEDY')), 'SPEEDY', 'the claimed name in the case typed');
  assert.deepEqual(await take(p, 'claimed'), [], 'no claim made or moved');
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).questions), 0, 'no question about the typed name');
  assert.deepEqual(await take(p, 'asked'), ['Racer_X_99', 'Taken1', 'NAME', 'Older', '', '', 'SPEEDY'], 'the server asked (a refused name: for the claim only)');
  // no answer at Play: the claimed name as the page heard it before, else as typed
  await set(p, { down: true, heard: 'Speedy' });
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).play('Racer_X_99')), 'Speedy');
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).play('speedy')), null, 'the same name: as typed');
  await set(p, { heard: null });
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).play('Racer_X_99')), null);
  await set(p, { heard: 'Speedy', linked: false });
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).play('Racer_X_99')), null, 'no passphrase here: nothing heard counts');
});

test('Play without a claim: a free name is claimed with a passphrase, played as typed without one', async () => {
  const p = await page();
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).play('Newbie')), 'Newbie');
  assert.deepEqual(await take(p, 'claimed'), ['Newbie'], 'claimed at Play');
  await set(p, { mine: null, linked: false });
  await take(p, 'asked');
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).play('Racer')), 'Racer');
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).play('')), null, 'no name, no passphrase: nothing to ask');
  assert.deepEqual(await take(p, 'asked'), ['Racer']);
  assert.deepEqual(await take(p, 'claimed'), []);
});

test('the line under Your name: the claimed name is the one played', async () => {
  const p = await page();
  await set(p, { mine: { name: 'Speedy', until: Date.now() + 700 * DAY, next: 0 }, states: { taken1: 'taken', name: 'default', speedy: 'yours', older: 'old' } });
  assert.deepEqual(await typed(p, 'Racer_X_99'), {
    text: 'You play as Speedy, your claimed name. To play as Racer_X_99, press Change name: your friends move with you, and Speedy stays yours for 14 days. One change a week.',
    cls: 'env', button: 'Change name', placeholder: 'Speedy',
  });
  assert.deepEqual(await typed(p, 'Taken1'), { text: 'Another player claimed Taken1. You play as Speedy, your claimed name.', cls: 'env', button: null, placeholder: 'Speedy' });
  assert.equal((await typed(p, 'NAME')).text, 'You play as Speedy, your claimed name.');
  assert.match((await typed(p, 'Older')).text, /^You play as Speedy, your claimed name\. Older was yours before: yours until /);
  assert.deepEqual(await typed(p, 'speedy'), { text: 'Claimed with your passphrase. It stays yours as long as the passphrase does.', cls: 'ok', button: null, placeholder: 'Speedy' });
  assert.deepEqual(await typed(p, ''), { text: 'Claimed with your passphrase. It stays yours as long as the passphrase does.', cls: 'ok', button: null, placeholder: 'Speedy' },
    'an empty box (the profile is another name) plays as the claim, which shows in it');
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).kept), 'Speedy', 'the Cloud saves section hears of the claim');
  // (settings-ui's "applies the next time you press Play" is not said of a name Play does not use)
  assert.deepEqual(await p.evaluate(() => ['Racer_X_99', 'speedy', '', null].map(n => /** @type {any} */ (window).row.notPlayed(n))), [true, false, false, false]);
  // a change made less than a week ago: the day the next may be
  const next = Date.now() + 2 * DAY;
  await set(p, { mine: { name: 'Speedy', until: Date.now() + 700 * DAY, next } });
  const soon = await typed(p, 'Racer_X_99');
  assert.match(soon.text, /^You play as Speedy, your claimed name\. You can change it to Racer_X_99 on .+\.$/);
  assert.equal(soon.button, null);
  assert.equal((await typed(p, 'Older')).button, null, 'no change back before then either');
});

test('Change name moves the claim, and the line says it is the one played', async () => {
  const p = await page();
  await set(p, { mine: { name: 'Speedy', until: Date.now() + 700 * DAY, next: 0 }, states: { speedy: 'yours' } });
  await typed(p, 'Racer_X_99');
  await p.evaluate(() => { /** @type {any} */ (window).states = { racer_x_99: 'yours', speedy: 'old' }; });
  await p.click('button.btn');
  await p.waitForFunction(() => /** @type {any} */ (window).claimed.length);
  assert.deepEqual(await take(p, 'claimed'), ['Racer_X_99']);
  await p.waitForFunction(() => document.getElementById('set-roam_name-claim').textContent.startsWith('Claimed'));
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).play('Racer_X_99')), 'Racer_X_99');
});

test('the line without a claim: as before', async () => {
  const p = await page();
  assert.deepEqual(await typed(p, 'Newbie'), { text: 'Not claimed yet. Your passphrase claims it the next time you play with others.', cls: 'free', button: 'Claim', placeholder: 'profile name' });
  await set(p, { linked: false });
  assert.equal((await typed(p, 'Newbie')).button, 'Claim');
  assert.match((await typed(p, 'Newbie')).text, /^Not claimed: anyone can play as Newbie\./);
  // a claim gone (another passphrase loaded): the box's own placeholder back
  await set(p, { linked: true, mine: { name: 'Speedy', until: Date.now() + 700 * DAY, next: 0 } });
  assert.equal((await typed(p, '')).placeholder, 'Speedy');
  await set(p, { mine: null });
  assert.equal((await typed(p, '')).placeholder, 'profile name');
});
