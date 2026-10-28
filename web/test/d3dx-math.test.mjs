// Most Wanted's D3DX math, native (runtime/mw/d3dxmath_mw.c, TRACKER M2-08, PLAN D-10):
//   node --test web/test/d3dx-math.test.mjs                                  (needs wasi-sdk in deps/)
//   NFSMW_D3DX=<d3dx9_26.dll> node --test web/test/d3dx-math.test.mjs        (and the oracle comparison)
// The eleven math functions speed.exe imports from d3dx9_26.dll, each answered natively.
//   - web/test/d3dxmath_unit.c, no game files: known answers, and the cases of web/test/d3dxmath_cases.h (12,000 per
//     function: ordinary values, special ones, random bits, NaN-dense ones; aliased, overlapping and misaligned
//     pointers; the x87 ones under several control words) with the digest of the answers.
//   - web/test/d3dx_oracle_unit.c "math", where the oracle is (a dev oracle build, speed.exe and Microsoft's DLL, as
//     web/test/d3dx-oracle.test.mjs): the same cases through the oracle's export (its lifted kernels) and through
//     speed.exe's IAT slot (the thunk, then the native answer): every case the same, window and eax (L-046); and
//     NFSMW_D3DX_MATH=oracle, which leaves the math to the oracle for an A/B run.
// DIGESTS are the digests of a run in which the oracle comparison found no difference: equal digests in the first
// test carry that comparison to a build without the oracle.  A change to the cases or the answers changes them;
// record them again only from an oracle run with 0 differences.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { CC, buildD3dxMathUnit, buildD3dxOracleUnit } from '../tools/win32unit.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.join(REPO, 'out/web/test');
const EXNREF = spawnSync(process.execPath, ['--experimental-wasm-exnref', '-e', '0']).status === 0 ? ['--experimental-wasm-exnref'] : [];
const mainClone = () => {
  const r = spawnSync('git', ['-C', REPO, 'rev-parse', '--path-format=absolute', '--git-common-dir'], { encoding: 'utf8' });
  return r.status === 0 ? path.dirname(r.stdout.trim()) : REPO;
};
const DATA = process.env.NFSMW_DATA || [REPO, mainClone()].map(d => path.join(d, 'mw/Need For Speed Most Wanted Black Edition'))
  .find(d => fs.existsSync(path.join(d, 'speed.exe')));
const DLL = process.env.NFSMW_D3DX;

const DIGESTS = {
  D3DXMatrixMultiply: '7e6a1c0b580c71a5',
  D3DXVec4Transform: '8dcb4dbd36e87efc',
  D3DXVec3Transform: '26847da6aafa58a0',
  D3DXMatrixInverse: 'd01366ec0a2163e5',
  D3DXMatrixOrthoLH: 'f262d5989999cece',
  D3DXMatrixPerspectiveLH: 'a7cdb070a80a55dd',
  D3DXMatrixTranslation: '05f31f3d64cc1085',
  D3DXVec3TransformCoordArray: 'b894fef85be62fa1',
  D3DXVec3Normalize: 'e151f4cd9aaa7cd9',
  D3DXVec3TransformNormal: '8b6a9a8d3dcde41f',
  D3DXMatrixTranspose: 'bf249810ff0e2f9e',
};
const run = (w, args, env = {}) => {
  const r = spawnSync(process.execPath, [...EXNREF, path.join(REPO, 'web/test/wasi-run.mjs'), w, ...args],
    { encoding: 'utf8', timeout: 600000, maxBuffer: 1 << 26, env: { ...process.env, WASI_PAGES: '16384', ...env } });
  return { status: r.status, out: r.stdout ?? '', err: r.stderr ?? '' };
};
const digests = out => Object.fromEntries([...out.matchAll(/^T math (\w+): 12000 cases, .*digest ([0-9a-f]{16})$/gm)].map(m => [m[1], m[2]]));

test('the native D3DX math alone: known answers, stdcall on every case, the oracle-proven digests', { skip: !CC && 'no wasi-sdk in deps/' }, () => {
  const r = run(buildD3dxMathUnit(path.join(OUT, 'd3dxmath_unit.wasm')), []);
  if (r.status !== 0) console.log(r.out + r.err);
  const m = /d3dxmath_unit: (\d+) passed, (\d+) failed/.exec(r.out);
  assert.ok(m, 'ran to the end');
  assert.equal(+m[2], 0, 'no failed check');
  assert.ok(+m[1] >= 42, `every check ran (${m[1]})`);
  // a patch the dispatch table does not take leaves its slot on IMP_MAGIC + index, not on its own thunk
  assert.match(r.out, /^T binding: 10 of 11 answered, 10 patches taken, Normalize's slot 0xf0000108$/m);
  assert.match(r.err, /^\[d3dx\] D3DXVec3Normalize: the dispatch table took no patch at 0x007e7d9a, its import left unbound$/m);
  assert.equal(r.status, 0);
  assert.deepEqual(digests(r.out), DIGESTS);
});

const oracleSkip = !CC ? 'no wasi-sdk in deps/' : !DATA ? 'no speed.exe (NFSMW_DATA=<the game folder>)'
  : !DLL || !fs.existsSync(DLL) ? 'NFSMW_D3DX names no d3dx9_26.dll (a local dev oracle, GAME.md 5.1)' : false;
let unit;
const oracleUnit = () => unit ??= buildD3dxOracleUnit(path.join(OUT, 'd3dx_oracle_unit_math.wasm'));
const runOracle = (...args) => run(oracleUnit(), ['/g/speed.exe', `/dx/${path.basename(DLL)}`, ...args],
  { WASI_DIR: `/g=${DATA}`, WASI_DIR2: `/dx=${path.dirname(DLL)}` });
test('the native D3DX math against the oracle: 12,000 cases per function, 0 differences', { skip: oracleSkip }, (t) => {
  if (!oracleUnit()) { t.skip('no oracle build: GAME=mw web/build.sh with gen/mw/d3dx'); return; }
  const r = runOracle('math');
  if (r.status !== 0) console.log(r.out + r.err);
  const m = /d3dx_oracle_unit: (\d+) passed, (\d+) failed/.exec(r.out);
  assert.ok(m, 'ran to the end');
  assert.equal(+m[2], 0, 'no failed check');
  assert.equal(r.status, 0);
  // the binding: the game's install answers the 11 natively, the oracle keeps the two effect imports
  assert.match(r.err, /^\[d3dx\] 11 of 11 D3DX math imports answered natively$/m);
  assert.match(r.err, /^\[module\] d3dx9_26\.dll: 2 of the exe's 13 imports of it bound to its exports, 11 kept on the game's own answers$/m);
  // the oracle's CPU table: no 3DNow!, SSE (IsProcessorFeaturePresent 7, then 6), so its SSE kernels
  assert.match(r.err, /IsProcessorFeaturePresent\(7\) -> 0[\s\S]*IsProcessorFeaturePresent\(6\) -> 1/);
  const lines = [...r.out.matchAll(/^T math (\w+): (\d+) cases, (\d+) differ, (\d+) with registers off, (\d+) passes by the NaN rule, digest ([0-9a-f]{16})$/gm)];
  assert.equal(lines.length, 11, 'all eleven functions');
  for (const [, name, n, differ, regs] of lines) {
    assert.ok(+n >= 10000, `${name}: ${n} cases (L-046: at least 10,000)`);
    assert.equal(+differ, 0, `${name}: native and oracle the same`);
    assert.equal(+regs, 0, `${name}: stdcall, the caller's registers kept`);
  }
  // the lane-by-lane pass ran where a NaN came out (the SSE kernels but the inverse, which only has that pass)
  for (const [, name, , , , exact] of lines)
    if (!/Inverse|OrthoLH|PerspectiveLH|Translation|Transpose/.test(name)) assert.ok(+exact > 0, `${name}: NaN cases reached the NaN rule`);
  assert.deepEqual(digests(r.out), DIGESTS, 'the digests the first test holds the native math to');
});

test('NFSMW_D3DX_MATH=oracle in an oracle build: the math left to the oracle (A/B)', { skip: oracleSkip }, (t) => {
  if (!oracleUnit()) { t.skip('no oracle build: GAME=mw web/build.sh with gen/mw/d3dx'); return; }
  const r = runOracle('boot', 'NFSMW_D3DX_MATH=oracle');
  if (r.status !== 0) console.log(r.out + r.err);
  assert.match(r.out, /d3dx_oracle_unit: \d+ passed, 0 failed/);
  assert.match(r.err, /^\[d3dx\] NFSMW_D3DX_MATH=oracle: the 11 math imports are left to the D3DX oracle$/m);
  assert.match(r.err, /^\[module\] d3dx9_26\.dll: 13 of the exe's 13 imports of it bound to its exports$/m);
});
