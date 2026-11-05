// Last.fm's scrobbles (web/src/app/lastfm.ts) on a fake server and fake songs: when "now playing" and a scrobble go, with which tags,
// what is kept when Last.fm cannot be asked, and a session that stops working.  The server's side: web/server-rs/src/app/lastfm.rs.
//   node --test web/test/lastfm.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const load = async f => await import('data:text/javascript;base64,' + Buffer.from(
  (await esbuild.build({ entryPoints: [path.join(ROOT, f)], bundle: true, format: 'esm', write: false })).outputFiles[0].text).toString('base64'));
const { Scrobbler, authLink } = await load('web/src/app/lastfm.ts');

const settle = async () => { for (let k = 0; k < 8; k++) await new Promise(r => setImmediate(r)); };
const SESSION = { name: 'someone', sk: 'SK' };
const T0 = 1_800_000_000_000;
const lines = (artist, title, album = '') => ({ artist, title, album });

function setup({ answer = () => ({ status: 200, json: { ok: true } }), folder = null } = {}) {
  const kept = {};
  globalThis.localStorage = { getItem: k => kept[k] ?? null, setItem: (k, v) => { kept[k] = String(v); }, removeItem: k => { delete kept[k]; } };
  const calls = [], log = [];
  let now = T0, dropped = 0;
  const s = new Scrobbler({
    api: { call: async body => { calls.push(body); const a = answer(body); if (a instanceof Error) throw a; return a; } },
    folder: () => folder, dropped: () => { dropped++; }, log: l => log.push(l), now: () => now,
  });
  // the song the runtime tells, `ms` heard by now; the clock goes on with it
  const hear = async (song, ms, session = SESSION) => { now = T0 + ms; s.tick({ ...song, ms }, session); await settle(); };
  return { s, calls, log, kept, hear, dropped: () => dropped, at: ms => { now = T0 + ms; } };
}
const ops = calls => calls.map(c => c.op);
const game = (seq, extra = {}) => ({ seq, kind: 'game', ended: false, lines: lines('Snoop Dogg', 'Riders on the Storm', 'NFSU2'), ...extra });

test('an EA Trax song: now playing once heard, a scrobble after 4 minutes, each once', async () => {
  const t = setup();
  await t.hear(game(2), 500);
  assert.deepEqual(ops(t.calls), [], 'nothing before a second is heard');
  await t.hear(game(2), 1500);
  assert.deepEqual(t.calls, [{ op: 'playing', sk: 'SK', artist: 'Snoop Dogg', track: 'Riders on the Storm', album: 'NFSU2' }]);
  await t.hear(game(2), 239_000);
  assert.deepEqual(ops(t.calls), ['playing']);
  await t.hear(game(2), 240_000);
  await t.hear(game(2), 300_000);
  assert.deepEqual(ops(t.calls), ['playing', 'scrobble']);
  assert.equal(t.calls[1].timestamp, Math.round(T0 / 1000), 'the time it began');
});

test('an EA Trax song that played to its end: scrobbled after 30 s heard, not before; one cut short is not', async () => {
  const t = setup();
  await t.hear(game(2), 20_000);
  await t.hear(game(2, { ended: true }), 20_000);
  assert.deepEqual(ops(t.calls), ['playing'], '20 s is too short');
  await t.hear(game(4), 1000);
  await t.hear(game(4), 100_000);
  await t.hear(game(6), 1000);                   // the next song key: song 4 never ended
  assert.deepEqual(ops(t.calls), ['playing', 'playing', 'playing']);
  await t.hear(game(6, { ended: true }), 31_000);
  assert.deepEqual(ops(t.calls), ['playing', 'playing', 'playing', 'scrobble']);
});

test('the Radio\'s track: its own tags and length, scrobbled at half of it', async () => {
  const folder = { tags: Promise.resolve(lines('Sigur Rós', 'Sigur Rós - Hoppípolla', 'Takk...')), plaque: Promise.resolve(lines('Sigur Ros', 'Hoppipolla', 'Takk...')), seconds: 268.4 };
  const t = setup({ folder });
  const song = { seq: 2, kind: 'folder', ended: false, lines: lines('Sigur Ros', 'Hoppipolla', 'Takk...') };
  await t.hear(song, 0);
  await t.hear(song, 1000);
  assert.deepEqual(t.calls[0], { op: 'playing', sk: 'SK', artist: 'Sigur Rós', track: 'Hoppípolla', album: 'Takk...', duration: 268 });
  await t.hear(song, 133_000);
  assert.deepEqual(ops(t.calls), ['playing']);
  await t.hear(song, 134_000);
  assert.deepEqual(ops(t.calls), ['playing', 'scrobble']);
  // a track the plaque does not show (another one served meanwhile): the game's lines, no length
  const other = { seq: 4, kind: 'folder', ended: false, lines: lines('Someone Else', 'Other Song') };
  await t.hear(other, 0);
  await t.hear(other, 2000);
  assert.deepEqual(t.calls[2], { op: 'playing', sk: 'SK', artist: 'Someone Else', track: 'Other Song' });
});

test('nothing for silence, a song without an artist, or without a session', async () => {
  const t = setup();
  await t.hear({ seq: 2, kind: 'none', ended: false, lines: lines('', '') }, 300_000);
  await t.hear({ seq: 4, kind: 'game', ended: false, lines: lines('', 'Title') }, 300_000);
  await t.hear(game(6), 300_000, null);
  assert.deepEqual(t.calls, []);
});

test('Last.fm out of reach: the scrobble is kept on this device and sent later; a scrobble older than two weeks is let go', async () => {
  let down = true;
  const t = setup({ answer: b => down && b.op === 'scrobble' ? { status: 503, json: { error: 'down' } } : { status: 200, json: { ok: true } } });
  await t.hear(game(2), 250_000);
  assert.deepEqual(ops(t.calls), ['playing', 'scrobble']);
  assert.equal(JSON.parse(t.kept['nfsu2.lastfm.queue']).length, 1, 'kept');
  down = false;
  await t.hear(game(2), 260_000);
  assert.deepEqual(ops(t.calls), ['playing', 'scrobble'], 'not again at once');
  await t.hear(game(2), 250_000 + 121_000);
  assert.deepEqual(ops(t.calls), ['playing', 'scrobble', 'scrobble'], 'sent after the wait');
  assert.equal(t.calls[2].timestamp, t.calls[1].timestamp);
  assert.equal(t.kept['nfsu2.lastfm.queue'], undefined, 'nothing kept');
  // a network that throws is out of reach too
  const u = setup({ answer: b => b.op === 'scrobble' ? new Error('offline') : { status: 200, json: {} } });
  await u.hear(game(2), 250_000);
  assert.equal(JSON.parse(u.kept['nfsu2.lastfm.queue']).length, 1);
  const old = setup();
  old.kept['nfsu2.lastfm.queue'] = JSON.stringify([{ artist: 'a', track: 't', timestamp: Math.round(T0 / 1000) - 15 * 86_400 }]);
  const o = new Scrobbler({ api: { call: async b => { old.calls.push(b); return { status: 200, json: {} }; } }, folder: () => null, dropped() {}, log() {}, now: () => T0 });
  o.tick(null, SESSION); await settle();
  assert.deepEqual(old.calls, [], 'too old for Last.fm');
});

test('a session key that stopped working: dropped, and nothing more is sent with it', async () => {
  const t = setup({ answer: () => ({ status: 401, json: { error: 'session' } }) });
  await t.hear(game(2), 1500);
  assert.equal(t.dropped(), 1);
  assert.deepEqual(ops(t.calls), ['playing']);
});

test('the link to Last.fm\'s page names this server\'s key and its way back', () => {
  assert.equal(authLink('K&1', 'https://nfsu2.example'), 'https://www.last.fm/api/auth/?api_key=K%261&cb=https%3A%2F%2Fnfsu2.example%2Flastfm.html');
});
