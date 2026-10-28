// Builds web/test/win32_unit.c for wasm with web/build.sh's target and
// features: runtime/win32.c, win32_ext.c, win32_misc.c and keyboard.c, the
// rest of the runtime stubbed (web/test/win32-wasm.test.mjs runs its "hle"
// half under node, web/tools/waitstress.sh its "stress" half in the browser).
//   node web/tools/win32unit.mjs <out.wasm>
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
const SRC = ['web/test/win32_unit.c', 'runtime/win32.c', 'runtime/win32_ext.c', 'runtime/win32_misc.c', 'runtime/keyboard.c'];
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
    return build(out, [...common, rt('mw/game_mw.c'), rt('mw/registry_mw.c')], ['-DNFSU2_GAME_MW', ...gi]);
  }
  return build(out, [...common, rt('game_ug2.c')], ['-DREGPATH_UG2']);
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const out = process.argv[2];
  if (!out) { console.error('usage: node web/tools/win32unit.mjs <out.wasm>'); process.exit(2); }
  buildWin32Unit(path.resolve(out));
  console.log(`built ${out}`);
}
