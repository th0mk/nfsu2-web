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

const tracks = ['TRACKS/L4RA.BUN', 'TRACKS/STREAML4RA.BUN', 'TRACKS/ROUTESL4RA/PathsFreeRoam.bin'];
const pack = tracks.map(p => `MODPACKS/open-tunnel/${p}`);
const files = [...tracks, ...pack, 'MODPACKS/other/file.bin', 'GLOBAL/GlobalB.lzc'].map(path => ({path, size: 10, mtime: 1}));
const ahead = (enabled, entries = files) => D.aheadFiles(entries, 'english', {texpack: '', songs: '', openTunnel: enabled}).map(f => f.path).sort();

test('tunnel is disabled by default and can be enabled and disabled in saved settings', () => {
  const env = ini => S.resolveEnv(ini, {}, {}).env.NFSU2_OPEN_TUNNEL;
  assert.notEqual(env(''), '1');
  assert.equal(env('open_tunnel = on'), '1');
  assert.equal(env('open_tunnel = off'), '0');
});
test('preload uses originals by default and only the complete selected overlay when enabled', () => {
  assert.deepEqual(ahead(false), [...tracks, 'GLOBAL/GlobalB.lzc'].sort());
  assert.deepEqual(ahead(true), [...pack, 'GLOBAL/GlobalB.lzc'].sort());
  assert.deepEqual(ahead(true, files.filter(f => f.path !== pack[2])), ahead(false));
  assert.deepEqual(ahead(true, files.map(f => ({...f, path: f.path.toLowerCase()}))), [...pack, 'GLOBAL/GlobalB.lzc'].map(p => p.toLowerCase()).sort());
});
