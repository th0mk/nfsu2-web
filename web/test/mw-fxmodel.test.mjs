// Most Wanted's effect model (docs/mw/fx-model.md), the base of the ID3DXEffect HLE: the two implementations
// written apart from the design, runtime/mw/fxmodel.c with tools/mw/fxmodeldump.c (A) and tools/fxmodel.py (B), give
// the same model dump of all 31 effects created in one pool, byte for byte and with no UNKNOWN, and the same trace of
// every Set*, Get* and lookup on every node (--ops); A's unit tests (tools/mw/fxmodel_test.c: the state table, the
// conversions, the CTAB match, the pool and its lifetime) and B's self-test and pool test pass; and A and B also agree
// on made-up effect sets (tools/mw/fxdiff_synth.py --model).  The effects come out of speed.exe into out/mw/fx/mtest/
// (ignored: the game's own data) and are removed afterwards.
//   node --test web/test/mw-fxmodel.test.mjs
// NFSMW_DATA is the game folder (default mw/Need For Speed Most Wanted Black Edition in the checkout).  Without
// speed.exe there, clang, or a Python with pefile (.venv, requirements.txt), the test is skipped.  NFSMW_D3DX names
// d3dx9_26.dll for the state-table check against the DLL's own table (fx-model §4.5 a); without it that part is a
// skipped subtest.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const DATA = process.env.NFSMW_DATA || path.join(REPO, 'mw/Need For Speed Most Wanted Black Edition');
const EXE = path.join(DATA, 'speed.exe');
const D3DX = process.env.NFSMW_D3DX || '';
const OUT = path.join(REPO, 'out/mw/fx/mtest');
const RES = path.join(OUT, 'res');
const PY = fs.existsSync(path.join(REPO, '.venv/bin/python')) ? path.join(REPO, '.venv/bin/python') : 'python3';
const haveClang = spawnSync('clang', ['--version'], { encoding: 'utf8' }).status === 0;
const havePefile = spawnSync(PY, ['-I', '-c', 'import pefile'], { encoding: 'utf8' }).status === 0;
const SKIP = !fs.existsSync(EXE) ? 'no speed.exe in the game folder (NFSMW_DATA)' : !haveClang ? 'no clang'
  : !havePefile ? 'no Python with pefile (.venv: pip install -r requirements.txt)' : false;
const CFLAGS = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
  '-fno-sanitize-recover=all', '-Iruntime/mw'];
test.after(() => fs.rmSync(OUT, { recursive: true, force: true }));

const run = (cmd, args, env) => execFileSync(cmd, args, { cwd: REPO, encoding: 'utf8', maxBuffer: 1 << 28,
  env: { ...process.env, NFSMW_D3DX: '', ...env } });
/** where two texts part: the first line that differs */
function firstDiff(a, b) {
  const x = a.split('\n'), y = b.split('\n');
  for (let i = 0; ; i++) if (x[i] !== y[i]) return `line ${i + 1}\n  A: ${x[i]}\n  B: ${y[i]}`;
}
const count = (text, re) => (text.match(re) || []).length;

let built = false, files = [];
/** the 31 effects and A's two tools, once */
function build() {
  if (built) return;
  fs.rmSync(OUT, { recursive: true, force: true });
  fs.mkdirSync(OUT, { recursive: true });
  run(PY, ['-I', 'tools/fxres.py', EXE, RES]);
  const model = ['runtime/mw/fxmodel.c', 'runtime/mw/fx2.c'];
  run('clang', [...CFLAGS, '-o', path.join(OUT, 'fxmodeldump'), 'tools/mw/fxmodeldump.c', ...model]);
  run('clang', [...CFLAGS, '-o', path.join(OUT, 'fxmodel_test'), 'tools/mw/fxmodel_test.c', ...model]);
  files = fs.readdirSync(RES).filter(f => f.endsWith('.fxo')).sort().map(f => path.join(RES, f));
  assert.equal(files.length, 31);
  built = true;
}

test('mw-fxmodel: A and B model all 31 effects alike, dump and Set/Get trace, with no UNKNOWN',
  { skip: SKIP, timeout: 300_000 }, () => {
    build();
    const b = path.join(OUT, 'b');
    const da = run(path.join(OUT, 'fxmodeldump'), ['-', ...files]);
    run(PY, ['-I', 'tools/fxmodel.py', RES, b]);
    const db = fs.readFileSync(path.join(b, 'all.txt'), 'utf8');
    assert.ok(da === db, `the model dumps differ at ${da === db ? '' : firstDiff(da, db)}`);
    assert.equal(count(da, /UNKNOWN\(/g), 0, 'UNKNOWN in the model dump');
    // fx-model §7's counts for the 31 effects
    assert.equal(count(da, /^effect \d+ /gm), 31);
    assert.equal(count(da, /^ +const /gm), 1355);
    assert.equal(count(da, /^ {2}slot /gm), 20);
    assert.equal(count(da, /^ {4}holder /gm), 156);
    assert.equal(count(da, /^ {4}apart /gm), 9);

    const oa = run(path.join(OUT, 'fxmodeldump'), ['--ops', '-', ...files]);
    run(PY, ['-I', 'tools/fxmodel.py', '--ops', RES, b]);
    const ob = fs.readFileSync(path.join(b, 'ops.txt'), 'utf8');
    assert.ok(oa === ob, `the Set/Get/lookup traces differ at ${oa === ob ? '' : firstDiff(oa, ob)}`);
    assert.match(oa, /^fxmodel ops 1\neffects 31\n/);
    assert.ok(oa.endsWith('end\n'));
  });

test('mw-fxmodel: A\'s unit tests: state table, conversions, lookups, CTAB, pool and lifetime',
  { skip: SKIP, timeout: 300_000 }, () => {
    build();
    const out = run(path.join(OUT, 'fxmodel_test'), ['--mw', ...files]);
    assert.match(out, /^fxmodel_test: \d+ checks, 0 failed$/m);
    assert.match(out, /^pool: 20 live slots, 156 holders, 9 apart; every holder wrote, every holder read it back$/m);
    assert.match(out, /^rows: the effects use 46 distinct state rows/m);
  });

test('mw-fxmodel: the state table against d3dx9_26.dll\'s own table',
  { skip: SKIP || (!D3DX && 'NFSMW_D3DX is not set (the path of d3dx9_26.dll)'), timeout: 300_000 }, () => {
    build();
    const out = run(path.join(OUT, 'fxmodel_test'), [], { NFSMW_D3DX: D3DX });
    assert.match(out, /^dll: .* matches game\.conf; 4 copies of the 182-row table, every row agrees/m);
    assert.match(out, /^fxmodel_test: \d+ checks, 0 failed$/m);
  });

test('mw-fxmodel: B\'s self-test and pool test', { skip: SKIP, timeout: 300_000 }, () => {
  build();
  assert.match(run(PY, ['-I', 'tools/fxmodel.py', '--self-test', RES]), /^fxmodel --self-test: \d+ checks, 0 failed/m);
  assert.match(run(PY, ['-I', 'tools/fxmodel.py', '--pool-test', RES]), /^fxmodel --pool-test: \d+ checks, 0 failed$/m);
});

test('mw-fxmodel: A and B agree on made-up effect sets', { skip: SKIP, timeout: 300_000 }, () => {
  build();
  const out = run(PY, ['-I', 'tools/mw/fxdiff_synth.py', '--model', REPO, '300', '17'],
    { FXMODELDUMP: path.join(OUT, 'fxmodeldump') });
  assert.match(out, /^same output +300$/m);
});
