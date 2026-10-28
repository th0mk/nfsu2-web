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
  win32_misc: 'win32_misc.c answers Most Wanted\'s imports; UG2\'s stay unimplemented as they always were',
  reg_keys: 'UG2\'s registry is the flat one: every key opens, values by name alone, as always',
  sandbox: 'UG2 maps every path into its game folder, as it always has',
  init_tables: 'no [init] line: UG2\'s boot log stays as it was', ninit_tables: 'as init_tables', init_check: 'as init_tables',
  thread_names: 'the watchdog\'s report and the [thread] lines stay as they were', thread_start: 'as thread_names',
};
// the fields Most Wanted leaves 0 (its table's comment says the same), and why
const MW_UNSET = {
  reg_defaults: 'its registry answers per key (reg_keys, registry_mw.c)',
  lang_suffixes: 'Q-06, the movies\' language suffix',
  install: 'no MW patches yet', hook: 'no MW hook sites (games/mw has no hook list)',
  on_present: 'Q-24, MW\'s per-frame anchor', on_frame: 'Q-24', status: 'the page\'s overlay, M6',
  sms_request: 'a UG2 feature', photo_input: 'a UG2 feature',
  racer: 'test traces, M4', text_entry: 'M3-11',
  crt_lo: 'the C runtime\'s code range is not measured', crt_hi: 'as crt_lo',
  on_qpc: 'MW\'s frame wait loop, M4', qpc_caller: 'as on_qpc',
  lan_obj: 'MW\'s LAN glue, M8', lan_start: 'M8', lan_stop: 'M8', lan_running: 'M8',
  res_labels: 'M3-03', on_sound_unlock: 'UG2 debugging only',
  online_host_va: 'M9: no MW lobby on the server yet', online_host: 'M9',
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
  assert.equal(t.get('win32_misc'), 'win32_misc_call');
  // its registry (registry_mw.c) in its own file, never UG2's; paths that never leave C:\NFSMW (fileio.c)
  assert.equal(t.get('reg_keys'), 'mw_reg_keys');
  assert.match(read('runtime/mw/registry_mw.c'), /\nconst RegKey mw_reg_keys\[\] = \{/);
  assert.equal(t.get('reg_file'), '".nfsmw_registry"');
  assert.equal(t.get('sandbox'), '1');
  assert.equal(ug2Table().get('reg_file'), '".nfsu2_registry"', 'UG2 keeps ~/.nfsu2_registry');
  assert.match(read('runtime/win32_misc.c'), /\nint win32_misc_call\(CPU \*c, const char \*dll, const char \*n, uint32_t \*out\)\{/,
    'win32_misc.c defines win32_misc_call as game.h\'s win32_misc');
  // the harness (fe_mw.c), the initializer tables the boot counts, the threads the watchdog names (threads_mw.c)
  assert.equal(t.get('flow'), 'mw_flow');
  assert.deepEqual([t.get('flow_fe'), t.get('flow_race')], ['3', '6']);
  assert.equal(t.get('top_screen'), 'mw_top_screen');
  assert.equal(t.get('on_test_frame'), 'mw_fe_frame');
  assert.equal(t.get('init_tables'), 'mw_init_tables');
  assert.equal(t.get('init_check'), '0x009aa9d0u + 0x84cu');
  assert.equal(t.get('thread_names'), 'mw_thread_names');
  assert.equal(t.get('thread_start'), 'mw_thread_start');
  assert.match(read('runtime/mw/fe_mw.c'), /\nuint32_t mw_flow\(void\)\{[\s\S]*\nconst char \*mw_top_screen\(void\)\{[\s\S]*\nvoid mw_fe_frame\(void\)\{/);
  assert.match(read('runtime/mw/threads_mw.c'), /\nconst ThreadName mw_thread_names\[\] = \{[\s\S]*\nuint32_t mw_thread_start\(uint32_t entry, uint32_t arg\)\{/);
  // server.dll always; the D3DX oracle only in a build that linked its lift (web/build.sh: -DNFSU2_D3DX_ORACLE=1)
  const src = read('runtime/mw/game_mw.c');
  assert.match(src, /MODLINK_TABLES\(srv_\), \.slot_lo = MM_SRV_LO, \.slot_hi = MM_SRV_HI/);
  assert.match(src, /#if NFSU2_D3DX_ORACLE\s+\{ MODLINK_TABLES\(d3dx_\), \.slot_lo = MM_D3DX_LO, \.slot_hi = MM_D3DX_HI[^}]*\},\s+#endif/);
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
