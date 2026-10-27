// UG2's game table (runtime/game_ug2.c's g_game) sets every field of GameOps (runtime/game.h) but the ones it leaves 0
// on purpose, and the page's events reach their features through it.  Core calls a field only when it is set, so a
// field dropped from the table fails nowhere: the page's photo mode (IEV_PHOTO, input_web.c) or text messages
// (IEV_SMS) would just do nothing, and the test scripts' "photo ..." calls photo.c directly, so run-suite photo
// would still pass.  A new field must be named here as set or as left 0 for UG2.
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
/** game_ug2.c's g_game: field -> its value as written */
function ug2Table() {
  const m = /const GameOps g_game = \{([\s\S]*?)\n\};/.exec(read('runtime/game_ug2.c'));
  assert.ok(m, 'runtime/game_ug2.c defines const GameOps g_game');
  const t = new Map();
  for (const f of m[1].matchAll(/\.(\w+)\s*=\s*([^,]+?)\s*(?:,|$)/g)) t.set(f[1], f[2]);
  return t;
}

test('game_ug2.c sets every GameOps field but the ones UG2 leaves 0', () => {
  const all = fields(), t = ug2Table();
  assert.ok(all.length >= 30, `GameOps parsed (${all.length} fields)`);
  for (const f of t.keys()) assert.ok(all.includes(f), `${f} is a GameOps field`);
  for (const f of Object.keys(UG2_UNSET)) {
    assert.ok(all.includes(f), `${f} (left 0 for UG2) is still a GameOps field`);
    assert.ok(!t.has(f), `UG2 sets ${f}, named here as left 0 (${UG2_UNSET[f]})`);
  }
  const missing = all.filter(f => !t.has(f) && !(f in UG2_UNSET));
  assert.deepEqual(missing, [], 'fields UG2\'s table does not set (set them, or name them in UG2_UNSET with the reason)');
});

test('the page\'s events reach their features through the game table', () => {
  const input = read('runtime/web/input_web.c'), t = ug2Table();
  for (const [ev, [field, fn]] of Object.entries(EVENTS)) {
    assert.match(input, new RegExp(`case ${ev}:[^;]*g_game\\.${field}\\(`), `input_web.c: ${ev} calls g_game.${field}`);
    assert.equal(t.get(field), fn, `game_ug2.c: .${field} = ${fn}`);
  }
});
