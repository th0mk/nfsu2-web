// Builds web/test/win32_unit.c for wasm with web/build.sh's target and
// features: runtime/win32.c, win32_ext.c, win32_misc.c and keyboard.c (and Most
// Wanted's thread table, runtime/mw/threads_mw.c), the rest of the runtime stubbed (web/test/win32-wasm.test.mjs runs its "hle"
// half under node, web/tools/waitstress.sh its "stress" half in the browser).  Also web/test/wsaov_unit.c (buildWsaovUnit).
//   node web/tools/win32unit.mjs <out.wasm>            (win32_unit)
//   node web/tools/win32unit.mjs --wsaov <out.wasm>    (wsaov_unit)
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const WASI = fs.readdirSync(path.join(REPO, 'deps')).filter(d => d.startsWith('wasi-sdk-')).map(d => path.join(REPO, 'deps', d)).pop();
export const CC = WASI && path.join(WASI, 'bin/clang');
const FLAGS = ['--target=wasm32-wasip1-threads', '-pthread', '-O2', '-msimd128', '-mbulk-memory', '-mnontrapping-fptoint',
  '-mtail-call', '-mexception-handling', '-mllvm', '-wasm-enable-sjlj', '-mllvm', '-wasm-use-legacy-eh=false',
  '-D_WASI_EMULATED_MMAN', '-D_WASI_EMULATED_SIGNAL', '-D_WASI_EMULATED_PROCESS_CLOCKS', '-fno-delete-null-pointer-checks',
  '-include', 'runtime/web/compat/wasm_plat.h', '-Iruntime/web/compat', '-Iruntime', '-Iruntime/web', '-DNFS_ENHANCED=0'];
// the guest's addresses are linear memory below the program's own data (52 MB)
const LINK = ['-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=67108864,--max-memory=4294967296',
  '-Wl,--global-base=54525952,--no-stack-first', '-Wl,-z,stack-size=1048576',
  '-lsetjmp', '-lwasi-emulated-mman', '-lwasi-emulated-signal', '-lwasi-emulated-process-clocks'];
const SRC = ['web/test/win32_unit.c', 'runtime/win32.c', 'runtime/win32_ext.c', 'runtime/win32_misc.c', 'runtime/keyboard.c',
  'runtime/mw/threads_mw.c'];
// pthread_exit's trampoline, compiled as web/build.sh compiles it: target and features only.  The force-include
// renames pthread_create to nfsu2_pthread_create, so with it the trampoline's own call is to itself, an endless
// loop: the first thread the "stress" half started never came back (web/tools/waitstress.sh timed out)
const THREAD_EXIT = 'runtime/web/thread_exit.c';
const PLAIN = FLAGS.slice(0, FLAGS.indexOf('-D_WASI_EMULATED_MMAN'));

function build(out, src, defs) {
  if (!CC) throw new Error('no wasi-sdk in deps/');
  fs.mkdirSync(path.dirname(out), { recursive: true });
  const opt = { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }, te = `${out}.thread_exit.o`;
  execFileSync(CC, [...PLAIN, '-c', '-o', te, THREAD_EXIT], opt);
  try { execFileSync(CC, [...FLAGS, ...defs, '-o', out, ...src, te, ...LINK], opt); } finally { fs.rmSync(te, { force: true }); }
  return out;
}
// defs: more -D flags (['-DWIN32_UNIT_UG2']: the program with UG2's game table, its "ug2" half)
export function buildWin32Unit(out, defs = []) { return build(out, SRC, defs); }

// web/test/regpath_unit.c: the four files and fileio.c with a game's real table, as web/build.sh builds that game
// (Most Wanted: -DNFSU2_GAME_MW, its lift's gameinfo.h when gen/mw/all is here; UG2: -DREGPATH_UG2).  runtime is
// the folder the runtime's files come from: another one builds the same program on an older runtime (its files
// include their own headers first; the program itself reads no game table)
export function buildRegpathUnit(out, game, runtime = 'runtime') {
  const rt = f => path.join(runtime, f);
  const common = ['web/test/regpath_unit.c', ...['win32.c', 'win32_ext.c', 'win32_misc.c', 'keyboard.c', 'fileio.c'].map(rt)];
  if (game === 'mw') {
    const gi = fs.existsSync(path.join(REPO, 'gen/mw/all/gameinfo.h')) ? ['-Igen/mw/all'] : [];
    // the table's other files, where this runtime has them (fe_mw.c, threads_mw.c, msvcrt_mw.c, d3dxmath_mw.c, sound_mw.c: TRACKER
    // M2-18, M2-20, M2-07, M2-08, M3-12)
    const more = ['mw/fe_mw.c', 'mw/threads_mw.c', 'mw/msvcrt_mw.c', 'mw/d3dxmath_mw.c', 'mw/sound_mw.c'].filter(f => fs.existsSync(path.resolve(REPO, rt(f)))).map(rt);
    return build(out, [...common, rt('mw/game_mw.c'), rt('mw/registry_mw.c'), ...more], ['-DNFSU2_GAME_MW', ...gi]);
  }
  return build(out, [...common, rt('game_ug2.c')], ['-DREGPATH_UG2']);
}

// web/test/msvcrt_unit.c: Most Wanted's msvcrt.dll (runtime/mw/msvcrt_mw.c) with the four files, module.c and
// modheap.c and MW's real table with the D3DX oracle's entry, as GAME=mw web/build.sh builds an oracle build.  Its
// guest addresses go up to the oracle's heap (0x05000000) and the arrays at 0x06000000, so the program's own data
// starts where the game module's does (0x28000000; run it with WASI_PAGES=16384, 1 GB)
const LINK_BIG = ['-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=1073741824,--max-memory=4294967296',
  '-Wl,--global-base=671088640,--no-stack-first', '-Wl,-z,stack-size=1048576',
  '-lsetjmp', '-lwasi-emulated-mman', '-lwasi-emulated-signal', '-lwasi-emulated-process-clocks'];
export function buildMsvcrtUnit(out) {
  if (!CC) throw new Error('no wasi-sdk in deps/');
  fs.mkdirSync(path.dirname(out), { recursive: true });
  const opt = { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }, te = `${out}.thread_exit.o`;
  const gi = fs.existsSync(path.join(REPO, 'gen/mw/all/gameinfo.h')) ? ['-Igen/mw/all'] : [];
  const src = ['web/test/msvcrt_unit.c', ...['win32.c', 'win32_ext.c', 'win32_misc.c', 'keyboard.c', 'module.c', 'modheap.c',
    'web/sha256.c', 'mw/game_mw.c', 'mw/registry_mw.c', 'mw/fe_mw.c', 'mw/threads_mw.c', 'mw/msvcrt_mw.c', 'mw/d3dxmath_mw.c', 'mw/sound_mw.c']
    .map(f => `runtime/${f}`)];
  execFileSync(CC, [...PLAIN, '-c', '-o', te, THREAD_EXIT], opt);
  try { execFileSync(CC, [...FLAGS, '-DNFSU2_GAME_MW', '-DNFSU2_D3DX_ORACLE=1', ...gi, '-o', out, ...src, te, ...LINK_BIG], opt); }
  finally { fs.rmSync(te, { force: true }); }
  return out;
}

// web/test/d3dxmath_unit.c: Most Wanted's native D3DX math (runtime/mw/d3dxmath_mw.c) alone, no game files and no
// oracle; guest addresses as buildMsvcrtUnit's (run it with WASI_PAGES=16384)
export function buildD3dxMathUnit(out) {
  if (!CC) throw new Error('no wasi-sdk in deps/');
  fs.mkdirSync(path.dirname(out), { recursive: true });
  const opt = { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }, te = `${out}.thread_exit.o`;
  const gi = fs.existsSync(path.join(REPO, 'gen/mw/all/gameinfo.h')) ? ['-Igen/mw/all'] : [];
  execFileSync(CC, [...PLAIN, '-c', '-o', te, THREAD_EXIT], opt);
  try { execFileSync(CC, [...FLAGS, '-DNFSU2_GAME_MW', ...gi, '-o', out, 'web/test/d3dxmath_unit.c', 'runtime/mw/d3dxmath_mw.c', te, ...LINK_BIG], opt); }
  finally { fs.rmSync(te, { force: true }); }
  return out;
}

// web/test/d3dx_oracle_unit.c: the D3DX oracle on its effect-creation path, with the runtime's own files and the
// oracle's objects GAME=mw web/build.sh compiled (out/web/mw/obj/d3dx, listed in out/web/mw/lift.objs; dev builds
// only).  Returns null when there is no oracle build to link.
export function buildD3dxOracleUnit(out) {
  if (!CC) throw new Error('no wasi-sdk in deps/');
  const list = path.join(REPO, 'out/web/mw/lift.objs');
  if (!fs.existsSync(list) || !fs.existsSync(path.join(REPO, 'gen/mw/all/impargs.c'))) return null;
  const objs = fs.readFileSync(list, 'utf8').split('\n').filter(l => l.startsWith('obj/d3dx/')).map(l => path.join(REPO, 'out/web/mw', l));
  if (!objs.length || !objs.every(f => fs.existsSync(f))) return null;
  fs.mkdirSync(path.dirname(out), { recursive: true });
  const opt = { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }, te = `${out}.thread_exit.o`;
  const src = ['web/test/d3dx_oracle_unit.c', 'gen/mw/all/impargs.c', ...['mem.c', 'res.c', 'fileio.c', 'heap.c', 'dispatch.c',
    'win32.c', 'win32_ext.c', 'win32_misc.c', 'keyboard.c', 'module.c', 'modheap.c', 'web/sha256.c', 'web/tz.c', 'mw/game_mw.c',
    'mw/registry_mw.c', 'mw/fe_mw.c', 'mw/threads_mw.c', 'mw/msvcrt_mw.c', 'mw/d3dxmath_mw.c', 'mw/fx2.c', 'mw/sound_mw.c'].map(f => `runtime/${f}`)];
  execFileSync(CC, [...PLAIN, '-c', '-o', te, THREAD_EXIT], opt);
  try { execFileSync(CC, [...FLAGS, '-DNFSU2_GAME_MW', '-DNFSU2_D3DX_ORACLE=1', '-Igen/mw/all', '-o', out, ...src, ...objs, te, ...LINK_BIG], opt); }
  finally { fs.rmSync(te, { force: true }); }
  return out;
}

// web/test/d3d9_unit.c: the Direct3D 9 device (runtime/com.c) with the renderer and the rest of the runtime stubbed
// in the program; a function of the runtime a tested path does not reach is left out (a trapping stub, so a path
// that reaches one fails loudly).  mw: Most Wanted's answers, its real game table (runtime/mw/game_mw.c, built as
// GAME=mw web/build.sh builds it: -DNFSU2_GAME_MW, the lift's gameinfo.h when gen/mw/all is here), else UG2's.
export function buildD3d9Unit(out, mw) {
  if (!CC) throw new Error('no wasi-sdk in deps/');
  fs.mkdirSync(path.dirname(out), { recursive: true });
  const opt = { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }, te = `${out}.thread_exit.o`;
  const gi = mw && fs.existsSync(path.join(REPO, 'gen/mw/all/gameinfo.h')) ? ['-Igen/mw/all'] : [];
  const game = mw ? ['-DD3D9_UNIT_MW', '-DNFSU2_GAME_MW', ...gi] : [], table = mw ? ['runtime/mw/game_mw.c'] : [];
  execFileSync(CC, [...PLAIN, '-c', '-o', te, THREAD_EXIT], opt);
  try {
    execFileSync(CC, [...FLAGS, ...game, '-o', out, 'web/test/d3d9_unit.c', 'runtime/com.c', ...table, te, ...LINK,
      '-Wl,--unresolved-symbols=ignore-all'], opt);
  } finally { fs.rmSync(te, { force: true }); }
  return out;
}

// web/test/wsaov_unit.c: Most Wanted's overlapped Winsock (TRACKER M8-06, M8-07): runtime/winsock.c over the
// browser's virtual LAN (runtime/web/net_lan.c) with the Win32 layer's objects (the four files); its "hle" half
// runs under node (web/test/wsaov-wasm.test.mjs), its "stress" half in the browser host (web/tools/wsastress.sh)
const WSAOV_SRC = ['web/test/wsaov_unit.c', 'runtime/win32.c', 'runtime/win32_ext.c', 'runtime/win32_misc.c', 'runtime/keyboard.c',
  'runtime/winsock.c', 'runtime/web/net_lan.c'];
export function buildWsaovUnit(out) { return build(out, WSAOV_SRC, []); }

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const wsaov = process.argv[2] === '--wsaov', out = process.argv[wsaov ? 3 : 2];
  if (!out) { console.error('usage: node web/tools/win32unit.mjs [--wsaov] <out.wasm>'); process.exit(2); }
  (wsaov ? buildWsaovUnit : buildWin32Unit)(path.resolve(out));
  console.log(`built ${out}`);
}
