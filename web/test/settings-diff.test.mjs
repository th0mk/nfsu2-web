// settings.ini in the browser (web/src/app/settings.ts) against the Mac app's
// own C (runtime/bundle.m's table, validation and reader, compiled for this
// Mac): web/tools/settings-diff.mjs, which a merge of the Mac's runtime can
// break without anything else noticing (the macsync review).
//   node --test web/test/settings-diff.test.mjs        (needs clang: Xcode's command line tools)
import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const haveClang = spawnSync('clang', ['--version'], { encoding: 'utf8' }).status === 0;

// each game's template too (--game): its defaults bundle.m's, every key it lists read by that game's build
for (const game of ['ug2', 'mw']) test(`settings.ts agrees with bundle.m (${game})`, { skip: !haveClang && 'no clang' }, () => {
  const r = spawnSync(process.execPath, [path.join(REPO, 'web/tools/settings-diff.mjs'), '--game', game], { cwd: REPO, encoding: 'utf8' });
  const out = (r.stdout ?? '') + (r.stderr ?? '');
  console.log(out.trim().split('\n').filter(l => /^(DIFF|FILE DIFF|FAIL|NO READER|single settings)|problem|agrees/.test(l)).slice(0, 40).join('\n'));
  assert.equal(r.status, 0, 'settings-diff exits 0');
  assert.match(out, /settings\.ts agrees with bundle\.m/);
  assert.match(out, new RegExp(`ok   every key of the ${game} template has a reader`));
});
