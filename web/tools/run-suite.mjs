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
//     --jobs N          scenarios at once, each in its own browser context (default 1)
//     --env K=V         extra environment for every scenario (repeatable)
//     --timeout-scale X multiply every scenario's time limit (default 1)
//     --script FILE     an ad-hoc scenario: tests/scripts/FILE (with --marker, --name, --net NAME;
//                       NFSU2_SAVE_SEED seeds its save directory)
//     --save-out DIR    each scenario's save directory afterwards, as DIR/<scenario>/ (a seed for the next run)
//     --reg PATH        the registry every scenario starts from, as the server serves it (/tests/seed/X.txt),
//                       instead of the game's own (Underground 2's registry_perf4_native.txt, Most Wanted's none)
//     --list           print the scenario table and exit
//     --game ID         ug2 (default) or mw: that game's scenario table, test scripts and checks (tests/ or
//                       tests/mw/: the server, started with --game mw, serves tests/mw/ at /tests/)
//     --help            this text and the game's scenarios
//   no scenario names: every runnable scenario of the table.
//   NFSU2_SAVE_SEED=<dir> (as run_suite.sh): a save tree copied into the
//   scenario's OPFS save directory first (outrun: a career in stage 2 or later,
//   e.g. the Mac port's work/seed/career/s4_outrun; sponsor: one in stage 2
//   before its sponsor, work/seed/career/s2_start).
//
// Pass (as run_suite.sh): exit code 0, "quit at guest frame" (or, for a script
// that ends by `close`, the game's own exit after the WM_CLOSE), the scenario's
// marker, tests/checks/<name>.sh passes, and G-01 (web/tools/g01.mjs): no fault
// marker (FAULT IN GUEST, GUEST MEMORY FAULT, HOST SEGV), no "TIME LIMIT
// REACHED" (the runner's timeout writes it), no "[trap]" line, no "UNRESOLVED
// INDIRECT", no "[expect] FAIL", no "[net] BLOCKED" / "[roam] BLOCKED" line, and
// what Most Wanted added for every game: no C++ throw stop, no genexec bail, no
// "[hle] UNIMPLEMENTED" import off the game's reviewed list
// (games/<id>/hle-reviewed.txt), no watchdog stall report, no [init] line short
// of its tables.  The browser adds: no trap in a worker ("TRAP in thread"), no
// page error, no unimplemented host import, no stack overflow (RangeError).
// Scenarios marked net attach the page's virtual LAN (&net=<name>,
// web/src/net): lan_host hosts a LAN game with the lifted server.dll on the
// server's switch (the Mac runs it on loopback).
// Logs: out/web/runs/<run>/<scenario>.log (the game's stdout/stderr, one line
// per line, without the host's [tid] prefix); summary.txt and summary.json.
// The module the server sends (/game.json: revision, game, lift options,
// hash) is the run's first line, and in both summaries; for a game other than
// Underground 2 also each scenario log's first line (L-020).
// Each game has its own table below: Most Wanted's is empty until its first
// scenario exists (an ad-hoc --script runs one of tests/mw/scripts/).
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { requireDev } from './dev-server.mjs';
import { seedSave } from './save-seed.mjs';
import { GAMES, gameOf, usage, nothingFor, requireGame, moduleLine } from './rig-game.mjs';
import { g01, readReviewed } from './g01.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

// Underground 2's scenarios: name, script, marker, extra env, time limit (s; run_suite.sh's ALARM), not runnable yet: why
const UG2_TABLE = [
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
].map(([name, script, marker, extra, alarm, blocked, net]) => ({ name, script, marker, extra, alarm, blocked, net }));
// opt-in (named only): needs a career save from stage 2 on (NFSU2_SAVE_SEED on the Mac)
const SEED = process.env.NFSU2_SAVE_SEED ?? '';
const UG2_OPT = [{ name: 'outrun', script: 'career_outrun.txt', marker: 'player completion 6 (6 won', extra: 'NFSU2_CAREER_RACEWIN=3', alarm: 400,
  seed: true, blocked: SEED ? undefined : 'needs a career save from stage 2 on: NFSU2_SAVE_SEED=<save tree>' },
{ name: 'sponsor', script: 'career_sponsor.txt', marker: 'waitfng UI_postrace.fng satisfied', extra: 'NFSU2_CAREER_RACEWIN=3', alarm: 400,
  seed: true, blocked: SEED ? undefined : 'needs a career save in stage 2 before its sponsor: NFSU2_SAVE_SEED=<save tree>' },
// the thread bounds (runtime/win32_ext.c NFSU2_THREAD_TEST): 33 guest threads alive at once before the game starts, the
// 33rd refused, no stack in the heap (tests/checks/threads.sh); then the quick race.  A worker per thread: pool 64
{ name: 'threads', script: 'quickrace.txt', marker: '[thread] test: 33 threads asked for, 32 started, 1 refused',
  extra: 'NFSU2_THREAD_TEST=33:4000', alarm: 400, pool: 64 }];

// Most Wanted's scenarios (tests/mw/scripts/, tests/mw/checks/), as Underground 2's
const MW_TABLE = [
  // Underground 2's datafault on the start to the main menu (the races and the world are M4's): every 7th request to
  // the game data's server is lost, every 29th left unanswered, every 4th block's compressed address never answers
  // (the start reads no file as far as a 16th block, Underground 2's), and the cache on this PC fills up at the 40th
  // block; tests/mw/checks/datafault.sh wants each of them seen
  ['datafault', 'boot_menu.txt', 'waitfng MainMenu.fng satisfied', 'NFSU2_DATA_FAULT=fail:7,hang:29,stall:3000,deny:4,full:40', 400],
  // a Custom Race circuit (City Perimeter) to GO, then 10,000 frames of the game's AI driving with the race still
  // on: the per-frame anchor once a Present (TRACKER M4-01; runtime/mw/career_mw.c's aids run there); 25 races in a
  // row are race_loop.txt's, ad hoc
  ['race_frames', 'race_frames.txt', 'race_frames: 10000 frames', 'NFSU2_QUIT_FRAMES=20000', 600],
// NFSU2_SAVE_SEED seeds each, as an ad-hoc scenario: a build with the D3DX oracle needs d3dx9_26.dll in the save
// folder (and --env NFSMW_D3DX=/save/d3dx9_26.dll; tests/mw/scripts/boot_menu.txt)
].map(([name, script, marker, extra, alarm, blocked, net]) => ({ name, script, marker, extra, alarm, blocked, net, seed: !!SEED }));

// each game's table (run when no scenario is named) and its opt-in scenarios
const SCENARIOS = { ug2: [UG2_TABLE, UG2_OPT], mw: [MW_TABLE, []] };
const NONE = {};    // a game whose table is empty: why ("none yet" when not given)

// the environment of tests/run_suite.sh + tools/autorun.sh (DESIGN.md section 5)
const SUITE_ENV = {
  NFSU2_SAVEDIR: '/save', NFSU2_READONLY_DATA: '1', NFSU2_DISCORD: '0', NOFOCUS: '2', NFSU2_FNG_LOG: '1',
  NFSU2_STATUS_EVERY: '600', NOMOVIES: '1', SNDSYNC: '0', GPUVS: '1', FIXMAT: '0', FIXMAT_MODE: '3',
};

// ---- arguments ----
const argv = process.argv.slice(2);
const opt = { base: 'http://localhost:8611', run: '', jobs: 1, env: [], scale: 1, script: '', marker: '', name: '', net: '', saveOut: '', reg: '', list: false, help: false };
const names = [];
const GAME = gameOf(argv), G = GAMES[GAME], [TABLE, OPT] = SCENARIOS[GAME];
const OF = GAME === 'ug2' ? '' : ` (${G.name})`;   // in the run's lines and summary: Underground 2's stay as they were
for (let i = 0; i < argv.length; i++) {
  const a = argv[i], v = () => argv[++i];
  if (a === '--base') opt.base = v(); else if (a === '--run') opt.run = v(); else if (a === '--jobs') opt.jobs = +v();
  else if (a === '--env') opt.env.push(v()); else if (a === '--timeout-scale') opt.scale = +v();
  else if (a === '--script') opt.script = v(); else if (a === '--marker') opt.marker = v(); else if (a === '--name') opt.name = v();
  else if (a === '--net') opt.net = v(); else if (a === '--save-out') opt.saveOut = v();
  else if (a === '--reg') opt.reg = v();
  else if (a === '--game') v();                    // gameOf() above
  else if (a === '--help') opt.help = true;
  else if (a === '--list') opt.list = true; else names.push(a);
}
const listing = () => [...TABLE, ...OPT].map(s => `${s.name.padEnd(11)} ${s.script.padEnd(22)} ${s.blocked ? 'NOT RUNNABLE YET: ' + s.blocked : `marker "${s.marker}"${s.extra ? ', ' + s.extra : ''}, ${s.alarm} s`}`);
if (opt.help) usage(import.meta.url, GAME, 'Scenarios', listing(), NONE[GAME]);
if (opt.list) {
  for (const l of listing().length ? listing() : [NONE[GAME] ?? 'none yet']) console.log(l);
  process.exit(0);
}
if (!opt.script && !names.length && !TABLE.length) nothingFor(GAME, NONE[GAME]);
await requireDev(opt.base);
await requireGame(opt.base, GAME);
// the module the server sends (/game.json): its revision and lift go with the run's results
const MODULE = await moduleLine(opt.base);
if (MODULE) console.log(MODULE);
let scen;
if (opt.script) scen = [{ name: opt.name || path.basename(opt.script, '.txt'), script: opt.script, marker: opt.marker, extra: '', alarm: 400, net: opt.net,
  seed: !!SEED }];   // NFSU2_SAVE_SEED seeds an ad-hoc scenario too
else if (names.length) {
  scen = names.map(n => [...TABLE, ...OPT].find(s => s.name === n) ?? (console.error(`unknown scenario ${n} (--list)`), process.exit(2)));
} else scen = TABLE;
await requireOwnCheckout(opt.base, scen);
const stamp = new Date().toISOString().replace(/[-:]/g, '').replace('T', '-').slice(0, 15);
const run = opt.run || `suite-${stamp}`;
const OUT = path.join(ROOT, 'out/web/runs', run);
fs.mkdirSync(OUT, { recursive: true });
const extraEnv = Object.fromEntries(opt.env.map(e => [e.slice(0, e.indexOf('=')), e.slice(e.indexOf('=') + 1)]));
// Most Wanted's module with the D3DX oracle ("oracle":1 in /game.json, web/build.sh) loads d3dx9_26.dll before the
// game's entry point, from the page's save folder: without a seed holding it and NFSMW_D3DX naming it the start
// stops at its first effect import (an unimplemented D3DXCreateEffectPool), so the runs would fail for that alone
const ORACLE_SEED = 'this module has the D3DX oracle, which loads d3dx9_26.dll from the save folder: ' +
  'NFSU2_SAVE_SEED=<a folder holding it> and --env NFSMW_D3DX=/save/d3dx9_26.dll (tests/mw/scripts/boot_menu.txt)';
if (GAME === 'mw' && !(SEED && extraEnv.NFSMW_D3DX) && await isOracle(opt.base))
  for (const s of scen) s.blocked ??= ORACLE_SEED;

// the imports a run of this game has reviewed (G-01: an [hle] UNIMPLEMENTED line off the list fails a run)
const REVIEWED = readReviewed(GAME);

const saveName = s => `suite-${run}-${s.name}`.replace(/[^\w.-]/g, '_');

// The server at base serves this checkout: the checks are this checkout's (tests/checks/, tests/mw/checks/), so the
// scenarios' scripts it serves at /tests/scripts/ must be this checkout's too, or the run is refused (a server
// started for another worktree, by its NFSU2_REPO or by the binary's own default, the checkout it was built in,
// serves that one's scripts and module: web/tools/server-cmd.mjs sets NFSU2_REPO for the tools that start one).  A
// module other than this checkout's own build (out/web/nfsu2.json, out/web/mw/nfsmw.json) only gets a note: --wasm
// serves another on purpose, and the module line names the one a result is for.
async function requireOwnCheckout(base, list) {
  const at = p => new URL(p, base.replace(/\/*$/, '/'));
  for (const s of list) {
    if (!s.script || s.script.startsWith('/')) continue;
    const local = path.join(ROOT, G.tests, 'scripts', s.script);
    let mine; try { mine = fs.readFileSync(local, 'utf8'); } catch { continue; }    // the run says what is missing
    let r; try { r = await fetch(at(`tests/scripts/${s.script}`)); } catch { return; }
    const served = r.ok ? await r.text() : null;
    if (served === mine) continue;
    console.error(`${base} serves another checkout's tests: its /tests/scripts/${s.script} ` +
      `${served === null ? `is missing (HTTP ${r.status}), unlike` : 'is not'} this checkout's ${path.relative(ROOT, local)}; start the ` +
      `server with NFSU2_REPO=${ROOT} (and --wasm for another module)`);
    process.exit(2);
  }
  const own = { ug2: 'out/web/nfsu2.json', mw: 'out/web/mw/nfsmw.json' }[GAME];
  let j, o;
  try { j = await (await fetch(at('game.json'))).json(); o = JSON.parse(fs.readFileSync(path.join(ROOT, own), 'utf8')); } catch { return; }
  if (j?.rev !== o?.rev || j?.size !== o?.size)
    console.log(`note: the server sends another module (rev ${j?.rev}, ${j?.size} bytes) than this checkout's ${own} ` +
      `(rev ${o?.rev}, ${o?.size} bytes)`);
}

// the module the server sends has the D3DX oracle ("oracle":1 in its /game.json); false when there is no json
async function isOracle(base) {
  try { return !!(await (await fetch(new URL('game.json', base.replace(/\/*$/, '/')))).json())?.oracle; } catch { return false; }
}

function urlFor(s) {
  const env = { ...SUITE_ENV, ...Object.fromEntries(s.extra.split(/\s+/).filter(Boolean).map(e => e.split('='))), ...extraEnv };
  const q = new URLSearchParams();
  q.set('wasm', '/game.wasm'); q.set('pages', '16384'); q.append('arg', G.exe);
  for (const [k, v] of Object.entries(env)) q.append('env', `${k}=${v}`);
  q.set('save', saveName(s)); if (!s.seed) q.set('fresh', '');     // a seeded save dir is prepared by seedSave
  if (opt.reg || G.reg) q.set('reg', opt.reg || G.reg);
  q.set('script', s.script.startsWith('/') ? s.script : `/tests/scripts/${s.script}`);
  if (s.net) q.set('net', s.net);
  if (s.pool) q.set('pool', String(s.pool));
  return `${opt.base}/test/hostcheck.html?${q}`;
}

// a script's `close` (runtime/lifecycle.c): the game exited by itself after the WM_CLOSE.  Games other than
// Underground 2 only: its runs still quit by the runtime's own quit (the "quit at guest frame" line)
const closedByGame = log => !!OF && /^app: the game exited \(code 0\) after the quit request$/m.test(log);

function judge(s, log, exit) {
  const why = [];
  const has = re => re.test(log);
  if (exit === null) why.push('no-exit'); else if (exit !== 0) why.push(`exit=${exit}`);
  if (!log.includes('quit at guest frame') && !closedByGame(log)) why.push('no-quit');
  why.push(...g01(log, GAME, REVIEWED));
  if (s.marker && !log.includes(s.marker)) why.push(`missing:'${s.marker}'`);
  if (has(/^TRAP in thread/m)) why.push('wasm-trap');
  if (has(/^PAGEERROR|^WORKERERROR|^CRASH/m)) why.push('page-error');
  if (has(/unimplemented import/)) why.push('unimplemented-import');
  if (has(/RangeError|Maximum call stack/)) why.push('stack-overflow');
  const chk = path.join(ROOT, G.tests, 'checks', `${s.name}.sh`);
  if (fs.existsSync(chk)) {
    let out = '', ok = true;
    try { out = execFileSync(chk, [path.join(OUT, `${s.name}.log`)], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }); }
    catch (e) { ok = false; out = String(e.stdout ?? '') + String(e.stderr ?? ''); }
    fs.writeFileSync(path.join(OUT, `${s.name}.check.txt`), out);
    if (!ok) why.push(`check-failed(${out.trim().split('\n').pop()})`);
  }
  return why;
}

// the page's OPFS save directory dir into the folder out (files, and folders left empty, as the page has them: a
// folder a delete left behind seeds the next run too, save-seed.mjs)
async function saveOut(page, dir, out) {
  try {
    const { files, empty } = await page.evaluate(async dir => {
      const files = [], empty = [];
      async function walk(d, rel) {
        let n = 0;
        for await (const [name, h] of d.entries()) {
          const r = rel ? `${rel}/${name}` : name;
          n++;
          if (h.kind === 'directory') await walk(h, r);
          else {
            const b = new Uint8Array(await (await h.getFile()).arrayBuffer());
            let s = ''; for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode(...b.subarray(i, i + 0x8000));
            files.push([r, btoa(s)]);
          }
        }
        if (!n && rel) empty.push(rel);
      }
      await walk(await (await navigator.storage.getDirectory()).getDirectoryHandle(dir), '');
      return { files, empty };
    }, dir);
    fs.rmSync(out, { recursive: true, force: true });
    fs.mkdirSync(out, { recursive: true });
    for (const [rel, b64] of files) {
      fs.mkdirSync(path.dirname(path.join(out, rel)), { recursive: true });
      fs.writeFileSync(path.join(out, rel), Buffer.from(b64, 'base64'));
    }
    for (const rel of empty) fs.mkdirSync(path.join(out, rel), { recursive: true });
    return `${files.length} files${empty.length ? ` and ${empty.length} empty folder(s) (${empty.join(', ')})` : ''} to ${out}`;
  } catch (e) { return `failed: ${e}`; }
}

async function runOne(browser, s) {
  const logPath = path.join(OUT, `${s.name}.log`);
  const lines = OF && MODULE ? [MODULE] : [];     // the module under test in the log itself (Underground 2's stay as they were)
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
  const frame = /quit at guest frame (\d+)/.exec(log)?.[1]
    ?? (closedByGame(log) ? `${/^\s*Present \(frames\)\s*:\s*(\d+)/m.exec(log)?.[1] ?? '?'} (WM_CLOSE)` : '-');
  return { name: s.name, pass: why.length === 0, why, wall, frame, status: st, log: path.relative(ROOT, logPath) };
}

const results = [], skipped = [];
const queue = scen.filter(s => { if (s.blocked) { skipped.push(s); return false; } return true; });
// (nothing to run, every scenario named not runnable yet: no browser, only the summaries)
const browser = queue.length ? await chromium.launch({ ...pickBrowser(), headless: true,
  args: ['--enable-unsafe-webgpu', ...(process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ') : [])] }) : null;
console.log(`web suite ${run}${OF}: ${queue.length} scenario(s), ${opt.jobs} at a time, logs in ${path.relative(ROOT, OUT)}/`);
async function worker() {
  for (let s; (s = queue.shift());) {
    const r = await runOne(browser, s);
    results.push(r);
    console.log(`${r.pass ? 'PASS' : 'FAIL'} ${r.name.padEnd(11)} ${r.wall.toFixed(0).padStart(4)} s  frame ${r.frame}${r.pass ? '' : '  ' + r.why.join(' ')}`);
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
const sum = `web suite ${run}${OF}: ${pass} passed, ${results.length - pass} failed, ${skipped.length} not runnable yet\n` +
  `${MODULE ? MODULE + '\n' : ''}\n${rows.join('\n')}\n`;
fs.writeFileSync(path.join(OUT, 'summary.txt'), sum);
fs.writeFileSync(path.join(OUT, 'summary.json'), JSON.stringify({ run, ...(OF ? { game: GAME } : {}), module: MODULE, env: { ...SUITE_ENV, ...extraEnv }, results, skipped: skipped.map(s => ({ name: s.name, why: s.blocked })) }, null, 1));
console.log('\n' + sum);
process.exitCode = results.length === pass ? 0 : 1;
