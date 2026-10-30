// The server's fixes for the security and privacy audit, one test per finding,
// against the real server (out/web/server-rs/nfsu2-server, web/tools/server-cmd.mjs):
//   node --test --test-concurrency=1 web/test/audit.test.mjs
// - F1:  ?vip= is ignored; a dropped player's address goes back only to its token (/lan?resume); a client that
//        opens /lan over and over gets 429; the console names no client address
// - F3:  one client behind a proxy gets at most 64 requests at once (429 above), one IPv6 /48 at most 128 however
//        many /56s it uses, the last quarter of the budget is only for clients that hold nothing, others are
//        not locked out, and a download the client stopped reading (or reads a trickle of) is let go
// - F4:  with --tls-cert, no certificate for a handshake without the server's name (SNI) or with another;
//        a Host that is not its name gets 421
// - F7, PRIV-4: a DNS-rebinding page (any other Host) gets 421 everywhere, the /lan upgrade included
// - PRIV-1: the game folder is served and listed only as far as it is the game (the real folder's installer
//        log, SAVE and mod scripts never)
// - DEP-1: the server starts and serves in every mode, reading no network interfaces (systemd without AF_NETLINK)
// - DEP-4: a client's refused upgrades cost the console two lines a minute, not one each
// - DEP-8: the state folder the server makes is private (0700), the files in it 0600
// Ports: NFSU2_TEST_AUDIT_PORT1 (8529), NFSU2_TEST_AUDIT_PORT2 (8629): not deploy.test.mjs's, so the two can
// run at once.  Needs the game folder (NFSU2_DATA) and a built out/web/nfsu2.wasm; skipped without them.
// NFSU2_SERVER_BIN=<binary>: another server (a negative control: a server without the audit's fixes fails these
// tests).  Options that are new here go by environment (NFSU2_HOST), which an older one ignores.
// NFSU2_TEST_GAME=mw: the same against Most Wanted's server (--game mw) with a made-up game folder (game-fixture.mjs),
// every route under /mw; PRIV-1 then names Most Wanted's own (scripts, Support, Uninstall, 00000000.256).  Its server
// keeps no keyphrase saves (its /vault goes on to the U2 box's, vault_forward.rs), so the F3 test that holds saves is
// Underground 2's alone (the budget is the same code for both games).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import net from 'node:net';
import tls from 'node:tls';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn, execFileSync } from 'node:child_process';
import { WebSocket } from 'ws';
import { serverCommand } from '../tools/server-cmd.mjs';
import { G } from './game-fixture.mjs';

const DATA = G.data, B = G.base;
const P1 = +(process.env.NFSU2_TEST_AUDIT_PORT1 || 8529), P2 = +(process.env.NFSU2_TEST_AUDIT_PORT2 || 8629);
const TMP = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'nfsu2-audit-test-'));
test.after(() => fs.rmSync(TMP, { recursive: true, force: true }));
const SKIP = G.skip;
const sleep = ms => new Promise(r => setTimeout(r, ms));
const ssl = (...a) => execFileSync('openssl', a, { stdio: ['ignore', 'pipe', 'pipe'] }).toString();
const HOST = 'nfsu2.test';

async function startServer(args, { env = {}, state = fs.mkdtempSync(path.join(TMP, 'state-')), expectExit = false } = {}) {
  const c = spawn(...serverCommand(['--data', DATA, ...G.args, '--state', state, ...args]), { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ...env } });
  let out = '';
  c.stdout.on('data', d => { out += d; }); c.stderr.on('data', d => { out += d; });
  const exited = new Promise(r => c.on('exit', code => r(code)));
  const ready = await new Promise(res => {
    const t = setTimeout(() => res(false), 20000);
    const check = () => { if (out.includes('  access    :')) { clearTimeout(t); setTimeout(() => res(true), 300); } };
    c.stdout.on('data', check);
    exited.then(() => { clearTimeout(t); res(false); });
  });
  if (expectExit === false && !ready) { c.kill(); throw new Error(`server did not start:\n${out}`); }
  return { c, state, ready, out: () => out, exited, stop: async () => { c.kill('SIGTERM'); await exited; } };
}
function request(port, { method = 'GET', p = '/', host = `localhost:${port}`, headers = {}, useTls = false, servername, timeout = 10000 } = {}) {
  // (a 101 answer leaves the connection open: the head is enough)
  return new Promise((res, rej) => {
    const sock = useTls ? tls.connect({ host: '127.0.0.1', port, servername, rejectUnauthorized: false }) : net.connect(port, '127.0.0.1');
    const chunks = [];
    const t = setTimeout(() => { sock.destroy(); res({ status: 0, timedOut: true }); }, timeout);
    sock.on(useTls ? 'secureConnect' : 'connect', () => sock.write(`${method} ${p.startsWith('/') ? B + p : p} HTTP/1.1\r\nHost: ${host}\r\nConnection: close\r\n` +
      Object.entries(headers).map(([k, v]) => `${k}: ${v}\r\n`).join('') + '\r\n'));
    sock.on('data', d => { chunks.push(d); if (/^HTTP\/1\.1 101 /.test(String(chunks[0])) && Buffer.concat(chunks).includes('\r\n\r\n')) sock.destroy(); });
    sock.on('error', e => { clearTimeout(t); chunks.length ? done() : res({ status: 0, error: e.code ?? e.message }); });
    sock.on('close', () => { clearTimeout(t); done(); });
    const done = () => {
      const b = Buffer.concat(chunks), end = b.indexOf('\r\n\r\n');
      res({ status: +String(b.subarray(0, 12)).split(' ')[1] || 0, head: String(b.subarray(0, end)), body: b.subarray(end + 4) });
    };
  });
}
// a /lan client behind the proxy, as the client at address xff
function lanClient(port, xff, { q = '', hello = null, name = 'player' } = {}) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}${B}/lan${q}`, { headers: { Host: HOST, 'X-Forwarded-For': xff } });
  const c = { ws, ctl: [], frames: [], status: 0 };
  c.opened = new Promise(res => {
    ws.on('open', () => { if (hello) ws.send(JSON.stringify(hello)); res(true); });
    ws.on('unexpected-response', (q, r) => { c.status = r.statusCode; res(false); });
    ws.on('error', () => res(false));
  });
  c.welcome = new Promise(res => {
    ws.on('message', (d, bin) => {
      if (bin) { c.frames.push(Buffer.from(d)); return; }
      const m = JSON.parse(String(d)); c.ctl.push(m);
      if (m.t === 'welcome') { if (!hello) ws.send(JSON.stringify({ t: 'hello', name })); res(m); }
    });
    ws.on('close', () => res(null));
  });
  return c;
}

test('F1: ?vip= is ignored, a dropped player\'s address goes back only to its token; /lan opened too fast gets 429; no client address on the console', { skip: SKIP, timeout: 90_000 }, async () => {
  const s = await startServer(['--behind-proxy', '--http-port', String(P2)], { env: { NFSU2_HOST: HOST } });
  try {
    const alice = lanClient(P2, '198.51.100.1', { name: 'Alice' }), bob = lanClient(P2, '198.51.100.2', { name: 'Bob' });
    await alice.welcome; const wb = await bob.welcome;
    assert.match(wb.token ?? '', /^[A-Za-z0-9_-]{22}$/, 'the welcome carries a token');
    bob.ws.terminate();
    await sleep(200);
    // Mallory asks for Bob's address every way it can: ?vip=, a resume with a made-up token, a plain join
    const tries = [lanClient(P2, '203.0.113.9', { q: `?vip=${wb.vipStr}` }),
      lanClient(P2, '203.0.113.9', { q: '?resume', hello: { t: 'hello', name: 'Bob', vip: wb.vipStr, token: 'B'.repeat(22) } }),
      lanClient(P2, '203.0.113.9')];
    const got = (await Promise.all(tries.map(t => t.welcome))).map(w => w?.vipStr);
    console.log(`Bob had ${wb.vipStr}; Mallory's tries got ${got.join(', ')}`);
    assert.ok(got.every(v => v && v !== wb.vipStr), 'never Bob\'s address');
    const bob2 = lanClient(P2, '198.51.100.22', { q: '?resume', hello: { t: 'hello', name: 'Bob', vip: wb.vipStr, token: wb.token } });
    assert.equal((await bob2.welcome)?.vipStr, wb.vipStr, 'Bob, with his token, from another address');
    for (const c of [alice, bob2, ...tries]) c.ws.terminate();
    // a client that opens and closes /lan as fast as it can: a burst, then 429 (the review of F1: churning)
    const codes = {};
    for (let i = 0; i < 40; i++) {
      const c = lanClient(P2, '203.0.113.50');
      const ok = await c.opened;
      codes[ok ? 101 : c.status] = (codes[ok ? 101 : c.status] ?? 0) + 1;
      c.ws.terminate();
    }
    const other = lanClient(P2, '203.0.113.51');
    const otherOk = await other.opened; other.ws.terminate();
    console.log(`40 /lan openings in a row from one client: ${JSON.stringify(codes)}; another client meanwhile: ${otherOk ? 101 : other.status}`);
    assert.ok(codes[429] >= 15 && codes[101] >= 15, 'a burst, then 429');
    assert.ok(otherOk, 'another client is not affected');
    await sleep(300);
    const lanLines = s.out().split('\n').filter(l => l.startsWith('[lan]'));
    assert.ok(lanLines.some(l => / is back /.test(l)) && lanLines.some(l => /too fast/.test(l)), lanLines.join('\n'));
    assert.deepEqual(lanLines.filter(l => /198\.51\.100|203\.0\.113/.test(l)), [], 'no client address on the console');
  } finally { await s.stop(); }
});

test('F7 / PRIV-4: a page that rebinds its own name to this machine gets 421 everywhere, /lan included', { skip: SKIP, timeout: 60_000 }, async () => {
  const s = await startServer(['--http-port', String(P2)]);
  try {
    const evil = `rebind.attacker.example:${P2}`;
    const got = {};
    for (const p of ['/', '/players', '/data-manifest', `/data/${G.exe}`, '/game.json'])
      got[p] = (await request(P2, { p, host: evil })).status;
    const up = { Upgrade: 'websocket', Connection: 'Upgrade', 'Sec-WebSocket-Version': '13', 'Sec-WebSocket-Key': crypto.randomBytes(16).toString('base64') };
    got['/lan'] = (await request(P2, { p: '/lan', host: evil, headers: { ...up, Origin: `http://${evil}` } })).status;
    console.log('rebinding Host:', JSON.stringify(got));
    for (const [p, code] of Object.entries(got)) assert.equal(code, 421, `${p} with Host ${evil}`);
    // this machine's names still work; another local site's page still may not join
    assert.equal((await request(P2, { p: '/game.json' })).status, 200);
    assert.equal((await request(P2, { p: '/game.json', host: `127.0.0.1:${P2}` })).status, 200);
    assert.equal((await request(P2, { p: '/game.json', host: `[::1]:${P2}` })).status, 200);
    assert.equal((await request(P2, { p: '/lan', headers: { ...up, Origin: 'http://localhost:3000' } })).status, 403, 'another local origin');
    assert.equal((await request(P2, { p: '/lan', headers: { ...up, Origin: `http://localhost:${P2}` } })).status, 101, 'its own page');
    assert.equal((await request(P2, { p: 'http://evil.example/game.json' })).status, 421, 'an absolute URL for another host');
    const h10 = await new Promise(res => { const so = net.connect(P2, '127.0.0.1', () => so.write('GET /game.json HTTP/1.0\r\n\r\n')); let b = ''; so.on('data', d => { b += d; }); so.on('close', () => res(+b.split(' ')[1])); });
    assert.equal(h10, 421, 'HTTP/1.0 without a Host');
  } finally { await s.stop(); }
});

test('F4: with --tls-cert, a handshake without the server\'s name or with another one gets no certificate', { skip: SKIP, timeout: 60_000 }, async () => {
  const dir = fs.mkdtempSync(path.join(TMP, 'cert-'));
  ssl('req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes', '-keyout', path.join(dir, 'k.pem'), '-out', path.join(dir, 'c.pem'),
    '-days', '5', '-subj', '/CN=game.test', '-addext', 'subjectAltName=DNS:game.test,DNS:*.wild.test');
  const s = await startServer(['--port', String(P1), '--http-port', '0', '--bind', '127.0.0.1', '--tls-cert', path.join(dir, 'c.pem'), '--tls-key', path.join(dir, 'k.pem')]);
  const hello = servername => new Promise(res => {
    const so = tls.connect({ host: '127.0.0.1', port: P1, servername, rejectUnauthorized: false }, () => { res(so.getPeerX509Certificate()?.subject ?? 'none'); so.destroy(); });
    so.on('error', e => res(`error ${e.code ?? e.message}`));
  });
  try {
    const r = { none: await hello(undefined), other: await hello('scan.invalid'), own: await hello('game.test'), wild: await hello('a.wild.test') };
    console.log('handshakes:', JSON.stringify(r));
    assert.match(r.none, /^error/, 'no SNI: no certificate');
    assert.match(r.other, /^error/, 'another name: no certificate');
    assert.equal(r.own, 'CN=game.test');
    assert.equal(r.wild, 'CN=game.test', 'a wildcard name of the certificate');
    const ok = await request(P1, { useTls: true, servername: 'game.test', host: 'game.test', p: '/players' });
    const odd = await request(P1, { useTls: true, servername: 'game.test', host: 'elsewhere.example', p: '/' });
    console.log(`Host game.test: ${ok.status}, another name: ${odd.status}`);
    assert.equal(ok.status, 200);
    assert.equal(odd.status, 421);
  } finally { await s.stop(); }
});

test(`PRIV-1: of the game folder only the game is listed and served (${G.private.join(', ')} never)`, { skip: SKIP, timeout: 60_000 }, async () => {
  const s = await startServer(['--http-port', String(P2)]);
  try {
    const m = JSON.parse((await request(P2, { p: '/data-manifest' })).body);
    const top = [...new Set(m.files.map(f => f.p.split('/')[0]))].sort();
    console.log(`manifest: ${m.files.length} entries; top level: ${top.join(' ')}`);
    const allowed = new Set(G.id === 'ug2' ? ['speed2.exe', 'server.dll', 'server.cfg', 'cars', 'credits', 'frontend', 'global', 'langpacks', 'languages', 'memcard', 'movies', 'nis',
      'pfdata', 'sound', 'speech', 'subtitles', 'texpacks', 'tracks']
      : ['speed.exe', 'server.dll', 'server.cfg', 'cars', 'credits', 'frontend', 'global', 'languages', 'memcard', 'movies', 'nis', 'sound', 'subtitles', 'tracks']);
    assert.deepEqual(top.filter(t => !allowed.has(t.toLowerCase())), [], 'nothing but the game\'s own entries');
    const never = G.id === 'ug2' ? ['unins000.dat', 'unins000.exe', 'SAVE', 'scripts/NFSUnderground2.WidescreenFix.ini', 'dinput8.dll', 'SDATA.Backup/sdat.viv', '_How To Play!!.txt']
      : ['SAVE', 'SAVE/NFS Most Wanted/profile', 'scripts', 'scripts/mod.ini', 'Support', 'Support/dxdiag.log', 'Uninstall', 'Uninstall/uninstall.log', '00000000.256', 'dinput8.dll'];
    if (G.id === 'mw') for (const p of G.private) assert.ok(fs.existsSync(path.join(DATA, p)), `the made-up folder has ${p}`);
    for (const p of never)
      if (fs.existsSync(path.join(DATA, p))) {
        assert.equal((await request(P2, { p: '/data/' + p.split('/').map(encodeURIComponent).join('/') })).status, 404, p);
        assert.ok(!m.files.some(f => f.p.toLowerCase() === p.toLowerCase() || f.p.toLowerCase().startsWith(p.toLowerCase() + '/')), `${p} listed`);
      }
    assert.equal((await request(P2, { p: `/data/${G.exe}`, headers: { Range: 'bytes=0-1' } })).status, 206);
    assert.equal((await request(P2, { p: '/data/GLOBAL/GLOBALB.LZC', headers: { Range: 'bytes=0-1' } })).status, fs.existsSync(path.join(DATA, 'GLOBAL/GLOBALB.LZC')) ? 206 : 404);
  } finally { await s.stop(); }
});

// the upstream side of a proxy: one connection per request, each for the client at xff(i), never read
const BIG = B + G.big;
// (answers: 200 held, 429 refused, closed: the connection closed with no answer (a refusal that raced the
// close, past the port's maxConnections, or past the 128 a Mac queues for the server to accept), other: no answer
// in 20 s)
async function hold(port, n, xff, socks, p = BIG) {
  const answers = { 200: 0, 429: 0, closed: 0, other: 0 };
  await Promise.all(Array.from({ length: n }, (_, i) => new Promise(res => {
    const so = net.connect(port, '127.0.0.1');
    socks.push(so);
    let done = false;
    const end = k => { if (done) return; done = true; clearTimeout(t); answers[k in answers ? k : 'other']++; res(); };
    const t = setTimeout(() => end('other'), 20000);
    so.on('error', () => {});
    so.once('connect', () => so.write(`GET ${p} HTTP/1.1\r\nHost: ${HOST}\r\nX-Forwarded-For: ${xff(i)}\r\n\r\n`));
    so.once('data', d => { so.pause(); end(+String(d).split(' ')[1]); });
    so.once('close', () => end('closed'));
  })));
  return answers;
}
// Requests in progress that hold nothing in the kernel: saves (PUT /vault) whose body never comes, each for the client
// at xff(i).  An unread download fills its socket buffers on both ends, about 1 MB on a Mac's loopback: the ~800 that
// F3 held at once took 0.9-1.0 GB of the kernel's network memory (netstat -m: some 60,000 requests for memory denied
// in each run), the server lost some of the held ones, so the fill fell short of 768, and other tests' connections
// failed meanwhile.  Saves hold the shares now, downloads only the few past the vault's own limit (60 writes from a
// client in a burst, 120 from a site).  "100 Continue" goes out just before the request reaches the budget (nfsu2_http
// conn.rs), a refusal right after it; a save with no answer past it holds its share until the server's request
// timeout (30 s).  (answers: held, 429 refused, other: anything else, or nothing in 20 s; first: the first other's
// status line)
// They connect 64 at a time, each wave once the last one was answered: a Mac queues at most 128 connections the server
// has not accepted yet (kern.ipc.somaxconn) and resets the rest, so 768 at once lost every one past 128 the moment
// the server's accept loop fell a little behind (on a busy machine the fill once got 677 of 768, the others closed
// without a byte; with the server stopped for 300 ms, 128).
async function holdSaves(port, n, xff, socks) {
  const save = i => new Promise(res => {
    const so = net.connect(port, '127.0.0.1');
    socks.push(so);
    let text = '';
    const t = setTimeout(() => res(() => text), 20000);
    so.on('error', () => {});
    so.once('connect', () => so.write(`PUT ${B}/vault HTTP/1.1\r\nHost: ${HOST}\r\nX-Forwarded-For: ${xff(i)}\r\n` +
      `Authorization: Bearer ${'A'.repeat(43)}\r\nIf-Match: "${'0'.repeat(15)}1"\r\nContent-Length: 1024\r\nExpect: 100-continue\r\n\r\n`));
    so.on('data', d => { text += d; clearTimeout(t); res(() => text); });
    so.once('close', () => { clearTimeout(t); res(() => text); });
  });
  const got = [];
  for (let i = 0; i < n; i += 64) got.push(...await Promise.all(Array.from({ length: Math.min(64, n - i) }, (_, j) => save(i + j))));
  await sleep(500);
  const answers = { held: 0, 429: 0, other: 0 };
  for (const text of got.map(f => f())) {
    const after = text.replace(/^HTTP\/1\.1 100 Continue\r\n\r\n/, '');
    const k = text === after ? 'other' : after === '' ? 'held' : after.startsWith('HTTP/1.1 429 ') ? 429 : 'other';
    answers[k]++;
    if (k === 'other') answers.first ??= text.split('\r\n')[0] || '(nothing)';
  }
  return answers;
}
const xffOther = (port, ip) => request(port, { p: '/players', host: HOST, headers: { 'X-Forwarded-For': ip } });
test('F3: one client behind a proxy holds at most 64 requests at once, one IPv6 /48 at most 128; the last quarter is for newcomers', { skip: SKIP || (G.id === 'mw' && 'Most Wanted\'s server keeps no saves to hold (its /vault is the U2 box\'s): the same budget code as Underground 2\'s'), timeout: 180_000 }, async () => {
  const s = await startServer(['--behind-proxy', '--http-port', String(P2)], { env: { NFSU2_HOST: HOST } });
  await sleep(1500);                                   // the module's compression at start
  let socks = [];
  const free = () => { for (const so of socks) so.destroy(); socks = []; return sleep(500); };
  try {
    const a0 = await holdSaves(P2, 60, () => '203.0.113.66', socks);
    const a = await hold(P2, 140, () => '203.0.113.66', socks);
    const other = await xffOther(P2, '198.51.100.7');
    console.log(`one client: 60 saves ${JSON.stringify(a0)}, then 140 downloads ${JSON.stringify(a)}; another client meanwhile: ${other.status}`);
    assert.equal(a0.other, 0, `every save held or refused (${a0.first})`);
    assert.ok(a0.held + a[200] <= 64, `at most 64 at once for one client (${a0.held} + ${a[200]})`);
    assert.ok(a0[429] + a[429] + a.closed >= 130, `the rest refused (${a0[429]} + ${a[429]} + ${a.closed})`);
    assert.equal(other.status, 200, 'another client is not locked out');
    await free();
    // the review of F3: one /48 spread over 17 /56s, 64 each (the /56 budgets alone gave it 16 x 64 = 1024)
    const xff6 = i => `2001:db8:77:${(i % 17).toString(16)}00::${(i >> 4) + 1}`;
    const v0 = await holdSaves(P2, 120, xff6, socks);
    const v6 = await hold(P2, 17 * 64 - 120, i => xff6(i + 120), socks);
    const other6 = await xffOther(P2, '2001:db8:99::1');
    console.log(`17 x 64 from one /48: 120 saves ${JSON.stringify(v0)}, then downloads ${JSON.stringify(v6)}; another /48 meanwhile: ${other6.status}`);
    assert.equal(v0.other, 0, `every save held or refused (${v0.first})`);
    assert.ok(v0.held + v6[200] <= 128, `at most 128 at once for one /48 (${v0.held} + ${v6[200]})`);
    assert.equal(other6.status, 200);
    await free();
    // the last quarter of the 1024: a client that already holds one, under its own budget, may not take it
    // (24 clients fill it with 32 saves each: under the vault's own limit of 60 writes per client in a burst)
    const early = await hold(P2, 1, () => '198.51.100.9', socks);
    const saves = await holdSaves(P2, 24 * 32, i => `192.0.2.${(i % 24) + 1}`, socks);
    const held = early[200] + saves.held;
    const late = await hold(P2, 10, () => '2001:db8:b0::1', socks);
    const holder = await hold(P2, 1, () => '198.51.100.9', socks);
    const newcomer = await xffOther(P2, '198.51.100.10');
    console.log(`held ${held} of 1024 (saves: ${JSON.stringify(saves)}); then a new /48 got ${JSON.stringify(late)}, a client that held one got ${JSON.stringify(holder)}, a newcomer ${newcomer.status}`);
    assert.ok(held >= 768, `filled up to the last quarter (${held})`);
    assert.equal(saves.other, 0, `every save held or refused (${saves.first})`);
    assert.equal(late[200], 1, 'a client that holds nothing gets one of the last quarter, then no more');
    assert.equal(holder[200], 0, 'one that holds some gets none of it');
    assert.equal(newcomer.status, 200, 'a newcomer is never locked out');
  } finally { await free(); await s.stop(); }
});

test('F3: a download the client stopped reading, or reads only a trickle of, is let go', { skip: SKIP, timeout: 180_000 }, async () => {
  const s = await startServer(['--behind-proxy', '--http-port', String(P2)], { env: { NFSU2_HOST: HOST } });
  await sleep(1500);
  const socks = [];
  try {
    const a = await hold(P2, 20, i => `203.0.113.${i + 1}`, socks);
    assert.equal(a[200], 20);
    // three that read 1.2 KB a second (the review's slowread.py)
    let trickled = 0;
    for (let i = 0; i < 3; i++) {
      const so = net.connect(P2, '127.0.0.1');
      socks.push(so); so.on('error', () => {});
      so.once('connect', () => so.write(`GET ${BIG} HTTP/1.1\r\nHost: ${HOST}\r\nX-Forwarded-For: 198.51.100.${i + 40}\r\n\r\n`));
      so.pause();
      const t = setInterval(() => { trickled += so.read(1200)?.length ?? 0; }, 1000); so.on('close', () => clearInterval(t));
    }
    await sleep(1500);
    // seen from the server (its connections on the port): a client that does not read never sees the close
    const established = () => execFileSync('lsof', ['-nP', '-a', '-p', String(s.c.pid), `-iTCP:${P2}`, '-sTCP:ESTABLISHED']).toString()
      .split('\n').filter(l => l.includes('ESTABLISHED')).length;
    const lsof = () => { try { return established(); } catch { return 0; } };      // lsof exits 1 when it lists nothing
    const t0 = Date.now(), first = lsof();
    let now = first;
    while (now > 0 && Date.now() - t0 < 100_000) { await sleep(2000); now = lsof(); }
    console.log(`the server's connections for 20 stalled and 3 trickling downloads (${trickled} bytes read): ${first} at first, ${now} after ${((Date.now() - t0) / 1000).toFixed(0)} s`);
    assert.ok(first >= 23, `held at first (${first})`);
    assert.equal(now, 0, 'every stalled and trickling download let go');
    assert.ok(Date.now() - t0 < 60_000);
  } finally { for (const so of socks) so.destroy(); await s.stop(); }
});

test('DEP-1: the server starts and serves in every mode (it reads no network interfaces: systemd without AF_NETLINK)', { skip: SKIP, timeout: 90_000 }, async () => {
  const dir = fs.mkdtempSync(path.join(TMP, 'cert-'));
  ssl('req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes', '-keyout', path.join(dir, 'k.pem'), '-out', path.join(dir, 'c.pem'),
    '-days', '5', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost');
  const modes = {
    'A, D (behind a proxy)': [['--behind-proxy', '--http-port', String(P2)], { NFSU2_HOST: HOST }, () => request(P2, { p: '/players' })],
    'B (a certificate)': [['--port', String(P1), '--http-port', '0', '--bind', '127.0.0.1', '--tls-cert', path.join(dir, 'c.pem'), '--tls-key', path.join(dir, 'k.pem')], {},
      () => request(P1, { useTls: true, servername: 'localhost', p: '/players' })],
    'this machine only': [['--http-port', String(P2)], {}, () => request(P2, { p: '/players' })],
  };
  for (const [name, [args, env, probe]] of Object.entries(modes)) {
    const s = await startServer(args, { env, expectExit: true });
    const r = s.ready ? await probe() : { status: 0 };
    const code = s.ready ? 'running' : `exited ${await s.exited}`;
    console.log(`${name}: ${code}, /players ${r.status}${s.ready ? '' : `\n${s.out().split('\n').filter(l => /Error|error/.test(l)).slice(0, 2).join('\n')}`}`);
    if (s.ready) await s.stop();
    assert.ok(s.ready, `${name}: starts`);
    assert.equal(r.status, 200, `${name}: serves`);
  }
});

test('DEP-4: refused /lan upgrades from one client cost the console a few lines, not one each', { skip: SKIP, timeout: 90_000 }, async () => {
  const s = await startServer(['--http-port', String(P2)]);
  const held = [];
  try {
    for (let i = 0; i < 8; i++) { const ws = new WebSocket(`ws://127.0.0.1:${P2}${B}/lan`); held.push(ws); await new Promise(r => ws.once('open', r)); }
    const before = s.out().split('\n').length;
    const up = `GET ${B}/lan HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\n\r\n`;
    let refused = 0;
    for (let i = 0; i < 2000; i += 50) {
      await Promise.all(Array.from({ length: 50 }, () => new Promise(res => {
        const so = net.connect(P2, '127.0.0.1', () => so.write(up));
        so.on('data', d => { if (String(d).startsWith('HTTP/1.1 429')) refused++; so.destroy(); res(); });
        so.on('error', res); so.on('close', res);
      })));
    }
    await sleep(300);
    const lines = s.out().split('\n').slice(before - 1).filter(l => l.includes('refused'));
    console.log(`${refused} upgrades refused with 429; console lines about them: ${lines.length}: ${JSON.stringify(lines)}`);
    assert.ok(refused >= 1900);
    assert.ok(lines.length >= 1 && lines.length <= 3, `the first ones shown, then at most a few lines a minute (${lines.length})`);
    // the joins of the 8 connections are still each on the console (another topic, its own budget), without an address
    const joins = s.out().split('\n').filter(l => /^\[lan\] 10\.77\.[0-7]\.\d+ joined/.test(l));
    assert.equal(joins.length, 8);
    assert.deepEqual(s.out().split('\n').filter(l => l.startsWith('[lan]') && /127\.0\.0\.1|::1/.test(l)), [], 'no client address');
  } finally { for (const ws of held) ws.terminate(); await s.stop(); }
});

test('DEP-8: the state folder the server makes is its own (0700), the files in it 0600', { skip: SKIP, timeout: 60_000 }, async () => {
  const state = path.join(TMP, 'fresh-state');
  const s = await startServer(['--http-port', String(P2)], { state, env: {} });
  try {
    const ws = new WebSocket(`ws://127.0.0.1:${P2}${B}/lan`);
    await new Promise(r => ws.once('message', r));
    await sleep(800);                                  // the switch stores its addresses shortly after a change
    const mode = f => (fs.statSync(path.join(state, f)).mode & 0o777).toString(8);
    const modes = { state: (fs.statSync(state).mode & 0o777).toString(8), 'etag.key': mode('etag.key'), 'lan-addresses.json': mode('lan-addresses.json') };
    console.log('modes:', JSON.stringify(modes));
    assert.equal(modes.state, '700');
    for (const k of ['etag.key', 'lan-addresses.json']) assert.equal(modes[k], '600', k);
    ws.terminate();
  } finally { await s.stop(); }
});
