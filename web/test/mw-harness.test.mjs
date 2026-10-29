// Most Wanted's boot count, test harness and thread names, without a boot (TRACKER M2-17, M2-18, M2-20):
//   node --test web/test/mw-harness.test.mjs      (needs clang, MW's lift in gen/mw/all and speed.exe: NFSMW_DATA, else
//                                                  the ignored mw/ folder of this checkout or of the main clone)
// tests/mw_harness_unit.c links MW's real game table (runtime/mw/game_mw.c, fe_mw.c, threads_mw.c) with the real
// dispatch.c and watchdog.c, natively.  This file writes out/web/test/mw_harness_gen.c for it from the exe and the
// lift: the words of the exe's initializer tables (0x8EA000-0x8EB198), the lifted _initterm (sub_007ca540) and
// _initterm_e (sub_007ca570) as gen/mw/all has them, and the lift's function table with the tables' functions as
// stand-ins that count their calls.  Nothing of it goes into the repository (out/ is not tracked).
// What the [init] line must say is counted here, from the exe, apart from the unit: 870 of the 1,113 slots of __xc
// and 5 of the 6 of __xi hold a function (GAME.md 2.6).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.join(REPO, 'out/web/test');
const LIFT = path.join(REPO, 'gen/mw/all');
const mainClone = () => {
  const r = spawnSync('git', ['-C', REPO, 'rev-parse', '--path-format=absolute', '--git-common-dir'], { encoding: 'utf8' });
  return r.status === 0 ? path.dirname(r.stdout.trim()) : REPO;
};
const DATA = process.env.NFSMW_DATA || [REPO, mainClone()].map(d => path.join(d, 'mw/Need For Speed Most Wanted Black Edition'))
  .find(d => fs.existsSync(path.join(d, 'speed.exe')));
const EXE = DATA && path.join(DATA, 'speed.exe');
const skip = spawnSync('clang', ['--version']).status !== 0 ? 'no clang'
  : !fs.existsSync(path.join(LIFT, 'fntab.c')) ? 'no gen/mw/all (GAME=mw web/gen.sh <speed.exe> lifts it)'
  : !EXE || !fs.existsSync(EXE) ? 'no speed.exe (NFSMW_DATA=<the game folder>)' : false;

const TABLES = [0x8ea000, 0x8eb198];               // __xc, __xi and the two exit tables
const XC = [0x8ea000, 0x8eb164], XI = [0x8eb168, 0x8eb180];

/** the exe's bytes at virtual addresses [lo, hi) (PE32: the section that holds them) */
function exeBytes(file, lo, hi) {
  const b = fs.readFileSync(file);
  const pe = b.readUInt32LE(0x3c), nsec = b.readUInt16LE(pe + 6), opt = pe + 24, base = b.readUInt32LE(opt + 28);
  const sec = opt + b.readUInt16LE(pe + 20);
  for (let i = 0; i < nsec; i++) {
    const s = sec + 40 * i, va = base + b.readUInt32LE(s + 12), vs = b.readUInt32LE(s + 8);
    const raw = b.readUInt32LE(s + 16), ptr = b.readUInt32LE(s + 20);
    if (lo >= va && hi <= va + Math.max(vs, raw)) {
      const out = Buffer.alloc(hi - lo);
      const n = Math.max(0, Math.min(hi, va + raw) - lo);
      b.copy(out, 0, ptr + (lo - va), ptr + (lo - va) + n);   // past the raw data: zeros, as loaded
      return out;
    }
  }
  throw new Error(`0x${lo.toString(16)} is in no section of ${file}`);
}
/** the text of a lifted function, as its shard has it */
function lifted(name) {
  for (const f of fs.readdirSync(LIFT).filter(f => /^shard_\d+\.c$/.test(f))) {
    const src = fs.readFileSync(path.join(LIFT, f), 'utf8');
    const i = src.indexOf(`/* ===== ${name} `);
    if (i < 0) continue;
    const j = src.indexOf('/* ===== ', i + 10);
    return src.slice(i, j < 0 ? undefined : j);
  }
  throw new Error(`${name} is not in ${LIFT}`);
}
const hex = v => `0x${v.toString(16).padStart(8, '0')}u`;

let built = null;
function build() {
  if (built) return built;
  fs.mkdirSync(OUT, { recursive: true });
  const words = exeBytes(EXE, ...TABLES);
  const word = a => words.readUInt32LE(a - TABLES[0]);
  const slots = ([lo, hi]) => Array.from({ length: (hi - lo) / 4 }, (_, i) => word(lo + 4 * i));
  const xi = slots(XI).filter(Boolean), xc = slots(XC).filter(Boolean);
  // the lift's functions; the tables' are the stand-ins
  const fntab = fs.readFileSync(path.join(LIFT, 'fntab.c'), 'utf8');
  const addrs = [...new Set([...(/g_fn_addr\[\] = \{([\s\S]*?)\};/.exec(fntab)[1]).matchAll(/0x([0-9a-f]+)u/g)].map(m => parseInt(m[1], 16))
    .concat(xi, xc))].sort((a, b) => a - b);
  const xiSet = new Set(xi), xcSet = new Set(xc);
  const fn = a => a === 0x889dd0 ? 'bitset_ctor' : xiSet.has(a) ? 'xi_stub' : xcSet.has(a) ? 'xc_stub' : 'named_stub';
  const rows = (list, f) => list.map(f).reduce((o, s, i) => o + s + (i % 8 === 7 ? ',\n  ' : ','), '  ');
  const gen = `/* written by web/test/mw-harness.test.mjs from ${path.basename(EXE)} and gen/mw/all: not tracked */
#define NFSU2_LREGS 1
#include "cpu.h"
#include "decls.h"
void xi_stub(CPU *c); void xc_stub(CPU *c); void bitset_ctor(CPU *c); void named_stub(CPU *c);
const uint32_t mwt_ntables = ${words.length / 4};
const uint32_t mwt_tables[] = {
${rows(Array.from({ length: words.length / 4 }, (_, i) => words.readUInt32LE(4 * i)), hex)}
};
const uint32_t g_fn_count = ${addrs.length};
const uint32_t g_fn_addr[] = {
${rows(addrs, hex)}
};
const guest_fn_t g_fn_ptr[] = {
${rows(addrs, fn)}
};
${lifted('sub_007ca540')}
${lifted('sub_007ca570')}
`;
  const genFile = path.join(OUT, 'mw_harness_gen.c'), exe = path.join(OUT, 'mw_harness_unit');
  fs.writeFileSync(genFile, gen);
  const b = spawnSync('clang', ['-O1', '-g', '-Wall', '-Wno-unused-parameter', '-Wno-unused-label', '-Wno-parentheses-equality',
    '-Wno-unused-function', '-Wno-unused-but-set-variable', '-Wno-unused-variable', '-DNFSU2_GAME_MW', '-Iruntime', `-I${LIFT}`, '-o', exe, 'tests/mw_harness_unit.c', 'runtime/dispatch.c',
    'runtime/mw/game_mw.c', 'runtime/mw/fe_mw.c', 'runtime/mw/threads_mw.c', 'runtime/mw/sound_mw.c', genFile], { cwd: REPO, encoding: 'utf8' });
  assert.equal(b.status, 0, `tests/mw_harness_unit.c builds:\n${b.stderr}`);
  built = { exe, xi: xi.length, xc: xc.length, xiSlots: slots(XI).length, xcSlots: slots(XC).length };
  return built;
}
/** runs a mode: its output, every check passed */
function run(mode) {
  const { exe } = build();
  const r = spawnSync(exe, [mode], { cwd: REPO, encoding: 'utf8', timeout: 60000 });
  const out = (r.stdout ?? '') + (r.stderr ?? '');
  assert.doesNotMatch(out, /^FAIL /m, out);
  assert.equal(r.status, 0, out);
  assert.match(out, new RegExp(`mw_harness_unit ${mode}: all passed`));
  return { out, pass: [...out.matchAll(/^PASS (\S+): (.*)$/mg)].map(m => m[2]) };
}

test('the exe\'s initializer tables: 5 of 6 and 870 of 1,113 slots hold a function (GAME.md 2.6)', { skip }, () => {
  const b = build();
  assert.deepEqual([b.xi, b.xiSlots, b.xc, b.xcSlots], [5, 6, 870, 1113]);
});

test('M2-17: the lifted _initterm_e and _initterm run the exe\'s tables; the [init] line counts 5 of 5 and 870 of 870', { skip }, () => {
  const { out, pass } = run('init');
  assert.match(out, /^line: \[init\] 5 of 5 C initializers \(__xi 0x008eb168-0x008eb180, 6 slots, by 0x007ca570 from 0x007ca2e4\), 870 of 870 C\+\+ constructors \(__xc 0x008ea000-0x008eb164, 1113 slots, by 0x007ca540 from 0x007ca311\); \[0x009ab21c\] = 0x009a2de0, set$/m);
  assert.ok(pass.length >= 9, `every check ran (${pass.length})`);
});

test('M2-17: a C initializer that fails stops _initterm_e and the line says where; a NULL check; a boot that stops early', { skip }, () => {
  assert.match(run('stop').out, /^line: \[init\] 3 of 5 C initializers \(.*; stopped: slot 0x008eb174 returned 0x0000001a\), 0 of 870 C\+\+ constructors \(.*; not run\); \[0x009ab21c\] = 0: NULL/m);
  assert.match(run('nobitset').out, /^line: \[init\] 5 of 5 .*, 870 of 870 C\+\+ constructors .*; \[0x009ab21c\] = 0: NULL, a constructor did not run its body$/m);
  assert.match(run('exit').out, /^line: \[init\] 5 of 5 C initializers .*, 0 of 870 C\+\+ constructors .*; not run\).*; at exit: the tables did not finish$/m);
});

test('M2-18: GameFlow, the top package and the screens of each frame, over a front end built as FEngine keeps it', { skip }, () => {
  const { pass } = run('fe');
  assert.ok(pass.some(p => p.startsWith('the main menu: the tail is on top (MainMenu_Sub.fng)')), pass.join('\n'));
  assert.ok(pass.length >= 9, `every check ran (${pass.length})`);
});

test('M2-20: the threads by the routines they run, and a forced stall report naming each with eip, wait and the GIL holder', { skip }, () => {
  const { out, pass } = run('threads');
  assert.match(out, /^ {2}thread 1 tid=102 timer \(0x007ec601\): waiting on handle 0x00a00004 .* eip=0x007ec613 \(sub_007ec601\+0x12\)$/m);
  assert.ok(pass.length >= 17, `every check ran (${pass.length})`);
});
