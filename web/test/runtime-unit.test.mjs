// The runtime's native unit tests, built with the commands in their heads and run: tests/objtable_unit.c (the kernel
// object table of runtime/win32_ext.c: a full table takes back a finished thread's slot, never one a wait or a handle
// still names) and tests/watchdog_unit.c (runtime/watchdog.c: no false stall from a stamp a little ahead of its
// clock).  Every case of theirs passes, and each one ran.
//   node --test web/test/runtime-unit.test.mjs        (macOS with clang: Xcode's command line tools; about 10 s;
//                                                      Linux with clang: watchdog_unit only)
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
// the native runtime builds on macOS only (win32_ext.c's kqueue); watchdog.c builds on Linux too (its
// pthread_setname_np and dispatch.c's clock have Linux branches)
const noClang = spawnSync('clang', ['--version'], { encoding: 'utf8' }).status !== 0 && 'no clang';
const skip = process.platform !== 'darwin' ? 'the native runtime builds on macOS only' : noClang;
const skipWd = process.platform !== 'darwin' && process.platform !== 'linux' ? 'watchdog.c builds on macOS and Linux only'
  : noClang;

/** builds tests/<name>.c with its head's flags into out/web/test, runs it; its PASS lines and its output */
function run(name, flags) {
  const exe = path.join(OUT, name);
  const b = spawnSync('clang', [...flags, '-Iruntime', '-o', exe, `tests/${name}.c`], { cwd: REPO, encoding: 'utf8' });
  assert.equal(b.status, 0, `tests/${name}.c builds:\n${b.stderr}`);
  const r = spawnSync(exe, [], { cwd: REPO, encoding: 'utf8', timeout: 60000 });
  const out = (r.stdout ?? '') + (r.stderr ?? '');
  assert.doesNotMatch(out, /^FAIL /m, out);
  assert.equal(r.status, 0, out);
  return { out, pass: [...out.matchAll(/^PASS (\S+):/mg)].map(m => m[1]) };
}

test('objtable_unit: the object table takes back finished threads\' slots only', { skip }, () => {
  const { out, pass } = run('objtable_unit', ['-O1', '-g', '-Wall', '-Wno-unused-parameter', '-Wno-unused-function']);
  for (const c of ['below', 'reclaim', 'oldest', 'stale', 'kept', 'waiting']) assert.ok(pass.includes(c), `case ${c} passed`);
  assert.ok(pass.length >= 13, `every check ran (${pass.length})`);
  assert.match(out, /object table unit tests: all passed/);
});

test('watchdog_unit: no false stall from a stamp ahead of the watchdog\'s clock, a real one reported', { skip: skipWd, timeout: 120000 }, () => {
  const { out, pass } = run('watchdog_unit', ['-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter']);
  for (const c of ['ahead-us', 'ahead-s', 'stall', 'lines', 'short', 'download']) assert.ok(pass.includes(c), `case ${c} passed`);
  assert.ok(pass.length >= 8, `every check ran (${pass.length})`);
  assert.match(out, /watchdog unit tests: all passed/);
});
