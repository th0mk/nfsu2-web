// UG2's game table (runtime/game_ug2.c's g_game) sets every field of GameOps (runtime/game.h) but the ones it leaves 0
// on purpose, and the page's events reach their features through it.  Core calls a field only when it is set, so a
// field dropped from the table fails nowhere: the page's photo mode (IEV_PHOTO, input_web.c) or text messages
// (IEV_SMS) would just do nothing, and the test scripts' "photo ..." calls photo.c directly, so run-suite photo
// would still pass.  A new field must be named here as set or as left 0 for UG2, and for Most Wanted.
// Most Wanted's table (runtime/mw/game_mw.c) is held to the same rule, with its own list of fields left 0, and
// only MW's build links it: web/build.sh compiles runtime/<game>/*.c for that game alone, never for UG2.
//   node --test web/test/game-table.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const read = f => fs.readFileSync(path.join(REPO, f), 'utf8').replace(/\/\*[\s\S]*?\*\//g, ' ').replace(/\/\/.*$/mg, '');

// the fields UG2 leaves 0, and why
const UG2_UNSET = {
  on_frame: 'UG2 hooks its main loop through on_present and hook',
  frame_site: 'as on_frame: guest_hook hands every hit to hook, as it always has',
  win32_misc: 'win32_misc.c answers Most Wanted\'s imports; UG2\'s stay unimplemented as they always were',
  reg_keys: 'UG2\'s registry is the flat one: every key opens, values by name alone, as always',
  sandbox: 'UG2 maps every path into its game folder, as it always has',
  find_sorted: 'UG2\'s folder scans keep the host\'s order, as they always have',
  fe_match: 'UG2\'s scripts wait on package names only: waitfng and tapuntil match as they always have',
  init_tables: 'no [init] line: UG2\'s boot log stays as it was', ninit_tables: 'as init_tables', init_check: 'as init_tables',
  thread_names: 'the watchdog\'s report and the [thread] lines stay as they were', thread_start: 'as thread_names',
  d3d_modes: 'EnumAdapterModes lists UG2\'s own 6-row mode table (com.c g_modes), with the 4K row\'s stand-in',
  d3d9_strict: 'UG2\'s runtime keeps its own D3D9 answers (descriptors, GetType, multisampling, failed creators, Reset sizes)',
  vclock_sleep_real: 'UG2\'s virtual clock stands at the in-frame cap apart from its creep, as it always has',
  fe_object: 'UG2\'s scripts wait on its screens and memory: $O names no object there',
  on_sound_position: 'Most Wanted\'s sound thread watch; UG2\'s GetCurrentPosition answers as it always did',
  sound_wall_catchup: 'the wall clock\'s old rule: a backlog over 100 ms dropped, the cursors a slice at a time',
  pad_late: 'UG2 finds a late controller by its own 32-frame rescan (pad_rescan)',
  pad_buttons: 'UG2\'s pad keeps the button order its layouts (pad_ug2.c) were made for',
  gpu_stages: 'UG2 keeps its 4 texture stages and no depth textures (0), as it always has',
  d3d_sm3: 'UG2 keeps its 2.0 shader model caps (0), as it always has',
};
// the fields Most Wanted leaves 0 (its table's comment says the same), and why
const MW_UNSET = {
  reg_defaults: 'its registry answers per key (reg_keys, registry_mw.c)',
  lang_suffixes: 'Q-06, the movies\' language suffix',
  on_present: 'the page\'s overlay, M6', status: 'the page\'s overlay, M6',
  sms_request: 'a UG2 feature', photo_input: 'a UG2 feature',
  racer: 'test traces, M4', text_entry: 'M3-11',
  crt_lo: 'the C runtime\'s code range is not measured', crt_hi: 'as crt_lo',
  on_qpc: 'MW\'s frame wait loop, M4', qpc_caller: 'as on_qpc',
  lan_obj: 'MW\'s LAN glue, M8', lan_start: 'M8', lan_stop: 'M8', lan_running: 'M8',
  res_labels: 'M3-03', on_sound_unlock: 'UG2 debugging only',
  online_host_va: 'M9: no MW lobby on the server yet', online_host: 'M9',
  pad_rescan: 'MW enumerates its devices once: a late pad comes through pad_late',
  pad_trigger_axes: 'MW\'s driving layout, M6-37', pad_jolt: 'MW\'s rumble, M6-37',
  pad_hold_screen: 'MW\'s controls screen, M6-37', pad_fe_keys: 'MW\'s menus take the pad\'s buttons themselves',
};
// the page's events (input_web.c) and the feature each reaches
const EVENTS = { IEV_SMS: ['sms_request', 'sms_request'], IEV_PHOTO: ['photo_input', 'photo_input'] };

/** GameOps' field names, in order */
function fields() {
  const m = /typedef struct GameOps \{([\s\S]*?)\} GameOps;/.exec(read('runtime/game.h'));
  assert.ok(m, 'runtime/game.h has typedef struct GameOps');
  const names = [];
  for (const decl of m[1].split(';').map(s => s.trim()).filter(Boolean)) {
    const fp = /\(\s*\*\s*(\w+)\s*\)/.exec(decl);
    if (fp) { names.push(fp[1]); continue; }
    for (const part of decl.split(',')) names.push(/(\w+)\s*$/.exec(part.trim())[1]);
  }
  return names;
}
/** a game file's g_game: field -> its value as written */
function table(file) {
  const m = /const GameOps g_game = \{([\s\S]*?)\n\};/.exec(read(file));
  assert.ok(m, `${file} defines const GameOps g_game`);
  const t = new Map();
  for (const f of m[1].matchAll(/\.(\w+)\s*=\s*([^,]+?)\s*(?:,|$)/g)) t.set(f[1], f[2]);
  return t;
}
const ug2Table = () => table('runtime/game_ug2.c');
/** every GameOps field is set in t or named in unset, and none named in unset is set */
function setOrUnset(t, unset, game) {
  const all = fields();
  for (const f of t.keys()) assert.ok(all.includes(f), `${game}: ${f} is a GameOps field`);
  for (const f of Object.keys(unset)) {
    assert.ok(all.includes(f), `${game}: ${f} (left 0) is still a GameOps field`);
    assert.ok(!t.has(f), `${game} sets ${f}, named here as left 0 (${unset[f]})`);
  }
  const missing = all.filter(f => !t.has(f) && !(f in unset));
  assert.deepEqual(missing, [], `fields ${game}'s table does not set (set them, or name them as left 0 with the reason)`);
}

test('game_ug2.c sets every GameOps field but the ones UG2 leaves 0', () => {
  assert.ok(fields().length >= 30, `GameOps parsed (${fields().length} fields)`);
  setOrUnset(ug2Table(), UG2_UNSET, 'UG2');
});

test('game_mw.c sets every GameOps field but the ones Most Wanted leaves 0, and answers win32_misc.c\'s imports', () => {
  const t = table('runtime/mw/game_mw.c');
  setOrUnset(t, MW_UNSET, 'MW');
  assert.equal(t.get('id'), '"mw"');
  assert.equal(t.get('exe_name'), '"speed.exe"');
  for (const f of ['vroot', 'module_path']) assert.match(t.get(f), /^"C:\\\\NFSMW/, `${f} is under C:\\NFSMW`);
  assert.equal(t.get('entry'), 'GAMEINFO_ENTRY_FN');
  // the imports the shared layers leave: msvcrt.dll's (the D3DX oracle's C runtime, msvcrt_mw.c), then win32_misc.c's
  assert.equal(t.get('win32_misc'), 'mw_imports');
  assert.match(read('runtime/mw/game_mw.c'), /static int mw_imports\(CPU \*c, const char \*dll, const char \*n, uint32_t \*r\)\{\s+if \(!strcasecmp\(dll, "msvcrt\.dll"\)\) return msvcrt_call\(c, n, r\);\s+return win32_misc_call\(c, dll, n, r\);/);
  assert.match(read('runtime/mw/msvcrt_mw.c'), /\nint msvcrt_call\(CPU \*c, const char \*n, uint32_t \*out\)\{/);
  // its registry (registry_mw.c) in its own file, never UG2's; paths that never leave C:\NFSMW (fileio.c)
  assert.equal(t.get('reg_keys'), 'mw_reg_keys');
  assert.match(read('runtime/mw/registry_mw.c'), /\nconst RegKey mw_reg_keys\[\] = \{/);
  assert.equal(t.get('reg_file'), '".nfsmw_registry"');
  assert.equal(t.get('sandbox'), '1');
  assert.equal(t.get('find_sorted'), '1', 'its folder scans in NTFS order (the alias lists)');
  assert.equal(ug2Table().get('reg_file'), '".nfsu2_registry"', 'UG2 keeps ~/.nfsu2_registry');
  assert.match(read('runtime/win32_misc.c'), /\nint win32_misc_call\(CPU \*c, const char \*dll, const char \*n, uint32_t \*out\)\{/,
    'win32_misc.c defines win32_misc_call as game.h\'s win32_misc');
  // the harness (fe_mw.c), the initializer tables the boot counts, the threads the watchdog names (threads_mw.c)
  assert.equal(t.get('flow'), 'mw_flow');
  assert.deepEqual([t.get('flow_fe'), t.get('flow_race')], ['3', '6']);
  assert.equal(t.get('top_screen'), 'mw_top_screen');
  assert.equal(t.get('on_test_frame'), 'mw_fe_frame');
  assert.equal(t.get('fe_object'), 'mw_fe_object');
  assert.equal(t.get('fe_match'), 'mw_fe_match', 'the scripts\' dialog and highlight waits');
  assert.match(read('runtime/mw/fe_mw.c'), /\nint mw_fe_match\(const char \*name\)\{/);
  assert.equal(t.get('init_tables'), 'mw_init_tables');
  assert.equal(t.get('init_check'), '0x009aa9d0u + 0x84cu');
  assert.equal(t.get('thread_names'), 'mw_thread_names');
  assert.equal(t.get('thread_start'), 'mw_thread_start');
  // the per-frame anchor (Q-24): core runs on_frame at frame_site, a hook site of MW's own file the lift collects
  // (tools/genstamp.sh hooks: HOOK_SITE in runtime/mw/*.c); its other hooks are a debugging lift's (debug_mw.c)
  assert.equal(t.get('on_frame'), 'mw_frame');
  assert.equal(t.get('frame_site'), 'MW_FRAME_SITE');
  assert.match(read('runtime/mw/game_mw.c'), /#define MW_FRAME_SITE HOOK_SITE\(0x00663ec6\)/);
  assert.match(read('runtime/mw/career_mw.c'), /\nvoid mw_frame\(CPU \*c\)\{/);
  assert.equal(t.get('hook'), 'mw_debug_hook');
  assert.match(read('runtime/debug.c'), /if \(ip == g_game\.frame_site && ip\)\{\s+g_frame_passes\+\+;\s+if \(g_game\.on_frame\) g_game\.on_frame\(c\);\s+return;\s+\}\s+if \(g_game\.hook\) g_game\.hook\(c, ip\);/,
    'guest_hook runs on_frame at frame_site and hands every other hit to hook; UG2 (frame_site 0) as always');
  assert.match(read('runtime/mw/fe_mw.c'), /\nuint32_t mw_flow\(void\)\{[\s\S]*\nconst char \*mw_top_screen\(void\)\{[\s\S]*\nvoid mw_fe_frame\(void\)\{[\s\S]*\nuint32_t mw_fe_object\(const char \*package, uint32_t hash\)\{/);
  assert.match(read('runtime/mw/threads_mw.c'), /\nconst ThreadName mw_thread_names\[\] = \{[\s\S]*\nuint32_t mw_thread_start\(uint32_t entry, uint32_t arg\)\{/);
  // server.dll always; the D3DX oracle only in a build that linked its lift (web/build.sh: -DNFSU2_D3DX_ORACLE=1)
  const src = read('runtime/mw/game_mw.c');
  assert.match(src, /MODLINK_TABLES\(srv_\), \.slot_lo = MM_SRV_LO, \.slot_hi = MM_SRV_HI/);
  assert.match(src, /#if NFSU2_D3DX_ORACLE\s+\{ MODLINK_TABLES\(d3dx_\), \.slot_lo = MM_D3DX_LO, \.slot_hi = MM_D3DX_HI[^}]*\},\s+#endif/);
  // the oracle is a system DLL (NFSMW_D3DX names it), with msvcrt's variable bound; UG2's server.dll has neither
  assert.match(src, /MODLINK_TABLES\(d3dx_\)[^}]*\.file_env = "NFSMW_D3DX", \.bind = msvcrt_bind \}/);
  assert.doesNotMatch(read('runtime/game_ug2.c'), /file_env|\.bind\b/, 'UG2\'s server.dll entry sets neither');
  // its install: the D3DX math imports answered natively (d3dxmath_mw.c), told whether the oracle is linked; an
  // oracle build also reads NFSMW_FXCENSUS, the effect census (fxcensus_mw.c), which no other build has
  assert.equal(t.get('install'), 'mw_install');
  assert.match(src, /static void mw_install\(void\)\{\s+#if NFSU2_D3DX_ORACLE\s+d3dxmath_install\(1\);\s+fxcensus_init\(\);\s+#else\s+d3dxmath_install\(0\);\s+#endif\s+\}/);
  assert.match(read('runtime/mw/d3dxmath_mw.c'), /\nint d3dxmath_install\(int oracle_linked\)\{/);
  assert.match(read('runtime/mw/fxcensus_mw.c'), /\nvoid fxcensus_init\(void\)\{/);
});

test('each game\'s table is in its own build only', () => {
  const build = read('web/build.sh'), core = read('runtime/core.list');
  const ug2 = /^RT_UG2="([^"]*)"/m.exec(build);
  assert.ok(ug2, 'web/build.sh has RT_UG2');
  assert.ok(ug2[1].split(/\s+/).includes('game_ug2') && !/game_mw/.test(ug2[1]), 'RT_UG2 has game_ug2.c, not game_mw.c');
  // another game's files: runtime/<game>/*.c, for that game alone (UG2's RT_GAME is empty)
  assert.match(build, /if \[ "\$GAME" = ug2 \]; then RT="\$RT_CORE \$RT_UG2"; RT_GAME=""\nelse RT="\$RT_CORE"; RT_GAME="\$\( \(cd "runtime\/\$GAME"/);
  assert.ok(!/game_(ug2|mw)/.test(core), 'core.list names no game\'s table');
  assert.ok(fs.existsSync(path.join(REPO, 'runtime/mw/game_mw.c')));
});

test('the page\'s events reach their features through the game table', () => {
  const input = read('runtime/web/input_web.c'), t = ug2Table();
  for (const [ev, [field, fn]] of Object.entries(EVENTS)) {
    assert.match(input, new RegExp(`case ${ev}:[^;]*g_game\\.${field}\\(`), `input_web.c: ${ev} calls g_game.${field}`);
    assert.equal(t.get(field), fn, `game_ug2.c: .${field} = ${fn}`);
  }
});

test('the game pad is core\'s, and each game\'s own parts are reached through its table', () => {
  assert.match(fs.readFileSync(path.join(REPO, 'runtime/core.list'), 'utf8'), /^dinput_pad\.c\b/m, 'dinput_pad.c is a core file');
  const build = read('web/build.sh'), ug2 = /^RT_UG2="([^"]*)"/m.exec(build)[1].split(/\s+/);
  assert.ok(ug2.includes('pad_ug2') && !ug2.includes('dinput_pad'), 'RT_UG2 has pad_ug2.c; dinput_pad.c comes with the core');
  const u = ug2Table(), m = table('runtime/mw/game_mw.c');
  assert.deepEqual(['pad_rescan', 'pad_trigger_axes', 'pad_jolt', 'pad_hold_screen', 'pad_fe_keys'].map(f => u.get(f)),
    ['ug2_pad_rescan', 'ug2_pad_trigger_axes', 'ug2_pad_jolt', '"UI_Options_PC_Controller.fng"', '1']);
  // UG2's overrides of its profile lookup, its Joypad page and its input controller stay in its own file
  const p = read('runtime/pad_ug2.c');
  for (const f of ['sub_005b81b0', 'sub_005b8460', 'sub_005c1720', 'sub_00407750']) assert.match(p, new RegExp(`\\nvoid ${f}\\(CPU \\*c\\)\\{`));
  // Most Wanted: a late pad through its input manager's callback, then its input set-up; a DualShock's button order
  assert.equal(m.get('pad_late'), '&mw_pad_late');
  assert.match(read('runtime/mw/game_mw.c'),
    /static const PadLate mw_pad_late = \{\s+\.cb = 0x007f1a6bu, \.setup = 0x0063f850u, \.setup_this = 0x00920de8u, \.setup_vtbl = 0x008a7b9cu,\s+\};/);
  assert.equal(m.get('pad_buttons'), '"X A B Y LB RB LT RT BACK START LS RS"');
});
