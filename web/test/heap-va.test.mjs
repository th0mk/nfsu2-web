// runtime/heap.c's address space on wasm (web/test/heap_va_unit.c): best fit keeps the long free runs whole, a
// heap gives back the segments it no longer uses, and 60 simulated trips out of a crowded free roam and back never
// run out of room for the track streamer's 60000 KB block; with the guest heap's window (a 2 GB memory,
// runtime/guest_span.h) large blocks go into it and nothing into the runtime's memory, and 64 cars' trips have room;
// and the runtime's heap (runtime/web/sbrk.c) steps over the window.  Built with the game's code generation:
//   node --test web/test/heap-va.test.mjs        (needs wasi-sdk in deps/)
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const WASI = fs.existsSync(path.join(REPO, 'deps')) &&
  fs.readdirSync(path.join(REPO, 'deps')).filter(d => d.startsWith('wasi-sdk-')).map(d => path.join(REPO, 'deps', d)).pop();
const CC = WASI && path.join(WASI, 'bin/clang');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
/* a test program under web/tools/wasi-run.mjs, what it wrote (stdout, then stderr); env: more of its environment */
function run(w, env) {
  const NODE_ENV = { ...process.env }; delete NODE_ENV.NFSU2_HEAP_WINDOW; delete NODE_ENV.WASI_PAGES;
  const r = spawnSync(process.execPath, [path.join(REPO, 'web/tools/wasi-run.mjs'), w], { encoding: 'utf8', timeout: 240_000, env: { ...NODE_ENV, ...env } });
  return (r.stdout ?? '') + (r.stderr ?? '');
}

test('heap address space: best fit, segments given back, trips never short of the streamer\'s block', { skip: !CC && 'no wasi-sdk in deps/', timeout: 300_000 }, () => {
  const w = path.join(OUT, 'heap_va_unit.wasm');
  // as web/build.sh: the guest owns [0, 640 MB) of the memory, the runtime's data above it
  execFileSync(CC, ['--target=wasm32-wasip1-threads', '-pthread', '-O2', '-msimd128', '-mbulk-memory', '-mnontrapping-fptoint',
    '-D_WASI_EMULATED_MMAN', '-D_WASI_EMULATED_SIGNAL', '-D_WASI_EMULATED_PROCESS_CLOCKS', '-fno-delete-null-pointer-checks',
    '-include', 'runtime/web/compat/wasm_plat.h', '-Iruntime/web/compat', '-Iruntime',
    '-o', w, 'web/test/heap_va_unit.c', 'runtime/heap.c', 'runtime/mem.c',
    '-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=1073741824,--max-memory=4294967296',
    '-Wl,--global-base=671088640', '-lwasi-emulated-mman', '-lwasi-emulated-signal', '-lwasi-emulated-process-clocks'],
  { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  // web/tools/wasi-run.mjs: a 1 GB memory (the guest's 640 MB and the runtime above), as the game's
  let out = run(w, {});
  console.log(out.trim());
  const trace = /^trace (\w+)$/m.exec(out)?.[1];
  for (const c of ['bestfit-small-hole', 'bestfit-big-hole', 'bighole-untouched', 'bighole-back', 'racehole-in-hole', 'racehole-rest-untouched', 'racehole-back', 'segments-back', 'segments-pinned', 'segments-reuse', 'segments-empty',
    'trips-no-failure', 'trips-bounded', 'trips-streamer-room']) assert.match(out, new RegExp(`^PASS ${c}$`, 'm'), `${c} passed`);
  assert.doesNotMatch(out, /^FAIL /m);
  assert.match(out, /^heap_va_unit: passed/m);
  // asked for, with a memory too small for it (a browser that refused 2 GB): no window, every block where it was
  out = run(w, { NFSU2_HEAP_WINDOW: '1' });
  assert.match(out, /^\[heap\] no window above the runtime: the memory is 1024 MB/m);
  assert.equal(/^trace (\w+)$/m.exec(out)?.[1], trace, 'the same addresses as without the ask');
  assert.match(out, /^heap_va_unit: passed/m);
  // the guest heap's window: a 2 GB memory at the start (runtime/guest_span.h)
  out = run(w, { NFSU2_HEAP_WINDOW: '1', WASI_PAGES: '32768' });
  console.log(out.trim());
  for (const c of ['win-on', 'win-large-high', 'win-segments-low', 'win-committed', 'win-fill', 'win-no-hole', 'win-back',
    'win-hole-nobodys', 'win-query-free', 'wtrips-no-failure', 'wtrips-streamer-room']) assert.match(out, new RegExp(`^PASS ${c}$`, 'm'), `${c} passed`);
  assert.doesNotMatch(out, /^FAIL /m);
  assert.match(out, /^heap_va_unit: passed/m);
});

test('the runtime\'s heap steps over the guest heap\'s window (runtime/web/sbrk.c with dlmalloc)', { skip: !CC && 'no wasi-sdk in deps/', timeout: 300_000 }, () => {
  const w = path.join(OUT, 'sbrk_window_unit.wasm');
  execFileSync(CC, ['--target=wasm32-wasip1-threads', '-pthread', '-O2', '-Iruntime', '-o', w, 'web/test/sbrk_window_unit.c', 'runtime/web/sbrk.c',
    '-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=67108864,--max-memory=4294967296'],
  { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  const out = run(w, { WASI_PAGES: '1024' });
  console.log(out.trim());
  for (const c of ['sbrk-past-window', 'sbrk-none-in-window', 'sbrk-reuse']) assert.match(out, new RegExp(`^PASS ${c}$`, 'm'), `${c} passed`);
  assert.match(out, /^sbrk_window_unit: passed/m);
});
