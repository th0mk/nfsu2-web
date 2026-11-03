// Quick chat's picker on the page (web/src/app/quickchat.ts): when it may open, in a real browser
// (web/tools/browser.mjs):
//   node --test web/test/quickchat.test.mjs
// - free roam with free roam on: 1 opens the first group and 1 again says its first phrase (free roam's way), Esc
//   closes the picker and the game does not get it; a flick of the right stick alone is the game's (its look round
//   the car), the stick's click shows the four groups, a flick opens one and another says its phrase, a second click
//   closes it
// - the career's free roam with free roam off (the game's "drive" is 2 there too): no picker by a key, the stick, its
//   click or the touch button, and 1-4, Esc and the right stick stay the game's
// - a race between players (the game's "netrace") with free roam off: the picker, and the phrase goes to the race
// - quick chat off: no picker anywhere
// - the countdown (the special phrase, in the default sixteen's last place): its stopwatch in the picker, 5 to 1 and
//   GO! big for the one who said it and a friend within 50 m, over the car for one farther; while one is on and 30 s
//   after, another's is not shown and the player's own waits (a far one holds back only far ones); in a race dimmed,
//   and it says nothing there
// - the countdown where the game's "nocount" says it is not the player's to start (a car meet, not stayed): dimmed,
//   picked it says only that it is not allowed here and nothing goes to the game; the game's own refusal the same
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
  await p.clock.install();
  await p.setContent('<div id="stage" style="position:relative;width:640px;height:480px"><canvas id="game" tabindex="0"></canvas></div>');
  await p.addScriptTag({ content: js });
  await p.evaluate(() => {
    const w = /** @type {any} */ (window);
    w.st = {}; w.said = []; w.game = [];
    document.getElementById('game').addEventListener('keydown', e => w.game.push(e.code));
    w.qc = new w.M.QuickChat(document.getElementById('stage'), { line() {}, lobby() {}, place() {}, log() {} });
    w.session = { status: { read: () => new Map(Object.entries(w.st)) }, net: { say: (p, race) => { w.said.push([p, race]); return true; } },
      viewRect: () => ({ x: 0, y: 0, w: 640, h: 480 }) };
  });
  await p.clock.runFor(1000);           // (the page's clock is the test's: past the status's 100 ms between reads)
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
/** the right stick flicked (up, or to x, y) and back: whether the picker took it */
const flick = (p, x = 0, y = -1) => p.evaluate(([x, y]) => {
  const q = /** @type {any} */ (window).qc; const took = q.stick(x, y); q.stick(0, 0); return took;
}, [x, y]);
/** the right stick pressed in: whether the picker took it */
const click = p => p.evaluate(() => /** @type {any} */ (window).qc.click());
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
  assert.equal(await flick(p), false, 'a flick alone opens nothing: the game looks round with the stick');
  assert.equal(await open(p), -1);
  assert.equal(await click(p), true, 'the stick\'s click opens it');
  assert.equal(await open(p), 5, 'the four groups');
  assert.deepEqual(await p.evaluate(() => [...document.querySelectorAll('.qc-row')].map(r => r.textContent)),
    ['↑Hi', '→Reply', '↓Drive', '←Race'], 'each with its flick');
  assert.equal(await p.evaluate(() => {
    const q = /** @type {any} */ (window).qc; q.stick(0, 0); return q.stick(0.5, 0);
  }), true, 'back at rest, then part way out: the picker\'s still (the game gets it at rest)');
  assert.equal(await flick(p, 1, 0), true, 'a flick right opens the second group');
  assert.equal(await open(p), 1);
  assert.equal(await flick(p, 0, 1), true, 'a flick down says its third phrase');
  assert.deepEqual(await said(p), [[0, false], [6, false]]);
  assert.equal(await open(p), -1);
  assert.equal(await click(p), true);
  assert.equal(await click(p), true, 'a second click closes it');
  assert.equal(await open(p), -1);
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
  assert.equal(await click(p), false, 'and its click (a car\'s hydraulics)');
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

/** what the game sends back (roam_game.c say_page): a phrase said by player id */
const from = (p, o) => p.evaluate(o => /** @type {any} */ (window).qc.said(JSON.stringify(o)), o);
const count = p => p.evaluate(() => /** @type {any} */ (window).qc.state().count);
const text = (p, sel) => p.evaluate(sel => document.querySelector(sel)?.textContent ?? null, sel);

test('the countdown: big near, over the car far, one at a time, not in a race', async () => {
  const p = await page();
  await game(p, true, true, 2);
  await key(p, 'Digit4');
  assert.equal(await p.evaluate(() => document.querySelectorAll('.qc-row svg.qc-watch').length), 1, 'the fourth group shows it with its stopwatch');
  assert.equal(await text(p, '.qc-row:nth-of-type(5) .qc-text'), 'Countdown', 'in the last place (the default sixteen)');
  await key(p, 'Digit4');
  assert.deepEqual(await said(p), [[152, false]], 'said as a phrase, in free roam');
  await from(p, { id: 1, name: 'Me', p: 152, me: 1 });
  assert.deepEqual(await count(p), { id: 1, big: true, text: '5' }, 'the player\'s own: big');
  assert.equal(await text(p, '.qc-count'), '5');
  for (const n of ['4', '3', '2', '1', 'GO!']) {
    await p.clock.runFor(1000);
    assert.equal((await count(p))?.text, n);
  }
  await from(p, { id: 2, name: 'Ann', p: 152, d: 10 });
  assert.equal((await count(p))?.id, 1, 'another\'s while one is on: not shown');
  await p.clock.runFor(1000);
  assert.equal(await count(p), null, 'gone after GO!');
  assert.equal(await p.evaluate(() => document.querySelectorAll('.qc-count').length), 0);
  await from(p, { id: 2, name: 'Ann', p: 152, d: 10 });
  await p.clock.runFor(20000);
  await from(p, { id: 2, name: 'Ann', p: 152, d: 10 });
  assert.equal(await count(p), null, 'nor within 30 s after');
  await key(p, 'Digit4'); await key(p, 'Digit4');
  assert.deepEqual(await said(p), [[152, false]], 'the player\'s own waits too');
  assert.equal(await text(p, '.qc-wait'), 'Wait a moment');
  await p.clock.runFor(10000);
  await from(p, { id: 3, name: 'Bo', p: 152, d: 80 });
  assert.deepEqual(await count(p), { id: 3, big: false, text: '5' }, 'past 50 m: over the car');
  assert.deepEqual(await p.evaluate(() => /** @type {any} */ (window).qc.state().lines), [3]);
  assert.equal(await p.evaluate(() => document.querySelectorAll('.qc-line svg.qc-watch').length), 1, 'its line with the stopwatch');
  await from(p, { id: 5, name: 'Dee', p: 152, d: 90 });
  assert.equal((await count(p))?.id, 3, 'another far one while it is on: not shown');
  await from(p, { id: 4, name: 'Cy', p: 152, d: 50 });
  assert.deepEqual(await count(p), { id: 4, big: true, text: '5' }, 'within 50 m: big, a far one holds back no near one');
  assert.deepEqual(await p.evaluate(() => /** @type {any} */ (window).qc.state().lines), [], 'the far one\'s line gone');
  await p.clock.runFor(35000);
  await from(p, { id: 6, name: 'Eve', p: 152, d: 20 });
  assert.equal(await count(p), null, 'nor another near one 35 s after the near one\'s start');
  await p.clock.runFor(1000);
  await from(p, { id: 6, name: 'Eve', p: 152, d: 20 });
  assert.equal((await count(p))?.id, 6, '36 s after: shown');
  await game(p, true, false, 1, 1);
  await key(p, 'Digit4');
  assert.equal(await p.evaluate(() => document.querySelectorAll('.qc-row .qc-off svg.qc-watch').length), 1, 'in a race: dimmed');
  await key(p, 'Digit4');
  assert.deepEqual(await said(p), [], 'and it says nothing there');
  await p.close();
});

test('the countdown at a car meet where the player has not stayed: dimmed, not allowed, nothing said', async () => {
  const p = await page();
  await game(p, true, true, 2);
  await p.evaluate(() => { /** @type {any} */ (window).st.nocount = 1; });
  await p.clock.runFor(200);                                     // (the status is read again)
  await key(p, 'Digit4');
  assert.equal(await p.evaluate(() => document.querySelectorAll('.qc-row .qc-off svg.qc-watch').length), 1, 'dimmed');
  await key(p, 'Digit4');
  assert.deepEqual(await said(p), [], 'nothing goes to the game');
  assert.equal(await text(p, '.qc-no'), 'You are not allowed to start a countdown here');
  await p.clock.runFor(4100);
  assert.equal(await open(p), -1, 'the message closes as the picker does');
  await key(p, 'Digit1'); await key(p, 'Digit1');
  assert.deepEqual(await said(p), [[0, false]], 'other phrases go as ever');
  // the game's own refusal (a pick as the status changed): the same message, no countdown
  await from(p, { id: 1, name: 'Me', p: 152, me: 1, no: 1 });
  assert.equal(await count(p), null, 'no countdown');
  assert.equal(await text(p, '.qc-no'), 'You are not allowed to start a countdown here');
  // stayed (or the meet gone): its own again
  await p.evaluate(() => { /** @type {any} */ (window).st.nocount = 0; });
  await p.clock.runFor(4200);
  await key(p, 'Digit4');
  assert.equal(await p.evaluate(() => document.querySelectorAll('.qc-row .qc-off').length), 0, 'not dimmed');
  await key(p, 'Digit4');
  assert.deepEqual(await said(p), [[0, false], [152, false]], 'said');
  await p.close();
});
