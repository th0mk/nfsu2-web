import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { fileURLToPath } from 'node:url';
const ROOT = fileURLToPath(new URL('../..', import.meta.url));
const { outputFiles } = await build({ absWorkingDir: ROOT, entryPoints: ['web/src/app/plates.ts'], bundle: true, format: 'esm', write: false });
const { plateText, plateValid } = await import('data:text/javascript;base64,' + Buffer.from(outputFiles[0].text).toString('base64'));
test('plates accept only eight ASCII letters and digits, including pasted text', () => {
  assert.equal(plateText('ab-12 /čＤ🙂Z'), 'AB12Z');
  assert.equal(plateText('123456789'), '12345678');
  for (const s of ['', 'AB12', 'ABCDEFGH']) assert(plateValid(s));
  for (const s of ['lower', 'A B', 'АBC', '123456789', '\0']) assert(!plateValid(s));
});

const settingsJS = (await build({ stdin: { contents: "export * from './web/src/app/settings.ts'; export { resetAll } from './web/src/app/settings-ui.ts';", resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false })).outputFiles[0].text;
const S = await import('data:text/javascript;base64,' + Buffer.from(settingsJS).toString('base64'));
test('plate settings normalize, reject invalid files, survive reset, and clear', () => {
  assert.equal(S.checkSetting('license_plate', 'ab123').value, 'AB123');
  for (const text of ['a b', 'ABCDEFGHI', 'é', 'АBC']) assert.equal(S.checkSetting('license_plate', text).ok, false);
  const ini = S.settingsPut(S.TEMPLATE, 'license_plate', 'ABC123');
  assert.equal(S.resolveEnv(ini, {}, {}).env.NFSU2_LICENSE_PLATE, 'ABC123');
  assert.equal(S.resolveEnv(S.resetAll(ini), {}, {}).env.NFSU2_LICENSE_PLATE, 'ABC123');
  assert.equal(S.resolveEnv(S.settingsPut(ini, 'license_plate', ''), {}, {}).env.NFSU2_LICENSE_PLATE ?? '', '');
});

// Compile the renderer's actual cache with fault-injected guest texture creation.
test('plate cache recovers from failed GPU creation without starving other plates', async () => {
  const { mkdtempSync, rmSync } = await import('node:fs');
  const { tmpdir } = await import('node:os');
  const { join } = await import('node:path');
  const { spawnSync } = await import('node:child_process');
  const dir = mkdtempSync(join(tmpdir(), 'plates-cache-'));
  try {
    const exe = join(dir, 'cache-test');
    const build = spawnSync(process.env.CC || 'cc', ['-O1', '-Iruntime', 'tests/plates_cache_unit.c', '-o', exe], { cwd: ROOT, encoding: 'utf8' });
    assert.equal(build.status, 0, build.stderr || String(build.error));
    const run = spawnSync(exe, [], { encoding: 'utf8' });
    assert.equal(run.status, 0, run.stderr || String(run.error));
  } finally { rmSync(dir, { recursive: true, force: true }); }
});
