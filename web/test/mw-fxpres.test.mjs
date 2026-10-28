// Most Wanted's FXLC preshader evaluator (docs/mw/fx-preshader.md): the two implementations written apart from the
// design, runtime/mw/fxpres.c with tools/mw/fxpresrun.c (A) and tools/fxpres.py (B), give the same results byte for
// byte on the interpreter cases of tools/mw/fxpres_cases.py (every opcode, form and operand kind, the loader's
// checks, random programs, rounding corners, every preshader of the 31 effects at x87 precision 24, 53 and 64,
// corrupted copies of them) and on SetValue and typed-setter rounds over all 31 effects in one pool and over copies
// of them whose preshaders compute (the value of every preshader state of every pass, every setter's result); A's
// unit tests (tools/mw/fxpres_test.c), B's self-test and the oracle comparison's self-test
// (tools/mw/fxpres_compare.py) pass.  With NFSMW_D3DX naming d3dx9_26.dll, A is also
// checked against the DLL under Unicorn (tools/mw/fxoracle.sh --quick).  The effects come out of speed.exe into
// out/mw/fx/ptest/ (ignored: the game's own data) and are removed afterwards.
//   node --test web/test/mw-fxpres.test.mjs
// NFSMW_DATA is the game folder (default mw/Need For Speed Most Wanted Black Edition in the checkout).  Without
// speed.exe there, clang, or a Python with pefile (.venv, requirements.txt), the test is skipped; without
// NFSMW_D3DX, or Unicorn (UNICORN_INC and UNICORN_LIB, or ../nsfu2/work/deps/unicorn-2.1.4), the oracle subtest is.
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
const OUT = path.join(REPO, 'out/mw/fx/ptest');
const RES = path.join(OUT, 'res');
const PY = fs.existsSync(path.join(REPO, '.venv/bin/python')) ? path.join(REPO, '.venv/bin/python') : 'python3';
const haveClang = spawnSync('clang', ['--version'], { encoding: 'utf8' }).status === 0;
const havePefile = spawnSync(PY, ['-I', '-c', 'import pefile'], { encoding: 'utf8' }).status === 0;
const SKIP = !fs.existsSync(EXE) ? 'no speed.exe in the game folder (NFSMW_DATA)' : !haveClang ? 'no clang'
  : !havePefile ? 'no Python with pefile (.venv: pip install -r requirements.txt)' : false;
const unicorn = ['../nsfu2/work/deps/unicorn-2.1.4', '../../nsfu2/work/deps/unicorn-2.1.4', '../../work/deps/unicorn-2.1.4']
  .map(d => path.join(REPO, d)).find(d => fs.existsSync(path.join(d, 'lib/libunicorn.a')));
const haveUnicorn = (process.env.UNICORN_INC && process.env.UNICORN_LIB) || unicorn;
const CFLAGS = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
  '-fno-sanitize-recover=all', '-Iruntime/mw'];
test.after(() => fs.rmSync(OUT, { recursive: true, force: true }));

const run = (cmd, args, env) => execFileSync(cmd, args, { cwd: REPO, encoding: 'utf8', maxBuffer: 1 << 28,
  env: { ...process.env, ...env } });
/** where two texts part: the first line that differs */
function firstDiff(a, b) {
  const x = a.split('\n'), y = b.split('\n');
  for (let i = 0; ; i++) if (x[i] !== y[i]) return `line ${i + 1}\n  A: ${x[i]}\n  B: ${y[i]}`;
}

let built = false, files = [];
/** the 31 effects and A's two tools, once */
function build() {
  if (built) return;
  fs.rmSync(OUT, { recursive: true, force: true });
  fs.mkdirSync(OUT, { recursive: true });
  run(PY, ['-I', 'tools/fxres.py', EXE, RES]);
  const src = ['runtime/mw/fxpres.c', 'runtime/mw/fxmodel.c', 'runtime/mw/fx2.c', '-lm'];
  run('clang', [...CFLAGS, '-o', path.join(OUT, 'fxpresrun'), 'tools/mw/fxpresrun.c', ...src]);
  run('clang', [...CFLAGS, '-o', path.join(OUT, 'fxpres_test'), 'tools/mw/fxpres_test.c', ...src]);
  files = fs.readdirSync(RES).filter(f => f.endsWith('.fxo')).sort().map(f => path.join(RES, f));
  assert.equal(files.length, 31);
  built = true;
}
/** A's and B's texts for one input: [a, b] */
function both(mode, input, extra = []) {
  const a = path.join(OUT, `a_${mode}.txt`), b = path.join(OUT, `b_${mode}.txt`);
  run(path.join(OUT, 'fxpresrun'), [mode === 'mut' ? 'vm' : mode, input, a, ...extra]);
  run(PY, ['-I', 'tools/fxpres.py', mode === 'mut' ? 'vm' : mode, input, b, ...extra]);
  return [fs.readFileSync(a, 'utf8'), fs.readFileSync(b, 'utf8')];
}

test('mw-fxpres: A\'s unit tests and B\'s self-test', { skip: SKIP, timeout: 300_000 }, () => {
  build();
  const out = run(path.join(OUT, 'fxpres_test'), ['--mw', ...files]);
  assert.match(out, /^effects: 31 files, 616 preshaders, 739 instructions \(739 mov\), \d+ pass states evaluated/m);
  assert.match(out, /^padding: 150 preshaders read past their float registers, \d+ pass states read 0 there$/m);
  assert.match(out, /^fxpres_test: \d+ checks, 0 failed$/m);
  assert.match(run(PY, ['-I', 'tools/fxpres.py', '--self-test']), /^fxpres --self-test: \d+ checks, 0 failed$/m);
  // the oracle comparison catches each way A can be wrong (a refusal, a stop, a dropped state, an error, a setter)
  assert.match(run(PY, ['-I', 'tools/mw/fxpres_compare.py', '--self-test']),
    /^fxpres_compare --self-test: \d+ checks, 0 failed$/m);
});

test('mw-fxpres: A and B give the same interpreter results on every case', { skip: SKIP, timeout: 300_000 }, () => {
  build();
  const cases = path.join(OUT, 'cases.txt');
  run(PY, ['-I', 'tools/mw/fxpres_cases.py', 'vm', RES, cases, '--mw', '1', '--synth', '1', '--random', '500',
    '--corners', '6']);
  const [a, b] = both('vm', cases);
  assert.ok(a === b, `the results differ at ${a === b ? '' : firstDiff(a, b)}`);
  assert.equal((a.match(/^case mw_/gm) || []).length, 616 * 3, 'every preshader at three precisions');
  assert.equal((a.match(/^case mw_\S+ load ok$/gm) || []).length, 616 * 3, 'and every one loads');
  assert.ok((a.match(/^ {2}run ok$/gm) || []).length > 3000);
  assert.ok((a.match(/^case corner_/gm) || []).length > 120, 'the rounding corners');
  assert.ok((a.match(/^case snanm_/gm) || []).length > 50, 'signalling NaNs in memory operands');
  const mut = path.join(OUT, 'mut.txt');
  run(PY, ['-I', 'tools/mw/fxpres_cases.py', 'mutate', RES, mut, '--copies', '3']);
  const [ma, mb] = both('mut', mut);
  assert.ok(ma === mb, `the results on corrupted copies differ at ${ma === mb ? '' : firstDiff(ma, mb)}`);
});

test('mw-fxpres: A and B give the same state values over SetValue rounds on the 31', { skip: SKIP, timeout: 300_000 },
  () => {
    build();
    const rounds = path.join(OUT, 'rounds.txt');
    run(PY, ['-I', 'tools/mw/fxpres_cases.py', 'rounds', RES, rounds, '--rounds', '2']);
    const [a, b] = both('effect', rounds, files);
    assert.ok(a === b, `the state values differ at ${a === b ? '' : firstDiff(a, b)}`);
    assert.equal((a.match(/^round /gm) || []).length, 6);
    assert.equal((a.match(/ = error /g) || []).length, 0, 'no state without a value');
    assert.ok((a.match(/ state /g) || []).length > 5000);
    assert.ok((a.match(/^call Set/gm) || []).length > 300, 'the typed setters ran in the rounds');
    // the same rounds on copies whose preshaders read the input table's padding or compute (rcp, rsq, neg, frc, exp,
    // log, sin): the gather, the precision and the scatter's conversions on values that are not exact
    for (const spec of [['pad'], ['op', '103'], ['op', '107'], ['op', '101'], ['op', '104'], ['op', '105'],
      ['op', '106', '--n1'], ['op', '108']]) {
      const dir = path.join(OUT, `fx_${spec.slice(0, 2).join('_')}`);
      run(PY, ['-I', 'tools/mw/fxpres_cases.py', 'patch', RES, dir, ...spec]);
      const copies = fs.readdirSync(dir).filter(f => f.endsWith('.fxo')).sort().map(f => path.join(dir, f));
      const [ca, cb] = both('effect', rounds, copies);
      assert.ok(ca === cb, `${spec.join(' ')}: the state values differ at ${ca === cb ? '' : firstDiff(ca, cb)}`);
      assert.ok((ca.match(/ state /g) || []).length > 5000, spec.join(' '));
    }
  });

test('mw-fxpres: A against d3dx9_26.dll under Unicorn',
  { skip: SKIP || (!D3DX && 'NFSMW_D3DX is not set (the path of d3dx9_26.dll)') || (!haveUnicorn && 'no Unicorn'),
    timeout: 600_000 }, () => {
    build();
    const out = run('tools/mw/fxoracle.sh', ['--quick'], { NFSMW_D3DX: D3DX, FXRES: RES, FXOUT: path.join(OUT, 'oracle') });
    assert.equal((out.match(/^A and the oracle agree on \d+ cases$/gm) || []).length, 2, out);
    assert.equal((out.match(/^A and the oracle agree on every preshader state of \d+ passes$/gm) || []).length, 2, out);
    assert.equal((out.match(/^effect, .*: A and the oracle agree on every preshader state of \d+ passes/gm) || []).length,
      8, 'the rounds over the patched copies');
  });
