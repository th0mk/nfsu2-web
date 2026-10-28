// The D3DX oracle on Most Wanted's effect-creation path, without a boot (TRACKER M2-07):
//   NFSMW_D3DX=<d3dx9_26.dll> node --test web/test/d3dx-oracle.test.mjs
// Needs wasi-sdk in deps/, an oracle build (GAME=mw web/build.sh with gen/mw/d3dx lifted: its objects in
// out/web/mw/obj/d3dx), speed.exe (NFSMW_DATA=<the game folder>, else the ignored mw/ folder of this checkout or of
// the main clone) and Microsoft's DLL, which is never in the repo (GAME.md 5.1): NFSMW_D3DX names it.  Without
// them the test is skipped.  web/test/d3dx_oracle_unit.c loads the oracle as the boot does (the game's install
// answers the 11 math imports natively, d3dxmath_mw.c; module_load_imports: DllMain, the exe's other two d3dx9_26
// slots bound to its exports), then makes the exe's calls: D3DXCreateEffectPool and the 31
// D3DXCreateEffectFromResourceA of the exe's effect table, with a stand-in device.  The game folder and the DLL's
// folder are WASI preopens; the sandboxed game folder is never written (fileio.c).  web/test/d3dx-math.test.mjs
// checks the native math against the oracle.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { CC, buildD3dxOracleUnit } from '../tools/win32unit.mjs';

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
const skip = !CC ? 'no wasi-sdk in deps/' : !DATA ? 'no speed.exe (NFSMW_DATA=<the game folder>)'
  : !DLL || !fs.existsSync(DLL) ? 'NFSMW_D3DX names no d3dx9_26.dll (a local dev oracle, GAME.md 5.1)' : false;

test('the D3DX oracle: DllMain, the exe\'s slots, D3DXCreateEffectPool and all 31 effects from the exe\'s resources', { skip }, (t) => {
  const w = buildD3dxOracleUnit(path.join(OUT, 'd3dx_oracle_unit.wasm'));
  if (!w) { t.skip('no oracle build: GAME=mw web/build.sh with gen/mw/d3dx'); return; }
  const r = spawnSync(process.execPath, [...EXNREF, path.join(REPO, 'web/test/wasi-run.mjs'), w, '/g/speed.exe', `/dx/${path.basename(DLL)}`],
    { encoding: 'utf8', timeout: 300000, env: { ...process.env, WASI_PAGES: '16384', WASI_DIR: `/g=${DATA}`, WASI_DIR2: `/dx=${path.dirname(DLL)}` } });
  const out = r.stdout ?? '', err = r.stderr ?? '';
  if (r.status !== 0) console.log(out + err);
  const m = /d3dx_oracle_unit: (\d+) passed, (\d+) failed/.exec(out);
  assert.ok(m, 'ran to the end');
  assert.equal(+m[2], 0, 'no failed check');
  assert.equal(r.status, 0);
  // the loader: from NFSMW_D3DX, pinned, msvcrt's variable bound, DllMain; of the exe's 13 slots the two effect
  // imports bound to it, the 11 math imports kept on MW's native answers (d3dxmath_mw.c, bound by the game's install)
  assert.match(err, /^\[module\] d3dx9_26\.dll mapped at 0x00d80000-0x00fca000 from \/dx\/\S+ \(sha256 ok, \d+ fixups, 165 imports bound, 1 of them variables\)$/m);
  assert.match(err, /^\[module\] d3dx9_26\.dll: DllMain\(PROCESS_ATTACH\) -> 1 \(load 1\)$/m);
  assert.match(err, /^\[d3dx\] 11 of 11 D3DX math imports answered natively$/m);
  assert.match(err, /^\[module\] d3dx9_26\.dll: 2 of the exe's 13 imports of it bound to its exports, 11 kept on the game's own answers$/m);
  // the calls the boot makes
  assert.match(out, /^T D3DXCreateEffectPool -> 0x00000000, pool 0x0300[0-9a-f]{4}$/m, 'the pool on the oracle\'s heap');
  const made = [...out.matchAll(/^T D3DXCreateEffectFromResourceA\("(IDI_\w+_FX)"\) -> 0x00000000, effect set, errors NULL/gm)].map(x => x[1]);
  assert.equal(new Set(made).size, 31, '31 distinct effects, each S_OK');
  // every shader of the 31 made on the device: 71 vertex and 71 pixel shaders (GAME.md 5.3's census)
  assert.match(out, /^T 31 of 31 effects created; 71 vertex and 71 pixel shaders; 0 traps; 0 unresolved indirect calls$/m);
  // the msvcrt imports this path calls, all answered; nothing unimplemented anywhere
  const crt = [...err.matchAll(/^\[msvcrt\] (\S+): first call/gm)].map(x => x[1]).sort();
  assert.deepEqual(crt, ['??2@YAPAXI@Z', '??3@YAXPAX@Z', '_initterm', '_stricmp', 'malloc', 'qsort'].sort(), `msvcrt on the path: ${crt}`);
  assert.doesNotMatch(out + err, /not implemented|UNIMPLEMENTED/);
});
