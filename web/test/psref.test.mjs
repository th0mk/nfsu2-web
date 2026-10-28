// The D3D9 reference for ps_2_x, ps_3_0 and vs_3_0 (web/tools/psref.c, docs/web/psref.md): its unit tests, one group
// per instruction class (web/test/psref_unit.c), and with Most Wanted's game folder, every ps_2_x, ps_3_0 and vs_3_0
// blob of its 31 effects loaded under an SM3 device's caps (the ps_2_x ones also under ps_2_b's, the profile their
// constant tables name) and run.  Then the WGSL translator of those models on the unit tests' programs and on every
// shader of the effects (its differential test against psref: node web/tools/shader-check.mjs --sm3).  The effects come out of speed.exe into out/mw/psref/ (ignored: the game's own data)
// and are removed afterwards.
//   node --test web/test/psref.test.mjs
// NFSMW_DATA is the game folder (default mw/Need For Speed Most Wanted Black Edition in the checkout).  Without clang
// the test is skipped; without speed.exe there or a Python with pefile (.venv, requirements.txt), its second part is.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const DATA = process.env.NFSMW_DATA || path.join(REPO, 'mw/Need For Speed Most Wanted Black Edition');
const EXE = path.join(DATA, 'speed.exe');
const OUT = path.join(REPO, 'out/mw/psref');
const BIN = path.join(REPO, 'out/web/test/psref_unit');
const PY = fs.existsSync(path.join(REPO, '.venv/bin/python')) ? path.join(REPO, '.venv/bin/python') : 'python3';
const haveClang = spawnSync('clang', ['--version'], { encoding: 'utf8' }).status === 0;
const havePefile = spawnSync(PY, ['-I', '-c', 'import pefile'], { encoding: 'utf8' }).status === 0;
const SKIP = !haveClang && 'no clang';
const SKIP_MW = SKIP || (!fs.existsSync(EXE) ? 'no speed.exe in the game folder (NFSMW_DATA)'
  : !havePefile ? 'no Python with pefile (.venv: pip install -r requirements.txt)' : false);
test.after(() => fs.rmSync(OUT, { recursive: true, force: true }));

const run = (cmd, args) => execFileSync(cmd, args, { cwd: REPO, encoding: 'utf8', maxBuffer: 1 << 26 });
let built = false;
function build() {
  if (built) return;
  fs.mkdirSync(path.dirname(BIN), { recursive: true });
  run('clang', ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-ffp-contract=off', '-fsanitize=address,undefined',
    '-fno-sanitize-recover=all', '-Iweb/tools', '-o', BIN, 'web/test/psref_unit.c', 'web/tools/psref.c', '-lm']);
  built = true;
}

test('psref: unit tests per instruction class', { skip: SKIP, timeout: 300_000 }, () => {
  build();
  const out = run(BIN, []);
  assert.match(out, /^psref_unit: \d+ checks, 0 failed$/m, out);
});

test('psref: Most Wanted\'s 22 SM3-class blobs load and run', { skip: SKIP_MW, timeout: 300_000 }, () => {
  build();
  fs.rmSync(OUT, { recursive: true, force: true });
  const res = path.join(OUT, 'res');
  run(PY, ['-I', 'tools/fxres.py', EXE, res]);
  const files = fs.readdirSync(res).filter(f => f.endsWith('.fxo')).sort().map(f => path.join(res, f));
  assert.equal(files.length, 31);
  const out = run(BIN, ['--fx', ...files]);
  assert.match(out, /^mw: 22 SM3-class blobs: 18 ps_2_x, 2 ps_3_0, 2 vs_3_0; 22 loaded, 22 ran, 0 failed$/m, out);
  assert.match(out, /^mw: with ps_2_b caps 18 of 18 ps_2_x load$/m, out);
  // the same blob in two places gives the same results (its inputs come from its bytes)
  const byBytes = new Map();
  let lines = 0;
  for (const m of out.matchAll(/^blob \S+ #\d+ (\S+): \d+ dwords, \d+ instructions, bytes ([0-9a-f]{8}), ran, digest ([0-9a-f]{8})$/gm)) {
    lines++;
    const k = `${m[1]} ${m[2]}`;
    if (byBytes.has(k)) assert.equal(byBytes.get(k), m[3], `blob ${k}: two results`);
    byBytes.set(k, m[3]);
  }
  assert.equal(lines, 22);
});

// ---- the WGSL translator of these models (runtime/web/vsemu_sm3.inc) ----
// Here only whether it translates, with sanitizers: the modules' differential test against psref runs in a
// browser (node web/tools/shader-check.mjs --sm3).  A unit program may be refused only for what the module ABI
// has no place for (shader-check.mjs ABI_LIMITS).
const XL = path.join(REPO, 'out/web/test/wgsl-xl-sm3');
const ABI_LIMITS = /REJECTED: (MRT|vertex texture fetch|a vs_3_0 without a POSITION0 output|a ps_3_0 input the module ABI has no place for|a vs_3_0 output the module ABI has no place for|a loop count the application sets|a b# the application sets)/;
let xlBuilt = false;
function buildXl() {
  if (xlBuilt) return;
  build();
  run('clang', ['-O1', '-g', '-ffp-contract=off', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-DVSEMU_WGSL',
    '-DNFS_ENHANCED=0', '-Iruntime', '-Iruntime/web', '-Iweb/tools', '-o', XL, 'runtime/vsemu.c', 'web/tools/wgsl-xl.c', 'web/tools/psref.c', '-lm']);
  xlBuilt = true;
}
const index = dir => fs.readFileSync(path.join(dir, 'index.txt'), 'utf8').split('\n').filter(Boolean);

test('WGSL translator: psref\'s unit programs, refused only for what the module ABI lacks', { skip: SKIP, timeout: 300_000 }, () => {
  buildXl();
  const dir = path.join(OUT, 'xl-unit'), corpus = path.join(OUT, 'corpus');
  fs.rmSync(OUT, { recursive: true, force: true });
  fs.mkdirSync(dir, { recursive: true }); fs.mkdirSync(corpus);
  run(BIN, ['--emit', corpus]);
  const progs = fs.readdirSync(corpus).sort().map(f => path.join(corpus, f));
  const r = spawnSync(XL, ['--stages', '8', dir, ...progs], { cwd: REPO, encoding: 'utf8', maxBuffer: 1 << 26 });
  // 1: some refusals; a sanitizer's report also exits with 1
  assert.ok(r.status === 0 || r.status === 1, r.stderr);
  assert.doesNotMatch(r.stderr, /Sanitizer|runtime error/);
  const lines = index(dir).filter(l => /^(ps|vs) /.test(l));
  assert.equal(lines.length, progs.length);
  const bad = lines.filter(l => /REJECTED/.test(l) && !ABI_LIMITS.test(l));
  assert.deepEqual(bad, []);
  // every class of instruction in t_corpus translates, with vectors (psref_unit.c); a program psref refuses
  // on every input (an undefined read, say) gets none
  assert.ok(fs.readdirSync(dir).filter(f => f.endsWith('.p3vec')).length >= 71);
  assert.ok(fs.readdirSync(dir).filter(f => f.endsWith('.v3vec')).length >= 4);
});

test('WGSL translator: every shader of Most Wanted\'s 31 effects, for 8 stages', { skip: SKIP_MW, timeout: 300_000 }, () => {
  buildXl();
  fs.rmSync(OUT, { recursive: true, force: true });
  const res = path.join(OUT, 'res'), dir = path.join(OUT, 'xl-mw');
  run(PY, ['-I', 'tools/fxres.py', EXE, res]);
  fs.mkdirSync(dir);
  const fx = fs.readdirSync(res).filter(f => f.endsWith('.fxo')).sort().map(f => path.join(res, f));
  const out = run(XL, ['--stages', '8', '--fx', dir, ...fx]);
  assert.match(out, /^31 effects: 95 distinct shaders \(30 vertex, 65 pixel\), 47 repeated; 0 translation failures$/m, out);
  const first = f => fs.readFileSync(path.join(dir, f), 'utf8').split('\n')[0];
  const models = {};
  for (const f of fs.readdirSync(dir).filter(f => f.endsWith('.wgsl'))) {
    const m = /^\/\/ [pv]s_\d+: ([pv]s_\d_[0-9x])/.exec(first(f));
    if (m) models[m[1]] = (models[m[1]] ?? 0) + 1;
  }
  // the 22 SM3-class blobs, distinct by bytes, each with its vectors
  assert.equal(models.ps_2_x, 17); assert.equal(models.ps_3_0, 2); assert.equal(models.vs_3_0, 1);
  assert.equal(fs.readdirSync(dir).filter(f => f.endsWith('.p3vec')).length, 19);
  assert.equal(fs.readdirSync(dir).filter(f => f.endsWith('.v3vec')).length, 1);
  // the older models (ps_1_x, ps_2_0, vs_1_1), with 8 stages, get vectors from ps_ref_run and vs_run: 46 pixel
  // shaders and their cube twins, 29 vertex shaders
  assert.equal(fs.readdirSync(dir).filter(f => f.endsWith('.pvec')).length, 92);
  assert.equal(fs.readdirSync(dir).filter(f => f.endsWith('.vec')).length, 29);
});
