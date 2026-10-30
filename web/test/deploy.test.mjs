// The server as it would face the internet (out/web/server-rs/nfsu2-server, web/tools/server-cmd.mjs):
//   node --test --test-concurrency=1 web/test/deploy.test.mjs
// - the no-leak crawl: every route and error path, in each TLS mode (none:
//   this machine only, a certificate, behind a Cloudflare-like proxy, --dev)
//   and under a base path (--base-path localhost=/u2): no host name,
//   local address, home folder, user name, absolute path, Node version or
//   stack trace in any status line, header, body, WebSocket message or close
//   reason, and the security and cache headers on every response
// - path traversal (encoded dots, backslashes, double encoding, NUL, case,
//   dot files, symbolic links out of the folder)
// - limits (methods, bodies, header size, slow headers, Expect, /lan per
//   address, WebSocket message size, players)
// - --dev gating and the start-up refusals
// - trusted proxies: CF-Connecting-IP / X-Forwarded-* believed only from a
//   trusted peer (checked from this machine's LAN address, which is not one)
// - HSTS only over a real certificate or a proxy that says https
// - the Cloudflare simulation (web/test/cf-sim.mjs): TLS at the proxy, real
//   client address from CF-Connecting-IP, byte ranges and the brotli module
//   through it, the switch's WebSocket kept alive past a 40 s idle cut
// - availability (the phase-2 deploy review): a WebSocket ping flood, queued
//   (pipelined) requests, TLS handshakes that never finish, connections per
//   address, the keep-alive of peers that are not Cloudflare's, IPv6
// The tests run one after another, except the three that wait on the server's
// own clocks (the keep-alives of 65 and 905 s, the proxy's idle cut, the
// once-a-minute certificate check): they run beside the others, each on ports
// of its own (one after another the file took 271 s).
// Ports: NFSU2_TEST_PORT1 (8527), NFSU2_TEST_PORT2 (8627), and for the three
// beside them NFSU2_TEST_PORT3 to 6 (8535 to 8538); temporary files in
// $TMPDIR.  Needs the game folder (NFSU2_DATA) and a built web/dist and
// out/web/nfsu2.wasm; skipped without them.
// NFSU2_TEST_GAME=mw: the same against Most Wanted's server (--game mw) with a made-up game folder and module
// (game-fixture.mjs) and its built page (web/dist-mw: node build.mjs --game mw): every route under /mw (the requests'
// paths go there; the crawl also asks the root's and the bare /mw), its program speed.exe, its test scripts tests/mw;
// it has no robots.txt or other root file of its own and no --base-path case (UG2's).
import test, { describe } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import net from 'node:net';
import tls from 'node:tls';
import path from 'node:path';
import zlib from 'node:zlib';
import crypto from 'node:crypto';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { WebSocket, WebSocketServer } from 'ws';
import { makeTestCerts, startCfSim } from './cf-sim.mjs';
import { serverCommand } from '../tools/server-cmd.mjs';
import { G } from './game-fixture.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '../..');
// NFSU2_SERVER_BIN / NFSU2_TEST_READY: another server and its start-up line (a negative control: the crawl
// against a server before a fix lists what it gave away)
const READY = process.env.NFSU2_TEST_READY || '  access    :';
// a server from a bundle (web/deploy/pack.sh, NFSU2_REPO its folder) has no test pages, test scripts or source maps
// even with --dev
const SERVED = process.env.NFSU2_REPO;
const DIST = G.id === 'ug2' ? 'web/dist' : `web/dist-${G.id}`, TESTS = G.id === 'ug2' ? 'tests' : `tests/${G.id}`;
const AIDS = fs.existsSync(path.join(SERVED, `${DIST}-dev/test`)) && fs.existsSync(path.join(SERVED, TESTS));
const DATA = G.data;
// the game's routes' base on this machine ('' for Underground 2, '/mw' for Most Wanted), its program, a test script
const B = G.base, EXE = G.exe, SCRIPT = G.id === 'ug2' ? 'quickrace.txt' : 'boot_menu.txt';
const P1 = +(process.env.NFSU2_TEST_PORT1 || 8527), P2 = +(process.env.NFSU2_TEST_PORT2 || 8627);
// the keep-alive test's, the renewed certificate's, and the Cloudflare simulation's two (the proxy, the server)
const P3 = +(process.env.NFSU2_TEST_PORT3 || 8535), P4 = +(process.env.NFSU2_TEST_PORT4 || 8536);
const P5 = +(process.env.NFSU2_TEST_PORT5 || 8537), P6 = +(process.env.NFSU2_TEST_PORT6 || 8538);
const TMP = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'nfsu2-deploy-test-'));
const CERTS = makeTestCerts(path.join(TMP, 'certs'));
const SKIP = G.skip || (!fs.existsSync(path.join(REPO, DIST, 'main.js')) && `no ${DIST} (node build.mjs${G.id === 'ug2' ? '' : ` --game ${G.id}`})`);
// this machine's LAN address: a peer the server does not trust
const LAN_IP = Object.values(os.networkInterfaces()).flat().find(a => a && a.family === 'IPv4' && !a.internal)?.address ?? null;
test.after(() => fs.rmSync(TMP, { recursive: true, force: true }));
// the tests, registered at the end of the file: one after another, and the ones that wait on the clocks beside them
const IN_TURN = [], BESIDE = [];
const inTurn = (...a) => IN_TURN.push(a), beside = (...a) => BESIDE.push(a);

// ---- what must never be in a response ----
const hostShort = os.hostname().replace(/\.local$/i, '');
const NEEDLES = [...new Set([
  os.homedir(), os.hostname(), hostShort, `${hostShort}.local`, DATA, REPO, TMP, fs.realpathSync(TMP), process.version, `Node.js/${process.versions.node}`,
  ...Object.values(os.networkInterfaces()).flat().filter(a => a && !a.internal).map(a => a.address),
])].filter(s => s && s.length >= 4);
const USER = os.userInfo().username;
// in text answers: no absolute path of any machine, no stack trace
const TEXT_PATTERNS = [/\/Users\/|\/home\/|\/root\/|\/private\/|\/var\/folders|(?<![A-Za-z0-9])[A-Z]:\\/,   // a drive letter, not 'macOS:\n' /\n\s*at .*:\d+:\d+/, /node:internal|ERR_[A-Z_]{4,}/,
  new RegExp(`\\b${USER.replace(/[^A-Za-z0-9]/g, '.')}\\b`, 'i')];
function leaks(text, { binary = false } = {}) {
  const found = NEEDLES.filter(n => text.toLowerCase().includes(n.toLowerCase()));
  if (!binary) for (const re of TEXT_PATTERNS) if (re.test(text)) found.push(String(re));
  return found;
}

// ---- headers every response carries ----
const REQUIRED = {
  'content-security-policy': v => v.includes("default-src 'self'") && v.includes("'wasm-unsafe-eval'") && v.includes("frame-ancestors 'none'") && !v.includes('unsafe-inline'),
  'x-content-type-options': v => v === 'nosniff', 'referrer-policy': v => v === 'no-referrer', 'x-frame-options': v => v === 'DENY',
  'permissions-policy': v => v.includes('gamepad=(self)') && v.includes('camera=()'), 'x-robots-tag': v => /^(no)?index, (no)?follow$/.test(v),
  'cross-origin-opener-policy': v => v === 'same-origin', 'cross-origin-embedder-policy': v => v === 'require-corp',
  'cross-origin-resource-policy': v => v === 'same-origin',
  'cache-control': v => /\bprivate\b/.test(v) && /\bno-(store|cache)\b/.test(v), 'cdn-cache-control': v => v === 'no-store',
  'cloudflare-cdn-cache-control': v => v === 'no-store',
};
// (Most Wanted's server has no robots.txt of its own: Underground 2's answers the shared site's root)
const INDEXABLE = new Set(['index', 'index.html', 'HEAD /', ...(G.id === 'ug2' ? ['robots'] : [])]);
const FORBIDDEN = ['server', 'x-powered-by', 'last-modified'];
function headerProblems(r, { viaProxy = false, indexable = false } = {}) {
  const bad = [];
  for (const [k, ok] of Object.entries(REQUIRED)) if (!(k in r.headers) || !ok(r.headers[k])) bad.push(`${k}: ${r.headers[k] ?? '(missing)'}`);
  const wantRobots = indexable ? 'index, follow' : 'noindex, nofollow';
  if ('x-robots-tag' in r.headers && r.headers['x-robots-tag'] !== wantRobots) bad.push(`x-robots-tag: ${r.headers['x-robots-tag']} (expected ${wantRobots})`);
  for (const k of FORBIDDEN) if (k in r.headers && !(viaProxy && k === 'server')) bad.push(`${k} present`);
  return bad;
}

// ---- a raw HTTP/1.1 client (exact request bytes: fetch would clean the paths) ----
function rawRequest({ port, useTls = false, host = '127.0.0.1', servername = 'localhost', ca = CERTS.caPem, data, timeout = 30_000 }) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    const sock = useTls ? tls.connect({ host, port, servername: net.isIP(servername) ? undefined : servername, ca, rejectUnauthorized: false })
      : net.connect(port, host);
    const t = setTimeout(() => { sock.destroy(); resolve(parse(Buffer.concat(chunks), true)); }, timeout);
    sock.on(useTls ? 'secureConnect' : 'connect', () => sock.write(data));
    sock.on('data', c => chunks.push(c));
    sock.on('close', () => { clearTimeout(t); resolve(parse(Buffer.concat(chunks), false)); });
    sock.on('error', e => { clearTimeout(t); if (chunks.length) resolve(parse(Buffer.concat(chunks), false)); else reject(e); });
  });
}
function parse(buf, timedOut) {
  const end = buf.indexOf('\r\n\r\n');
  if (end < 0) return { status: 0, headers: {}, head: buf.toString('latin1'), body: Buffer.alloc(0), timedOut };
  const head = buf.subarray(0, end).toString('latin1');
  const [statusLine, ...lines] = head.split('\r\n');
  const headers = {};
  for (const l of lines) { const i = l.indexOf(':'); const k = l.slice(0, i).trim().toLowerCase(); headers[k] = headers[k] ? headers[k] + ', ' + l.slice(i + 1).trim() : l.slice(i + 1).trim(); }
  let body = buf.subarray(end + 4);
  if (/chunked/i.test(headers['transfer-encoding'] ?? '')) {
    const out = []; let i = 0;
    while (i < body.length) { const nl = body.indexOf('\r\n', i); const n = parseInt(body.subarray(i, nl).toString(), 16); if (!n) break; out.push(body.subarray(nl + 2, nl + 2 + n)); i = nl + 2 + n + 2; }
    body = Buffer.concat(out);
  }
  return { status: +statusLine.split(' ')[1], statusLine, headers, head, body, timedOut };
}
// (a path from the root goes under the game's base: B; rootReq sends it as it is)
const rootReq = (method, p, { host = 'localhost', headers = {}, body = '', version = '1.1' } = {}) =>
  `${method} ${p} HTTP/${version}\r\n${host === null ? '' : `Host: ${host}\r\n`}Connection: close\r\n${Object.entries(headers).map(([k, v]) => `${k}: ${v}\r\n`).join('')}\r\n${body}`;
const req = (method, p, o) => rootReq(method, p.startsWith('/') ? B + p : p, o);
const upgrade = (p, extra = {}) => req('GET', p, { headers: { Upgrade: 'websocket', Connection: 'Upgrade', 'Sec-WebSocket-Version': '13',
  'Sec-WebSocket-Key': crypto.randomBytes(16).toString('base64'), ...extra } }).replace('Connection: close\r\n', '');

// ---- a server under test ----
async function startServer(args, { expectExit = false, env = {}, state = fs.mkdtempSync(path.join(TMP, 'state-')) } = {}) {
  const c = spawn(...serverCommand([...(args.includes('--data') ? [] : ['--data', DATA]), ...G.args, '--state', state, ...args]), { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ...env } });
  let out = '';
  c.stdout.on('data', d => { out += d; }); c.stderr.on('data', d => { out += d; });
  const exited = new Promise(r => c.on('exit', code => r(code)));
  if (expectExit) return { code: await exited, out: () => out };
  await new Promise((res, rej) => {
    const t = setTimeout(() => rej(new Error('server did not start:\n' + out)), 20000);
    const check = () => { if (out.includes(READY)) { clearTimeout(t); res(); } };
    c.stdout.on('data', check);
    exited.then(code => { clearTimeout(t); rej(new Error(`server exited (${code}):\n${out}`)); });
  });
  return { c, state, out: () => out,
    stop: async () => { c.kill('SIGTERM'); await exited; } };
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
function wsOpen(url, opts = {}) {
  return new Promise(res => {
    const ws = new WebSocket(url, { rejectUnauthorized: false, ...opts });
    const msgs = [];
    ws.on('message', (d, bin) => { if (!bin) msgs.push(String(d)); });
    ws.on('open', () => res({ ws, msgs, status: 101 }));
    ws.on('unexpected-response', (q, r) => { let b = ''; r.on('data', d => { b += d; }); r.on('end', () => res({ ws: null, status: r.statusCode, headers: r.headers, body: b })); });
    ws.on('error', e => res({ ws: null, status: 0, error: e.message }));
  });
}
const closeOf = (ws, ms = 0) => new Promise(r => {
  if (!ws) return r({ code: 'no connection', reason: '' });
  ws.on('close', (code, reason) => r({ code, reason: String(reason) }));
  if (ms) setTimeout(() => { r({ code: 'still open', reason: '' }); ws.terminate(); }, ms);
});

// ---- the crawl ----
// base: the routes' base path ('/u2': each path under it, as the server serves a name that has one, PLAN D-38), with
// the paths around it besides: the origin's root files, the bare base, and the root's own routes
// (Most Wanted's: its own base, B, everywhere)
function crawlRequests(base = '') {
  const r = [];
  const add = (name, data, o = {}) => r.push({ name, data, ...o });
  const bare = base || B;
  crawlPaths(add, (method, p, o) => req(method, p.startsWith('/') ? base + p : p, o), (p, extra) => upgrade(base + p, extra));
  if (bare) {
    const up = (p, extra = {}) => rootReq('GET', p, { headers: { Upgrade: 'websocket', Connection: 'Upgrade', 'Sec-WebSocket-Version': '13',
      'Sec-WebSocket-Key': crypto.randomBytes(16).toString('base64'), ...extra } }).replace('Connection: close\r\n', '');
    add('root robots', rootReq('GET', '/robots.txt')); add('bare base', rootReq('GET', bare)); add('bare base query', rootReq('GET', `${bare}?a=1`));
    add('root index', rootReq('GET', '/')); add('root path', rootReq('GET', '/game.json')); add('root manifest', rootReq('GET', '/data-manifest'));
    add('root lan', up('/lan'));
  }
  return r;
}
// (req and upgrade: the base's own)
function crawlPaths(add, req, upgrade) {
  add('index', req('GET', '/')); add('index.html', req('GET', '/index.html')); add('HEAD /', req('HEAD', '/'));
  add('main.js', req('GET', '/main.js')); add('main.css', req('GET', '/main.css'));
  add('io-worker', req('GET', '/host/io-worker.js')); add('game-worker', req('GET', '/host/game-worker.js'));
  add('worklet', req('GET', '/audio/worklet.js'));
  add('main.js.map', req('GET', '/main.js.map')); add('test page', req('GET', '/test/hostcheck.html'));
  add('test script', req('GET', `/tests/scripts/${SCRIPT}`));
  add('game.json', req('GET', '/game.json'));
  add('game.wasm br', req('GET', '/game.wasm', { headers: { 'Accept-Encoding': 'gzip, br' } }), { binary: true, br: true });
  add('game.wasm range', req('GET', '/game.wasm', { headers: { Range: 'bytes=0-65535' } }), { binary: true });
  add('HEAD game.wasm', req('HEAD', '/game.wasm'));
  add('manifest gz', req('GET', '/data-manifest', { headers: { 'Accept-Encoding': 'gzip' } }), { gz: true });
  add('manifest', req('GET', '/data-manifest')); add('manifest refresh', req('GET', '/data-manifest?refresh'));
  add('data range', req('GET', `/data/${EXE}`, { headers: { Range: 'bytes=0-4095' } }), { binary: true });
  add('data blocks', req('GET', `/data-blocks/${EXE}?b=0&n=2`, { headers: { 'Accept-Encoding': 'gzip, br' } }), { binary: true, br: true, gz: true });
  add('data blocks bad', req('GET', `/data-blocks/${EXE}?b=0&n=99`));
  add('data blocks past', req('GET', `/data-blocks/${EXE}?b=999999&n=1`));
  add('data 416', req('GET', `/data/${EXE}`, { headers: { Range: 'bytes=999999999999-' } }));
  add('data missing', req('GET', '/data/no/such/file.bin')); add('data dir', req('GET', '/data/')); add('data dir2', req('GET', '/data/FRONTEND'));
  // gone: what the page used to be told about the server, and the built-in certificate authority
  add('server-info', req('GET', '/server-info')); add('robots', req('GET', '/robots.txt'));
  add('setup', req('GET', '/setup')); add('ca.crt', req('GET', '/ca.crt')); add('favicon', req('GET', '/favicon.ico'));
  add('404', req('GET', '/nothing/here')); add('bad escape', req('GET', '/%ZZ')); add('NUL', req('GET', '/a%00b'));
  add('backslash', req('GET', '/a%5cb')); add('traversal', req('GET', '/..%2f..%2f..%2fetc/passwd'));
  add('absolute-form', req('GET', 'http://evil.example/robots.txt'));
  add('POST', req('POST', '/', { headers: { 'Content-Length': 5 }, body: 'hello' })); add('PUT artifact', req('PUT', '/artifacts/x.png', { headers: { 'Content-Length': 4 }, body: 'PNG!' }));
  add('DELETE', req('DELETE', '/')); add('OPTIONS', req('OPTIONS', '*')); add('TRACE', req('TRACE', '/')); add('PATCH', req('PATCH', '/'));
  add('GET body', req('GET', '/', { headers: { 'Content-Length': 3 }, body: 'abc' }));
  add('chunked GET', req('GET', '/', { headers: { 'Transfer-Encoding': 'chunked' }, body: '3\r\nabc\r\n0\r\n\r\n' }));
  add('big header', req('GET', '/', { headers: { 'X-Big': 'a'.repeat(20000) } }));
  add('no host', req('GET', '/', { host: null })); add('expect', req('GET', '/', { headers: { Expect: 'teapot' } }));
  add('garbage', 'HELLO THERE\r\n\r\n'); add('HTTP/2 preface', 'PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n');
  add('lan plain GET', req('GET', '/lan'));
  add('upgrade other', upgrade('/other')); add('upgrade bad key', upgrade('/lan', { 'Sec-WebSocket-Key': 'nope' }));
  add('upgrade evil origin', upgrade('/lan', { Origin: 'https://evil.example' }));
  // refused by the ws library itself (the review's finding 4: its own 400, without our headers)
  add('upgrade bad protocol', upgrade('/lan', { 'Sec-WebSocket-Protocol': 'a b' }));
  add('upgrade bad protocol 2', upgrade('/lan', { 'Sec-WebSocket-Protocol': 'x,,y' }));
  add('dev bypass', req('GET', '/%2ftest/play.html')); add('dev bypass 2', req('GET', '/te%C5%BFt/play.html'));
  add('upgrade h2c', req('GET', '/', { headers: { Upgrade: 'h2c', 'HTTP2-Settings': 'AAMAAABkAARAAAAAAAIAAAAA' } }).replace('Connection: close', 'Connection: Upgrade, HTTP2-Settings'));
}

// indexable: the requests whose answer search engines may index (INDEXABLE at the root)
async function crawl(label, target, { viaProxy = false, base = '', indexable = INDEXABLE } = {}) {
  const results = [];
  let etag = null;
  for (const r of crawlRequests(base)) {
    // the first module request waits for its compression (seconds on a busy machine)
    const res = await rawRequest({ ...target, data: r.data, timeout: r.br ? 180_000 : 30_000 });
    results.push({ ...r, res });
    if (r.name === 'data range') etag = res.headers.etag;
  }
  if (etag) results.push({ name: 'data 304', res: await rawRequest({ ...target, data: req('GET', `${base}/data/${EXE}`, { headers: { 'If-None-Match': etag } }) }) });
  const problems = [];
  for (const { name, res, binary, br, gz } of results) {
    if (!res.status) { problems.push(`${label} ${name}: no HTTP answer (${JSON.stringify(res.head.slice(0, 80))})`); continue; }
    let body = res.body;
    if (br && res.headers['content-encoding'] === 'br') body = zlib.brotliDecompressSync(body);
    if (gz && res.headers['content-encoding'] === 'gzip') body = zlib.gunzipSync(body);
    const text = res.head + '\n' + body.toString('latin1');
    const l = leaks(text, { binary: !!binary });
    if (l.length) problems.push(`${label} ${name} (${res.status}): leaks ${l.join(', ')}`);
    if (res.status >= 400 && body.length > 64) problems.push(`${label} ${name} (${res.status}): error body of ${body.length} bytes`);
    // through the proxy, a request it refuses itself (as Cloudflare's edge would) never reaches the server
    const fromProxy = viaProxy && !('content-security-policy' in res.headers) && [400, 417, 431, 502, 505].includes(res.status);
    const hp = headerProblems(res, { viaProxy, indexable: indexable.has(name) });
    if (hp.length && !fromProxy) problems.push(`${label} ${name} (${res.status}): ${hp.join('; ')}`);
  }
  return { results, problems, statuses: Object.fromEntries(results.map(r => [r.name, r.res.status])) };
}

// WebSocket paths: messages and close reasons
async function wsCrawl(label, url, opts) {
  const problems = [];
  const a = await wsOpen(url, opts);
  assert.equal(a.status, 101, `${label}: /lan opens`);
  a.ws.send(JSON.stringify({ t: 'hello', name: 'crawler' }));
  a.ws.send(JSON.stringify({ t: 'ping', id: 1 }));
  await sleep(200);
  // over the message limit: closed by the server
  const closed = closeOf(a.ws, 3000);
  a.ws.send(Buffer.alloc(70 << 10));
  const c = await closed;
  for (const m of [...a.msgs, c.reason]) { const l = leaks(m); if (l.length) problems.push(`${label} ws ${m.slice(0, 60)}: ${l}`); }
  return { problems, oversizeClose: c, msgs: a.msgs };
}

inTurn('crawl: every route and error path, each TLS mode: nothing about this machine, headers on every answer', { skip: SKIP, timeout: 900_000 }, async () => {
  const summary = {};
  const all = [];
  // soft checks: every problem of every mode is listed at the end
  const expect = (ok, msg) => { if (!ok) all.push(msg); };
  const is = (label, statuses, name, code) => expect(statuses[name] === code, `${label} ${name}: ${statuses[name]}, not ${code}`);
  const gone = (label, statuses) => { for (const n of ['server-info', 'setup', 'ca.crt']) is(label, statuses, n, 404); is(label, statuses, 'robots', G.id === 'ug2' ? 200 : 404); };
  // Most Wanted's under /mw on every name: the bare /mw goes there, nothing outside it answers, the manifest names
  // its blocks there (PLAN D-38)
  const underBase = (label, h) => {
    if (!B) return;
    for (const n of ['root robots', 'root index', 'root path', 'root manifest', 'root lan']) is(label, h.statuses, n, 404);
    is(label, h.statuses, 'bare base', 308);
    const res = n => h.results.find(r => r.name === n).res;
    expect(res('bare base').headers.location === `${B}/`, `${label}: ${B} goes to ${res('bare base').headers.location}`);
    expect(res('bare base query').headers.location === `${B}/?a=1`, `${label}: ${B}?a=1 goes to ${res('bare base query').headers.location}`);
    expect(JSON.parse(res('manifest').body).blocks.url === `${B}/data-blocks/v2/`, `${label}: the manifest names its blocks under ${B}`);
    for (const n of ['index', 'main.js', 'game.json', 'manifest']) is(label, h.statuses, n, 200);
  };
  // 1. no TLS (the default: this machine only, http://localhost), no --dev
  {
    const s = await startServer(['--http-port', String(P2)]);
    try {
      const http = await crawl('no-tls http', { port: P2 });
      all.push(...http.problems);
      summary['no-tls'] = http.statuses;
      gone('http', http.statuses);
      underBase('http', http);
      is('http', http.statuses, 'main.js.map', 404); is('http', http.statuses, 'test page', 404); is('http', http.statuses, 'test script', 404);
      is('http', http.statuses, 'PUT artifact', 405);
      const w = await wsCrawl('no-tls', `ws://localhost:${P2}${B}/lan`, { headers: { Origin: `http://localhost:${P2}` } });
      all.push(...w.problems);
    } finally { await s.stop(); }
  }
  // 2. --dev with --artifacts: the test aids, still nothing about the machine
  {
    const art = fs.mkdtempSync(path.join(TMP, 'art-'));
    const s = await startServer(['--http-port', String(P2), '--dev', '--artifacts', art]);
    try {
      const http = await crawl('dev http', { port: P2 });
      // the test pages carry source maps whose sources are repo-relative; source map bodies are allowed to name
      // the repo's own files, never this machine's absolute paths
      all.push(...http.problems);
      summary.dev = http.statuses;
      const aid = AIDS ? 200 : 404;
      is('http', http.statuses, 'main.js.map', aid); is('http', http.statuses, 'test page', aid); is('http', http.statuses, 'test script', aid);
      is('http', http.statuses, 'PUT artifact', 201);
    } finally { await s.stop(); }
  }
  // 3. a certificate (--tls-cert), as on a VPS with Let's Encrypt or a Cloudflare Origin CA certificate
  {
    const s = await startServer(['--port', String(P1), '--http-port', '0', '--tls-cert', CERTS.cert, '--tls-key', CERTS.key]);
    try {
      const https = await crawl('tls-cert', { port: P1, useTls: true, servername: 'nfsu2.test' });
      all.push(...https.problems);
      summary['tls-cert'] = https.statuses;
      gone('https', https.statuses);
      underBase('https', https);
      is('https', https.statuses, 'main.js.map', 404); is('https', https.statuses, 'test page', 404); is('https', https.statuses, 'test script', 404);
      const w = await wsCrawl('tls-cert', `wss://localhost:${P1}${B}/lan`, { ca: CERTS.caPem, headers: { Origin: `https://localhost:${P1}` } });
      all.push(...w.problems);
      expect(w.oversizeClose.code === 1009, `a message over 64 KB closes the connection with 1009, not ${w.oversizeClose.code}`);
      summary.wsClose = w.oversizeClose;
    } finally { await s.stop(); }
  }
  // 4. behind a proxy (the Cloudflare simulation in front)
  {
    const s = await startServer(['--behind-proxy', '--http-port', String(P2), '--host', 'nfsu2.test', '--max-peers', '1']);
    const sim = startCfSim({ port: P1, target: P2, certs: CERTS, fakeClientIp: '198.51.100.7' });
    await sim.ready;
    try {
      const viaProxy = await crawl('cf-proxy', { port: P1, useTls: true, servername: 'nfsu2.test' }, { viaProxy: true });
      const direct = await crawl('behind-proxy direct', { port: P2 });
      all.push(...viaProxy.problems, ...direct.problems);
      summary['cf-proxy'] = viaProxy.statuses;
      gone('viaProxy', viaProxy.statuses);
      const w = await wsCrawl('cf-proxy', `wss://localhost:${P1}${B}/lan`, { ca: CERTS.caPem, headers: { Origin: `https://localhost:${P1}` } });
      all.push(...w.problems);
      // players: one at most here; the second gets the fixed reason
      const a = await wsOpen(`ws://127.0.0.1:${P2}${B}/lan`);
      const b = await wsOpen(`ws://127.0.0.1:${P2}${B}/lan`);
      const cb = await closeOf(b.ws, 3000);
      expect(cb.code === 1013 && cb.reason === 'LAN full', `a full switch closes with ${cb.code} ${cb.reason}`);
      a.ws.close();
    } finally { await sim.close(); await s.stop(); }
  }
  // 5. under a base (PLAN D-38): this machine's name standing in for the site UG2 shares with Most Wanted, as
  // --base-path puts it under /u2: every answer with the same headers and nothing about the machine; the page's index
  // the one indexable answer under /u2 and robots.txt at the root alone, naming both games' bases (Underground 2's
  // alone: Most Wanted's is under its base in every mode above)
  if (G.id === 'ug2') {
    const s = await startServer(['--http-port', String(P2), '--base-path', 'localhost=/u2']);
    try {
      const http = await crawl('base /u2', { port: P2 }, { base: '/u2', indexable: new Set(['index', 'index.html', 'HEAD /', 'root robots']) });
      all.push(...http.problems);
      summary['base /u2'] = http.statuses;
      for (const n of ['server-info', 'setup', 'ca.crt', 'robots', 'main.js.map', 'test page', 'test script', 'root index', 'root path', 'root manifest'])
        is('base', http.statuses, n, 404);
      for (const n of ['index', 'index.html', 'main.js', 'game.json', 'manifest', 'root robots']) is('base', http.statuses, n, 200);
      is('base', http.statuses, 'bare base', 308); is('base', http.statuses, 'root lan', 404);
      const body = n => http.results.find(r => r.name === n).res;
      expect(body('bare base').headers.location === '/u2/', `base: /u2 goes to ${body('bare base').headers.location}`);
      expect(body('bare base query').headers.location === '/u2/?a=1', `base: /u2?a=1 goes to ${body('bare base query').headers.location}`);
      expect(JSON.parse(body('manifest').body).blocks.url === '/u2/data-blocks/v2/', 'base: the manifest names its blocks under /u2');
      const robots = body('root robots').body.toString();
      expect(/^Disallow: \/u2\/data\/$/m.test(robots) && /^Disallow: \/mw\/vault$/m.test(robots) && !/^Disallow: \/data\/$/m.test(robots),
        `base: the root's robots.txt names both games' bases: ${JSON.stringify(robots)}`);
      const w = await wsCrawl('base /u2', `ws://localhost:${P2}/u2/lan`, { headers: { Origin: `http://localhost:${P2}` } });
      all.push(...w.problems);
    } finally { await s.stop(); }
  }
  console.log('statuses:', JSON.stringify(summary));
  assert.deepEqual(all, [], all.join('\n'));
});

inTurn('path traversal and file safety', { skip: SKIP, timeout: 60_000 }, async () => {
  // a game folder of our own, with links pointing out of it
  const data = fs.mkdtempSync(path.join(TMP, 'data-'));
  const outside = fs.mkdtempSync(path.join(TMP, 'outside-'));
  fs.writeFileSync(path.join(outside, 'secret.txt'), 'SECRET-OUTSIDE');
  fs.writeFileSync(path.join(data, EXE), 'MZ fake');
  fs.mkdirSync(path.join(data, 'GLOBAL'));
  fs.writeFileSync(path.join(data, 'GLOBAL', 'Game.txt'), 'inside');
  fs.writeFileSync(path.join(data, '.hidden'), 'SECRET-DOT');
  fs.writeFileSync(path.join(data, 'GLOBAL', '.hidden'), 'SECRET-DOT');
  fs.writeFileSync(path.join(data, 'GLOBAL', 'page.html'), '<script>alert(1)</script>');
  // what an install folder holds besides the game (the audit's PRIV-1): never served, never listed
  fs.writeFileSync(path.join(data, 'unins000.dat'), 'SECRET-PC-NAME');
  fs.mkdirSync(path.join(data, 'SAVE')); fs.writeFileSync(path.join(data, 'SAVE', 'profile'), 'SECRET-SAVE');
  fs.mkdirSync(path.join(data, 'scripts')); fs.writeFileSync(path.join(data, 'scripts', 'mod.ini'), 'SECRET-INI');
  for (const at of [data, path.join(data, 'GLOBAL')]) {
    fs.symlinkSync(path.join(outside, 'secret.txt'), path.join(at, 'link-out.txt'));
    fs.symlinkSync(outside, path.join(at, 'dir-out'));
    fs.symlinkSync('/etc', path.join(at, 'etc'));
  }
  const s = await startServer(['--http-port', String(P2), '--data', data]);
  const get = async p => rawRequest({ port: P2, data: req('GET', p) });
  try {
    const attacks = [
      '/data/../build.mjs', '/data/%2e%2e/%2e%2e/etc/passwd', '/data/..%2f..%2f..%2fetc%2fpasswd', '/data/%252e%252e/%252e%252e/etc/passwd',
      '/data/....//....//etc/passwd', '/data/..%5c..%5cetc%5cpasswd', '/data/..\\..\\etc\\passwd', '/data/SPEED2.EXE%00.txt',
      '/data/.%2e/.%2e/etc/passwd', '/data/%2e%2e%2f%2e%2e%2fweb/build.mjs', '/%2e%2e/%2e%2e/etc/passwd', '/..%2f..%2fout/web/server/etag.key',
      '/../web/build.mjs', '/web/build.mjs', '/ca.key', '/server.key', '/etag.key', '/.git/config', '/data/.hidden', '/data/%2ehidden',
      '/data/link-out.txt', '/data/dir-out/secret.txt', '/data/etc/passwd', '/data/etc/hosts', '/data/GLOBAL/../../outside/secret.txt',
      '/data//etc/passwd', '/data/%2fetc%2fpasswd', '/data/%c0%ae%c0%ae/etc/passwd', '/data/%u002e%u002e/etc/passwd', '/DATA/../../etc/passwd',
      `/data/${encodeURIComponent(outside + '/secret.txt')}`, '/data/GLOBAL%2f..%2f..%2fetc%2fpasswd',
      '/data/GLOBAL/link-out.txt', '/data/GLOBAL/dir-out/secret.txt', '/data/GLOBAL/etc/passwd', '/data/GLOBAL/.hidden',
      '/data/unins000.dat', '/data/UNINS000.DAT', '/data/SAVE/profile', '/data/save/profile', '/data/scripts/mod.ini', '/data/GLOBAL/../unins000.dat',
    ];
    const seen = {};
    for (const a of attacks) {
      const r = await get(a);
      seen[a] = r.status;
      assert.ok(r.status === 400 || r.status === 404, `${a}: ${r.status}`);
      const b = r.body.toString();
      assert.ok(!/SECRET|root:|nobody|localhost|server\.mjs|BEGIN|import /.test(b), `${a}: ${b.slice(0, 80)}`);
      assert.ok(b.length < 64);
    }
    // what is inside is served, case as stored, as bytes
    const ok = await get('/data/GLOBAL/Game.txt');
    assert.equal(ok.status, 200); assert.equal(ok.body.toString(), 'inside');
    const html = await get('/data/GLOBAL/page.html');
    assert.equal(html.headers['content-type'], 'application/octet-stream', 'a file in the game folder is never served as a page');
    // the manifest lists neither the links, the dot files nor what is not the game's
    const m = JSON.parse((await get('/data-manifest')).body);
    assert.deepEqual(m.files.map(f => f.p).sort(), ['GLOBAL', 'GLOBAL/Game.txt', 'GLOBAL/page.html', EXE].sort());
    assert.ok(!('root' in m), 'no folder name');
    console.log(`traversal: ${attacks.length} attempts, statuses ${JSON.stringify(Object.entries(seen).reduce((o, [, v]) => ({ ...o, [v]: (o[v] ?? 0) + 1 }), {}))}`);
  } finally { await s.stop(); }
});

inTurn('limits: methods, bodies, header size, slow headers, /lan per address', { skip: SKIP, timeout: 90_000 }, async () => {
  const s = await startServer(['--http-port', String(P2)]);
  try {
    const st = async (data) => (await rawRequest({ port: P2, data })).status;
    for (const m of ['POST', 'PUT', 'DELETE', 'PATCH', 'OPTIONS', 'TRACE', 'PROPFIND']) assert.equal(await st(req(m, '/')), 405, m);
    assert.equal(await st(req('GET', '/', { headers: { 'Content-Length': 10 }, body: '0123456789' })), 413);
    assert.equal(await st(req('GET', '/', { headers: { 'X-Big': 'a'.repeat(17 << 10) } })), 431);
    assert.equal(await st(req('GET', '/', { host: null })), 400);
    assert.equal(await st(req('GET', '/', { headers: { Expect: 'teapot' } })), 417);
    assert.equal(await st(req('GET', '/' + 'a'.repeat(20 << 10))), 431, 'a URL longer than the header limit');
    // slow headers (slow-loris): one header line every 2 s; the server hangs up within its 15 s header limit (+5 s check)
    // (by the monotonic clock: WSL's wall clock jumps by seconds now and then)
    const t0 = performance.now();
    const sock = net.connect(P2, '127.0.0.1');
    let answer = '';
    sock.on('data', d => { answer += d; });
    const closed = new Promise(r => sock.on('close', r));
    sock.write('GET / HTTP/1.1\r\nHost: localhost\r\n');
    const drip = setInterval(() => { try { sock.write(`X-Drip-${Date.now()}: 1\r\n`); } catch { /* closed */ } }, 2000);
    await closed; clearInterval(drip);
    const tSlow = (performance.now() - t0) / 1000;
    assert.ok(tSlow <= 22, `slow headers cut after ${tSlow.toFixed(1)} s`);
    assert.match(answer, /^HTTP\/1\.1 408/);
    console.log(`slow headers: connection closed after ${tSlow.toFixed(1)} s with ${answer.split('\r\n')[0]}`);
    // /lan: 8 connections from one address, the 9th refused
    const socks = [];
    for (let i = 0; i < 8; i++) { const o = await wsOpen(`ws://127.0.0.1:${P2}${B}/lan`); assert.equal(o.status, 101); socks.push(o.ws); }
    const ninth = await wsOpen(`ws://127.0.0.1:${P2}${B}/lan`);
    assert.equal(ninth.status, 429, 'the 9th connection from one address');
    for (const w of socks) w.close();
    await sleep(300);
    assert.equal((await wsOpen(`ws://127.0.0.1:${P2}${B}/lan`)).status, 101, 'free again once they closed');
    // the console (a VPS's journal) names players by virtual address, never by their real one
    const lanLines = s.out().split('\n').filter(l => l.startsWith('[lan]'));
    assert.ok(lanLines.some(l => / 10\.77\.[0-7]\.\d+ joined/.test(l)) && lanLines.some(l => l.includes('refused')), lanLines.join('\n'));
    assert.deepEqual(lanLines.filter(l => /127\.0\.0\.1|::1|::ffff/.test(l)), [], 'no client address in the [lan] lines');
    // a handshake the ws library refuses: our 400 with our headers, no body naming the library
    for (const proto of ['a b', 'x,,y', '"q"']) {
      const r = await rawRequest({ port: P2, data: upgrade('/lan', { 'Sec-WebSocket-Protocol': proto }) });
      assert.equal(r.status, 400, proto);
      assert.deepEqual(headerProblems(r), [], `${proto}: ${headerProblems(r)}`);
      assert.equal(r.body.length, 0, `${proto}: ${r.body}`);
    }
  } finally { await s.stop(); }
});

// ---- availability ----
const rssMb = pid => +execFileSync('ps', ['-o', 'rss=', '-p', String(pid)]).toString().trim() / 1024;
const openFiles = pid => execFileSync('lsof', ['-n', '-P', '-p', String(pid)]).toString().split('\n').filter(l => / REG /.test(l)).length;
function rawConnect(port, host = '127.0.0.1', opts = {}) {
  return new Promise((res, rej) => {
    const sock = net.connect({ port, host, ...opts });
    sock.once('connect', () => res(sock));
    sock.once('error', rej);
  });
}
// a /lan WebSocket on a plain socket (exact frames; it reads only what it is told to)
async function rawLan(port) {
  const sock = await rawConnect(port);
  sock.on('error', () => {});
  sock.write(upgrade('/lan'));
  const head = await new Promise(r => sock.once('data', d => r(String(d).split('\r\n')[0])));
  assert.equal(head, 'HTTP/1.1 101 Switching Protocols');
  return sock;
}
// a masked client frame (mask 0: the payload as it is)
const wsFrame = (op, payload = Buffer.alloc(0)) => Buffer.concat([Buffer.from([0x80 | op, 0x80 | payload.length, 0, 0, 0, 0]), payload]);

inTurn('a ping flood from a client that never reads: cut, the server\'s memory stays flat', { skip: SKIP, timeout: 120_000 }, async () => {
  // the phase-2 deploy review's finding 1: the ws library answered every ping and queued the pongs for a
  // client that never read (+5.6 GB in 20 s); unasked pongs kept it "alive"
  const s = await startServer(['--http-port', String(P2)]);
  try {
    await sleep(1500);                                   // the module's compression at start
    const r0 = rssMb(s.c.pid);
    const sock = await rawLan(P2);
    sock.pause();
    let closed = false; sock.on('close', () => { closed = true; });
    const burst = Buffer.concat(Array(512).fill(wsFrame(0x9, Buffer.alloc(125, 0x41))));
    let sent = 0;
    const t0 = Date.now();
    while (!closed && !sock.destroyed && Date.now() - t0 < 6000) {
      const ok = sock.write(burst);
      sent += burst.length;
      if ((sent / burst.length) % 64 === 0) sock.write(wsFrame(0xa));     // a pong nobody asked for
      if (!ok) await Promise.race([new Promise(r => sock.once('drain', r)), new Promise(r => sock.once('close', r)), sleep(500)]);
    }
    const dt = Date.now() - t0;
    const r1 = rssMb(s.c.pid);
    console.log(`ping flood: ${(sent / 1048576).toFixed(1)} MB of pings in ${dt} ms, connection ${closed || sock.destroyed ? 'cut by the server' : 'still open'}, ` +
      `server RSS ${r0.toFixed(0)} -> ${r1.toFixed(0)} MB`);
    assert.ok(closed || sock.destroyed, 'the flooding client is cut');
    assert.ok(dt < 3000, `cut within 3 s (${dt} ms)`);
    assert.ok(r1 - r0 < 64, `memory flat (+${(r1 - r0).toFixed(0)} MB)`);
    sock.destroy();
    // the switch still works for others
    const ok = await wsOpen(`ws://127.0.0.1:${P2}${B}/lan`);
    assert.equal(ok.status, 101); ok.ws.close();
  } finally { await s.stop(); }
});

inTurn('requests queued on one connection (pipelined) and never read: cut, no memory or open files held', { skip: SKIP, timeout: 120_000 }, async () => {
  // each queued answer opened its file and buffered its first 64 KB; when the client left, the files of the
  // answers still queued stayed open for good (5000 requests: +600 MB, 5000 files; 20000: +2.7 GB)
  const s = await startServer(['--http-port', String(P2)]);
  try {
    await sleep(1500);
    const r0 = rssMb(s.c.pid), f0 = openFiles(s.c.pid);
    const sock = await rawConnect(P2);
    sock.on('error', () => {});
    sock.pause();
    let closed = false; sock.on('close', () => { closed = true; });
    sock.write(req('GET', `/data/${EXE}`).replace('Connection: close\r\n', '').repeat(5000));
    await sleep(3000);
    const r1 = rssMb(s.c.pid), f1 = openFiles(s.c.pid);
    sock.destroy();
    await sleep(1000);
    const f2 = openFiles(s.c.pid);
    console.log(`pipelined: 5000 requests on one connection, ${closed ? 'cut by the server' : 'still open'}; RSS ${r0.toFixed(0)} -> ${r1.toFixed(0)} MB, ` +
      `open files ${f0} -> ${f1}, ${f2} after the client left`);
    assert.ok(closed, 'the connection is cut');
    assert.ok(r1 - r0 < 64, `memory flat (+${(r1 - r0).toFixed(0)} MB)`);
    assert.ok(f1 - f0 <= 8 && f2 - f0 <= 2, `open files ${f0} -> ${f1} -> ${f2}`);
    // a browser's way (one request at a time, keep-alive) still works on one connection
    const c = await rawConnect(P2);
    let got = '';
    c.on('data', d => { got += d; });
    for (let i = 0; i < 5; i++) {
      c.write(req('GET', '/players').replace('Connection: close\r\n', ''));
      const t0 = Date.now();
      while ((got.match(/HTTP\/1\.1 200/g) ?? []).length <= i && Date.now() - t0 < 5000) await sleep(20);
    }
    assert.equal((got.match(/HTTP\/1\.1 200/g) ?? []).length, 5, 'five requests one after another on one connection');
    // and a short pipeline (within the limit) is answered in order
    const c2 = await rawConnect(P2);
    let got2 = '';
    c2.on('data', d => { got2 += d; });
    c2.write(req('GET', '/players').replace('Connection: close\r\n', '').repeat(3));
    await sleep(500);
    assert.equal((got2.match(/HTTP\/1\.1 200/g) ?? []).length, 3, 'three pipelined requests answered');
    c.destroy(); c2.destroy();
  } finally { await s.stop(); }
});

inTurn('TLS handshakes that never finish: closed after the handshake timeout, one address cannot fill the port', { skip: SKIP, timeout: 180_000 }, async () => {
  // the phase-2 deploy review's finding 2: the error handler wrote plain HTTP into TLS sockets whose handshake
  // timed out and never closed them; 1024 such sockets locked the HTTPS port until a restart
  const s = await startServer(['--port', String(P1), '--http-port', '0', '--tls-cert', CERTS.cert, '--tls-key', CERTS.key]);
  const realClient = async (host = '127.0.0.1') => {
    try { return (await rawRequest({ port: P1, useTls: true, host, servername: 'nfsu2.test', data: req('GET', '/players'), timeout: 8000 })).status; }
    catch (e) { return e.code ?? 'error'; }
  };
  const socks = [];
  try {
    // 1. 1100 connections that never send a byte, from this machine (a trusted peer: no per-address cap),
    //    more than the 1024 the port takes
    let closedByServer = 0;
    const t0 = Date.now();
    for (let i = 0; i < 1100; i += 50) {
      await Promise.all(Array.from({ length: 50 }, async () => {
        try {
          const so = await rawConnect(P1);
          so.on('error', () => {}); so.on('close', () => { closedByServer++; });
          socks.push(so);
        } catch { /* refused */ }
      }));
      await sleep(50);
    }
    const held = socks.length - closedByServer;
    console.log(`stalled handshakes: ${socks.length} connected, ${held} held by the server, a real client now: ${await realClient()}`);
    assert.ok(held >= 1000, `the port is full (${held})`);
    // the server closes every one of them once its handshake timeout (15 s) is up; a real client gets in
    while (closedByServer < socks.length && Date.now() - t0 < 40_000) await sleep(250);
    const dt = (Date.now() - t0) / 1000;
    const after = await realClient();
    console.log(`stalled handshakes: all ${closedByServer} closed by the server after ${dt.toFixed(1)} s; a real client: ${after}`);
    assert.equal(closedByServer, socks.length, 'every stalled connection closed by the server');
    assert.ok(dt < 30, `within the handshake timeout and a margin (${dt.toFixed(1)} s)`);
    assert.equal(after, 200);
    for (const so of socks.splice(0)) so.destroy();
    // 2. from this machine's LAN address (not a trusted peer): at most 64 at once; the others are closed at
    //    once, and other addresses get in meanwhile
    if (!LAN_IP) { console.log('no LAN address: the per-address check is skipped'); return; }
    let cut = 0;
    for (let i = 0; i < 100; i++) {
      try {
        const so = await rawConnect(P1, LAN_IP);
        so.on('error', () => {}); so.on('close', () => { cut++; });
        socks.push(so);
      } catch { cut++; }
    }
    await sleep(500);
    const fromOthers = await realClient(), fromSame = await realClient(LAN_IP);
    console.log(`one address: ${100 - cut} of 100 held, ${cut} closed at once; a client from another address: ${fromOthers}, from the same: ${fromSame}`);
    assert.equal(100 - cut, 64, 'at most 64 connections from one address');
    assert.equal(fromOthers, 200, 'another address is not locked out');
    assert.notEqual(fromSame, 200, 'the 65th from the same address is refused');
  } finally { for (const so of socks) so.destroy(); await s.stop(); }
});

inTurn('no certificate: this machine only (HTTP on 127.0.0.1), no HTTPS port, and the start-up says so', { skip: SKIP, timeout: 60_000 }, async () => {
  // no --tls-cert and no --behind-proxy: there is no certificate of the server's own any more, so other PCs
  // cannot reach it (Chrome would not run the game over plain http from another address anyway)
  const s = await startServer(['--port', String(P1), '--http-port', String(P2)]);
  try {
    assert.match(s.out(), /other PCs : none: they need HTTPS/);
    assert.equal((await rawRequest({ port: P2, data: req('GET', '/players') })).status, 200, 'http://localhost');
    const refused = async (o) => { try { await rawRequest({ ...o, data: req('GET', '/players'), timeout: 5000 }); return false; } catch (e) { return e.code === 'ECONNREFUSED'; } };
    assert.ok(await refused({ port: P1, useTls: true }), 'no HTTPS port');
    if (LAN_IP) assert.ok(await refused({ port: P2, host: LAN_IP }), 'the HTTP port is not on the LAN');
  } finally { await s.stop(); }
});

inTurn('IPv6: a certificate listens on both (::), --bind as given', { skip: SKIP, timeout: 60_000 }, async () => {
  const has6 = Object.values(os.networkInterfaces()).flat().some(a => a && a.family === 'IPv6' && a.internal);
  if (!has6) { console.log('no IPv6 loopback: skipped'); return; }
  const st = async (host, port = P1) => {
    try { return (await rawRequest({ port, useTls: true, host, servername: 'nfsu2.test', data: req('GET', '/players'), timeout: 5000 })).status; }
    catch (e) { return e.code; }
  };
  const a = await startServer(['--port', String(P1), '--http-port', '0', '--tls-cert', CERTS.cert, '--tls-key', CERTS.key]);
  try {
    assert.match(a.out(), /HTTPS     : port \d+ on :: \(IPv6 and IPv4\)/);
    assert.equal(await st('::1'), 200, 'over IPv6');
    assert.equal(await st('127.0.0.1'), 200, 'over IPv4');
  } finally { await a.stop(); }
  const c = await startServer(['--port', String(P1), '--http-port', '0', '--tls-cert', CERTS.cert, '--tls-key', CERTS.key, '--bind', '127.0.0.1']);
  try {
    assert.equal(await st('127.0.0.1'), 200);
    assert.equal(await st('::1'), 'ECONNREFUSED');
  } finally { await c.stop(); }
});

beside('with --trusted-proxies cloudflare, the long keep-alive is for the proxies only', { skip: SKIP || (!LAN_IP && 'no LAN address'), timeout: 180_000 }, async () => {
  // Cloudflare reuses an origin connection for up to 900 s (905 s here); a peer that is not a trusted proxy
  // (this machine's LAN address) gets the usual 65 s
  const s = await startServer(['--port', String(P3), '--http-port', '0', '--tls-cert', CERTS.cert, '--tls-key', CERTS.key, '--trusted-proxies', 'cloudflare']);
  const idle = host => new Promise((res, rej) => {
    const so = tls.connect({ host, port: P3, servername: 'nfsu2.test', rejectUnauthorized: false });
    let t0 = 0, head = '';
    so.on('data', d => { head += d; if (!t0 && head.includes('\r\n\r\n')) t0 = Date.now(); });
    so.on('secureConnect', () => so.write(req('GET', '/players').replace('Connection: close\r\n', 'Connection: keep-alive\r\n')));
    so.on('error', rej);
    so.on('close', () => res({ s: (Date.now() - t0) / 1000, head }));
    setTimeout(() => { so.destroy(); }, 80_000);
  });
  try {
    const [direct, local] = await Promise.all([idle(LAN_IP), idle('127.0.0.1')]);
    console.log(`keep-alive with Cloudflare trusted: another peer closed after ${direct.s.toFixed(1)} s idle, this machine's after ${local.s.toFixed(1)} s (cut by the test)`);
    assert.match(direct.head, /^HTTP\/1\.1 200/);
    assert.ok(direct.s > 60 && direct.s < 72, `the usual keep-alive for another peer (${direct.s.toFixed(1)} s)`);
    assert.ok(local.s > 75, `a trusted proxy keeps the long one (${local.s.toFixed(1)} s)`);
  } finally { await s.stop(); }
});

inTurn('--dev gating and start-up refusals', { skip: SKIP, timeout: 60_000 }, async () => {
  const STUB = fs.mkdtempSync(path.join(TMP, 'stub-'));
  fs.writeFileSync(path.join(STUB, EXE), 'MZ'); fs.mkdirSync(path.join(STUB, 'GLOBAL'));
  fs.copyFileSync(CERTS.key, path.join(STUB, 'GLOBAL', 'k.pem'));
  fs.writeFileSync(path.join(STUB, 'GLOBAL', EXE), 'MZ');            // GLOBAL as a game folder of its own (inside --state)
  const refusals = [
    [['--http-port', String(P2), '--artifacts', path.join(TMP, 'a')], /needs --dev/],
    [['--port', String(P1), '--tls-cert', CERTS.cert], /go together/],
    [['--behind-proxy', '--tls-cert', CERTS.cert, '--tls-key', CERTS.key], /not both/],
    [['--behind-proxy', '--http-port', '0'], /needs --http-port/],
    [['--http-port', String(P2), '--trusted-proxies', 'example.com'], /not an address/],
    [['--http-port', String(P2), '--real-ip-header', 'x-real-ip'], /real-ip-header/],
    [['--http-port', String(P2), '--lan-impair', 'delay=1'], /needs --dev/],
    [['--http-port', String(P2), '--no-such-option'], /unknown option/],
    [['--http-port', String(P2), '--max-peers', '0'], /max-peers/],
    [['--http-port', '0'], /no port to listen on/],
    [['--http-port', String(P2), '--bind', '0.0.0.0'], /--bind is for the HTTPS port/],
    [['--port', '0', '--http-port', String(P2), '--tls-cert', CERTS.cert, '--tls-key', CERTS.key], /needs --port/],
    // the audit's F7: behind a proxy the server must know its public name
    [['--behind-proxy', '--http-port', String(P2)], /needs --host/],
    [['--behind-proxy', '--http-port', String(P2), '--host', 'not a name'], /not a host name/],
    // the audit's F5 / DEP-6: keys never inside what the server serves
    [['--http-port', String(P2), '--data', STUB], /--state .* is inside --data/, { state: path.join(STUB, 'GLOBAL', 'state') }],
    [['--http-port', String(P2), '--data', STUB], /--state .* is inside --dist/, { state: path.join(REPO, DIST, 'state') }],
    [['--http-port', String(P2), '--data', STUB, '--tls-cert', CERTS.cert, '--tls-key', path.join(STUB, 'GLOBAL', 'k.pem')], /--tls-key .* is inside --data/],
    [['--http-port', String(P2), '--data', path.join(STUB, 'GLOBAL')], /is inside --state/, { state: STUB }],
  ];
  for (const [args, msg, o = {}] of refusals) {
    const r = await startServer(args, { expectExit: true, ...o });
    assert.equal(r.code, 1, args.join(' '));
    assert.match(r.out(), msg);
  }
  // options with a value may come from the environment, with the same checks
  const e = await startServer(['--http-port', String(P2)], { expectExit: true, env: { NFSU2_ARTIFACTS: path.join(TMP, 'b') } });
  assert.equal(e.code, 1); assert.match(e.out(), /needs --dev/);
  // NFSU2_DEV in the environment does not turn on the test aids
  const s = await startServer(['--http-port', String(P2)], { env: { NFSU2_DEV: '1' } });
  try {
    for (const p of ['/test/hostcheck.html', `/tests/scripts/${SCRIPT}`, '/main.js.map', '/host/io-worker.js.map',
      '/TEST/hostcheck.html', '/Test/hostcheck.html', '/main.js.MAP', `/Tests/scripts/${SCRIPT}`, '/test%2fhostcheck.html',
      // the phase-2 deploy review's bypasses: an encoded slash that a later normalisation merged, and a
      // character the Mac's disk folds to "s" (U+017F); then their relatives
      '/%2ftest/play.html', '/%2Ftest/play.js', '/te%C5%BFt/play.html', `/%2ftests/scripts/${SCRIPT}`, '/%2fmain.js.map',
      '//test/play.html', '/x/..%2ftest/play.html', '/%5ctest/play.html', '/TE%C5%BFT/hostcheck.html', '/te%E1%BA%9Bt/play.html',
      '/%EF%BD%94est/play.html', '/main.js.%EF%BD%8Dap', `/te%C5%BFts/scripts/${SCRIPT}`, '/./test/play.html', '/test/./play.html',
      '/test%2f..%2ftest/play.html', '/%2e/test/play.html']) {
      const r = await rawRequest({ port: P2, data: req('GET', p) });
      assert.ok(r.status === 404 || r.status === 400, `${p}: ${r.status}`);
      assert.ok(r.body.length < 64 && !/script|import/.test(r.body.toString()), `${p}: ${r.body.toString().slice(0, 60)}`);
    }
    assert.equal((await rawRequest({ port: P2, data: req('PUT', '/artifacts/x.png', { headers: { 'Content-Length': 1 }, body: 'x' }) })).status, 405);
  } finally { await s.stop(); }
  const art = fs.mkdtempSync(path.join(TMP, 'art-'));
  const d = await startServer(['--http-port', String(P2), '--dev', '--artifacts', art]);
  try {
    assert.equal((await rawRequest({ port: P2, data: req('GET', '/test/hostcheck.html') })).status, AIDS ? 200 : 404);
    // with --dev the test pages and maps come from web/dist-dev; the page itself still from web/dist
    assert.equal((await rawRequest({ port: P2, data: req('GET', '/test/play.js') })).status, AIDS ? 200 : 404);
    assert.equal((await rawRequest({ port: P2, data: req('GET', '/main.js.map') })).status, AIDS ? 200 : 404);
    assert.equal((await rawRequest({ port: P2, data: req('GET', '/main.js') })).status, 200);
    assert.equal((await rawRequest({ port: P2, data: req('GET', '/%2ftest/play.html') })).status, 400, 'an encoded slash, even with --dev');
    assert.equal((await rawRequest({ port: P2, data: req('PUT', '/artifacts/x.png', { headers: { 'Content-Length': 1 }, body: 'x' }) })).status, 201);
    // a frame dump only from this machine itself, never through a proxy
    assert.equal((await rawRequest({ port: P2, data: req('PUT', '/artifacts/y.png', { headers: { 'Content-Length': 1, 'X-Forwarded-For': '203.0.113.9' }, body: 'y' }) })).status, 403);
    // never outside the folder: dot segments (cleaned by the URL parser: no longer an artifact path) and encoded ones
    assert.equal((await rawRequest({ port: P2, data: req('PUT', '/artifacts/../z.png', { headers: { 'Content-Length': 1 }, body: 'z' }) })).status, 405);
    assert.equal((await rawRequest({ port: P2, data: req('PUT', '/artifacts/%2e%2e%2fz.png', { headers: { 'Content-Length': 1 }, body: 'z' }) })).status, 400, 'an encoded slash');
    assert.equal((await rawRequest({ port: P2, data: req('PUT', '/artifacts/a%5cb.png', { headers: { 'Content-Length': 1 }, body: 'z' }) })).status, 400);
    assert.deepEqual(fs.readdirSync(art), ['x.png']);
    assert.ok(!fs.existsSync(path.join(path.dirname(art), 'z.png')));
  } finally { await d.stop(); }
});

inTurn('trusted proxies: client address headers believed only from a trusted peer', { skip: SKIP, timeout: 90_000 }, async () => {
  const logged = (s, marker) => { const l = s.out().split('\n').find(x => x.includes(marker)); return l ? l.split(' ')[1] : null; };
  let n = 0;
  const probe = async (s, target, headers) => {
    const marker = `m${++n}-${Date.now()}`;
    await rawRequest({ ...target, data: req('GET', `/players?${marker}`, { headers }) });
    await sleep(100);
    return logged(s, marker);
  };
  // (the LAN address as one of the server's names: the test certificate does not name it)
  const s = await startServer(['--port', String(P1), '--http-port', String(P2), '--tls-cert', CERTS.cert, '--tls-key', CERTS.key, '--access-log',
    ...(LAN_IP ? ['--host', LAN_IP] : [])]);
  try {
    const ca = CERTS.caPem;
    const local = { port: P2 };
    // this machine (a proxy here: Caddy, nginx, cloudflared) is trusted: X-Forwarded-For, right-most untrusted entry
    assert.equal(await probe(s, local, {}), '127.0.0.1');
    assert.equal(await probe(s, local, { 'X-Forwarded-For': '203.0.113.9' }), '203.0.113.9');
    assert.equal(await probe(s, local, { 'X-Forwarded-For': '198.51.100.1, 203.0.113.9' }), '203.0.113.9', 'the right-most entry (the one our proxy added)');
    assert.equal(await probe(s, local, { 'X-Forwarded-For': '203.0.113.9, 127.0.0.1' }), '203.0.113.9', 'trusted hops skipped');
    assert.equal(await probe(s, local, { 'X-Forwarded-For': 'garbage' }), '127.0.0.1');
    assert.equal(await probe(s, local, { 'CF-Connecting-IP': '203.0.113.50' }), '127.0.0.1', 'CF-Connecting-IP only when asked for');
    if (!LAN_IP) { console.log('no LAN address: the untrusted-peer checks are skipped'); return; }
    // from this machine's LAN address: not a trusted peer, so its headers say nothing
    const lan = { port: P1, useTls: true, host: LAN_IP, ca };
    assert.equal(await probe(s, lan, { 'X-Forwarded-For': '203.0.113.9', 'CF-Connecting-IP': '203.0.113.9', 'X-Real-IP': '203.0.113.9' }), LAN_IP);
    // an untrusted peer cannot claim another host for the /lan Origin check
    const evil = { Origin: 'https://evil.example', 'X-Forwarded-Host': 'evil.example' };
    assert.equal((await wsOpen(`wss://${LAN_IP}:${P1}${B}/lan`, { ca, headers: evil })).status, 403);
    // from a trusted proxy, X-Forwarded-Host counts, for one of this server's own names only (the audit's F7)
    assert.equal((await wsOpen(`ws://127.0.0.1:${P2}${B}/lan`, { headers: evil })).status, 403, 'not for another site\'s name');
    const fromLocal = await wsOpen(`ws://127.0.0.1:${P2}${B}/lan`, { headers: { Origin: 'https://localhost:9999', 'X-Forwarded-Host': 'localhost:9999' } });
    assert.equal(fromLocal.status, 101, 'X-Forwarded-Host from a trusted proxy counts');
    fromLocal.ws.close();
    // nor dodge the per-address limit with a different CF-Connecting-IP each time
    const socks = [];
    for (let i = 0; i < 8; i++) { const o = await wsOpen(`wss://${LAN_IP}:${P1}${B}/lan`, { ca, headers: { 'CF-Connecting-IP': `203.0.113.${i + 1}`, 'X-Forwarded-For': `203.0.113.${i + 1}` } }); assert.equal(o.status, 101); socks.push(o.ws); }
    const ninth = await wsOpen(`wss://${LAN_IP}:${P1}${B}/lan`, { ca, headers: { 'CF-Connecting-IP': '203.0.113.99' } });
    assert.equal(ninth.status, 429, 'spoofed CF-Connecting-IP from an untrusted peer ignored');
    for (const w of socks) w.close();
    // X-Forwarded-Proto from an untrusted peer: nothing (HSTS is about TLS this server sees, or a trusted proxy's word)
  } finally { await s.stop(); }
  if (!LAN_IP) return;
  // the LAN address as a configured proxy, with Cloudflare's header
  const t = await startServer(['--port', String(P1), '--http-port', String(P2), '--tls-cert', CERTS.cert, '--tls-key', CERTS.key, '--access-log', '--trusted-proxies', `${LAN_IP}/32`, '--real-ip-header', 'cf-connecting-ip']);
  try {
    const lan = { port: P1, useTls: true, host: LAN_IP };
    assert.equal(await probe(t, lan, { 'CF-Connecting-IP': '203.0.113.77' }), '203.0.113.77');
  } finally { await t.stop(); }
  // --trusted-proxies cloudflare: Cloudflare's ranges only (this LAN address is not one of them)
  const c = await startServer(['--port', String(P1), '--http-port', String(P2), '--tls-cert', CERTS.cert, '--tls-key', CERTS.key, '--access-log', '--trusted-proxies', 'cloudflare']);
  try {
    assert.match(c.out(), /proxies   : this machine, cloudflare \(client address from cf-connecting-ip\)/);
    const lan = { port: P1, useTls: true, host: LAN_IP };
    assert.equal(await probe(c, lan, { 'CF-Connecting-IP': '203.0.113.77' }), LAN_IP);
  } finally { await c.stop(); }
});

inTurn('HSTS: over a certificate or a proxy that says https, never on localhost or an IP', { skip: SKIP, timeout: 60_000 }, async () => {
  const hsts = async (target, host, headers = {}) => (await rawRequest({ ...target, data: req('GET', '/players', { host, headers }) })).headers['strict-transport-security'] ?? null;
  const s = await startServer(['--port', String(P1), '--http-port', String(P2), '--tls-cert', CERTS.cert, '--tls-key', CERTS.key]);
  try {
    const tlsT = { port: P1, useTls: true, servername: 'nfsu2.test' };
    assert.equal(await hsts(tlsT, 'nfsu2.test'), 'max-age=31536000');
    assert.equal(await hsts(tlsT, 'localhost:8527'), null);
    assert.equal(await hsts(tlsT, '127.0.0.1:8527'), null);
    assert.equal(await hsts({ port: P2 }, 'nfsu2.test'), null, 'plain HTTP');
    assert.equal(await hsts({ port: P2 }, 'nfsu2.test', { 'X-Forwarded-Proto': 'https' }), 'max-age=31536000', 'a trusted proxy says https');
  } finally { await s.stop(); }
  // no certificate: plain HTTP on this machine, never HSTS, whatever the request claims
  const l = await startServer(['--http-port', String(P2)]);
  try {
    assert.equal(await hsts({ port: P2 }, 'nfsu2.test'), null, 'no certificate');
    assert.equal(await hsts({ port: P2 }, 'nfsu2.test', { 'X-Forwarded-Proto': 'https' }), null, 'no certificate, a proxy that says https');
  } finally { await l.stop(); }
});

beside('Cloudflare simulation: client address, cache headers, ranges, brotli, the switch past an idle cut', { skip: SKIP, timeout: 600_000 }, async () => {
  const s = await startServer(['--behind-proxy', '--http-port', String(P6), '--host', 'nfsu2.test', '--access-log']);
  const sim = startCfSim({ port: P5, target: P6, certs: CERTS, fakeClientIp: '198.51.100.7', wsIdleMs: 40_000 });
  await sim.ready;
  const edge = { port: P5, useTls: true, servername: 'nfsu2.test' };
  try {
    // the page, as the edge hands it on: HSTS (the proxy says https), nothing for a shared cache to keep
    const page = await rawRequest({ ...edge, data: req('GET', '/', { host: 'nfsu2.test', headers: { 'CF-Connecting-IP': '1.2.3.4' } }) });
    assert.equal(page.status, 200);
    assert.equal(page.headers['strict-transport-security'], 'max-age=31536000');
    assert.equal(page.headers['cdn-cache-control'], 'no-store'); assert.equal(page.headers['cloudflare-cdn-cache-control'], 'no-store');
    assert.match(page.headers['cache-control'], /private, no-store, no-transform/);
    await sleep(100);
    assert.ok(s.out().includes(`[http] 198.51.100.7 GET ${B}/ 200`), 'the client address from CF-Connecting-IP (the edge replaced the one the client sent)');
    assert.ok(!s.out().includes('1.2.3.4'));
    // a byte range of a game file through the proxy
    const file = fs.readFileSync(path.join(DATA, EXE));
    const rg = await rawRequest({ ...edge, data: req('GET', `/data/${EXE}`, { host: 'nfsu2.test', headers: { Range: 'bytes=100-199' } }) });
    assert.equal(rg.status, 206); assert.equal(rg.headers['content-range'], `bytes 100-199/${file.length}`);
    assert.ok(rg.body.equals(file.subarray(100, 200)));
    // a run of compressed blocks through the proxy, as the edge asks for it (gzip, br): the exact bytes
    const run = await rawRequest({ ...edge, data: req('GET', `/data-blocks/${EXE}?b=1&n=2`, { host: 'nfsu2.test', headers: { 'Accept-Encoding': 'gzip, br' } }) });
    assert.equal(run.status, 200); assert.equal(run.headers['content-encoding'], 'br');
    assert.match(run.headers['cache-control'], /private, no-store, no-transform/);
    assert.ok(zlib.brotliDecompressSync(run.body).equals(file.subarray(1 << 20, 3 << 20)));
    // the module, brotli end to end, and a 304 for the browser's copy
    const mod = await rawRequest({ ...edge, data: req('GET', '/game.wasm', { host: 'nfsu2.test', headers: { 'Accept-Encoding': 'br' } }), timeout: 180_000 });
    assert.equal(mod.status, 200); assert.equal(mod.headers['content-encoding'], 'br');
    assert.match(mod.headers['cache-control'], /private, no-cache, no-transform/);
    const sha = b => crypto.createHash('sha256').update(b).digest('hex');
    assert.equal(sha(zlib.brotliDecompressSync(mod.body)), sha(fs.readFileSync(G.wasm)));
    const again = await rawRequest({ ...edge, data: req('GET', '/game.wasm', { host: 'nfsu2.test', headers: { 'Accept-Encoding': 'br', 'If-None-Match': `W/${mod.headers.etag}` } }) });
    assert.equal(again.status, 304, 'a weak If-None-Match (as a proxy may make it) still matches');
    // two players on the switch through the proxy; quiet for 70 s, past the proxy's 40 s idle cut
    const url = `wss://localhost:${P5}${B}/lan`, opts = { ca: CERTS.caPem, headers: { Origin: `https://localhost:${P5}` } };
    const a = await wsOpen(url, opts), b = await wsOpen(url, opts);
    assert.equal(a.status, 101); assert.equal(b.status, 101);
    let pings = 0; a.ws.on('ping', () => pings++);
    a.ws.send(JSON.stringify({ t: 'hello', name: 'Alice' })); b.ws.send(JSON.stringify({ t: 'hello', name: 'Bob' }));
    const aClosed = closeOf(a.ws);
    let gone = null; aClosed.then(c => { gone = c; });
    await sleep(70_000);
    assert.equal(gone, null, 'the connection outlived the idle cut');
    assert.equal(sim.stats.wsIdleCuts, 0);
    assert.ok(pings >= 2, `server pings while quiet (${pings} in 70 s)`);
    // and it still works: B -> A
    const welcomeA = JSON.parse(a.msgs.find(m => m.includes('welcome')));
    const f = Buffer.alloc(20); f[0] = 1; f.writeUInt32BE(welcomeA.vip >>> 0, 8); f.write('hey!', 16);
    // (the first binary message: a text one, such as a peers list, may come first)
    const got = new Promise(r => { const on = (d, bin) => { if (bin) { a.ws.off('message', on); r(d); } }; a.ws.on('message', on); });
    b.ws.send(f);
    assert.equal(Buffer.from(await got).subarray(16).toString(), 'hey!');
    for (const m of [...a.msgs, ...b.msgs]) assert.deepEqual(leaks(m), [], m);
    assert.ok(![...a.msgs, ...b.msgs].some(m => m.includes('198.51.100.7') || m.includes('127.0.0.1')), 'no real address on the switch');
    const peers = JSON.parse(a.msgs.filter(m => m.includes('"peers"')).pop()).peers;
    // the players; the server's own online host (server: true) is on the list for the game, not as a player (none
    // for a game whose server has no Online mode yet)
    assert.deepEqual(peers.filter(p => !p.server).map(p => p.name).sort(), ['Alice', 'Bob']);
    assert.deepEqual(peers.filter(p => p.server).map(p => p.vip), G.online ? [0x0a4d0001] : []);
    a.ws.close(); b.ws.close();
    console.log(`cf-sim: ${sim.stats.requests} requests, ${sim.stats.upgrades} upgrades, ${pings} pings in 70 s, idle cuts ${sim.stats.wsIdleCuts}`);
  } finally { await sim.close(); await s.stop(); }
  // the control: a WebSocket server that never pings, behind the same proxy (cut after 3 s quiet here)
  const bare = new WebSocketServer({ port: P6, host: '127.0.0.1' });
  const sim2 = startCfSim({ port: P5, target: P6, certs: CERTS, wsIdleMs: 3000 });
  await sim2.ready;
  try {
    const q = await wsOpen(`wss://localhost:${P5}/`, { ca: CERTS.caPem });
    assert.equal(q.status, 101);
    const t0 = Date.now();
    await closeOf(q.ws);
    const dt = (Date.now() - t0) / 1000;
    assert.equal(sim2.stats.wsIdleCuts, 1);
    console.log(`control: a WebSocket without pings was cut by the proxy after ${dt.toFixed(1)} s quiet`);
  } finally { await sim2.close(); await new Promise(r => bare.close(r)); }
});

beside('a renewed certificate is served without a restart (SIGHUP, file check)', { skip: SKIP, timeout: 150_000 }, async () => {
  const other = makeTestCerts(path.join(TMP, 'certs-renewed'));
  const live = fs.mkdtempSync(path.join(TMP, 'live-'));
  const crt = path.join(live, 'fullchain.pem'), key = path.join(live, 'privkey.pem');
  fs.copyFileSync(CERTS.cert, crt); fs.copyFileSync(CERTS.key, key);
  const served = () => new Promise((res, rej) => {
    const so = tls.connect({ host: '127.0.0.1', port: P4, servername: 'nfsu2.test', rejectUnauthorized: false }, () => { res(so.getPeerX509Certificate().fingerprint256); so.destroy(); });
    so.on('error', rej);
  });
  const fp = f => new crypto.X509Certificate(fs.readFileSync(f)).fingerprint256;
  const s = await startServer(['--port', String(P4), '--http-port', '0', '--tls-cert', crt, '--tls-key', key]);
  try {
    assert.equal(await served(), fp(CERTS.cert));
    fs.copyFileSync(other.cert, crt); fs.copyFileSync(other.key, key);
    s.c.kill('SIGHUP');
    await sleep(500);
    assert.equal(await served(), fp(other.cert), 'the new certificate after SIGHUP');
    assert.match(s.out(), /certificate reloaded/);
    // and without a signal: the file is checked once a minute
    await sleep(1100);                                   // a newer modification time than the last copy
    fs.copyFileSync(CERTS.cert, crt); fs.copyFileSync(CERTS.key, key);
    const t0 = Date.now();
    while (Date.now() - t0 < 80_000 && await served() !== fp(CERTS.cert)) await sleep(2000);
    assert.equal(await served(), fp(CERTS.cert), 'picked up by the file check');
    console.log(`certificate: reloaded on SIGHUP, and by the file check after ${((Date.now() - t0) / 1000).toFixed(0)} s`);
  } finally { await s.stop(); }
});

describe('deploy', { concurrency: true }, () => {
  describe('one after another', { concurrency: false }, () => { for (const a of IN_TURN) test(...a); });
  for (const a of BESIDE) test(...a);
});
