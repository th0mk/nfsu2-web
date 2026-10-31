// The browser build's memcmp/bcmp (runtime/web/memcmp.c, 16 bytes at a time)
// against a byte loop, built with the game's code generation:
//   node --test web/test/memcmp.test.mjs        (needs wasi-sdk in deps/)
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

test('memcmp/bcmp on wasm = a byte loop', { skip: !CC && 'no wasi-sdk in deps/' }, () => {
  const w = path.join(OUT, 'memcmp_unit.wasm');
  execFileSync(CC, ['--target=wasm32-wasip1-threads', '-pthread', '-O3', '-msimd128', '-mbulk-memory', '-mnontrapping-fptoint',
    '-mtail-call', '-o', w, 'web/test/memcmp_unit.c', 'runtime/web/memcmp.c',
    '-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=67108864,--max-memory=268435456'],
  { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  const out = execFileSync(process.execPath, [path.join(REPO, 'web/test/wasi-run.mjs'), w], { encoding: 'utf8', timeout: 120_000 }).trim();
  console.log(out);
  assert.match(out, /^memcmp_unit: passed/);
});
