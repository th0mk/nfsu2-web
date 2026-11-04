// The Settings panel's resets (web/src/app/settings-ui.ts):
//   node --test web/test/settings-reset.test.mjs
// - Reset all settings: the template, but the name and language as the file has them (also set by a variable's
//   own name), and nothing to reset in a file that is the template already
// - Reset on a line the panel has no row for: the template's commented-out line in its place, or the line gone
//   (a variable by its own name, an option the template does not list), found by its line in the file
// The modules are bundled from web/src with esbuild, as web/test/page-audit.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * from './web/src/app/settings.ts'; export * as UI from './web/src/app/settings-ui.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const S = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

// the player's file from the report (2026-10-24): the panel's old Car memory pool row left roam_carpool_kb = 11000
const player = () => {
  let ini = S.TEMPLATE;
  for (const [k, v] of [['movies', 'off'], ['background', 'run'], ['resolution', '1920x1080'], ['render', 'scale:2'], ['max_pixels', '200'],
    ['roam_name', 'Lessis'], ['online', 'on'], ['roam', 'on'], ['roam_delay', '1000'], ['roam_extrap', '1000'], ['roam_carpool_kb', '11000'],
    ['language', 'russian'], ['ui_textures', 'uhud'], ['horn', 'clown']]) ini = S.settingsPut(ini, k, v);
  return ini + 'NFSU2_STATUS_EVERY = 600\n';
};

test('Reset all settings: the template, the name and language kept', () => {
  const out = S.UI.resetAll(player());
  const r = S.resolveEnv(out, {}, {});
  const set = Object.keys(r.byEnv).sort();
  assert.deepEqual(set, ['NFSU2_LANGUAGE', 'NFSU2_ROAM_NAME'], 'only the name and language are set');
  assert.equal(r.env.NFSU2_ROAM_NAME, 'Lessis');
  assert.equal(r.env.NFSU2_LANGUAGE, 'russian');
  assert.equal(r.env.NFSU2_ROAM_CARPOOL_KB, undefined, 'the car pool is the runtime default again');
  assert.equal(r.source.NFSU2_STATUS_EVERY, 'default', 'a variable set by its own name goes');
  assert.equal(out, S.settingsPut(S.settingsPut(S.TEMPLATE, 'roam_name', 'Lessis'), 'language', 'russian'), 'in the template\'s own places');
  assert.equal(S.UI.resetAll(out), out, 'nothing more to reset');
});

test('Reset all settings: kept settings by a variable\'s own name, refused ones, none', () => {
  // the name by its variable: kept as roam_name; one that roam_name would read otherwise (" ; " starts a comment
  // there, not in a variable's line) is not
  const out = S.UI.resetAll('NFSU2_ROAM_NAME = Bob\nlanguage = german\nfps = 60\n');
  assert.match(out, /^roam_name = Bob$/m);
  assert.match(out, /^language = german$/m);
  assert.doesNotMatch(out, /^NFSU2_ROAM_NAME =|^fps =/m);
  assert.equal(S.UI.resetAll('NFSU2_ROAM_NAME = Bob ; me\n'), S.TEMPLATE);
  assert.equal(S.UI.resetAll('language = klingon\nroam_name = Al\n'), S.settingsPut(S.TEMPLATE, 'roam_name', 'Al'), 'a refused language is the default');
  assert.equal(S.UI.resetAll(null), S.TEMPLATE, 'no file yet');
  assert.equal(S.UI.resetAll(S.TEMPLATE), S.TEMPLATE);
});

test('Reset on a line without a row: the template line, or the line gone, by its line in the file', () => {
  const ini = player();
  const lines = ini.split('\n');
  const at = k => lines.findIndex(l => l.startsWith(k + ' =')) + 1;
  // an option of the template: its own commented line in its place, the rest as it was
  const ex = S.UI.resetLine(ini, at('roam_extrap'));
  assert.equal(ex.split('\n').length, lines.length);
  assert.equal(ex.split('\n')[at('roam_extrap') - 1], '#roam_extrap = 250');
  assert.equal(S.resolveEnv(ex, {}, {}).env.NFSU2_ROAM_EXTRAP, undefined);
  assert.equal(S.resolveEnv(ex, {}, {}).env.NFSU2_ROAM_DELAY, '1000', 'other lines stay');
  // the car pool (only the Mac app reads it since issue #68): never passed on, and its Reset removes the line
  const r0 = S.resolveEnv(ini, {}, {});
  assert.equal(r0.env.NFSU2_ROAM_CARPOOL_KB, undefined);
  assert.ok(r0.notes.some(n => /roam_carpool_kb is only used by the Mac app, ignored/.test(n)));
  const pool = S.UI.resetLine(ini, at('roam_carpool_kb'));
  assert.equal(pool.split('\n').length, lines.length - 1);
  assert.ok(!pool.split('\n').some(l => l.startsWith('roam_carpool_kb')));
  // a variable by its own name: removed
  const v = S.UI.resetLine(ini, at('NFSU2_STATUS_EVERY'));
  assert.equal(v.split('\n').length, lines.length - 1);
  assert.ok(!v.split('\n').includes('NFSU2_STATUS_EVERY = 600'));
  // the Mac app's option (no template line): removed; a comment or a line past the end: nothing changes
  assert.equal(S.UI.resetLine('movies = off\ngraphics = enhanced\n', 2), 'movies = off\n');
  assert.equal(S.UI.resetLine(ini, 1), ini);
  assert.equal(S.UI.resetLine(ini, lines.length + 5), ini);
  // found by its line in the file, not by the 1199-byte pieces a long line before it is read in
  const long = '# ' + 'x'.repeat(3000) + '\nroam_rate = 30\n';
  const r = S.resolveEnv(long, {}, {});
  assert.equal(r.file.roam_rate.phys, 2);
  assert.equal(S.UI.resetLine(long, r.file.roam_rate.phys), '# ' + 'x'.repeat(3000) + '\n#roam_rate = 20\n');
});
