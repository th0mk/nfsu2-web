// web/tools/ug2-same.sh's gameinfo.h check in stub trees (review round 2): the
// lift's all/gameinfo.h and the copy's runtime/gameinfo_ug2.h must both have
// the #define lines of the baseline's gameinfo.expect (main's values), so a
// lifter change made together with the same change to gameinfo_ug2.h is
// caught; a baseline without gameinfo.expect is refused; --make-expect writes
// it from main's sources at a commit (a stand-in repository here) and stops
// when one of them no longer reads as expected.  The lift is a stand-in
// web/gen.sh (--no-copy) that copies its files from the copy's lift/.
//   node --test web/test/ug2-same.test.mjs
import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawnSync, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const base = fs.mkdtempSync(path.join(os.tmpdir(), 'nfsu2-ug2same-'));
after(() => fs.rmSync(base, { recursive: true, force: true }));

let n = 0;
const w = (t, p, s = '', mode) => { const f = path.join(t, p); fs.mkdirSync(path.dirname(f), { recursive: true }); fs.writeFileSync(f, s); if (mode) fs.chmodSync(f, mode); };
const EXE = 'not really SPEED2.EXE';
const h16 = s => crypto.createHash('sha256').update(s).digest('hex').slice(0, 16);
const BODY = `overrides 0 (0)\nhooks 0 (0)\nlifter 0\nexe ${h16(EXE)} server.dll 0000000000000000\noptions X87PC=1 FLAGABI=0\n`;
// a UG2 gameinfo.h as tools/genall.py writes it, and the runtime's fallback header
const GAMEINFO = `/* The lifted exe (tools/genall.py) */
#ifndef GAMEINFO_H
#define GAMEINFO_H
#define GAMEINFO_IMAGE_BASE 0x00400000u   /* ImageBase */
#define GAMEINFO_IMAGE_SIZE 0x00532000u   /* SizeOfImage: the image ends at 0x00932000 */
#define GAMEINFO_TEXT_BASE  0x00401000u   /* .text: one dispatch table slot per byte */
#define GAMEINFO_TEXT_SIZE  0x00381af1u
#define GAMEINFO_ENTRY      0x0075bcc7u   /* the entry point, lifted as GAMEINFO_ENTRY_FN */
#define GAMEINFO_ENTRY_FN   sub_0075bcc7
#define GAMEINFO_FTOL2      0x0075bed4u   /* the C runtime's _ftol2 (FTOL2_VA) */
#endif
`;
const FALLBACK = GAMEINFO.replace('The lifted exe (tools/genall.py)', 'SPEED2.EXE for a lift without gameinfo.h (runtime/gameinfo_ug2.h)');
const DEFINES = ['#define GAMEINFO_IMAGE_BASE 0x00400000u', '#define GAMEINFO_IMAGE_SIZE 0x00532000u', '#define GAMEINFO_TEXT_BASE 0x00401000u',
  '#define GAMEINFO_TEXT_SIZE 0x00381af1u', '#define GAMEINFO_ENTRY 0x0075bcc7u', '#define GAMEINFO_ENTRY_FN sub_0075bcc7', '#define GAMEINFO_FTOL2 0x0075bed4u'];
const EXPECT = `# UG2's exe values as main's runtime and lifter have them\n${DEFINES.join('\n')}\n`;

/** a checkout with ug2-same, a baseline in base/ (gameinfo.expect unless expect is null) and the copy a last
 *  run left, whose lift writes lift/gameinfo.h; fallback is the copy's runtime/gameinfo_ug2.h */
function tree({ gameinfo = GAMEINFO, fallback = FALLBACK, expect = EXPECT } = {}) {
  const t = path.join(base, `t${n++}`);
  for (const f of ['ug2-same.sh', 'ug2-same.allow']) w(t, `web/tools/${f}`, fs.readFileSync(path.join(ROOT, 'web/tools', f)), 0o755);
  w(t, 'SPEED2.EXE', EXE);
  const stamp = `# gen.sh 2026-10-26T00:00:00Z: what gen/all and gen/server were lifted from\n${BODY}`;
  w(t, 'base/all/x.c', 'int x;\n'); w(t, 'base/all/web-lift.txt', 'FLAGABI X87OPT\n'); w(t, 'base/all/stamp.txt', stamp);
  w(t, 'base/server/y.c', 'int y;\n'); w(t, 'base/server/stamp.txt', stamp);
  if (expect !== null) w(t, 'base/gameinfo.expect', expect);
  const c = 'out/mw/ug2-same/tree';
  w(t, `${c}/web/gen.sh`, `#!/bin/bash
cp lift/x.c lift/web-lift.txt lift/gameinfo.h gen/all/; cp lift/y.c gen/server/
{ echo "# gen.sh $(date -u +%Y-%m-%dT%H:%M:%SZ): what gen/all and gen/server were lifted from"; cat lift/stamp.body; } > gen/all/stamp.txt
cp gen/all/stamp.txt gen/server/stamp.txt
`, 0o755);
  w(t, `${c}/lift/x.c`, 'int x;\n'); w(t, `${c}/lift/web-lift.txt`, 'FLAGABI X87OPT\n'); w(t, `${c}/lift/y.c`, 'int y;\n');
  w(t, `${c}/lift/stamp.body`, BODY); w(t, `${c}/lift/gameinfo.h`, gameinfo);
  if (fallback !== null) w(t, `${c}/runtime/gameinfo_ug2.h`, fallback);
  return t;
}
const envWith = (extra = {}) => {
  const e = { ...process.env };
  for (const k of ['UG2_BASE', 'NFSU2_DATA', 'GAME', 'NFSU2_GAME', 'GIT_DIR', 'GIT_WORK_TREE']) delete e[k];
  return { ...e, ...extra };
};
const same = (t, args = ['--no-copy', path.join(t, 'SPEED2.EXE')], extra = {}) => {
  const r = spawnSync('bash', [path.join(t, 'web/tools/ug2-same.sh'), ...args], { cwd: t, encoding: 'utf8', timeout: 60000, env: envWith({ UG2_BASE: path.join(t, 'base'), ...extra }) });
  return { code: r.status, out: (r.stdout ?? '') + (r.stderr ?? '') };
};

test('ug2-same: a lift whose gameinfo.h and gameinfo_ug2.h have main\'s values is identical (comments and spacing aside)', () => {
  for (const gameinfo of [GAMEINFO, GAMEINFO.replace(/   \/\*[^\n]*\*\//g, '').replace(/ +/g, ' ')]) {
    const r = same(tree({ gameinfo }));
    assert.equal(r.code, 0, r.out);
    assert.match(r.out, /ug2-same: identical to /);
    assert.match(r.out, /new \(allowed in web\/tools\/ug2-same\.allow\): all\/gameinfo\.h/);
  }
});

test('ug2-same: a lifter change made together with the same change to gameinfo_ug2.h differs from main', () => {
  const bump = s => s.replace('0x00381af1u', '0x00381af2u');
  const r = same(tree({ gameinfo: bump(GAMEINFO), fallback: bump(FALLBACK) }));
  assert.equal(r.code, 1, r.out);
  assert.match(r.out, /differs  all\/gameinfo\.h: its #define lines are not main's/);
  assert.match(r.out, /differs  runtime\/gameinfo_ug2\.h: its #define lines are not main's/);
  assert.match(r.out, /\(2 differences;/);
});

test('ug2-same: either header alone off main\'s values, a #define more, or no gameinfo_ug2.h is named', () => {
  const bump = s => s.replace('0x0075bed4u', '0x0075bed5u');
  let r = same(tree({ fallback: bump(FALLBACK) }));
  assert.equal(r.code, 1, r.out);
  assert.match(r.out, /differs  runtime\/gameinfo_ug2\.h/); assert.doesNotMatch(r.out, /differs  all\/gameinfo\.h/);
  r = same(tree({ gameinfo: bump(GAMEINFO) }));
  assert.equal(r.code, 1, r.out);
  assert.match(r.out, /differs  all\/gameinfo\.h/); assert.doesNotMatch(r.out, /differs  runtime\/gameinfo_ug2\.h/);
  r = same(tree({ gameinfo: GAMEINFO.replace('#endif', '#define GAMEINFO_TEXT_END 0x00782af1u\n#endif') }));
  assert.equal(r.code, 1, r.out); assert.match(r.out, /differs  all\/gameinfo\.h/);
  r = same(tree({ fallback: null }));
  assert.equal(r.code, 1, r.out); assert.match(r.out, /differs  runtime\/gameinfo_ug2\.h/);
});

test('ug2-same: a baseline without gameinfo.expect, or one with no #define line, is refused before any lift', () => {
  let t = tree({ expect: null });
  let r = same(t);
  assert.equal(r.code, 2, r.out);
  assert.match(r.out, /no .*base\/gameinfo\.expect: make it with web\/tools\/ug2-same\.sh --make-expect <main's commit/);
  assert.ok(!fs.existsSync(path.join(t, 'out/mw/ug2-same/tree/gen/all/x.c')), 'no lift ran');
  t = tree({ expect: '# nothing\n' });
  r = same(t);
  assert.equal(r.code, 2, r.out); assert.match(r.out, /gameinfo\.expect has no #define lines/);
});

/** a stand-in for main: a git repository holding ug2-same and the lines of main's sources --make-expect reads */
function mainRepo(t, { cpu = '#define DISPATCH_TEXT_BASE 0x00401000u\n#define DISPATCH_TEXT_SIZE 0x00381af1u\n' } = {}) {
  w(t, 'runtime/cpu.h', `/* dispatch */\n${cpu}extern guest_fn_t g_dtab[DISPATCH_TEXT_SIZE];\n`);
  w(t, 'runtime/heap.c', '        int img = pg >= 0x00400000u && pg < 0x00932000u;\n        else if (!(a >= 0x00400000u && a < 0x00932000u)) old = 4u;\n');
  w(t, 'runtime/web/main_web.c', 'static void run(void) {\n        sub_0075bcc7(&cpu);\n}\n');
  w(t, 'tools/lift.py', '                if t == 0x0075bed4 and _FTOL2INL:\n');
  const git = (...a) => execFileSync('git', ['-c', 'user.name=t', '-c', 'user.email=t@example.invalid', '-c', 'commit.gpgsign=false', '-c', 'core.hooksPath=/dev/null', ...a],
    { cwd: t, encoding: 'utf8', env: envWith() }).trim();
  if (!fs.existsSync(path.join(t, '.git'))) git('init', '-q');
  git('add', '-A'); git('commit', '-qm', 'main');
  return git('rev-parse', 'HEAD');
}

test('ug2-same --make-expect writes main\'s values from main\'s sources at a commit, as the #define lines a lift has', () => {
  const t = tree({ expect: null });
  const head = mainRepo(t);
  const r = same(t, ['--make-expect', head]);
  assert.equal(r.code, 0, r.out);
  const made = fs.readFileSync(path.join(t, 'base/gameinfo.expect'), 'utf8');
  assert.deepEqual(made.split('\n').filter(l => l.startsWith('#define')), DEFINES);
  assert.match(made, new RegExp(`^# .* at ${head} \\(${head}\\):$`, 'm'));
  // the baseline with it: the lift above is identical
  const ok = same(t);
  assert.equal(ok.code, 0, ok.out);
  assert.match(ok.out, /identical to /);
});

test('ug2-same --make-expect stops when main\'s source no longer reads as expected, or with no commit', () => {
  const t = tree();
  const head = mainRepo(t, { cpu: '#include "gameinfo.h"\n#define DISPATCH_TEXT_BASE GAMEINFO_TEXT_BASE\n' });
  const r = same(t, ['--make-expect', head]);
  assert.equal(r.code, 2, r.out);
  assert.match(r.out, /--make-expect: runtime\/cpu\.h at [0-9a-f]+ has no single line matching/);
  assert.equal(fs.readFileSync(path.join(t, 'base/gameinfo.expect'), 'utf8'), EXPECT, 'the expect file is left as it was');
  for (const args of [['--make-expect'], ['--make-expect', 'no-such-commit']]) {
    const u = same(t, args);
    assert.equal(u.code, 2, u.out); assert.match(u.out, /usage: web\/tools\/ug2-same\.sh --make-expect|no-such-commit is not a commit/);
  }
});
