// Messages between friends on the page (web/src/app/messages.ts), in a real browser (web/tools/browser.mjs):
//   node --test web/test/messages.test.mjs
// - in the game: a friend's message (one by one, or the ones that waited, with the list) on the log while the player
//   drives in free roam, held until they do; "I Write back" under it; I answers the newest, else a friend to pick
//   (1-9, in free roam first), Enter sends {op:"msg"}, Esc closes; the server's answer puts the player's own on the
//   log ("To Bo", a note when it waits) or says why not; no answer in 10 s: not sent; I never in a text field, with a
//   modifier or while not driving; T stays the game's (its Career Status, issue #52)
// - the conversations: kept in this browser per claimed name, the friends' unread until shown
// - the start page's conversation (ChatDialog): the messages kept, Send, what became of it, why one did not go
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
  stdin: { contents: `export * from './web/src/app/messages.ts';`, resolveDir: ROOT, loader: 'ts' },
  bundle: true, format: 'iife', globalName: 'M', write: false, platform: 'browser',
})).outputFiles[0].text;

const browser = await chromium.launch({ ...pickBrowser(), headless: true });
test.after(() => browser.close());

const CHAT = `<dialog id="chat-pop"><h2 id="chat-pop-title"></h2><div id="chat-log"></div>
  <input type="text" id="chat-text"><button id="chat-send">Send</button><span id="chat-note"></span><span id="chat-left"></span></dialog>`;

/** a blank page with the module, a stage and a FriendMessages whose deps record what it does (driving: w.driving) */
async function page() {
  const p = await browser.newPage();
  // an origin of its own: localStorage (a blank page has none)
  await p.route('http://messages.test/', r => r.fulfill({ contentType: 'text/html',
    body: `<div id="stage" style="position:relative;width:640px;height:480px"><canvas id="game" tabindex="0"></canvas></div><input id="field">${CHAT}` }));
  await p.goto('http://messages.test/');
  await p.addScriptTag({ content: js });
  await p.evaluate(() => {
    const w = /** @type {any} */ (window);
    w.sent = []; w.lines = []; w.hints = []; w.toasts = []; w.driving = true; w.answer = true;
    w.store = new w.M.MessageStore();
    w.msgs = new w.M.FriendMessages(document.getElementById('stage'), w.store, {
      send: m => { w.sent.push(m); return w.answer; }, driving: () => w.driving,
      line: (name, text, out, note) => { w.lines.push(`${out ? '>' : '<'} ${name}: ${text}${note ? ` ${note}` : ''}`); return document.createElement('div'); },
      hint: ms => w.hints.push(ms === null ? null : 'I'), placeNow: () => {}, focusGame: () => document.getElementById('game').focus(),
      toast: s => w.toasts.push(s), log: () => {},
    });
    w.msgs.start({}, true);
    document.getElementById('game').focus();
  });
  return p;
}
const take = (p, what) => p.evaluate(k => /** @type {any} */ (window)[k].splice(0), what);
const server = (p, m) => p.evaluate(x => /** @type {any} */ (window).msgs.server({ t: 'friend', ...x }), m);
const state = p => p.evaluate(() => /** @type {any} */ (window).msgs.state());
/** the card's text, a space between its parts (the page has no style sheet here), or null when it is closed */
const card = p => p.evaluate(() => {
  const c = document.querySelector('.msg-card'), parts = [], tw = document.createTreeWalker(c, NodeFilter.SHOW_TEXT);
  while (tw.nextNode()) parts.push(tw.currentNode.textContent);
  return c.hidden ? null : parts.join(' ').replace(/\s+/g, ' ').trim();
});
const wait = (p, ms) => p.evaluate(t => new Promise(r => setTimeout(r, t)), ms);
const list = { op: 'list', me: 'Al_', friends: ['Gus', 'Bo_', 'Cy_'], asks: [] };
const where = { op: 'where', friends: [{ name: 'Gus', where: '' }, { name: 'Bo_', where: 'roam', room: '' }, { name: 'Cy_', where: 'online' }] };

test('in the game: messages on the log, I answers, Enter sends, the answer', async () => {
  const p = await page();
  await server(p, { ...list, mail: [{ from: 'Bo_', body: 'meet me at the airport', at: 1000 }] });
  assert.deepEqual(await take(p, 'lines'), ['< Bo_: meet me at the airport'], 'the ones that waited, with the list');
  assert.deepEqual(await take(p, 'hints'), ['I']);
  // I: the answer to the newest, the message quoted; the game's keys go to the field
  await p.keyboard.press('KeyI');
  assert.equal(await card(p), 'Message Bo_ Bo_ meet me at the airport Enter sends · Esc closes 120');
  assert.equal(await p.evaluate(() => document.activeElement?.className), 'msg-input');
  assert.deepEqual(await take(p, 'hints'), [null], '"I Write back" goes');
  await p.keyboard.type('on my way');
  assert.match(await card(p), / 111$/, 'the characters left');
  await p.keyboard.press('Enter');
  assert.equal(await card(p), null);
  assert.equal(await p.evaluate(() => document.activeElement?.id), 'game', 'the keys back to the game');
  assert.deepEqual(await take(p, 'sent'), [{ op: 'msg', name: 'Bo_', body: 'on my way', id: 1 }]);
  assert.deepEqual(await take(p, 'lines'), [], 'on the log once the server says it went');
  await server(p, { op: 'msg', id: 1, name: 'Bo_', done: 'sent' });
  assert.deepEqual(await take(p, 'lines'), ['> To Bo_: on my way']);
  // one by one; while not driving, held until the player drives
  await p.evaluate(() => { /** @type {any} */ (window).driving = false; });
  await server(p, { op: 'mail', from: 'Cy_', body: 'race later?', at: 2000 });
  assert.deepEqual(await take(p, 'lines'), []);
  await p.keyboard.press('KeyI');
  assert.equal(await card(p), null, 'no I while not driving');
  await p.evaluate(() => { /** @type {any} */ (window).driving = true; });
  await wait(p, 300);
  assert.deepEqual(await take(p, 'lines'), ['< Cy_: race later?']);
  // a friend who is away: the note; why one did not go; no answer at all
  await p.keyboard.press('KeyI');
  await p.keyboard.type('sure');
  await p.keyboard.press('Enter');
  await server(p, { op: 'msg', id: 2, name: 'Cy_', done: 'kept' });
  assert.deepEqual(await take(p, 'lines'), ['> To Cy_: sure (gets it when back)']);
  for (const [done, why] of [['none', 'Cy_ is not on your friends list'], ['off', 'You can\'t send messages for a few minutes'], ['rate', 'Wait a moment']]) {
    await p.keyboard.press('KeyI');
    await p.keyboard.type('x');
    await p.keyboard.press('Enter');
    const { id } = (await take(p, 'sent')).at(-1);
    await server(p, { op: 'msg', id, name: 'Cy_', done });
    assert.deepEqual(await take(p, 'toasts'), [why], done);
  }
  assert.deepEqual(await take(p, 'lines'), []);
  const msgs = await p.evaluate(() => /** @type {any} */ (window).store.messages('cy_').map(m => `${m.dir} ${m.body} ${m.st ?? ''}`.trim()));
  assert.deepEqual(msgs, ['in race later?', 'out sure kept', 'out x failed', 'out x failed', 'out x failed'], 'kept in this browser');
  await p.close();
});

test('in the game: I with nothing to answer picks a friend; Esc; never in a field', async () => {
  const p = await page();
  await server(p, list);
  await server(p, where);
  // T is the game's (Career Status): not taken, the game gets it
  await p.evaluate(() => { const w = /** @type {any} */ (window); w.gameT = 0;
    document.getElementById('game').addEventListener('keydown', e => { if (e.code === 'KeyT' && !e.defaultPrevented) w.gameT++; }); });
  await p.keyboard.press('KeyT');
  assert.equal(await card(p), null, 'T opens nothing');
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).gameT), 1, 'T goes to the game');
  await p.keyboard.press('KeyI');
  assert.deepEqual(await take(p, 'sent'), [{ op: 'where' }], 'where each plays now');
  assert.equal(await card(p), 'Message a friend 1 Bo_ free roam 2 Cy_ Online lobby 3 Gus offline Esc closes');
  await p.keyboard.press('Digit2');
  assert.match(await card(p), /^Message Cy_ Enter sends/);
  await p.keyboard.press('Escape');
  assert.equal(await card(p), null);
  assert.equal(await p.evaluate(() => document.activeElement?.id), 'game');
  // nobody yet
  await server(p, { ...list, friends: [] });
  await server(p, { op: 'where', friends: [] });
  await p.keyboard.press('KeyI');
  assert.equal(await card(p), 'Message a friend Touch front bumpers in free roam for 3 s to add a player. Esc closes');
  await p.keyboard.press('Escape');
  // not in a text field, not with a modifier
  await p.focus('#field');
  await p.keyboard.press('KeyI');
  assert.equal(await card(p), null);
  assert.equal(await p.$eval('#field', e => e.value), 'i');
  await p.focus('#game');
  await p.keyboard.press('Control+KeyI');
  assert.equal(await card(p), null);
  // a message the link could not take; one the server never answered
  await server(p, { op: 'mail', from: 'Bo_', body: 'yo', at: 3 });
  await p.keyboard.press('KeyI');
  await p.evaluate(() => { /** @type {any} */ (window).answer = false; });
  await p.keyboard.type('a');
  await p.keyboard.press('Enter');
  assert.deepEqual(await take(p, 'toasts'), ['The message was not sent: try again in a moment']);
  await p.keyboard.press('Escape');
  await p.evaluate(() => { /** @type {any} */ (window).answer = true; });
  await p.keyboard.press('KeyI');
  await p.keyboard.type('b');
  await p.keyboard.press('Enter');
  await wait(p, 10_400);
  assert.deepEqual(await take(p, 'toasts'), ['The message was not sent: try again in a moment'], 'no answer in 10 s');
  await p.close();
});

test('in the game on a touch screen: the sheet\'s Message a friend, a tap on a friend, Send', async () => {
  const p = await page();
  await server(p, list);
  await server(p, where);
  await p.evaluate(() => /** @type {any} */ (window).msgs.pickByTouch());
  assert.equal(await p.$eval('.msg-card', c => c.dataset.mode), 'touch');
  assert.deepEqual(await p.$$eval('.msg-card button.msg-friend', bs => bs.map(b => b.dataset.friend)), ['Bo_', 'Cy_', 'Gus'], 'rows to tap, no keys');
  await p.click('.msg-card button[data-friend="Gus"]');
  assert.match(await card(p), /^Message Gus Send 120$/);
  await p.fill('.msg-input', 'later');
  await p.click('.msg-send');
  assert.deepEqual((await take(p, 'sent')).at(-1), { op: 'msg', name: 'Gus', body: 'later', id: 1 });
  assert.equal(await card(p), null);
  await p.close();
});

test('the conversations: per claimed name, in this browser', async () => {
  const p = await page();
  const r = await p.evaluate(() => {
    const w = /** @type {any} */ (window), s = new w.M.MessageStore();
    s.setMe('Al_');
    s.add('Bo_', { dir: 'in', body: 'hi', t: 1 }, true);
    s.add('bo_', { dir: 'out', body: 'yo', t: 2, st: 'wait' }, false);
    s.mark('Bo_', 2, 'sent');
    const unread = s.unread('BO_');
    const again = new w.M.MessageStore();
    again.setMe('al_');
    const kept = again.messages('Bo_').map(m => `${m.dir} ${m.body} ${m.st ?? ''}`.trim());
    again.read('Bo_');
    // a message ahead of the name: kept under it once known
    const early = new w.M.MessageStore();
    early.add('Cy_', { dir: 'in', body: 'early', t: 3 }, true);
    early.setMe('Al_');
    const earlyKept = [early.messages('cy_').length, early.unread('Cy_'), early.messages('bo_').length];
    const other = new w.M.MessageStore();
    other.setMe('Zed');
    for (let i = 0; i < 45; i++) other.add('Bo_', { dir: 'in', body: `m${i}`, t: i }, false);
    return { unread, kept, read: again.unread('Bo_'), last: again.lastIn('Bo_')?.body, other: other.messages('Bo_').length, zed: other.messages('Bo_')[0].body, earlyKept };
  });
  assert.deepEqual(r, { unread: 1, kept: ['in hi', 'out yo sent'], read: 0, last: 'hi', other: 40, zed: 'm5', earlyKept: [1, 1, 2] });
  await p.close();
});

test('the start page\'s conversation', async () => {
  const p = await page();
  await p.evaluate(() => {
    const w = /** @type {any} */ (window);
    w.store.setMe('Al_');
    w.store.add('Bo_', { dir: 'in', body: 'hi there', t: Date.now() - 60_000 }, true);
    w.done = 'kept'; w.asked = [];
    w.chat = new w.M.ChatDialog(w.store, { send: async (name, body) => { w.asked.push([name, body]); return w.done; }, log: () => {} });
    w.chat.open('Bo_');
  });
  const log = () => p.$$eval('#chat-log .chat-msg', ms => ms.map(m => `${m.querySelector('b').textContent}: ${m.querySelector('p').textContent}${m.querySelector('.chat-head span').textContent.includes('·') ? ` (${m.querySelector('.chat-head span').textContent.split('· ')[1]})` : ''}`));
  assert.deepEqual(await log(), ['Bo_: hi there']);
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).store.unread('Bo_')), 0, 'read once open');
  await p.fill('#chat-text', '  see you   soon ');
  assert.equal(await p.$eval('#chat-left', e => e.textContent), '103');
  await p.press('#chat-text', 'Enter');
  await wait(p, 50);
  assert.deepEqual(await p.evaluate(() => /** @type {any} */ (window).asked.splice(0)), [['Bo_', 'see you soon']]);
  assert.deepEqual(await log(), ['Bo_: hi there', 'You: see you soon (gets it when back)']);
  assert.equal(await p.$eval('#chat-text', e => e.value), '');
  await p.evaluate(() => { /** @type {any} */ (window).done = 'none'; });
  await p.fill('#chat-text', 'hello?');
  await p.click('#chat-send');
  await wait(p, 50);
  assert.equal(await p.$eval('#chat-note', e => e.textContent), 'Bo_ is not on your friends list');
  assert.deepEqual((await log()).at(-1), 'You: hello? (not sent)');
  // a friend's message while it is open: shown, and read
  await p.evaluate(() => /** @type {any} */ (window).store.add('Bo_', { dir: 'in', body: 'still here', t: Date.now() }, true));
  assert.deepEqual((await log()).at(-1), 'Bo_: still here');
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).store.unread('Bo_')), 0);
  await p.close();
});
