// A mixed crowd against one server, as nfsu2.xyz has it near its limit: players driving in free roam, players with
// free roam on but in a race or the menus (idle: they hold a place and send next to nothing), the game's Online mode,
// and more pages than places, so that a line forms (nfsu2-lan queue.rs), with players leaving and coming back all the
// while.  The bots are out/web/server-rs/nfsu2-roamload (each group a process of its own) and
// web/tools/online-load.mjs; this script starts the server, the groups, samples the server's CPU and memory (ps) and
// /players, and prints one report (and writes <out>/mix.json).
//
//   node web/tools/mix-load.mjs [--server out/web/server-rs/nfsu2-server] [--port 8697] [--max-peers 5000]
//        [--roam 1750] [--idle 2750] [--online 500] [--line 600] [--seconds 300] [--spread 3000]
//        [--churn 900] [--line-churn 300] [--leave-drop 0.5] [--drop-wait 0.2] [--plain | --compact] [--old]
//        [--threads 8] [--gen-threads 3] [--out <dir>] [--data <game folder>]
//   --roam, --idle  players driving / idle (free roam on); --online  Online lobby clients (online-load.mjs: logins,
//                  chat, races; pages of before: no line for them, so they start first); --line  more players than
//                  places, idle ones that come back after a while (--line-churn) and wait in line again
//   --churn        each roam and idle bot plays about this many seconds, then leaves (--leave-drop of them by a
//                  dropped link: the place is kept a minute) and comes back as a new player
//   --drop-wait    of the bots waiting in line, this share drop their link once and come back with their ticket
//   --plain, --compact   the bots' hello takes plain bundles or compact ones without deltas (default: deltas, as the
//                  page now does); --old: a server from before the line (no ?queue: a full server refuses)
//   --threads      the server's workers (NFSU2_THREADS); --gen-threads each bot process's threads
//   --check-every  one snapshot in this many is checked byte for byte (1: all; thousands driving: the checks would be
//                  the bots' limit, and they would measure themselves)
//   --blip 150:0.4 at 150 s, 40 % of the players on the switch lose their link at once (a Cloudflare Tunnel connection
//                  going) and come straight back with their address and token, as the page does
//   --game mw      Most Wanted's server (--game mw: its routes under /mw, no Online mode yet, so --online is 0 unless
//                  given) and Most Wanted's players (the bots' HELLO names the game); --data then Most Wanted's folder
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : d; };
const flag = k => argv.includes('--' + k);
const SERVER = path.resolve(opt('server', path.join(REPO, 'out/web/server-rs/nfsu2-server')));
const ROAMLOAD = path.resolve(opt('roamload', path.join(REPO, 'out/web/server-rs/nfsu2-roamload')));
const PORT = +opt('port', 8697), MAX = +opt('max-peers', 5000), SECONDS = +opt('seconds', 300);
const N = { roam: +opt('roam', 1750), idle: +opt('idle', 2750), online: +opt('online', 500), line: +opt('line', 600) };
const SPREAD = opt('spread', '3000'), CHURN = opt('churn', '900'), LINE_CHURN = opt('line-churn', '300');
const LEAVE_DROP = opt('leave-drop', '0.5'), DROP_WAIT = opt('drop-wait', '0.2');
const FORM = flag('plain') ? [] : flag('compact') ? ['--compact'] : ['--delta'];
const OLD = flag('old');
// --blip 150:0.4: at 150 s into the run, 40 % of the players on the switch lose their link and come back as the page does
const BLIP = opt('blip', '') ? opt('blip').split(':').map(Number) : null;
// the game (ug2 unless --game mw): its server's routes are under its base, and its bots say which game they play
const GAME = opt('game', 'ug2').toLowerCase();
if (GAME !== 'ug2' && GAME !== 'mw') { console.error(`--game ${GAME}: ug2 or mw`); process.exit(2); }
const GBASE = GAME === 'mw' ? '/mw' : '';
if (GAME === 'mw' && !argv.includes('--online')) N.online = 0;
// (Most Wanted's folder has no default: --data or NFSMW_DATA)
const DATA = opt('data', GAME === 'mw' ? process.env.NFSMW_DATA : (process.env.NFSU2_DATA || path.join(os.homedir(), 'projects/nsfu2/work/gamedata-ro')));
if (!DATA) { console.error(`--game ${GAME}: no game folder (--data "<the folder with speed.exe>", or NFSMW_DATA)`); process.exit(2); }
const OUT = path.resolve(opt('out', path.join(REPO, 'out/web/mix-load')));
fs.mkdirSync(OUT, { recursive: true });
const sleep = ms => new Promise(r => setTimeout(r, ms));
const BASE = `http://127.0.0.1:${PORT}${GBASE}`;

// ---- the server ----
const state = fs.mkdtempSync(path.join(os.tmpdir(), 'nfsu2-mix-'));
const env = { ...process.env, NFSU2_REPO: REPO, ...(opt('threads', '') ? { NFSU2_THREADS: opt('threads') } : {}) };
// UG2's server: a NFSU2_GAME left in the shell is not passed on (as web/tools/server-cmd.mjs does)
delete env.NFSU2_GAME;
const server = spawn(SERVER, [...(GAME === 'ug2' ? [] : ['--game', GAME]), '--data', DATA, '--http-port', String(PORT), '--max-peers', String(MAX), '--dev', '--state', state],
  { env, stdio: ['ignore', 'pipe', 'pipe'] });
const serverLog = [];
const logFile = fs.createWriteStream(path.join(OUT, 'server.log'));
for (const s of [server.stdout, server.stderr]) s.on('data', d => { logFile.write(d); for (const l of String(d).split('\n')) if (l) serverLog.push(l); });
await new Promise((res, rej) => {
  const t = setTimeout(() => rej(new Error('the server did not start')), 30000);
  const check = setInterval(() => { if (serverLog.some(l => l.includes('  access    :'))) { clearInterval(check); clearTimeout(t); res(); } }, 100);
  server.on('exit', c => rej(new Error(`the server exited ${c}`)));
});
console.log(`mix-load: ${path.relative(REPO, SERVER)}${GAME === 'ug2' ? '' : ` --game ${GAME}`} on ${PORT}, ${MAX} places; roam ${N.roam}, idle ${N.idle}, online ${N.online}, line ${N.line} ` +
  `(${N.roam + N.idle + N.online + N.line} pages); ${FORM.join(' ') || 'plain bundles'}${OLD ? ', a server of before (no line)' : ''}; ${SECONDS} s`);

const get = p => new Promise(res => {
  http.get(`${BASE}${p}`, r => { let b = ''; r.on('data', d => { b += d; }); r.on('end', () => res(b)); }).on('error', () => res(null));
});
// the server's CPU time (s) and resident memory (MB), from ps
const cpuSecs = t => { const p = t.split(/[:-]/).map(Number); return p.reduce((a, x) => a * 60 + x, 0); };
const usage = () => {
  try {
    const [rss, time] = execFileSync('ps', ['-o', 'rss=,time=', '-p', String(server.pid)], { encoding: 'utf8' }).trim().split(/\s+/);
    return { rssMB: +rss / 1024, cpu: cpuSecs(time) };
  } catch { return null; }
};

// ---- the groups ----
const groups = [];
function bots(tag, n, first, extra, seconds) {
  if (n <= 0) return null;
  const args = ['--base', BASE, '--n', String(n), '--first', String(first), '--seconds', String(Math.round(seconds)), '--spread', SPREAD,
    '--threads', opt('gen-threads', '3'), '--stagger-ms', '2', '--tag', tag, '--out', path.join(OUT, `${tag}.json`),
    ...(OLD ? [] : ['--queue']), '--check-every', opt('check-every', '1'), ...(GAME === 'ug2' ? [] : ['--game', GAME]), ...extra,
    // the blip at one moment of the run for every group (each counts from its own start)
    ...(BLIP ? ['--blip', `${(BLIP[0] - (Date.now() - t0) / 1000).toFixed(1)}:${BLIP[1]}`] : [])];
  const p = spawn(ROAMLOAD, args, { stdio: ['ignore', 'pipe', 'pipe'] });
  const g = { tag, n, p, samples: [], summary: null, err: '' };
  let buf = '';
  p.stdout.on('data', d => {
    buf += d;
    for (let i; (i = buf.indexOf('\n')) >= 0;) {
      const l = buf.slice(0, i); buf = buf.slice(i + 1);
      try { const j = JSON.parse(l); if ('t' in j) g.samples.push(j); else g.summary = j; } catch {}
    }
  });
  p.stderr.on('data', d => { g.err += d; });
  g.done = new Promise(r => p.on('exit', r));
  groups.push(g);
  return g;
}
const t0 = Date.now();
const ramp = 25;   // s: the groups come in one after another
// Online first (its clients cannot wait in line), then the players, the line last, once the server is full
let online = null;
if (N.online > 0) {
  const o = spawn(process.execPath, [path.join(REPO, 'web/tools/online-load.mjs'), '--base', BASE, '--clients', String(N.online),
    '--seconds', String(SECONDS + 2 * ramp), '--out', path.join(OUT, 'online')], { stdio: ['ignore', 'pipe', 'pipe'] });
  let out = '';
  const olog = fs.createWriteStream(path.join(OUT, 'online.log'));
  o.stdout.on('data', d => { out += d; olog.write(d); });
  o.stderr.on('data', d => { out += d; olog.write(d); });
  online = { done: new Promise(r => o.on('exit', r)), out: () => out };
  await sleep(8000);
}
bots('roam', N.roam, 4096, [...FORM, '--churn', CHURN, '--leave-drop', LEAVE_DROP, '--drop-wait', DROP_WAIT], SECONDS + ramp);
await sleep(ramp * 400);
bots('idle', N.idle, 16384, [...FORM, '--mode', 'idle', '--churn', CHURN, '--leave-drop', LEAVE_DROP, '--drop-wait', DROP_WAIT], SECONDS + ramp / 2);
await sleep(ramp * 600);
bots('line', N.line, 32768, [...FORM, '--mode', 'idle', '--churn', LINE_CHURN, '--leave-drop', LEAVE_DROP, '--drop-wait', DROP_WAIT], SECONDS);

// ---- sampling ----
const samples = [];
let last = usage(), lastT = Date.now();
const sampler = setInterval(async () => {
  const u = usage(), now = Date.now();
  const players = await get('/players');
  if (u && last) samples.push({ t: (now - t0) / 1000, cores: (u.cpu - last.cpu) / ((now - lastT) / 1000), rssMB: u.rssMB, players: players ? JSON.parse(players) : null });
  last = u; lastT = now;
  const s = samples.at(-1);
  if (s && Math.round(s.t) % 15 < 3) {
    const g = Object.fromEntries(groups.map(x => [x.tag, x.samples.at(-1)]));
    console.log(`  t=${s.t.toFixed(0)}s server ${s.cores.toFixed(2)} cores ${s.rssMB.toFixed(0)} MB; /players ${players}; ` +
      groups.map(x => { const l = g[x.tag]; return l ? `${x.tag} on ${l.players} wait ${l.waiting} peers ${l.peersMean} p99 ${l.latencyMs.p99} ms in ${l.bytesInPerPlayerS} B/s bad ${l.bad}` : `${x.tag} -`; }).join('; '));
  }
}, 3000);
await Promise.all(groups.map(g => g.done));
const onlineOut = online ? (await online.done, online.out()) : '';
clearInterval(sampler);
const info = await get('/players');
await sleep(500);
server.kill('SIGTERM');
await new Promise(r => server.once('exit', r));
fs.rmSync(state, { recursive: true, force: true });

// ---- the report ----
const steady = samples.filter(s => s.t > ramp + 20 && s.t < SECONDS);
const avg = a => a.length ? a.reduce((x, y) => x + y, 0) / a.length : null;
const max = a => a.length ? Math.max(...a) : null;
let onlineJson = null;
try { onlineJson = JSON.parse(onlineOut.slice(onlineOut.lastIndexOf('\n{'))); } catch { try { onlineJson = JSON.parse(fs.readFileSync(path.join(OUT, 'online/load.json'), 'utf8')); } catch {} }
const problems = serverLog.filter(l => /panic|error|ERROR|overload|cut |\bbusy\b/i.test(l) && !/^\s+(access|dev|online|saves|free roam)/.test(l));
const report = {
  server: path.relative(REPO, SERVER), game: GAME, maxPeers: MAX, form: FORM.join(' ') || 'plain', old: OLD, seconds: SECONDS, pages: N,
  serverCores: { mean: +avg(steady.map(s => s.cores))?.toFixed(2), max: +max(steady.map(s => s.cores))?.toFixed(2) },
  serverRssMB: { max: +max(samples.map(s => s.rssMB))?.toFixed(0) },
  players: { mean: Math.round(avg(steady.map(s => s.players?.players ?? 0))), max: max(samples.map(s => s.players?.players ?? 0)),
    queueMean: Math.round(avg(steady.map(s => s.players?.queue ?? 0))), queueMax: max(samples.map(s => s.players?.queue ?? 0)) },
  groups: Object.fromEntries(groups.map(g => [g.tag, g.summary])),
  online: onlineJson && { loginsOk: onlineJson.loginsOk, loginsFailed: onlineJson.loginsFailed, loginMs: onlineJson.loginMs, chatMs: onlineJson.chatMs,
    races: onlineJson.races, reports: onlineJson.reports, abuse: onlineJson.abuse },
  serverLines: { players: serverLog.filter(l => l.startsWith('[players]')), problems: problems.slice(0, 40), problemCount: problems.length },
  lastPlayers: info,
};
fs.writeFileSync(path.join(OUT, 'mix.json'), JSON.stringify({ report, samples, groupSamples: Object.fromEntries(groups.map(g => [g.tag, g.samples])) }, null, 2));
console.log(JSON.stringify(report, null, 2));
for (const g of groups) if (g.err.trim()) console.log(`${g.tag} stderr: ${g.err.trim().slice(0, 500)}`);
