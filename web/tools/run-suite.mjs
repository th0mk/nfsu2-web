// The regression suite of tests/run_suite.sh, in headless Chrome: the same
// scenarios (input scripts), environment and pass criteria, run through the
// browser host's test page (hostcheck.html) with no renderer, audio or input
// layer attached: draws are dropped (no frame dumps), the mixer runs on the
// wall clock, and the scripts' ns* keys and mouse are translated in C
// (runtime/web/input_web.c).
//
//   node web/tools/run-suite.mjs [options] [scenario ...]
//     --base URL        the local server, started with --dev (default http://localhost:8611)
//     --run NAME        output directory out/web/runs/NAME (default suite-<date>-<time>)
//     --jobs N          scenarios at once, each in its own browser context (default: a third of the cores, 1 to 4;
//                       the longest first, by the last run's times)
//     --env K=V         extra environment for every scenario (repeatable)
//     --timeout-scale X multiply every scenario's time limit (default 1)
//     --script FILE     an ad-hoc scenario: tests/scripts/FILE (with --marker, --name, --net NAME;
//                       NFSU2_SAVE_SEED seeds its save directory)
//     --save-out DIR    each scenario's save directory afterwards, as DIR/<scenario>/ (a seed for the next run)
//     --allow WHY,...   with --script: what the scenario expects of web/tools/judge.mjs's failures (freeze for
//                       watchdog_loading.txt)
//     --list            print the scenario table and exit
//   no scenario names: every runnable scenario of the table.
//   NFSU2_SAVE_SEED=<dir> (as run_suite.sh): a save tree copied into the
//   scenario's OPFS save directory first (outrun: a career in stage 2 or later,
//   e.g. the Mac port's work/seed/career/s4_outrun; sponsor: one in stage 2
//   before its sponsor, work/seed/career/s2_start).
//
// Pass (as run_suite.sh): exit code 0, "quit at guest frame", the scenario's
// marker, tests/checks/<name>.sh passes, and none of web/tools/judge.mjs's
// failures: a fault marker (FAULT IN GUEST, GUEST MEMORY FAULT, HOST SEGV),
// "TIME LIMIT REACHED" (the runner's timeout writes it), a "[trap]" line,
// "UNRESOLVED INDIRECT", "[expect] FAIL", a trap in a worker ("TRAP in
// thread"), a page error, an unimplemented host import, a stack overflow
// (RangeError), a "[net] BLOCKED" / "[roam] BLOCKED" line, a script wait that
// ran out (its TIMEOUT line: the script goes on, and a marker after it still
// comes), a script line the runtime could not read, a freeze the stall
// watchdog reported.  Scenarios marked
// net attach the page's virtual LAN (&net=<name>, web/src/net): lan_host hosts
// a LAN game with the lifted server.dll on the server's switch (the Mac runs it
// on loopback).
// Logs: out/web/runs/<run>/<scenario>.log (the game's stdout/stderr, one line
// per line, without the host's [tid] prefix); summary.txt and summary.json.
// The module the server sends (/game.json: revision, lift options, hash) is
// the run's first line, and in both summaries.
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { requireDev, moduleLine } from './dev-server.mjs';
import { seedSave } from './save-seed.mjs';
import { logWhy } from './judge.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

// what a scenario is known to show of web/tools/judge.mjs's failures: noted beside its PASS, not failing it
const ALLOW = {
  // the car lot's save dialog stays open after "save: Yes", so the script's waitfng HUD_SingleRace.fng runs out (every
  // run since 10-18); the career bot then leaves the shop by itself
  careerbot: ['wait-timeout'],
};
// name, script, marker, extra env, time limit (s; run_suite.sh's ALARM), not runnable yet: why
const TABLE = [
  ['quickrace', 'quickrace.txt', 'waitmem [0x008654a4]==6 satisfied', '', 400],
  ['sprint', 'qr_mode1.txt', 'waitmem [0x008654a4]==6 satisfied', '', 400],
  ['drag', 'qr_mode2.txt', 'waitmem [0x008654a4]==6 satisfied', '', 400],
  ['drift', 'qr_mode3.txt', 'waitmem [0x008654a4]==6 satisfied', '', 400],
  ['driftscore', 'drift_score.txt', '[expect] PASS', 'NFSU2_FIXED_DT=120', 600],
  ['streetx', 'qr_mode4.txt', 'waitmem [0x008654a4]==6 satisfied', '', 400],
  ['url', 'qr_mode5.txt', 'waitmem [0x008654a4]==6 satisfied', '', 400],
  ['career', 'career_prologue.txt', 'waitfng HUD_SingleRace.fng satisfied', '', 400],
  ['customize', 'mainmenu_item3.txt', 'screen UI_GenericParts_Browser.fng', '', 400],
  ['movies', 'career_movies.txt', 'tapuntil UI_StartCareer.fng satisfied', 'NOMOVIES=0', 400],
  ['options', 'options_display.txt', '[D3D] Reset', '', 400],
  // the Horn Volume slider (runtime/hornmenu.c): Options > Audio, and the pause menu's, whose bar is made of copies
  ['hornvolume', 'options_horn.txt', '[horn] volume 85 %', '', 400],
  ['hornpause', 'pause_horn.txt', 'parts as SLIDER_BASE_8 SLIDER_KNOB_8 SLIDER_HANDLE_8 SLIDER_BEG_8 SLIDER_END_8', '', 400],
  // Exposure and White Level (runtime/tonemenu.c): Options > Display after Level Of Detail, and the pause menu's
  ['tone', 'options_tone.txt', '[tone] white level 70 %', '', 400],
  ['tonepause', 'pause_tone.txt', '[tone] white level 90 %', '', 400],
  // Power Units (runtime/powerunits.c): a row of Options > Gameplay after Speedometer Units
  ['powerunits', 'options_units.txt', '[units] power in bhp', '', 400],
  // the European game's Corsa and 106 (runtime/regional.c): Quick Race in the Corsa
  ['eucars', 'qr_eucars.txt', '[expect] PASS', '', 400],
  // photo mode (runtime/photo.c): on, its camera, off, and the pause menu's Photo Mode, as the page drives them
  ['photo', 'photo_mode.txt', "the pause menu's Photo Mode", '', 400],
  // the pause menu's Replay (runtime/photo.c leaves it to the game): after some driving it plays the race back
  ['replay', 'replay_pause.txt', 'LOG back_to_pause', '', 400],
  // ...and at 240 fps to the end, also after skipping back to the start and forward: it checks every frame that
  // WorldTimer ticks as in the race (framerate.c's timer fix follows the game's own clock, which a replay restores)
  ['replay240', 'replay_sync.txt', 'LOG back_to_pause', 'NFSU2_FPS=240 NFSU2_HFR_LOCKSTEP=1', 400],
  // ns* commands: without a page input layer, runtime/web/input_web.c translates them into the event ring
  ['keyboard', 'kb_rebind_save.txt', 'waitmem [0x008654a4]==6 satisfied', '', 400],
  ['careerbot', 'career_bot.txt', 'segment done', 'NFSU2_CAREER_BOT_RACES=2', 600],
  ['lan_host', 'lan_host_short.txt', "auth NAME 'SUITEHOST'", '', 400, null, 'suitehost'],
  // the browser's alone: the game data's server loses every 7th request and leaves every 29th unanswered, and the
  // reads wait and ask again (web/src/host/io-worker.ts; a failed read gave the game 0 bytes, and it crashed at boot);
  // every 16th block's compressed address never answers (its byte range does), and the cache on this PC fills up
  // at the 40th block (a read failed with it, and the game crashed)
  ['datafault', 'quickrace.txt', 'waitmem [0x008654a4]==6 satisfied', 'NFSU2_DATA_FAULT=fail:7,hang:29,stall:3000,deny:16,full:40', 400],
].map(([name, script, marker, extra, alarm, blocked, net]) => ({ name, script, marker, extra, alarm, blocked, net, allow: ALLOW[name] }));
// opt-in (named only): needs a career save from stage 2 on (NFSU2_SAVE_SEED on the Mac)
const SEED = process.env.NFSU2_SAVE_SEED ?? '';
const OPT = [{ name: 'outrun', script: 'career_outrun.txt', marker: 'player completion 6 (6 won', extra: 'NFSU2_CAREER_RACEWIN=3', alarm: 400,
  seed: true, blocked: SEED ? undefined : 'needs a career save from stage 2 on: NFSU2_SAVE_SEED=<save tree>' },
{ name: 'sponsor', script: 'career_sponsor.txt', marker: 'waitfng UI_postrace.fng satisfied', extra: 'NFSU2_CAREER_RACEWIN=3', alarm: 400,
  seed: true, blocked: SEED ? undefined : 'needs a career save in stage 2 before its sponsor: NFSU2_SAVE_SEED=<save tree>' }];

// the environment of tests/run_suite.sh + tools/autorun.sh
const SUITE_ENV = {
  NFSU2_SAVEDIR: '/save', NFSU2_READONLY_DATA: '1', NFSU2_DISCORD: '0', NOFOCUS: '2', NFSU2_FNG_LOG: '1',
  NFSU2_STATUS_EVERY: '600', NOMOVIES: '1', SNDSYNC: '0', GPUVS: '1', FIXMAT: '0', FIXMAT_MODE: '3',
};

// ---- arguments ----
const argv = process.argv.slice(2);
// a game in headless Chrome keeps about three cores busy
const JOBS = Math.max(1, Math.min(4, Math.floor(os.cpus().length / 3)));
const opt = { base: 'http://localhost:8611', run: '', jobs: JOBS, env: [], scale: 1, script: '', marker: '', name: '', net: '', saveOut: '', allow: [], list: false };
const names = [];
for (let i = 0; i < argv.length; i++) {
  const a = argv[i], v = () => argv[++i];
  if (a === '--base') opt.base = v(); else if (a === '--run') opt.run = v(); else if (a === '--jobs') opt.jobs = +v();
  else if (a === '--env') opt.env.push(v()); else if (a === '--timeout-scale') opt.scale = +v();
  else if (a === '--script') opt.script = v(); else if (a === '--marker') opt.marker = v(); else if (a === '--name') opt.name = v();
  else if (a === '--net') opt.net = v(); else if (a === '--save-out') opt.saveOut = v();
  else if (a === '--allow') opt.allow.push(...v().split(','));
  else if (a === '--list') opt.list = true; else names.push(a);
}
if (opt.list) {
  for (const s of [...TABLE, ...OPT]) console.log(`${s.name.padEnd(11)} ${s.script.padEnd(22)} ${s.blocked ? 'NOT RUNNABLE YET: ' + s.blocked : `marker "${s.marker}"${s.extra ? ', ' + s.extra : ''}, ${s.alarm} s`}`);
  process.exit(0);
}
await requireDev(opt.base);
// the module the server sends (/game.json): its revision and lift go with the run's results
const MODULE = await moduleLine(opt.base);
if (MODULE) console.log(MODULE);
let scen;
if (opt.script) scen = [{ name: opt.name || path.basename(opt.script, '.txt'), script: opt.script, marker: opt.marker, extra: '', alarm: 400, net: opt.net,
  seed: !!SEED, allow: opt.allow }];   // NFSU2_SAVE_SEED seeds an ad-hoc scenario too
else if (names.length) {
  scen = names.map(n => [...TABLE, ...OPT].find(s => s.name === n) ?? (console.error(`unknown scenario ${n} (--list)`), process.exit(2)));
} else scen = TABLE;
const stamp = new Date().toISOString().replace(/[-:]/g, '').replace('T', '-').slice(0, 15);
const run = opt.run || `suite-${stamp}`;
const OUT = path.join(ROOT, 'out/web/runs', run);
fs.mkdirSync(OUT, { recursive: true });
const extraEnv = Object.fromEntries(opt.env.map(e => [e.slice(0, e.indexOf('=')), e.slice(e.indexOf('=') + 1)]));

const saveName = s => `suite-${run}-${s.name}`.replace(/[^\w.-]/g, '_');

function urlFor(s) {
  const env = { ...SUITE_ENV, ...Object.fromEntries(s.extra.split(/\s+/).filter(Boolean).map(e => e.split('='))), ...extraEnv };
  const q = new URLSearchParams();
  q.set('wasm', '/game.wasm'); q.set('pages', '16384'); q.append('arg', '/data/SPEED2.EXE');
  for (const [k, v] of Object.entries(env)) q.append('env', `${k}=${v}`);
  q.set('save', saveName(s)); if (!s.seed) q.set('fresh', '');     // a seeded save dir is prepared by seedSave
  q.set('reg', '/tests/web/seed/registry_perf4_native.txt');
  q.set('script', s.script.startsWith('/') ? s.script : `/tests/scripts/${s.script}`);
  if (s.net) q.set('net', s.net);
  return `${opt.base}/test/hostcheck.html?${q}`;
}

function judge(s, log, exit) {
  const why = [];
  if (exit === null) why.push('no-exit'); else if (exit !== 0) why.push(`exit=${exit}`);
  if (!log.includes('quit at guest frame')) why.push('no-quit');
  if (s.marker && !log.includes(s.marker)) why.push(`missing:'${s.marker}'`);
  why.push(...logWhy(log, { allow: s.allow }));
  why.noted = logWhy(log).filter(w => s.allow?.includes(w));
  const chk = path.join(ROOT, 'tests/checks', `${s.name}.sh`);
  if (fs.existsSync(chk)) {
    let out = '', ok = true;
    try { out = execFileSync(chk, [path.join(OUT, `${s.name}.log`)], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }); }
    catch (e) { ok = false; out = String(e.stdout ?? '') + String(e.stderr ?? ''); }
    fs.writeFileSync(path.join(OUT, `${s.name}.check.txt`), out);
    if (!ok) why.push(`check-failed(${out.trim().split('\n').pop()})`);
  }
  return why;
}

// the page's OPFS save directory dir into the folder out (files, as the page has them)
async function saveOut(page, dir, out) {
  try {
    const files = await page.evaluate(async dir => {
      const got = [];
      async function walk(d, rel) {
        for await (const [name, h] of d.entries()) {
          const r = rel ? `${rel}/${name}` : name;
          if (h.kind === 'directory') await walk(h, r);
          else {
            const b = new Uint8Array(await (await h.getFile()).arrayBuffer());
            let s = ''; for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode(...b.subarray(i, i + 0x8000));
            got.push([r, btoa(s)]);
          }
        }
      }
      await walk(await (await navigator.storage.getDirectory()).getDirectoryHandle(dir), '');
      return got;
    }, dir);
    fs.rmSync(out, { recursive: true, force: true });
    for (const [rel, b64] of files) {
      fs.mkdirSync(path.dirname(path.join(out, rel)), { recursive: true });
      fs.writeFileSync(path.join(out, rel), Buffer.from(b64, 'base64'));
    }
    return `${files.length} files to ${out}`;
  } catch (e) { return `failed: ${e}`; }
}

async function runOne(browser, s) {
  const logPath = path.join(OUT, `${s.name}.log`);
  const lines = [];
  const ctx = await browser.newContext();
  const page = await ctx.newPage();
  let exit = null;
  const t0 = Date.now();
  page.on('console', m => {
    const t = m.text();
    // the test page prefixes game output with [tid] or [tid!] (stderr)
    const g = /^\[\d+!?\] ([^]*)$/.exec(t);
    if (g) lines.push(g[1]);
    else {
      const e = /^EXIT (-?\d+)$/.exec(t);
      if (e) exit = +e[1];
      lines.push(t);
    }
  });
  page.on('pageerror', e => lines.push(`PAGEERROR ${e}`));
  page.on('crash', () => lines.push('CRASH (renderer process)'));
  const limit = s.alarm * opt.scale * 1000;
  if (s.seed) lines.push(`[suite] save seed: ${await seedSave(page, opt.base, saveName(s), SEED)} files from ${SEED}`);
  await page.goto(urlFor(s));
  let timedOut = false;
  try { await page.waitForFunction(() => document.title === 'DONE', null, { timeout: limit, polling: 1000 }); }
  catch { timedOut = true; lines.push(`TIME LIMIT REACHED: the web suite runner stopped the run after ${(limit / 1000).toFixed(0)} s`); }
  if (opt.saveOut) lines.push(`[suite] save out: ${await saveOut(page, saveName(s), path.join(opt.saveOut, s.name))}`);
  const wall = (Date.now() - t0) / 1000;
  await ctx.close().catch(() => {});
  const log = lines.join('\n') + '\n';
  fs.writeFileSync(logPath, log);
  const why = judge(s, log, timedOut ? null : exit);
  const st = (log.match(/^\[status\] .*$/gm) ?? []).pop()?.replace(/^\[status\] /, '') ?? '';
  const frame = /quit at guest frame (\d+)/.exec(log)?.[1] ?? '-';
  return { name: s.name, pass: why.length === 0, why, noted: why.noted, wall, frame, status: st, log: path.relative(ROOT, logPath) };
}

const results = [], skipped = [];
const queue = scen.filter(s => { if (s.blocked) { skipped.push(s); return false; } return true; });
// the longest first, so that the jobs end together instead of a long one starting last: each scenario's wall time in
// the latest run that had it (out/web/runs/*/summary.json), else its time limit
const lastWall = {};
const runsDir = path.join(ROOT, 'out/web/runs');
for (const d of (fs.existsSync(runsDir) ? fs.readdirSync(runsDir) : []).map(d => path.join(runsDir, d, 'summary.json'))
  .filter(f => fs.existsSync(f)).sort((a, b) => fs.statSync(a).mtimeMs - fs.statSync(b).mtimeMs)) {
  try { for (const r of JSON.parse(fs.readFileSync(d, 'utf8')).results ?? []) if (r.wall) lastWall[r.name] = r.wall; } catch {}
}
queue.sort((a, b) => (lastWall[b.name] ?? b.alarm) - (lastWall[a.name] ?? a.alarm));
// (nothing to run, every scenario named not runnable yet: no browser, only the summaries)
const browser = queue.length ? await chromium.launch({ ...pickBrowser(), headless: true,
  args: ['--enable-unsafe-webgpu', ...(process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ') : [])] }) : null;
console.log(`web suite ${run}: ${queue.length} scenario(s), ${opt.jobs} at a time, logs in ${path.relative(ROOT, OUT)}/`);
async function worker() {
  for (let s; (s = queue.shift());) {
    const r = await runOne(browser, s);
    results.push(r);
    console.log(`${r.pass ? 'PASS' : 'FAIL'} ${r.name.padEnd(11)} ${r.wall.toFixed(0).padStart(4)} s  frame ${r.frame}${r.pass ? '' : '  ' + r.why.join(' ')}` +
      (r.noted.length ? `  (known: ${r.noted.join(' ')})` : ''));
  }
}
await Promise.all(Array.from({ length: Math.max(1, opt.jobs) }, worker));
await browser?.close();

const order = scen.map(s => s.name);
results.sort((a, b) => order.indexOf(a.name) - order.indexOf(b.name));
const rows = [
  '| scenario | result | wall | quit frame | reason / last [status] |', '|---|---|---|---|---|',
  ...results.map(r => `| ${r.name} | ${r.pass ? 'PASS' : 'FAIL'} | ${r.wall.toFixed(0)} s | ${r.frame} | ${r.pass ? r.status.replace(/\|/g, '/') : r.why.join(' ')} |`),
  ...skipped.map(s => `| ${s.name} | not runnable yet | - | - | ${s.blocked} |`),
];
const pass = results.filter(r => r.pass).length;
const sum = `web suite ${run}: ${pass} passed, ${results.length - pass} failed, ${skipped.length} not runnable yet\n` +
  `${MODULE ? MODULE + '\n' : ''}\n${rows.join('\n')}\n`;
fs.writeFileSync(path.join(OUT, 'summary.txt'), sum);
fs.writeFileSync(path.join(OUT, 'summary.json'), JSON.stringify({ run, module: MODULE, env: { ...SUITE_ENV, ...extraEnv }, results, skipped: skipped.map(s => ({ name: s.name, why: s.blocked })) }, null, 1));
console.log('\n' + sum);
process.exitCode = results.length === pass ? 0 : 1;
