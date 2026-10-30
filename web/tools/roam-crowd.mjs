// Simulated free-roam players for load tests: n players, each the real core (runtime/roam_core.c in hub mode,
// tests/roam_crowd.c, built here for wasm without a C library) on its own WebSocket to a server's switch,
// driving along a track.  Real game pages (web/tools/roam-pair.mjs) can join the same server meanwhile.
//
//   node web/tools/roam-crowd.mjs [--base http://localhost:8655] [--n 62] [--seconds 120] [--room <name>] [--crew <name>]
//        [--track <pose.csv>] [--speed 15] [--spread 20] [--car 350Z] [--cars A,B,..|all] [--parts <list>|@<file>] [--stagger 50] [--states 1]
//        [--out <summary.json>]
//        [--race 1-3:60:120] [--bundle] [--first 0] [--loop-ms 10] [--hold 50] [--lobbies 7:S2_SPRINT_3,S2_URL_2]
//        [--game ug2|mw] (the game's cars and start point; Most Wanted has none yet) [--help] (this text and those)
//
// --bundle the players send and take bundles as the page does (web/src/net/index.ts TxBundle, nfsu2-lan kind 32): what
//          one tick or one received message made them send goes as one message, a snapshot to several friends once
//          with their addresses.  Without it each datagram is a message of its own, as a page of before sends them.
// --loop-ms the core's timers run this often (the game's network thread: runtime/web/roam_web.c, rc_tick every 10 ms),
//          each pass's datagrams one message, a car state still 20 times a second; without it everything runs once a
//          car state (50 ms), so a player's pings, pongs and HELLOs mostly go with its snapshots: fewer messages than
//          a page sends.
// --hold   a player's own datagrams (pings, pongs, HELLOs, the SNAP to the hub) wait for its next car state to a friend,
//          this many ms after its last message at most, as the page holds them (web/src/net/index.ts TX_HOLD_MS)
// --first  the first player's number (from 0), for several of these at once on one server: their names and client
//          addresses stay apart.
// --cars   the players' cars in turn (car type names, CarTypeInfo +0), or all: the game's 31 player cars, PEUGOT to
//          IMPREZAWRX; without it every player drives --car.
// --parts  the players' cars' parts instead of the stock ones, slot:key:owner in hex, comma-separated (or from a file): a
//          record names every part (tests/scripts/roam_crowd_rims.parts: a 350Z with the rim shops' rims).
// --states the car states in each SNAP: 1 as the browsers send them (roam_web.c), 3 as a LAN does.
// --crew   every player is in this crew (the Crews setting's one name): the hub links them to the players who share it
//          first, wherever they are, and those see them in the crew's colour.
// --race   players first to last (Bot01 is 1) are in a race from one second to the other after the start: they
//          announce it (RC_ST_EVENT) and send no car meanwhile, as a game in a race does.
// --lobbies every n-th player hosts a race lobby where it is at its first tick, one of these career races in turn (their
//          names: the hub keeps the race's name hash), with 1 to 5 players, its advert to the hub once a second: the world
//          map's lobby icons and labels (tests/scripts/roam_map_filter.txt).
// --track  a pose log (NFSU2_ROAM_POSELOG): the first player's L rows, a real drive; every simulated player
//          follows it from its own point along it, so they spread over the route.  Without one: circles of 20 to
//          120 m around the new career's start (1695, -884, 24), or --spread m apart (6 circles, the first at --spread).
// Each player is its own client address for the server's per-address limits (X-Forwarded-For, which the
// server takes from this machine, its trusted proxy).  Every 10 s one line: players on the switch, their peers
// (min, mean, max: the hub gives each at most 32), datagrams a second in and out, joins and "out of range"
// leaves, and their bytes.  Needs wasi-sdk in deps/ (its clang) and the ws package (web/node_modules).
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { WebSocket } from 'ws';
import { gameOf, usage, nothingFor } from './rig-game.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : d; };
// each game's free roam for the bots: its player cars (CarTypeInfo +0), the one they drive without --car or --cars
// and the new career's start (the circles' centre without --track)
const ROAM_OF = {
  ug2: {
    cars: ['PEUGOT', 'FOCUS', 'COROLLA', '240SX', 'MIATA', 'CIVIC', 'PEUGOT106', 'CORSA', 'HUMMER', 'NAVIGATOR', 'ESCALADE', 'TIBURON',
      'SENTRA', 'CELICA', 'IS300', 'SUPRA', 'GOLF', 'A3', 'RSX', 'ECLIPSE', 'TT', 'RX8', '350Z', 'G35', '3000GT', 'GTO', 'MUSTANGGT', 'SKYLINE',
      'LANCEREVO8', 'RX7', 'IMPREZAWRX'],
    car: '350Z', start: [1695, -884, 24.1],
  },
};
const NONE = 'none yet: no free-roam cars or start point for Most Wanted (M10-25)';
const GAME = gameOf(argv), R = ROAM_OF[GAME];
if (argv.includes('--help')) usage(import.meta.url, GAME, 'Cars and start', R ? [`--cars all: ${R.cars.join(' ')}`,
  `--car default: ${R.car}`, `start (circles without --track): ${R.start.join(', ')}`] : [], NONE);
if (!R) nothingFor(GAME, NONE);
const BASE = opt('base', 'http://localhost:8655'), N = +opt('n', 62), SECONDS = +opt('seconds', 120), ROOM = opt('room', ''), CREW = opt('crew', '');
const SPEED = +opt('speed', 15), CAR = opt('car', R.car), STAGGER = +opt('stagger', 50), TRACK = opt('track', ''), OUTF = opt('out', '');
const STATES = +opt('states', 1), SPREAD = +opt('spread', 20), BUNDLE = argv.includes('--bundle'), FIRST = +opt('first', 0);
const LOOP_MS = +opt('loop-ms', 0), HOLD = +opt('hold', 0);
const PARTS = (p => p.startsWith('@') ? fs.readFileSync(p.slice(1), 'utf8').trim() : p)(opt('parts', ''));
const RACE = (opt('race', '').match(/^(\d+)-(\d+):(\d+):(\d+)$/) ?? []).slice(1).map(Number);
const ALL_CARS = R.cars;
const CARS = opt('cars', '') === 'all' ? ALL_CARS : opt('cars', '') ? opt('cars', '').split(',') : [CAR];
const LOBBIES = (opt('lobbies', '').match(/^(\d+):(.+)$/) ?? []).slice(1);
const HUB = 0x0A4D00FD, PORT = 47210;

// the core for wasm: no C library (wasip1 for its headers: names.h's <string.h>), bulk memory for the copies
const WASI = fs.readdirSync(path.join(REPO, 'deps')).filter(d => d.startsWith('wasi-sdk-')).map(d => path.join(REPO, 'deps', d)).pop();
const WASM = path.join(REPO, 'out/web/test/roam_crowd.wasm');
fs.mkdirSync(path.dirname(WASM), { recursive: true });
execFileSync(path.join(WASI, 'bin/clang'), ['--target=wasm32-wasip1', '-O2', '-ffreestanding', '-nostdlib', '-mbulk-memory', '-ffp-contract=off',
  '-Iruntime', '-Wl,--no-entry', '-Wl,--export-dynamic', '-o', WASM, 'tests/roam_crowd.c', 'runtime/roam_core.c'], { cwd: REPO, stdio: 'inherit' });
const mod = await WebAssembly.compile(fs.readFileSync(WASM));

// a room's hash as roam_core.c rc_room_hash makes it of an ASCII name (capitals and spaces do not count; the core also
// folds capitals past A to Z and Unicode's blanks, which a crowd's names here never have)
const roomHash = s => {
  let h = 0x811C9DC5, kept = 0;
  for (let c of Buffer.from(s, 'utf8')) {
    if (c === 32 || (c >= 9 && c <= 13)) continue;
    if (c >= 65 && c <= 90) c += 32;
    h ^= c; h = Math.imul(h, 0x01000193) >>> 0; kept++;
  }
  return !kept ? 0 : h || 1;
};
const bhash = s => { let h = 0xFFFFFFFF; for (const c of Buffer.from(s, 'latin1')) h = (Math.imul(h, 33) + c) >>> 0; return h; };   // the game's bStringHash
const nowUs = () => (performance.timeOrigin + performance.now()) * 1000;

// the track: (t s) -> { p, v }
let track;
if (TRACK) {
  const rows = fs.readFileSync(TRACK, 'utf8').split('\n').filter(l => l.startsWith('L,')).map(l => l.split(','));
  const id = rows[0][3], pts = rows.filter(r => r[3] === id).map(r => ({ t: +r[1] / 1e6, p: [+r[4], +r[5], +r[6]] }));
  const t0 = pts[0].t, T = pts[pts.length - 1].t - t0;
  for (const q of pts) q.t -= t0;
  track = (t, i) => {
    const u = ((t + (i / N) * T) % T + T) % T;
    let lo = 0, hi = pts.length - 2;
    while (lo < hi) { const m = (lo + hi + 1) >> 1; if (pts[m].t <= u) lo = m; else hi = m - 1; }
    const k = lo;
    const a = pts[k], b = pts[k + 1], dt = Math.max(1e-3, b.t - a.t), f = Math.min(1, Math.max(0, (u - a.t) / dt));
    return { p: a.p.map((x, j) => x + f * (b.p[j] - x)), v: a.p.map((x, j) => (b.p[j] - x) / dt) };
  };
  console.log(`roam-crowd: track ${TRACK}: ${pts.length} points over ${T.toFixed(0)} s`);
} else {
  track = (t, i) => {
    const r = SPREAD + (i % 6) * SPREAD, w = SPEED / r, a = w * t + i;
    return { p: [R.start[0] + r * Math.cos(a), R.start[1] + r * Math.sin(a), R.start[2]], v: [-r * w * Math.sin(a), r * w * Math.cos(a), 0] };
  };
}

const st = { inDg: 0, outDg: 0, inB: 0, outB: 0, inMsg: 0, outMsg: 0, joins: 0, outOfRange: 0, otherLeaves: 0, notes: 0 }, why = {};
const tally = t => { const k = t.replace(/^Bot\d+/, '<bot>').replace(/\d+(\.\d+)*(:\d+)?/g, '#'); why[k] = (why[k] ?? 0) + 1; };
/** the frames of one tick (or one received message) as bundles, as the page's TxBundle makes them: a snapshot of
 *  the same bytes to another friend in the entry before it, unless something for that friend came between */
function bundled(q) {
  const e = [], last = new Map();
  for (const f of q) {
    const dst = f.readUInt32BE(8);
    let i = (last.get(dst) ?? -1) + 1;
    for (; i < e.length; i++) {
      const g = e[i].f;
      if (e[i].dst.length < 64 && g.length === f.length && !f.compare(g, 0, 8, 0, 8) && !f.compare(g, 12, g.length, 12, f.length)) break;
    }
    if (i < e.length) e[i].dst.push(dst); else e.push({ f, dst: [dst] });
    last.set(dst, i);
  }
  if (e.length === 1 && e[0].dst.length === 1) return [e[0].f];
  const n = 4 + e.reduce((a, x) => a + 4 + (x.dst.length > 1 ? 4 * x.dst.length : 0) + x.f.length, 0), b = Buffer.alloc(n);
  b[0] = 32;
  let at = 4;
  for (const { f, dst } of e) {
    const k = dst.length > 1 ? dst.length : 0;
    b[at] = k; b.writeUInt16BE(f.length, at + 2);
    for (let j = 0; j < k; j++) b.writeUInt32BE(dst[j], at + 4 + 4 * j);
    f.copy(b, at + 4 + 4 * k); at += 4 + 4 * k + f.length;
  }
  return [b];
}
class Bot {
  constructor(i) {
    this.i = i; this.name = `Bot${String(i + 1).padStart(2, '0')}`; this.vip = 0; this.ready = false; this.car = CARS[i % CARS.length];
    this.id = (Math.random() * 0xfffffffe + 1) >>> 0;
    this.bundles = false; this.q = [];                     // the switch takes bundles; what waits to go as one
  }
  async start() {
    this.inst = await WebAssembly.instantiate(mod, { crowd: {
      send: (ip, port, ptr, n) => this.out(ip, port, ptr, n),
      event: (ev, ptr, n) => {
        const t = Buffer.from(this.mem().subarray(ptr, ptr + n)).toString('latin1');
        if (ev === 1) st.joins++;
        else if (ev === 2) { if (t.includes('out of range')) st.outOfRange++; else { st.otherLeaves++; tally(t); } }
        else if (ev !== 4) { st.notes++; tally(t); }
      },
    } });
    this.x = this.inst.exports;
    const ws = this.ws = new WebSocket(BASE.replace(/^http/, 'ws') + '/lan', { headers: { 'X-Forwarded-For': `10.200.${this.i >> 8}.${this.i & 255}` } });
    ws.binaryType = 'nodebuffer';
    ws.on('error', e => console.log(`${this.name}: ${e.message}`));
    ws.on('close', (c, r) => { if (this.ready) console.log(`${this.name}: closed ${c} ${r}`); this.ready = false; });
    ws.on('message', (d, bin) => {
      if (!bin) {
        const m = JSON.parse(String(d));
        if (m.t === 'welcome' && !this.vip) {
          this.vip = m.vip >>> 0; this.bundles = BUNDLE && m.bundle === 1;
          ws.send(JSON.stringify({ t: 'hello', name: this.name, deltas: 1, ...(BUNDLE ? { bundle: 1 } : {}) }));
          const io = this.x.crowd_io(), m8 = this.mem();
          const b = Buffer.from(`${this.name}\0${this.car}\0${PARTS}\0`, 'latin1'); m8.set(b, io);
          this.x.crowd_init(this.id, roomHash(ROOM), roomHash(CREW), HUB, PORT, bhash(this.car), STATES, nowUs());
          this.ready = true;
        }
        return;
      }
      st.inMsg++;
      if (d[0] === 32) {
        for (let at = 4; at + 4 <= d.length;) {
          const f = at + 4 + 4 * d[at], end = f + d.readUInt16BE(at + 2);
          if (end > d.length) break;
          this.take(d.subarray(f, end)); at = end;
        }
      } else this.take(d);
      this.flush();
    });
  }
  /** a datagram from the switch into the core */
  take(d) {
    if (d.length < 16 || d[0] !== 1 || d.readUInt16BE(14) !== PORT || d.length - 16 > 1200) return;
    st.inDg++; st.inB += d.length;
    this.mem().set(d.subarray(16), this.x.crowd_io());
    this.x.crowd_recv(d.readUInt32BE(4), d.readUInt16BE(12), d.length - 16, nowUs());
  }
  mem() { return new Uint8Array(this.x.memory.buffer); }
  out(ip, port, ptr, n) {
    if (!this.ready || this.ws.readyState !== 1) return;
    const f = Buffer.alloc(16 + n);
    f[0] = 1; f.writeUInt32BE(this.vip, 4); f.writeUInt32BE(ip >>> 0, 8); f.writeUInt16BE(PORT, 12); f.writeUInt16BE(port, 14);
    f.set(this.mem().subarray(ptr, ptr + n), 16);
    st.outDg++; st.outB += f.length;
    if (this.bundles) this.q.push(f); else { this.ws.send(f); st.outMsg++; }
  }
  /** what waits, as bundles */
  flush(now = false) {
    if (!this.q.length) return;
    if (HOLD && !now && performance.now() - (this.sent ?? 0) < HOLD && !this.q.some(f => f[21] === 3 && f.readUInt32BE(8) !== HUB)) return;
    if (this.ws.readyState === 1) for (const m of bundled(this.q)) { this.ws.send(m); st.outMsg++; }
    this.q = []; this.sent = performance.now();
  }
  tick(t, snap = true) {
    if (!this.ready) return;
    const racing = RACE.length === 4 && this.i + 1 >= RACE[0] && this.i + 1 <= RACE[1] && t >= RACE[2] && t < RACE[3];
    if (racing !== !!this.racing) {
      this.racing = racing; this.x.crowd_status(racing ? 3 : 2);
      console.log(`roam-crowd: t=${t.toFixed(0)}s ${this.name} ${racing ? 'in a race' : 'back in free roam'}`);
    }
    if (racing || !snap) { this.x.crowd_wait(nowUs()); this.flush(); return; }
    const { p, v } = track(t, this.i);
    this.x.crowd_tick(nowUs(), p[0], p[1], p[2], v[0], v[1], v[2], Math.atan2(v[1], v[0]));
    if (LOBBIES.length && this.i % +LOBBIES[0] === 0 && t - (this.advT ?? -9) >= 1) {
      const races = LOBBIES[1].split(',');
      this.advT = t;
      this.lob ??= { id: (this.id ^ 0x5a5a) >>> 0, race: bhash(races[(this.i / +LOBBIES[0] | 0) % races.length]), x: Math.round(p[0]), y: Math.round(p[1]), n: 1 + this.i % 5 };
      this.x.crowd_lobby(this.lob.id, this.lob.race, this.lob.x, this.lob.y, this.lob.n, 6);
    }
    this.flush();
  }
  stop() { if (this.ready) { this.x.crowd_stop(); this.flush(true); } setTimeout(() => this.ws.close(), 200); }
}

const bots = [];
for (let i = 0; i < N; i++) { const b = new Bot(FIRST + i); bots.push(b); await b.start(); await new Promise(r => setTimeout(r, STAGGER)); }
const t0 = performance.now();
let passes = 0;
const timer = setInterval(() => {
  const t = (performance.now() - t0) / 1000, snap = !LOOP_MS || passes++ % Math.round(50 / LOOP_MS) === 0;
  for (const b of bots) b.tick(t, snap);
}, LOOP_MS || 50);
let last = { ...st }, lastT = performance.now();
const samples = [];
const report = setInterval(() => {
  const dt = (performance.now() - lastT) / 1000, on = bots.filter(b => b.ready), peers = on.map(b => b.x.crowd_peers());
  const s = { t: Math.round((performance.now() - t0) / 1000), players: on.length, peersMin: Math.min(...peers), peersMean: +(peers.reduce((a, b) => a + b, 0) / Math.max(1, peers.length)).toFixed(2),
    peersMax: Math.max(...peers), inPerS: Math.round((st.inDg - last.inDg) / dt), outPerS: Math.round((st.outDg - last.outDg) / dt),
    inKBs: +((st.inB - last.inB) / dt / 1024).toFixed(1), outKBs: +((st.outB - last.outB) / dt / 1024).toFixed(1),
    inMsgPerS: Math.round((st.inMsg - last.inMsg) / dt), outMsgPerS: Math.round((st.outMsg - last.outMsg) / dt),
    joins: st.joins - last.joins, outOfRange: st.outOfRange - last.outOfRange, otherLeaves: st.otherLeaves - last.otherLeaves };
  samples.push(s);
  console.log(`roam-crowd: t=${s.t}s players ${s.players}, peers ${s.peersMin}/${s.peersMean}/${s.peersMax}, datagrams ${s.inPerS}/s in ${s.outPerS}/s out ` +
    `(${s.inKBs} / ${s.outKBs} KB/s with the switch's 16-byte header) in ${s.inMsgPerS} / ${s.outMsgPerS} messages a second, ` +
    `joins ${s.joins}, out of range ${s.outOfRange}, other leaves ${s.otherLeaves}`);
  last = { ...st }; lastT = performance.now();
}, 10_000);
await new Promise(r => setTimeout(r, SECONDS * 1000));
clearInterval(timer); clearInterval(report);
for (const b of bots) b.stop();
await new Promise(r => setTimeout(r, 1000));
if (OUTF) fs.writeFileSync(OUTF, JSON.stringify({ n: N, seconds: SECONDS, track: TRACK || 'circles', totals: st, why, samples }, null, 1));
for (const [k, v] of Object.entries(why).sort((a, b) => b[1] - a[1]).slice(0, 8)) console.log(`roam-crowd:   ${v} x ${k}`);
console.log(`roam-crowd: done, ${st.joins} joins, ${st.outOfRange} out of range, ${st.otherLeaves} other leaves, ${st.notes} notes`);
process.exit(0);
