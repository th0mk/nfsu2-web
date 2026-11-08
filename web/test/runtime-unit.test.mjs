// The runtime's native unit tests, built with the commands in their heads and run: tests/objtable_unit.c (the kernel
// object table of runtime/win32_ext.c: a full table takes back a finished thread's slot, never one a wait or a handle
// still names), tests/watchdog_unit.c (runtime/watchdog.c: no false stall from a stamp a little ahead of its
// clock), tests/racenet_unit.c (runtime/racenet.c: the creator's list only from the creator, with the slot each
// joiner's game drives, each joiner's car states for that slot only), tests/racecars_unit.c (runtime/racecars.c:
// each racer's own car states, who may speak for which car, the cars drawn and driven) and tests/catchup_unit.c
// (runtime/catchup.c: the career's rubberbanding for each setting, and the game's own outside career races), and
// tests/modparts_unit.c (runtime/modpacks.c's MODPARTS.DAT beside a profile: the mods' parts, the cars of car types the
// game has not, and runtime/stance.c's stances).  Every case of theirs passes, and each one ran.
//   node --test web/test/runtime-unit.test.mjs        (macOS with clang: Xcode's command line tools; about 10 s)
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
// the native runtime builds on macOS only (kqueue, the one-argument pthread_setname_np)
const skip = process.platform !== 'darwin' ? 'the native runtime builds on macOS only'
  : spawnSync('clang', ['--version'], { encoding: 'utf8' }).status !== 0 && 'no clang';

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

test('watchdog_unit: no false stall from a stamp ahead of the watchdog\'s clock, a real one reported', { skip, timeout: 120000 }, () => {
  const { out, pass } = run('watchdog_unit', ['-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter']);
  for (const c of ['ahead-us', 'ahead-s', 'stall', 'short', 'download']) assert.ok(pass.includes(c), `case ${c} passed`);
  assert.ok(pass.length >= 7, `every check ran (${pass.length})`);
  assert.match(out, /watchdog unit tests: all passed/);
});

test('racenet_unit: the creator\'s list only from the creator, a joiner\'s states only for its own slot', { skip }, () => {
  const { out, pass } = run('racenet_unit', ['-O1', '-g', '-Wall', '-Wno-unused-function']);
  for (const c of ['stranger', 'creator', 'second', 'direct', 'own-slot', 'taken-slot', 'rules', 'reset', 'list-slots', 'old-creator', 'host-list',
    'host-slots']) assert.ok(pass.includes(c), `case ${c} passed`);
  assert.ok(pass.length >= 17, `every check ran (${pass.length})`);
  assert.match(out, /racenet unit tests: all passed/);
});

test('racecars_unit: each racer\'s own car states, only for its own car, drawn and driven', { skip }, () => {
  const { out, pass } = run('racecars_unit', ['-O1', '-g', '-Wall', '-Wno-unused-function', '-ffp-contract=off']);
  for (const c of ['wire', 'stranger', 'own-slot', 'bound', 'rtt', 'drawn', 'ghost', 'hit', 'hit-back', 'driven', 'reset', 'claimed', 'honest',
    'old-creator']) assert.ok(pass.includes(c), `case ${c} passed`);
  assert.match(out, /racecars unit tests: all passed/);
});

test('catchup_unit: the career\'s rubberbanding as the setting says, outside career races the game\'s own', { skip }, () => {
  const { out, pass } = run('catchup_unit', ['-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-function']);
  for (const m of ['original', 'noboost', 'off'])
    for (const c of ['no-race', 'behind', 'ahead', 'no-car']) assert.ok(pass.includes(`${m}-${c}`), `case ${m}-${c} passed`);
  for (const c of ['unset', 'bogus', 'case']) assert.ok(pass.includes(c), `case ${c} passed`);
  assert.match(out, /catchup unit tests: all passed/);
});

test('modparts_unit: what a profile never holds, kept beside it (MODPARTS.DAT), stances too', { skip, timeout: 120000 }, () => {
  const { out } = run('modparts_unit', ['-O1', '-g', '-Wall', '-Wno-unused-function']);
  const pass = [...out.matchAll(/^PASS (\S+)/mg)].map(m => m[1]);
  for (const c of ['af-hide', 'af-back', 'trade', 'writefail', 'counter', 'deleted', 'modded-keys', 'stance-basic', 'stance-elsewhere', 'stance-replaced', 'stance-addon',
    'stance-split', 'stance-noprofile', 'stance-deleted', 'stance-order', 'stance-changed', 'stance-later', 'stance-trade', 'stance-modrim',
    'stance-reset-all']) assert.ok(pass.includes(c), `case ${c} passed`);
  assert.match(out, /modparts_unit: passed/);
});
