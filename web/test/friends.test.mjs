// Free roam's friends on the page (web/src/app/friends.ts), in a real browser (web/tools/browser.mjs):
//   node --test web/test/friends.test.mjs
// - the line over the game: nothing until the server says what touching that player may lead to, then "Add Bo as a
//   friend" (F, a click on the line, or the page menu's button), "Bo wants to be friends: accept", the request sent,
//   friends, or why not (no claimed name here or there); gone when the cars part, the name over the car and the
//   claimed name being one player; F only while the line offers something, never in a text field, nor while the
//   player has one of the game's controls on F (the game's status "keys": the line still takes a click)
// - the room's name to the server once there are friends, and again when the room changes
// - where the friends play, asked every minute while there are some: a notice when one comes into free roam (none for
//   one just met here); the page menu's Friends: the ones in free roam with a Join (none in the player's own
//   room), the others in a line
// - the start page's Friends: a line each, the requests to answer, each friend where it plays with a Join in free roam
//   (its room), the requests sent; Accept, Decline, Remove (asked first), Take back; a keyphrase without a claimed
//   name; no keyphrase; a friend's Message, lime while messages wait to be read (the ones that waited come with the
//   list); a friend's Discord icon copies its username, a toast at it says so (or shows the name where the browser
//   would not copy), there and in the page menu's Friends
//   past 20 lines in pages (‹ 1 of 3 › under the list) with Filter on the row's right; the page menu's Friends
//   names 10 a line in the Online lobby and offline, then how many more
// - a friend on a race track (tracks.ts): its world as its room's hash has it (the main world's tracks too, whatever
//   name came with it; none for a name that leads elsewhere), the track in its notice (Join goes there) and in the
//   lists, a race invite only to one where the player drives
// - the start page's Your Discord: the server's username in it; one typed is sent as Discord allows one (discordName),
//   else refused on the page; what the server refused; none takes it away
// - race invites: an Invite in the page menu's Friends for each friend in free roam in the player's room while the
//   player hosts a race lobby with room in it, what the server said of it; a friend's invite: its line, the lobby key
//   (Z) or a click joins its lobby (the game's LB_JOIN with the host's id, the lobby's and its race's hash)
// - crew invites: a crew icon on a friend in free roam (the start page's Friends, the page menu's Friends) while the
//   player has a crew; one crew a click invites to it, more open under the line and a click on one invites to that;
//   what the server said in a toast at the icon.  A friend's invite: its card under a race invite's, Z (when no race
//   invite shows) or a click joins its crew in its room; it waits in the page menu's Crews (Join, Decline) until the
//   game ends; none for a crew the player is in, or names no Crews box would take
// - the Crews control (settings-ui.ts) says why a name cannot join, nothing more
// - /friends asked with the keyphrase's token, its answer read leniently
// The module is bundled from web/src with esbuild into a blank page.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { chromium } from 'playwright';
import { pickBrowser } from '../tools/browser.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const js = (await esbuild.build({
  stdin: { contents: `export * from './web/src/app/friends.ts'; export * from './web/src/app/messages.ts'; export { crewControl } from './web/src/app/settings-ui.ts'; export { trackRoom } from './web/src/app/tracks.ts'; export { roomHash } from './web/src/app/players.ts';`, resolveDir: ROOT, loader: 'ts' },
  bundle: true, format: 'iife', globalName: 'F', write: false, platform: 'browser',
})).outputFiles[0].text;

const browser = await chromium.launch({ ...pickBrowser(), headless: true });
test.after(() => browser.close());

/** a blank page with the module, a stage and the Friends box, and a FriendPrompt with deps that record what it does */
async function page() {
  const p = await browser.newPage();
  await p.setContent('<div id="stage" style="position:relative;width:640px;height:480px"><div class="toasts"></div></div><div id="friends" hidden></div><input id="field">');
  await p.addScriptTag({ content: js });
  await p.evaluate(() => {
    const w = /** @type {any} */ (window);
    w.sent = []; w.toasts = []; w.notices = []; w.placed = 0; w.columned = 0; w.room = ''; w.spot = 0; w.hosted = null; w.joined = [];
    // /friends (its answers and requests by name): what it was asked, and the lists it answers with (null: no answer)
    w.apiActs = []; w.apiLists = { me: 'Al_', friends: [], asks: [], sent: [], mail: [], done: 'friends' }; w.driving = true;
    w.crews = []; w.crewJoins = []; w.crewsChanged = 0; w.crewAsked = []; w.crewDone = 'sent';
    const api = {
      act: async (op, name) => { w.apiActs.push([op, name]); return w.apiLists; },
      crew: async (name, crew, room) => { w.crewAsked.push([name, crew, room]); return w.crewDone ? { ...w.apiLists, done: w.crewDone } : null; },
    };
    w.prompt = new w.F.FriendPrompt(document.getElementById('stage'), {
      send: m => { w.sent.push(m); return true; }, room: () => w.room, spot: () => w.spot, log: () => {}, place: () => { w.columned++; },
      toast: (s, o) => w.toasts.push(o?.hot ? `[hot] ${s}` : s),
      notice: (name, text, here) => w.notices.push(here ? [name, text, 'here'] : [name, text]), menu: () => 'the menu (F9)', placed: () => { w.placed++; },
      hosted: () => w.hosted, joinLobby: (host, lobby, race) => { w.joined.push([host, lobby, race]); return true; },
      api, driving: () => w.driving,
      crews: () => w.crews, joinCrew: (crew, room) => w.crewJoins.push([crew, room]), crewsChanged: () => { w.crewsChanged++; },
    });
    w.prompt.start({ viewRect: () => ({ x: 0, y: 0, w: 640, h: 480 }) }, true);
  });
  return p;
}
const line = p => p.evaluate(() => { const b = document.querySelector('.friend-prompt:not(.invite)'); return b.hidden ? null : { text: b.textContent, offer: b.classList.contains('offer') }; });
const sent = p => p.evaluate(() => /** @type {any} */ (window).sent.splice(0));
const toasts = p => p.evaluate(() => /** @type {any} */ (window).toasts.splice(0));
const bump = (p, name, on) => p.evaluate(([n, o]) => /** @type {any} */ (window).prompt.bump(JSON.stringify({ id: 7, name: n, on: o })), [name, on]);
const server = (p, m) => p.evaluate(x => /** @type {any} */ (window).prompt.server({ t: 'friend', ...x }), m);
const raf = p => p.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
const notices = p => p.evaluate(() => /** @type {any} */ (window).notices.splice(0));

test('the line over the game: asked, offered, sent, friends, parted', async () => {
  const p = await page();
  await raf(p);
  assert.deepEqual(await sent(p), [{ op: 'list' }], 'the list asked once the link is up');
  await bump(p, 'Bo', 1);
  assert.deepEqual(await sent(p), [{ op: 'can', name: 'Bo' }], 'the server asked about the player the car touched');
  assert.equal(await line(p), null, 'nothing shown until it answers');
  await server(p, { op: 'can', name: 'Bo_', st: 'new' });
  assert.deepEqual(await line(p), { text: 'FAdd Bo_ as a friend', offer: true });
  assert.equal((await p.evaluate(() => /** @type {any} */ (window).prompt.menuItem()?.label)), 'Add Bo_ as a friend', 'the page menu offers it too');
  // F in a text field is the field's
  await p.focus('#field');
  await p.keyboard.press('KeyF');
  assert.deepEqual(await sent(p), []);
  assert.equal(await p.inputValue('#field'), 'f');
  await p.evaluate(() => document.activeElement.blur());
  await p.keyboard.press('KeyF');
  assert.deepEqual(await sent(p), [{ op: 'add', name: 'Bo_' }], 'F asks');
  assert.equal(await line(p), null, 'waiting for the answer');
  await server(p, { op: 'add', name: 'Bo_', done: 'asked' });
  assert.deepEqual(await line(p), { text: 'Friend request sent to Bo_', offer: false });
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).prompt.menuItem()), null, 'nothing in the menu now');
  await p.keyboard.press('KeyF');
  assert.deepEqual(await sent(p), [], 'F offers nothing now');
  // the other player accepts: the line says so (no note over it as well)
  await server(p, { op: 'friends', name: 'Bo_' });
  assert.deepEqual(await toasts(p), []);
  assert.deepEqual(await line(p), { text: 'You and Bo_ are friends', offer: false });
  // the cars part: the name over the car is the claimed name's player
  await bump(p, 'Bo', 0);
  assert.equal(await line(p), null);
  // a request answered once the cars parted: a note
  await server(p, { op: 'friends', name: 'Cy_' });
  assert.deepEqual(await toasts(p), ['You and Cy_ are friends now']);
  await p.close();
});

test('the other player asked first: accept on the line, by a click', async () => {
  const p = await page();
  await raf(p);                // the list goes first (at the first frame), not among the clicks' messages
  await bump(p, 'Cy Two', 1);
  await server(p, { op: 'can', name: 'Cy_Two', st: 'asks' });
  assert.deepEqual(await line(p), { text: 'FCy_Two wants to be friends: accept', offer: true });
  assert.deepEqual(await p.evaluate(() => [[...document.querySelector('.toasts').children].map(e => e.className),
    document.querySelector('.friend-prompt b')?.textContent, /** @type {any} */ (window).columned > 0]),
  [['friend-prompt offer invite', 'friend-prompt offer invite crew-invite', 'friend-prompt offer'], 'Cy_Two', true],
  'in the column of notes at the top, the invites first (a race\'s, a crew\'s); the name bold');
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).prompt.menuItem()?.label), 'Accept Cy_Two as a friend');
  await sent(p);
  await p.click('.friend-prompt:not(.invite)');
  assert.deepEqual(await sent(p), [{ op: 'add', name: 'Cy_Two' }], 'a click on the line');
  await server(p, { op: 'add', name: 'Cy_Two', done: 'friends' });
  assert.deepEqual(await toasts(p), [], 'the line says it');
  assert.deepEqual(await line(p), { text: 'You and Cy_Two are friends', offer: false });
  // the menu's button for another player's request while touching
  await bump(p, 'Cy Two', 0);
  await bump(p, 'Dee', 1);
  await server(p, { op: 'asks', name: 'Dee' });
  await p.evaluate(() => /** @type {any} */ (window).prompt.menuItem().act());
  assert.deepEqual((await sent(p)).slice(-1), [{ op: 'add', name: 'Dee' }], 'their request while the cars touch: the menu accepts');
  // a request while the cars do not touch: on the line once they part (from afar: test below)
  await server(p, { op: 'asks', name: 'Eve' });
  assert.deepEqual(await toasts(p), [], 'no note: the line has it');
  await bump(p, 'Dee', 0);
  await raf(p);
  assert.deepEqual(await line(p), { text: 'FEve wants to be friends: accept', offer: true });
  await p.close();
});

test('a request from afar: on the line once the player drives, answered on /friends', async () => {
  const p = await page();
  const acts = () => p.evaluate(() => /** @type {any} */ (window).apiActs.splice(0));
  const set = (k, v) => p.evaluate(([k, v]) => { /** @type {any} */ (window)[k] = v; }, [k, v]);
  const wait = ms => p.evaluate(ms => new Promise(r => setTimeout(r, ms)), ms);
  // the game's first list: the newest request that waited, once the player drives (not on a loading screen, a shop)
  await set('driving', false);
  await server(p, { op: 'list', me: 'Al_', friends: [], asks: ['Old', 'New'] });
  await raf(p);
  assert.equal(await line(p), null, 'not driving: it waits');
  await set('driving', true);
  await raf(p);
  assert.deepEqual(await line(p), { text: 'FNew wants to be friends: accept', offer: true });
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).prompt.menuItem()?.label), 'Accept New as a friend', 'the page menu offers it too');
  // F accepts, on /friends (the game's "add" takes only a player the car touched)
  await sent(p);
  await p.keyboard.press('KeyF');
  await wait(20);
  assert.deepEqual(await acts(p), [['accept', 'New']]);
  assert.ok(!(await sent(p)).some(m => m.op === 'add'), 'not on the link');
  assert.deepEqual(await toasts(p), ['You and New are friends now']);
  assert.equal(await line(p), null, 'answered: gone');
  // a later list does not bring the older one back to the line (it waits in the menu's Friends)
  await server(p, { op: 'list', me: 'Al_', friends: ['New'], asks: ['Old'] });
  await raf(p);
  assert.equal(await line(p), null);
  // one asked while the game runs: at once while driving; gone when the player stops driving (a race, a shop)
  await server(p, { op: 'asks', name: 'Eve' });
  await raf(p);
  assert.deepEqual(await line(p), { text: 'FEve wants to be friends: accept', offer: true });
  await set('driving', false);
  await raf(p);
  assert.equal(await line(p), null);
  await set('driving', true);
  await raf(p);
  assert.equal(await line(p), null, 'not again: it waits in the menu\'s Friends');
  // a click on the line; no answer from the server: said, and the request still waits
  await server(p, { op: 'asks', name: 'Fay' });
  await raf(p);
  await set('apiLists', null);
  await p.click('.friend-prompt:not(.invite)');
  await wait(20);
  assert.deepEqual(await acts(p), [['accept', 'Fay']]);
  assert.deepEqual(await toasts(p), ['Not sent: try again in a moment']);
  const asks = await p.evaluate(() => { const box = document.createElement('div'); /** @type {any} */ (window).prompt.drawPlaced(box, () => {});
    return [...box.querySelectorAll('.fl li .fl-name')].map(e => e.firstChild.textContent); });
  assert.deepEqual(asks, ['Old', 'Eve', 'Fay'], 'still in the menu\'s Friends');
  // the other one's friendship takes its request off the line
  await set('apiLists', { me: 'Al_', friends: [], asks: [], sent: [], mail: [], done: 'friends' });
  await server(p, { op: 'asks', name: 'Gil' });
  await raf(p);
  await server(p, { op: 'friends', name: 'Gil' });
  assert.equal(await line(p), null);
  await p.close();
});

test('the page menu\'s Friends with no friends yet: how to add one, the requests, a request by name', async () => {
  const p = await page();
  const set = (k, v) => p.evaluate(([k, v]) => { /** @type {any} */ (window)[k] = v; }, [k, v]);
  const pop = () => p.evaluate(() => {
    const w = /** @type {any} */ (window);
    let box = document.getElementById('pop');
    if (!box) { box = document.createElement('div'); box.id = 'pop'; document.body.append(box); }
    const joins = w.prompt.drawPlaced(box, () => {});
    return { joins, rows: [...box.querySelectorAll('.fl li')].map(li => [li.querySelector('.fl-name').firstChild.textContent, li.querySelector('.fl-st').textContent,
      [...li.querySelectorAll('button')].map(b => b.getAttribute('aria-label')).join('|')]),
      lines: [...box.querySelectorAll(':scope > p.hint')].map(e => e.textContent), field: !!box.querySelector('.fl-add input'),
      note: box.querySelector('.fl-note')?.textContent ?? null };
  });
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).prompt.available()), true, 'free roam with others: the menu has Friends');
  assert.deepEqual(await pop(), { joins: false, rows: [], lines: ['Checking.'], field: false, note: null }, 'not listed yet');
  await server(p, { op: 'list', me: null, friends: [], asks: [] });
  assert.deepEqual(await pop(), { joins: false, rows: [], lines: ['Claim your name on the start page to add friends'], field: false, note: null });
  await server(p, { op: 'list', me: 'Al_', friends: [], asks: ['Dee'] });
  assert.deepEqual(await pop(), { joins: false, rows: [['Dee', 'wants to be friends', 'Accept|Decline']],
    lines: ['Touch front bumpers in free roam for 3 s to add a player.'], field: true, note: '' });
  await set('apiLists', { me: 'Al_', friends: [], asks: [], sent: [], mail: [], done: 'ok' });
  await p.click('#pop .fl-no');
  await p.evaluate(() => new Promise(r => setTimeout(r, 20)));
  assert.deepEqual(await p.evaluate(() => /** @type {any} */ (window).apiActs.splice(0)), [['decline', 'Dee']]);
  assert.deepEqual((await pop()).rows, [], 'declined: gone');
  // a request by name, from the game: the field and its note as the start page's
  await set('apiLists', { me: 'Al_', friends: [], asks: [], sent: ['Zed'], mail: [], done: 'asked' });
  await p.fill('#pop .fl-add input', 'zed');
  await p.press('#pop .fl-add input', 'Enter');
  await p.evaluate(() => new Promise(r => setTimeout(r, 20)));
  assert.deepEqual(await p.evaluate(() => /** @type {any} */ (window).apiActs.splice(0)), [['ask', 'zed']]);
  assert.equal((await pop()).note, 'Friend request sent to Zed');
  await set('apiLists', { me: null, friends: [], asks: [], sent: [], mail: [], done: 'claim' });
  await p.fill('#pop .fl-add input', 'zed');
  await p.press('#pop .fl-add input', 'Enter');
  await p.evaluate(() => new Promise(r => setTimeout(r, 20)));
  assert.deepEqual((await pop()).lines, ['Claim your name on the start page to add friends'], 'its claim gone meanwhile');
  // typing goes on over a redraw (the server's answers redraw the menu's Friends)
  await server(p, { op: 'list', me: 'Al_', friends: [], asks: [] });
  await pop();
  await p.fill('#pop .fl-add input', 'Bo');
  await p.focus('#pop .fl-add input');
  await server(p, { op: 'asks', name: 'Cy_' });
  await pop();
  await p.keyboard.type('b');
  assert.equal(await p.$eval('#pop .fl-add input', i => i.value), 'Bob');
  await p.close();
});

test('one of the game\'s controls on F (the game\'s status "keys"): F is the game\'s, the line still adds', async () => {
  const p = await page();
  // F's bit in the status (runtime/dinput_kbd.c page_keys: bit 1)
  await p.evaluate(() => { const w = /** @type {any} */ (window); w.keys = 2;
    w.prompt.start({ viewRect: () => ({ x: 0, y: 0, w: 640, h: 480 }), status: { read: () => new Map([['keys', w.keys]]) } }, true); });
  await raf(p);
  await bump(p, 'Bo', 1);
  await server(p, { op: 'can', name: 'Bo_', st: 'new' });
  assert.deepEqual(await line(p), { text: 'Add Bo_ as a friend', offer: true }, 'no F on the line');
  await sent(p);
  await p.keyboard.press('KeyF');
  assert.deepEqual(await sent(p), [], 'F is the game\'s');
  await p.click('.friend-prompt:not(.invite)');
  assert.deepEqual(await sent(p), [{ op: 'add', name: 'Bo_' }], 'a click on the line still asks');
  await p.close();
});

test('why not, and what is no answer to the touch', async () => {
  const p = await page();
  for (const [st, text] of [['claim', 'Claim your name on the start page to add friends'], ['theirs', 'Bo_ has no claimed name, so can\'t add friends yet']]) {
    await bump(p, 'Bo', 1);
    await server(p, { op: 'can', name: 'Bo_', st });
    assert.deepEqual(await line(p), { text, offer: false }, st);
    await p.keyboard.press('KeyF');
    assert.ok(!(await sent(p)).some(m => m.op === 'add'), `${st}: F does nothing`);
    await bump(p, 'Bo', 0);
  }
  await bump(p, 'Bo', 1);
  await server(p, { op: 'can', name: 'Bo_', st: 'far' });
  assert.equal(await line(p), null, 'not linked: nothing');
  await bump(p, 'Bo', 1);
  await server(p, { op: 'can', name: 'Someone', st: 'new' });
  assert.equal(await line(p), null, 'an answer about another player');
  await server(p, { op: 'add', name: 'Bo_', done: 'full' });
  assert.deepEqual(await toasts(p), ['A friends list is full (250 at most)']);
  // friends off (no free roam with others): the game's word is not taken
  await p.evaluate(() => { const w = /** @type {any} */ (window); w.prompt.stop(); w.prompt.start({ viewRect: () => ({ x: 0, y: 0, w: 640, h: 480 }) }, false); });
  await sent(p);
  await bump(p, 'Bo', 1);
  assert.deepEqual(await sent(p), []);
  assert.equal(await line(p), null);
  await p.close();
});

test('the room\'s name goes to the server once there are friends', async () => {
  const p = await page();
  await p.evaluate(() => { /** @type {any} */ (window).room = 'Night Owls'; });
  await server(p, { op: 'list', me: 'Al_', friends: [], asks: [] });
  assert.ok(!(await sent(p)).some(m => m.t === 'room'), 'no friends: not told');
  await server(p, { op: 'list', me: 'Al_', friends: ['Bo_'], asks: [] });
  assert.deepEqual((await sent(p)).filter(m => m.t === 'room'), [{ t: 'room', name: 'Night Owls' }]);
  await server(p, { op: 'list', me: 'Al_', friends: ['Bo_', 'Cy_'], asks: [] });
  assert.deepEqual((await sent(p)).filter(m => m.t === 'room'), [], 'once');
  await p.evaluate(() => { const w = /** @type {any} */ (window); w.room = ''; w.prompt.roomChanged(); });
  assert.deepEqual((await sent(p)).filter(m => m.t === 'room'), [{ t: 'room', name: '' }], 'again in another room (the room of everyone: "")');
  // the link back after a restart: the list asked again, the room told again (and where the friends are)
  await p.evaluate(() => /** @type {any} */ (window).prompt.online());
  await raf(p);
  assert.deepEqual((await sent(p)).filter(m => m.op !== 'where'), [{ op: 'list' }]);
  await server(p, { op: 'list', me: 'Al_', friends: ['Bo_'], asks: [] });
  assert.deepEqual((await sent(p)).filter(m => m.t === 'room'), [{ t: 'room', name: '' }]);
  await p.close();
});

test('where the friends play: notices, and the page menu\'s Friends', async () => {
  const p = await page();
  await raf(p);
  await sent(p);
  await p.evaluate(() => { /** @type {any} */ (window).room = 'Night Owls'; });
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).prompt.hasFriends()), false, 'no friends listed yet: no Friends in the menu');
  await server(p, { op: 'list', me: 'Al_', friends: ['Bo_', 'Cy_', 'Dee', 'Eve', 'Fox'], asks: [] });
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).prompt.hasFriends()), true);
  assert.equal(await p.evaluate(() => { const w = /** @type {any} */ (window), n = w.placed; w.placed = 0; return n; }), 1, 'the menu\'s Friends drawn again with the list');
  await raf(p);
  assert.deepEqual((await sent(p)).filter(m => m.op === 'where'), [{ op: 'where' }], 'asked with the next frame once listed');
  await raf(p);
  assert.deepEqual((await sent(p)).filter(m => m.op === 'where'), [], 'then not again before a minute');
  const where = [
    { name: 'Bo_', where: 'roam', room: 'Blue Room' }, { name: 'Cy_', where: 'roam', room: 'night owls', contact: 'cy.races' },
    { name: 'Dee', where: 'roam', room: '' }, { name: 'Eve', where: 'online' }, { name: 'Fox', where: '' },
  ];
  await server(p, { op: 'where', friends: where });
  assert.deepEqual(await notices(p), [
    ['Bo_', 'Your friend Bo_ is in free roam in "Blue Room": Join in the menu (F9)'],
    ['Cy_', 'Your friend Cy_ is in free roam', 'here'],
    ['Dee', 'Your friend Dee is in free roam in Main world: Join in the menu (F9)'],
  ], 'the ones in free roam, at the first answer too (a room as the game hashes it: case and blanks do not count)');
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).placed), 1, 'the menu\'s Friends drawn again');
  await server(p, { op: 'where', friends: where });
  assert.deepEqual(await notices(p), [], 'still there: no notice again');
  // Eve comes into free roam; Bo leaves and comes back; Fox is befriended here first (no notice for one just met)
  await server(p, { op: 'where', friends: [where[0], where[1], where[2], { name: 'Eve', where: 'roam' }, { name: 'Fox', where: '' }] });
  assert.deepEqual(await notices(p), [['Eve', 'Your friend Eve is in free roam', 'here']], 'a room its page did not tell: no Join');
  await server(p, { op: 'where', friends: [{ name: 'Bo_', where: 'online' }, where[1], where[2], where[3], where[4]] });
  await server(p, { op: 'friends', name: 'Fox' });
  await server(p, { op: 'where', friends: [...where.slice(0, 4), { name: 'Fox', where: 'roam', room: 'night owls' }] });
  assert.deepEqual(await notices(p), [['Bo_', 'Your friend Bo_ is in free roam in "Blue Room": Join in the menu (F9)']]);
  // the menu's Friends: in free roam (where: the world's name, or your world), a Join for each one in another room, a Discord icon for one that gave its name (copied:
  // a toast at it says so); the others in a line
  const pop = () => p.evaluate(async () => {
    const w = /** @type {any} */ (window), box = document.createElement('div');
    w.joins = []; w.copies = [];
    Object.defineProperty(navigator, 'clipboard', { configurable: true, value: { writeText: async s => { w.copies.push(s); } } });
    document.body.append(box);
    w.prompt.drawPlaced(box, f => w.joins.push(f.room));   // (a game without trips: Join for one in another world, its world)
    const rows = [...box.querySelectorAll('.fl li')].map(li => [li.querySelector('.fl-name').firstChild.textContent, li.querySelector('.fl-st').textContent,
      [...li.querySelectorAll('button')].map(b => b.getAttribute('aria-label')).join('|')]);
    for (const b of box.querySelectorAll('button')) b.click();
    await new Promise(r => setTimeout(r, 20));
    const toasts = [...document.querySelectorAll('.copy-toast')].map(e => e.textContent);
    box.remove();
    return { rows, lines: [...box.querySelectorAll(':scope > p.hint')].map(e => e.textContent), joins: w.joins, copies: w.copies, toasts };
  });
  assert.deepEqual(await pop(), {
    rows: [['Bo_', 'Blue Room', 'Join'], ['Cy_', 'your world', 'Copy Discord name cy.races'], ['Dee', 'Main world', 'Join'],
      ['Fox', 'your world', '']],
    lines: ['In the Online lobby: Eve.'], joins: ['Blue Room', ''], copies: ['cy.races'], toasts: ['Copied cy.races'],
  });
  // a game that goes to friends (runtime/travel.c): a Join on each one in free roam, in the player's world too
  const goes = await p.evaluate(() => {
    const w = /** @type {any} */ (window), box = document.createElement('div'), joins = [];
    document.body.append(box);
    w.prompt.drawPlaced(box, f => joins.push(f.name), true);
    for (const b of box.querySelectorAll('button[aria-label="Join"]')) b.click();
    box.remove();
    return joins;
  });
  assert.deepEqual(goes, ['Bo_', 'Cy_', 'Dee', 'Fox']);
  await server(p, { op: 'where', friends: [{ name: 'Bo_', where: 'online' }, { name: 'Cy_', where: '' }, { name: 'Dee', where: '' }] });
  await p.evaluate(() => new Promise(r => setTimeout(r, 4400)));
  assert.deepEqual(await pop(), {
    rows: [], lines: ['None of your friends is in free roam now.', 'In the Online lobby: Bo_. Offline: Cy_, Dee.'], joins: [], copies: [], toasts: [],
  }, 'the toast gone after a moment');
  // asked again when the menu's Friends opens
  await p.evaluate(() => /** @type {any} */ (window).prompt.askWhere());
  assert.deepEqual((await sent(p)).filter(m => m.op === 'where'), [{ op: 'where' }]);
  await p.close();
});

test('friends on race tracks: their world, the track in the notice and the menu, Join there, race invites', async () => {
  const p = await page();
  await raf(p);
  await sent(p);
  await server(p, { op: 'list', me: 'Al_', friends: ['Gus', 'Hal', 'Ivy', 'Jo'], asks: [] });
  // Gus on the main world's Stadium drift (a server of before named that world "player"), Hal on World 2's Street X
  // Parkade, Ivy in World 3 under the name of a world it left, Jo in the main world's city
  const where = await p.evaluate(() => {
    const F = /** @type {any} */ (window).F, w2 = F.roomHash('World 2');
    return [
      { name: 'Gus', where: 'roam', room: 'player', at: F.trackRoom(0, 4311) },
      { name: 'Hal', where: 'roam', room: 'World 2', at: F.trackRoom(w2, 4605) },
      { name: 'Ivy', where: 'roam', room: 'Blue Room', at: F.roomHash('World 3') },
      { name: 'Jo', where: 'roam', room: '', at: 0 },
    ];
  });
  await server(p, { op: 'where', friends: where });
  assert.deepEqual(await notices(p), [
    ['Gus', 'Your friend Gus is at Drift: Stadium. Join in the menu (F9)', 'here'],
    ['Hal', 'Your friend Hal is at Street X: Parkade in World 2. Join in the menu (F9)'],
    ['Ivy', 'Your friend Ivy is in free roam', 'here'],
    ['Jo', 'Your friend Jo is in free roam', 'here'],
  ], 'the main world from its hash, not a world named "player"; a world name that leads elsewhere: none');
  const pop = () => p.evaluate(() => {
    const w = /** @type {any} */ (window), box = document.createElement('div'), joins = [];
    document.body.append(box);
    w.prompt.drawPlaced(box, f => joins.push([f.name, f.room ?? null, f.at]), true);
    const rows = [...box.querySelectorAll('.fl li')].map(li => [li.querySelector('.fl-name').firstChild.textContent, li.querySelector('.fl-st').textContent,
      [...li.querySelectorAll('button')].map(b => b.getAttribute('aria-label')).join('|')]);
    for (const b of box.querySelectorAll('button[aria-label="Join"]')) b.click();
    box.remove();
    return { rows, joins };
  });
  const { rows, joins } = await pop();
  assert.deepEqual(rows, [
    ['Gus', 'your world · Drift: Stadium', 'Join'], ['Hal', 'World 2 · Street X: Parkade', 'Join'], ['Ivy', 'free roam', 'Join'],
    ['Jo', 'your world', 'Join'],
  ]);
  assert.deepEqual(joins.map(j => j.slice(0, 2)), [['Gus', ''], ['Hal', 'World 2'], ['Ivy', null], ['Jo', '']],
    'Join takes the world the friend is in (none: the game looks for it where the player is)');
  // hosting a race: an Invite for the friend where the player drives, the city or a track (the server's hub links
  // only those)
  await p.evaluate(() => { /** @type {any} */ (window).hosted = { kind: 'sprint', stage: 2, n: 1, max: 6 }; });
  const invites = async () => (await pop()).rows.filter(r => r[2].includes('Invite')).map(r => r[0]);
  assert.deepEqual(await invites(), ['Jo'], 'in the city: Jo, not Gus on its track');
  await p.evaluate(() => { /** @type {any} */ (window).spot = 3; });
  assert.deepEqual(await invites(), ['Gus'], 'on the Stadium drift: Gus');
  // the start page's line for one (as /friends's answer is read: Gus in the main world, Ivy's world none)
  const lines = await p.evaluate(ws => ws.map(f => /** @type {any} */ (window).F.whereText(f)),
    where.map(f => f.name === 'Gus' ? { ...f, room: '' } : f.name === 'Ivy' ? { name: f.name, where: f.where, at: f.at } : f));
  assert.deepEqual(lines, ['free roam · Drift: Stadium', 'free roam · World 2 · Street X: Parkade', 'free roam', 'free roam']);
  await p.close();
});

/** a FriendsPanel in the page with an api that answers `lists` and records what it is asked */
async function panel(p, lists) {
  await p.evaluate(l => {
    const w = /** @type {any} */ (window);
    w.acts = []; w.joined = []; w.chats = []; w.lists = l; w.confirmYes = true; w.discords = []; w.discordDone = 'ok';
    w.crewAsked = []; w.crewDone = 'sent';
    const api = {
      list: async () => w.lists, act: async (op, name) => { w.acts.push([op, name]); return w.lists; },
      crew: async (name, crew, room) => { w.crewAsked.push([name, crew, room]); return w.crewDone ? { ...w.lists, done: w.crewDone } : null; },
      discord: async name => {
        w.discords.push(name);
        if (w.discordDone === 'ok') w.lists = { ...w.lists, contact: name || null };
        return { ...w.lists, done: w.discordDone };
      },
    };
    w.store ??= new w.F.MessageStore();
    w.waiting = [];
    w.panel = new w.F.FriendsPanel(document.getElementById('friends'), {
      api, join: room => w.joined.push(room), confirm: async () => w.confirmYes, idle: () => true, log: () => {},
      store: w.store, chat: name => w.chats.push(name), waiting: n => w.waiting.push(n),
      crews: () => w.panelCrews ? { crews: w.panelCrews, room: w.panelRoom ?? '' } : null,
    });
  }, lists);
  await p.evaluate(() => /** @type {any} */ (window).panel.refresh(true));
}
const rows = p => p.$$eval('#friends .fl li', rs => rs.map(r => [r.querySelector('.fl-name').firstChild.textContent, r.querySelector('.fl-st').textContent,
  [...r.querySelectorAll('button')].map(b => b.getAttribute('aria-label')).join('|'), r.className]));

test('race invites: Invite in the menu\'s Friends while hosting, and a friend\'s invite joined by Z or a click', async () => {
  const p = await page();
  await raf(p);
  await p.evaluate(() => { /** @type {any} */ (window).room = 'Night Owls'; });
  await server(p, { op: 'list', me: 'Al_', friends: ['Bo_', 'Cy_'], asks: [] });
  await server(p, { op: 'where', friends: [{ name: 'Bo_', where: 'roam', room: 'night owls' }, { name: 'Cy_', where: 'roam', room: 'Blue Room' }] });
  await notices(p); await sent(p);
  const rows = () => p.evaluate(() => {
    const w = /** @type {any} */ (window), box = document.createElement('div');
    document.body.append(box);
    w.prompt.drawPlaced(box, () => {});
    w.box = box;
    return [...box.querySelectorAll('.fl li')].map(li => [li.querySelector('.fl-name').firstChild.textContent, li.querySelector('.fl-st').textContent,
      [...li.querySelectorAll('button')].map(b => b.getAttribute('aria-label')).join('|')]);
  });
  const done = () => p.evaluate(() => /** @type {any} */ (window).box.remove());
  assert.deepEqual(await rows(), [['Bo_', 'your world', ''], ['Cy_', 'Blue Room', 'Join']], 'not hosting: no Invite');
  await done();
  await p.evaluate(() => { /** @type {any} */ (window).hosted = { kind: 'sprint', stage: 2, n: 1, max: 6 }; });
  assert.deepEqual(await rows(), [['Bo_', 'your world', 'Invite Bo_ to your race'], ['Cy_', 'Blue Room', 'Join']],
    'hosting: an Invite for a friend in our room only');
  await p.evaluate(() => /** @type {any} */ (window).box.querySelector('.fl-invite').click());
  await done();
  assert.deepEqual((await sent(p)).filter(m => m.op !== 'where'), [{ op: 'invite', name: 'Bo_', kind: 'sprint', stage: 2 }]);
  await server(p, { op: 'invite', name: 'Bo_', done: 'sent' });
  assert.deepEqual(await toasts(p), ['Invite sent to Bo_']);
  assert.deepEqual(await rows(), [['Bo_', 'your world · invited', ''], ['Cy_', 'Blue Room', 'Join']], 'invited: no Invite again for a while');
  await done();
  for (const [d, text] of [['away', 'Bo_ isn\'t in free roam in your world now'], ['full', 'Your race lobby is full'], ['nolobby', 'Open a race lobby first, then invite'],
    ['rate', 'Too many invites at once: try again in a moment'], ['none', 'Bo_ can\'t be invited now']]) {
    await server(p, { op: 'invite', name: 'Bo_', done: d });
    assert.deepEqual(await toasts(p), [text], d);
  }
  // a friend's invite: its line just above the friend line; Z (the race lobby card's key) joins, a click on it too
  const inv = () => p.evaluate(() => { const b = document.querySelector('.friend-prompt.invite'); return b.hidden ? null : b.textContent; });
  await server(p, { op: 'invite', name: 'Dee', host: 0x11223344, lobby: 0xa1b2c3d4, race: 0xdeadbeef, kind: 'drift', n: 2, max: 6 });
  assert.equal(await inv(), 'Dee invites you to a drift (2/6)ZJoin');
  await p.keyboard.press('KeyZ');
  assert.deepEqual(await p.evaluate(() => /** @type {any} */ (window).joined.splice(0)), [[0x11223344, 0xa1b2c3d4, 0xdeadbeef]]);
  assert.equal(await inv(), null, 'gone once taken');
  await p.keyboard.press('KeyZ');
  assert.deepEqual(await p.evaluate(() => /** @type {any} */ (window).joined.splice(0)), [], 'Z does nothing without one');
  await server(p, { op: 'invite', name: 'Dee', host: 1, lobby: 2, race: 3, kind: 'circuit', n: 1, max: 4 });
  await p.click('.friend-prompt.invite');
  assert.deepEqual(await p.evaluate(() => /** @type {any} */ (window).joined.splice(0)), [[1, 2, 3]], 'a click');
  await server(p, { op: 'invite', name: 'Dee', host: 0, lobby: 2, race: 3 });
  assert.equal(await inv(), null, 'an invite that names no lobby: none');
  await p.close();
});

/** the newest copy toast's text (copy.ts), after the server's answer came */
const toastNow = p => p.evaluate(async () => { await new Promise(r => setTimeout(r, 30)); return [...document.querySelectorAll('.copy-toast')].at(-1)?.textContent ?? null; });

test('crew invites: an icon on a friend in free roam, the crews to pick under its line, the server\'s answer at it', async () => {
  const p = await page();
  await raf(p);
  await p.evaluate(() => { /** @type {any} */ (window).room = 'Night Owls'; });
  await server(p, { op: 'list', me: 'Al_', friends: ['Bo_', 'Cy_'], asks: [] });
  await server(p, { op: 'where', friends: [{ name: 'Bo_', where: 'roam', room: 'night owls' }, { name: 'Cy_', where: 'online' }] });
  await notices(p); await sent(p);
  // the page menu's Friends, drawn again as the shell does when the prompt says so (placed)
  const draw = () => p.evaluate(() => {
    const w = /** @type {any} */ (window);
    w.box?.remove();
    const box = w.box = document.createElement('div');
    document.body.append(box);
    w.prompt.drawPlaced(box, () => {});
  });
  const rowsOf = sel => p.$$eval(sel, ls => ls.map(li => [li.classList.contains('fl-pick') ? '(under)' : li.querySelector('.fl-name').firstChild.textContent,
    li.querySelector('.fl-st').textContent, [...li.querySelectorAll('button')].map(b => b.getAttribute('aria-label')).join('|')]));
  const asked = () => p.evaluate(() => /** @type {any} */ (window).crewAsked.splice(0));
  await draw();
  assert.deepEqual(await rowsOf('body > div .fl li'), [['Bo_', 'your world', '']], 'no crew: no icon');
  await p.evaluate(() => { /** @type {any} */ (window).crews = [{ name: 'JDM', rgb: '3cd2ff' }]; });
  await draw();
  assert.deepEqual(await rowsOf('body > div .fl li'), [['Bo_', 'your world', 'Invite Bo_ to a crew']]);
  await p.click('.fl-crew');
  assert.deepEqual(await asked(), [['Bo_', 'JDM', 'Night Owls']], 'one crew: invited at once, in the game\'s room');
  assert.equal(await toastNow(p), 'Invited Bo_ to JDM');
  for (const [done, text] of [['away', 'Bo_ isn\'t in free roam now'], ['rate', 'Wait a moment'], ['none', 'Not sent: try again in a moment'], [null, 'Not sent: try again in a moment']]) {
    await p.evaluate(d => { /** @type {any} */ (window).crewDone = d; }, done);
    await p.click('.fl-crew');
    assert.equal(await toastNow(p), text, String(done));
  }
  await asked();
  await p.evaluate(() => { const w = /** @type {any} */ (window); w.crewDone = 'sent'; w.crews = [{ name: 'JDM', rgb: '3cd2ff' }, { name: 'Wangan', rgb: 'ff50c8' }]; w.placed = 0; });
  await draw();
  await p.click('.fl-crew');
  assert.deepEqual(await asked(), [], 'more crews: one to pick');
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).placed), 1, 'drawn again');
  await draw();
  assert.deepEqual(await rowsOf('body > div .fl li'), [['Bo_', 'your world', 'Invite Bo_ to a crew'], ['(under)', 'Invite Bo_ to', 'Invite Bo_ to crew JDM|Invite Bo_ to crew Wangan']]);
  assert.equal(await p.getAttribute('.fl-crew', 'aria-expanded'), 'true');
  assert.deepEqual(await p.$$eval('.crew-pick', bs => bs.map(b => [b.textContent, b.style.getPropertyValue('--crew')])), [['JDM', '#3cd2ff'], ['Wangan', '#ff50c8']], 'as the Crews row\'s chips');
  await p.click('.crew-pick >> nth=1');
  assert.deepEqual(await asked(), [['Bo_', 'Wangan', 'Night Owls']]);
  await draw();
  assert.deepEqual(await rowsOf('body > div .fl li'), [['Bo_', 'your world', 'Invite Bo_ to a crew']], 'closed once sent');
  assert.equal(await toastNow(p), 'Invited Bo_ to Wangan', 'at the icon');
  await p.evaluate(() => /** @type {any} */ (window).box.remove());
  // the start page's Friends: settings.ini's crews and room, on the friends in free roam only
  await p.evaluate(() => { document.getElementById('friends').hidden = false; });
  await panel(p, { me: 'Al_', asks: [], sent: [], friends: [{ name: 'Bo_', where: 'roam', room: '' }, { name: 'Gus', where: 'online' }] });
  assert.deepEqual(await rowsOf('#friends .fl li'), [['Bo_', 'free roam', 'Join|Message|Remove'], ['Gus', 'Online lobby', 'Message|Remove']], 'no crew: no icon');
  await p.evaluate(() => { const w = /** @type {any} */ (window); w.panelCrews = [{ name: 'JDM', rgb: '3cd2ff' }, { name: 'Wangan', rgb: 'ff50c8' }]; w.panelRoom = 'Blue Room'; w.panel.render(); });
  assert.deepEqual(await rowsOf('#friends .fl li'), [['Bo_', 'free roam', 'Join|Invite Bo_ to a crew|Message|Remove'], ['Gus', 'Online lobby', 'Message|Remove']]);
  await p.click('#friends .fl-crew');
  assert.deepEqual((await rowsOf('#friends .fl li'))[1], ['(under)', 'Invite Bo_ to', 'Invite Bo_ to crew JDM|Invite Bo_ to crew Wangan']);
  assert.equal(await p.evaluate(() => document.activeElement?.textContent), 'JDM', 'the keyboard on the first crew');
  await p.click('#friends .fl-crew');
  assert.equal((await rowsOf('#friends .fl li')).length, 2, 'the icon again closes them');
  await p.click('#friends .fl-crew');
  await p.click('#friends .crew-pick >> nth=0');
  assert.deepEqual(await asked(), [['Bo_', 'JDM', 'Blue Room']], 'settings.ini\'s room');
  assert.equal(await toastNow(p), 'Invited Bo_ to JDM');
  assert.equal(await p.evaluate(() => document.activeElement?.getAttribute('aria-label')), 'Invite Bo_ to a crew', 'the keyboard back on the icon');
  await p.close();
});

test('a friend\'s crew invite: its card, Z or a click joins its crew in its room, it waits in the menu\'s Crews', async () => {
  const p = await page();
  await raf(p);
  const card = () => p.evaluate(() => { const b = document.querySelector('.friend-prompt.crew-invite'); return b.hidden ? null : b.textContent; });
  const joins = () => p.evaluate(() => /** @type {any} */ (window).crewJoins.splice(0));
  const waiting = () => p.evaluate(() => /** @type {any} */ (window).prompt.crewAsksNow().map(a => [a.name, a.crew, a.room]));
  await server(p, { op: 'crew', name: 'Bo_', crew: 'Touge', room: 'Blue Room' });
  assert.equal(await card(), 'Bo_ invites you to crew "Touge"ZJoin');
  assert.equal(await p.evaluate(() => document.querySelector('.friend-prompt.crew-invite b')?.textContent), 'Bo_', 'the friend\'s name bold');
  assert.ok(await p.evaluate(() => /** @type {any} */ (window).crewsChanged) > 0, 'the menu\'s Crews told');
  assert.deepEqual(await waiting(), [['Bo_', 'Touge', 'Blue Room']]);
  await p.keyboard.press('KeyZ');
  assert.deepEqual(await joins(), [['Touge', 'Blue Room']], 'Z joins, in the friend\'s room');
  assert.equal(await card(), null);
  assert.deepEqual(await waiting(), []);
  await p.keyboard.press('KeyZ');
  assert.deepEqual(await joins(), [], 'Z does nothing without one');
  // the names tidied as a link's; a click; the room of everyone
  await server(p, { op: 'crew', name: 'Cy_', crew: '  Night   Owls ', room: '' });
  assert.equal(await card(), 'Cy_ invites you to crew "Night Owls"ZJoin');
  await p.click('.friend-prompt.crew-invite');
  assert.deepEqual(await joins(), [['Night Owls', '']], 'a click');
  // a race's invite with it: Z takes the race's (only while its lobby is open), the crew's stays
  await server(p, { op: 'crew', name: 'Dee', crew: 'Wangan', room: '' });
  await server(p, { op: 'invite', name: 'Dee', host: 1, lobby: 2, race: 3, kind: 'sprint', n: 1, max: 4 });
  assert.deepEqual(await p.evaluate(() => [...document.querySelectorAll('.toasts .friend-prompt:not([hidden])')].map(e => e.className)),
    ['friend-prompt offer invite', 'friend-prompt offer invite crew-invite'], 'the race\'s first');
  await p.keyboard.press('KeyZ');
  assert.deepEqual([await p.evaluate(() => /** @type {any} */ (window).joined.splice(0)), await joins()], [[[1, 2, 3]], []]);
  assert.equal(await card(), 'Dee invites you to crew "Wangan"ZJoin');
  // the page menu's Crews: a line each, Join and Decline
  await server(p, { op: 'crew', name: 'Eve', crew: 'Touge', room: 'Blue Room' });
  const crewsPop = () => p.evaluate(() => {
    const w = /** @type {any} */ (window), ul = w.prompt.drawCrewAsks();
    if (!ul) return null;
    document.body.append(ul); w.ul = ul;
    return [...ul.querySelectorAll('li')].map(li => [li.querySelector('.fl-name').firstChild.textContent, li.querySelector('.fl-st').textContent,
      [...li.querySelectorAll('button')].map(b => b.getAttribute('aria-label')).join('|')]);
  });
  assert.deepEqual(await crewsPop(), [['Dee', 'invites you to crew "Wangan"', 'Join|Decline'], ['Eve', 'invites you to crew "Touge"', 'Join|Decline']]);
  await p.evaluate(() => /** @type {any} */ (window).ul.querySelector('.fl-no').click());
  assert.deepEqual([await joins(), await waiting()], [[], [['Eve', 'Touge', 'Blue Room']]], 'Dee\'s declined');
  await p.evaluate(() => /** @type {any} */ (window).ul.remove());
  assert.equal(await card(), 'Eve invites you to crew "Touge"ZJoin', 'the newest one\'s card');
  await crewsPop();
  await p.evaluate(() => /** @type {any} */ (window).ul.querySelector('.fl-yes').click());
  assert.deepEqual([await joins(), await waiting(), await card()], [[['Touge', 'Blue Room']], [], null], 'Eve\'s joined, its card gone');
  assert.equal(await crewsPop(), null, 'none waits');
  // with 4 crews already: a note, and it waits (Leave is beside it in the menu's Crews)
  await p.evaluate(() => { /** @type {any} */ (window).crews = ['A', 'B', 'C', 'D'].map(name => ({ name, rgb: '3cd2ff' })); });
  await server(p, { op: 'crew', name: 'Fox', crew: 'Neon', room: '' });
  await toasts(p);
  await p.keyboard.press('KeyZ');
  assert.deepEqual([await joins(), await toasts(p), await waiting(), await card()], [[], ['Up to 4 crews: leave one to join another.'], [['Fox', 'Neon', '']], null]);
  await p.evaluate(() => { /** @type {any} */ (window).crews = []; });
  await p.evaluate(() => /** @type {any} */ (window).prompt.answerCrew(/** @type {any} */ (window).prompt.crewAsks[0], false));
  // none for a crew the player is in (it joined one since: that one's left out), nor names no Crews box would take
  await server(p, { op: 'crew', name: 'Fox', crew: 'Neon', room: '' });
  await p.evaluate(() => { /** @type {any} */ (window).crews = [{ name: 'JDM', rgb: '3cd2ff' }, { name: 'neon', rgb: 'ff50c8' }]; });
  assert.deepEqual(await waiting(), [], 'joined since');
  await p.evaluate(() => /** @type {any} */ (window).prompt.answerCrew(/** @type {any} */ (window).prompt.crewAsks[0], false));
  for (const m of [{ name: 'Fox', crew: 'jdm' }, { name: 'Fox', crew: 'a, b' }, { name: 'Fox', crew: 'x_fag_x' }, { name: 'Fox', crew: 'x'.repeat(25) },
    { name: 'Fox', crew: 'Ok', room: 'x_fag_x' }, { name: '', crew: 'Ok' }, { name: 'Fox', crew: '' }, { name: 'Fox', crew: 5 }]) {
    await server(p, { op: 'crew', ...m });
    assert.equal(await card(), null, JSON.stringify(m));
  }
  assert.deepEqual(await waiting(), []);
  // the game's end: none waits
  await server(p, { op: 'crew', name: 'Gus', crew: 'Late', room: '' });
  await p.evaluate(() => /** @type {any} */ (window).prompt.stop());
  assert.deepEqual([await waiting(), await card()], [[], null]);
  await p.close();
});

test('the notes at the top in one look: a crew\'s invite as a race\'s, the friend line and a plain note (only the border apart)', async () => {
  const p = await page();
  await raf(p);
  // the page's own stylesheet (style.css), a plain note (shell.ts toast) beside the prompt's lines in the column
  await p.addStyleTag({ content: fs.readFileSync(path.join(ROOT, 'web/src/app/style.css'), 'utf8') });
  await p.evaluate(() => {
    document.getElementById('stage').style.setProperty('--u', '1.5px');
    const t = Object.assign(document.createElement('div'), { className: 'toast', textContent: 'You and Bo_ are friends now' });
    document.querySelector('.toasts').append(t);
  });
  await bump(p, 'Bo', 1);
  await server(p, { op: 'can', name: 'Bo_', st: 'friends' });
  await server(p, { op: 'crew', name: 'Bo_', crew: 'Touge', room: '' });
  await server(p, { op: 'invite', name: 'Bo_', host: 1, lobby: 2, race: 3, kind: 'sprint', n: 1, max: 4 });
  const look = await p.evaluate(() => Object.fromEntries([...document.querySelectorAll('.toasts > *')].map(e => {
    const c = getComputedStyle(e), r = e.getBoundingClientRect();
    return [e.className, { box: [Math.round(r.height), c.paddingTop, c.paddingLeft, c.borderTopLeftRadius, c.backgroundColor, c.fontSize, c.lineHeight].join(' '),
      border: c.boxShadow !== 'none' }];
  })));
  const note = look.toast.box;
  assert.deepEqual(look, {
    'friend-prompt offer invite': { box: note, border: true }, 'friend-prompt offer invite crew-invite': { box: note, border: true },
    'friend-prompt': { box: note, border: false }, toast: { box: note, border: false },
  }, 'one box for all; a lime border only on what waits for an answer in its time');
  assert.equal(note, '53 15px 18px 4px rgba(13, 13, 13, 0.78) 16.5px 23.1px', 'the cards\' box at 1.5 px a HUD unit');
  await p.close();
});

test('the Crews control says why a name cannot join, nothing else (the row\'s help says how)', async () => {
  const p = await page();
  await p.evaluate(() => {
    const w = /** @type {any} */ (window);
    w.ctl = list => {
      w.cbox?.remove();
      const box = w.cbox = document.createElement('div');
      document.body.append(box);
      w.F.crewControl({ list, id: 'c', labelId: 'l', placeholder: 'Crew name', draft: '', onDraft() {}, change() {}, link: () => '' }, box);
    };
  });
  const note = () => p.evaluate(() => { const n = document.getElementById('c-note'); return [n.textContent, n.classList.contains('warn')]; });
  await p.evaluate(() => /** @type {any} */ (window).ctl([]));
  assert.deepEqual(await note(), ['', false], 'no crew yet: nothing');
  await p.fill('#c', 'a, b');
  assert.deepEqual(await note(), ['No commas, # or ; in a crew name.', true], 'why a name cannot join, in the warning\'s colour');
  await p.fill('#c', 'Night Owls');
  assert.deepEqual(await note(), ['', false], 'one that can: nothing again');
  await p.evaluate(() => /** @type {any} */ (window).ctl([{ name: 'JDM', rgb: '3cd2ff' }]));
  assert.deepEqual(await note(), ['', false], 'in a crew: nothing');
  await p.close();
});

test('the start page\'s Friends', async () => {
  const p = await page();
  await p.evaluate(() => { document.getElementById('friends').hidden = false; });   // (its tab open: tabs.ts)
  await panel(p, { me: 'Al_', asks: ['Dee'], sent: ['Eve'], friends: [
    { name: 'Bo_', where: 'roam', room: 'Night Owls', contact: 'bo.races' }, { name: 'Cy_', where: 'roam', room: '' }, { name: 'Fox', where: 'roam' },
    { name: 'Gus', where: 'online' }, { name: 'Hal', where: '', contact: 'hal.drives' }] });
  assert.deepEqual(await rows(p), [
    ['Dee', 'wants to be friends', 'Accept|Decline', ''],
    ['Bo_', 'free roam · Night Owls', 'Copy Discord name bo.races|Join|Message|Remove', ''],
    ['Cy_', 'free roam', 'Join|Message|Remove', ''],
    ['Fox', 'free roam', 'Message|Remove', ''],
    ['Gus', 'Online lobby', 'Message|Remove', ''],
    ['Hal', 'offline', 'Copy Discord name hal.drives|Message|Remove', 'off'],
    ['Eve', 'request sent', 'Take back', 'off'],
  ]);
  assert.deepEqual(await p.$$eval('#friends .fl-st', es => es.map(e => e.classList.contains('ok'))), [true, true, true, true, true, false, false], 'lime while it plays (or asks)');
  const waiting = () => p.evaluate(() => /** @type {any} */ (window).waiting.splice(0).at(-1));
  assert.equal(await waiting(), 1, 'the tab\'s count: a request to answer');
  // a friend's Discord icon: copied, and a toast at it says so; where the browser would not copy (neither way), its name
  const copy = async ok => {
    await p.evaluate(ok => {
      const w = /** @type {any} */ (window);
      w.copies = [];
      Object.defineProperty(navigator, 'clipboard', { configurable: true, value: { writeText: async s => { w.copies.push(s); if (!ok) throw new Error('no'); } } });
      document.execCommand = () => false;
    }, ok);
    await p.click('#friends .fl li:nth-of-type(6) .fl-discord');
    await p.evaluate(() => new Promise(r => setTimeout(r, 20)));
    return [await p.evaluate(() => /** @type {any} */ (window).copies), await p.$$eval('.copy-toast', es => es.map(e => e.textContent))];
  };
  assert.deepEqual(await copy(true), [['hal.drives'], ['Copied hal.drives']]);
  assert.deepEqual(await copy(false), [['hal.drives'], ['Discord: hal.drives']], 'one toast a button: the new one in its place');
  const click = async (row, text) => {
    const b = await p.$(`#friends .fl li:nth-of-type(${row}) button[aria-label="${text}"]`);
    await b.click();
    await p.evaluate(() => new Promise(r => setTimeout(r, 20)));
  };
  const did = () => p.evaluate(() => { const w = /** @type {any} */ (window); return { acts: w.acts.splice(0), joined: w.joined.splice(0) }; });
  await click(1, 'Accept');
  await click(1, 'Decline');
  await click(2, 'Join');
  await click(3, 'Join');
  assert.deepEqual(await did(), { acts: [['accept', 'Dee'], ['decline', 'Dee']], joined: ['Night Owls', ''] });
  await p.evaluate(() => { /** @type {any} */ (window).confirmYes = false; });
  await click(2, 'Remove');
  assert.deepEqual((await did()).acts, [], 'Remove asks first: not now');
  await p.evaluate(() => { /** @type {any} */ (window).confirmYes = true; });
  await click(2, 'Remove');
  await click(7, 'Take back');
  assert.deepEqual((await did()).acts, [['remove', 'Bo_'], ['remove', 'Eve']]);
  // Message: the conversation opens; messages that waited come with the list, counted on Message until read
  await click(3, 'Message');
  assert.deepEqual(await p.evaluate(() => /** @type {any} */ (window).chats.splice(0)), ['Cy_']);
  await panel(p, { me: 'Al_', asks: [], sent: [], friends: [{ name: 'Bo_', where: '' }, { name: 'Cy_', where: '' }],
    mail: [{ from: 'Bo_', body: 'hi', at: 1 }, { from: 'Bo_', body: 'race?', at: 2 }] });
  assert.deepEqual(await rows(p), [['Bo_', 'offline', 'Message (2)|Remove', 'off'], ['Cy_', 'offline', 'Message|Remove', 'off']]);
  assert.deepEqual(await p.$$eval('#friends .fl-msg.unread', bs => bs.map(b => b.title)), ['Message (2)'], 'unread: the lime icon, its count as the tooltip');
  assert.equal(await waiting(), 2, 'the tab\'s count: the messages not read');
  await p.evaluate(() => /** @type {any} */ (window).store.read('bo_'));
  assert.deepEqual((await rows(p))[0], ['Bo_', 'offline', 'Message|Remove', 'off'], 'read: shown again at once');
  assert.equal(await waiting(), 0, 'read: none');
  // nobody yet; a keyphrase without a claimed name; no keyphrase
  await panel(p, { me: 'Al_', asks: [], sent: [], friends: [] });
  assert.equal(await p.$eval('#friends .set-list', e => e.textContent), 'FriendsTouch front bumpers in free roam for 3 s to add a player.', 'nobody yet: how to');
  await panel(p, { me: null, asks: [], sent: [], friends: [] });
  assert.equal(await p.$eval('#friends', e => e.textContent), 'FriendsClaim your name (Online, Your name) to add friends.');
  assert.deepEqual(await rows(p), []);
  await panel(p, null);
  assert.equal(await p.$eval('#friends', e => e.textContent), 'FriendsClaim your name (Online, Your name) to add friends.', 'no keyphrase: how to have friends');
  // a name claimed while the list is on its way: asked again once it has come, so the claim shows at once
  const asked = await p.evaluate(async () => {
    const w = /** @type {any} */ (window);
    let n = 0, answer;
    w.panel.deps.api.list = () => { n++; return new Promise(r => { answer = r; }); };
    const first = w.panel.refresh(true);
    await w.panel.refresh(true);                    // the claim's, while the first waits for its answer
    answer(null); await first;
    await new Promise(r => setTimeout(r, 0));
    const again = n;
    answer({ me: 'Al_', asks: [], sent: [], friends: [] });
    await new Promise(r => setTimeout(r, 0));
    return { again, text: document.querySelector('#friends .set-list').textContent };
  });
  assert.deepEqual(asked, { again: 2, text: 'FriendsTouch front bumpers in free roam for 3 s to add a player.' });
  await p.close();
});

test('the start page\'s Your Discord', async () => {
  const p = await page();
  await p.evaluate(() => { document.getElementById('friends').hidden = false; });   // (its tab open: tabs.ts)
  // Discord's rules, as the server's buddy.rs discord_name
  assert.deepEqual(await p.evaluate(() => ['  @Al.Drives_9 ', 'a', 'a..b', 'ann drives', 'ann-drives', 'ännchen', '#1234', 'x'.repeat(33), '._.', 'x'.repeat(32)]
    .map(s => /** @type {any} */ (window).F.discordName(s))), ['al.drives_9', null, null, null, null, null, null, null, '._.', 'x'.repeat(32)]);
  await panel(p, { me: 'Al_', asks: [], sent: [], friends: [], contact: 'al.races' });
  const row = () => p.evaluate(() => {
    const r = document.querySelector('#friends .set:has(#set-discord)'), st = r.querySelector('.set-state');
    return { label: r.querySelector('.set-label').textContent, value: r.querySelector('input').value, state: st.hidden ? '' : st.textContent };
  });
  const enter = async typed => {
    await p.fill('#set-discord', typed);
    await p.press('#set-discord', 'Enter');
    await p.evaluate(() => new Promise(r => setTimeout(r, 20)));
    return p.evaluate(() => /** @type {any} */ (window).discords.splice(0));
  };
  assert.deepEqual(await row(), { label: 'Your Discord', value: 'al.races', state: '' }, 'the server\'s');
  assert.deepEqual(await enter('Al Races'), [], 'not Discord\'s form: refused here');
  assert.deepEqual(await row(), { label: 'Your Discord', value: 'Al Races', state: 'Not a Discord username: 2 to 32 small letters, digits, _ or .' });
  assert.deepEqual(await enter(' @Al.Drives '), ['al.drives']);
  assert.deepEqual(await row(), { label: 'Your Discord', value: 'al.drives', state: '' });
  assert.deepEqual(await enter('al.drives'), [], 'the same: not sent again');
  await p.evaluate(() => { /** @type {any} */ (window).discordDone = 'name'; });
  assert.deepEqual(await enter('badname'), ['badname']);
  assert.deepEqual(await row(), { label: 'Your Discord', value: 'badname', state: 'badname isn\'t allowed here.' }, 'the server refused it');
  await p.evaluate(() => { /** @type {any} */ (window).discordDone = 'rate'; });
  await enter('other.name');
  assert.equal((await row()).state, 'Wait a moment');
  await p.evaluate(() => { /** @type {any} */ (window).discordDone = 'ok'; });
  assert.deepEqual(await enter(''), [''], 'none: taken away');
  assert.deepEqual(await row(), { label: 'Your Discord', value: '', state: '' });
  // a list from the server while the player types: what is typed stays, the field keeps the keyboard
  await p.fill('#set-discord', 'al.dr');
  await p.evaluate(() => { const w = /** @type {any} */ (window); w.lists = { ...w.lists, contact: 'from.elsewhere' }; return w.panel.refresh(true); });
  assert.deepEqual([(await row()).value, await p.evaluate(() => document.activeElement?.id)], ['al.dr', 'set-discord']);
  await p.evaluate(() => /** @type {HTMLElement} */ (document.activeElement).blur());
  assert.deepEqual(await p.evaluate(() => /** @type {any} */ (window).discords.splice(0)), ['al.dr'], 'left: sent');
  // no claimed name: no row
  await panel(p, { me: null, asks: [], sent: [], friends: [] });
  assert.equal(await p.$('#set-discord'), null);
  await p.close();
});

test('the start page\'s Friends: a request by name', async () => {
  const p = await page();
  await panel(p, { me: 'Al_', asks: [], sent: [], friends: [] });
  await p.evaluate(() => { document.getElementById('friends').hidden = false; });   // (its tab open: tabs.ts)
  const ask = async (lists, typed, how = 'Enter') => {
    await p.evaluate(l => { /** @type {any} */ (window).lists = l; }, lists);
    await p.fill('#friends .fl-add input', typed);
    if (how === 'Enter') await p.press('#friends .fl-add input', 'Enter'); else await p.click('#friends .fl-ask');
    await p.evaluate(() => new Promise(r => setTimeout(r, 30)));
    return p.evaluate(() => ({ note: document.querySelector('#friends .fl-note').textContent, warn: document.querySelector('#friends .fl-note').classList.contains('warn'),
      field: document.querySelector('#friends .fl-add input').value, acts: /** @type {any} */ (window).acts.splice(0) }));
  };
  assert.equal(await p.$eval('#friends .fl-ask', b => b.disabled), true, 'nothing typed: nothing to send');
  assert.deepEqual(await ask({ me: 'Al_', asks: [], sent: ['Zed'], friends: [], done: 'asked' }, ' zed '),
    { note: 'Friend request sent to Zed', warn: false, field: '', acts: [['ask', 'zed']] }, 'the claim\'s spelling from the list');
  assert.deepEqual((await rows(p)).map(r => r.slice(0, 3)), [['Zed', 'request sent', 'Take back']]);
  assert.deepEqual(await ask({ me: 'Al_', asks: [], sent: [], friends: [], done: 'claim' }, 'Nobody', 'click'),
    { note: 'Nobody has no claimed name, so can\'t add friends yet', warn: true, field: 'Nobody', acts: [['ask', 'Nobody']] }, 'the name stays to fix');
  assert.equal((await ask({ me: 'Al_', asks: [], sent: [], friends: [], done: 'self' }, 'al')).note, 'That\'s your own name');
  // typing goes on over a new list (a message, the minute's poll)
  await p.fill('#friends .fl-add input', 'Bo');
  await p.focus('#friends .fl-add input');
  await p.evaluate(() => /** @type {any} */ (window).panel.render());
  await p.keyboard.type('b');
  assert.equal(await p.$eval('#friends .fl-add input', i => i.value), 'Bob');
  await p.close();
});

test('the start page\'s Friends in pages of 20: Filter on the row, ‹ 1 of 3 › under the list', async () => {
  const p = await page();
  await p.evaluate(() => { document.getElementById('friends').hidden = false; });   // (its tab open: tabs.ts)
  const name = i => `P${String(i).padStart(2, '0')}`;
  // a request to answer, 45 friends (P30 in free roam: first of them), a request sent: 47 lines
  const friends = Array.from({ length: 45 }, (_, i) => ({ name: name(i), where: i === 30 ? 'roam' : '' }));
  await panel(p, { me: 'Al_', asks: ['Ask'], sent: ['Snt'], friends });
  const view = () => p.evaluate(() => {
    const $ = s => document.querySelector(`#friends ${s}`);
    return {
      names: [...document.querySelectorAll('#friends .fl li .fl-who')].map(e => e.textContent),
      page: $('.fl-page')?.textContent ?? null, prev: $('.fl-prev')?.disabled ?? null, next: $('.fl-next')?.disabled ?? null,
      find: $('.set-list > .fl-find') ? $('.fl-find input').value : null, clear: $('.fl-clear')?.disabled ?? null,
      none: $('.set-control > p.set-note:not(.fl-note)')?.textContent ?? '', focus: document.activeElement?.className ?? '',
    };
  });
  const range = (a, b) => Array.from({ length: b - a }, (_, i) => name(a + i));
  let v = await view();
  assert.deepEqual(v.names, ['Ask', 'P30', ...range(0, 18)], 'page 1: the request, the friend in free roam, then the others');
  assert.deepEqual([v.page, v.prev, v.next, v.find, v.clear], ['1 of 3', true, false, '', true], 'Filter empty: its magnifier, dimmed');
  await p.click('#friends .fl-next');
  v = await view();
  assert.deepEqual([v.names, v.page, v.prev, v.next, v.focus], [[...range(18, 30), ...range(31, 39)], '2 of 3', false, false, 'icon-btn fl-next'], 'the focus stays on ›');
  await p.click('#friends .fl-next');
  v = await view();
  assert.deepEqual([v.names, v.page, v.next, v.focus], [[...range(39, 45), 'Snt'], '3 of 3', true, 'icon-btn fl-prev'], 'the last page: › dimmed, ‹ takes the focus');
  await p.evaluate(() => /** @type {any} */ (window).panel.render());
  assert.equal((await view()).page, '3 of 3', 'a new list (the minute\'s poll) keeps the page');
  // Filter looks in every line, from the first page; no pager for one page; none found says so
  await p.fill('#friends .fl-find input', 'p4');
  v = await view();
  assert.deepEqual([v.names, v.page, v.clear], [range(40, 45), null, false], 'found: no pages, the ✕ to empty it');
  await p.focus('#friends .fl-find input');
  await p.evaluate(() => /** @type {any} */ (window).panel.render());
  await p.keyboard.type('2');
  assert.deepEqual((await view()).names, ['P42'], 'typing goes on over a new list');
  await p.fill('#friends .fl-find input', 'zzz');
  v = await view();
  assert.deepEqual([v.names, v.none], [[], 'No friend has that name']);
  await p.press('#friends .fl-find input', 'Escape');
  v = await view();
  assert.deepEqual([v.find, v.page, v.names.length, v.none], ['', '1 of 3', 20, ''], 'Esc empties it: page 1 again');
  await p.fill('#friends .fl-find input', 'snt');
  await p.click('#friends .fl-clear');
  assert.deepEqual([(await view()).find, (await view()).focus], ['', ''], 'the ✕ empties it, the field keeps the focus');
  assert.equal(await p.evaluate(() => document.activeElement === document.querySelector('#friends .fl-find input')), true);
  // a page or less: neither
  await panel(p, { me: 'Al_', asks: [], sent: [], friends: friends.slice(0, 20) });
  v = await view();
  assert.deepEqual([v.names.length, v.page, v.find], [20, null, null]);
  await p.close();
});

test('the page menu\'s Friends: the first 10 names a line, then how many more', async () => {
  const p = await page();
  const on = Array.from({ length: 12 }, (_, i) => `On${i}`), off = Array.from({ length: 13 }, (_, i) => `Off${i}`);
  await server(p, { op: 'list', me: 'Al_', friends: [...on, ...off], asks: [] });
  await server(p, { op: 'where', friends: [...on.map(name => ({ name, where: 'online' })), ...off.map(name => ({ name, where: '' }))] });
  const lines = await p.evaluate(() => {
    const box = document.createElement('div');
    /** @type {any} */ (window).prompt.drawPlaced(box, () => {});
    return [...box.querySelectorAll(':scope > p.hint')].map(e => e.textContent);
  });
  assert.deepEqual(lines, ['None of your friends is in free roam now.',
    `In the Online lobby: ${on.slice(0, 10).join(', ')} and 2 more. Offline: ${off.slice(0, 10).join(', ')} and 3 more.`]);
  await p.close();
});

test('/friends with the keyphrase\'s token', async () => {
  const p = await page();
  const r = await p.evaluate(async () => {
    const w = /** @type {any} */ (window), asked = [];
    w.fetch = async (url, init) => {
      asked.push({ url, method: init?.method ?? 'GET', auth: init?.headers?.authorization, body: init?.body ?? null });
      return new Response(JSON.stringify({ me: 'Al_', done: 'ok', friends: [{ name: 'Bo_', where: 'roam', room: 'X' }, { name: 5 }, { name: 'Cy_', where: 'mars' }],
        asks: ['Dee', 7], sent: 'Eve', contact: 5 }), { status: 200 });
    };
    const api = new w.F.FriendsApi(async () => 'T'.repeat(43));
    const list = await api.list(), act = await api.act('remove', 'Bo_');
    await api.discord('al.races');
    await api.crew('Bo_', 'Night Owls', 'Blue Room');
    const none = await new w.F.FriendsApi(async () => null).list();
    w.fetch = async () => new Response('', { status: 404 });
    const gone = await api.list();
    return { list, act: act.done, asked, none, gone };
  });
  assert.deepEqual(r.list, { me: 'Al_', done: 'ok', friends: [{ name: 'Bo_', where: 'roam', room: 'X' }, { name: 'Cy_', where: '' }], asks: ['Dee'], sent: [], mail: [], contact: null }, 'read leniently');
  assert.equal(r.act, 'ok');
  assert.deepEqual(r.asked, [
    { url: '/friends', method: 'GET', auth: `Bearer ${'T'.repeat(43)}`, body: null },
    { url: '/friends', method: 'POST', auth: `Bearer ${'T'.repeat(43)}`, body: '{"op":"remove","name":"Bo_"}' },
    { url: '/friends', method: 'POST', auth: `Bearer ${'T'.repeat(43)}`, body: '{"op":"discord","name":"al.races"}' },
    { url: '/friends', method: 'POST', auth: `Bearer ${'T'.repeat(43)}`, body: '{"op":"crew","name":"Bo_","crew":"Night Owls","room":"Blue Room"}' },
  ]);
  assert.equal(r.none, null, 'no keyphrase: not asked');
  assert.equal(r.gone, null, 'a server without friends (404)');
  await p.close();
});
