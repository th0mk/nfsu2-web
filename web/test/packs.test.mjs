// The packs' check (tools/packcheck.py, which every pack tool runs on what it made before it writes it):
//   node --test web/test/packs.test.mjs
// - tools/test_packcheck.py: small packs made as the tools make them, each then broken one way (cut short, a block
//   past the file's end, two textures in one place, one among the game's own, JDLZ the native unpacker refuses, a
//   memory file that still lists a spliced file ...): the check names each one and passes the good ones
// - the real packs, where this machine has them (the game folder: NFSU2_GAME, or ~/projects/nsfu2/work/gamedata-ro;
//   the packs: out/web/{vinylpacks,modpacks,langpacks,texpacks} of this clone, else of the main one): the check passes
//   them, but for vinyl packs made before 8473add3, whose textures runtime/vinylpacks.c moves (a note: rebuild them)
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const python = process.platform === 'win32' ? 'python' : 'python3';
const havePython = spawnSync(python, ['--version']).status === 0;

test('tools/packcheck.py: broken packs refused, good ones passed', { skip: !havePython && 'no Python' }, () => {
  const r = spawnSync(python, ['-I', path.join(ROOT, 'tools/test_packcheck.py')], { encoding: 'utf8' });
  assert.equal(r.status, 0, r.stdout + r.stderr);
});

/** the folders with the real packs: this clone's out/web, else the main clone's (a worktree has none) */
function packFolders() {
  const common = spawnSync('git', ['rev-parse', '--path-format=absolute', '--git-common-dir'], { cwd: ROOT, encoding: 'utf8' });
  const roots = [ROOT, ...(common.status === 0 ? [path.dirname(common.stdout.trim())] : [])];
  const out = [];
  for (const kind of ['vinylpacks', 'modpacks', 'langpacks', 'texpacks']) {
    const hit = roots.map(r => path.join(r, 'out', 'web', kind)).find(d => fs.existsSync(d));
    if (hit) out.push(hit);
  }
  return out;
}
const GAME = process.env.NFSU2_GAME || path.join(os.homedir(), 'projects/nsfu2/work/gamedata-ro');
const folders = fs.existsSync(path.join(GAME, 'SPEED2.EXE')) ? packFolders() : [];

test('the real packs, as the game reads them', {
  skip: !havePython ? 'no Python' : !folders.length && `no game folder (${GAME}) or no packs in out/web: nothing to check`,
}, (t) => {
  const r = spawnSync(python, ['-I', path.join(ROOT, 'tools/packcheck.py'), GAME, ...folders], { encoding: 'utf8', maxBuffer: 64 << 20 });
  const lines = r.stdout.split('\n').filter(Boolean);
  const summary = lines.pop() ?? '';
  assert.match(summary, /^packs: \d+ files/, r.stdout + r.stderr);
  // vinyl packs made before 8473add3: the runtime moves their textures (runtime/vinylpacks.c tex_in)
  const moved = lines.filter(l => /^VINYLPACKS\/.*at the first vinyl packs' places/.test(l));
  if (moved.length) t.diagnostic(`${moved.length} vinyl packs made before 8473add3 (the runtime moves their textures): rebuild them with tools/vinylpacks.py`);
  const rest = lines.filter(l => !moved.includes(l));
  assert.deepEqual(rest, [], `tools/packcheck.py ${GAME} ${folders.join(' ')}`);
  t.diagnostic(`${summary} (${folders.map(f => path.relative(ROOT, f) || f).join(', ')})`);
});
