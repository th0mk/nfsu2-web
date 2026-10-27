// The lifted DLLs' loader and their own heaps, built for wasm:
//   node --test web/test/module-wasm.test.mjs        (needs wasi-sdk in deps/)
// web/test/module_unit.c: runtime/module.c and runtime/modheap.c (with
// runtime/memmap.c's layout check), the test playing the lifted code.  With
// MW's map it loads two DLLs, server.dll and d3dx9_26.dll, each in its slot
// and with its own heap, apart from the other's; with UG2's, server.dll with
// its two heaps.  In both, a server.dll with one byte changed is refused.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const WASI = fs.readdirSync(path.join(REPO, 'deps')).filter(d => d.startsWith('wasi-sdk-')).map(d => path.join(REPO, 'deps', d)).pop();
const CC = WASI && path.join(WASI, 'bin/clang');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
// web/build.sh's target, features and defines
const FLAGS = ['--target=wasm32-wasip1-threads', '-pthread', '-O2', '-msimd128', '-mbulk-memory', '-mnontrapping-fptoint',
  '-mtail-call', '-mexception-handling', '-mllvm', '-wasm-enable-sjlj', '-mllvm', '-wasm-use-legacy-eh=false',
  '-D_WASI_EMULATED_MMAN', '-D_WASI_EMULATED_SIGNAL', '-D_WASI_EMULATED_PROCESS_CLOCKS', '-fno-delete-null-pointer-checks',
  '-include', 'runtime/web/compat/wasm_plat.h', '-Iruntime/web/compat', '-Iruntime', '-Iruntime/web'];
// the guest's addresses are linear memory: the program's own data starts above the
// slots and the module heaps' first pages (wasi-run.mjs gives 64 MB)
const LINK = ['-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=67108864,--max-memory=268435456',
  '-Wl,--global-base=54525952,--no-stack-first', '-Wl,-z,stack-size=1048576',
  '-lsetjmp', '-lwasi-emulated-mman', '-lwasi-emulated-signal', '-lwasi-emulated-process-clocks'];
const skip = !CC && 'no wasi-sdk in deps/';

function build(name, defs) {
  const w = path.join(OUT, `module_unit_${name}.wasm`);
  execFileSync(CC, [...FLAGS, ...defs, '-o', w, 'web/test/module_unit.c', 'runtime/module.c', 'runtime/modheap.c', 'runtime/memmap.c',
    'runtime/mem.c', 'runtime/web/sha256.c', 'runtime/web/thread_exit.c', ...LINK], { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  return w;
}
function run(w) {
  const dir = fs.mkdtempSync(path.join(OUT, 'module-unit-'));
  try {
    const r = spawnSync(process.execPath, [path.join(REPO, 'web/test/wasi-run.mjs'), w, '/t'],
      { encoding: 'utf8', env: { ...process.env, WASI_DIR: `/t=${dir}` } });
    return { status: r.status, out: r.stdout ?? '', err: r.stderr ?? '' };
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
}
// the pin (module.c): the test's server.dll with one byte changed is not the file its ModInfo names
const REFUSED = /\[module\] server\.dll in the game folder \(\/t\/server\.dll, 16384 bytes, sha256 [0-9a-f]{64}\) is not the file this build was lifted from \(sha256 [0-9a-f]{64}, 16384 bytes\): it is NOT loaded/;
function counts(r) {
  if (r.status !== 0) console.log(r.out + r.err);
  const m = /module_unit: (\d+) passed, (\d+) failed/.exec(r.out);
  assert.ok(m, 'module_unit ran to the end');
  assert.equal(+m[2], 0, 'no failed check');
  return +m[1];
}

test('module_unit, MW\'s map: two DLLs loaded, each heap in its own part of the modheap region', { skip }, () => {
  const r = run(build('mw', ['-DNFSU2_GAME_MW']));
  assert.ok(counts(r) >= 59, 'every check ran');
  assert.equal(r.status, 0);
  // the loader's own lines: both mapped at their slots, from the files the test wrote
  assert.match(r.err, /\[module\] server\.dll mapped at 0x00d00000-0x00d04000 from \/t\/server\.dll \(sha256 ok, 1 fixups, 3 imports bound\)/);
  assert.match(r.err, /\[module\] d3dx9_26\.dll mapped at 0x00d80000-0x00d84000 from \/t\/d3dx9_26\.dll \(sha256 ok, 1 fixups, 3 imports bound\)/);
  assert.match(r.err, /^\[mem\] mw: image 0x00400000-0x00a79000, server\.dll 0x00d00000-0x00d6f000, d3dx9_26\.dll 0x00d80000-0x00fca000, /m);
  // the heaps: server.dll's in 0x01000000-0x03000000, the oracle's in 0x03000000-0x05000000
  assert.match(r.out, /server\.dll heap 0x01000000, its part 0x01000000-0x03000000/);
  assert.match(r.out, /d3dx9_26\.dll heap 0x03000000, its part 0x03000000-0x05000000/);
  assert.match(r.err, /\[modheap\] HeapFree\(0x03000000, 0x0100[0-9a-f]{4}\): not a live block \(foreign\)/);
  assert.match(r.err, /^\[modheap\] HeapDestroy\(0x01000000\): /m);
  assert.match(r.err, /^\[modheap\] HeapDestroy\(0x03000000\): /m);
  assert.match(r.err, REFUSED, 'server.dll with one byte changed is refused');
});

test('module_unit, UG2\'s map: server.dll with its two heaps, as before', { skip }, () => {
  const r = run(build('ug2', []));
  assert.ok(counts(r) >= 32, 'every check ran');
  assert.match(r.err, /\[module\] server\.dll mapped at 0x00a00000-0x00a04000 from \/t\/server\.dll/);
  assert.match(r.out, /server\.dll heap 0x01000000, its part 0x01000000-0x05000000/);
  assert.match(r.err, /\[modheap\] HeapCreate: no free module heap \(2 in use\)/);
  assert.match(r.err, REFUSED, 'server.dll with one byte changed is refused');
});
