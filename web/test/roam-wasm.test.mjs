// The free-roam core and the browser's platform layer, built for wasm:
//   node --test web/test/roam-wasm.test.mjs        (needs wasi-sdk in deps/)
// 1. tests/roam_unit.c (the Mac's 119 checks: wire format, sessions over a fake
//    network, hostile datagrams, interpolation, and the platform layer's guard
//    and peer list) against runtime/roam_core.c + runtime/web/roam_web.c;
// 2. tests/roam_selftest.c's byte stream with the game build's code
//    generation: it must be the Mac's and x86's (37,880 bytes, FNV-1a 74ddeef6);
// 3. the pose log's buffer (web/test/poselog_unit.c).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const WASI = fs.readdirSync(path.join(REPO, 'deps')).filter(d => d.startsWith('wasi-sdk-')).map(d => path.join(REPO, 'deps', d)).pop();
const CC = WASI && path.join(WASI, 'bin/clang');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
// the game build's target and code generation (web/build.sh), roam_core with -ffp-contract=off as there
const FLAGS = ['--target=wasm32-wasip1-threads', '-pthread', '-O3', '-msimd128', '-mbulk-memory', '-mnontrapping-fptoint',
  '-mtail-call', '-ffp-contract=off', '-Iruntime', '-Iruntime/web'];
const LINK = ['-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=67108864,--max-memory=268435456', '-Wl,-z,stack-size=8388608'];   // an 8 MB stack as on the Mac: roam_unit keeps sessions on it
const cc = (out, srcs) => execFileSync(CC, [...FLAGS, '-o', out, ...srcs, ...LINK], { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
const run = (wasm, ...args) => execFileSync(process.execPath, [path.join(REPO, 'web/test/wasi-run.mjs'), wasm, ...args], { encoding: 'utf8', timeout: 120_000 });

test('roam_unit on wasm (core + web platform layer)', { skip: !CC && 'no wasi-sdk in deps/' }, () => {
  const w = path.join(OUT, 'roam_unit.wasm');
  cc(w, ['tests/roam_unit.c', 'runtime/roam_core.c', 'runtime/web/roam_web.c']);
  const out = run(w);
  const m = /roam_unit: (\d+) passed, (\d+) failed/.exec(out);
  console.log(out.trim().split('\n').filter(l => /FAIL|roam_unit/.test(l)).join('\n'));
  assert.ok(m, 'roam_unit ran to the end');
  assert.equal(+m[2], 0, 'no failed check');
  assert.ok(+m[1] >= 119, 'every check ran');
});

test('the pose log flushes only every 240 frames (roam_host.h buffer)', { skip: !CC && 'no wasi-sdk in deps/' }, () => {
  const w = path.join(OUT, 'poselog_unit.wasm');
  execFileSync(CC, ['--target=wasm32-wasip1-threads', '-pthread', '-O2', '-include', 'runtime/web/compat/wasm_plat.h',
    '-Iruntime/web/compat', '-Iruntime', '-o', w, 'web/test/poselog_unit.c', ...LINK], { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  const dir = fs.mkdtempSync(path.join(OUT, 'poselog-'));
  try {
    const out = execFileSync(process.execPath, [path.join(REPO, 'web/test/wasi-run.mjs'), w],
      { encoding: 'utf8', env: { ...process.env, WASI_DIR: `/tmp=${dir}` }, timeout: 120_000 });
    console.log(out.trim());
    assert.match(out, /poselog_unit: passed/);
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});

test('roam_selftest bytes on wasm = Mac = x86', { skip: !CC && 'no wasi-sdk in deps/' }, () => {
  const w = path.join(OUT, 'roam_selftest.wasm');
  cc(w, ['web/test/roam_selftest_main.c', 'tests/roam_selftest.c']);
  const out = run(w).trim();
  console.log(out);
  assert.equal(out, 'roam_selftest: 37880 bytes, FNV-1a 74ddeef6');
});
