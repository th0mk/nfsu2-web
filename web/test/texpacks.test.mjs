// The texture packs the page picks and fetches (web/src/app/settings.ts resolveEnv, web/src/host/datacache.ts):
//   node --test web/test/texpacks.test.mjs
// - ui_textures = uhud turns on the maps' pack too (NFSU2_MAP_TEXTURES = detailed), unless NFSU2_MAP_TEXTURES says
//   otherwise; original turns on neither
// - the preload fetches each pack picked and its .tracks, and no other pack
// The modules are bundled from web/src with esbuild, as web/test/songs.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const load = async (file) => {
  const js = (await esbuild.build({ stdin: { contents: `export * from './${file}';`, resolveDir: ROOT, loader: 'ts' }, bundle: true,
    format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
  return import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
};
const S = await load('web/src/app/settings.ts'), D = await load('web/src/host/datacache.ts');

test('Upscaled turns on the HUD\'s and the maps\' packs; Original neither', () => {
  const env = (ini) => S.resolveEnv(ini, {}, {}).env;
  assert.equal(env('ui_textures = uhud\n').NFSU2_UI_TEXTURES, 'uhud');
  assert.equal(env('ui_textures = uhud\n').NFSU2_MAP_TEXTURES, 'detailed');
  assert.equal(env('ui_textures = original\n').NFSU2_MAP_TEXTURES, undefined);
  assert.equal(env('').NFSU2_MAP_TEXTURES, undefined, 'the default: the game\'s own');
  assert.equal(env('NFSU2_MAP_TEXTURES = original\nui_textures = uhud\n').NFSU2_MAP_TEXTURES, 'original', 'the test knob');
  assert.equal(S.resolveEnv('', {}, {}, { ui_textures: 'uhud' }).env.NFSU2_MAP_TEXTURES, 'detailed', 'a device\'s default too');
});

test('the preload fetches the packs picked and their .tracks', () => {
  const files = ['TEXPACKS/uhud.pak', 'TEXPACKS/detailed.pak', 'TEXPACKS/detailed.tracks', 'TEXPACKS/other.pak', 'TRACKS/L4RA.BUN']
    .map(p => ({ path: p, size: 10, mtime: 1 }));
  const ahead = (texpack) => D.aheadFiles(files, 'english', { texpack, songs: '' }).map(x => x.path).sort();
  assert.deepEqual(ahead(''), ['TRACKS/L4RA.BUN']);
  assert.deepEqual(ahead('uhud'), ['TEXPACKS/uhud.pak', 'TRACKS/L4RA.BUN']);
  assert.deepEqual(ahead('uhud,detailed'), ['TEXPACKS/detailed.pak', 'TEXPACKS/detailed.tracks', 'TEXPACKS/uhud.pak', 'TRACKS/L4RA.BUN']);
});
