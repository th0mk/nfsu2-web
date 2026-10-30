// The perf track's race benchmark: a scripted Quick Race (tests/scripts/hfr_drive.txt: Resort Loop, ~50 s of
// driving timed in game-world ms, so the same driving happens at any frame rate) on /test/play.html with the
// WebGPU renderer attached, in Chrome, uncapped and/or paced at 120 fps, repeated, with the machine load noted.
//
//   node web/tools/bench.mjs [options]
//     --base URL        the local server (default http://localhost:8625)
//     --fps LIST        comma list of NFSU2_FPS values: uncapped, 120, 60, auto (default uncapped,120)
//     --runs N          runs per mode (default 2), modes interleaved
//     --headed          a real window (rAF follows the display: 120 Hz on the ProMotion panel)
//     --size WxH        canvas size in device pixels (default 1800x1098)
//     --script PATH     input script (default /tests/web/scripts/perf_drive.txt = hfr_drive.txt with "log cpu_begin" at
//                       15 s and "log cpu_end" at 45 s of race time; needs "log race_loaded"/"log drive_end")
//                       (perf_nodump.txt: the same without the frame dumps, for counting late frames)
//     --env K=V         extra game environment (repeatable)
//     --js-flags S      V8 flags for Chrome (experiments only: a player's Chrome has none)
//     --chrome-arg A    extra Chrome argument (repeatable)
//     --profile DIR     persistent Chrome profile (HTTP cache, wasm code cache, localStorage survive runs)
//     --warmup N        N untimed runs first (with --profile: fills the wasm code cache)
//     --cpuprofile      also sample the game workers with the DevTools CPU profiler during the race window
//                       (Profiler domain on each worker target; wasm names from the name section)
//     --prof-from S     start the profile S seconds after race_loaded (default 0)
//     --interval US     profiler sampling interval in microseconds (default 250)
//     --trace S         record a Chrome trace of every process for S seconds from --prof-from (Tracing domain:
//                       the GPU process's Dawn and compositor work, the page's tasks); <mode>-<i>.trace.json and a
//                       self-time summary per thread, -trace.txt
//     --trace-cats LIST the trace's categories (default: toplevel, gpu, the Dawn and compositor ones, v8)
//     --cpu-from S      take the first per-thread CPU snapshot S seconds after race_loaded (default 0), so the
//                       instructions/cycles per frame cover the warm part only
//     --wasm LIST       module(s) to compare: name=/url,name=/url (default game=/game.wasm); every run of every
//                       mode is made once per module, interleaved, so load drifts hit all of them alike.
//                       A variant may add page parameters after a '+': name=/url+direct=0&k=v, and may name
//                       another server: name=http://host:port/url (a page A/B against an older build's server)
//     --run NAME        output directory out/web/bench/NAME (default bench-<date>-<time>)
//     --timeout S       per-run limit (default 400)
//     --game ID         ug2 (default) or mw: the game's race script and module; Most Wanted has none yet
//     --help            this text and the game's race
// Comparing builds' work: an uncapped race is not the same race twice (camera, HUD and AI timers follow real
// time), and run to run it differs by several percent.  This plays the same race every time, and the game
// thread's P-core cycles per frame (the cpu_begin..cpu_end window) agree within ~1-2 % between runs:
//   --fps 120 --script /tests/web/scripts/perf_short.txt --env NFSU2_FIXED_DT=120 --env NFSU2_HFR_LOCKSTEP=1
//   --env VCLOCK=fixed --env NFSU2_PACE_SPIN=0 --runs 3 --wasm a=http://localhost:A/game.wasm,b=http://localhost:B/game.wasm
// (NFSU2_PACE_SPIN=0: the pacer sleeps without spinning, so the counters see the work alone; the guest busy
// times of the race's first ~1,700 frames compare well too, later the runs drift apart.)
// Every run writes <mode>-<i>.log (the console), .json (the parsed numbers) and, with --cpuprofile,
// .cpuprofile + -prof.txt (and -page.cpuprofile, the page's main thread); summary.md/summary.json hold the table.
//
// Numbers, per run:
//   CPU time per racing frame (macOS and Windows) of the game thread, the page's main thread (the WebGPU
//   encoder), the GPU process and the whole renderer process, from per-thread CPU snapshots at cpu_begin and
//   cpu_end; on Windows the game thread's includes the frame pacer's spin (win32_ext.c sleep_precise_ns);
//   guest busy ms per racing frame (runtime/web/gcpu.c NFSU2_GCPU_FRAMES=1: lock held minus host waits), for the whole
//   race, its first 15 s (V8 still tiering up) and the rest ("warm"), p50/p95/p99/max;
//   [hfr] race fps, missed 120 Hz slots and slots/frame (paced modes; the intro NIS runs at 30 by design);
//   present intervals > 1.5 periods in the warm drive (paced modes);
//   renderer encode ms per frame on the page thread and GPU ms (timestamp queries) in the race window;
//   machine load: the 1-minute load average (macOS) or the busy cores (elsewhere) sampled every 5 s during the
//   run (min/mean/max), plus uptime before/after.
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { execFileSync, spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dist, pct } from './stats.mjs';
import WebSocket from 'ws';
import { GAMES, gameOf, usage, nothingFor, requireGame } from './rig-game.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const argv = process.argv.slice(2);
// each game's race: the default script and the module's json (its revision in the summary)
const RACE_OF = { ug2: { script: '/tests/web/scripts/perf_drive.txt', json: 'out/web/nfsu2.json' } };
const NONE = 'none yet: no Most Wanted bench scenes (M5-26)';
const GAME = gameOf(argv), G = GAMES[GAME], RACE = RACE_OF[GAME];
if (argv.includes('--help')) usage(import.meta.url, GAME, 'The race', RACE ? [`--script default: ${RACE.script}`, `revision from ${RACE.json}`] : [], NONE);
if (!RACE) nothingFor(GAME, NONE);
const o = { base: 'http://localhost:8625', fps: 'uncapped,120', runs: 2, headed: false, size: '1800x1098',
  script: RACE.script, env: [], jsFlags: '', chromeArgs: [], profile: '', warmup: 0, cpuprofile: false,
  profFrom: 0, cpuFrom: 0, interval: 250, run: '', timeout: 400, wasm: 'game=/game.wasm', trace: 0,
  traceCats: 'toplevel,gpu,gpu.dawn,disabled-by-default-gpu.dawn,gpu.service,viz,cc,benchmark,disabled-by-default-gpu.decoder,v8' };
for (let i = 0; i < argv.length; i++) {
  const a = argv[i], v = () => argv[++i];
  if (a === '--base') o.base = v(); else if (a === '--fps') o.fps = v(); else if (a === '--runs') o.runs = +v();
  else if (a === '--headed') o.headed = true; else if (a === '--size') o.size = v(); else if (a === '--script') o.script = v();
  else if (a === '--env') o.env.push(v()); else if (a === '--js-flags') o.jsFlags = v(); else if (a === '--chrome-arg') o.chromeArgs.push(v());
  else if (a === '--profile') o.profile = path.resolve(v()); else if (a === '--warmup') o.warmup = +v();
  else if (a === '--cpuprofile') o.cpuprofile = true; else if (a === '--prof-from') o.profFrom = +v();
  else if (a === '--interval') o.interval = +v(); else if (a === '--cpu-from') o.cpuFrom = +v(); else if (a === '--wasm') o.wasm = v(); else if (a === '--run') o.run = v(); else if (a === '--timeout') o.timeout = +v();
  else if (a === '--trace') o.trace = +v(); else if (a === '--trace-cats') o.traceCats = v();
  else if (a === '--game') v();                    // gameOf() above
  else { console.error(`unknown option ${a}`); process.exit(2); }
}
await requireGame(o.base, GAME);
const stamp = new Date().toISOString().replace(/[-:]/g, '').replace('T', '-').slice(0, 15);
const OUT = path.join(ROOT, 'out/web/bench', o.run || `bench-${stamp}`);
fs.mkdirSync(OUT, { recursive: true });
const [W, H] = o.size.split('x').map(Number);
const modes = o.fps.split(',').filter(Boolean);
const rev = (() => { try { return JSON.parse(fs.readFileSync(path.join(ROOT, RACE.json), 'utf8')).rev; } catch { return '?'; } })();

const SUITE_ENV = { NOMOVIES: '1', SNDSYNC: '0', NFSU2_FNG_LOG: '1', NFSU2_STATUS_EVERY: '600', NFSU2_DISCORD: '0',
  NFSU2_READONLY_DATA: '1', NFSU2_SAVEDIR: '/save', NOFOCUS: '2', GPUVS: '1', NFSU2_GCPU_FRAMES: '1' };
let saveN = 0;
const variants = o.wasm.split(',').filter(Boolean).map(x => { const i = x.indexOf('='); return i > 0 ? { name: x.slice(0, i), url: x.slice(i + 1) } : { name: path.basename(x, '.wasm'), url: x }; });
function urlFor(fps, spec) {
  const q = new URLSearchParams();
  let [wasm, extra] = spec.split('+'), base = o.base;
  if (/^https?:/.test(wasm)) { const u = new URL(wasm); base = u.origin; wasm = u.pathname; }
  q.set('wasm', wasm);
  for (const [k, v] of new URLSearchParams(extra ?? '')) if (k === 'env') q.append(k, v); else q.set(k, v);
  q.append('arg', G.exe); q.set('w', String(W)); q.set('h', String(H));
  q.set('gputime', ''); q.set('bench', '');
  q.set('save', `perf-bench-${process.pid}-${saveN++}`); q.set('fresh', '');
  q.set('reg', G.reg); q.set('script', o.script);
  const env = { ...SUITE_ENV, NFSU2_FPS: fps, ...Object.fromEntries(o.env.map(e => [e.slice(0, e.indexOf('=')), e.slice(e.indexOf('=') + 1)])) };
  for (const [k, v] of Object.entries(env)) q.append('env', `${k}=${v}`);
  return `${base}/test/play.html?${q}`;
}

// the machine's load: the 1-minute load average on macOS; elsewhere the busy cores since the last sample
// (os.cpus() times: Windows has no load average), this run's own Chrome included in both
let cpusLast = null;
const load1 = () => {
  if (process.platform === 'darwin') { try { return +execFileSync('sysctl', ['-n', 'vm.loadavg'], { encoding: 'utf8' }).replace(/[{}]/g, '').trim().split(/\s+/)[0]; } catch { return NaN; } }
  const c = os.cpus().map(x => [x.times.user + x.times.nice + x.times.sys + x.times.irq, x.times.idle]), z = cpusLast;
  cpusLast = c;
  if (!z || z.length !== c.length) return NaN;
  let busy = 0, all = 0;
  c.forEach(([b, i], k) => { busy += b - z[k][0]; all += b + i - z[k][0] - z[k][1]; });
  return all > 0 ? busy / all * c.length : NaN;
};
const uptime = () => { try { return execFileSync('uptime', { encoding: 'utf8' }).trim(); } catch { return ''; } };

// ---- DevTools CPU profiler on the page's dedicated workers (flat CDP sessions over a raw socket) ----
class Cdp {
  constructor(ws) { this.ws = ws; this.id = 0; this.wait = new Map(); this.handlers = [];
    ws.on('message', d => { const m = JSON.parse(d); if (m.id && this.wait.has(m.id)) { const w = this.wait.get(m.id); this.wait.delete(m.id); m.error ? w.rej(new Error(JSON.stringify(m.error))) : w.res(m.result); } else for (const h of this.handlers) h(m); }); }
  send(method, params = {}, sessionId) { const id = ++this.id; this.ws.send(JSON.stringify({ id, method, params, sessionId })); return new Promise((res, rej) => this.wait.set(id, { res, rej })); }
  static async connect(port) {
    for (let i = 0; i < 50; i++) {
      try { const j = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json(); const ws = new WebSocket(j.webSocketDebuggerUrl, { perMessageDeflate: false, maxPayload: 1 << 30 }); await new Promise((r, x) => { ws.once('open', r); ws.once('error', x); }); return new Cdp(ws); }
      catch { await new Promise(r => setTimeout(r, 200)); }
    }
    throw new Error('no DevTools endpoint');
  }
}
async function profilerFor(port) {
  const cdp = await Cdp.connect(port);
  const workers = new Map();       // sessionId -> { title, url }
  cdp.handlers.push(m => {
    if (m.method === 'Target.attachedToTarget') {
      const t = m.params.targetInfo;
      if (t.type === 'worker') workers.set(m.params.sessionId, { title: t.title, url: t.url });
      cdp.send('Runtime.runIfWaitingForDebugger', {}, m.params.sessionId).catch(() => {});
    }
  });
  const { targetInfos } = await cdp.send('Target.getTargets');
  const page = targetInfos.find(t => t.type === 'page' && t.url.includes('/test/play.html'));
  const { sessionId } = await cdp.send('Target.attachToTarget', { targetId: page.targetId, flatten: true });
  await cdp.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, sessionId);
  workers.set(sessionId, { title: 'page', url: page.url });     // the page's main thread: the renderer's encoder
  return {
    async start() {
      for (const s of workers.keys()) {
        await cdp.send('Profiler.enable', {}, s); await cdp.send('Profiler.setSamplingInterval', { interval: o.interval }, s);
        await cdp.send('Profiler.start', {}, s);
      }
      return workers.size;
    },
    async stop() {
      const res = [];
      for (const [s, w] of workers) { try { const { profile } = await cdp.send('Profiler.stop', {}, s); res.push({ ...w, profile }); } catch { /* gone */ } }
      cdp.ws.close();
      return res;
    },
  };
}
// Samples whose leaf is one of these are a blocked or idle thread, not work: V8's sampler keeps sampling a thread
// parked in Atomics.wait (the pool's idle loop in onmessage, futex waits, the guest lock's ticket wait, the
// host's sleep/RPC wait), so they are counted apart.
const WAITS = new Set(['(idle)', '(program)', '(garbage collector)', '(root)', 'onmessage', '__wasilibc_futex_wait',
  'gil_acquire', 'q', 'poll_oneoff', 'wait32', 'tramp_main', 'input_main']);
function analyse(prof, interval) {
  const nodes = new Map(prof.nodes.map(n => [n.id, n])), parent = new Map();
  for (const n of prof.nodes) for (const c of n.children ?? []) parent.set(c, n.id);
  const name = n => (n.callFrame.functionName || '(anonymous)').replace(/^\$/, '');
  const self = new Map(), incl = new Map(), waits = new Map();
  let work = 0, total = 0, lifted = 0;
  const dts = prof.timeDeltas ?? [];
  for (let i = 0; i < prof.samples.length; i++) {
    const n = nodes.get(prof.samples[i]); total++;
    const nm = name(n);
    if (WAITS.has(nm)) { waits.set(nm, (waits.get(nm) ?? 0) + 1); continue; }
    work++; if (nm.startsWith('sub_')) lifted++;
    self.set(nm, (self.get(nm) ?? 0) + 1);
    const seen = new Set();
    for (let id = n.id; id !== undefined; id = parent.get(id)) { const k = name(nodes.get(id)); if (!seen.has(k)) { seen.add(k); incl.set(k, (incl.get(k) ?? 0) + 1); } }
  }
  const span = (prof.endTime - prof.startTime) / 1e3;
  const top = (m, k) => [...m.entries()].sort((a, b) => b[1] - a[1]).slice(0, k).map(([f, c]) => ({ f, n: c, share: c / Math.max(1, work) }));
  return { samples: total, busy: work, lifted, waits: Object.fromEntries(waits), spanMs: span,
    avgIntervalUs: dts.length ? dts.reduce((a, b) => a + b, 0) / dts.length : interval, self: top(self, 80), incl: top(incl, 80) };
}

// ---- Chrome trace of every process (Tracing domain on the browser target) ----
async function tracerFor(port) {
  const cdp = await Cdp.connect(port);
  const events = [];
  let done; const complete = new Promise(r => { done = r; });
  cdp.handlers.push(m => { if (m.method === 'Tracing.dataCollected') for (const e of m.params.value) events.push(e); else if (m.method === 'Tracing.tracingComplete') done(); });
  return {
    start: () => cdp.send('Tracing.start', { transferMode: 'ReportEvents', traceConfig: { recordMode: 'recordContinuously',
      includedCategories: o.traceCats.split(','), enableSampling: false } }),
    async stop() { await cdp.send('Tracing.end'); await complete; cdp.ws.close(); return events; },
  };
}
// self time per thread and event name: complete (X) and begin/end (B/E) slices nested by a stack per thread
function traceSummary(events, frames) {
  const tname = new Map(), pname = new Map(), per = new Map();
  for (const e of events) {
    if (e.ph !== 'M') continue;
    if (e.name === 'thread_name') tname.set(`${e.pid}:${e.tid}`, e.args.name);
    if (e.name === 'process_name') pname.set(e.pid, e.args.name);
  }
  const byThread = new Map();
  for (const e of events) if (e.ph === 'X' || e.ph === 'B' || e.ph === 'E') {
    const k = `${e.pid}:${e.tid}`; if (!byThread.has(k)) byThread.set(k, []); byThread.get(k).push(e);
  }
  let t0 = Infinity, t1 = -Infinity;
  for (const [k, evs] of byThread) {
    evs.sort((a, b) => a.ts - b.ts || (a.ph === 'E' ? -1 : 0) - (b.ph === 'E' ? -1 : 0) || (b.dur ?? 0) - (a.dur ?? 0));
    const self = new Map(), stack = [];
    // a slice ends at ts + dur (X) or at its E; an X slice is closed when a later event starts after its end
    const close = (until) => { while (stack.length && stack[stack.length - 1].end !== undefined && stack[stack.length - 1].end <= until) {
      const s = stack.pop(); self.set(s.name, (self.get(s.name) ?? 0) + (s.end - s.start) - s.kids); if (stack.length) stack[stack.length - 1].kids += s.end - s.start; } };
    for (const e of evs) {
      t0 = Math.min(t0, e.ts); t1 = Math.max(t1, e.ts + (e.dur ?? 0));
      close(e.ts);
      if (e.ph === 'X') stack.push({ name: e.name, start: e.ts, end: e.ts + (e.dur ?? 0), kids: 0 });
      else if (e.ph === 'B') stack.push({ name: e.name, start: e.ts, end: undefined, kids: 0 });
      else { for (let i = stack.length - 1; i >= 0; i--) if (stack[i].end === undefined) { stack[i].end = e.ts; break; } close(e.ts); }
    }
    close(Infinity);
    const tot = [...self.values()].reduce((a, b) => a + b, 0);
    per.set(k, { name: `${pname.get(+k.split(':')[0]) ?? '?'}/${tname.get(k) ?? k}`, tot, self });
  }
  const span = (t1 - t0) / 1e3, out = [`trace: ${events.length} events over ${(span / 1000).toFixed(2)} s, ${frames} frames`];
  for (const t of [...per.values()].sort((a, b) => b.tot - a.tot).slice(0, 14)) {
    out.push('', `${t.name}: ${(t.tot / 1e3).toFixed(0)} ms in slices (${(t.tot / 1e3 / (frames || 1)).toFixed(3)} ms/frame)`);
    for (const [n, us] of [...t.self].sort((a, b) => b[1] - a[1]).slice(0, 16)) out.push(`  ${(us / 1e3 / (frames || 1)).toFixed(3).padStart(8)} ms/frame  ${(100 * us / t.tot).toFixed(1).padStart(5)} %  ${n}`);
  }
  return out.join('\n') + '\n';
}

// ---- per-thread CPU time of this run's Chrome processes ----
// macOS: web/tools/threadcpu.c (built on first use; Mach thread info and counters).  Windows:
// web/tools/threadcpu-win.ps1, one PowerShell server for the whole bench (thread times, cycle counts,
// Chrome's thread names; no instruction counts).  Elsewhere the CPU columns stay empty and the guest
// busy times still count.
const MAC = process.platform === 'darwin', WIN = process.platform === 'win32';
const TMP = MAC ? '/private/tmp/nsfu2-web-perf' : path.join(os.tmpdir(), 'nsfu2-web-perf');
fs.mkdirSync(TMP, { recursive: true });
const TCPU = path.join(TMP, 'threadcpu/threadcpu');
if (MAC && !fs.existsSync(TCPU)) { fs.mkdirSync(path.dirname(TCPU), { recursive: true }); execFileSync('clang', ['-O2', '-o', TCPU, path.join(ROOT, 'web/tools/threadcpu.c')]); }
let winCpu = null;
function winSnap(dir) {
  if (!winCpu) {
    // a copy on the local disk: PowerShell refuses unsigned scripts from a network path (\\wsl.localhost)
    const ps1 = path.join(TMP, 'threadcpu-win.ps1');
    fs.copyFileSync(path.join(ROOT, 'web/tools/threadcpu-win.ps1'), ps1);
    const proc = spawn('pwsh', ['-NoProfile', '-NonInteractive', '-File', ps1], { stdio: ['pipe', 'pipe', 'inherit'] });
    winCpu = { proc, waiters: [], lines: [], rest: '' };
    proc.stdout.setEncoding('utf8');
    proc.stdout.on('data', d => {
      const parts = (winCpu.rest + d).split('\n'); winCpu.rest = parts.pop();
      for (const l of parts.map(s => s.trim())) {
        if (!l.startsWith('END ')) { if (l) winCpu.lines.push(l); continue; }
        const lines = winCpu.lines; winCpu.lines = [];
        winCpu.waiters.shift()?.({ lines, t: +l.slice(4) });
      }
    });
    proc.on('error', e => { console.error(`threadcpu-win: ${e}`); for (const w of winCpu.waiters.splice(0)) w(null); });
    proc.unref(); proc.stdout.unref?.();
  }
  return new Promise(res => { winCpu.waiters.push(res); winCpu.proc.stdin.write(`snap ${dir}\n`); });
}
/** every thread of the run's Chrome processes: { m: Map, t: epoch ms of the snapshot }, or null */
async function threadCpu(dir) {
  if (WIN) {
    const s = await winSnap(dir);
    if (!s || !s.lines.length) return null;
    const m = new Map();
    for (const l of s.lines) {
      const [pid, id, ns, name, type, cns] = l.split(' ');
      // the cycle count is exact; the thread times only advance on clock ticks
      m.set(`${pid}:${id}`, { pid: +pid, type, name, ns: +cns || +ns, pi: NaN, pc: NaN, pn: NaN, ei: NaN, ec: NaN, en: NaN });
    }
    return { m, t: s.t };
  }
  if (!MAC) return null;
  const m = threadCpuMac(dir);
  return m ? { m, t: Date.now() } : null;
}
function threadCpuMac(dir) {
  const ps = execFileSync('ps', ['-axo', 'pid=,ppid=,command='], { encoding: 'utf8', maxBuffer: 1 << 26 }).split('\n')
    .map(l => /^\s*(\d+)\s+(\d+)\s+(.*)$/.exec(l)).filter(Boolean).map(m => ({ pid: +m[1], ppid: +m[2], cmd: m[3] }));
  const br = ps.find(p => p.cmd.includes(`--user-data-dir=${dir}`) && !p.cmd.includes('--type='));
  if (!br) return null;
  const pids = ps.filter(p => p.ppid === br.pid && /--type=(renderer|gpu-process)/.test(p.cmd)).map(p => ({ pid: p.pid, type: /--type=([\w-]+)/.exec(p.cmd)[1] }));
  const out = execFileSync(TCPU, pids.map(p => String(p.pid)), { encoding: 'utf8' });
  const m = new Map();
  for (const l of out.trim().split('\n')) {
    const [pid, id, ns, name, pi, pc, pn, ei, ec, en] = l.split(' ');
    m.set(`${pid}:${id}`, { pid: +pid, type: pids.find(p => p.pid === +pid)?.type, name, ns: +ns, pi: +pi, pc: +pc, pn: +pn, ei: +ei, ec: +ec, en: +en });
  }
  return m;
}
function cpuDelta(a, b, frames, wallMs) {
  const th = [];
  for (const [k, v] of b) {
    const z = a.get(k) ?? { ns: 0, pi: 0, pc: 0, pn: 0, ei: 0, ec: 0, en: 0 }, ns = v.ns - z.ns;
    if (ns > 0) th.push({ ...v, ms: ns / 1e6, dpi: v.pi - z.pi, dpc: v.pc - z.pc, dpn: v.pn - z.pn, dei: v.ei - z.ei, dec: v.ec - z.ec, den: v.en - z.en });
  }
  th.sort((x, y) => y.ms - x.ms);
  const sum = f => th.filter(f).reduce((s, t) => s + t.ms, 0);
  const main = th.find(t => t.type === 'renderer' && /Worker/i.test(t.name) && !/ThreadPool/.test(t.name)) ?? th[0];
  const per = x => frames ? x / frames : NaN;
  return { frames, wallMs, mainMs: main?.ms, mainName: main?.name, mainPerFrame: frames ? main.ms / frames : NaN, mainUtil: main ? main.ms / wallMs : NaN,
    // instructions retired per frame (P + E cores; less load-dependent than time, not independent of it), P-core cycles per frame,
    // the share of the thread's CPU time spent on E cores, and the P-core clock (cycles / P time)
    minsPerFrame: main ? per(main.dpi + main.dei) / 1e6 : NaN, mcycPPerFrame: main ? per(main.dpc) / 1e6 : NaN,
    eShare: main && main.dpn + main.den ? main.den / (main.dpn + main.den) : NaN, pGHz: main && main.dpn ? main.dpc / main.dpn : NaN,
    ipcP: main && main.dpc ? main.dpi / main.dpc : NaN,
    rendererMs: sum(t => t.type === 'renderer'), poolMs: sum(t => t.type === 'renderer' && /ThreadPool/.test(t.name)),
    pageMs: sum(t => t.name === 'CrRendererMain'), gpuProcMs: sum(t => t.type === 'gpu-process'),
    top: th.slice(0, 12).map(t => `${t.type}/${t.name} ${t.ms.toFixed(0)}`) };
}

// ---- one run ----
async function runOnce(fps, label, timed, wasm = variants[0].url) {
  const url = urlFor(fps, wasm);
  const port = 9500 + (process.pid % 400);
  const args = ['--enable-unsafe-webgpu', ...(o.jsFlags ? [`--js-flags=${o.jsFlags}`] : []), ...o.chromeArgs,
    ...((o.cpuprofile || o.trace) && timed ? [`--remote-debugging-port=${port}`] : [])];
  // always a persistent context with a known directory: it finds this run's Chrome processes for the
  // per-thread CPU times (a fresh, deleted directory unless --profile keeps one)
  const dir = o.profile || fs.mkdtempSync(path.join(TMP, 'chprof-'));
  const ctx = await chromium.launchPersistentContext(dir, { ...pickBrowser(), headless: !o.headed, args, viewport: null });
  const page = ctx.pages()[0] ?? await ctx.newPage();
  const browser = { close: async () => { await ctx.close(); if (!o.profile) fs.rmSync(dir, { recursive: true, force: true }); } };
  const lines = [], loads = [];
  let cpu0 = null, cpu1 = null, f0 = 0, f1 = 0, tw0 = 0, tw1 = 0, cpuE = null, twE = 0, mark0 = null, mark1 = null, mt0 = 0, mt1 = 0;
  const up0 = uptime(), t0 = Date.now();
  const lt = setInterval(() => loads.push(load1()), 5000); loads.push(load1());
  let prof = null, profStarted = null, profStop = null, traceDone = null, traceF0 = 0, traceF1 = 0;
  let raceAt = 0;
  const onLine = async t => {
    // the CPU window: cpu_begin .. cpu_end when the script has them (a fixed stretch of race time)
    // (snapshots are asynchronous on Windows: each keeps the time it was taken)
    if (timed && t.includes(' LOG cpu_begin')) { const s = await threadCpu(dir); if (s) { mark0 = s.m; mt0 = s.t; } }
    if (timed && t.includes(' LOG cpu_end') && mark0 && !mark1) { const s = await threadCpu(dir); if (s) { mark1 = s.m; mt1 = s.t; } }
    if (timed && t.includes(' LOG race_loaded')) {
      const f = +(/ f=(\d+) /.exec(t)?.[1] ?? 0);
      if (o.cpuFrom) { const s = await threadCpu(dir); if (s) { cpuE = s.m; twE = s.t; } }      // the early window: race_loaded .. +cpuFrom
      setTimeout(async () => { const s = await threadCpu(dir); if (s) { cpu0 = s.m; tw0 = s.t; f0 = o.cpuFrom ? 0 : f; } }, o.cpuFrom * 1000);
    }
    if (timed && t.includes(' LOG drive_end') && cpu0 && !cpu1) { const f = o.cpuFrom ? 0 : +(/ f=(\d+) /.exec(t)?.[1] ?? 0), s = await threadCpu(dir); if (s) { cpu1 = s.m; tw1 = s.t; f1 = f; } }
    if (o.trace && timed && t.includes(' LOG race_loaded')) setTimeout(async () => {
      try {
        const tr = await tracerFor(port); await tr.start(); traceF0 = Date.now(); lines.push(`[bench] trace started`);
        traceDone = new Promise(r => setTimeout(async () => { traceF1 = Date.now(); r(await tr.stop()); }, o.trace * 1000));
      } catch (e) { lines.push(`[bench] trace failed: ${e}`); }
    }, o.profFrom * 1000);
    if (!o.cpuprofile || !timed) return;
    if (t.includes(' LOG race_loaded')) {
      raceAt = Date.now();
      setTimeout(async () => { try { prof = await profilerFor(port); const n = await prof.start(); profStarted = Date.now(); lines.push(`[bench] profiler started on ${n} workers`); } catch (e) { lines.push(`[bench] profiler failed: ${e}`); } }, o.profFrom * 1000);
    }
    if (t.includes(' LOG drive_end') && prof && !profStop) profStop = prof.stop();
  };
  page.on('console', m => { const t = m.text(); lines.push(t); onLine(t); });
  page.on('pageerror', e => lines.push(`PAGEERROR ${e}`));
  page.on('crash', () => lines.push('CRASH (renderer process)'));
  await page.goto(url);
  let timedOut = false;
  try { await page.waitForFunction(() => document.title === 'DONE', null, { timeout: o.timeout * 1000, polling: 1000 }); }
  catch { timedOut = true; lines.push('[bench] TIMEOUT'); }
  const profiles = profStop ? await profStop : [];
  const traceEvents = traceDone ? await traceDone : null;
  clearInterval(lt); loads.push(load1());
  const wall = (Date.now() - t0) / 1000, up1 = uptime();
  await browser.close();
  const log = lines.join('\n') + '\n';
  if (!timed) { fs.writeFileSync(path.join(OUT, `${label}.log`), log); return null; }
  fs.writeFileSync(path.join(OUT, `${label}.log`), log);
  const r = parse(log);
  // frames in the CPU window: the autotest frame numbers, or (--cpu-from) the racing frames presented between the
  // two snapshots (gcpu.c's epoch-ms Present times; the snapshots take a few ms, which bounds the error)
  if (cpu0 && cpu1 && o.cpuFrom) { const t00 = +(/\[gcpuf0\] ([\d.]+)/.exec(log)?.[1] ?? NaN); f1 = r.frameT.filter(x => t00 + x >= tw0 && t00 + x < tw1).length; }
  if (mark0 && mark1) {
    const t00 = +(/\[gcpuf0\] ([\d.]+)/.exec(log)?.[1] ?? NaN);
    r.cpu = cpuDelta(mark0, mark1, r.frameT.filter(x => t00 + x >= mt0 && t00 + x < mt1).length, mt1 - mt0); r.cpu.window = 'cpu_begin..cpu_end';
  } else if (cpu0 && cpu1) r.cpu = cpuDelta(cpu0, cpu1, f1 - f0, tw1 - tw0);
  if (cpuE && cpu0) { const t00 = +(/\[gcpuf0\] ([\d.]+)/.exec(log)?.[1] ?? NaN);
    r.cpuEarly = cpuDelta(cpuE, cpu0, r.frameT.filter(x => t00 + x >= twE && t00 + x < tw0).length, tw0 - twE); }
  const ld = loads.filter(x => !Number.isNaN(x));
  Object.assign(r, { label, fps, wall, timedOut, load: { min: Math.min(...ld), mean: ld.reduce((a, b) => a + b, 0) / ld.length, max: Math.max(...ld) }, uptimeBefore: up0, uptimeAfter: up1 });
  if (profiles.length) {
    const an = profiles.map(p => ({ title: p.title, ...analyse(p.profile, o.interval) })).sort((a, b) => b.lifted - a.lifted);
    r.profile = { from: o.profFrom, workers: an.map(a => ({ title: a.title, busy: a.busy, samples: a.samples, spanMs: a.spanMs })), main: an[0] };
    fs.writeFileSync(path.join(OUT, `${label}.cpuprofile`), JSON.stringify(profiles.find(p => p.title === an[0].title).profile));
    const pg = profiles.find(p => p.title === 'page');
    if (pg) fs.writeFileSync(path.join(OUT, `${label}-page.cpuprofile`), JSON.stringify(pg.profile));
    const txt = [`main guest worker ${an[0].title}: ${an[0].busy} working of ${an[0].samples} samples over ${(an[0].spanMs / 1000).toFixed(1)} s (interval ${an[0].avgIntervalUs.toFixed(0)} us); waiting: ${JSON.stringify(an[0].waits)}`,
      '', 'self:', ...an[0].self.map(x => `  ${(100 * x.share).toFixed(2).padStart(6)} %  ${x.f}`),
      '', 'inclusive:', ...an[0].incl.map(x => `  ${(100 * x.share).toFixed(2).padStart(6)} %  ${x.f}`),
      '', 'other workers:', ...an.slice(1).filter(a => a.busy).map(a => `  ${a.title}: ${a.busy} busy samples; top self ${a.self.slice(0, 5).map(x => `${x.f} ${(100 * x.share).toFixed(1)}%`).join(', ')}`)];
    fs.writeFileSync(path.join(OUT, `${label}-prof.txt`), txt.join('\n') + '\n');
  }
  if (traceEvents) {
    const t00 = +(/\[gcpuf0\] ([\d.]+)/.exec(log)?.[1] ?? NaN), fr = r.frameT.filter(x => t00 + x >= traceF0 && t00 + x < traceF1).length;
    fs.writeFileSync(path.join(OUT, `${label}.trace.json`), JSON.stringify({ traceEvents }));
    fs.writeFileSync(path.join(OUT, `${label}-trace.txt`), traceSummary(traceEvents, fr));
  }
  fs.writeFileSync(path.join(OUT, `${label}.json`), JSON.stringify(r, null, 1));
  return r;
}

function parse(log) {
  const r = { exit: null, race: null, early: null, warm: null, hfr: null, late: null, renderer: null, gpuFinal: null };
  const ex = /^EXIT (-?\d+)$/m.exec(log); if (ex) r.exit = +ex[1];
  const busy = [], t = [];
  for (const m of log.matchAll(/\[gcpuf\] (\S+)/g)) for (const e of m[1].split(';')) { const [b, tt] = e.split('@'); busy.push(+b / 1000); t.push(+tt); }
  r.race = dist(busy); Object.defineProperty(r, 'frameT', { value: t, enumerable: false });
  const EARLY = 15000;
  r.early = dist(busy.filter((_, i) => t[i] < EARLY)); r.warm = dist(busy.filter((_, i) => t[i] >= EARLY));
  const h = /\[hfr\] race wall=([\d.]+)s fps=([\d.]+) slots\/f=([\d.]+) miss=(\d+).* work=([\d.]+)ms\/f sleep=([\d.]+)ms\/f/.exec(log);
  if (h) r.hfr = { wall: +h[1], fps: +h[2], slotsPerFrame: +h[3], missed: +h[4], workMs: +h[5], sleepMs: +h[6] };
  const rate = /content rate (\d+)/.exec(log); const hz = rate ? +rate[1] : 0;
  if (hz > 0 && t.length) {    // present intervals in the warm drive (after the intro NIS and the first 15 s)
    const per = 1000 / hz; let n = 0, late = 0, lost = 0;
    for (let i = 1; i < t.length; i++) if (t[i - 1] >= EARLY) { const d = t[i] - t[i - 1]; n++; if (d > 1.5 * per) { late++; lost += Math.round(d / per) - 1; } }
    r.late = { intervals: n, late, lostSlots: lost, periodMs: per };
  }
  const b = /\[bench\] renderer (\{.*\})/.exec(log); if (b) r.renderer = JSON.parse(b[1]);
  const g = /\[gpu\] final: (\{.*\})/.exec(log); if (g) { const s = JSON.parse(g[1]); r.gpuFinal = { encodeMsAvg: s.encodeMsAvg, gpuMsAvg: s.gpuMsAvg, errors: s.errors, frames: s.frames }; }
  r.compileMs = +(/compiled in (\d+) ms/.exec(log)?.[1] ?? NaN);
  r.raceLoadedMs = +(/w=([\d.]+)ms LOG race_loaded/.exec(log)?.[1] ?? NaN);
  r.loadMs = +(/\[bench\] race load (\d+) ms/.exec(log)?.[1] ?? NaN);
  return r;
}

const f2 = x => (x === undefined || Number.isNaN(x)) ? '-' : x.toFixed(2);
const row = r => `| ${r.label} | ${r.exit} | ${Number.isNaN(r.loadMs) ? '-' : (r.loadMs / 1000).toFixed(2)} | ${r.cpu ? `${f2(r.cpu.mainPerFrame)} (${(100 * r.cpu.mainUtil).toFixed(0)} %)` : '-'} | ${r.cpu ? `${f2(r.cpu.pageMs / r.cpu.frames)} / ${f2(r.cpu.gpuProcMs / r.cpu.frames)} / ${f2(r.cpu.rendererMs / r.cpu.frames)}` : '-'} | ${r.cpu ? `${f2(r.cpu.minsPerFrame)} / ${(100 * r.cpu.eShare).toFixed(0)} %` : '-'}${r.cpuEarly ? ` (early ${f2(r.cpuEarly.minsPerFrame)})` : ''} | ${f2(r.load.min)}–${f2(r.load.max)} (${f2(r.load.mean)}) | ${r.race.n} | ${f2(r.race.p50)} / ${f2(r.race.p95)} / ${f2(r.race.p99)} / ${f2(r.race.max)} | ${f2(r.early.p50)} / ${f2(r.early.p95)} | ${f2(r.warm.p50)} / ${f2(r.warm.p95)} / ${f2(r.warm.p99)} | ${r.hfr ? `${f2(r.hfr.fps)} / ${r.hfr.missed} / ${r.hfr.slotsPerFrame.toFixed(3)}` : '-'} | ${r.late ? `${r.late.late}/${r.late.intervals}` : '-'} | ${r.renderer ? `${f2(r.renderer.encodeMs[0])} / ${f2(r.renderer.encodeMs[1])}` : '-'} | ${r.renderer?.gpuMs?.length ? `${f2(r.renderer.gpuMs[0])} / ${f2(r.renderer.gpuMs[1])}` : '-'} |`;
const HEAD = ['| run | exit | race load s | game thread CPU ms/frame (CPU/wall) | page thread / GPU process / renderer CPU ms/frame | M instructions/frame / E-core time | load 1-min min–max (mean) | race frames | busy ms p50 / p95 / p99 / max | first 15 s p50 / p95 | warm p50 / p95 / p99 | [hfr] fps / missed slots / slots per frame | warm late frames | encode ms p50 / p95 | GPU ms p50 / p95 |',
  '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|'];

console.log(`bench ${path.relative(ROOT, OUT)}: rev ${rev}, modes ${modes.join(',')}, ${o.runs} run(s) each, ${o.headed ? 'headed' : 'headless'} ${W}x${H}${o.jsFlags ? `, js-flags ${o.jsFlags}` : ''}${o.profile ? `, profile ${o.profile}` : ''}${o.cpuprofile ? ', cpuprofile' : ''}`);
console.log(`machine: ${os.cpus().length} cores, ${uptime()}`);
for (let i = 0; i < o.warmup; i++) for (const v of variants) { console.log(`warm-up ${i + 1}/${o.warmup} ${v.name}`); await runOnce(modes[0], `warmup-${v.name}-${i}`, false, v.url); }
const results = [];
for (let i = 0; i < o.runs; i++) for (const m of modes) for (const v of variants) {
  const r = await runOnce(m, `${variants.length > 1 ? v.name + '-' : ''}${m}-${i}`, true, v.url);
  r.variant = v.name;
  results.push(r);
  console.log(HEAD[0]); console.log(row(r));
}
const agg = [];
for (const v of variants) for (const m of modes) {
  const rs = results.filter(r => r.fps === m && r.variant === v.name), med = a => pct(a.filter(x => !Number.isNaN(x)), 0.5);
  const span = (k1, k2) => { const v = rs.map(r => r[k1][k2]).filter(x => !Number.isNaN(x)); return `${f2(med(v))} (${f2(Math.min(...v))}–${f2(Math.max(...v))})`; };
  const cv = rs.map(r => r.cpu?.mainPerFrame).filter(x => x !== undefined && !Number.isNaN(x));
  const iv = rs.map(r => r.cpu?.minsPerFrame).filter(x => x !== undefined && !Number.isNaN(x));
  const ie = rs.map(r => r.cpuEarly?.minsPerFrame).filter(x => x !== undefined && !Number.isNaN(x));
  agg.push(`| ${variants.length > 1 ? v.name + ' ' : ''}${m} × ${rs.length} | ${cv.length ? `${f2(med(cv))} (${f2(Math.min(...cv))}–${f2(Math.max(...cv))})` : '-'} | ${iv.length ? `${f2(med(iv))} (${f2(Math.min(...iv))}–${f2(Math.max(...iv))})` : '-'}${ie.length ? `; early ${f2(med(ie))} (${f2(Math.min(...ie))}–${f2(Math.max(...ie))})` : ''} | race p50 ${span('race', 'p50')}, p95 ${span('race', 'p95')}, p99 ${span('race', 'p99')} | warm p50 ${span('warm', 'p50')}, p95 ${span('warm', 'p95')} | first 15 s p50 ${span('early', 'p50')} |`);
}
const sum = [`bench ${path.basename(OUT)} — rev ${rev}, ${o.headed ? 'headed' : 'headless'} ${W}x${H}, script ${o.script}${o.jsFlags ? `, js-flags \`${o.jsFlags}\`` : ''}${o.profile ? ', persistent profile' : ''}${o.env.length ? `, env ${o.env.join(' ')}` : ''}`, '',
  ...HEAD, ...results.map(row), '', '| mode | game thread CPU ms/frame | M instructions/frame | race busy (median of runs, min–max) | warm | early |', '|---|---|---|---|---|---|', ...agg, ''].join('\n');
fs.writeFileSync(path.join(OUT, 'summary.md'), sum);
fs.writeFileSync(path.join(OUT, 'summary.json'), JSON.stringify({ rev, opts: o, results }, null, 1));
console.log('\n' + sum);
winCpu?.proc.stdin.end();
