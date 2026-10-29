// The registry and the paths each game sees (TRACKER M2-12, M2-13), built for wasm and called as lifted code calls
// them:
//   node --test web/test/regpath-wasm.test.mjs        (needs wasi-sdk in deps/)
// web/test/regpath_unit.c links runtime/win32.c, win32_ext.c, win32_misc.c, keyboard.c and fileio.c with a game's
// real table (runtime/mw/game_mw.c and registry_mw.c; runtime/game_ug2.c) and runs in a folder of its own under
// out/web/test (the game folder, the save overlay, $HOME), deleted at the end.  Most Wanted: its registry keys and
// values, FirstTime only once the game wrote it (a first start, then a later one on the same registry file), its
// own registry file, C:\NFSMW and nothing outside it (the bin.dat probe on every drive letter), the game folder never
// written.  UG2: the flat registry and the paths it always had (the same program built on the runtime before
// M2-12/13 gave the same lines: LOG.md).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { CC, buildRegpathUnit } from '../tools/win32unit.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
const skip = !CC && 'no wasi-sdk in deps/';
const EXNREF = spawnSync(process.execPath, ['--experimental-wasm-exnref', '-e', '0']).status === 0 ? ['--experimental-wasm-exnref'] : [];
const MWKEY = 'Software\\EA Games\\Need for Speed Most Wanted';
const SETTINGS = ['VERSION', 'SIZE', 'g_CarEnvironmentMapEnable', 'g_CarEnvironmentMapUpdateData', 'g_RoadReflectionEnable',
  'g_MotionBlurEnable', 'g_ParticleSystemEnable', 'g_WorldLodLevel', 'g_CarLodLevel', 'g_OverBrightEnable', 'g_FSAALevel',
  'g_RainEnable', 'g_TextureFiltering', 'g_RacingResolution', 'FirstTime', 'g_PerformanceLevel', 'g_VSyncOn',
  'g_ShadowDetail', 'g_VisualTreatment'];
const CHOSEN = [1, 132, 3, 1, 3, 1, 1, 3, 1, 1, 7, 1, 2, 1, 1, 5, 0, 2, 1];

/** the import table in regpath_unit.c: name -> nargs */
function unitTable() {
  const src = fs.readFileSync(path.join(REPO, 'web/test/regpath_unit.c'), 'utf8');
  return new Map([...src.matchAll(/\{ IAT\(\d+\),\s*"[^"]+",\s*"([^"]+)",\s*(\d+) \}/g)].map(m => [m[1], +m[2]]));
}
/** a lift's import table (impargs.c): name -> nargs, null when the lift is not here */
function liftTable(dir) {
  const f = path.join(REPO, dir, 'impargs.c');
  if (!fs.existsSync(f)) return null;
  return new Map([...fs.readFileSync(f, 'utf8').matchAll(/\{0x[0-9a-f]+u,"[^"]+","([^"]+)",(\d+)\}/g)].map(m => [m[1], +m[2]]));
}

/** a fresh test folder: game/ (an exe, bin.dat, GLOBAL/GLOBALA.BUN), save/ (its own bin.dat), home/ */
function folder(name, exe) {
  const dir = path.join(OUT, `regpath-${process.pid}`, name);
  fs.rmSync(dir, { recursive: true, force: true });
  for (const d of ['game/GLOBAL', 'game/TRACKS', 'save', 'home']) fs.mkdirSync(path.join(dir, d), { recursive: true });
  fs.writeFileSync(path.join(dir, 'game', exe), '');
  fs.writeFileSync(path.join(dir, 'game/bin.dat'), 'game');
  fs.writeFileSync(path.join(dir, 'game/GLOBAL/GLOBALA.BUN'), 'g');
  fs.writeFileSync(path.join(dir, 'save/bin.dat'), 'save');
  return dir;
}
/** every file under dir: relative path -> contents */
function tree(dir, rel = '', out = new Map()) {
  for (const e of fs.readdirSync(path.join(dir, rel), { withFileTypes: true })) {
    const r = path.join(rel, e.name);
    if (e.isDirectory()) { out.set(`${r}/`, ''); tree(dir, r, out); } else out.set(r, fs.readFileSync(path.join(dir, r), 'latin1'));
  }
  return out;
}
/** runs a mode of the program in dir (the WASI folder /t): its output, errors and checks */
function run(wasm, mode, dir) {
  const r = spawnSync(process.execPath, [...EXNREF, path.join(REPO, 'web/test/wasi-run.mjs'), wasm, mode],
    { encoding: 'utf8', timeout: 60000, env: { ...process.env, WASI_DIR: `/t=${dir}` } });
  const out = r.stdout ?? '', err = r.stderr ?? '';
  if (r.status !== 0) console.log(`${mode}:\n${out}${err}`);
  const m = /regpath_unit: (\d+) passed, (\d+) failed/.exec(out);
  assert.ok(m, `${mode} ran to the end`);
  assert.equal(+m[2], 0, `${mode}: no failed check`);
  assert.equal(r.status, 0, `${mode}: exit 0`);
  return { out, err, passed: +m[1] };
}
const esc = s => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
const built = {};
const wasm = game => built[game] ??= buildRegpathUnit(path.join(OUT, `regpath_${game}.wasm`), game);

test.after(() => fs.rmSync(path.join(OUT, `regpath-${process.pid}`), { recursive: true, force: true }));

test('regpath_unit: the import table\'s stdcall counts are KNOWN\'s and both lifts\'', () => {
  const unit = unitTable(), known = fs.readFileSync(path.join(REPO, 'tools/argcount.py'), 'utf8');
  assert.equal(unit.size, 29, 'the table parsed');
  for (const [name, n] of unit) {
    const k = new RegExp(`'${name}':\\s*(\\d+)`).exec(known);
    assert.ok(k, `${name} is in KNOWN`);
    assert.equal(n, +k[1], `${name}: KNOWN has ${k[1]}`);
  }
  for (const dir of ['gen/all', 'gen/mw/all']) {
    const t = liftTable(dir); if (!t) continue;
    for (const [name, n] of unit) {
      if (name !== 'GetFileAttributesA') assert.ok(t.has(name), `${dir} imports ${name}`);   // no game's: fileio.c's all the same
      if (t.has(name)) assert.equal(t.get(name), n, `${dir}: ${name} pops ${t.get(name)}`);
    }
  }
});

test('Most Wanted\'s registry: its keys, FirstTime written by the game on the first start and found on the next, its own file', { skip }, () => {
  const w = wasm('mw'), dir = folder('mw-reg', 'speed.exe');
  const reg = () => fs.readFileSync(path.join(dir, 'save/registry.txt'), 'latin1');
  // a first start: FirstTime is not there, the game writes it, then SAVE_SETTINGS (after the auto-detect) saves
  const b1 = run(w, 'boot1', dir);
  assert.ok(b1.passed >= 150, `boot1: every check ran (${b1.passed})`);
  for (const l of [
    `[reg] RegOpenKeyA(HKLM\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0, access 0xf003f) -> 0x`,
    `[reg] HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0\\~MHz = 3000 (REG_DWORD, the runtime's)`,
    `[reg] RegOpenKeyExA(HKLM\\Software\\Electronic Arts\\EA Games\\Need for Speed Most Wanted\\ergc): no such key, error 2`,
    `[reg] RegOpenKeyExA(HKLM\\${MWKEY}, access 0x2001f) -> 0x`,
    `[reg] ${MWKEY}\\Install Dir = "C:\\NFSMW" (REG_SZ, the runtime's)`,
    `[reg] ${MWKEY}\\CD Drive: not there, error 2`,
    `[reg] ${MWKEY}\\FirstTime: not there, error 2`,
    `[reg] ${MWKEY}\\FirstTime := "\\x01" (REG_SZ, 4 bytes)`,
    `[reg] ${MWKEY}\\Language = "English US" (REG_SZ, the runtime's)`,
    `[reg] ${MWKEY}\\VERSION := 1 (REG_DWORD)`,
    `[reg] ${MWKEY}\\SIZE := 132 (REG_DWORD)`,
    `[reg] ${MWKEY}\\FirstTime := 1 (REG_DWORD)`,
  ]) assert.match(b1.err, new RegExp(`^${esc(l)}`, 'm'), l);
  let lines = reg().split('\n').filter(Boolean);
  SETTINGS.forEach((v, i) => assert.ok(lines.includes(`d ${v}=${CHOSEN[i]}`), `registry.txt: d ${v}=${CHOSEN[i]}`));
  assert.ok(!lines.some(l => /Install Dir|Language|~MHz|CD Drive/.test(l)), 'the runtime\'s answers are not stored');
  // a later start on the same file: FirstTime is there, LOAD_SETTINGS reads what was saved, German from the page
  const b2 = run(w, 'boot2', dir);
  for (const l of [`[reg] ${MWKEY}\\FirstTime = 1 (REG_DWORD, stored)`, `[reg] ${MWKEY}\\Language = "German" (REG_SZ, the runtime's)`,
    `[reg] HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0\\~MHz = 2400 (REG_DWORD, the runtime's)`,
    `[reg] RegOpenKeyExA(HKLM\\${MWKEY}, access 0x20019) -> 0x`, `[reg] ${MWKEY}\\g_FSAALevel = 7 (REG_DWORD, stored)`,
    `[reg] ${MWKEY}\\VTMode: not there, error 2`]) assert.match(b2.err, new RegExp(`^${esc(l)}`, 'm'), l);
  assert.doesNotMatch(b2.err, /FirstTime: not there/, 'boot 2 finds FirstTime');
  assert.ok(reg().split('\n').includes('s FirstTime=\x01'), 'the start-up\'s REG_SZ FirstTime is kept as the game wrote it');
  // and the next start reads that one back
  const b3 = run(w, 'boot2', dir);
  assert.match(b3.err, new RegExp(`^${esc(`[reg] ${MWKEY}\\FirstTime = "\\x01" (REG_SZ, stored)`)}`, 'm'));
  // the key table's rules
  const k = run(w, 'keys', dir);
  assert.ok(k.passed >= 200, `keys: every check ran (${k.passed})`);
  assert.match(k.err, /^\[reg\] language "russian": the game has no text in it, English US it is$/m);
  assert.match(k.err, new RegExp(`^${esc(`[reg] RegSetValueExA(${MWKEY}\\g_VSyncOn, type 4, 4 bytes): opened without KEY_SET_VALUE, error 5`)}`, 'm'));
  assert.match(k.err, /^\[reg\] RegCreateKeyA\(HKLM\\Software\\Other\): this machine makes no keys of its own, error 5/m);
  // without NFSU2_REGFILE: $HOME/.nfsmw_registry, never UG2's file
  run(w, 'home', dir);
  assert.equal(fs.readFileSync(path.join(dir, 'home/.nfsmw_registry'), 'utf8'), 'd g_RainEnable=1\n');
  assert.ok(!fs.existsSync(path.join(dir, 'home/.nfsu2_registry')), 'no ~/.nfsu2_registry');
});

test('Most Wanted\'s paths: C:\\NFSMW and nothing outside it, the bin.dat probe not found on any drive, the folder never written', { skip }, () => {
  const w = wasm('mw'), dir = folder('mw-paths', 'speed.exe');
  const game = tree(path.join(dir, 'game'));
  const p = run(w, 'paths', dir);
  assert.ok(p.passed >= 1000, `paths: every check ran (${p.passed})`);
  for (let l = 0; l < 26; l++) {
    const d = String.fromCharCode(65 + l), e = d === 'C' || d === 'D' ? 2 : 3;
    for (const call of ['CreateFileA', 'GetFileAttributesA', 'FindFirstFileA'])
      assert.match(p.err, new RegExp(`^${esc(`[file] ${call}("${d}:\\bin.dat"): names no file of C:\\NFSMW, error ${e}`)}`, 'm'), `${call} ${d}:`);
  }
  assert.match(p.err, /^\[write\] CreateFileA\(CREATE_ALWAYS, 0x40000000\) "C:\\NFSMW\\NFS Most Wanted\\ABC\\ABC" -> ".*\/save\/NFS Most Wanted\/ABC\/ABC"$/m);
  assert.equal(fs.readFileSync(path.join(dir, 'save/NFS Most Wanted/ABC/ABC'), 'utf8'), '20CM', 'the save is in the save overlay');
  assert.ok(fs.existsSync(path.join(dir, 'save/nfsmw000.tga')), 'the screenshot too');
  assert.deepEqual(tree(path.join(dir, 'game')), game, 'the game folder unchanged');
  assert.ok(![...tree(dir).keys()].some(f => /evil|moved/.test(f)), 'no write outside reached the disk');
  assert.ok(!fs.existsSync(path.join(OUT, `regpath-${process.pid}`, 'evil.txt')), 'nothing next to the test folder');
  // no save overlay: the game folder stays read-only
  const r = run(w, 'ro', dir);
  assert.match(r.err, /^\[write\] CreateFileA\(CREATE_ALWAYS, 0x40000000\) "C:\\NFSMW\\new\.txt" -> ".*\/game\/new\.txt"  BLOCKED \(game folder is read-only\)$/m);
  assert.deepEqual(tree(path.join(dir, 'game')), game, 'the game folder unchanged without a save overlay');
});

test('Most Wanted\'s folder scans: NTFS order whatever the host\'s, a deleted alias gone, the game folder and the overlay in one order', { skip }, () => {
  const w = wasm('mw'), dir = folder('mw-scan', 'speed.exe');
  const s = run(w, 'scan', dir);
  assert.ok(s.passed >= 100, `scan: every check ran (${s.passed})`);
  assert.match(s.out, /^T scan "C:\\NFSMW\\NFS Most Wanted\\\*": AB Alpha a_b bravo CHARLIE _x$/m);
  assert.match(s.out, /^T scan "C:\\NFSMW\\NFS Most Wanted\/\*": AB a_b bravo CHARLIE _x$/m);
  assert.deepEqual([...tree(path.join(dir, 'save')).keys()].filter(k => k.startsWith('NFS Most Wanted/') && k !== 'NFS Most Wanted/').sort(),
    ['NFS Most Wanted/AB/', 'NFS Most Wanted/CHARLIE/', 'NFS Most Wanted/_x/', 'NFS Most Wanted/a_b/', 'NFS Most Wanted/bravo/'],
    'the save overlay holds the folders left');
});

test('UG2\'s registry and paths: the flat registry, every path into the game folder, ~/.nfsu2_registry, as always', { skip }, () => {
  const w = wasm('ug2'), dir = folder('ug2', 'SPEED2.EXE');
  const u = run(w, 'ug2', dir);
  assert.ok(u.passed >= 500, `ug2: every check ran (${u.passed})`);
  assert.doesNotMatch(u.err, /^\[reg\]|names no file of/m, 'none of Most Wanted\'s lines');
  for (const l of ['T CreateFileA("D:\\bin.dat", 0x80000000, 3) -> a handle, error 0', 'T CreateFileA("..\\bin.dat", 0x80000000, 3) -> a handle, error 0',
    'T GetModuleHandleA("speed.exe") -> 0x00000000, error 126', 'T GetModuleFileNameA -> 21 "C:\\\\NFSU2\\\\SPEED2.EXE"',
    'T query "VERSION" -> 0, type 1 "1.2.0.0", cb 8', 'T RegOpenKeyExA(0x80000001, "No\\Such\\Key", 0x20019) -> 0, hkey set'])
    assert.ok(u.out.split('\n').includes(l), l);
  assert.ok(fs.existsSync(path.join(dir, 'save/fromd.txt')), 'D:\\ maps into the save overlay, as always');
  run(w, 'ug2home', dir);
  assert.equal(fs.readFileSync(path.join(dir, 'home/.nfsu2_registry'), 'utf8'), 'd g_RainEnable=1\n');
  assert.ok(!fs.existsSync(path.join(dir, 'home/.nfsmw_registry')), 'no ~/.nfsmw_registry');
  run(w, 'ug2nosave', dir);
  assert.ok(fs.existsSync(path.join(dir, 'game/new.txt')), 'without a save overlay UG2 writes into its folder, as it always could');
});
