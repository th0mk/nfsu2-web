// datacache.ts voices setting ('dubbed' vs 'original') for language packs:
// with 'dubbed' (the default), language pack speech banks and movies replace the base game's;
// with 'original', speech banks and movies from the language pack are skipped, keeping the
// base game's English audio while preserving the language pack's localized text.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export { gameFiles, aheadFiles, languageMovies } from './web/src/host/datacache.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const { gameFiles, aheadFiles, languageMovies } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

const FILES = [
  { path: 'GLOBAL/GLOBALB.LZC', size: 1000, mtime: 1 },
  { path: 'speech/UG2_Speech.big', size: 50000, mtime: 1 },
  { path: 'speech/UG2_Speech.idx', size: 200, mtime: 1 },
  { path: 'MOVIES/Scene01.vp6', size: 30000, mtime: 1 },
  { path: 'MOVIES/Scene02.vp6', size: 30000, mtime: 1 },
  { path: 'LANGUAGES/English.bin', size: 5000, mtime: 1 },
  { path: 'LANGPACKS/portuguese/LANGUAGES/English.bin', size: 5200, mtime: 2 },
  { path: 'LANGPACKS/portuguese/CREDITS/NA_ENGLISH.TXT', size: 1200, mtime: 2 },
  { path: 'LANGPACKS/portuguese/speech/UG2_Speech.big', size: 48000, mtime: 2 },
  { path: 'LANGPACKS/portuguese/speech/UG2_Speech.idx', size: 200, mtime: 2 },
  { path: 'LANGPACKS/portuguese/MOVIES/Scene01.vp6', size: 28000, mtime: 2 },
  { path: 'LANGPACKS/portuguese/MOVIES/Scene02.vp6', size: 28000, mtime: 2 },
  { path: 'LANGPACKS/french/speech/UG2_Speech.big', size: 49000, mtime: 2 },
];

test('datacache: dubbed voices include language pack speech and movies', () => {
  const gf = gameFiles(FILES, 'portuguese', '', 'dubbed');
  const paths = new Set(gf.map(f => f.path));

  // localized text and credits from pack
  assert.ok(paths.has('LANGPACKS/portuguese/LANGUAGES/English.bin'));
  assert.ok(paths.has('LANGPACKS/portuguese/CREDITS/NA_ENGLISH.TXT'));
  assert.ok(!paths.has('LANGUAGES/English.bin'));

  // dubbed speech and movies from pack
  assert.ok(paths.has('LANGPACKS/portuguese/speech/UG2_Speech.big'));
  assert.ok(paths.has('LANGPACKS/portuguese/speech/UG2_Speech.idx'));
  assert.ok(paths.has('LANGPACKS/portuguese/MOVIES/Scene01.vp6'));
  assert.ok(paths.has('LANGPACKS/portuguese/MOVIES/Scene02.vp6'));

  // original speech and movies masked
  assert.ok(!paths.has('speech/UG2_Speech.big'));
  assert.ok(!paths.has('speech/UG2_Speech.idx'));
  assert.ok(!paths.has('MOVIES/Scene01.vp6'));
  assert.ok(!paths.has('MOVIES/Scene02.vp6'));

  // other language pack excluded
  assert.ok(!paths.has('LANGPACKS/french/speech/UG2_Speech.big'));
});

test('datacache: original voices skip language pack speech and movies', () => {
  const gf = gameFiles(FILES, 'portuguese', '', 'original');
  const paths = new Set(gf.map(f => f.path));

  // localized text and credits still come from pack
  assert.ok(paths.has('LANGPACKS/portuguese/LANGUAGES/English.bin'));
  assert.ok(paths.has('LANGPACKS/portuguese/CREDITS/NA_ENGLISH.TXT'));
  assert.ok(!paths.has('LANGUAGES/English.bin'));

  // pack speech and movies skipped
  assert.ok(!paths.has('LANGPACKS/portuguese/speech/UG2_Speech.big'));
  assert.ok(!paths.has('LANGPACKS/portuguese/speech/UG2_Speech.idx'));
  assert.ok(!paths.has('LANGPACKS/portuguese/MOVIES/Scene01.vp6'));
  assert.ok(!paths.has('LANGPACKS/portuguese/MOVIES/Scene02.vp6'));

  // original speech and movies kept
  assert.ok(paths.has('speech/UG2_Speech.big'));
  assert.ok(paths.has('speech/UG2_Speech.idx'));
  assert.ok(paths.has('MOVIES/Scene01.vp6'));
  assert.ok(paths.has('MOVIES/Scene02.vp6'));
});

test('datacache: aheadFiles filters out movies but respects voices setting for speech', () => {
  const dubbed = aheadFiles(FILES, 'portuguese', '', 'dubbed');
  const dubbedPaths = new Set(dubbed.map(f => f.path));
  assert.ok(dubbedPaths.has('LANGPACKS/portuguese/speech/UG2_Speech.big'));
  assert.ok(!dubbedPaths.has('LANGPACKS/portuguese/MOVIES/Scene01.vp6'));
  assert.ok(!dubbedPaths.has('MOVIES/Scene01.vp6'));

  const orig = aheadFiles(FILES, 'portuguese', '', 'original');
  const origPaths = new Set(orig.map(f => f.path));
  assert.ok(!origPaths.has('LANGPACKS/portuguese/speech/UG2_Speech.big'));
  assert.ok(origPaths.has('speech/UG2_Speech.big'));
  assert.ok(!origPaths.has('LANGPACKS/portuguese/MOVIES/Scene01.vp6'));
  assert.ok(!origPaths.has('MOVIES/Scene01.vp6'));
});

test('datacache: languageMovies returns dubbed movies when dubbed, original movies when original', () => {
  const dubbedMovies = languageMovies(FILES, 'portuguese', false, 'dubbed');
  assert.deepEqual(dubbedMovies.map(f => f.path), [
    'LANGPACKS/portuguese/MOVIES/Scene01.vp6',
    'LANGPACKS/portuguese/MOVIES/Scene02.vp6',
  ]);

  const origMovies = languageMovies(FILES, 'portuguese', false, 'original');
  assert.deepEqual(origMovies.map(f => f.path), [
    'MOVIES/Scene01.vp6',
    'MOVIES/Scene02.vp6',
  ]);
});
