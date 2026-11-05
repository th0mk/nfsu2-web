// The flags before players' names (web/src/app/flags.ts; settings.ts roam_flag; runtime/flags.dat, made by
// web/tools/mkflags.mjs; runtime/roam_game.c flag_*):
//   node --test web/test/flags.test.mjs
// - the list: in order (the game looks a code up by halves), each code once, as roam_core.c sends it (lower case
//   letters and '-', at most RC_FLAG_MAX); the countries of ISO 3166-1, Kosovo, the United Kingdom's nations and Quebec
// - flags.dat: the same codes in the same order, each flag's runs whole (48x36 pixels), as flag_rgb reads them
// - /flags.png (web/build.mjs): each flag at its place in the sheet, the sheet as big as the flags take
// - the setting: a code in any case is the code; anything else refused (none at Play); its row's names in the page's
//   language, the nations translated
// The modules are bundled from web/src with esbuild, as web/test/phrases.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * from './web/src/app/settings.ts'; export * as F from './web/src/app/flags.ts'; export * as I from './web/src/app/i18n.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const S = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
const { F } = S;
const FLAG_MAX = +fs.readFileSync(path.join(ROOT, 'runtime/roam_core.h'), 'utf8').match(/#define RC_FLAG_MAX\s+(\d+)/)[1];

test('the list: in order, each once, codes the wire takes', () => {
  const c = [...F.FLAG_CODES];
  assert.equal(c.length, 255);
  for (let i = 1; i < c.length; i++) assert.ok(c[i - 1] < c[i], `${c[i - 1]} before ${c[i]}`);
  for (const x of c) assert.match(x, /^[a-z-]+$/), assert.ok(x.length <= FLAG_MAX, x);
  for (const x of ['nl', 'br', 'us', 'tr', 'pl', 'hu', 'ua', 'xk', 'gb', 'gb-eng', 'gb-sct', 'gb-wls', 'gb-nir', 'aq', 'ca-qc']) assert.ok(c.includes(x), x);
  for (const x of ['eu', 'un', 'es-ct', 'es-pv', 'xx', 'arab']) assert.ok(!c.includes(x), `${x} left out`);
});

test('flags.dat: the same codes, each flag whole', () => {
  const b = fs.readFileSync(path.join(ROOT, 'runtime/flags.dat'));
  assert.equal(b.toString('latin1', 0, 4), 'FLG1');
  const n = b.readUInt16LE(4), w = b[6], h = b[7];
  assert.equal(n, F.FLAG_CODES.length); assert.deepEqual([w, h], [48, 36]);
  for (let i = 0; i < n; i++) {
    const e = 8 + 16 * i, code = b.toString('latin1', e, e + 8).replace(/\0+$/, '');
    assert.equal(code, F.FLAG_CODES[i]);
    const off = b.readUInt32LE(e + 8), len = b.readUInt32LE(e + 12);
    let p = off, px = 0;
    while (p < off + len) { const k = b[p++]; if (k < 128) { px += k + 1; p += 3 * (k + 1); } else { px += k - 126; p += 3; } }
    assert.equal(p, off + len, `${code}: runs end with the flag`);
    assert.equal(px, w * h, `${code}: ${px} pixels`);
  }
});

test('/flags.png: Your flag\'s picture at the flag\'s place in the sheet web/build.mjs writes', () => {
  assert.deepEqual([F.FLAG_W, F.FLAG_H], [48, 36]);
  assert.deepEqual(F.flagAt('ad'), [0, 0]);
  const i = F.FLAG_CODES.indexOf('ca-qc'), cols = F.FLAG_SHEET_COLS;
  assert.deepEqual(F.flagAt('ca-qc'), [(i % cols) * 48, Math.floor(i / cols) * 36]);
  assert.equal(F.flagAt(''), null); assert.equal(F.flagAt('xx'), null);
  // the page build's sheet: as wide as FLAG_SHEET_COLS flags, as high as the rows they take
  const png = path.join(ROOT, 'web/dist/flags.png');
  if (fs.existsSync(png)) {
    const b = fs.readFileSync(png);
    assert.deepEqual([b.readUInt32BE(16), b.readUInt32BE(20)], [cols * 48, Math.ceil(F.FLAG_CODES.length / cols) * 36]);
  }
});

test('the setting: a code, any case; anything else refused', () => {
  assert.deepEqual(S.checkSetting('roam_flag', 'NL'), { ok: true, env: 'NFSU2_ROAM_FLAG', value: 'nl' });
  assert.deepEqual(S.checkSetting('roam_flag', ' gb-Eng ; mine'), { ok: true, env: 'NFSU2_ROAM_FLAG', value: 'gb-eng' });
  assert.deepEqual(S.checkSetting('roam_flag', 'CA-QC'), { ok: true, env: 'NFSU2_ROAM_FLAG', value: 'ca-qc' });
  assert.equal(S.checkSetting('roam_flag', ''), null);
  for (const v of ['nld', 'Netherlands', 'eu', 'gb_eng', '12']) assert.equal(S.checkSetting('roam_flag', v)?.ok, false, v);
  assert.match(S.TEMPLATE, /^#roam_flag =$/m);
});

test('the names: the page\'s language, the nations translated', async () => {
  assert.equal(F.flagName('nl'), 'Netherlands');
  assert.equal(F.flagName('gb-sct'), 'Scotland');
  assert.equal(F.flagName('ca-qc'), 'Quebec');
  const all = F.flagsByName();
  assert.equal(all.length, F.FLAG_CODES.length);
  for (let i = 1; i < all.length; i++) assert.ok(all[i - 1].name.localeCompare(all[i].name, 'en') <= 0, `${all[i - 1].name} before ${all[i].name}`);
  const dutch = JSON.parse(fs.readFileSync(path.join(ROOT, 'web/src/app/lang/dutch.json'), 'utf8'));
  globalThis.fetch = async () => new Response(JSON.stringify(dutch));
  assert.equal(await S.I.setLanguage('dutch'), true);
  assert.equal(F.flagName('nl'), 'Nederland');
  assert.equal(F.flagName('gb-eng'), 'Engeland');
  const french = JSON.parse(fs.readFileSync(path.join(ROOT, 'web/src/app/lang/french.json'), 'utf8'));
  globalThis.fetch = async () => new Response(JSON.stringify(french));
  assert.equal(await S.I.setLanguage('french'), true);
  assert.equal(F.flagName('ca-qc'), 'Québec');
});
