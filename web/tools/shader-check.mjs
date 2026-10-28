// Validate the WGSL shader set in Chrome (docs/web/gpu-abi.md section 9).
//
//   node web/tools/shader-check.mjs --game [--out out/web/shaders]
//       run the quick race headless with NFSU2_SHADER_DUMP (null renderer),
//       copy the dump from OPFS to --out, add every vertex shader x declaration
//       pair and the CPU-interpreter test vectors (web/tools/wgsl-xl --pairs,
//       built with the Mac's cc), then check all of it
//   node web/tools/shader-check.mjs --from <dir>
//       check a dump on disk (an earlier --game run or web/tools/wgsl-xl output)
//   node web/tools/shader-check.mjs --game mw
//       the game run of that game (--game alone: ug2's); Most Wanted has none yet
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
// --census (with --from: also apply the game-run census below to that dump),
// --script <url path> (game run, default /tests/scripts/quickrace.txt), --env K=V
// (game run, repeatable), --help (this text and the game's run).
// Needs the server (out/web/server-rs/nfsu2-server, started with --dev) on that port and web/dist built.
// Exit code 0 only when every module compiles, every pipeline builds, the
// vertex-pulling and differential tests match, no translation was REJECTED
// (index.txt, pairs.txt) and, for a game run, the game ran the whole quick race
// (EXIT 0, the race reached, "quit at guest frame", no fault marker) and its
// dump has at least the shader census of a full quick race (CENSUS below).
// The dump is game-derived: keep it out of git.
import { chromium } from 'playwright';
import { execFileSync, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { gameOf, usage, nothingFor } from './rig-game.mjs';

const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
const port = +opt('--port', 8612), save = opt('--save', 'shadercheck');
const out = opt('--out', 'out/web/shaders'), from = opt('--from', null);
const timeout = +opt('--timeout', 600000);
const origin = `http://localhost:${port}`;
// the word after --game, unless it is another option, names the game of the run (ug2 without one)
const gameWord = argv[argv.indexOf('--game') + 1];
const GAME = argv.includes('--game') && gameWord !== undefined && !gameWord.startsWith('--') ? gameOf(argv) : 'ug2';
const RUN_OF = { ug2: ['the quick race (--script /tests/scripts/quickrace.txt) to its end; at least the census of a full quick race'] };
const NONE = 'none yet: no Most Wanted script or census floor (M3-06)';
if (argv.includes('--help')) usage(import.meta.url, GAME, 'The game run', RUN_OF[GAME] ?? [], NONE);
if (!RUN_OF[GAME]) nothingFor(GAME, NONE);
await (await import('./dev-server.mjs')).requireDev(origin);

// what a whole quick race creates (gpu-shaders report section 6): distinct
// vertex / pixel shader objects and declarations, registered (shader,
// declaration) vertex modules, pixel shader modules (with the cube twins)
const CENSUS = { vsObjects: 16, psObjects: 24, decls: 11, vsPairs: 17, psModules: 68 };
// --sm3: what the module ABI has no place for (gpu-abi.md section 11.1; runtime/web/vsemu_sm3.inc), which
// psref's unit programs use and the game's shaders do not; and the modules checked at least, by the result's
// field: against psref (ps3diff, vs3diff) today's unit programs (cube twins included; a program psref refuses
// on every input has no vectors and does not count) and Most Wanted's 22 SM3-class blobs (distinct by bytes:
// 17 ps_2_x, 2 ps_3_0, 1 vs_3_0); against ps_ref_run and vs_run (psdiff, diff) its 75 older-model shaders,
// 46 pixel (each with its stage-0 cube twin) and 29 vertex
const ABI_LIMITS = /REJECTED: (MRT|vertex texture fetch|a vs_3_0 without a POSITION0 output|a ps_3_0 input the module ABI has no place for|a vs_3_0 output the module ABI has no place for|a loop count the application sets|a b# the application sets)/;
const SM3_FLOOR = { unit: { ps3diff: 71, vs3diff: 4 }, mw: { ps3diff: 19, vs3diff: 1, psdiff: 92, diff: 29 } };
const problems = [];               // reasons the run fails besides the browser check
// REJECTED translations (but those allow matches) and, for a game dump, the census
function checkDump(dir, census, allow = null) {
  for (const f of ['index.txt', 'pairs.txt']) {
    const p = path.join(dir, f);
    if (!fs.existsSync(p)) continue;
    const rej = fs.readFileSync(p, 'utf8').split('\n').filter(l => /REJECTED/.test(l) && !(allow && allow.test(l)));
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

const browser = await chromium.launch({ channel: 'chrome', headless: !argv.includes('--headed'),
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
      const got = r[k]?.modules ?? 0;
      if (got < min) problems.push(`${s.name}: ${got} modules checked in ${k} < ${min}`);
    }
    fs.writeFileSync(path.join(s.dir, 'shadercheck.json'), JSON.stringify({ ...r, problems }, null, 1));
  }
  await browser.close();
  for (const p of problems) console.log('  FAIL ' + p);
  console.log(problems.length ? `FAIL (${problems.length} problem(s))` : 'PASS');
  process.exit(problems.length ? 1 : 0);
}

if (argv.includes('--game')) {
  // DESIGN.md section 5 suite environment + the dump
  const env = ['NOMOVIES=1', 'SNDSYNC=0', 'GPUVS=1', 'NFSU2_FNG_LOG=1', 'NFSU2_STATUS_EVERY=600', 'NFSU2_DISCORD=0',
    'NFSU2_SHADER_DUMP=/save/shaders', ...argv.flatMap((a, i) => a === '--env' ? [argv[i + 1]] : [])];
  const u = `${origin}/test/hostcheck.html?wasm=/game.wasm&pages=16384&arg=/data/SPEED2.EXE&save=${save}&fresh` +
    `&reg=/tests/web/seed/registry_perf4_native.txt&script=${opt('--script', '/tests/scripts/quickrace.txt')}` +
    env.map(e => `&env=${encodeURIComponent(e)}`).join('');
  console.log('game: ' + u);
  await page.goto(u);
  if (!await waitDone(timeout)) { console.log('TIMEOUT (game run)'); await browser.close(); process.exit(2); }
  console.log('game ' + (lastExit ?? 'no EXIT line'));
  // a crash, a stop before the race or an early exit gives a smaller dump
  // that would otherwise still pass
  const glog = gameLog.join('\n');
  if (lastExit !== 'EXIT 0') problems.push(`game: ${lastExit ?? 'no EXIT line'}`);
  if (!glog.includes('quit at guest frame')) problems.push('game: no "quit at guest frame" (the script did not finish)');
  if (!glog.includes('waitmem [0x008654a4]==6 satisfied')) problems.push('game: the race was not reached');
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
    console.log(execFileSync(xl, ['--pairs', out, out]).toString().trim());
    await upload(out);
  }
} else if (from) {
  await upload(from);
} else { console.log('need --game, --from <dir> or --sm3'); await browser.close(); process.exit(2); }
const census = checkDump(argv.includes('--game') ? out : from, argv.includes('--game') || argv.includes('--census'));
if (census) console.log('CENSUS ' + JSON.stringify(census) + ' (floor ' + JSON.stringify(CENSUS) + ')');

await pageCheck();
await browser.close();
if (!result) { console.log('no SHADERCHECK result'); process.exit(1); }
const { failures, ...summary } = result;
console.log('SHADERCHECK ' + JSON.stringify(summary));
for (const f of failures ?? []) console.log('  FAIL ' + f);
for (const p of problems) console.log('  FAIL ' + p);
if (argv.includes('--game') || from) {
  const dst = from && !argv.includes('--game') ? from : out;
  fs.writeFileSync(path.join(dst, 'shadercheck.json'), JSON.stringify({ ...result, census, problems }, null, 1));
}
const ok = result.ok && problems.length === 0;
console.log(ok ? 'PASS' : `FAIL (${problems.length} run/dump problem(s)${result.ok ? '' : ', shader check failed'})`);
process.exit(ok ? 0 : 1);
