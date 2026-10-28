// Most Wanted's overlapped Winsock (TRACKER M8-06, M8-07), built for wasm and called as lifted code calls it:
//   node --test web/test/wsaov-wasm.test.mjs        (needs wasi-sdk in deps/)
// web/test/wsaov_unit.c links runtime/winsock.c over the browser's virtual LAN (runtime/web/net_lan.c) with the
// Win32 layer's objects (win32.c, win32_ext.c, win32_misc.c, keyboard.c): WSA events, WSAWaitForMultipleEvents,
// WSARecv / WSARecvFrom with an event OVERLAPPED, WSAGetOverlappedResult, cancellation on close, the LAN mailbox's
// want word, and the call sequence of DirtySock's socket thread (speed.exe 0x84AC10, server.dll 0x100043C0).  Its
// import table's stdcall counts are checked here against argcount's KNOWN and the lifts in this worktree (MW's
// gen/mw/all and gen/mw/server).  The threaded half runs in the browser host: web/tools/wsastress.sh.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { CC, buildWsaovUnit } from '../tools/win32unit.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
const skip = !CC && 'no wasi-sdk in deps/';
const EXNREF = spawnSync(process.execPath, ['--experimental-wasm-exnref', '-e', '0']).status === 0 ? ['--experimental-wasm-exnref'] : [];

function unitTable() {
  const src = fs.readFileSync(path.join(REPO, 'web/test/wsaov_unit.c'), 'utf8');
  const t = new Map();
  for (const m of src.matchAll(/\{ IAT\(\d+\),\s*"([^"]+)",\s*"([^"]+)",\s*(\d+) \}/g)) t.set(m[2], { dll: m[1], nargs: +m[3] });
  return t;
}
function knownCounts() {
  const src = fs.readFileSync(path.join(REPO, 'tools/argcount.py'), 'utf8');
  const k = new Map();
  for (const m of src.matchAll(/'([A-Za-z_][A-Za-z0-9_]*)':\s*(\d+)/g)) if (!k.has(m[1])) k.set(m[1], +m[2]);
  return k;
}
function liftTable(dir) {
  const f = path.join(REPO, dir, 'impargs.c');
  if (!fs.existsSync(f)) return null;
  return new Map([...fs.readFileSync(f, 'utf8').matchAll(/\{0x[0-9a-f]+u,"([^"]+)","([^"]+)",(\d+)\}/g)].map(m => [m[2], +m[3]]));
}
// the overlapped imports (GAME.md 3.3): speed.exe and server.dll import all eight
const WSA8 = ['WSACreateEvent', 'WSACloseEvent', 'WSASetEvent', 'WSAResetEvent', 'WSARecv', 'WSARecvFrom', 'WSAGetOverlappedResult',
  'WSAWaitForMultipleEvents'];

test('wsaov_unit: the import table\'s stdcall counts are argcount\'s KNOWN ones and MW\'s lifts\'', () => {
  const unit = unitTable(), known = knownCounts();
  assert.equal(unit.size, 26, 'the table parsed');
  for (const [name, { nargs }] of unit) {
    assert.ok(known.has(name), `${name} is in KNOWN`);
    assert.equal(nargs, known.get(name), `${name}: KNOWN has ${known.get(name)}`);
  }
  for (const k of WSA8) assert.ok(unit.has(k), `${k} is in the unit table`);
  for (const dir of ['gen/mw/all', 'gen/mw/server']) {
    const t = liftTable(dir); if (!t) continue;
    for (const k of WSA8) {
      assert.ok(t.has(k), `${dir} imports ${k}`);
      assert.equal(t.get(k), unit.get(k).nargs, `${dir}: ${k} pops ${t.get(k)}`);
    }
  }
});

test('wsaov_unit hle: WSA events and overlapped receives answer as Windows does, the stack balanced', { skip }, () => {
  const w = buildWsaovUnit(path.join(OUT, 'wsaov_unit.wasm'));
  const r = spawnSync(process.execPath, [...EXNREF, path.join(REPO, 'web/test/wasi-run.mjs'), w, 'hle'], { encoding: 'utf8', timeout: 120000 });
  const out = r.stdout ?? '', err = r.stderr ?? '';
  if (r.status !== 0) console.log(out + err);
  const m = /wsaov_unit: (\d+) passed, (\d+) failed/.exec(out);
  assert.ok(m, 'wsaov_unit ran to the end');
  assert.equal(+m[2], 0, 'no failed check');
  assert.ok(+m[1] >= 7000, `every check ran (${m[1]})`);
  assert.equal(r.status, 0);
  // the log says when the overlapped calls are first used, and what was refused
  assert.match(err, /^\[net\] WSAWaitForMultipleEvents: first wait, 1 object\(s\), wait for any, a timeout, from 0x00401234$/m);
  assert.match(err, /^\[net\] overlapped: first WSARecvFrom pending \(socket 0x[0-9a-f]+, event 0x00a[0-9a-f]{5}\) from 0x00401234$/m);
  assert.match(err, /^\[net\] WSARecv: completion routine 0x00401000 refused \(no APCs\) -> WSAEOPNOTSUPP$/m);
  assert.doesNotMatch(err, /UNIMPLEMENTED|no kernel objects/);
});
