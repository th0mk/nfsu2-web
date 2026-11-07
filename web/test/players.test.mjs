// The server's /players (how many play on this server now, for the start page's Multiplayer settings) and the
// line the page makes of it:
//   node --test web/test/players.test.mjs
// Against the real server when the game folder is there: the three counts, whether it is full, how many wait in line
// and whether free roam is busy, nothing else in the body, never cached, the Host check of every route, null with
// --online off and --roam-hub off, full with --max-peers 1 (a page of before refused, a page of now in line).  The online count itself (logged in, or racing an online race) is in the
// online crate's tests (web/server-rs/crates/online/src/tests/lobby.rs), the free-roam one (driving there, or paused)
// in the roam crate's (web/server-rs/crates/roam/tests/roam_hub.rs).
// Ports: NFSU2_TEST_PLAYERS_PORT1 (8530), NFSU2_TEST_PLAYERS_PORT2 (8630).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import http from 'node:http';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { WebSocket } from 'ws';
import * as esbuild from 'esbuild';
import { serverCommand } from '../tools/server-cmd.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DATA = process.env.NFSU2_DATA || path.join(os.homedir(), 'projects/nsfu2/Need for Speed - Underground 2');
const P1 = +(process.env.NFSU2_TEST_PLAYERS_PORT1 || 8530), P2 = +(process.env.NFSU2_TEST_PLAYERS_PORT2 || 8630);
const SKIP = !fs.existsSync(path.join(DATA, 'SPEED2.EXE')) && 'no game folder (NFSU2_DATA)';
const sleep = ms => new Promise(r => setTimeout(r, ms));
/** what a wait gives, or a failure after ms (a message that never comes fails the test, and the server still stops) */
const within = (ms, what, p) => {
  let t;
  return Promise.race([p, new Promise((_, rej) => { t = setTimeout(() => rej(new Error(`no ${what} in ${ms} ms`)), ms); })]).finally(() => clearTimeout(t));
};

async function server(port, extra = []) {
  const st = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'nfsu2-players-test-'));
  const srv = spawn(...serverCommand(['--data', DATA, '--http-port', String(port), '--state', st, ...extra]),
    { stdio: ['ignore', 'pipe', 'pipe'] });
  await new Promise((res, rej) => {
    let out = '';
    const t = setTimeout(() => rej(new Error('server did not start: ' + out)), 15000);
    srv.stdout.on('data', d => { out += d; if (out.includes('  access    :')) { clearTimeout(t); res(); } });
    srv.on('exit', c => { clearTimeout(t); rej(new Error('server exited ' + c + ' ' + out)); });
  });
  return {
    get: (p, host = `localhost:${port}`) => new Promise((res, rej) => {
      http.get({ host: '127.0.0.1', port, path: p, headers: { host } }, r => {
        let body = ''; r.on('data', d => { body += d; }); r.on('end', () => res({ status: r.statusCode, headers: r.headers, body }));
      }).on('error', rej);
    }),
    // a browser's game on the switch: its welcome, then its hello
    player: name => within(5000, `welcome for ${name}`, new Promise(res => {
      const ws = new WebSocket(`ws://127.0.0.1:${port}/lan`);
      ws.on('message', (d, bin) => { if (!bin && JSON.parse(String(d)).t === 'welcome') { ws.send(JSON.stringify({ t: 'hello', name })); res(ws); } });
    })),
    stop: async () => {
      const gone = srv.exitCode !== null || srv.signalCode !== null ? null : new Promise(res => srv.once('exit', res));
      srv.kill(); await gone;
      fs.rmSync(st, { recursive: true, force: true });
    },
  };
}

test('/players: three counts, full, the line and roamBusy, nothing else, never cached, only for the server\'s own names', { skip: SKIP }, async () => {
  const s = await server(P1);
  try {
    let r = await s.get('/players');
    assert.equal(r.status, 200);
    assert.match(r.headers['content-type'], /^application\/json/);
    assert.match(r.headers['cache-control'], /no-store/);
    assert.equal(r.headers['cdn-cache-control'], 'no-store');
    assert.ok(r.headers['x-content-type-options'], 'the security headers of every response');
    assert.deepEqual(JSON.parse(r.body), { players: 0, online: 0, roam: 0, full: false, queue: 0, roamBusy: false });
    assert.deepEqual(Object.keys(JSON.parse(r.body)), ['players', 'online', 'roam', 'full', 'queue', 'roamBusy'], 'nothing else in the body');
    // a page in a room asks how many are in free roam there (the hub's count: web/server-rs/crates/roam/tests/roam_hub.rs)
    assert.deepEqual(JSON.parse((await s.get('/players?room=370129458')).body), { players: 0, online: 0, roam: 0, full: false, queue: 0, roamBusy: false, room: 0 });
    assert.deepEqual(JSON.parse((await s.get('/players?room=0')).body), { players: 0, online: 0, roam: 0, full: false, queue: 0, roamBusy: false }, 'room 0 is none');
    // two games on the switch, one leaves
    const a = await s.player('Alice'), b = await s.player('Bob');
    await sleep(100);
    assert.deepEqual(JSON.parse((await s.get('/players')).body), { players: 2, online: 0, roam: 0, full: false, queue: 0, roamBusy: false });
    b.close(); await sleep(300);
    r = await s.get('/players');
    assert.equal(r.body, '{"players":1,"online":0,"roam":0,"full":false,"queue":0,"roamBusy":false}', 'counts only: no names, addresses or times');
    a.close();
    // another site's name (DNS rebinding) gets what every route gives it
    assert.equal((await s.get('/players', 'evil.example')).status, 421);
    // HEAD: the headers, no body
    const head = await new Promise(res => http.request({ host: '127.0.0.1', port: P1, path: '/players', method: 'HEAD', headers: { host: `localhost:${P1}` } },
      x => { let n = 0; x.on('data', d => { n += d.length; }); x.on('end', () => res({ status: x.statusCode, n })); }).end());
    assert.deepEqual(head, { status: 200, n: 0 });
  } finally { await s.stop(); }
});

test('/players with --online off --roam-hub off: online and roam are null', { skip: SKIP }, async () => {
  const s = await server(P2, ['--online', 'off', '--roam-hub', 'off']);
  try {
    assert.deepEqual(JSON.parse((await s.get('/players')).body), { players: 0, online: null, roam: null, full: false, queue: 0, roamBusy: false });
  } finally { await s.stop(); }
});

test('/players with --max-peers 1: full while one plays, the next game of before gets "LAN full", one of now waits in line', { skip: SKIP }, async () => {
  const s = await server(P1, ['--max-peers', '1']);
  try {
    const a = await s.player('Alice');
    await sleep(100);
    assert.deepEqual(JSON.parse((await s.get('/players')).body), { players: 1, online: 0, roam: 0, full: true, queue: 0, roamBusy: false });
    const b = new WebSocket(`ws://127.0.0.1:${P1}/lan`);
    assert.deepEqual(await within(5000, 'close', new Promise(r => b.on('close', (k, why) => r([k, String(why)])))), [1013, 'LAN full']);
    // a page that can wait (?queue): its place in line, and the welcome once Alice is gone
    const c = new WebSocket(`ws://127.0.0.1:${P1}/lan?queue`), got = [];
    let gone = null;
    c.on('close', (k, why) => { gone = `closed ${k} ${why}`; });
    const next = t => within(5000, `"${t}"`, new Promise((res, rej) => {
      const f = () => { const m = got.find(m => m.t === t); if (m) res(m); else if (gone) rej(new Error(`no "${t}": ${gone}`)); else setTimeout(f, 20); };
      f();
    }));
    c.on('message', (d, bin) => { if (!bin) got.push(JSON.parse(String(d))); });
    const q = await next('queue');
    assert.deepEqual([q.pos, q.of, typeof q.ticket], [1, 1, 'string']);
    assert.deepEqual(JSON.parse((await s.get('/players')).body), { players: 1, online: 0, roam: 0, full: true, queue: 1, roamBusy: false });
    // Alice leaves as the page lets go of its place (web/src/net: 1000 "closed").  A link that just drops (no code)
    // keeps its place for a minute, for the page to come back to (the switch's reserve_ms): no place for the line
    a.close(1000, 'closed');
    await next('welcome');
    await sleep(100);
    assert.deepEqual(JSON.parse((await s.get('/players')).body), { players: 1, online: 0, roam: 0, full: true, queue: 0, roamBusy: false });
    c.close(1000, 'closed'); await sleep(300);
    assert.deepEqual(JSON.parse((await s.get('/players')).body), { players: 0, online: 0, roam: 0, full: false, queue: 0, roamBusy: false });
  } finally { await s.stop(); }
});

test('the page\'s numbers and lines', async () => {
  const out = await esbuild.build({ entryPoints: [path.join(HERE, '../src/app/players.ts')], bundle: true, write: false, format: 'esm', platform: 'neutral' });
  const { countText, fullLine, queueText, roomHash, roomFold, worldCounts } = await import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));
  // the room's hash as roam_core.c makes it (tests/roam_unit.c checks the same names): capitals, blanks and invisible
  // characters do not count
  assert.equal(roomHash('blueroom'), 0x160fba32);
  assert.equal(roomHash(' Blue Room\t'), 0x160fba32);
  assert.equal(roomHash(''), 0);
  assert.equal(roomHash('   '), 0);
  assert.equal(roomHash(' ​　'), 0, 'Unicode\'s blanks and the zero-width space are blanks');
  const fnv = s => { let h = 0x811c9dc5; for (const b of new TextEncoder().encode(s)) h = Math.imul(h ^ b, 0x01000193) >>> 0; return h || 1; };
  for (const same of [['blueroom', 'Blue Room', 'Blue​Room﻿', 'ＢＬＵＥ　Ｒｏｏｍ'], ['комната', 'КОМНАТА', 'Комната', 'ком ната'],
    ['zoë', 'ZOË', 'Zoë', 'z o ë'], ['istanbul', 'İSTANBUL', 'ıstanbul', 'Istanbul'], ['ομαδασ', 'ΟΜΑΔΑΣ', 'ομαδας', 'ΟΜΑ­ΔΑΣ'], ['łóź', 'ŁÓŹ', 'Łóź', 'łóź']]) {
    for (const n of same) assert.equal(roomHash(n), roomHash(same[0]), `${n} is ${same[0]}`);
    assert.equal(roomHash(same[0]), fnv(same[0]), `${same[0]}: small and without blanks already, its bytes' FNV-1a`);
  }
  assert.notEqual(roomHash('Zoe'), roomHash('Zoë'));
  assert.notEqual(roomHash('straße'), roomHash('strasse'));
  assert.equal(roomHash('Ό'), roomHash('ό'));
  assert.equal(roomHash('Café'), roomHash('café'), 'an accent typed after its letter: the same name (NFC)');
  // every character from U+0001 to U+2FFFF (but the surrogates) alone, as the game hashes it (without the page's NFC):
  // the same sum as tests/roam_unit.c's ROOM_FOLD_SUM, so the two tables agree on every one
  let sum = 0;
  for (let c = 1; c < 0x30000; c++) {
    if (c >= 0xd800 && c <= 0xdfff) continue;
    const f = roomFold(c), h = f ? fnv(String.fromCodePoint(f)) : 0;
    sum = (Math.imul(sum, 31) + h) >>> 0;
  }
  assert.equal(sum.toString(16), '198373d3');
  // the Playing now panel's numbers (grouped as the page's language writes them; '–' for one the server does not give),
  // the World list's counts by hash and the line under the numbers while the server is full
  assert.deepEqual([7592, 0, 3, null, undefined, -1, 1.5].map(countText), ['7,592', '0', '3', '–', '–', '–', '–']);
  assert.deepEqual([...worldCounts({ players: 3, online: 1, roam: 2, rooms: [1, 0, 1] }, [0, 7, 9])], [[0, 1], [7, 0], [9, 1]]);
  assert.deepEqual([...worldCounts({ players: 3, online: 1, roam: 2, rooms: [1, -1] }, [0, 7])], [[0, 1]], 'only counts');
  assert.deepEqual([...worldCounts({ players: 3, online: 1, roam: 2 }, [0, 7])], [], 'an older server: no counts');
  assert.equal(fullLine({ players: 3, online: 1, full: false }), '');
  assert.equal(fullLine({ players: 3, online: 1 }), '');
  assert.equal(fullLine({ players: 128, online: 9, roam: 100, full: true }), 'The server is full: multiplayer starts when a place frees up.');
  assert.equal(fullLine({ players: 5000, online: 300, roam: 1800, full: true, queue: 120 }),
    'The server is full, with 120 in line. At Play you can wait in line (playing alone until your turn) or play offline.');
  assert.match(fullLine({ players: 5000, online: 300, full: true, queue: 1 }), /full, with 1 in line/);
  assert.match(fullLine({ players: 5000, online: 300, full: true, queue: 0 }), /full: multiplayer starts when a place frees up\.$/);
  // the line over a game that waits for a place
  assert.equal(queueText({ pos: 1, of: 40 }), 'The server is full. In line for a place: next.');
  assert.equal(queueText({ pos: 1, of: 40, eta: 20 }), 'The server is full. In line for a place: next (less than a minute).');
  assert.equal(queueText({ pos: 2, of: 40, eta: 150 }), 'The server is full. In line for a place: 2nd of 40 (about 3 min).');
  assert.equal(queueText({ pos: 13, of: 40 }), 'The server is full. In line for a place: 13th of 40.');
  assert.equal(queueText({ pos: 23, of: 40, eta: 2 * 3600 }), 'The server is full. In line for a place: 23rd of 40 (about 2 h).');
  assert.equal(queueText({ pos: 101, of: 140 }), 'The server is full. In line for a place: 101st of 140.');
  assert.equal(queueText({ pos: 112, of: 140 }), 'The server is full. In line for a place: 112th of 140.');
});
