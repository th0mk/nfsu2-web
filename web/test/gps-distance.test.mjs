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
const S = await load('web/src/app/settings.ts');

test('GPS distance is opt-in and can be disabled again', () => {
  const env = ini => S.resolveEnv(ini, {}, {}).env.NFSU2_GPS_DISTANCE;
  assert.notEqual(env(''), '1');
  assert.equal(env('gps_distance = on'), '1');
  assert.equal(env('gps_distance = off'), '0');
});
