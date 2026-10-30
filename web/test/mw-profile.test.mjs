// Most Wanted's alias saves on the page (web/src/games/mw/profile.ts, behind '@game/profile' in its build; the
// clean-room spec is docs/mw/save-format.md), and the Saves section's restore, import and rename with them:
//   node --test web/test/mw-profile.test.mjs
// - the container's CRC (src/app/memcard.ts) and MD5 (src/app/md5.ts) against their check values
// - a save made from the spec: its name, bank, checks; an edited one is found and sealed again; a renamed one holds
//   the new name and passes the checks; a name the game's field does not make, a wrong size or Underground 2's
//   profile is refused
// - the Saves section with Most Wanted's module (saves-ui.ts readBackup, addProfiles): a PC folder's aliases found
//   wherever the zip has them, an edited one sealed again with a warning, a backup of the page's restored in place,
//   Underground 2's saves (a backup of its page, its PC folder) refused by any of their marks (src/games/save-marks.ts)
// - real saves, when there are any (the game's own, never in the repository: $NFSMW_SAVES, else out/career/seeds, each
//   folder a save tree with "NFS Most Wanted/<ALIAS>/<ALIAS>"): every one passes the checks under its folder's name,
//   sealing one again gives it byte for byte, a renamed copy passes too, and an edit is found and sealed again
import { test } from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * as P from './web/src/games/mw/profile.ts'; export { md5 } from './web/src/app/md5.ts';
  export { readBackup, addProfiles } from './web/src/app/saves-ui.ts'; export { zip } from './web/src/app/zip.ts';`;
// '@game' is Most Wanted's here, as in its page's build (web/build.mjs --game mw)
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false,
  platform: 'neutral', alias: { '@game': path.join(ROOT, 'web/src/games/mw') }, loader: { '.txt': 'text', '.png': 'dataurl' } })).outputFiles[0].text;
const { P, md5, readBackup, addProfiles, zip } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

const DIR = 'NFS Most Wanted', SIZE = 0xF86C, NAME = 0x5A31, MONEY = 0x4039;
const enc = new TextEncoder();
const at = n => `${DIR}/${n}/${n}`;
const hex = b => Buffer.from(b).toString('hex');

/** sealed as the spec says (section 5), apart from the module: the first MD5, the last, B's CRC, A's, the header's */
function seal(f) {
  const h = (a, b) => crypto.createHash('md5').update(f.subarray(a, b)).digest();
  h(0x44, 0x4034).copy(f, 0x34); h(0x34, 0xF85C).copy(f, 0xF85C);
  f.writeUInt32LE(P.checksum(f.subarray(0x24)), 20); f.writeUInt32LE(P.checksum(f.subarray(0x1C, 0x24)), 16);
  f.writeUInt32LE(P.checksum(f.subarray(0, 24)), 24);
  return new Uint8Array(f.buffer, f.byteOffset, f.length);
}
/** an alias save as far as the spec has it: the container, 'Game', the bank, the name; the rest zero */
function alias(name, { money = 0, version = 0x10D, size = SIZE } = {}) {
  const f = Buffer.alloc(size);
  f.write('20CM', 0, 'latin1');
  f.writeUInt32LE(size, 4); f.writeUInt32LE(8, 8); f.writeUInt32LE(0xF848, 12);
  f.writeUInt32LE(version, 0x1C); f.writeUInt32LE(0xF848, 0x20);
  f.write('Game', 0x44, 'latin1');
  f.writeUInt32LE(money, MONEY);
  f.write(name, NAME, 'latin1');
  return seal(f);
}
const z = files => zip(Object.entries(files).map(([name, data]) => ({ name, data: typeof data === 'string' ? enc.encode(data) : data })));
const sorted = a => [...a].sort();
const paths = r => sorted(r.entries.map(e => e.path));

/** a save folder in memory, as savefs.ts filesOf gives it (RenameFiles) */
function memFs(files) {
  const m = new Map(Object.entries(files));
  return {
    m,
    listFiles: async () => [...m.keys()].map(p => ({ path: p })),
    readBytes: async p => { if (!m.has(p)) throw new Error(`no ${p}`); return m.get(p); },
    readText: async p => m.has(p) ? new TextDecoder().decode(m.get(p)) : null,
    writeFile: async (p, d) => { m.set(p, typeof d === 'string' ? enc.encode(d) : d); },
    remove: async p => { for (const k of [...m.keys()]) if (k === p || k.startsWith(p + '/')) m.delete(k); },
  };
}

test('the container\'s CRC and MD5: their check values', () => {
  assert.equal(P.checksum(enc.encode('123456789')), 0x74527081);
  assert.equal(P.checksum(new Uint8Array([...enc.encode('123456789'), 0, 0, 0, 0])), 0xFC891918, 'CRC-32/BZIP2 with four zeros after');
  assert.equal(P.checksum(enc.encode('abcd')), 0x61626364);
  assert.equal(P.checksum(enc.encode('abc')), 0);
  assert.equal(hex(md5(new Uint8Array(0))), 'd41d8cd98f00b204e9800998ecf8427e');
  assert.equal(hex(md5(enc.encode('abc'))), '900150983cd24fb0d6963f7d28e17f72');
  assert.equal(hex(md5(enc.encode('The quick brown fox jumps over the lazy dog'))), '9e107d9d372bb6826bd81d3542a419d6');
  for (const n of [55, 56, 63, 64, 65, 0x3FF0, 0xF828, 100003]) {
    const d = crypto.randomBytes(n);
    assert.equal(hex(md5(new Uint8Array(d))), crypto.createHash('md5').update(d).digest('hex'), `${n} bytes`);
  }
});

test('profileName, profileMoney: a save made from the spec, and why one is refused', () => {
  assert.equal(P.PROFILES_DIR, DIR);
  assert.deepEqual(P.profileName(alias('ALPHA', { money: 3000 })), { name: 'ALPHA', edited: false });
  assert.equal(P.profileMoney(alias('ALPHA', { money: 3000 })), 3000);
  assert.equal(P.profileMoney(alias('ALPHA').subarray(0, 60000)), null);
  const edited = Buffer.from(alias('ALPHA')); edited.writeUInt32LE(9999999, MONEY);         // a save editor's cash
  assert.deepEqual(P.profileName(edited), { name: 'ALPHA', edited: true });
  const head = Buffer.from(alias('ALPHA')); head[0x100] ^= 1;                                // inside the first MD5's range
  assert.deepEqual(P.profileName(head), { name: 'ALPHA', edited: true });
  assert.match(P.profileName(alias('ALPHA', { version: 0x10C })).bad, /another version/);
  assert.match(P.profileName(alias('TOOLONGX')).bad, /not one the game makes/);
  assert.match(P.profileName(alias('a_b')).bad, /not one the game makes/);
  assert.match(P.profileName(alias('ALPHA').subarray(0, 60000)).bad, /60000 bytes/);
  assert.match(P.profileName(new Uint8Array(54966).fill(0x32, 0, 1)).bad, /no "20CM"/);
  const ug2 = Buffer.alloc(54966); ug2.write('20CM', 0, 'latin1'); ug2.writeUInt32LE(54966, 4);
  assert.match(P.profileName(new Uint8Array(ug2)).bad, /54966 bytes, not 63596/, 'an Underground 2 profile');
  assert.ok(P.isProfileName('aB3xy01') && !P.isProfileName('aB3xy012') && !P.isProfileName('') && !P.isProfileName('a b'));
  assert.equal(P.NAME_MAX, 7);
  assert.equal(P.coverOf('ALPHA DVD 1'), null, 'no covers in this game');
  assert.equal(P.modCars(alias('ALPHA')), 0);
});

test('sealed: an edited save\'s checks made again, its bytes kept; a sealed one comes out as it was', () => {
  const good = alias('ALPHA', { money: 3000 });
  assert.deepEqual(hex(P.sealed(good)), hex(good));
  const e = Buffer.from(good); e.writeUInt32LE(123456, MONEY); e[0x200] = 7; e[20] ^= 1; e[0xF85C] ^= 1;
  const s = P.sealed(new Uint8Array(e));
  assert.deepEqual(P.profileName(s), { name: 'ALPHA', edited: false });
  assert.equal(P.profileMoney(s), 123456, 'the edit stays');
  const changed = [];
  for (let i = 0; i < s.length; i++) if (s[i] !== e[i]) changed.push(i);
  const checks = i => (i >= 16 && i < 28) || (i >= 0x34 && i < 0x44) || i >= 0xF85C;
  assert.ok(changed.every(checks), `only the checks change: ${changed.filter(i => !checks(i))}`);
  assert.equal(e[20], good[20] ^ 1, 'the original is not touched');
});

test('renameProfile: the folder, the file and the name inside, sealed again; refused with nothing written', async () => {
  const f = memFs({ [at('ALPHA')]: alias('ALPHA', { money: 3000 }), [at('BRAVO')]: alias('BRAVO'), 'settings.ini': 'x = 1\n' });
  assert.deepEqual(await P.renameProfile(f, DIR, 'ALPHA', 'Zed42'), { files: 1 });
  assert.deepEqual(sorted(f.m.keys()), sorted([at('Zed42'), at('BRAVO'), 'settings.ini']));
  assert.deepEqual(P.profileName(f.m.get(at('Zed42'))), { name: 'Zed42', edited: false });
  assert.equal(P.profileMoney(f.m.get(at('Zed42'))), 3000);
  assert.equal(f.m.get(at('Zed42'))[NAME + 5], 0, 'NUL-padded');
  assert.deepEqual(await P.renameProfile(f, DIR, 'Zed42', 'ZED42'), { files: 1 }, 'a change of case');
  assert.deepEqual(sorted(f.m.keys()), sorted([at('ZED42'), at('BRAVO'), 'settings.ini']));
  const before = sorted(f.m.keys());
  for (const [from, to, why] of [['ZED42', 'bravo', /already/], ['ZED42', 'TOOLONGX', /1 to 7 letters/], ['ZED42', 'a-b', /1 to 7/],
    ['NOBODY', 'X', /no profile NOBODY/]]) {
    assert.match((await P.renameProfile(f, DIR, from, to)).refused, why, `${from} -> ${to}`);
    assert.deepEqual(sorted(f.m.keys()), before);
  }
  const bad = Buffer.from(alias('CHARLIE')); bad[0x300] ^= 1;
  f.m.set(at('CHARLIE'), new Uint8Array(bad));
  assert.match((await P.renameProfile(f, DIR, 'CHARLIE', 'DELTA')).refused, /cannot be renamed here: its checksums/);
  assert.ok(!f.m.has(at('DELTA')));
});

test('restore: a PC folder\'s aliases wherever the zip has them; an edited one sealed again with a warning', async () => {
  const edited = Buffer.from(alias('BRAVO')); edited.writeUInt32LE(50000, MONEY);
  const r = await readBackup(z({
    [`Documents/${at('ALPHA')}`]: alias('ALPHA'), [`x/BRAVO/BRAVO`]: new Uint8Array(edited), 'x/readme.txt': 'hi',
    'x/TOOLONGX/TOOLONGX': alias('TOOLONGX'), 'x/ug2/ug2': (() => { const u = Buffer.alloc(54966); u.write('20CM', 0, 'latin1'); return new Uint8Array(u); })(),
  }));
  assert.equal(r.kind, 'profiles');
  assert.deepEqual(paths(r), [at('ALPHA'), at('BRAVO')]);
  assert.deepEqual(r.edited, ['BRAVO']);
  assert.deepEqual(P.profileName(r.entries.find(e => e.path === at('BRAVO')).data), { name: 'BRAVO', edited: false });
  assert.deepEqual(sorted(r.refused.map(x => x.name)), ['TOOLONGX', 'ug2']);
  assert.match(r.refused.find(x => x.name === 'ug2').why, /54966 bytes, not 63596/);
  // one alias's own file
  const one = await readBackup(alias('ECHO'), 'ECHO');
  assert.deepEqual([one.kind, paths(one)], ['profiles', [at('ECHO')]]);
  // added beside the ones here: one of the same name in any case goes
  const f = memFs({ [at('alpha')]: alias('alpha'), [at('KEEP')]: alias('KEEP'), 'registry.txt': 'd FirstTime=1\n' });
  await addProfiles(f, r);
  assert.deepEqual(sorted(f.m.keys()), sorted([at('ALPHA'), at('BRAVO'), at('KEEP'), 'registry.txt']));
});

test('restore: a backup of the page\'s comes back in place, without an alias the game would not load', async () => {
  const bad = Buffer.from(alias('BAD')); bad.write('BAX', NAME, 'latin1');
  const r = await readBackup(z({ 'registry.txt': 'd FirstTime=1\n', 'settings.ini': 'fps = 60\n', [at('ALPHA')]: alias('ALPHA'),
    [at('BAD')]: seal(bad), 'nfsmw000.tga': new Uint8Array(4) }));
  assert.equal(r.kind, 'backup');
  assert.deepEqual(r.profiles, ['ALPHA']);
  assert.deepEqual(r.refused.map(x => x.name), ['BAD']);
  assert.deepEqual(paths(r), sorted(['registry.txt', 'settings.ini', at('ALPHA'), 'nfsmw000.tga']));
});

test('restore: Underground 2\'s saves are refused, by any of its marks; Most Wanted\'s own with a stray folder are not', async () => {
  const ug2 = (() => { const u = Buffer.alloc(54966); u.write('20CM', 0, 'latin1'); return new Uint8Array(u); })();
  const REG = 'd NotFirstTime=1\nd VERSION=1\n', INI = '# NFSU2 settings, read at every launch.\nmovies = off\n';
  const U2 = 'NFS Underground 2/ALPHA/ALPHA';
  for (const [what, files, kind] of [
    ['a backup of its page', { 'registry.txt': REG, 'settings.ini': INI, [U2]: ug2 }, 'backup'],
    ['one with no profile', { 'registry.txt': REG, 'settings.ini': INI }, 'backup'],
    ['its settings.ini alone', { 'settings.ini': INI }, 'backup'],
    ['its registry.txt alone (no type letter too)', { 'registry.txt': 'NotFirstTime=1\n' }, 'backup'],
    ['zipped from a folder', { 'saves/registry.txt': REG, 'saves/settings.ini': INI, [`saves/${U2}`]: ug2 }, 'backup'],
    ['its profiles beside a settings.ini with no mark',{ 'settings.ini': 'fps = 60\n', [U2]: ug2 }, 'backup'],
    ['the PC game\'s folder', { [U2]: ug2 }, 'profiles'],
    ['the PC game\'s folder, deeper', { [`Documents/${U2}`]: ug2, 'Documents/x.txt': 'hi' }, 'profiles'],
  ]) {
    const r = await readBackup(z(files));
    assert.deepEqual([r.other, r.kind, paths(r), r.profiles], ['Need for Speed Underground 2', kind, [], []], what);
  }
  // Most Wanted's marks win: a backup of its page with a stray Underground 2 folder or line restores as before
  const own = await readBackup(z({ 'registry.txt': 'd FirstTime=1\nd NotFirstTime=1\n', 'settings.ini': '# NFS Most Wanted settings, read at every launch.\n',
    [at('ALPHA')]: alias('ALPHA'), [U2]: ug2 }));
  assert.deepEqual([own.other, own.kind, own.profiles], [undefined, 'backup', ['ALPHA']]);
  for (const ini of ['# NFS Most Wanted settings, read at every launch.\n', 'fps = 60\n']) {
    const r = await readBackup(z({ 'settings.ini': ini, 'registry.txt': 'd FirstTime=1\n' }));
    assert.deepEqual([r.other, r.kind, paths(r)], [undefined, 'backup', ['registry.txt', 'settings.ini']], ini);
  }
  // no marks at all: a zip of profiles found by what they hold, as before (Underground 2's, refused by its size)
  const bare = await readBackup(z({ 'x/ug2/ug2': ug2 }));
  assert.deepEqual([bare.other, bare.kind, bare.refused.map(x => x.name)], [undefined, 'profiles', ['ug2']]);
});

// ---------------------------------------------------------------- real saves --

const SAVES = process.env.NFSMW_SAVES || path.join(ROOT, 'out/career/seeds');
const real = [];
for (const tree of fs.existsSync(SAVES) ? fs.readdirSync(SAVES).sort() : []) {
  const dir = path.join(SAVES, tree, DIR);
  if (!fs.existsSync(dir)) continue;
  for (const a of fs.readdirSync(dir).sort()) {
    const f = path.join(dir, a, a);
    if (fs.existsSync(f)) real.push({ tree, name: a, data: new Uint8Array(fs.readFileSync(f)) });
  }
}

test(`real saves: the game's own pass every check, and seal again to themselves (${real.length} in ${SAVES})`,
  { skip: !real.length && `no saves in ${SAVES} (set NFSMW_SAVES)` }, async () => {
  for (const s of real) {
    const id = `${s.tree}/${s.name}`;
    assert.equal(s.data.length, SIZE, id);
    assert.deepEqual(P.profileName(s.data), { name: s.name, edited: false }, id);
    assert.ok(P.profileMoney(s.data) >= 0, id);
    assert.equal(hex(P.sealed(s.data)), hex(s.data), `${id}: sealing it again gives the game's own bytes`);
    const e = Buffer.from(s.data); e.writeUInt32LE(P.profileMoney(s.data) + 1000, MONEY);
    assert.deepEqual(P.profileName(e), { name: s.name, edited: true }, `${id}: an edit is seen`);
    assert.deepEqual(P.profileName(P.sealed(new Uint8Array(e))), { name: s.name, edited: false }, `${id}: and sealed again`);
    const f = memFs({ [at(s.name)]: s.data });
    assert.deepEqual(await P.renameProfile(f, DIR, s.name, 'RENAMED'), { files: 1 }, id);
    assert.deepEqual(P.profileName(f.m.get(at('RENAMED'))), { name: 'RENAMED', edited: false }, id);
    assert.deepEqual(await P.renameProfile(f, DIR, 'RENAMED', s.name), { files: 1 }, id);
    assert.equal(hex(f.m.get(at(s.name))), hex(s.data), `${id}: renamed back, it is the game's own file again`);
  }
  const money = Object.fromEntries(real.map(s => [`${s.tree}/${s.name}`, P.profileMoney(s.data)]));
  if ('bin15_start/ALPHA' in money) assert.equal(money['bin15_start/ALPHA'], 3000, 'the first safehouse\'s Cash 3,000');
  if ('new/ALPHA' in money) assert.equal(money['new/ALPHA'], 0, 'a new alias has none');
});
