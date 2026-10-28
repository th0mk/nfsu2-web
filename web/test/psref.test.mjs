// The D3D9 reference for ps_2_x, ps_3_0 and vs_3_0 (web/tools/psref.c, docs/web/psref.md): its unit tests, one group
// per instruction class (web/test/psref_unit.c), and with Most Wanted's game folder, every ps_2_x, ps_3_0 and vs_3_0
// blob of its 31 effects loaded under an SM3 device's caps (the ps_2_x ones also under ps_2_b's, the profile their
// constant tables name) and run.  The effects come out of speed.exe into out/mw/psref/ (ignored: the game's own data)
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
