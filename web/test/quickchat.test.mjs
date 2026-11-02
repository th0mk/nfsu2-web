// Quick chat's picker on the page (web/src/app/quickchat.ts): when it may open, in a real browser
// (web/tools/browser.mjs):
//   node --test web/test/quickchat.test.mjs
// - free roam with free roam on: 1 opens the first group and 1 again says its first phrase (free roam's way), Esc
//   closes the picker and the game does not get it, a flick of the right stick opens it
// - the career's free roam with free roam off (the game's "drive" is 2 there too): no picker by a key, the stick or
//   the touch button, and 1-4, Esc and the right stick stay the game's
// - a race between players (the game's "netrace") with free roam off: the picker, and the phrase goes to the race
// - quick chat off: no picker anywhere
// The module is bundled from web/src with esbuild into a blank page.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { chromium } from 'playwright';
import { pickBrowser } from '../tools/browser.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const js = (await esbuild.build({
  stdin: { contents: `export * from './web/src/app/quickchat.ts'; export { DEFAULT_PHRASES, DEFAULT_GROUPS } from './web/src/app/phrases.ts';`,
    resolveDir: ROOT, loader: 'ts' },
  bundle: true, format: 'iife', globalName: 'M', write: false, platform: 'browser',
})).outputFiles[0].text;

const browser = await chromium.launch({ ...pickBrowser(), headless: true });
test.after(() => browser.close());

/** a blank page with the module and a QuickChat; the game's status is w.st, what it was asked to say w.said, the keys
 *  that reached the game (the canvas) w.game */
async function page() {
  const p = await browser.newPage();
  await p.setContent('<div id="stage" style="position:relative;width:640px;height:480px"><canvas id="game" tabindex="0"></canvas></div>');
  await p.addScriptTag({ content: js });
  await p.evaluate(() => {
    const w = /** @type {any} */ (window);
    w.st = {}; w.said = []; w.game = [];
    document.getElementById('game').addEventListener('keydown', e => w.game.push(e.code));
    w.qc = new w.M.QuickChat(document.getElementById('stage'), { line() {}, lobby() {}, place() {}, log() {} });
    w.session = { status: { read: () => new Map(Object.entries(w.st)) }, net: { say: (p, race) => { w.said.push([p, race]); return true; } } };
  });
  return p;
}
/** a game started: quick chat on or off, free roam on or off, the game's "drive" and "netrace" */
const game = (p, on, roam, drive, netrace = 0) => p.evaluate(([on, roam, drive, netrace]) => {
  const w = /** @type {any} */ (window);
  w.qc.stop();
  w.st = { drive, netrace }; w.said = []; w.game = [];
  w.qc.start(w.session, on, roam, w.M.DEFAULT_PHRASES, w.M.DEFAULT_GROUPS);
}, [on, roam, drive, netrace]);
/** a key pressed on the game: whether it went on (not kept from the game) */
const key = (p, code) => p.evaluate(c => document.getElementById('game').dispatchEvent(
  new KeyboardEvent('keydown', { code: c, key: c, bubbles: true, cancelable: true })), code);
const open = p => p.evaluate(() => /** @type {any} */ (window).qc.state().open);
const said = p => p.evaluate(() => /** @type {any} */ (window).said);
const keys = p => p.evaluate(() => /** @type {any} */ (window).game);
/** the right stick flicked up and back: whether the picker took it */
const flick = p => p.evaluate(() => { const q = /** @type {any} */ (window).qc; const took = q.stick(0, -1); q.stick(0, 0); return took; });
const touch = p => p.evaluate(() => /** @type {any} */ (window).qc.toggle());

test('free roam with free roam on: keys, Esc, the stick', async () => {
  const p = await page();
  await game(p, true, true, 2);
  await key(p, 'Digit1');
  assert.equal(await open(p), 0, '1 opens the first group');
  await key(p, 'Digit1');
  assert.deepEqual(await said(p), [[0, false]], 'its first phrase, in free roam');
  assert.equal(await open(p), -1);
  await key(p, 'Digit2');
  assert.equal(await key(p, 'Escape'), false, 'Esc closes the picker, not the game');
  assert.equal(await open(p), -1);
  assert.equal(await flick(p), true, 'the stick opens it');
  assert.equal(await open(p), 0);
  await p.close();
});

test('the career\'s free roam with free roam off: no picker, the keys and the stick stay the game\'s', async () => {
  const p = await page();
  await game(p, true, false, 2);
  for (const k of ['Digit1', 'Digit2', 'Numpad3', 'Digit4']) {
    assert.equal(await key(p, k), true, k);
    assert.equal(await open(p), -1, `${k} opens no picker`);
  }
  assert.equal(await key(p, 'Escape'), true, 'Esc goes to the game (its pause menu)');
  assert.deepEqual(await keys(p), ['Digit1', 'Digit2', 'Numpad3', 'Digit4', 'Escape']);
  assert.equal(await flick(p), false, 'the stick stays the game\'s');
  await touch(p);
  assert.equal(await open(p), -1, 'nor the touch button');
  assert.equal(await p.evaluate(() => /** @type {HTMLElement} */ (document.querySelector('.qc-picker')).hidden), true);
  assert.deepEqual(await said(p), []);
  await p.close();
});

test('a race between players with free roam off: the picker, the phrase to the race', async () => {
  const p = await page();
  for (const [drive, netrace] of [[1, 1], [0, 2]]) {   // driving, the results
    await game(p, true, false, drive, netrace);
    await key(p, 'Digit2');
    assert.equal(await open(p), 1, `netrace ${netrace}: 2 opens the second group`);
    await key(p, 'Digit3');
    assert.deepEqual(await said(p), [[6, true]], `netrace ${netrace}: its third phrase, in the race`);
  }
  await game(p, true, false, 1, 1);
  await touch(p);
  assert.equal(await open(p), 4, 'the touch button shows all sixteen');
  await p.close();
});

test('quick chat off: no picker', async () => {
  const p = await page();
  for (const [roam, drive, netrace] of [[true, 2, 0], [false, 1, 1]]) {
    await game(p, false, roam, drive, netrace);
    assert.equal(await key(p, 'Digit1'), true);
    assert.equal(await flick(p), false);
    await touch(p);
    assert.equal(await open(p), -1);
  }
  await p.close();
});
