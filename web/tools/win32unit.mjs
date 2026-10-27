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
const SRC = ['web/test/win32_unit.c', 'runtime/win32.c', 'runtime/win32_ext.c', 'runtime/win32_misc.c', 'runtime/keyboard.c',
  'runtime/web/thread_exit.c'];

export function buildWin32Unit(out) {
  if (!CC) throw new Error('no wasi-sdk in deps/');
  fs.mkdirSync(path.dirname(out), { recursive: true });
  execFileSync(CC, [...FLAGS, '-o', out, ...SRC, ...LINK], { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  return out;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const out = process.argv[2];
  if (!out) { console.error('usage: node web/tools/win32unit.mjs <out.wasm>'); process.exit(2); }
  buildWin32Unit(path.resolve(out));
  console.log(`built ${out}`);
}
