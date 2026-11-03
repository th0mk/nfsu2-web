// The EA Trax songs, settings.ini songs = uncensored | original (tools/musicpack.py --original, web/src/host/datacache.ts,
// web/src/host/io-worker.ts dataFile):
//   node --test web/test/songs.test.mjs
// - which file the page opens for a file of pfdata with the original songs: pfdata/<name> -> pfdata/original/<name>
// - what the preload fetches and counts: pfdata/original only with the original songs, and then in place of pfdata's
//   music files; with the texture pack and a language pack beside it
// The modules are bundled from web/src with esbuild, as web/test/movies-av1.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * from './web/src/host/datacache.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const D = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

test('the original songs\' file for a file of pfdata', () => {
  assert.equal(D.songsPath('pfdata/MusicSFx.mus'), 'pfdata/original/MusicSFx.mus');
  assert.equal(D.songsPath('PFData/MusicSFX.mpf'), 'PFData/original/MusicSFX.mpf');
  for (const p of ['pfdata/original/MusicSFx.mus', 'MusicSFx.mus', 'SOUND/MusicSFx.mus', 'LANGPACKS/french/pfdata/MusicSFx.mus'])
    assert.equal(D.songsPath(p), null, p);
});

test('the files the preload fetches with the uncensored and the original songs', () => {
  const f = (p) => ({ path: p, size: 10, mtime: 1 });
  const files = ['pfdata/MusicSFx.mus', 'pfdata/MusicSFx.mpf', 'pfdata/original/MusicSFx.mus', 'pfdata/original/MusicSFx.mpf',
    'TEXPACKS/uhud.pak', 'LANGPACKS/french/LANGUAGES/French.bin', 'LANGUAGES/French.bin', 'TRACKS/L4RA.BUN', 'MOVIES/PSA.vp6'].map(f);
  const ahead = (language, packs) => D.aheadFiles(files, language, packs).map(x => x.path).sort();
  const common = ['LANGUAGES/French.bin', 'TRACKS/L4RA.BUN'];
  assert.deepEqual(ahead('english'), [...common, 'pfdata/MusicSFx.mpf', 'pfdata/MusicSFx.mus'].sort(), 'no packs: the folder\'s songs');
  assert.deepEqual(ahead('english', { texpack: '', songs: 'uncensored' }), [...common, 'pfdata/MusicSFx.mpf', 'pfdata/MusicSFx.mus'].sort());
  assert.deepEqual(ahead('english', { texpack: '', songs: 'original' }),
    [...common, 'pfdata/original/MusicSFx.mpf', 'pfdata/original/MusicSFx.mus'].sort(), 'original: pfdata/original in place of pfdata\'s');
  assert.deepEqual(ahead('french', { texpack: 'uhud', songs: 'original' }),
    ['LANGPACKS/french/LANGUAGES/French.bin', 'TEXPACKS/uhud.pak', 'TRACKS/L4RA.BUN', 'pfdata/original/MusicSFx.mpf', 'pfdata/original/MusicSFx.mus'].sort(),
    'with a language pack and the texture pack');
});

test('a folder without the original songs: the folder\'s, whatever the setting', () => {
  const files = ['pfdata/MusicSFx.mus', 'pfdata/MusicSFx.mpf', 'TRACKS/L4RA.BUN'].map(p => ({ path: p, size: 10, mtime: 1 }));
  assert.deepEqual(D.aheadFiles(files, 'english', { texpack: '', songs: 'original' }).map(x => x.path).sort(),
    ['TRACKS/L4RA.BUN', 'pfdata/MusicSFx.mpf', 'pfdata/MusicSFx.mus']);
});
