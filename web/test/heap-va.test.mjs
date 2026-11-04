// runtime/heap.c's address space on wasm (web/test/heap_va_unit.c): best fit keeps the long free runs whole, a
// heap gives back the segments it no longer uses, and 60 simulated trips out of a crowded free roam and back never
// run out of room for the track streamer's 60000 KB block; a double free is refused, big blocks leave room for new
// segments, kept holes expire, and under NFSU2_HEAPCHECK a region reserved again reads zero.  Built with the game's
// code generation:
//   node --test web/test/heap-va.test.mjs        (needs wasi-sdk in deps/)
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const WASI = fs.existsSync(path.join(REPO, 'deps')) &&
  fs.readdirSync(path.join(REPO, 'deps')).filter(d => d.startsWith('wasi-sdk-')).map(d => path.join(REPO, 'deps', d)).pop();
const CC = WASI && path.join(WASI, 'bin/clang');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });

test('heap address space: best fit, segments given back, trips never short of the streamer\'s block, double frees refused', { skip: !CC && 'no wasi-sdk in deps/', timeout: 300_000 }, () => {
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
  const run = (args, env) => {
    let out;
    try { out = execFileSync(process.execPath, [path.join(REPO, 'web/tools/wasi-run.mjs'), w, ...args], { encoding: 'utf8', timeout: 240_000, env: { ...process.env, ...env } }); }
    catch (e) { out = (e.stdout ?? '') + (e.stderr ?? ''); }
    console.log(out.trim());
    assert.doesNotMatch(out, /^FAIL /m);
    assert.match(out, /^heap_va_unit: passed/m);
    return out;
  };
  const out = run([], {});
  for (const c of ['bestfit-small-hole', 'bestfit-big-hole', 'bighole-untouched', 'bighole-back', 'racehole-in-hole', 'racehole-rest-untouched', 'racehole-back', 'segments-back', 'segments-pinned', 'segments-reuse', 'segments-empty',
    'trips-no-failure', 'trips-bounded', 'trips-streamer-room', 'dfree-refused', 'dfree-not-live', 'dfree-no-overlap', 'reserve-small-after-big', 'reserve-big-not-held',
    'holes-kept', 'holes-kept-a-while', 'holes-expire', 'holes-compact', 'holes-age']) assert.match(out, new RegExp(`^PASS ${c}$`, 'm'), `${c} passed`);
  const chk = run(['check'], { NFSU2_HEAPCHECK: '1' });
  for (const c of ['check-release-zero', 'check-decommit-zero', 'check-hole-kept']) assert.match(chk, new RegExp(`^PASS ${c}$`, 'm'), `${c} passed`);
});
