// LAN play between browsers: two (to six) game instances in headless Chrome
// pages (separate browser contexts, so separate OPFS) on one local server,
// racing each other over the server's virtual LAN -- the browser version of
// the Mac's tests/net/lan_race.sh.  Instance 1 hosts with the lifted
// server.dll (Host LAN Server, Select Car, Create Game), the others find it,
// join and ready; the game's AI drives every car (drag: the synthetic pad).
// Scripts from tools/gen_lan_race.py, with lan_race.sh's options.
//
//   node web/tools/lan-pair.mjs --out <dir> [--mode circuit|sprint|drag] [--races N] [--players 2..6]
//        [--http-port 8643] [--base http://localhost:8623] [--timeout <s>] [--stagger <s>]
//        [--env K=V ...] [--host-env K=V ...] [--join-env K=V ...] [--headed] [--bounce <s>]
//        [--host-arg X ...] [--join-arg X ...]   more tools/gen_lan_race.py options, e.g. --join-arg --confirm-join
//                    (a slow link: the joiner answers the game's "connection quality below average" question)
//        [--impair delay=MS,jitter=MS,loss=PCT,reorder=PCT,hold=PCT,holdms=MS,seed=N]   (own server only: the
//                    switch's --lan-impair, vlanhub.py's impairments on LAN play's frames; hold: a link held for
//                    holdms, then everything at once, as a player's WebSocket after a lost TCP segment)
//        [--page play [--size 960x540] [--play-only N]]   the renderer's test page instead of the null
//                    platform's (WebGPU; frame dumps go to the server's /artifacts/ as i<n>-*.png); --play-only:
//                    only instance N renders (two renderers at once overloaded a busy machine: §4.4 of the report)
//        [--page app]  each instance is the player's start page (web/index.html): Play is clicked; the
//                    renderer, audio, input and the network are the app's (it joins the switch by itself,
//                    free roam off, as a player who only uses the game's LAN menu)
//        [--drop N:S]  instance N (1-based) closes its page S seconds after its network race began, as a
//                    player who closes the tab mid-race: the others must still reach the results; the dropped
//                    one is left out of the judgement and of the trace check
//        --mode drift|streetx: drift and Street X games (the mode carousel's 4th and 5th entries, as the
//                    "mode" dumps of the runs show: Circuit, Sprint, Drag, Drift, Street X)
//        [--game ug2|mw]   the game (ug2 unless given): its script generator, modes and results screen; Most
//                    Wanted has none yet
//        [--help]    this text and the game's modes
//
// Without --base it starts its own server (web/tools/server-cmd.mjs, the game
// folder from --data or NFSU2_DATA) and stops it by its child handle at the end.
// --bounce <s>: stops that server <s> s after the first page opened and starts
// it again 3 s later (the pages reconnect; TCP connections are reset).
//
// Writes <out>/i<n>/log.txt (the game's output, [tid] prefixes removed, as the
// Mac's logs), <out>/i<n>/cars.csv (NFSU2_CAR_TRACE, from the page's OPFS),
// <out>/i<n>/page.log (the page console with time stamps and NETSTAT lines),
// <out>/host.txt, join.txt (the scripts), trace.txt (tools/lan_trace.py) and
// summary.json.  PASS as lan_race.sh: every instance exits 0 through its
// script, reaches the results screen, logs no fault / timeout / trap /
// unresolved call / "[net] BLOCKED", and lan_trace.py finds each car where its
// owner had it while the cars are within 150 m (p95 < 1.5 m, no jumps, at
// least 500 samples).
import { chromium } from 'playwright';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync, spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { serverCommand } from './server-cmd.mjs';
import { GAMES, gameOf, usage, nothingFor, requireGame } from './rig-game.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : d; };
const opts = (k) => argv.flatMap((a, i) => (a === '--' + k && i + 1 < argv.length ? [argv[i + 1]] : []));
// each game's LAN races: the script generator with lan_race.sh's options per mode (host, joiners) and the results
// screen
const RACES_OF = {
  ug2: {
    gen: 'tools/gen_lan_race.py', results: 'screen UI_PostRaceResults.fng',
    modes: {
      circuit: { h: ['--mode', '0', '--race-ms', '120000'], j: ['--refresh', '--race-ms', '120000'] },
      sprint: { h: ['--mode', '1', '--race-ms', '90000'], j: ['--filter', '1', '--race-ms', '90000'] },
      drag: { h: ['--mode', '2', '--drag', '--race-ms', '40000'], j: ['--filter', '2', '--drag', '--race-ms', '40000'] },
      drift: { h: ['--mode', '3', '--race-ms', '120000'], j: ['--filter', '3', '--race-ms', '120000'] },
      streetx: { h: ['--mode', '4', '--race-ms', '120000'], j: ['--filter', '4', '--race-ms', '120000'] },
    },
  },
};
const NONE = 'none yet: no LAN menu script generator for Most Wanted (M8-12)';
const GAME = gameOf(argv), G = GAMES[GAME], LAN = RACES_OF[GAME];
if (argv.includes('--help')) usage(import.meta.url, GAME, 'Modes (--mode)', Object.entries(LAN?.modes ?? {})
  .map(([m, { h, j }]) => `${m.padEnd(8)} ${LAN.gen} host ${h.join(' ')}; join ${j.join(' ')}`), NONE);
if (!LAN) nothingFor(GAME, NONE);
const HTTP_PORT = +opt('http-port', 8643);
const OWN = !argv.includes('--base');
const BASE = opt('base', `http://localhost:${HTTP_PORT}`);
if (!OWN) await requireGame(BASE, GAME);
const DATA = opt('data', process.env.NFSU2_DATA || path.join(os.homedir(), 'projects/nsfu2/Need for Speed - Underground 2'));
const OUT = path.resolve(opt('out', path.join(REPO, 'out/web/lan-pair')));
const MODE = opt('mode', 'circuit'), RACES = +opt('races', 1), PLAYERS = Math.min(6, Math.max(2, +opt('players', 2)));
const TMO = +opt('timeout', 1200 * RACES) * 1000;
const STAGGER = +opt('stagger', 3) * 1000;
const BOUNCE = argv.includes('--bounce') ? +opt('bounce', 0) * 1000 : 0;
const DROP = opt('drop', '') ? opt('drop', '').split(':').map(Number) : null;   // [instance (1-based), seconds]
const PAGE = opt('page', 'hostcheck'), SIZE = opt('size', '960x540').split('x').map(Number), PLAY_ONLY = +opt('play-only', 0);
const PY = fs.existsSync(path.join(REPO, '.venv/bin/python')) ? path.join(REPO, '.venv/bin/python') : 'python3';
fs.mkdirSync(OUT, { recursive: true });

// lan_race.sh's scripts
const MODES = LAN.modes;
if (!MODES[MODE]) { console.error('--mode circuit|sprint|drag|streetx|drift'); process.exit(2); }
const NAMES = ['HOSTWEB', 'JOINWEB', 'JOINWEB3', 'JOINWEB4', 'JOINWEB5', 'JOINWEB6'];
const gen = (role, name, car, extra) => execFileSync(PY, [path.join(REPO, LAN.gen), role, '--name', name, '--car', String(car),
  '--races', String(RACES), ...extra], { encoding: 'utf8' });
const scripts = [gen('host', NAMES[0], 0, [...MODES[MODE].h, ...opts('host-arg')])];
for (let i = 1; i < PLAYERS; i++) scripts.push(gen('join', NAMES[i], i, [...MODES[MODE].j, ...opts('join-arg')]));
fs.writeFileSync(path.join(OUT, 'host.txt'), scripts[0]);
fs.writeFileSync(path.join(OUT, 'join.txt'), scripts[1]);

// the suite's environment (web/tools/run-suite.mjs, tests/net/vlan_run.sh)
const COMMON = {
  NFSU2_SAVEDIR: '/save', NFSU2_READONLY_DATA: '1', NFSU2_DISCORD: '0', NOFOCUS: '2', NFSU2_FNG_LOG: '1',
  NFSU2_STATUS_EVERY: '600', NOMOVIES: '1', SNDSYNC: '0', GPUVS: '1', NFSU2_CAR_TRACE: '/save/cars.csv',
};
for (const kv of opts('env')) COMMON[kv.slice(0, kv.indexOf('='))] = kv.slice(kv.indexOf('=') + 1);
const stamp = Date.now().toString(36);
// The generated scripts are answered by this tool (Playwright's request
// interception in start()) at a path of the page's own origin: the server's
// Content-Security-Policy (connect-src 'self') refuses a data: URL, and the
// server only serves the repository's own tests/.
const scriptPath = i => `/tests/lan-pair/${stamp}-i${i + 1}.txt`;
function url(i) {
  const env = { ...COMMON };
  for (const kv of opts(i === 0 ? 'host-env' : 'join-env')) env[kv.slice(0, kv.indexOf('='))] = kv.slice(kv.indexOf('=') + 1);
  const q = new URLSearchParams({ wasm: '/game.wasm', pages: '16384', arg: G.exe,
    reg: G.reg, save: `lan-${stamp}-i${i + 1}`, net: NAMES[i],
    script: scriptPath(i) });
  if (PAGE === 'app') {
    // the player's page: no wasm/pages/arg/net (the app's own), the switch name from roam_name, free roam off
    const a = new URLSearchParams({ reg: q.get('reg'), save: q.get('save'), script: q.get('script') });
    for (const [k, v] of Object.entries({ ...env, NFSU2_ROAM: '0', NFSU2_ROAM_NAME: NAMES[i] })) if (k !== 'NFSU2_SAVEDIR') a.append('env', `${k}=${v}`);
    return `${BASE}/?${a}&fresh`;
  }
  const page = PAGE === 'play' && (!PLAY_ONLY || PLAY_ONLY === i + 1) ? 'play' : 'hostcheck';
  if (page === 'play'){ q.set('w', String(SIZE[0])); q.set('h', String(SIZE[1])); q.set('artifacts', `/artifacts/i${i + 1}-`); q.set('dumpscale', '1'); }
  for (const [k, v] of Object.entries(env)) q.append('env', `${k}=${v}`);
  return `${BASE}/test/${page}.html?${q}&fresh`;
}

// our own server (stopped by its child handle only)
let server = null;
const serverLog = fs.createWriteStream(path.join(OUT, 'server.log'), { flags: 'a' });
async function startServer() {
  const c = spawn(...serverCommand(['--data', DATA,
    '--http-port', String(HTTP_PORT), '--dev', '--artifacts', OUT, ...(opt('impair', '') ? ['--lan-impair', opt('impair', '')] : [])]), { stdio: ['ignore', 'pipe', 'pipe'] });
  c.stdout.pipe(serverLog, { end: false }); c.stderr.pipe(serverLog, { end: false });
  await new Promise((res, rej) => {
    let out = '';
    const t = setTimeout(() => rej(new Error('server did not start')), 20000);
    c.stdout.on('data', d => { out += d; if (out.includes('this Mac') || out.includes('this host')) { clearTimeout(t); res(); } });
    c.on('exit', code => { clearTimeout(t); rej(new Error(`server exited (${code})`)); });
  });
  serverLog.write(`--- server pid ${c.pid} up at ${new Date().toISOString()}\n`);
  return c;
}
async function stopServer() {
  if (!server) return;
  const c = server; server = null;
  const gone = new Promise(r => c.once('exit', r));
  c.kill('SIGTERM'); await gone;
  serverLog.write(`--- server stopped at ${new Date().toISOString()}\n`);
}
if (OWN) server = await startServer();

const browser = await chromium.launch({ channel: 'chrome', headless: !argv.includes('--headed'),
  args: ['--enable-unsafe-webgpu', ...(process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ') : [])] });
const t0 = Date.now();
const inst = [];
async function start(i) {
  const dir = path.join(OUT, `i${i + 1}`); fs.mkdirSync(dir, { recursive: true });
  const ctx = await browser.newContext();
  const page = await ctx.newPage();
  await page.route(`**${scriptPath(i)}`, r => r.fulfill({ status: 200, contentType: 'text/plain; charset=utf-8', body: scripts[i] }));
  const plog = fs.createWriteStream(path.join(dir, 'page.log'));
  const lines = [];
  const r = { dropAt: null, dropped: 0 };
  let exit = null;
  const w = (s) => plog.write(`${((Date.now() - t0) / 1000).toFixed(3)} ${s}\n`);
  page.on('console', m => {
    const t = m.text(); w(t);
    const g = /^\[\d+!?\] ([^]*)$/.exec(t);
    if (g) lines.push(g[1]); else { const e = /^EXIT (-?\d+)$/.exec(t); if (e) exit = +e[1]; lines.push(t); }
    if (DROP && DROP[0] === i + 1 && !r.dropAt && /network race begins/.test(t)) {
      r.dropAt = setTimeout(() => { r.dropped = (Date.now() - t0) / 1000; console.log(`lan-pair: ${NAMES[i]} closes its page at ${r.dropped.toFixed(1)} s (--drop)`);
        void ctx.close().catch(() => {}); }, DROP[1] * 1000);
    }
    if (PAGE === 'app') { const x = /guest exited with code (-?\d+)|\[app\] the game exited with code (-?\d+)/.exec(t); if (x) exit = +(x[1] ?? x[2]); }
  });
  page.on('pageerror', e => { w('PAGEERROR ' + e); lines.push(`PAGEERROR ${e}`); });
  page.on('crash', () => lines.push('CRASH (renderer process)'));
  const u = url(i);
  w(`URL ${u}`);
  await page.goto(u);
  if (PAGE === 'app') {
    await page.waitForFunction(() => window.__nfsu2?.phase === 'ready', null, { timeout: 120000 });
    await page.click('#play');
    w('[lan-pair] clicked Play');
  }
  const timer = setInterval(async () => {
    try { const s = await page.evaluate(() => { const n = globalThis.nfsu2Net; return n ? { ...n.stats(), peers: n.peers } : null; }); if (s) w(`NETSTAT ${JSON.stringify(s)}`); } catch {}
  }, 10000);
  Object.assign(r, { i, name: NAMES[i], dir, page, ctx, plog, lines, timer, exitCode: () => exit, done: null });
  r.done = page.waitForFunction(() => document.title === 'DONE' || window.__nfsu2?.phase === 'exited', null, { timeout: TMO, polling: 1000 })
    .then(() => 'done', () => 'timeout');
  inst.push(r);
  console.log(`lan-pair: ${r.name} started at ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  return r;
}
await start(0);
let bounced = null;
if (BOUNCE && OWN) bounced = new Promise(r => setTimeout(async () => {
  console.log(`lan-pair: stopping the server at ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  await stopServer();
  await new Promise(q => setTimeout(q, 3000));
  server = await startServer();
  console.log(`lan-pair: server back at ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  r();
}, BOUNCE));
for (let i = 1; i < PLAYERS; i++) { await new Promise(r => setTimeout(r, STAGGER)); await start(i); }
const res = await Promise.all(inst.map(x => x.done));
console.log(`lan-pair: ${inst.map((x, k) => `${x.name} ${res[k]}`).join(', ')} after ${((Date.now() - t0) / 1000).toFixed(0)} s`);
for (const x of inst) {
  clearInterval(x.timer);
  if (res[x.i] === 'timeout') x.lines.push(`TIME LIMIT REACHED: lan-pair stopped the run after ${(TMO / 1000).toFixed(0)} s`);
  fs.writeFileSync(path.join(x.dir, 'log.txt'), x.lines.join('\n') + '\n');
  if (x.dropped) { x.plog.end(); continue; }
  try {
    const txt = await x.page.evaluate(async (dir) => {
      const root = await navigator.storage.getDirectory();
      const d = await root.getDirectoryHandle(dir);
      return await (await (await d.getFileHandle('cars.csv')).getFile()).text();
    }, `lan-${stamp}-i${x.i + 1}`);
    fs.writeFileSync(path.join(x.dir, 'cars.csv'), txt);
  } catch (e) { console.log(`lan-pair: no car trace from ${x.name}: ${e}`); }
  x.plog.end();
}
await browser.close();
if (bounced) await bounced;
await stopServer();
serverLog.end();

// judge as tests/net/lan_race.sh
const summary = { mode: MODE, races: RACES, players: PLAYERS, result: {}, why: [] };
for (const x of inst) {
  const log = fs.readFileSync(path.join(x.dir, 'log.txt'), 'utf8'), has = (re) => re.test(log), n = `i${x.i + 1}`;
  const why = [];
  if (x.dropped) {                                     // --drop: only that it ran into the race and failed cleanly
    if (has(/FAULT IN GUEST|GUEST MEMORY FAULT|HOST SEGV/)) why.push(`${n}:fault`);
    summary.result[x.name] = { dropped: `closed its page at ${x.dropped.toFixed(1)} s`, why };
    summary.why.push(...why);
    continue;
  }
  if (res[x.i] !== 'done') why.push(`${n}:timeout`);
  if (x.exitCode() !== 0) why.push(`${n}:exit=${x.exitCode()}`);
  if (!log.includes('quit at guest frame')) why.push(`${n}:no-quit`);
  if (!log.includes(LAN.results)) why.push(`${n}:no-results`);
  if (has(/FAULT IN GUEST|GUEST MEMORY FAULT|HOST SEGV/)) why.push(`${n}:fault`);
  if (log.includes('TIME LIMIT REACHED')) why.push(`${n}:timeout`);
  if (has(/^\[trap\]/m) || has(/^TRAP in thread/m)) why.push(`${n}:trap`);
  if (log.includes('UNRESOLVED INDIRECT')) why.push(`${n}:unresolved-icall`);
  if (has(/^\[net\] BLOCKED/m)) why.push(`${n}:net-blocked`);
  if (has(/^PAGEERROR|^CRASH/m)) why.push(`${n}:page-error`);
  if (RACES > 1 && !has(new RegExp(`LOG results_r${RACES}$`, 'm'))) why.push(`${n}:no-results-r${RACES}`);
  const L = log.split('\n'), grab = (re) => L.filter(l => re.test(l));
  summary.result[x.name] = {
    exit: x.exitCode(), quit: grab(/quit at guest frame/).pop() ?? null,
    net: grab(/^\[net\] (virtual LAN|mode vlan|WSAStartup)/).slice(0, 6),
    lanstats: grab(/^\[net\] virtual LAN (stats|at exit)/).slice(-2),
    network: grab(/network \(vlan\)/).pop() ?? null,
    hfr: grab(/^\[hfr\] network race/).slice(0, 4),
    status: grab(/^\[status\]/).slice(-3),
    lobby: grab(/^\[guest\] .*(auth|Server ready|login)/).slice(0, 8),
    page: grab(/^NET \{/).pop() ?? null,
    why,
  };
  summary.why.push(...why);
}
let trace = '';
try { trace = execFileSync(PY, [path.join(REPO, 'tools/lan_trace.py'), OUT], { encoding: 'utf8' }); }
catch (e) { trace = `lan_trace.py failed: ${e.stdout ?? ''}${e.stderr ?? e}`; }
fs.writeFileSync(path.join(OUT, 'trace.txt'), trace);
console.log(trace);
const near = [...trace.matchAll(/within \d+ m of the viewer: (\d+) samples, err median \S+ m, p95 (\S+) m; jumps (\d+)/g)];
const pairs = PLAYERS * (PLAYERS - 1);
const nearOk = near.length === pairs && near.every(([, n, p, j]) => +n >= 500 && +p < 1.5 && +j === 0);
if (!nearOk && !DROP) summary.why.push('trace');
summary.pass = summary.why.length === 0;
fs.writeFileSync(path.join(OUT, 'summary.json'), JSON.stringify(summary, null, 2));
console.log(JSON.stringify(summary, null, 2));
console.log(summary.pass ? `PASS lan-pair ${MODE} (${OUT})` : `FAIL lan-pair ${MODE}: ${summary.why.join(' ')} (${OUT})`);
process.exitCode = summary.pass ? 0 : 1;
