// License plates on the page (web/src/app/plates.ts, settings.ts license_plate):
//   node --test web/test/plates.test.mjs
// the setting takes capitals and digits (small letters as capitals), refuses the rest and anything that reads as a
// slur in a plain line, and keeps its line in the template and through "Reset all settings" (the game takes it at
// the next Play, NFSU2_LICENSE_PLATE).
import test from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('../..', import.meta.url));
const load = async contents => {
  const { outputFiles } = await build({ stdin: { contents, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false });
  return import('data:text/javascript;base64,' + Buffer.from(outputFiles[0].text).toString('base64'));
};
const S = await load("export * from './web/src/app/settings.ts'; export * from './web/src/app/plates.ts';");

test('a plate: capitals and digits, small letters as capitals, at most 8', () => {
  assert.equal(S.checkSetting('license_plate', 'ab123').value, 'AB123');
  assert.equal(S.checkSetting('license_plate', 'ZZ99ZZ99').value, 'ZZ99ZZ99');
  assert.equal(S.checkSetting('license_plate', 'abc ; a comment').value, 'ABC');
  assert.equal(S.checkSetting('license_plate', ''), null);              // the game's own plate
  for (const v of ['a b', 'ABCDEFGHI', 'é', 'АBC', 'A-B', 'A_B'])
    assert.match(S.checkSetting('license_plate', v).reason, /is not a license plate \(letters A to Z and digits, at most 8\)/, v);
  // as a name, and as only a plate does (names.mjs PLATE: no small letters to split it by)
  for (const v of ['n1gger', 'N1GGER', '1488', 'bigfag', 'FAGBOY', 'HH88', 'KKK'])
    assert.match(S.checkSetting('license_plate', v).reason, /is not a license plate \(it reads as a slur\)/, v);
  for (const v of ['RACCOON', 'MUSTAFA6', 'SPICY', 'KONIG', 'KKKK', 'BOSS88'])
    assert.equal(S.checkSetting('license_plate', v).value, v);
  for (const r of [S.checkSetting('license_plate', 'a b'), S.checkSetting('license_plate', '1488')]) assert.doesNotMatch(r.reason, /ASCII/);
  assert.equal(S.plateUpper(' ab1 '), 'AB1');
  assert.ok(S.plateValid('') && S.plateValid('AB12') && !S.plateValid('ab') && !S.plateValid('123456789'));
});

test('settings.ini: its line in the template, the game\'s variable, kept by Reset all settings, cleared', () => {
  assert.match(S.TEMPLATE, /# \[NFSU2_LICENSE_PLATE\]\n#license_plate =\n/);
  const ini = S.settingsPut(S.TEMPLATE, 'license_plate', 'abc1');
  assert.equal(S.resolveEnv(ini, {}, {}).env.NFSU2_LICENSE_PLATE, 'ABC1');
  assert.equal(S.resolveEnv(S.resetAll(ini), {}, {}).env.NFSU2_LICENSE_PLATE, 'ABC1');
  assert.equal(S.resolveEnv(S.settingsPut(ini, 'license_plate', ''), {}, {}).env.NFSU2_LICENSE_PLATE ?? '', '');
  // a slur in the file: refused, the game's own plate
  assert.equal(S.resolveEnv(S.settingsPut(S.TEMPLATE, 'license_plate', 'N1GGER'), {}, {}).env.NFSU2_LICENSE_PLATE ?? '', '');
});
