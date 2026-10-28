// web/tools/g01.mjs, G-01's scanner (TRACKER M2-19), without a run:
//   node --test web/test/g01.test.mjs
// 1. games/mw/hle-reviewed.txt parses: one row per import the runtime leaves unimplemented, each an import of MW's lift
//    with its IAT (when gen/mw is here), none reviewed yet; a row's "yes" lets the import's line pass, a "no" on any
//    row of the same import does not.
// 2. The thrown type reads as C++ writes it.
// 3. A clean log has no reason; a log with one line of each marker has every reason, in g01's order; Underground 2's
//    runs are held to the same markers (it has no reviewed list: no import is reviewed).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { g01, readReviewed, typeName } from '../tools/g01.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const LIST = path.join(REPO, 'games/mw/hle-reviewed.txt');

test('games/mw/hle-reviewed.txt: the imports MW leaves unimplemented, none reviewed yet', () => {
  const rows = fs.readFileSync(LIST, 'utf8').split('\n').map(l => l.replace(/#.*/, '').trim()).filter(Boolean).map(l => l.split(/\s+/));
  assert.ok(rows.length >= 60, `${rows.length} rows`);
  for (const r of rows) {
    assert.ok(r.length === 4 || r.length >= 5 && r[3] === 'yes', `<module> <dll>!<name> <IAT> <yes|no> [<run>]: ${r.join(' ')}`);
    assert.match(r[0], /^(speed\.exe|server\.dll)$/);
    assert.match(r[2], /^0x[0-9a-f]{8}$/);
  }
  const list = readReviewed('mw');
  assert.equal(list.size, new Set(rows.map(r => r[1].replace(/^[^!]+/, d => d.toLowerCase()))).size, 'each import once (the exe\'s and server.dll\'s WSA ones are one)');
  assert.ok([...list.values()].every(e => !e.yes), 'no MW run has reviewed one yet');
  for (const k of ['kernel32.dll!DebugBreak', 'd3dx9_26.dll!D3DXCreateEffectFromResourceA', 'ws2_32.dll!WSARecvFrom', 'shell32.dll!ShellExecuteA'])
    assert.ok(list.has(k), `${k} is listed`);
  // each row is an import of MW's lift, at its IAT
  for (const [mod, dir] of [['speed.exe', 'gen/mw/all'], ['server.dll', 'gen/mw/server']]) {
    const f = path.join(REPO, dir, 'impargs.c');
    if (!fs.existsSync(f)) continue;
    const imps = new Map([...fs.readFileSync(f, 'utf8').matchAll(/\{0x([0-9a-f]+)u,"([^"]+)","([^"]+)",\d+\}/g)].map(m => [`${m[2]}!${m[3]}`, `0x${m[1]}`]));
    for (const r of rows.filter(r => r[0] === mod)) assert.equal(imps.get(r[1]), r[2], `${mod} ${r[1]}: IAT ${r[2]} in ${dir}/impargs.c`);
  }
});

test('the reviewed list: "yes" on every row of an import lets its line pass', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'g01-')), f = path.join(dir, 'list.txt');
  try {
    fs.writeFileSync(f, '# comment\nspeed.exe KERNEL32.dll!DebugBreak 0x00890110 yes mw-boot-1028\n' +
      'speed.exe WS2_32.dll!WSARecv 0x00890488 yes mw-lan-1\nserver.dll WS2_32.dll!WSARecv 0x00d4d1f0 no\n');
    const r = readReviewed('mw', f);
    assert.deepEqual(r.get('kernel32.dll!DebugBreak'), { yes: true, runs: ['mw-boot-1028'] });
    assert.equal(r.get('ws2_32.dll!WSARecv').yes, false, 'a "no" row of the same import');
    const log = '[hle] UNIMPLEMENTED KERNEL32.dll!DebugBreak (first call, from 0x006c1b2e, frame 12)\n' +
      '[hle] UNIMPLEMENTED WS2_32.dll!WSARecv (first call, from 0x0084abd1, frame 400)\n';
    assert.deepEqual(g01(log, 'mw', r), ['unimplemented(WS2_32.dll!WSARecv)']);
    fs.writeFileSync(f, 'speed.exe KERNEL32.dll!DebugBreak 0x00890110 maybe\n');
    assert.throws(() => readReviewed('mw', f), /not "<module> <dll>!<name> <IAT> <yes\|no> \[<run>\]"/);
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});

test('the thrown type, as C++ writes it', () => {
  assert.equal(typeName('.?AVlength_error@std@@'), 'class std::length_error');
  assert.equal(typeName('.?AVout_of_range@std@@'), 'class std::out_of_range');
  assert.equal(typeName('.?AUnode@@'), 'struct node');
  assert.equal(typeName('.?AV?$basic_string@DU?$char_traits@D@std@@@std@@'), '.?AV?$basic_string@DU?$char_traits@D@std@@@std@@', 'a template stays decorated');
});

// one line of each marker, as the runtime and the runners print them
const MARK = {
  fault: '*** FAULT IN GUEST code at 0x004a05af',
  timeout: 'TIME LIMIT REACHED: the web suite runner stopped the run after 400 s',
  trap: '[trap] unsupported instruction \'vpaddd\' at 0x00810b38 (first hit, frame 12)',
  'unresolved-icall': '  [UNRESOLVED INDIRECT CALL] target=0x0a1b2c3d  return=0x0081f9b0',
  'expect-failed': '[expect] FAIL [0x9bb130] > 100',
  'net-blocked': '[net] BLOCKED sendto 10.0.0.2:3658',
  'cxx-throw(class std::length_error)': '*** GUEST RAISEEXCEPTION: code 0xe06d7363 (C++ exception (throw)), flags 0x1, 3 args, from 0x007c56fb (thread 1)\n' +
    '    arg[0] = 0x19930520\n    thrown type .?AVlength_error@std@@',
  'genexec-bail(3)': '  generated guest code: 121348 calls, 61194047 instructions, 3 FAILED (last: 0xd9 @0x1a2b3c4d; seen: d9)',
  'unimplemented(GDI32.dll!CreateFontA)': '[hle] UNIMPLEMENTED GDI32.dll!CreateFontA (first call, from 0x00806d1a, frame 30)',
  'watchdog-stall': '[watchdog] no Present for 15.7 s (game flow 6, 7318 frames so far); guest threads:',
  'init-count': '[init] 5 of 5 C initializers (__xi 0x008eb168-0x008eb180, 6 slots, by 0x007ca570 from 0x007ca2e4), 869 of 870 C++ ' +
    'constructors (__xc 0x008ea000-0x008eb164, 1113 slots, by 0x007ca540 from 0x007ca311); [0x009ab21c] = 0x009a2de0, set',
};

test('a clean log has no reason; one line of each marker gives each reason, in order', () => {
  const clean = '[game] mw: speed.exe in C:\\NFSMW\n[init] 5 of 5 C initializers (__xi 0x008eb168-0x008eb180, 6 slots, by 0x007ca570 ' +
    'from 0x007ca2e4), 870 of 870 C++ constructors (__xc 0x008ea000-0x008eb164, 1113 slots, by 0x007ca540 from 0x007ca311); ' +
    '[0x009ab21c] = 0x009a2de0, set\n[watchdog] log downloaded: the last frame 0.1 s ago (game flow 3, 400 frames so far)\n' +
    '  generated guest code: 121348 calls, 61194047 instructions\n[autotest] quit at guest frame 4640 (quit)\n';
  assert.deepEqual(g01(clean, 'mw'), []);
  for (const [why, line] of Object.entries(MARK)) assert.deepEqual(g01(clean + line + '\n', 'mw'), [why], why);
  assert.deepEqual(g01(clean + Object.values(MARK).join('\n') + '\n', 'mw'), Object.keys(MARK));
  // a line planted by a test script's "log" command (runtime/autotest.c: "[autotest] f=N ... LOG TEXT") fails it too
  for (const why of ['cxx-throw(class std::length_error)', 'genexec-bail(3)', 'unimplemented(GDI32.dll!CreateFontA)', 'watchdog-stall'])
    assert.deepEqual(g01(MARK[why].split('\n').map(l => `[autotest] f=300 t=5000ms w=0.0ms LOG ${l.trim()}`).join('\n'), 'mw'), [why], `planted: ${why}`);
});

test('Underground 2\'s runs: the same markers; no reviewed list, so any unimplemented import fails', () => {
  assert.equal(readReviewed('ug2').size, 0);
  assert.deepEqual(g01(MARK['unimplemented(GDI32.dll!CreateFontA)'], 'ug2'), ['unimplemented(GDI32.dll!CreateFontA)']);
  assert.deepEqual(g01(MARK['watchdog-stall'], 'ug2'), ['watchdog-stall']);
  assert.deepEqual(g01('[autotest] quit at guest frame 4640 (quit)\n', 'ug2'), []);
});
