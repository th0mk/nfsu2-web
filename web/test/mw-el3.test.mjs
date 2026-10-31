// Most Wanted's EALayer3 decoder (runtime/mw/ealayer3_mw.c) off the game: tools/mw/el3_unit.c, built with the
// address and undefined-behaviour sanitizers, runs each of its tests (its core on made-up blocks, and its side in SND
// against a model of SND's stream).  Without clang the test is skipped.
//   node --test web/test/mw-el3.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.join(REPO, 'out/mw/el3unit');
const BIN = path.join(OUT, 'el3_unit');
const haveClang = spawnSync('clang', ['--version'], { encoding: 'utf8' }).status === 0;
const TESTS = ['count', 'stream', 'failed', 'bounds'];

test('el3_unit builds with the sanitizers', { skip: !haveClang && 'no clang' }, () => {
  fs.mkdirSync(OUT, { recursive: true });
  execFileSync('clang', ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-fsanitize=address,undefined',
    '-fno-sanitize-recover=all', '-Iruntime', '-o', BIN, 'tools/mw/el3_unit.c', '-lm'], { cwd: REPO, encoding: 'utf8' });
});
for (const t of TESTS) {
  test(`el3_unit ${t}`, { skip: !haveClang && 'no clang' }, () => {
    const r = spawnSync(BIN, [t], { encoding: 'utf8', timeout: 60_000 });
    assert.equal(r.status, 0, `${r.stdout}${r.stderr.split('\n').filter(l => !l.startsWith('[el3]')).slice(0, 20).join('\n')}`);
    assert.match(r.stdout, new RegExp(`^PASS ${t}$`, 'm'));
  });
}
