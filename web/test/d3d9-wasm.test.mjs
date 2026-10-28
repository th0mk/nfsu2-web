// The Direct3D 9 device's rules (runtime/com.c; TRACKER M3-02, M3-03, M3-04; L-085, L-087, L-088), built for wasm and
// called as lifted code calls a COM method:
//   node --test web/test/d3d9-wasm.test.mjs        (needs wasi-sdk in deps/)
// web/test/d3d9_unit.c links runtime/com.c with the renderer stubbed (it records what com.c hands it) and checks:
//   - lifetimes: the implicit depth surface survives the game's Get + two Releases, the cube survives a Release
//     after each face lookup and while a face is bound, counts live in the guest wrapper, a creator that made
//     nothing answers E_OUTOFMEMORY with a NULL pointer;
//   - state rules: a failed Reset reads as lost (TestCooperativeLevel, Present) until a Reset works, GetLevelCount
//     is a count, the descriptor structs, the mode list, CheckDeviceMultiSampleType;
//   - every primitive type through DrawPrimitiveUP, DrawPrimitive, DrawIndexedPrimitive and DrawIndexedPrimitiveUP
//     against a CPU reference of D3D9's vertex order (the fans with Most Wanted's strides 48, 24 and 32).
// Built twice: with Most Wanted's answers (game.h d3d_modes, d3d9_strict) and with UG2's, which stay as they were.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { CC, buildD3d9Unit } from '../tools/win32unit.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
const skip = !CC && 'no wasi-sdk in deps/';
const EXNREF = spawnSync(process.execPath, ['--experimental-wasm-exnref', '-e', '0']).status === 0 ? ['--experimental-wasm-exnref'] : [];

function run(game) {
  const w = buildD3d9Unit(path.join(OUT, `d3d9_unit_${game}.wasm`), game === 'mw');
  const r = spawnSync(process.execPath, [...EXNREF, path.join(REPO, 'web/test/wasi-run.mjs'), w],
    { encoding: 'utf8', timeout: 120000, env: { ...process.env, WASI_PAGES: '1024' } });
  const out = (r.stdout ?? '') + (r.stderr ?? '');
  assert.doesNotMatch(out, /^FAIL /m, out);
  assert.equal(r.status, 0, out);
  return { out, ok: new Set([...out.matchAll(/^ok (\S+)$/mg)].map(m => m[1])) };
}

const LIFETIMES = ['depth-get', 'depth-double-release', 'depth-same-after', 'cube-create', 'cube-faces-distinct',
  'cube-kept-by-face', 'cube-freed', 'refcount', 'refcount-free', 'stdcall',
  ...['CreateTexture', 'CreateCubeTexture', 'CreateVolumeTexture', 'CreateVertexBuffer', 'CreateIndexBuffer',
    'CreateRenderTarget', 'CreateDepthStencilSurface', 'CreateOffscreenPlainSurface'].map(c => `creator-${c}`)];
const PRIMS = (game) => {
  const all = [];
  for (const s of [48, 24, 32]) all.push(`prim-up-fan-${s}`);
  for (const t of ['points', 'lines', 'linestrip', 'triangles', 'tristrip', 'fan'])
    for (const api of ['up', 'vb', 'indexed', 'indexed-up']) all.push(`prim-${api}-${t}-32`, `prim-${api}-${t}-24`);
  if (game === 'mw') all.push('prim-vb-triangles-32', 'prim-indexed-lines-48', 'prim-up-triangles-32');
  return all;
};

test('d3d9_unit, Most Wanted: lifetimes, D3D9 state rules and answers, every primitive type', { skip }, () => {
  const { out, ok } = run('mw');
  for (const c of [...LIFETIMES, 'creator-gpu-refused', 'reset-failed-lost', 'reset-ok-clears', 'reset-size', 'levelcount',
    'leveldesc', 'leveldesc-past', 'surfacedesc-level', 'bufferdesc', 'gettype', 'modes', 'multisample', ...PRIMS('mw')])
    assert.ok(ok.has(c), `case ${c} passed`);
  assert.match(out, /^d3d9_unit \(mw\): (\d+) passed, 0 failed$/m);
});

test('d3d9_unit, UG2: the same rules, UG2\'s own answers where Most Wanted\'s differ', { skip }, () => {
  const { out, ok } = run('ug2');
  for (const c of [...LIFETIMES, 'creator-gpu-refused-ug2', 'reset-failed-lost', 'reset-ok-clears', 'reset-size-ug2',
    'levelcount', 'leveldesc-ug2', 'modes-ug2', 'multisample-ug2', ...PRIMS('ug2')])
    assert.ok(ok.has(c), `case ${c} passed`);
  assert.match(out, /^d3d9_unit \(ug2\): (\d+) passed, 0 failed$/m);
});
