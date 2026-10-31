// Side by side: the same load against builds of the server (web/server-rs, built with web/server-rs/build.sh), one
// after the other, on this machine: a change against main's, say.
//
//   node web/tools/server-compare.mjs [--load roam,online,http] [--n 120] [--seconds 60] [--servers a=<bin>,b=<bin>]
//        [--port 8691] [--data <game folder>] [--out <dir>] [--spread 3000] [--rate 20] [--gen-threads 8]
//        [--server-cpus 0-11] [--gen-cpus 12-23] [--server-env NFSU2_THREADS=8,NFSU2_BUNDLE_MS=8]
//   --server-cpus, --gen-cpus  the CPUs (taskset) the server and the load generators may use: apart, so that on
//                              one machine they do not take each other's cores, as on two machines
//   --server-env               environment for the server (its knobs: NFSU2_THREADS, NFSU2_BUNDLE_MS)
//   --servers                  name=binary for each (default rust=out/web/server-rs/nfsu2-server; a name alone is
//                              that binary too)
//
// Each server starts fresh for each load (its own state folder, --max-peers its most: 4000 for Rust,
// 2000 for the others; --dev), and while the load runs
// its process is sampled every second (CPU time and resident memory, from /proc), and a probe measures the
// switch's answer to a ping ({"t":"ping"}, a page's own measure of the switch) every 100 ms.  The loads:
//   roam    web/tools/roam-crowd.mjs --bundle: n players in free roam, in one spot (each with up to 32 friends)
//   crowd   out/web/server-rs/nfsu2-roamload (web/server-rs/build.sh): n simulated players in free roam, as many as a
//           full server holds (4000, 2000 for Node), spread over --spread metres (3000), each sending --rate snapshots a second
//           (20) to every friend the hub gives it; light enough to leave the cores to the server.  It reports the
//           friends each player has, the snapshots lost on the way, a snapshot's delay from player to player, the
//           players' own pings, and the messages each way; --gen-threads (8): its threads
//   mix     crowd and online at once, as nfsu2.xyz has them (2026-10-21: 850-1075 players, ~80 % in free roam, ~10 %
//           in Online mode): n - m players in free roam and m (--mix-online, n / 10) Online clients
//   online  web/tools/online-load.mjs: n clients in the Online lobby (logins, chat, games, races, abuse)
//   http    n clients fetching the game data's runs (/data-blocks/, compressed) as the page's workers do, each
//           its own client address (X-Forwarded-For) with 4 requests in flight on keep-alive connections
// The probe, and the HTTP clients (in 4 processes), run apart from this script, so that none of them waits behind
// another.  Besides the process's CPU, its threads' (busiestThread: the share of one core its busiest thread used,
// the ceiling of a server that runs everything on one; busyThreads: how many used a tenth of one or more), and the
// load generator's own CPU (a generator at its limit measures itself, not the server).  Prints one table and writes
// <out>/compare.json.  Linux (the /proc sampling).
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { WebSocket } from 'ws';
import { serverCommand, SERVER_BIN } from './server-cmd.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : d; };
const LOADS = opt('load', 'roam,online,http').split(',');
const BINS = Object.fromEntries(opt('servers', 'rust').split(',').map(s => s.split('=')).map(([n, b]) => [n, b ? path.resolve(b) : SERVER_BIN]));
const SERVERS = Object.keys(BINS);
const N = +opt('n', 120), SECONDS = +opt('seconds', 60), PORT = +opt('port', 8691);
const DATA = opt('data', process.env.NFSU2_DATA || path.join(os.homedir(), 'projects/nsfu2/Need for Speed - Underground 2'));
const OUT = path.resolve(opt('out', path.join(REPO, 'out/web/server-compare')));
fs.mkdirSync(OUT, { recursive: true });
const sleep = ms => new Promise(r => setTimeout(r, ms));
const pct = (a, p) => { if (!a.length) return null; const s = [...a].sort((x, y) => x - y); return +s[Math.min(s.length - 1, Math.floor(p / 100 * s.length))].toFixed(2); };
const TICK = 100;   // /proc's clock ticks a second (USER_HZ)
const SERVER_CPUS = opt('server-cpus', ''), GEN_CPUS = opt('gen-cpus', '');
const SERVER_ENV = Object.fromEntries(opt('server-env', '').split(',').filter(Boolean).map(kv => kv.split('=')));
/** [cmd, args] under taskset when cpus are given */
const pinned = (cpus, cmd, args) => (cpus ? ['taskset', ['-c', cpus, cmd, ...args]] : [cmd, args]);

async function startServer(impl, port, tag) {
  const state = fs.mkdtempSync(path.join(os.tmpdir(), `nfsu2-compare-${impl}-`));
  const [cmd, args] = serverCommand(['--data', DATA, '--http-port', String(port), '--max-peers', '4000', '--dev', '--state', state],
    BINS[impl]);
  const c = spawn(...pinned(SERVER_CPUS, cmd, args), { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ...SERVER_ENV } });
  const log = fs.createWriteStream(path.join(OUT, `server-${impl}-${tag}.log`));
  c.stdout.pipe(log); c.stderr.pipe(log);
  await new Promise((res, rej) => {
    let o = '';
    const t = setTimeout(() => rej(new Error(`${impl}: no start-up line`)), 30_000);
    c.stdout.on('data', d => { o += d; if (o.includes('  access    :')) { clearTimeout(t); res(); } });
    c.on('exit', code => rej(new Error(`${impl} exited ${code}`)));
  });
  // the module's compression at the start is not the load's
  await sleep(4000);
  return { c, state, stop: async () => { const gone = new Promise(r => c.once('exit', r)); c.kill('SIGTERM'); await Promise.race([gone, sleep(5000)]); c.kill('SIGKILL'); fs.rmSync(state, { recursive: true, force: true }); } };
}

/** CPU seconds (user + system) and resident MB of a process */
function usage(pid) {
  try {
    const st = fs.readFileSync(`/proc/${pid}/stat`, 'utf8').split(') ')[1].split(' ');
    const rss = +/VmRSS:\s+(\d+)/.exec(fs.readFileSync(`/proc/${pid}/status`, 'utf8'))[1] / 1024;
    return { cpu: (+st[11] + +st[12]) / TICK, rss };
  } catch { return null; }
}

/** CPU seconds of each thread of a process: tid -> seconds */
function threadCpu(pid) {
  const m = new Map();
  try {
    for (const t of fs.readdirSync(`/proc/${pid}/task`)) {
      try { const st = fs.readFileSync(`/proc/${pid}/task/${t}/stat`, 'utf8').split(') ')[1].split(' '); m.set(t, (+st[11] + +st[12]) / TICK); } catch { /* gone */ }
    }
  } catch { /* gone */ }
  return m;
}
function threadShares(a, b, wall) {
  const shares = [...b].map(([t, c]) => (c - (a.get(t) ?? 0)) / wall);
  return { busiestThread: shares.length ? +Math.max(...shares).toFixed(2) : null, busyThreads: shares.filter(x => x >= 0.1).length };
}

/** the switch's answer to a page's ping, every 100 ms */
function pingProbe(port) {
  const rtt = []; let lost = 0, id = 0;
  const sent = new Map();
  const ws = new WebSocket(`ws://127.0.0.1:${port}/lan`, { headers: { 'x-forwarded-for': '198.18.255.254' } });
  let timer = null;
  ws.on('message', (d, bin) => {
    if (bin) return;
    const m = JSON.parse(String(d));
    if (m.t === 'welcome') { ws.send(JSON.stringify({ t: 'hello', name: 'probe' })); timer = setInterval(() => { sent.set(++id, performance.now()); ws.send(JSON.stringify({ t: 'ping', id })); }, 100); }
    if (m.t === 'pong' && sent.has(m.id)) { rtt.push(performance.now() - sent.get(m.id)); sent.delete(m.id); }
  });
  ws.on('error', () => {});
  return { stop: () => { clearInterval(timer); lost = sent.size; ws.terminate(); return { p50: pct(rtt, 50), p90: pct(rtt, 90), p99: pct(rtt, 99), max: pct(rtt, 100), n: rtt.length, lost }; } };
}

function run(cmd, args, file) {
  return new Promise(res => {
    const c = spawn(...pinned(GEN_CPUS, cmd, args), { stdio: ['ignore', 'pipe', 'pipe'], cwd: REPO });
    const log = fs.createWriteStream(file);
    c.stdout.pipe(log); c.stderr.pipe(log);
    c.on('exit', code => res(code));
  });
}

// ---- the loads ----
async function roamLoad(port, tag) {
  const outf = path.join(OUT, `roam-${tag}.json`);
  await run(process.execPath, [path.join(REPO, 'web/tools/roam-crowd.mjs'), '--base', `http://127.0.0.1:${port}`, '--n', String(N),
    '--seconds', String(SECONDS), '--bundle', '--stagger', '20', '--out', outf], path.join(OUT, `roam-${tag}.log`));
  const j = JSON.parse(fs.readFileSync(outf, 'utf8'));
  const late = j.samples.slice(1);   // the first ten seconds are the players joining
  const mean = k => late.length ? Math.round(late.reduce((a, s) => a + s[k], 0) / late.length) : null;
  return { peersMean: late.length ? +(late.reduce((a, s) => a + s.peersMean, 0) / late.length).toFixed(1) : null,
    datagramsInPerS: mean('inPerS'), datagramsOutPerS: mean('outPerS'), messagesInPerS: mean('inMsgPerS'), messagesOutPerS: mean('outMsgPerS'),
    otherLeaves: j.totals.otherLeaves };
}

const SPREAD = +opt('spread', 3000), RATE = +opt('rate', 20), GEN_THREADS = +opt('gen-threads', 8);
async function crowdLoad(port, tag, n = N, first = 0) {
  const outf = path.join(OUT, `crowd-${tag}.json`);
  const bin = process.env.NFSU2_ROAMLOAD || path.join(REPO, 'out/web/server-rs/nfsu2-roamload');
  if (!fs.existsSync(bin)) throw new Error(`no ${bin} (web/server-rs/build.sh)`);
  const c = spawn(...pinned(GEN_CPUS, bin, ['--base', `http://127.0.0.1:${port}`, '--n', String(n), '--first', String(first), '--seconds', String(SECONDS),
    '--spread', String(SPREAD), '--rate', String(RATE), '--threads', String(GEN_THREADS), '--stagger-ms', String(Math.max(1, Math.round(10_000 / n))), '--out', outf]),
    { stdio: ['ignore', 'pipe', 'pipe'] });
  const log = fs.createWriteStream(path.join(OUT, `crowd-${tag}.log`));
  c.stdout.pipe(log); c.stderr.pipe(log);
  const u0 = usage(c.pid), t0 = performance.now();
  let u1 = u0;
  const sampler = setInterval(() => { u1 = usage(c.pid) ?? u1; }, 1000);
  await new Promise(res => c.on('exit', res));
  clearInterval(sampler);
  const j = JSON.parse(fs.readFileSync(outf, 'utf8'));
  // the samples after the players joined and the hub linked them (10 s)
  const ramp = 10 + 10, late = j.samples.filter(s => s.t > ramp);
  const mean = k => late.length ? Math.round(late.reduce((a, s) => a + s[k], 0) / late.length) : null;
  const st = j.summary.steady;
  return { players: late.length ? Math.min(...late.map(s => s.players)) : null,
    peersMean: late.length ? +(late.reduce((a, s) => a + s.peersMean, 0) / late.length).toFixed(1) : null,
    deliveriesPerS: mean('deliveriesPerS'), datagramsInPerS: mean('dgramsInPerS'), msgsToPlayersPerS: mean('msgsInPerS'), msgsFromPlayersPerS: mean('msgsOutPerS'),
    snapLossPct: st.snapLossPct, snapDelayMs: st.latencyMs, playersPingMs: st.pingMs, refused: j.summary.refused, closed: j.summary.closed,
    generatorCores: u0 && u1 ? +((u1.cpu - u0.cpu) / ((performance.now() - t0) / 1000)).toFixed(2) : null };
}

/** free roam and Online mode at once: the crowd's players from bot 4096 on (198.18.16.0 and up), so that no client
 *  address is both an Online client's (online-load.mjs: 198.18.0.x) and a player's */
async function mixLoad(port, tag) {
  const m = +opt('mix-online', Math.round(N / 10));
  const [crowd, online] = await Promise.all([crowdLoad(port, tag, N - m, 4096), onlineLoad(port, tag, m)]);
  return { ...crowd, ...Object.fromEntries(Object.entries(online).map(([k, v]) => ['online.' + k, v])) };
}

async function onlineLoad(port, tag, clients = N) {
  const out = path.join(OUT, `online-${tag}`);
  await run(process.execPath, [path.join(REPO, 'web/tools/online-load.mjs'), '--base', `http://127.0.0.1:${port}`, '--clients', String(clients),
    '--seconds', String(SECONDS), '--out', out], path.join(OUT, `online-${tag}.log`));
  const j = JSON.parse(fs.readFileSync(path.join(out, 'load.json'), 'utf8'));
  return { loginsOk: j.loginsOk, loginsFailed: j.loginsFailed, loginP50: j.loginMs.p50, loginP95: j.loginMs.p95, chatP50: j.chatMs.p50, chatP95: j.chatMs.p95,
    races: j.races, reports: j.reports };
}

/** the HTTP clients in WORKERS processes, their results together */
const WORKERS = 4;
async function httpLoads() {
  const per = Math.ceil(N / WORKERS);
  const rs = await Promise.all(Array.from({ length: WORKERS }, (_, w) => child(['--http-worker', '--port', String(PORT), '--n', String(per),
    '--first', String(w * per), '--seconds', String(SECONDS), '--data', DATA]).result));
  const sum = k => rs.reduce((a, r) => a + r[k], 0);
  return { requestsPerS: sum('requestsPerS'), MBPerS: +sum('MBPerS').toFixed(1), latencyP50: Math.max(...rs.map(r => r.latencyP50)),
    latencyP99: Math.max(...rs.map(r => r.latencyP99)), errors: sum('errors') };
}

async function httpLoad(port, first = 0) {
  const base = `http://127.0.0.1:${port}`;
  const manifest = await (await fetch(`${base}/data-manifest`)).json();
  const files = manifest.files.filter(f => !f.d && f.s > 0);
  const runs = [];
  for (const f of files) for (let b = 0; b * (1 << 20) < f.s; b += 4) runs.push(`/data-blocks/v2/${f.p.split('/').map(encodeURIComponent).join('/')}?b=${b}&n=4&m=${f.m}&s=${f.s}`);
  const lat = []; let bytes = 0, requests = 0, errors = 0;
  const end = Date.now() + SECONDS * 1000;
  const client = async (c) => {
    const agent = new http.Agent({ keepAlive: true, maxSockets: 4 });
    let i = c * 7919;
    const one = async () => {
      while (Date.now() < end) {
        const p = runs[i++ % runs.length], t0 = performance.now();
        try {
          await new Promise((res, rej) => {
            const q = http.get({ host: '127.0.0.1', port, path: p, agent, headers: { host: `localhost:${port}`, 'accept-encoding': 'zstd, br, gzip', 'x-forwarded-for': `198.18.${c >> 8}.${c & 255}` } }, r => {
              r.on('data', d => { bytes += d.length; }); r.on('end', () => (r.statusCode === 200 ? res() : rej(new Error(String(r.statusCode))))); r.on('error', rej);
            });
            q.on('error', rej);
          });
          lat.push(performance.now() - t0); requests++;
        } catch { errors++; await sleep(50); }
      }
    };
    await Promise.all([one(), one(), one(), one()]);
    agent.destroy();
  };
  const t0 = performance.now();
  await Promise.all(Array.from({ length: N }, (_, c) => client(first + c)));
  const s = (performance.now() - t0) / 1000;
  return { requestsPerS: Math.round(requests / s), MBPerS: +(bytes / s / 1048576).toFixed(1), latencyP50: pct(lat, 50), latencyP99: pct(lat, 99), errors };
}

// the probe and the HTTP clients run in processes of their own (--probe, --http-worker): in this one they would wait
// behind each other (700 MB/s of answers kept this process too busy to answer a ping in time)
if (argv.includes('--probe')) {
  const p = pingProbe(PORT);
  process.on('message', () => { process.send(p.stop()); process.exit(0); });
  await new Promise(() => {});
}
if (argv.includes('--http-worker')) {
  const first = +opt('first', 0);
  process.send(await httpLoad(PORT, first));
  process.exit(0);
}
function child(args) {
  const c = spawn(...pinned(GEN_CPUS, process.execPath, [fileURLToPath(import.meta.url), ...args]), { stdio: ['ignore', 'inherit', 'inherit', 'ipc'] });
  return { c, result: new Promise(res => c.once('message', res)) };
}
const results = {};
for (const load of LOADS) {
  for (const impl of SERVERS) {
    const tag = `${load}-${impl}`;
    console.log(`server-compare: ${load} against ${impl} (${N} clients, ${SECONDS} s)`);
    const s = await startServer(impl, PORT, load);
    const u0 = usage(s.c.pid), t0 = performance.now(), th0 = threadCpu(s.c.pid);
    let rssMax = u0?.rss ?? 0;
    const sampler = setInterval(() => { const u = usage(s.c.pid); if (u) rssMax = Math.max(rssMax, u.rss); }, 1000);
    const probe = child(['--probe', '--port', String(PORT)]);
    let r;
    try { r = load === 'roam' ? await roamLoad(PORT, tag) : load === 'crowd' ? await crowdLoad(PORT, tag) : load === 'mix' ? await mixLoad(PORT, tag) : load === 'online' ? await onlineLoad(PORT, tag) : await httpLoads(); }
    catch (e) { r = { error: String(e?.message ?? e) }; }
    probe.c.send('stop');
    const ping = await probe.result;
    clearInterval(sampler);
    const u1 = usage(s.c.pid), wall = (performance.now() - t0) / 1000;
    results[tag] = { ...r, cpuCores: u0 && u1 ? +((u1.cpu - u0.cpu) / wall).toFixed(2) : null, ...threadShares(th0, threadCpu(s.c.pid), wall),
      rssStartMB: u0 ? Math.round(u0.rss) : null, rssMaxMB: Math.round(rssMax), pingMs: ping };
    await s.stop();
    console.log(`server-compare: ${tag}: ${JSON.stringify(results[tag])}`);
  }
}
fs.writeFileSync(path.join(OUT, 'compare.json'), JSON.stringify({ n: N, seconds: SECONDS, cpus: os.cpus().length, results }, null, 1));

// the table: one row per measure, one column per server, per load
for (const load of LOADS) {
  const cols = SERVERS.map(impl => results[`${load}-${impl}`] ?? {});
  const keys = [...new Set(cols.flatMap(c => Object.keys(c)))];
  const cell = v => v === null || v === undefined ? '-' : typeof v === 'object' ? `${v.p50}/${v.p90}/${v.p99} (max ${v.max})` : String(v);
  console.log(`\n${load} (${N} clients, ${SECONDS} s)`);
  console.log(['', ...SERVERS].map((h, i) => i ? h.padStart(28) : h.padEnd(20)).join(''));
  for (const k of keys) console.log([k.padEnd(20), ...cols.map(c => cell(c[k]).padStart(28))].join(''));
}
console.log(`\n(pingMs: p50/p90/p99 of the switch's answer to a ping every 100 ms; cpuCores: the server's CPU time over the wall time)`);
process.exit(0);
