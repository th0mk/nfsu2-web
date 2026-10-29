// The Blocked players setting (settings.ini roam_block; web/src/app/settings.ts, shell.ts):
//   node --test web/test/roam-block.test.mjs
// - a list of names separated by commas, a "; comment" cut off, at most 32, each with something of a name in it (its
//   Online form: claim.ts), no control characters
// - the names a value lists, as the page's hello sends them: trimmed, the empty ones left out
// - settings.ini's line reaches NFSU2_ROAM_BLOCK; the template has the option's paragraph
// (The server's side: crates/roam tests/roam_block.rs and roam_lobby.rs, crates/lan tests/block.rs and page.rs.)
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * from './web/src/app/settings.ts'; export * as UI from './web/src/app/settings-ui.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const S = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

test('a list of up to 32 names', () => {
  assert.deepEqual(S.checkSetting('roam_block', 'Bob, carl ; the two'), { ok: true, env: 'NFSU2_ROAM_BLOCK', value: 'Bob, carl' });
  assert.equal(S.checkSetting('roam_block', 'Bob the Great, x').ok, true, 'a name as typed: the server compares its Online form');
  const names = n => Array.from({ length: n }, (_, i) => `player${i}`).join(', ');
  assert.equal(S.checkSetting('roam_block', names(32)).ok, true);
  const many = S.checkSetting('roam_block', names(33));
  assert.equal(many.ok, false);
  assert.match(many.reason, /is not a list of up to 32 player names, separated by commas/);
  assert.equal(S.checkSetting('roam_block', 'Bob, ?!').ok, false, 'nothing of a name in one');
  assert.equal(S.checkSetting('roam_block', 'Bob,, carl, ').ok, true, 'empty ones are left out');
  assert.equal(S.checkSetting('roam_block', 'Bob\x01').ok, false, 'no control characters');
  assert.equal(S.BLOCK_MAX, 32);
});

test('the names a value lists', () => {
  assert.deepEqual(S.blockList(''), []);
  assert.deepEqual(S.blockList('  Bob ,, carl ,  '), ['Bob', 'carl']);
});

test('settings.ini to the page', () => {
  assert.equal(S.resolveEnv('roam = on\nroam_block = Bob, carl ; mine\n', {}, {}).env.NFSU2_ROAM_BLOCK, 'Bob, carl');
  assert.match(S.TEMPLATE, /\n#roam_block =\n/);
  assert.ok(S.UI.PANEL_KEYS.includes('roam_block'));
});
