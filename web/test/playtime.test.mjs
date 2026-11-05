// The app's play counter (web/src/app/playtime.ts) on a fake clock: two hours of play with some input in sight ask
// the game for our message on its phone, again in each game until the game's status says the inbox has it; then
// never again.  Idle and hidden time does not count; the count carries over to the next game (localStorage).  The
// start page's figures: the latest game's play (kept as it goes), the whole, and "at least" for a count from before
// the page showed it (decided once).
//   node --test web/test/playtime.test.mjs
import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const js = (await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/app/playtime.ts')], bundle: true, format: 'esm', write: false })).outputFiles[0].text;
const { PlayTime, SMS_AFTER_MS, lastGameMs, playedMs, playedAtLeast, playedText } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

const MIN = 60_000;
let t = 1000;
Object.defineProperty(globalThis, 'performance', { value: { now: () => t }, configurable: true });
const store = new Map();
globalThis.localStorage = { getItem: k => store.get(k) ?? null, setItem: (k, v) => store.set(k, String(v)) };
globalThis.document = { hidden: false };

// a game: its input (the last input, the requests for our message) and its status
function game() {
  mock.timers.enable({ apis: ['setInterval'] });
  const g = { last: t, asked: [], sms: 0, logs: [] };
  const s = {
    input: { lastInput: () => g.last, sms: id => { g.asked.push(id); return true; } },
    status: { read: () => new Map([['sms', g.sms]]) },
  };
  g.pt = new PlayTime(s, SMS_AFTER_MS, l => g.logs.push(l));
  g.play = ms => { for (let k = 0; k < ms; k += 5000) { t += 5000; g.last = t; mock.timers.tick(5000); } };
  g.idle = ms => { for (let k = 0; k < ms; k += 5000) { t += 5000; mock.timers.tick(5000); } };
  g.end = () => { g.pt.stop(); mock.timers.reset(); };
  return g;
}

test('two hours over two games ask for the message, until the inbox has it; then never again', () => {
  store.clear();
  const a = game();
  a.play(70 * MIN);
  a.end();
  assert.deepEqual(a.asked, [], 'not after 70 min');
  const b = game();
  b.play(49 * MIN);
  assert.deepEqual(b.asked, [], 'not after 119 min');
  b.play(MIN);
  assert.deepEqual(b.asked, [1], 'at two hours');
  b.play(30 * MIN);
  assert.deepEqual(b.asked, [1], 'once a game');
  b.end();
  const c = game();
  c.play(5000);
  assert.deepEqual(c.asked, [1], 'again in the next game: its inbox does not have it yet');
  c.sms = 1;
  c.play(5000);
  assert.ok(c.logs.some(l => /in the game's inbox/.test(l)));
  c.end();
  const d = game();
  d.play(60 * MIN);
  d.end();
  assert.deepEqual(d.asked, [], 'never again');
});

test('idle and hidden time does not count', () => {
  store.clear();
  const a = game();
  a.play(60 * MIN);
  a.idle(4 * MIN);                       // still counts: input less than 5 min ago
  a.idle(120 * MIN);                     // the rest does not
  assert.deepEqual(a.asked, []);
  globalThis.document.hidden = true;
  a.play(120 * MIN);
  globalThis.document.hidden = false;
  assert.deepEqual(a.asked, []);
  a.play(55 * MIN);
  assert.deepEqual(a.asked, [], 'not at 119 min of play');
  a.play(2 * MIN);
  assert.deepEqual(a.asked, [1]);
  a.end();
});

test('the start page\'s figures: the latest game, the whole, and "at least" for a count from before', () => {
  store.clear();
  assert.equal(playedAtLeast(), false, 'no count yet: all of it counted from here');
  const a = game();
  a.play(30 * MIN);
  assert.equal(lastGameMs(), 30 * MIN, 'kept as the game goes (a page stopped now keeps it)');
  a.end();
  const b = game();
  assert.equal(lastGameMs(), 0, 'a new game starts its own');
  b.play(80 * MIN);
  b.idle(20 * MIN);
  b.end();
  assert.equal(lastGameMs(), 85 * MIN - 5000, 'its play, and input less than 5 min ago');
  assert.equal(playedMs(), 30 * MIN + 85 * MIN - 5000);
  assert.equal(playedAtLeast(), false, 'decided once');
  store.clear();
  store.set('nfsu2.play.ms', String(3 * 3600_000));
  assert.equal(playedAtLeast(), true, 'a count from before: it may miss play');
  store.set('nfsu2.play.ms', '0');
  assert.equal(playedAtLeast(), true, 'decided once');
  assert.deepEqual([59_999, 60_000, 45 * MIN + 59_000, 60 * MIN, 200 * MIN].map(playedText), ['', '1 min', '45 min', '1 h 0 min', '3 h 20 min']);
});
