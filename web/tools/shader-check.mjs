// Validate the WGSL shader set in Chrome (docs/web/gpu-abi.md section 9).
//
//   node web/tools/shader-check.mjs --game [--out out/web/shaders]
//       run the quick race headless with NFSU2_SHADER_DUMP (null renderer),
//       copy the dump from OPFS to --out, add every vertex shader x declaration
//       pair and the CPU-interpreter test vectors (web/tools/wgsl-xl --pairs,
//       built with the Mac's cc), then check all of it
//   node web/tools/shader-check.mjs --from <dir> [--game mw] [--census]
//       check a dump on disk (an earlier --game run or web/tools/wgsl-xl output);
//       with --from, --game only names the dump's game (its census floor for
//       --census): there is no game run
//   node web/tools/shader-check.mjs --game mw
//       the game run of that game (--game alone: ug2's): Most Wanted's boots to
//       its main menu (tests/mw/scripts/boot_menu.txt) and leaves by WM_CLOSE;
//       NFSU2_SAVE_SEED=<dir> seeds the save directory first (the D3DX oracle
//       module's d3dx9_26.dll, with --env NFSMW_D3DX=/save/d3dx9_26.dll)
//   node web/tools/shader-check.mjs --sm3 [--out out/web/sm3]
//       the ps_2_x, ps_3_0 and vs_3_0 translator against psref, the D3D9
//       reference (docs/web/psref.md): the programs of psref's unit tests
//       (psref_unit --emit) and, with Most Wanted's game folder (NFSMW_DATA,
//       default mw/Need For Speed Most Wanted Black Edition), every shader of
//       its 31 effects, translated for 8 stages with random test vectors
//       (web/tools/wgsl-xl: psref's for those models, vs_run's and
//       ps_ref_run's for the older ones), each set checked in its own page
//       run; a unit program may be refused only for what the module ABI has
//       no place for (ABI_LIMITS), and each set must check at least SM3_FLOOR
//
// Options: --port <http port> (8612), --save <OPFS dir> (shadercheck), --cross
// (every game VS x every game PS too), --timeout <ms> (game run, 600000),
// --census (with --from: also apply the game-run census below, of the game
// --game names, to that dump),
// --script <url path> (game run, default the game's: RUNS below), --env K=V
// (game run, repeatable), --help (this text and the game's run).
// Needs the server (out/web/server-rs/nfsu2-server, started with --dev, and
// --game mw for Most Wanted) on that port and web/dist built.
// Exit code 0 only when every module compiles, every pipeline builds, the
// vertex-pulling and differential tests match, no translation was REJECTED
// (index.txt, pairs.txt) and, for a game run, the game ran its whole script
// (EXIT 0, the race or the menu reached, the script's end, no fault marker) and
// its dump has at least that run's shader census (RUNS below): a run that
// stops early has a smaller dump, which fails (L-083).
// The dump is game-derived: keep it out of git.
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
import { execFileSync, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { GAMES, gameOf, usage, nothingFor } from './rig-game.mjs';
import { seedSave } from './save-seed.mjs';

const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
const port = +opt('--port', 8612), save = opt('--save', 'shadercheck');
const out = opt('--out', 'out/web/shaders'), from = opt('--from', null);
const timeout = +opt('--timeout', 600000);
const origin = `http://localhost:${port}`;
// the word after --game, unless it is another option, names the game of the run (ug2 without one); with --from it
// names the dump's game and nothing runs
const gameWord = argv[argv.indexOf('--game') + 1];
const GAME = argv.includes('--game') && gameWord !== undefined && !gameWord.startsWith('--') ? gameOf(argv) : 'ug2';
const gameRun = argv.includes('--game') && !from;
// each game's run: its script, the log lines that show it got where it goes and ran to its end, and what a whole
// run creates, the census floor: distinct vertex / pixel shader objects and declarations, registered (shader,
// declaration) vertex modules, pixel shader modules (with the cube twins).  Underground 2's is a full quick race's
// (gpu-shaders report section 6); Most Wanted's a boot to the main menu's (TRACKER M3-06: the main menu, its car
// and garage, the front end's own shaders).
const RUNS = {
  ug2: {
    what: 'the quick race (--script /tests/scripts/quickrace.txt) to its end; at least the census of a full quick race',
    script: '/tests/scripts/quickrace.txt',
    reached: ['waitmem [0x008654a4]==6 satisfied', 'the race was not reached'],
    ended: ['quit at guest frame', 'no "quit at guest frame" (the script did not finish)'],
    census: { vsObjects: 16, psObjects: 24, decls: 11, vsPairs: 17, psModules: 68 },
  },
  mw: {
    what: 'the boot to the main menu (--script /tests/scripts/boot_menu.txt) and its WM_CLOSE; at least the census of that boot',
    script: '/tests/scripts/boot_menu.txt',
    reached: ['waitfng MainMenu.fng satisfied', 'the main menu was not reached'],
    ended: ['app: the game exited (code 0) after the quit request', 'the game did not leave by WM_CLOSE (the script did not finish)'],
    // a boot to the main menu with the menu's car (headless, rev 162bdc2+, M3-06's run)
    census: { vsObjects: 30, psObjects: 65, decls: 19, vsPairs: 14, psModules: 244 },
  },
};
const RUN_OF = Object.fromEntries(Object.entries(RUNS).map(([g, r]) => [g, [r.what]]));
const NONE = 'none yet: no script or census floor for this game';
if (argv.includes('--help')) usage(import.meta.url, GAME, 'The game run', RUN_OF[GAME] ?? [], NONE);
if (!RUN_OF[GAME]) nothingFor(GAME, NONE);
await (await import('./dev-server.mjs')).requireDev(origin);

const RUN = RUNS[GAME], CENSUS = RUN.census;
// --sm3: what the module ABI has no place for (gpu-abi.md section 11.1; runtime/web/vsemu_sm3.inc), which
// psref's unit programs use and the game's shaders do not; and the modules checked at least, by the result's
// field: against psref (ps3diff, vs3diff) today's unit programs (cube twins included; a program psref refuses
// on every input has no vectors and does not count) and Most Wanted's 22 SM3-class blobs (distinct by bytes:
// 17 ps_2_x, 2 ps_3_0, 1 vs_3_0); against ps_ref_run and vs_run (psdiff, diff) its 75 older-model shaders,
// 46 pixel (each with its stage-0 cube twin) and 29 vertex; compiled, with their pipelines, Most Wanted's 65 depth
// variants (psDepth: each pixel shader with every 2D stage a depth stage, gpu-abi.md section 11.4)
const ABI_LIMITS = /REJECTED: (MRT|vertex texture fetch|a vs_3_0 without a POSITION0 output|a ps_3_0 input the module ABI has no place for|a vs_3_0 output the module ABI has no place for|a loop count the application sets|a b# the application sets)/;
const SM3_FLOOR = { unit: { ps3diff: 71, vs3diff: 4 }, mw: { ps3diff: 19, vs3diff: 1, psdiff: 92, diff: 29, psDepth: 65 } };
const problems = [];               // reasons the run fails besides the browser check
// A stage-0 cube twin (ps <id + 256>) is for a cube bound at stage 0 to a shader that does not say what it samples
// there (ps_1_x take the type from the binding).  ps_2_0, ps_2_x and ps_3_0 declare each sampler's type, and D3D9
// needs a texture of that type bound (a cube on a dcl_2d sampler is an undefined draw).  A ps_2_x or ps_3_0 whose s0
// is 2D has no twin (the SM3-class translator's reason, in index.txt by shader_dump.c); a ps_2_0 one keeps its twin,
// which reads s0 as a cube (vsemu_wgsl.inc).  The page logs "[gpu] ps N: a cube texture at stage 0 where s0 is
// declared 2D" should a game draw any of the three so (none in Most Wanted's race and menus, M3-06).
const TWIN_NOT_NEEDED = /^ps \d+ obj=\d+ REJECTED: the stage-0 cube twin of a shader whose s0 is 2D$/;
// REJECTED translations (but those allow matches) and, for a game dump, the census
function checkDump(dir, census, allow = null) {
  for (const f of ['index.txt', 'pairs.txt']) {
    const p = path.join(dir, f);
    if (!fs.existsSync(p)) continue;
    const lines = fs.readFileSync(p, 'utf8').split('\n');
    const twins = f === 'index.txt' ? lines.filter(l => TWIN_NOT_NEEDED.test(l)).length : 0;
    if (twins) console.log(`${f}: ${twins} cube twins not made, none needed: ps_2_x/ps_3_0 whose s0 is declared 2D ` +
      '(D3D9 needs a 2D texture there)');
    const rej = lines.filter(l => /REJECTED/.test(l) && !TWIN_NOT_NEEDED.test(l) && !(allow && allow.test(l)));
    for (const l of rej.slice(0, 5)) problems.push(`${f}: ${l}`);
    if (rej.length > 5) problems.push(`${f}: ${rej.length - 5} more REJECTED lines`);
  }
  if (!census) return null;
  const ix = fs.existsSync(path.join(dir, 'index.txt')) ? fs.readFileSync(path.join(dir, 'index.txt'), 'utf8') : '';
  if (!ix) problems.push('no index.txt in the dump');
  const distinct = re => new Set(fs.readdirSync(dir).filter(n => re.test(n))
    .map(n => fs.readFileSync(path.join(dir, n)).toString('base64'))).size;
  const got = {
    vsObjects: distinct(/^vsobj_\d+\.bin$/), psObjects: distinct(/^psobj_\d+\.bin$/), decls: distinct(/^decl_[0-9a-f]+\.bin$/),
    vsPairs: ix.split('\n').filter(l => /^vs \d+ obj=.* ok$/.test(l)).length,
    psModules: ix.split('\n').filter(l => /^ps \d+ obj=\d+ ok$/.test(l)).length,
  };
  for (const [k, min] of Object.entries(CENSUS)) if (!(got[k] >= min)) problems.push(`census: ${k} ${got[k]} < ${min}`);
  return got;
}

const browser = await chromium.launch({ ...pickBrowser(), headless: !argv.includes('--headed'),
  args: ['--enable-unsafe-webgpu', ...(process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ') : [])] });
const page = await browser.newPage();
let lastExit = null, result = null;
const gameLog = [];
page.on('console', m => {
  const t = m.text();
  if (t.startsWith('SHADERCHECK ')) { result = JSON.parse(t.slice(12)); return; }
  if (/^EXIT /.test(t)) lastExit = t;
  gameLog.push(t);
  if (argv.includes('--quiet') && !/ERROR|EXIT|shaderdump|PAGEERROR/.test(t)) return;
  console.log(t);
});
page.on('pageerror', e => console.log('PAGEERROR ' + e));
// write a dump directory (WGSL text + .vec test vectors) into OPFS <save>/shaders
async function upload(dir) {
  await page.goto(`${origin}/players`);
  const files = {};
  for (const n of fs.readdirSync(dir))
    if (/\.(wgsl|vec|pvec|p3vec|v3vec)$/.test(n)) files[n] = fs.readFileSync(path.join(dir, n)).toString('base64');
  await page.evaluate(async ([dir, files]) => {
    const root = await navigator.storage.getDirectory();
    const [a, b] = dir.split('/');
    try { await (await root.getDirectoryHandle(a)).removeEntry(b, { recursive: true }); } catch {}
    const d = await (await root.getDirectoryHandle(a, { create: true })).getDirectoryHandle(b, { create: true });
    for (const [n, b64] of Object.entries(files)) {
      const w = await (await d.getFileHandle(n, { create: true })).createWritable();
      await w.write(Uint8Array.from(atob(b64), c => c.charCodeAt(0))); await w.close();
    }
  }, [`${save}/shaders`, files]);
  console.log(`uploaded ${Object.keys(files).length} files from ${dir}`);
}
const waitDone = async ms => { try { await page.waitForFunction(() => document.title === 'DONE', null, { timeout: ms }); return true; } catch { return false; } };
const xl = 'out/wgsl-xl';
const buildXl = () => execFileSync('cc', ['-O1', '-ffp-contract=off', '-DVSEMU_WGSL', '-DNFS_ENHANCED=0', '-Iruntime', '-Iruntime/web', '-Iweb/tools',
  'runtime/vsemu.c', 'web/tools/wgsl-xl.c', 'web/tools/psref.c', '-lm', '-o', xl], { stdio: 'inherit' });
// the page's check of the dump uploaded last: its SHADERCHECK result (null without one); waitDone is the
// timeout ('commit': on a slow link goto's 30 s for the load event ran out)
async function pageCheck() {
  result = null;
  await page.goto(`${origin}/test/shadercheck.html?opfs=${save}/shaders${argv.includes('--cross') ? '&cross' : ''}`, { waitUntil: 'commit' });
  if (!await waitDone(300000)) console.log('TIMEOUT (shadercheck)');
  return result;
}

if (argv.includes('--sm3')) {
  const root = opt('--out', 'out/web/sm3'), unit = path.join(root, 'psref_unit');
  fs.rmSync(root, { recursive: true, force: true });
  for (const d of ['corpus', 'unit']) fs.mkdirSync(path.join(root, d), { recursive: true });
  buildXl();
  execFileSync('cc', ['-O1', '-Iweb/tools', 'web/test/psref_unit.c', 'web/tools/psref.c', '-lm', '-o', unit], { stdio: 'inherit' });
  console.log(execFileSync(unit, ['--emit', path.join(root, 'corpus')]).toString().trim());
  const progs = fs.readdirSync(path.join(root, 'corpus')).sort().map(n => path.join(root, 'corpus', n));
  // some refusals are expected here (ABI_LIMITS), so the exit code says nothing
  console.log('unit: ' + spawnSync(xl, ['--stages', '8', path.join(root, 'unit'), ...progs], { encoding: 'utf8' }).stdout.trim());
  const sets = [{ name: 'unit', dir: path.join(root, 'unit'), allow: ABI_LIMITS, floor: SM3_FLOOR.unit }];
  const exe = path.join(process.env.NFSMW_DATA || 'mw/Need For Speed Most Wanted Black Edition', 'speed.exe');
  if (fs.existsSync(exe)) {
    // the effects come out of speed.exe (tools/fxres.py, as web/test/psref.test.mjs): the game's own data, never committed
    const res = path.join(root, 'res'), mw = path.join(root, 'mw');
    execFileSync(fs.existsSync('.venv/bin/python') ? '.venv/bin/python' : 'python3', ['-I', 'tools/fxres.py', exe, res], { stdio: 'inherit' });
    fs.mkdirSync(mw);
    const r = spawnSync(xl, ['--stages', '8', '--fx', mw, ...fs.readdirSync(res).filter(n => n.endsWith('.fxo')).sort().map(n => path.join(res, n))],
      { encoding: 'utf8' });
    console.log('mw: ' + r.stdout.trim());
    if (r.status !== 0) problems.push(`mw: wgsl-xl exit ${r.status}`);
    sets.push({ name: 'mw', dir: mw, allow: null, floor: SM3_FLOOR.mw });
  } else console.log(`SKIP mw: no ${exe} (NFSMW_DATA)`);
  for (const s of sets) {
    const n0 = problems.length;
    checkDump(s.dir, false, s.allow);
    for (let i = n0; i < problems.length; i++) problems[i] = `${s.name}: ${problems[i]}`;
    await upload(s.dir);
    const r = await pageCheck();
    if (!r) { problems.push(`${s.name}: no SHADERCHECK result`); continue; }
    const { failures, ...summary } = r;
    console.log(`SHADERCHECK ${s.name} ` + JSON.stringify(summary));
    for (const f of failures ?? []) console.log(`  FAIL ${s.name}: ${f}`);
    if (!r.ok) problems.push(`${s.name}: shader check failed`);
    for (const [k, min] of Object.entries(s.floor)) {
      const got = (typeof r[k] === 'number' ? r[k] : r[k]?.modules) ?? 0;
      if (got < min) problems.push(`${s.name}: ${got} modules checked in ${k} < ${min}`);
    }
    fs.writeFileSync(path.join(s.dir, 'shadercheck.json'), JSON.stringify({ ...r, problems }, null, 1));
  }
  await browser.close();
  for (const p of problems) console.log('  FAIL ' + p);
  console.log(problems.length ? `FAIL (${problems.length} problem(s))` : 'PASS');
  process.exit(problems.length ? 1 : 0);
}

if (gameRun) {
  // DESIGN.md section 5 suite environment + the dump
  const env = ['NOMOVIES=1', 'SNDSYNC=0', 'GPUVS=1', 'NFSU2_FNG_LOG=1', 'NFSU2_STATUS_EVERY=600', 'NFSU2_DISCORD=0',
    'NFSU2_SHADER_DUMP=/save/shaders', ...argv.flatMap((a, i) => a === '--env' ? [argv[i + 1]] : [])];
  // NFSU2_SAVE_SEED: a save tree in the save directory first (as run-suite.mjs), so no &fresh
  const SEED = process.env.NFSU2_SAVE_SEED ?? '';
  if (SEED) console.log(`save seed: ${await seedSave(page, origin, save, SEED)} files from ${SEED}`);
  const u = `${origin}/test/hostcheck.html?wasm=/game.wasm&pages=16384&arg=${GAMES[GAME].exe}&save=${save}${SEED ? '' : '&fresh'}` +
    `${GAMES[GAME].reg ? `&reg=${GAMES[GAME].reg}` : ''}&script=${opt('--script', RUN.script)}` +
    env.map(e => `&env=${encodeURIComponent(e)}`).join('');
  console.log('game: ' + u);
  await page.goto(u);
  if (!await waitDone(timeout)) { console.log('TIMEOUT (game run)'); await browser.close(); process.exit(2); }
  console.log('game ' + (lastExit ?? 'no EXIT line'));
  // a crash, a stop before the race or the menu or an early exit gives a
  // smaller dump that would otherwise still pass
  const glog = gameLog.join('\n');
  if (lastExit !== 'EXIT 0') problems.push(`game: ${lastExit ?? 'no EXIT line'}`);
  if (!glog.includes(RUN.ended[0])) problems.push(`game: ${RUN.ended[1]}`);
  if (!glog.includes(RUN.reached[0])) problems.push(`game: ${RUN.reached[1]}`);
  const fault = gameLog.find(l => /FAULT IN GUEST|GUEST MEMORY FAULT|HOST SEGV|TIME LIMIT REACHED|TRAP in thread|unimplemented import|RangeError|PAGEERROR/.test(l));
  if (fault) problems.push(`game: ${fault}`);
  // copy the dump out of OPFS
  const files = await page.evaluate(async dir => {
    let d = await navigator.storage.getDirectory();
    for (const p of dir.split('/')) d = await d.getDirectoryHandle(p);
    const o = {};
    for await (const [n, h] of d.entries()) if (h.kind === 'file') {
      const b = new Uint8Array(await (await h.getFile()).arrayBuffer());
      let s = ''; for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode(...b.subarray(i, i + 0x8000));
      o[n] = btoa(s);
    }
    return o;
  }, `${save}/shaders`);
  fs.rmSync(out, { recursive: true, force: true }); fs.mkdirSync(out, { recursive: true });
  for (const [n, b64] of Object.entries(files)) fs.writeFileSync(path.join(out, n), Buffer.from(b64, 'base64'));
  console.log(`dump: ${Object.keys(files).length} files -> ${out}`);
  // every VS object x every declaration that feeds it + CPU test vectors
  if (!argv.includes('--no-pairs')) {
    buildXl();
    // wgsl-xl exits 1 when a pair is REJECTED: the dump is still checked (checkDump names the pairs.txt lines);
    // its stderr (fragile vertices, a stage count that does not match the dump's) goes straight through
    const r = spawnSync(xl, ['--pairs', out, out], { encoding: 'utf8', maxBuffer: 64 << 20, stdio: ['ignore', 'pipe', 'inherit'] });
    console.log(String(r.stdout ?? '').trim());
    if (r.status !== 0) problems.push(`wgsl-xl --pairs: exit ${r.status}`);
    await upload(out);
  }
} else if (from) {
  await upload(from);
} else { console.log('need --game, --from <dir> or --sm3'); await browser.close(); process.exit(2); }
const census = checkDump(gameRun ? out : from, gameRun || argv.includes('--census'));
if (census) console.log('CENSUS ' + JSON.stringify(census) + ' (floor ' + JSON.stringify(CENSUS) + ')');

await pageCheck();
await browser.close();
if (!result) { console.log('no SHADERCHECK result'); process.exit(1); }
const { failures, ...summary } = result;
console.log('SHADERCHECK ' + JSON.stringify(summary));
for (const f of failures ?? []) console.log('  FAIL ' + f);
for (const p of problems) console.log('  FAIL ' + p);
if (gameRun || from) {
  const dst = gameRun ? out : from;
  fs.writeFileSync(path.join(dst, 'shadercheck.json'), JSON.stringify({ ...result, census, problems }, null, 1));
}
const ok = result.ok && problems.length === 0;
console.log(ok ? 'PASS' : `FAIL (${problems.length} run/dump problem(s)${result.ok ? '' : ', shader check failed'})`);
process.exit(ok ? 0 : 1);
