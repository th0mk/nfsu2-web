// Free roam's friends on the page (web/src/app/friends.ts), in a real browser (web/tools/browser.mjs):
//   node --test web/test/friends.test.mjs
// - the line over the game: nothing until the server says what touching that player may lead to, then "Add Bo as a
//   friend" (F, a click on the line, or the page menu's button), "Bo wants to be friends: accept", the request sent,
//   friends, or why not (no claimed name here or there); gone when the cars part, the name over the car and the
//   claimed name being one player; F only while the line offers something, never in a text field
// - the room's name to the server once there are friends, and again when the room changes
// - where the friends play, asked every minute while there are some: a notice when one comes into free roam (none for
//   one just met here); the page menu's Friends popup: the ones in free roam with a Join (none in the player's own
//   room), the others in a line
// - the start page's Friends: a line each, the requests to answer, each friend where it plays with a Join in free roam
//   (its room), the requests sent; Accept, Decline, Remove (asked first), Take back; a keyphrase without a claimed
//   name; no keyphrase; a friend's Message, lime while messages wait to be read (the ones that waited come with the
//   list)
// - /friends asked with the keyphrase's token, its answer read leniently
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
  stdin: { contents: `export * from './web/src/app/friends.ts'; export * from './web/src/app/messages.ts';`, resolveDir: ROOT, loader: 'ts' },
  bundle: true, format: 'iife', globalName: 'F', write: false, platform: 'browser',
})).outputFiles[0].text;

const browser = await chromium.launch({ ...pickBrowser(), headless: true });
test.after(() => browser.close());

/** a blank page with the module, a stage and the Friends box, and a FriendPrompt with deps that record what it does */
async function page() {
  const p = await browser.newPage();
  await p.setContent('<div id="stage" style="position:relative;width:640px;height:480px"></div><div id="friends" hidden></div><input id="field">');
  await p.addScriptTag({ content: js });
  await p.evaluate(() => {
    const w = /** @type {any} */ (window);
    w.sent = []; w.toasts = []; w.notices = []; w.placed = 0; w.room = '';
    w.prompt = new w.F.FriendPrompt(document.getElementById('stage'), {
      send: m => { w.sent.push(m); return true; }, room: () => w.room, toast: s => w.toasts.push(s), log: () => {},
      notice: (name, text) => w.notices.push([name, text]), menu: () => 'the menu (F9)', placed: () => { w.placed++; },
    });
    w.prompt.start({ viewRect: () => ({ x: 0, y: 0, w: 640, h: 480 }) }, true);
  });
  return p;
}
const line = p => p.evaluate(() => { const b = document.querySelector('.friend-prompt'); return b.hidden ? null : { text: b.textContent, offer: b.classList.contains('offer') }; });
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
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).prompt.menuItem()?.label), 'Accept Cy_Two as a friend');
  await sent(p);
  await p.click('.friend-prompt');
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
  // a request while the cars do not touch: answered on the start page
  await server(p, { op: 'asks', name: 'Eve' });
  assert.deepEqual(await toasts(p), ['Eve wants to be friends: answer under Friends on the start page']);
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
  assert.deepEqual(await toasts(p), ['A friends list is full (50 at most)']);
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
  await raf(p);
  assert.deepEqual((await sent(p)).filter(m => m.op === 'where'), [{ op: 'where' }], 'asked with the next frame once listed');
  await raf(p);
  assert.deepEqual((await sent(p)).filter(m => m.op === 'where'), [], 'then not again before a minute');
  const where = [
    { name: 'Bo_', where: 'roam', room: 'Blue Room' }, { name: 'Cy_', where: 'roam', room: 'night owls' },
    { name: 'Dee', where: 'roam', room: '' }, { name: 'Eve', where: 'online' }, { name: 'Fox', where: '' },
  ];
  await server(p, { op: 'where', friends: where });
  assert.deepEqual(await notices(p), [
    ['Bo_', 'Your friend Bo_ is in free roam, room Blue Room: Join in the menu (F9)'],
    ['Cy_', 'Your friend Cy_ is in free roam'],
    ['Dee', 'Your friend Dee is in free roam, in no room: Join in the menu (F9)'],
  ], 'the ones in free roam, at the first answer too (a room as the game hashes it: case and blanks do not count)');
  assert.equal(await p.evaluate(() => /** @type {any} */ (window).placed), 1, 'the popup drawn again');
  await server(p, { op: 'where', friends: where });
  assert.deepEqual(await notices(p), [], 'still there: no notice again');
  // Eve comes into free roam; Bo leaves and comes back; Fox is befriended here first (no notice for one just met)
  await server(p, { op: 'where', friends: [where[0], where[1], where[2], { name: 'Eve', where: 'roam' }, { name: 'Fox', where: '' }] });
  assert.deepEqual(await notices(p), [['Eve', 'Your friend Eve is in free roam']], 'a room its page did not tell: no Join');
  await server(p, { op: 'where', friends: [{ name: 'Bo_', where: 'online' }, where[1], where[2], where[3], where[4]] });
  await server(p, { op: 'friends', name: 'Fox' });
  await server(p, { op: 'where', friends: [...where.slice(0, 4), { name: 'Fox', where: 'roam', room: 'night owls' }] });
  assert.deepEqual(await notices(p), [['Bo_', 'Your friend Bo_ is in free roam, room Blue Room: Join in the menu (F9)']]);
  // the popup: in free roam, a Join for each one in another room; the others in a line
  const pop = () => p.evaluate(() => {
    const w = /** @type {any} */ (window), box = document.createElement('div');
    w.joins = [];
    w.prompt.drawPlaced(box, r => w.joins.push(r));
    const rows = [...box.querySelectorAll('.fl li')].map(li => [li.querySelector('.fl-name').firstChild.textContent, li.querySelector('.fl-st').textContent,
      [...li.querySelectorAll('button')].map(b => b.getAttribute('aria-label')).join('|')]);
    for (const b of box.querySelectorAll('button')) b.click();
    return { rows, lines: [...box.querySelectorAll(':scope > p')].map(e => e.textContent), joins: w.joins };
  });
  assert.deepEqual(await pop(), {
    rows: [['Bo_', 'free roam · Blue Room', 'Join'], ['Cy_', 'free roam · your room', ''], ['Dee', 'free roam', 'Join'], ['Fox', 'free roam · your room', '']],
    lines: ['In the Online lobby: Eve.'], joins: ['Blue Room', ''],
  });
  await server(p, { op: 'where', friends: [{ name: 'Bo_', where: 'online' }, { name: 'Cy_', where: '' }, { name: 'Dee', where: '' }] });
  assert.deepEqual(await pop(), { rows: [], lines: ['None of your friends is in free roam now.', 'In the Online lobby: Bo_. Offline: Cy_, Dee.'], joins: [] });
  // asked again when the popup opens
  await p.evaluate(() => /** @type {any} */ (window).prompt.askWhere());
  assert.deepEqual((await sent(p)).filter(m => m.op === 'where'), [{ op: 'where' }]);
  await p.close();
});

/** a FriendsPanel in the page with an api that answers `lists` and records what it is asked */
async function panel(p, lists) {
  await p.evaluate(l => {
    const w = /** @type {any} */ (window);
    w.acts = []; w.joined = []; w.chats = []; w.lists = l; w.confirmYes = true;
    const api = { list: async () => w.lists, act: async (op, name) => { w.acts.push([op, name]); return w.lists; } };
    w.store ??= new w.F.MessageStore();
    w.panel = new w.F.FriendsPanel(document.getElementById('friends'), {
      api, join: room => w.joined.push(room), confirm: async () => w.confirmYes, idle: () => true, log: () => {},
      store: w.store, chat: name => w.chats.push(name),
    });
  }, lists);
  await p.evaluate(() => /** @type {any} */ (window).panel.refresh(true));
}
const rows = p => p.$$eval('#friends .fl li', rs => rs.map(r => [r.querySelector('.fl-name').firstChild.textContent, r.querySelector('.fl-st').textContent,
  [...r.querySelectorAll('button')].map(b => b.getAttribute('aria-label')).join('|'), r.className]));

test('the start page\'s Friends', async () => {
  const p = await page();
  await p.evaluate(() => { document.getElementById('friends').hidden = false; });   // (its tab open: tabs.ts)
  await panel(p, { me: 'Al_', asks: ['Dee'], sent: ['Eve'], friends: [
    { name: 'Bo_', where: 'roam', room: 'Night Owls' }, { name: 'Cy_', where: 'roam', room: '' }, { name: 'Fox', where: 'roam' },
    { name: 'Gus', where: 'online' }, { name: 'Hal', where: '', contact: 'gus#1234' }] });
  assert.deepEqual(await rows(p), [
    ['Dee', 'wants to be friends', 'Accept|Decline', ''],
    ['Bo_', 'free roam · Night Owls', 'Join|Message|Remove', ''],
    ['Cy_', 'free roam', 'Join|Message|Remove', ''],
    ['Fox', 'free roam', 'Message|Remove', ''],
    ['Gus', 'Online lobby', 'Message|Remove', ''],
    ['Hal', 'offline', 'Message|Remove', 'off'],
    ['Eve', 'request sent', 'Take back', 'off'],
  ]);
  assert.deepEqual(await p.$$eval('#friends .fl-st', es => es.map(e => e.classList.contains('ok'))), [true, true, true, true, true, false, false], 'lime while it plays (or asks)');
  assert.equal(await p.$eval('#friends .fl-contact', e => e.textContent), 'gus#1234', 'a friend\'s own line, when it has one');
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
  await p.evaluate(() => /** @type {any} */ (window).store.read('bo_'));
  assert.deepEqual((await rows(p))[0], ['Bo_', 'offline', 'Message|Remove', 'off'], 'read: shown again at once');
  // nobody yet; a keyphrase without a claimed name; no keyphrase
  await panel(p, { me: 'Al_', asks: [], sent: [], friends: [] });
  assert.equal(await p.$eval('#friends', e => e.textContent), 'FriendsTouch front bumpers in free roam for 3 s to add a player.', 'nobody yet: how to');
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
    return { again, text: document.getElementById('friends').textContent };
  });
  assert.deepEqual(asked, { again: 2, text: 'FriendsTouch front bumpers in free roam for 3 s to add a player.' });
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

test('/friends with the keyphrase\'s token', async () => {
  const p = await page();
  const r = await p.evaluate(async () => {
    const w = /** @type {any} */ (window), asked = [];
    w.fetch = async (url, init) => {
      asked.push({ url, method: init?.method ?? 'GET', auth: init?.headers?.authorization, body: init?.body ?? null });
      return new Response(JSON.stringify({ me: 'Al_', done: 'ok', friends: [{ name: 'Bo_', where: 'roam', room: 'X' }, { name: 5 }, { name: 'Cy_', where: 'mars' }],
        asks: ['Dee', 7], sent: 'Eve' }), { status: 200 });
    };
    const api = new w.F.FriendsApi(async () => 'T'.repeat(43));
    const list = await api.list(), act = await api.act('remove', 'Bo_');
    const none = await new w.F.FriendsApi(async () => null).list();
    w.fetch = async () => new Response('', { status: 404 });
    const gone = await api.list();
    return { list, act: act.done, asked, none, gone };
  });
  assert.deepEqual(r.list, { me: 'Al_', done: 'ok', friends: [{ name: 'Bo_', where: 'roam', room: 'X' }, { name: 'Cy_', where: '' }], asks: ['Dee'], sent: [], mail: [] }, 'read leniently');
  assert.equal(r.act, 'ok');
  assert.deepEqual(r.asked, [
    { url: '/friends', method: 'GET', auth: `Bearer ${'T'.repeat(43)}`, body: null },
    { url: '/friends', method: 'POST', auth: `Bearer ${'T'.repeat(43)}`, body: '{"op":"remove","name":"Bo_"}' },
  ]);
  assert.equal(r.none, null, 'no keyphrase: not asked');
  assert.equal(r.gone, null, 'a server without friends (404)');
  await p.close();
});
