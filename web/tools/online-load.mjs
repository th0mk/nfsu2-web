// Load and abuse test of the online host (web/server-rs/crates/online) through the real
// server: many fake browsers on /lan (each its own WebSocket and virtual
// address; a different X-Forwarded-For per client, which the server takes from
// this machine as a trusted proxy, so the per-address WebSocket limit is the
// one a household would meet), each speaking the game's lobby protocol over
// the virtual TCP of runtime/web/net_lan.c: redirector, login, persona, a
// room, chat, and games in groups of four (create, find, join, ready, start,
// the race report).  Plus abusive clients: a chat flood (must be muted), a
// message flood (dropped), an oversized message (dropped), more lobby
// sessions from one browser than allowed (refused), a frame with a forged
// source address (it never reaches anyone as that address).
//
//   node web/tools/online-load.mjs [--clients 200] [--seconds 60] [--http-port 8679] [--base URL]
//        [--chat-every 5000] [--out <dir>]
// Without --base it starts its own server (--max-peers 250 --dev, state in <out>/state) and samples its
// memory and CPU.  Prints login and chat latencies (p50/p95/max), races started and reported, what the
// abusive clients got, the server's resident memory and CPU time; writes <out>/load.json.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { WebSocket } from 'ws';
import { serverCommand } from './server-cmd.mjs';
import { own, stop } from './children.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : d; };
const N = +opt('clients', 200), SECONDS = +opt('seconds', 60), CHAT_EVERY = +opt('chat-every', 5000);
const HTTP_PORT = +opt('http-port', 8679), OWN = !argv.includes('--base');
const BASE = opt('base', `http://localhost:${HTTP_PORT}`);
const OUT = path.resolve(opt('out', path.join(REPO, 'out/web/online-load')));
const DATA = opt('data', process.env.NFSU2_DATA || path.join(os.homedir(), 'projects/nsfu2/Need for Speed - Underground 2'));
fs.mkdirSync(OUT, { recursive: true });
const VH = ((10 << 24) | (77 << 16) | 1) >>> 0;
const sleep = ms => new Promise(r => setTimeout(r, ms));
const pct = (a, p) => { if (!a.length) return null; const s = [...a].sort((x, y) => x - y); return +s[Math.min(s.length - 1, Math.floor(p / 100 * s.length))].toFixed(1); };

// ---- the game's side of the lobby protocol (the server's: web/server-rs/crates/online/src/aries.rs, vhost.rs) ----
// a message: [0..3] kind, [4..7] code (0 = success), [8..11] u32 big-endian length (these 12 bytes included), then
// KEY=VALUE fields, NUL-terminated; a value with a byte below 0x20, DEL or = " : % ^ has them as %xx, and is in
// double quotes when it has a space (as SPEED2.EXE writes one)
const HDR = 12, SPECIAL = new Set([0x3d, 0x22, 0x3a, 0x25, 0x5e, 0x7f]), HEX = '0123456789abcdef';
function encodeValue(v) {
  const b = Buffer.from(String(v), 'latin1');
  let s = '';
  for (const c of b) s += c < 0x20 || SPECIAL.has(c) ? '%' + HEX[c >> 4] + HEX[c & 15] : String.fromCharCode(c);
  return b.includes(0x20) ? `"${s}"` : s;
}
const tag4 = t => { const b = Buffer.alloc(4); if (t) Buffer.from(String(t), 'latin1').copy(b, 0, 0, 4); return b; };
const tagText = b => [...b].filter(c => c).map(c => c >= 0x20 && c < 0x7f ? String.fromCharCode(c) : '\\x' + HEX[c >> 4] + HEX[c & 15]).join('');
/** one message: kind, code, fields ([[k, v]] or {k: v}) */
function encodeFrame(kind, code, fields = []) {
  const list = Array.isArray(fields) ? fields : Object.entries(fields);
  const body = Buffer.from(list.filter(([, v]) => v !== undefined && v !== null).map(([k, v]) => `${k}=${encodeValue(v)}\n`).join('') + '\0', 'latin1');
  const f = Buffer.alloc(HDR + body.length);
  tag4(kind).copy(f, 0); tag4(code).copy(f, 4); f.writeUInt32BE(f.length, 8); body.copy(f, HDR);
  return f;
}
/** the messages of a byte stream: push() returns those complete so far as { kind, code, body } */
class FrameReader {
  constructor(max) { this.max = max; this.buf = Buffer.alloc(0); this.error = null; }
  push(chunk) {
    if (this.error) return [];
    this.buf = this.buf.length ? Buffer.concat([this.buf, chunk]) : Buffer.from(chunk);
    const out = [];
    while (this.buf.length >= HDR) {
      const n = this.buf.readUInt32BE(8);
      if (n < HDR || n > this.max) { this.error = `a message of ${n} bytes (at most ${this.max})`; this.buf = Buffer.alloc(0); break; }
      if (this.buf.length < n) break;
      out.push({ kind: tagText(this.buf.subarray(0, 4)), code: tagText(this.buf.subarray(4, 8)), body: this.buf.subarray(HDR, n) });
      this.buf = this.buf.subarray(n);
    }
    return out;
  }
}
/** a body's fields as a Map, keys upper-cased (the first of a repeated key wins, as the game's own search finds it) */
function fieldMap(body) {
  const m = new Map();
  let end = body.indexOf(0); if (end < 0) end = body.length;
  for (let i = 0; i < end;) {
    while (i < end && body[i] <= 0x20) i++;
    if (i >= end) break;
    let k = i;
    while (k < end && body[k] > 0x20 && body[k] !== 0x3d) k++;
    const key = body.toString('latin1', i, k).toUpperCase();
    if (k >= end || body[k] !== 0x3d) { i = k; continue; }
    let q = k + 1, raw;
    if (body[q] === 0x22) { const a = q + 1; q = a; while (q < end && body[q] !== 0x22) q++; raw = body.toString('latin1', a, q); i = q + 1; }
    else { const a = q; while (q < end && body[q] > 0x20) q++; raw = body.toString('latin1', a, q); i = q; }
    if (!m.has(key)) m.set(key, raw.replace(/%([0-9a-fA-F]{2})/g, (_, h) => String.fromCharCode(parseInt(h, 16))));
  }
  return m;
}
/** one switch frame of a LAN kind: [0] kind, [4..7] source, [8..11] destination, [12..15] ports, then the LAN header
 * (op, ttl, 2 reserved, the connection id, little-endian) and the data */
function lanFrame(kind, src, dst, sport, dport, op, cid, data = null) {
  const n = data ? data.length : 0;
  const f = Buffer.alloc(16 + 8 + n);
  f[0] = kind; f.writeUInt32BE(src >>> 0, 4); f.writeUInt32BE(dst >>> 0, 8);
  f.writeUInt16BE(sport, 12); f.writeUInt16BE(dport, 14);
  f[16] = op; f[17] = 64; f.writeUInt32LE(cid >>> 0, 20);
  if (n) data.copy(f, 24);
  return f;
}

let server = null;
if (OWN) {
  server = own(spawn(...serverCommand(['--data', DATA, '--http-port', String(HTTP_PORT), '--port', '0',
    '--dev', '--max-peers', '250', '--state', path.join(OUT, 'state')]), { stdio: ['ignore', 'pipe', 'pipe'] }));
  const log = fs.createWriteStream(path.join(OUT, 'server.log'));
  server.stdout.pipe(log); server.stderr.pipe(log);
  await new Promise((res, rej) => { let o = ''; server.stdout.on('data', d => { o += d; if (o.includes('access')) res(); }); server.on('exit', c => rej(new Error(`server exited ${c}`))); });
}
const usage = () => {
  if (!server) return null;
  try { const [rss, cpu] = execFileSync('ps', ['-o', 'rss=,time=', '-p', String(server.pid)], { encoding: 'utf8' }).trim().split(/\s+/); return { rssMB: +(rss / 1024).toFixed(1), cpu }; } catch { return null; }
};

const stats = { login: [], chat: [], loginsOk: 0, loginsFailed: 0, races: 0, reports: 0, msgs: 0, resets: 0 };
const abuse = {};

/** one fake browser: a WebSocket to the switch, virtual TCP to the online host */
class Browser {
  // (each browser's own ports for its game's connections, 40000 up and round again: a long run opens more than fit in 16 bits)
  constructor(i) { this.i = i; this.conns = new Map(); this.closed = false; this.nextPort = 40000; }
  async open() {
    const u = new URL(BASE); u.protocol = u.protocol === 'https:' ? 'wss:' : 'ws:'; u.pathname = '/lan';
    this.ws = new WebSocket(u.href, { headers: { 'x-forwarded-for': `198.18.${this.i >> 8}.${this.i & 255}` } });
    this.ws.binaryType = 'nodebuffer';
    await new Promise((res, rej) => {
      this.ws.on('message', (d, bin) => {
        if (!bin) { const m = JSON.parse(String(d)); if (m.t === 'welcome') { this.vip = m.vip >>> 0; this.ws.send(JSON.stringify({ t: 'hello', name: `load${this.i}` })); res(); } return; }
        this.frame(Buffer.from(d));
      });
      this.ws.on('error', rej); this.ws.on('close', () => { this.closed = true; rej(new Error('closed')); });
    });
  }
  frame(f) {
    if (f[0] !== 4 || f.length < 24) return;
    const c = this.conns.get(f.readUInt16BE(14)), op = f[16];
    if (!c) return;
    if (op === 2) c.open?.();
    else if (op === 3) { c.reset = true; stats.resets++; c.onreset?.(); }
    else if (op === 5) c.fin = true;
    else if (op === 4) for (const m of c.r.push(f.subarray(24))) { stats.msgs++; const x = { kind: m.kind, code: m.code, f: fieldMap(m.body, []) }; c.on?.(x); for (const w of c.waits.splice(0)) w(x); }
  }
  tcp(port) {
    const port0 = this.nextPort; this.nextPort = this.nextPort >= 65535 ? 40000 : this.nextPort + 1;
    const c = { port: port0, cid: (Math.random() * 2 ** 32) >>> 0, r: new FrameReader(1 << 16), waits: [], reset: false, fin: false };
    this.conns.set(c.port, c);
    c.send = (kind, fields = []) => this.ws.readyState === 1 && this.ws.send(lanFrame(4, this.vip, VH, c.port, port, 4, c.cid, encodeFrame(kind, 0, fields)));
    c.raw = buf => this.ws.readyState === 1 && this.ws.send(lanFrame(4, this.vip, VH, c.port, port, 4, c.cid, buf));
    c.close = () => this.ws.readyState === 1 && this.ws.send(lanFrame(4, this.vip, VH, c.port, port, 5, c.cid));
    c.wait = (pred, ms = 10000) => new Promise((res, rej) => {
      const t = setTimeout(() => rej(new Error(`timeout waiting (${this.name})`)), ms);
      const w = x => { if (pred(x)) { clearTimeout(t); res(x); } else c.waits.push(w); };
      c.waits.push(w);
    });
    const opened = new Promise((res, rej) => { c.open = res; c.onreset = () => rej(new Error('refused')); setTimeout(() => rej(new Error('no SYNACK')), 10000); });
    opened.catch(() => {});   // (its caller awaits it; a late rejection is never one nobody hears of)
    this.ws.send(lanFrame(4, this.vip, VH, c.port, port, 1, c.cid));
    c.ready = opened;
    return c;
  }
  async login(name) {
    this.name = name;
    const t0 = performance.now();
    const d = this.tcp(20920); await d.ready;
    d.send('@dir', [['PROD', 'nfs-pc-2005']]); await d.wait(x => x.kind === '@dir'); d.close();
    const c = this.tcp(20922); await c.ready;
    c.send('addr', [['ADDR', '0.0.0.0']]); c.send('skey', [['SKEY', '$00']]); c.send('news', [['NAME', 7]]); c.send('sele', [['STATS', 5000]]);
    c.send('auth', [['NAME', name], ['PASS', 'load-test-not-a-secret']]);
    const a = await c.wait(x => x.kind === 'auth');
    if (a.code) throw new Error(`auth ${a.code}`);
    c.send('pers', [['PERS', name]]);
    const p = await c.wait(x => x.kind === 'pers');
    if (p.code) throw new Error(`pers ${p.code}`);
    stats.login.push(performance.now() - t0);
    this.lobby = c;
    return c;
  }
}

// a race report the server takes: version 3, n players, circuit, index order = places, plausible times
function report(n) {
  const b = Buffer.alloc(8 + 55 * n);
  b[0] = 3; b[1] = n; b[2] = 0; b[3] = 0; b.writeUInt16BE(4014, 4); b[7] = 2;
  for (let i = 0; i < n; i++) { const o = 8 + 55 * i; b[o] = i; b[o + 1] = i + 1; b[o + 2] = 1; b[o + 3] = 21; b.writeFloatBE(20 + i, o + 8); }
  return b.toString('base64');
}

const t0 = Date.now(), end = t0 + SECONDS * 1000;
const browsers = [];
console.log(`online-load: ${N} clients for ${SECONDS} s against ${BASE}`);
const before = usage();
// connect and log in, 20 at a time
for (let i = 0; i < N; i += 20) {
  await Promise.all(Array.from({ length: Math.min(20, N - i) }, async (_, k) => {
    const b = new Browser(i + k + 1);
    try { await b.open(); await b.login(`Load${i + k + 1}`); stats.loginsOk++; browsers.push(b); } catch (e) { stats.loginsFailed++; }
  }));
}
console.log(`online-load: ${stats.loginsOk} logged in, ${stats.loginsFailed} failed, login p50 ${pct(stats.login, 50)} ms p95 ${pct(stats.login, 95)} ms`);
const peak = { ...usage() };

// rooms and chat: everyone in one of the seven rooms; each chats every CHAT_EVERY ms; latency = send -> seen by a roommate
const ROOMS = ['Lounge', 'Circuit', 'Sprint', 'Drift', 'Drag', 'StreetX', 'URL'];
const sent = new Map();
for (const b of browsers) {
  b.lobby.send('move', [['NAME', ROOMS[b.i % ROOMS.length]]]);
  b.lobby.on = x => { if (x.kind === '+msg') { const t = sent.get(x.f.get('T')); if (t !== undefined) { stats.chat.push(performance.now() - t); sent.delete(x.f.get('T')); } } };
}
const chatTimer = setInterval(() => {
  for (const b of browsers) if (Math.random() < 200 / CHAT_EVERY) { const t = `hi ${b.i} ${Math.random().toString(36).slice(2, 8)}`; sent.set(t, performance.now()); b.lobby.send('mesg', [['TEXT', t]]); }
}, 200);

// games in groups of four, round after round until the time is up
async function group(g) {
  while (Date.now() < end - 8000) {
    const [h, ...js] = g, name = `L${h.i}.${Date.now() % 100000}`;
    try {
      h.lobby.send('gcre', [['NAME', name], ['MAXSIZE', 4], ['MINSIZE', 2], ['CUSTFLAGS', 1], ['SYSFLAGS', 0], ['PARAMS', 'TRACK=4014\nDIR=0\nLAPS=2']]);
      await h.lobby.wait(x => x.kind === 'gcre');
      for (const j of js) { j.lobby.send('gjoi', [['NAME', name]]); await j.lobby.wait(x => x.kind === 'gjoi'); j.lobby.send('gset', [['NAME', name], ['USERFLAGS', 134217728]]); }
      h.lobby.send('gsta', [['NAME', name]]);
      const ses = await h.lobby.wait(x => x.kind === '+ses');
      stats.races++;
      await sleep(2000);
      for (const b of g) {                                // the race report, from a new lobby session as the game does
        b.lobby.close();
        const c = await b.login(b.name);
        c.send('rank', [['AUTH', ses.f.get('AUTH')], ['RESU', report(g.length)], ...g.map((x, k) => [`NAME${k}`, x.name])]);
        await c.wait(x => x.kind === 'rank'); stats.reports++;
        c.send('move', [['NAME', ROOMS[b.i % ROOMS.length]]]);
        c.on = b.lobby.on;
      }
    } catch (e) { await sleep(1000); }
  }
}
const groups = [];
for (let i = 0; i + 4 <= Math.floor(browsers.length / 2); i += 4) groups.push(browsers.slice(i, i + 4));   // half the clients race
const racing = Promise.all(groups.map(group));

// the abusive clients
async function abusers() {
  const x = new Browser(9001); await x.open();
  const c = await x.login('Flooder');
  c.send('move', [['NAME', 'Lounge']]);
  for (let i = 0; i < 12; i++) c.send('mesg', [['TEXT', `flood ${i}`]]);
  const muted = await c.wait(m => m.kind === 'mesg' && m.code === 'mute', 3000).then(() => true, () => false);
  abuse.chatFloodMuted = muted;
  for (let i = 0; i < 100; i++) c.send('~png', [['TIME', 1]]);
  await sleep(500); abuse.messageFloodDropped = c.reset;
  const y = new Browser(9002); await y.open();
  const d = await y.login('Bigmouth');
  const big = Buffer.alloc(12); big.write('mesg', 0, 'latin1'); big.writeUInt32BE(9000, 8);   // announces a 9,000-byte message
  d.raw(big);
  await sleep(500); abuse.oversizedDropped = d.reset;
  const z = new Browser(9003); await z.open();
  let refused = 0;
  for (let i = 0; i < 6; i++) { try { await z.login(`Multi${i}`); } catch { refused++; } }
  abuse.sessionsPerBrowserRefused = refused;
  // a forged source address: the switch writes the sender's own; the online host sees only that one
  const f = lanFrame(4, 0x0a4d0002, VH, 41000, 20922, 1, 7);
  z.ws.send(f); abuse.forgedSourceSent = true;
}
const abused = abusers().catch(e => { abuse.error = String(e); });

await sleep(Math.max(0, end - Date.now()));
clearInterval(chatTimer);
await Promise.race([Promise.all([racing, abused]), sleep(15000)]);
const after = usage();
for (const b of browsers) try { b.ws.close(); } catch {}
const result = {
  clients: N, seconds: SECONDS, loginsOk: stats.loginsOk, loginsFailed: stats.loginsFailed,
  loginMs: { p50: pct(stats.login, 50), p95: pct(stats.login, 95), max: pct(stats.login, 100), n: stats.login.length },
  chatMs: { p50: pct(stats.chat, 50), p95: pct(stats.chat, 95), max: pct(stats.chat, 100), n: stats.chat.length },
  races: stats.races, reports: stats.reports, messagesIn: stats.msgs, resets: stats.resets, abuse,
  server: { before, afterLogins: peak, after },
};
await stop(server);
fs.writeFileSync(path.join(OUT, 'load.json'), JSON.stringify(result, null, 2));
console.log(JSON.stringify(result, null, 2));
process.exit(0);
