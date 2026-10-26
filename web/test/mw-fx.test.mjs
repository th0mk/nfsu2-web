// Most Wanted's 31 compiled effects (docs/mw/fx2-format.md): the two dumpers written apart from the spec,
// runtime/mw/fx2.c with tools/mw/fxdump.c (A) and tools/fx2dump.py (B), give the same canonical dump of every
// effect byte for byte, with no UNKNOWN, and as many shader blocks per version and preshader blocks as
// tools/fxres.py --census counts from the raw blobs.  The effects come out of speed.exe into out/mw/fx/test/
// (ignored: the game's own data) and are removed afterwards.
//   node --test web/test/mw-fx.test.mjs
// NFSMW_DATA is the game folder (default mw/Need For Speed Most Wanted Black Edition in the checkout).  Without
// speed.exe there, clang, or a Python with pefile (.venv, requirements.txt), the test is skipped.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const DATA = process.env.NFSMW_DATA || path.join(REPO, 'mw/Need For Speed Most Wanted Black Edition');
const EXE = path.join(DATA, 'speed.exe');
const OUT = path.join(REPO, 'out/mw/fx/test');
const PY = fs.existsSync(path.join(REPO, '.venv/bin/python')) ? path.join(REPO, '.venv/bin/python') : 'python3';
const haveClang = spawnSync('clang', ['--version'], { encoding: 'utf8' }).status === 0;
const havePefile = spawnSync(PY, ['-I', '-c', 'import pefile'], { encoding: 'utf8' }).status === 0;
const SKIP = !fs.existsSync(EXE) ? 'no speed.exe in the game folder (NFSMW_DATA)' : !haveClang ? 'no clang'
  : !havePefile ? 'no Python with pefile (.venv: pip install -r requirements.txt)' : false;
test.after(() => fs.rmSync(OUT, { recursive: true, force: true }));

const run = (cmd, args) => execFileSync(cmd, args, { cwd: REPO, encoding: 'utf8', maxBuffer: 1 << 26 });
/** where two dumps part: the first line that differs */
function firstDiff(a, b) {
  const x = a.split('\n'), y = b.split('\n');
  for (let i = 0; ; i++) if (x[i] !== y[i]) return `line ${i + 1}\n  A: ${x[i]}\n  B: ${y[i]}`;
}

test('mw-fx: A and B dump all 31 effects alike, with no UNKNOWN and the census\'s blob counts', { skip: SKIP, timeout: 300_000 }, () => {
  fs.rmSync(OUT, { recursive: true, force: true });
  const res = path.join(OUT, 'res'), a = path.join(OUT, 'a'), b = path.join(OUT, 'b'), bin = path.join(OUT, 'fxdump');
  fs.mkdirSync(a, { recursive: true });
  run(PY, ['-I', 'tools/fxres.py', EXE, res]);
  run('clang', ['-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-Iruntime/mw', '-o', bin, 'tools/mw/fxdump.c', 'runtime/mw/fx2.c']);
  const names = fs.readdirSync(res).filter(f => f.endsWith('.fxo')).map(f => f.slice(0, -4)).sort();
  assert.equal(names.length, 31);
  for (const n of names) run(bin, [path.join(res, `${n}.fxo`), path.join(a, `${n}.txt`)]);
  run(PY, ['-I', 'tools/fx2dump.py', res, b]);
  assert.deepEqual(fs.readdirSync(b).sort(), fs.readdirSync(a).sort());

  // the census: per effect, the size-prefixed blobs of each version (fxres.py's line: name, size, kind count ...)
  const census = {};
  for (const m of run(PY, ['-I', 'tools/fxres.py', '--census', res]).matchAll(/^ {2}(IDI_\S+) +\d+ *(.*)$/gm)) {
    const t = m[2].trim().split(/\s+/).filter(Boolean);
    census[m[1]] = Object.fromEntries(t.flatMap((k, i) => i % 2 ? [] : [[k, +t[i + 1]]]));
  }
  let presh = 0;
  for (const n of names) {
    const da = fs.readFileSync(path.join(a, `${n}.txt`), 'utf8'), db = fs.readFileSync(path.join(b, `${n}.txt`), 'utf8');
    assert.ok(da === db, `${n}: the dumps differ at ${da === db ? '' : firstDiff(da, db)}`);
    assert.equal(da.split('UNKNOWN(').length - 1, 0, `${n}: UNKNOWN in the dump`);
    const got = {};
    for (const m of da.matchAll(/^ {2}shader (\S+) .* end$/gm)) got[m[1]] = (got[m[1]] || 0) + 1;
    const p = da.match(/^ {2}preshader 0x46580200 dwords \d+ end$/gm)?.length || 0;
    if (p) got.fxlc = p;
    presh += p;
    assert.deepEqual(got, census[n], `${n}: blocks in the dump against the census`);
  }
  assert.equal(presh, 616);
});
