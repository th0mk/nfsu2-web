// The Win32 imports of Most Wanted's HLE tasks (TRACKER M2-09 waitable timers,
// M2-10 small imports, M2-11 voice devices), built for wasm and called as
// lifted code calls them:
//   node --test web/test/win32-wasm.test.mjs        (needs wasi-sdk in deps/)
// web/test/win32_unit.c links runtime/win32.c, win32_ext.c, win32_misc.c and
// keyboard.c and checks every import against Windows' documented answers and
// the stack pointer after each call.  Its import table's stdcall counts are
// checked here against argcount's KNOWN (the STRICT table every lift takes
// its counts from) and against the lifts in this worktree: MW's gen/mw/all,
// UG2's gen/all and gen/server (UG2's server.dll imports CompareStringA/W,
// SetStdHandle and SetEnvironmentVariableA too).  The threaded half of the
// program runs in the browser host: web/tools/waitstress.sh (both built by
// web/tools/win32unit.mjs).  Built with UG2's game table (-DWIN32_UNIT_UG2,
// no g_game.win32_misc), those four of UG2's stay unimplemented, as they
// always were: UG2 does not change.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { CC, buildWin32Unit } from '../tools/win32unit.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
const skip = !CC && 'no wasi-sdk in deps/';
// the program uses setjmp (FatalAppExitA's exit path): wasm exception references, behind a flag in older Node
const EXNREF = spawnSync(process.execPath, ['--experimental-wasm-exnref', '-e', '0']).status === 0 ? ['--experimental-wasm-exnref'] : [];

/** the import table in win32_unit.c: name -> { dll, nargs } */
function unitTable() {
  const src = fs.readFileSync(path.join(REPO, 'web/test/win32_unit.c'), 'utf8');
  const t = new Map();
  for (const m of src.matchAll(/\{ IAT\(\d+\),\s*"([^"]+)",\s*"([^"]+)",\s*(\d+) \}/g)) t.set(m[2], { dll: m[1], nargs: +m[3] });
  return t;
}
/** argcount's KNOWN counts ('DSOUND#6' style keys for ordinals) */
function knownCounts() {
  const src = fs.readFileSync(path.join(REPO, 'tools/argcount.py'), 'utf8');
  const k = new Map();
  for (const m of src.matchAll(/'([A-Za-z_][A-Za-z0-9_]*|[A-Z0-9]+#\d+)':\s*(\d+)/g)) if (!k.has(m[1])) k.set(m[1], +m[2]);
  return k;
}
/** a lift's import table (impargs.c): name -> nargs, null when the lift is not here */
function liftTable(dir) {
  const f = path.join(REPO, dir, 'impargs.c');
  if (!fs.existsSync(f)) return null;
  const t = new Map();
  for (const m of fs.readFileSync(f, 'utf8').matchAll(/\{0x[0-9a-f]+u,"([^"]+)","([^"]+)",(\d+)\}/g))
    t.set(m[2] === '#6' || m[2] === '#1' ? `${m[1].toUpperCase().replace(/\.DLL$/, '')}${m[2]}` : m[2], +m[3]);
  return t;
}
const keyOf = (name, dll) => (name.startsWith('#') ? `${dll.toUpperCase().replace(/\.DLL$/, '')}${name}` : name);

// the imports the three tasks implement (GAME.md 3.3): MW's speed.exe imports all of them
const TASK_IMPORTS = ['CreateWaitableTimerA', 'SetWaitableTimer', 'GetSystemTime', 'SystemTimeToFileTime',
  'GlobalMemoryStatusEx', 'GetDateFormatA', 'GetTimeFormatA', 'CompareStringA', 'CompareStringW', 'SetStdHandle',
  'SetEnvironmentVariableA', 'GetKeyState', 'GetKeyboardLayout', 'MapVirtualKeyA', 'MapVirtualKeyExA', 'ToUnicode', 'SendInput',
  'QueueUserAPC', 'CreateProcessA', 'FatalAppExitA', 'DSOUND#6',
  ...['GetDevCapsA', 'GetNumDevs', 'Write', 'GetPosition', 'SetVolume', 'Reset', 'Open', 'Close', 'PrepareHeader', 'UnprepareHeader']
    .map(f => `waveOut${f}`),
  ...['Start', 'Stop', 'Reset', 'AddBuffer', 'GetPosition', 'Open', 'Close', 'GetNumDevs', 'GetDevCapsA', 'PrepareHeader', 'UnprepareHeader']
    .map(f => `waveIn${f}`)];

test('win32_unit: the import table\'s stdcall counts are argcount\'s KNOWN ones and the lifts\'', () => {
  const unit = unitTable(), known = knownCounts();
  assert.equal(unit.size, 54, 'the table parsed');
  for (const [name, { dll, nargs }] of unit) {
    const k = keyOf(name, dll);
    assert.ok(known.has(k), `${k} is in KNOWN`);
    assert.equal(nargs, known.get(k), `${k}: KNOWN has ${known.get(k)}`);
  }
  assert.equal(TASK_IMPORTS.length, 42);           // 4 for the timer thread, 16 small ones, DSOUND #6, 21 winmm
  for (const k of TASK_IMPORTS) assert.ok([...unit].some(([n, e]) => keyOf(n, e.dll) === k), `${k} is in the unit table`);
  // MW's lift (STRICT=1): every task import, with the table's count; UG2's lifts: the ones they have
  const mw = liftTable('gen/mw/all');
  if (mw) for (const k of TASK_IMPORTS) {
    assert.ok(mw.has(k), `MW's speed.exe imports ${k}`);
    const u = [...unit].find(([n, e]) => keyOf(n, e.dll) === k)[1];
    assert.equal(mw.get(k), u.nargs, `${k}: MW's impargs.c pops ${mw.get(k)}`);
  }
  for (const dir of ['gen/all', 'gen/server', 'gen/mw/server']) {
    const t = liftTable(dir); if (!t) continue;
    for (const [name, { dll, nargs }] of unit) {
      const k = keyOf(name, dll);
      if (t.has(k)) assert.equal(t.get(k), nargs, `${dir}: ${k} pops ${t.get(k)}`);
    }
  }
  const srv = liftTable('gen/server');
  if (srv) for (const k of ['CompareStringA', 'CompareStringW', 'SetStdHandle', 'SetEnvironmentVariableA']) assert.ok(srv.has(k), `UG2's server.dll imports ${k}`);
});

test('win32_unit hle: timers, small imports and voice devices answer as Windows does, the stack balanced', { skip }, () => {
  const w = buildWin32Unit(path.join(OUT, 'win32_unit.wasm'));
  const r = spawnSync(process.execPath, [...EXNREF, path.join(REPO, 'web/test/wasi-run.mjs'), w, 'hle'], { encoding: 'utf8', timeout: 60000 });
  const out = r.stdout ?? '', err = r.stderr ?? '';
  if (r.status !== 0) console.log(out + err);
  const m = /win32_unit: (\d+) passed, (\d+) failed/.exec(out);
  assert.ok(m, 'win32_unit ran to the end');
  assert.equal(+m[2], 0, 'no failed check');
  assert.ok(+m[1] >= 1400, `every check ran (${m[1]})`);
  assert.equal(r.status, 0);
  // the logs say what each refusal and each timer was
  assert.match(err, /^\[timer\] CreateWaitableTimerA\(manual reset\) -> 0x00a00001 \(from 0x00401234\)$/m);
  assert.match(err, /^\[timer\] SetWaitableTimer\(0x00a00001\): absolute due time, fires in 10\.0 ms, period 0 ms/m);
  assert.match(err, /^\[timer\] SetWaitableTimer\(0x00a00001\): completion routine 0x00401000 not supported \(no APCs\); the timer still signals$/m);
  assert.match(err, /^\[hle\] GlobalMemoryStatusEx: 1024 MiB of physical memory/m);
  assert.match(err, /^\[hle\] QueueUserAPC\(routine 0x00401000, thread 0xfffffffe\) refused: no APCs/m);
  assert.match(err, /^\[hle\] CreateProcessA\("C:\\x\.exe", ""\) refused: no other processes/m);
  assert.match(err, /^\[voice\] DirectSoundCaptureCreate -> DSERR_NODRIVER: no voice devices/m);
  assert.equal((err.match(/^\[voice\] /mg) || []).length, 22, 'one [voice] line per import (21 winmm, 1 DSOUND)');
  assert.match(err, /^\*\*\* guest FatalAppExitA\("the end"\) from 0x00401234 - exiting$/m);
  assert.doesNotMatch(err, /UNIMPLEMENTED/);
  // M2-20: a new thread's routine, past _threadstartex and the game's trampoline (Most Wanted's thread table)
  assert.match(err, /^\[thread\] tid=\d+ runs 0x007ec601 \(timer\), CreateThread\(entry 0x0085d6e0\) from 0x00401234$/m);
  assert.match(err, /^\[thread\] tid=\d+ runs 0x00820bd7 \(sound\), CreateThread\(entry 0x00820bd7\) from 0x00401234$/m);
});

test('win32_unit ug2: with UG2\'s game table its CompareStringA/W, SetStdHandle and SetEnvironmentVariableA stay unimplemented', { skip }, () => {
  const w = buildWin32Unit(path.join(OUT, 'win32_unit_ug2.wasm'), ['-DWIN32_UNIT_UG2']);
  const r = spawnSync(process.execPath, [...EXNREF, path.join(REPO, 'web/test/wasi-run.mjs'), w, 'ug2'], { encoding: 'utf8', timeout: 60000 });
  const out = r.stdout ?? '';
  if (r.status !== 0) console.log(out + (r.stderr ?? ''));
  const m = /win32_unit: (\d+) passed, (\d+) failed/.exec(out);
  assert.ok(m, 'win32_unit ran to the end');
  assert.equal(+m[2], 0, 'no failed check');
  assert.ok(+m[1] >= 40, `every check ran (${m[1]})`);
  assert.equal(r.status, 0);
  // UG2's table names no threads: CreateThread's log is as it always was
  assert.doesNotMatch(r.stderr ?? '', /\[thread\] tid=\d+ runs /);
});
