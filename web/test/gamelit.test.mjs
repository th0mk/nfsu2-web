// No game addresses in the runtime's core (tools/gamelit.sh): the files of
// runtime/core.list and runtime/web/*.c hold no hex literal in the games'
// image range 0x401000-0xA79000 outside tools/gamelit.allow, and every
// allowlist line has a reason and still matches.
//   node --test web/test/gamelit.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
const run = (root, args = []) => spawnSync('bash', [path.join(REPO, 'tools/gamelit.sh'), ...args],
  { cwd: REPO, encoding: 'utf8', env: { ...process.env, ...(root ? { GAMELIT_ROOT: root } : {}) } });

const made = [];
test.after(() => { for (const t of made) fs.rmSync(t, { recursive: true, force: true }); });
/** a copy of what the check reads: runtime/core.list and its files, runtime/web/*.c, tools/gamelit.allow */
function copy(name) {
  const t = path.join(OUT, `gamelit-${name}`);
  made.push(t);
  fs.rmSync(t, { recursive: true, force: true });
  fs.mkdirSync(path.join(t, 'runtime/web'), { recursive: true });
  fs.mkdirSync(path.join(t, 'tools'), { recursive: true });
  const list = fs.readFileSync(path.join(REPO, 'runtime/core.list'), 'utf8');
  fs.writeFileSync(path.join(t, 'runtime/core.list'), list);
  for (const f of list.split('\n').map(l => l.replace(/#.*/, '').trim()).filter(Boolean))
    fs.copyFileSync(path.join(REPO, 'runtime', f), path.join(t, 'runtime', f));
  for (const f of fs.readdirSync(path.join(REPO, 'runtime/web')).filter(f => f.endsWith('.c')))
    fs.copyFileSync(path.join(REPO, 'runtime/web', f), path.join(t, 'runtime/web', f));
  fs.copyFileSync(path.join(REPO, 'tools/gamelit.allow'), path.join(t, 'tools/gamelit.allow'));
  return t;
}
const append = (t, f, s) => fs.appendFileSync(path.join(t, f), s);

test('gamelit: this checkout passes', () => {
  const r = run();
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.equal(r.stdout, '');
});

test('gamelit: a game address planted in a core file fails, and names the file and line', () => {
  const t = copy('planted');
  const n = fs.readFileSync(path.join(t, 'runtime/mem.c'), 'utf8').split('\n').length;
  append(t, 'runtime/mem.c', 'static const unsigned planted = 0x5C7FEE;\n');
  const r = run(t);
  assert.equal(r.status, 1, r.stdout);
  assert.match(r.stdout, new RegExp(`^runtime/mem\\.c:${n}: 0x5C7FEE: a game address in a core file`, 'm'));
  // the same in a runtime/web file, spelled with leading zeros and a suffix
  const w = copy('planted-web');
  append(w, 'runtime/web/tz.c', 'static const unsigned planted = 0x005c7feeu;\n');
  assert.match(run(w).stdout, /^runtime\/web\/tz\.c:\d+: 0x005c7fee:/m);
});

test('gamelit: comments are skipped, strings are not, and the range ends are exact', () => {
  const c = copy('comment');
  append(c, 'runtime/mem.c', '/* UG2 calls 0x5C7FEE here */\n// and 0x5C7FEF\n');
  assert.equal(run(c).status, 0, run(c).stdout);
  const s = copy('string');
  append(s, 'runtime/mem.c', 'static const char planted[] = "at 0x5C7FEE";\n');
  assert.match(run(s).stdout, /^runtime\/mem\.c:\d+: 0x5C7FEE:/m);
  const e = copy('ends');
  append(e, 'runtime/mem.c', 'static const unsigned a = 0x401000, b = 0xA78FFF, c = 0x400FFF, d = 0xA79000, f = 0x1000005C7FEEull;\n');
  const r = run(e);
  assert.equal(r.status, 1);
  assert.deepEqual(r.stdout.trim().split('\n').map(l => l.split(': ')[1]), ['0x401000', '0xA78FFF']);
});

test('gamelit: an allowlist line needs a reason, may not be listed twice, and must still match', () => {
  const allow = t => path.join(t, 'tools/gamelit.allow');
  const why = copy('reason');
  fs.appendFileSync(allow(why), 'mem.c 0x5C7FEE\n');
  append(why, 'runtime/mem.c', 'static const unsigned planted = 0x5C7FEE;\n');
  assert.match(run(why).stdout, /gamelit\.allow:\d+: mem\.c 0x5C7FEE has no reason/);
  const ok = copy('allowed');
  fs.appendFileSync(allow(ok), 'mem.c 0x005C7FEEu a reviewed test address\n');
  append(ok, 'runtime/mem.c', 'static const unsigned planted = 0x5c7fee;\n');
  const r = run(ok, ['--all']);
  assert.equal(r.status, 0, r.stdout);
  assert.match(r.stdout, /^runtime\/mem\.c:\d+: 0x5c7fee \(allowed: a reviewed test address\)$/m);
  const twice = copy('twice');
  fs.appendFileSync(allow(twice), 'mem.c 0x5C7FEE one\nmem.c 0x5c7fee two\n');
  append(twice, 'runtime/mem.c', 'static const unsigned planted = 0x5C7FEE;\n');
  assert.match(run(twice).stdout, /mem\.c 0x5c7fee is listed twice/);
  const stale = copy('stale');
  fs.appendFileSync(allow(stale), 'mem.c 0x5C7FEE gone since\n');
  const s = run(stale);
  assert.equal(s.status, 1);
  assert.match(s.stdout, /gamelit\.allow:\d+: mem\.c 0x5c7fee matches nothing \(remove the line\)/);
});

test('gamelit: a core.list file that is missing fails', () => {
  const t = copy('missing');
  append(t, 'runtime/core.list', 'nosuch.c\n');
  const r = run(t);
  assert.equal(r.status, 1);
  assert.match(r.stdout, /^runtime\/core\.list:\d+: nosuch\.c: no such file in runtime\//m);
});
