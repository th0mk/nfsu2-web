// Most Wanted's msvcrt.dll, the D3DX oracle's C runtime (TRACKER M2-07), built for wasm and called as the oracle's
// lifted code calls it:
//   node --test web/test/msvcrt-wasm.test.mjs        (needs wasi-sdk in deps/)
// web/test/msvcrt_unit.c links runtime/mw/msvcrt_mw.c with runtime/win32.c, win32_ext.c, win32_misc.c, keyboard.c,
// module.c, modheap.c and MW's table with the oracle's entry (game_mw.c, -DNFSU2_D3DX_ORACLE=1); each import is served
// as the oracle's (its index in the oracle's part of the import table, its heap the oracle's own).  The import table
// is d3dx9_26.dll's 63 msvcrt imports: checked here against argcount's KNOWN and, when the oracle was lifted, the
// lift's own table.  The five imports that end the process run one per process.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { CC, buildMsvcrtUnit } from '../tools/win32unit.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
const skip = !CC && 'no wasi-sdk in deps/';
const EXNREF = spawnSync(process.execPath, ['--experimental-wasm-exnref', '-e', '0']).status === 0 ? ['--experimental-wasm-exnref'] : [];
let built = null;
const wasm = () => built ??= buildMsvcrtUnit(path.join(OUT, 'msvcrt_unit.wasm'));
function run(...args) {
  const r = spawnSync(process.execPath, [...EXNREF, path.join(REPO, 'web/test/wasi-run.mjs'), wasm(), ...args],
    { encoding: 'utf8', timeout: 120000, env: { ...process.env, WASI_PAGES: '16384' } });
  return { status: r.status, out: r.stdout ?? '', err: r.stderr ?? '' };
}

/** the unit's table: name -> [IAT, nargs] */
function unitTable() {
  const src = fs.readFileSync(path.join(REPO, 'web/test/msvcrt_unit.c'), 'utf8');
  return new Map([...src.matchAll(/CRT\((\d+), "([^"]+)"\)/g)].map(m => [m[2], [0xd811a4 + 4 * +m[1], 0]]));
}

test('msvcrt_unit\'s import table: the oracle\'s 63 msvcrt imports, cdecl in KNOWN and in the lift', () => {
  const unit = unitTable(), known = fs.readFileSync(path.join(REPO, 'tools/argcount.py'), 'utf8');
  assert.equal(unit.size, 63, 'the table parsed');
  for (const name of unit.keys()) {
    const k = new RegExp(`'${name.replace(/[?@$]/g, '\\$&')}':\\s*(\\d+)`).exec(known);
    assert.ok(k, `${name} is in KNOWN`);
    assert.equal(+k[1], 0, `${name}: cdecl, KNOWN pops 0`);
  }
  const lift = path.join(REPO, 'gen/mw/d3dx/impargs.c');
  if (!fs.existsSync(lift)) return;                          // the oracle is lifted on a dev machine only
  const t = new Map([...fs.readFileSync(lift, 'utf8').matchAll(/\{0x([0-9a-f]+)u,"msvcrt\.dll","([^"]+)",(\d+)\}/g)]
    .map(m => [m[2], [parseInt(m[1], 16), +m[3]]]));
  assert.deepEqual([...t.keys()], [...unit.keys()], 'the same imports in the same order as the lift');
  for (const [name, [iat, n]] of t) assert.deepEqual([iat, n], unit.get(name), `${name}: IAT and pops as the lift's`);
});

test('msvcrt: every import against the documented results, served as the oracle\'s', { skip }, () => {
  const r = run('crt');
  if (r.status !== 0) console.log(r.out + r.err);
  const m = /msvcrt_unit: (\d+) passed, (\d+) failed/.exec(r.out);
  assert.ok(m, 'ran to the end');
  assert.equal(+m[2], 0, 'no failed check');
  assert.ok(+m[1] >= 1400, `every check ran (${m[1]})`);
  assert.equal(r.status, 0);
  // the oracle's heap: its own part of the modheap region (memmap_mw.h)
  assert.match(r.out, /^T malloc\(100\) 0x030000[0-9a-f]{2}, /m);
  assert.match(r.out, /^T qsort\(1000 ints\): \d+ comparator calls, 0 outside the array$/m);
  for (const l of ['[msvcrt] _adjust_fdiv = 0 at 0x', '[msvcrt] free(0x01000040): not a block of this DLL\'s heap',
    '[msvcrt] _controlfp unmasks x87 exceptions (control word 0x0243): the runtime raises none',
    '[msvcrt] setlocale(0, "German"): only "C" is here', '[msvcrt] _tempnam: no temporary files here, NULL',
    '*** guest exit(7) (msvcrt) - exiting']) assert.ok(r.err.includes(l), l);
  assert.doesNotMatch(r.err, /UNIMPLEMENTED/);
  const first = new Set([...r.err.matchAll(/^\[msvcrt\] (\S+): first call \(from 0x00d81234\)$/gm)].map(x => x[1]));
  assert.equal(first.size, 58, 'all but the five that end the process were called');
});

test('msvcrt: the imports that end the process, each with its line and exit code', { skip }, () => {
  for (const [name, code, what] of [['?terminate@@YAXXZ', 3, 'terminate()'], ['_purecall', 3, '_purecall (a pure virtual function was called, R6025)'],
    ['_except_handler3', 5, '_except_handler3 (exception dispatch)'], ['__CxxFrameHandler', 5, '__CxxFrameHandler (exception dispatch)'],
    ['longjmp', 5, 'longjmp(0x06000800, 1) to the setjmp that returned to 0x00dabcde']]) {
    const r = run('stop', name);
    assert.equal(r.status, code, `${name}: exit code ${code}`);
    const how = code === 3 ? 'the process ends here on Windows too' : 'not supported by the runtime';
    assert.ok(r.err.includes(`*** GUEST msvcrt ${what} from 0x00d81234: ${how}, exiting (code ${code})`), `${name}'s line`);
    assert.doesNotMatch(r.out, /returned/, `${name} did not return`);
  }
});
