// Profiles from the PC game through Restore a backup (web/src/app/saves-ui.ts
// readBackup, addProfiles; profile.ts profileName, modCars, sealed):
//   node --test web/test/pcsave.test.mjs
// - a zip of the PC's NFS Underground 2 folder, in any shape (the folder, its
//   profile folders, deeper), or one profile's own file: each profile goes to
//   "NFS Underground 2/<the name inside>/", its cover shoots beside it, other files are left out
// - a zip with registry.txt or settings.ini at its top is a backup of the page's and
//   replaces everything, as before; a cover shoot is no profile in its list
// - a profile the game would not load (version, a name its field does not make) is
//   left out with why, its cover shoots too; one a save editor changed (checksums
//   that no longer hold) comes in sealed again, as the game would have saved it, and
//   one with cars a mod added (a car type the game's 31 player cars do not have),
//   each with a warning
// - adding: a profile of the same name here (in any case) goes with its cover shoots,
//   the other profiles and files stay, and its autosave-profiles.txt line goes
// The profiles here are synthetic: the car records' layout of profile.ts, all else zero.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * as P from './web/src/app/profile.ts'; export { readBackup, addProfiles, readVault, backupItems, refillSaves, snapshotFile } from './web/src/app/saves-ui.ts';
  export { zip } from './web/src/app/zip.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const { P, readBackup, addProfiles, readVault, backupItems, refillSaves, snapshotFile, zip } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

const DIR = 'NFS Underground 2', B = 36;
const enc = new TextEncoder();
const at = n => `${DIR}/${n}/${n}`;

function seal(f) {
  f.writeUInt32LE(P.checksum(f.subarray(28, 36)), 16); f.writeUInt32LE(P.checksum(f.subarray(36)), 20); f.writeUInt32LE(P.checksum(f.subarray(0, 24)), 24);
  return new Uint8Array(f.buffer, f.byteOffset, f.length);
}
/**
 * A profile as the game writes one, as far as the page reads it: the 31 stock cars, a
 * career car (type name or hash), the 6 preset cars, the name inside.
 */
function profile(name, { career = ['240SX'], stock = 31, custom = [], inside = name, version = 0x10D } = {}) {
  const f = Buffer.alloc(54966);
  f.write('20CM', 0, 'latin1');
  f.writeUInt32LE(54966, 4); f.writeUInt32LE(8, 8); f.writeUInt32LE(54930, 12);
  f.writeUInt32LE(version, 28); f.writeUInt32LE(54930, 32);
  const type = c => typeof c === 'number' ? c : P.bStringHash(c);
  for (let i = 0; i < 48; i++) f.writeUInt32LE(i < stock ? i : 0xFFFFFFFF, B + i * 24 + 20);
  f.writeUInt32LE(stock, B + 1152);
  for (let i = 0; i < 20; i++) f.writeUInt32LE(2, B + 1156 + i * 1072 + 16);
  custom.forEach((c, i) => f.writeUInt32LE(type(c), B + 1156 + i * 1072 + 28));
  career.forEach((c, i) => f.writeUInt32LE(type(c), B + 22596 + i * 2034 + 28));
  ['MIATA', 'FOCUS', '240SX', 'COROLLA', 'PEUGOT', 'FOCUS'].forEach((c, i) => f.writeUInt32LE(type(c), B + 32770 + i * 1076 + 28));
  f.write(inside, B + 53761, 'latin1');
  return seal(f);
}
/** a cover shoot's file: the same container, 1,048,612 bytes */
const cover = n => { const d = new Uint8Array(1048612); d.set(enc.encode('20CM')); d[40] = n.length; return d; };
const z = files => zip(Object.entries(files).map(([name, data]) => ({ name, data: typeof data === 'string' ? enc.encode(data) : data })));
const sorted = a => [...a].sort();
const paths = r => sorted(r.entries.map(e => e.path));

test('bStringHash gives the car type hashes the game data holds (CarTypeInfo +0xd0)', () => {
  assert.equal(P.bStringHash('PEUGOT'), 0x72b2e513);
  assert.equal(P.bStringHash('240SX'), 0x0150fb80);
  assert.equal(P.bStringHash('IMPREZAWRX'), 0xcf4458f8);
  assert.equal(P.bStringHash('TAXI'), 0x001d1775);
});

test('modCars: none among the game\'s own cars; a car type past them, by index or by hash', () => {
  assert.equal(P.modCars(profile('A')), 0);
  assert.equal(P.modCars(profile('A', { career: [] })), 0, 'no career car yet');
  assert.equal(P.modCars(profile('A', { career: ['240SX', 'SKYLINE', 'RX8'], custom: ['GOLF'] })), 0);
  assert.equal(P.modCars(profile('A', { career: ['240SX', 0x1badcafe] })), 1, 'a career car of an unknown type');
  assert.equal(P.modCars(profile('A', { custom: [0xdeadbeef, 'MIATA'] })), 1, 'a customized one');
  assert.equal(P.modCars(profile('A', { career: ['TAXI'] })), 1, 'a traffic car is no player car');
  assert.equal(P.modCars(profile('A', { stock: 33 })), 2, 'two car types past the stock list');
});

test('profileName: the checks of a profile from elsewhere, and why one is refused', () => {
  assert.deepEqual(P.profileName(profile('Velkira')), { name: 'Velkira', edited: false });
  const edited = Buffer.from(profile('Velkira')); edited[0xA16A] ^= 0x40;      // a save editor's money change
  assert.deepEqual(P.profileName(edited), { name: 'Velkira', edited: true });
  const head = Buffer.from(profile('Velkira')); head[16] ^= 1;                 // block A's checksum alone
  assert.deepEqual(P.profileName(head), { name: 'Velkira', edited: true });
  const old = profile('Velkira', { version: 0x10C }); old[0xA16A] ^= 0x40;
  assert.match(P.profileName(old).bad, /another version/, 'another version stays out, edited or not');
  assert.match(P.profileName(profile('Velkira', { version: 0x10C })).bad, /another version/);
  assert.match(P.profileName(profile('X', { inside: 'TOOLONGNAME' })).bad, /not one the game makes/);
  assert.match(P.profileName(profile('X', { inside: 'a b' })).bad, /not one the game makes/);
  assert.match(P.profileName(profile('Velkira').subarray(0, 54000)).bad, /54000 bytes/);
});

test('sealed: an edited profile\'s checksums made again, its bytes kept; the game\'s own file comes out as it was', () => {
  const good = profile('Velkira');
  assert.deepEqual([...P.sealed(good)], [...good]);
  const edited = Buffer.from(good); edited[0xA16A] ^= 0x40; edited[16] ^= 1; edited[24] ^= 0xff;
  const s = P.sealed(new Uint8Array(edited));
  assert.deepEqual(P.profileName(s), { name: 'Velkira', edited: false });
  assert.deepEqual(P.readSaveMode(s, 'Velkira'), { mode: 'off' }, 'the page\'s own checks take it from now on');
  const changed = [];
  for (let i = 0; i < s.length; i++) if (s[i] !== edited[i]) changed.push(i);
  assert.ok(changed.every(i => i >= 16 && i < 28), `only the checksums change: ${changed}`);
  assert.equal(s[0xA16A], edited[0xA16A], 'the edit stays');
  assert.equal(edited[16], good[16] ^ 1, 'the original is not touched');
});

test('a PC save\'s profiles are found wherever the zip has them', async () => {
  const folder = await readBackup(z({
    [at('Velkira')]: profile('Velkira'), [at('Velkira DVD 1')]: cover('a'), [at('Velkira Magazine 1')]: cover('b'),
    [at('MEH')]: profile('MEH'), [`${DIR}/desktop.ini`]: '[.ShellClassInfo]\n',
  }));
  assert.equal(folder.kind, 'profiles');
  assert.deepEqual(sorted(folder.profiles), ['MEH', 'Velkira']);
  assert.deepEqual(paths(folder), sorted([at('MEH'), at('Velkira'), at('Velkira DVD 1'), at('Velkira Magazine 1')]));
  assert.deepEqual(folder.skipped, [`${DIR}/desktop.ini`]);
  assert.deepEqual([folder.refused, folder.modded, folder.edited], [[], [], []]);

  const one = await readBackup(z({ 'Velkira/Velkira': profile('Velkira') }));
  assert.deepEqual([one.kind, paths(one)], ['profiles', [at('Velkira')]], 'one profile folder, zipped');
  const two = await readBackup(z({ 'A1/A1': profile('A1'), 'B2/B2': profile('B2'), 'B2 DVD 3/B2 DVD 3': cover('c') }));
  assert.deepEqual(paths(two), sorted([at('A1'), at('B2'), at('B2 DVD 3')]), 'profile folders at the top');
  const deep = await readBackup(z({ 'backup/AppData/Local/NFS Underground 2/A1/A1': profile('A1') }));
  assert.deepEqual(paths(deep), sorted([at('A1')]));
  const cased = await readBackup(z({ 'velkira/velkira': profile('Velkira'), 'velkira DVD 1/velkira DVD 1': cover('d') }));
  assert.deepEqual(paths(cased), sorted([at('Velkira'), at('Velkira DVD 1')]), 'named as inside, covers too');
  const orphan = await readBackup(z({ 'A1/A1': profile('A1'), 'OTHER DVD 1/OTHER DVD 1': cover('e') }));
  assert.deepEqual([paths(orphan), orphan.skipped], [[at('A1')], ['OTHER DVD 1/OTHER DVD 1']], 'a cover shoot without its profile');

  const file = await readBackup(profile('Velkira'), 'Velkira (1)');
  assert.deepEqual([file.kind, paths(file), file.profiles], ['profiles', [at('Velkira')], ['Velkira']], 'a profile\'s own file');
  const lone = await readBackup(cover('x'), 'Velkira DVD 1');
  assert.deepEqual([lone.profiles, lone.refused, lone.skipped], [[], [], ['Velkira DVD 1']], 'a cover shoot alone: no profile, nothing refused');
  const edited = Buffer.from(profile('Velkira')); edited[5000] ^= 1;
  const editedFile = await readBackup(new Uint8Array(edited), 'Velkira');
  assert.deepEqual([editedFile.profiles, editedFile.edited, editedFile.refused], [['Velkira'], ['Velkira'], []], 'an edited profile\'s own file');
  assert.deepEqual(P.profileName(editedFile.entries[0].data), { name: 'Velkira', edited: false }, 'sealed again');
  const old = await readBackup(profile('Velkira', { version: 0x10C }), 'Velkira');
  assert.deepEqual([old.profiles, old.refused.map(x => x.name)], [[], ['Velkira']]);
  await assert.rejects(readBackup(enc.encode('just some text'), 'notes.txt'), /not a zip/);
});

test('a profile the game would not load is left out with why, its covers too; an edited one and a mod\'s cars warn', async () => {
  const edited = Buffer.from(profile('EDIT')); edited[0xA16A] ^= 0x40;
  const r = await readBackup(z({
    [at('EDIT')]: new Uint8Array(edited), [at('EDIT DVD 1')]: cover('f'),
    [at('OLDV')]: profile('OLDV', { version: 0x10C }), [at('OLDV DVD 1')]: cover('o'),
    [at('MOD')]: profile('MOD', { career: ['240SX', 0x5ec0ffee] }), [at('OK')]: profile('OK'),
  }));
  assert.deepEqual(sorted(r.profiles), ['EDIT', 'MOD', 'OK']);
  assert.deepEqual([r.modded, r.edited], [['MOD'], ['EDIT']]);
  assert.deepEqual(Object.fromEntries(r.refused.map(x => [x.name, x.why])), { OLDV: 'saved by another version of the game (version 268, not 269)' });
  assert.deepEqual(paths(r), sorted([at('EDIT'), at('EDIT DVD 1'), at('MOD'), at('OK')]));
  assert.deepEqual(r.skipped, [at('OLDV DVD 1')]);
  const e = r.entries.find(x => x.path === at('EDIT')).data;
  assert.deepEqual([P.profileName(e), e[0xA16A]], [{ name: 'EDIT', edited: false }, edited[0xA16A]], 'sealed again, the edit kept');
  const none = await readBackup(z({ [at('OLDV')]: profile('OLDV', { version: 0x10C }) }));
  assert.deepEqual([none.kind, none.entries, none.refused.length], ['profiles', [], 1]);
});

test('a backup of the page\'s restores in place, without a profile the game would not load', async () => {
  const edited = Buffer.from(profile('EDIT')); edited[9000] ^= 2;
  const r = await readBackup(z({
    'registry.txt': 'NotFirstTime=1\n', 'settings.ini': 'movies = off\n', 'nfsu2000.tga': new Uint8Array(40), 'autosave-profiles.txt': 'frik\n',
    [at('frik')]: profile('frik'), [at('frik DVD 1')]: cover('g'),
    [at('BAD')]: profile('BAD', { version: 0x10C }), [at('BAD Magazine 1')]: cover('h'), [at('MOD')]: profile('MOD', { custom: [0x12345678] }),
    [at('EDIT')]: new Uint8Array(edited),
  }));
  assert.equal(r.kind, 'backup');
  assert.deepEqual(sorted(r.profiles), ['EDIT', 'MOD', 'frik'], 'a cover shoot is no profile');
  assert.deepEqual([r.modded, r.edited], [['MOD'], ['EDIT']]);
  assert.deepEqual(r.refused.map(x => x.name), ['BAD']);
  assert.deepEqual(paths(r), sorted([at('EDIT'), at('MOD'), at('frik'), at('frik DVD 1'), 'autosave-profiles.txt', 'nfsu2000.tga', 'registry.txt', 'settings.ini']));
  const e = r.entries.find(x => x.path === at('EDIT')).data;
  assert.deepEqual([P.profileName(e), e[9000]], [{ name: 'EDIT', edited: false }, edited[9000]], 'sealed again, the edit kept');
  // another name inside than its folder's: its folder's written inside (it used to be left out)
  const inside = await readBackup(z({ 'registry.txt': 'x', [at('NAMEA')]: profile('NAMEB') }));
  assert.deepEqual([inside.profiles, inside.refused, inside.repaired], [['NAMEA'], [], [{ name: 'NAMEA', to: 'NAMEA' }]]);
  assert.deepEqual(P.profileName(inside.entries.find(e => e.path === at('NAMEA')).data), { name: 'NAMEA', edited: false });
});

/** the save folder in memory, as savefs.ts filesOf gives it */
function folder(files) {
  const m = new Map(Object.entries(files));
  const writes = [];
  return {
    m, writes,
    listFiles: async () => [...m.keys()].sort().map(p => ({ path: p })),
    readBytes: async p => { if (!m.has(p)) throw new Error(`no ${p}`); const v = m.get(p); return typeof v === 'string' ? enc.encode(v) : v; },
    readText: async p => { const v = m.get(p); return v === undefined ? null : typeof v === 'string' ? v : new TextDecoder().decode(v); },
    writeFile: async (p, d) => { writes.push(p); m.set(p, d); },
    remove: async p => { writes.push(`-${p}`); for (const k of [...m.keys()]) if (k === p || k.startsWith(p + '/')) m.delete(k); },
  };
}

test('adding: a profile of the same name goes with its covers (in any case), all else stays', async () => {
  const tobi = profile('TOBI'), seen = '# the page\'s notes\nvelkira\nTOBI\n';
  const fs = folder({
    [at('velkira')]: profile('velkira'), [at('velkira DVD 1')]: cover('old1'), [at('velkira DVD 2')]: cover('old2'),
    [at('TOBI')]: tobi, [at('TOBI DVD 1')]: cover('t'), 'registry.txt': 'NotFirstTime=1\n', 'settings.ini': 'movies = off\n',
    [P.AUTOSAVE_SEEN]: seen,
  });
  const vel = profile('Velkira'), c1 = cover('new1');
  const r = await readBackup(z({ 'Velkira/Velkira': vel, 'Velkira DVD 1/Velkira DVD 1': c1, 'MEH/MEH': profile('MEH') }));
  await addProfiles(fs, r);
  assert.deepEqual(sorted(fs.m.keys()), sorted([at('MEH'), at('TOBI'), at('TOBI DVD 1'), at('Velkira'), at('Velkira DVD 1'),
    P.AUTOSAVE_SEEN, 'registry.txt', 'settings.ini']));
  assert.deepEqual([...fs.m.get(at('Velkira'))], [...vel]);
  assert.deepEqual([...fs.m.get(at('Velkira DVD 1'))], [...c1]);
  assert.equal(fs.m.get(at('TOBI')), tobi);
  assert.equal(fs.m.get('settings.ini'), 'movies = off\n');
  assert.equal(fs.m.get(P.AUTOSAVE_SEEN), '# the page\'s notes\nTOBI\n', 'Autosave comes to it as to a new profile');
  assert.equal(fs.writes.findIndex(w => w.startsWith('-')), r.entries.length, `the new folders are written before the old ones go: ${fs.writes.join(', ')}`);
  assert.deepEqual(fs.writes.slice(r.entries.length), [`-${DIR}/velkira DVD 1`, `-${DIR}/velkira DVD 2`, `-${DIR}/velkira`, P.AUTOSAVE_SEEN]);

  // the same name exactly: the file is written over, an old cover shoot of it goes
  const fs2 = folder({ [at('MEH')]: profile('MEH', { career: ['RX7'] }), [at('MEH Magazine 2')]: cover('m') });
  const meh = profile('MEH');
  await addProfiles(fs2, await readBackup(meh, 'MEH'));
  assert.deepEqual([...fs2.m.keys()], [at('MEH')]);
  assert.deepEqual([...fs2.m.get(at('MEH'))], [...meh]);
  assert.deepEqual(fs2.writes, [at('MEH'), `-${DIR}/MEH Magazine 2`]);

  // ...and the old one's MPWINS.DAT goes with it (its career wins waiting for its garage are not the new one's); its
  // GARAGE.DAT stays (its fingerprint keeps it aside for a profile of other cars)
  const fs3 = folder({ [at('MEH')]: profile('MEH', { career: ['RX7'] }), [`${DIR}/MEH/MPWINS.DAT`]: 'N2MW', [`${DIR}/MEH/GARAGE.DAT`]: 'G' });
  await addProfiles(fs3, await readBackup(meh, 'MEH'));
  assert.deepEqual(sorted(fs3.m.keys()), sorted([at('MEH'), `${DIR}/MEH/GARAGE.DAT`]));
  assert.deepEqual(fs3.writes, [at('MEH'), `-${DIR}/MEH/MPWINS.DAT`], 'the new profile is written before the old wins go');
});

// The game's new-profile box in Chinese starts with its text D6 8B 89 (LANGPACKS/chinese, 0x818F0385), the field
// keeps what is typed after it: a profile named so, its folder as the I/O worker decodes the path (U+FFFD).
const ZH = Buffer.from([0xd6, 0x8b, 0x89]).toString('latin1') + 'ads';
const ZH_DIR = new TextDecoder().decode(Buffer.from(ZH, 'latin1'));
const nameOf = f => { let n = ''; for (let i = B + 53761; f[i]; i++) n += String.fromCharCode(f[i]); return n; };
/** the bytes two profile files differ in */
const diff = (a, b) => { const d = []; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) d.push(i); return d; };
const same = (a, b) => Buffer.compare(Buffer.from(a), Buffer.from(b)) === 0;
const nameOrSums = i => (i >= 20 && i < 28) || (i >= B + 53761 && i < B + 53761 + 32);

test('fixedName: its own name when the game makes it, else its letters and digits, the name inside\'s, or Profile; never one taken', () => {
  const none = new Set();
  assert.equal(P.fixedName('TOBI', ZH, none), 'TOBI');
  assert.equal(P.fixedName(ZH_DIR, ZH, none), 'ads', 'the folder\'s letters and digits');
  assert.equal(P.fixedName('名稱', ZH, none), 'ads', 'the name inside\'s ASCII letters and digits (its other bytes are the game\'s font\'s)');
  assert.equal(P.fixedName(ZH_DIR, ZH, new Set(['ads'])), 'ads2', 'digits at the end of the first, when all are taken');
  assert.equal(P.fixedName('名稱', '\x88\xa3\xd4', none), 'Profile', 'nothing of a name in either');
  assert.equal(P.fixedName('Zoë-Racer 2', '', none), 'ZoeRace', 'seven of them at most');
  assert.equal(P.fixedName('ads', ZH, new Set(['ads', 'oads', 'profile'])), 'ads2', 'digits at the end');
  assert.equal(P.fixedName('ABCDEFG', '', new Set(['abcdefg', 'profile', 'abcdef2'])), 'ABCDEF3');
});

test('a backup or a cloud version: a profile whose name inside the game does not make comes in mended, its folder\'s files and covers with it', async () => {
  const zh = profile('X', { inside: ZH }), garage = new Uint8Array([1, 2, 3, 4]), dvd = cover('zh');
  const files = {
    'settings.ini': 'movies = off\n', [at('frik')]: profile('frik'),
    [at(ZH_DIR)]: zh, [`${DIR}/${ZH_DIR}/GARAGE.DAT`]: garage, [at(`${ZH_DIR} DVD 1`)]: dvd,
  };
  for (const [how, r] of [['backup', await readBackup(z({ 'registry.txt': 'x', ...files }))], ['cloud', await readVault(z(files))]]) {
    assert.equal(r.kind, 'backup', how);
    assert.deepEqual([sorted(r.profiles), r.refused, r.repaired, r.edited], [['ads', 'frik'], [], [{ name: ZH_DIR, to: 'ads' }], []], how);
    const covers = how === 'cloud' ? [] : [at('ads DVD 1')];          // covers stay in the browser that took them (vault.ts kept)
    assert.deepEqual(paths(r).filter(p => p.startsWith(DIR)), sorted([at('ads'), `${DIR}/ads/GARAGE.DAT`, ...covers, at('frik')]), how);
    const got = r.entries.find(e => e.path === at('ads')).data;
    assert.deepEqual(P.profileName(got), { name: 'ads', edited: false }, `${how}: the name written inside, the checksums made again`);
    assert.ok(diff(zh, got).every(nameOrSums), `${how}: nothing else changes`);
    assert.ok(got.subarray(B + 53761 + 3, B + 53761 + 32).every(b => b === 0), 'NUL-padded');
    assert.ok(same(r.entries.find(e => e.path === `${DIR}/ads/GARAGE.DAT`).data, garage), `${how}: slots 6-10 go with it`);
    if (covers.length) assert.ok(same(r.entries.find(e => e.path === at('ads DVD 1')).data, dvd), 'its cover shoot too');
    assert.ok(backupItems(r, how === 'cloud').includes(`${ZH_DIR}: its name inside was broken, so it comes in as ads.`), `${how}: the question says so`);
  }
  assert.equal(nameOf(zh), ZH, 'the file read is not touched');

  // a name taken already: the next one; a save editor's change too: sealed, with its warning; a profile of another
  // version stays out as before
  const edited = Buffer.from(profile('X', { inside: ZH })); edited[0xA16A] ^= 0x40;
  const r = await readVault(z({ [at('ads')]: profile('ads'), [at(ZH_DIR)]: new Uint8Array(edited), [at('OLDV')]: profile('OLDV', { version: 0x10C }) }));
  assert.deepEqual([sorted(r.profiles), r.repaired, r.edited, r.refused.map(x => x.name)], [['ads', 'ads2'], [{ name: ZH_DIR, to: 'ads2' }], ['ads2'], ['OLDV']]);
  const e = r.entries.find(x => x.path === at('ads2')).data;
  assert.deepEqual([P.profileName(e), e[0xA16A]], [{ name: 'ads2', edited: false }, edited[0xA16A]]);
  // the folder's name when the game makes it; heap bytes of a failed cover save (2026-10-27) as well
  const heap = Buffer.from('a09c1d25f846232520d8252528d82525c4501e2530d82525cc501e25a89c1d25', 'hex').toString('latin1');
  const two = await readVault(z({ [at('DRIFTER')]: profile('X', { inside: ZH }), [at('TOBI')]: profile('X', { inside: heap }) }));
  assert.deepEqual(sorted(two.repaired.map(x => `${x.name}>${x.to}`)), ['DRIFTER>DRIFTER', 'TOBI>TOBI']);
  assert.deepEqual(paths(two), [at('DRIFTER'), at('TOBI')]);
});

test('the PC game\'s profiles: one whose name inside the game does not make comes in under its file\'s name, never one here', async () => {
  const zh = profile('X', { inside: ZH });
  const one = await readBackup(zh, 'DRIFTER');
  assert.deepEqual([one.kind, one.profiles, one.repaired, one.refused, paths(one)], ['profiles', ['DRIFTER'], [{ name: 'DRIFTER', to: 'DRIFTER' }], [], [at('DRIFTER')]]);
  assert.deepEqual(P.profileName(one.entries[0].data), { name: 'DRIFTER', edited: false });
  const here = await readBackup(zh, 'DRIFTER', ['drifter', 'TOBI']);
  assert.deepEqual([here.profiles, here.repaired], [['ads'], [{ name: 'DRIFTER', to: 'ads' }]], 'a profile here keeps its name: no replacing it');
  const named = await readBackup(zh, `${ZH_DIR} (1)`);
  assert.deepEqual(named.profiles, ['ads1'], 'the letters and digits of its file\'s name');
  const pc = await readBackup(z({ [`${DIR}/${ZH_DIR}/${ZH_DIR}`]: zh, [`${DIR}/${ZH_DIR} DVD 2/${ZH_DIR} DVD 2`]: cover('c'), 'MEH/MEH': profile('MEH') }));
  assert.deepEqual([sorted(pc.profiles), pc.repaired, pc.skipped], [['MEH', 'ads'], [{ name: ZH_DIR, to: 'ads' }], []]);
  assert.deepEqual(paths(pc), sorted([at('MEH'), at('ads'), at('ads DVD 2')]), 'its cover shoot with it');
  // added beside the profiles here: nothing of theirs goes
  const fs = folder({ [at('TOBI')]: profile('TOBI'), [at('TOBI DVD 1')]: cover('t') });
  await addProfiles(fs, await readBackup(zh, 'TOBI', ['TOBI']));
  assert.deepEqual(sorted(fs.m.keys()), sorted([at('TOBI'), at('TOBI DVD 1'), at('ads')]));
});

test('the saves back in a save folder the browser deleted during a game: the cloud\'s with this device\'s own, or the start\'s', async () => {
  const dec = new TextDecoder(), text = (fs, p) => { const v = fs.m.get(p); return typeof v === 'string' ? v : dec.decode(v); };
  // the folder as the game's start had it: two profiles, this device's settings, the graphics options, the LAN file
  const start = [
    { path: at('TOP'), data: profile('TOP', { career: ['RX7'] }) }, { path: at('OLD'), data: profile('OLD') },
    { path: 'settings.ini', data: enc.encode('roam_name = Old\nfps = 120\n') }, { path: 'registry.txt', data: enc.encode('Res=1\n') },
    { path: 'gamefile.bin', data: new Uint8Array([7, 7]) }, { path: `${DIR}/TOP/GARAGE.DAT`, data: enc.encode('g-start') },
  ];
  // the cloud's version: newer TOP, a profile from another browser, its settings (none of the device's)
  const topFile = profile('TOP', { career: ['SKYLINE'] });
  const cloud = (await readVault(z({ [at('TOP')]: topFile, [at('MEH')]: profile('MEH'), [`${DIR}/TOP/GARAGE.DAT`]: 'g-cloud',
    'settings.ini': 'roam_name = Cloud\n' }))).entries;
  // the game wrote its profile already (the page answered late): it stays, the newest
  const mine = profile('TOP', { career: ['GTO'] });
  const absent = fs => async (p, d) => { if (fs.m.has(p)) return false; await fs.writeFile(p, d); return true; };
  const fs = folder({ [at('TOP')]: mine }); fs.writeIfAbsent = absent(fs);
  assert.deepEqual(await refillSaves(fs, cloud, start), { written: 6, failed: [] });
  assert.deepEqual(sorted(fs.m.keys()), sorted([at('TOP'), at('MEH'), at('OLD'), `${DIR}/TOP/GARAGE.DAT`, 'settings.ini', 'registry.txt', 'gamefile.bin']));
  assert.equal(fs.m.get(at('TOP')), mine, 'never over a file the folder has');
  assert.equal(text(fs, `${DIR}/TOP/GARAGE.DAT`), 'g-cloud');
  assert.match(text(fs, 'settings.ini'), /roam_name = Cloud/, 'the cloud\'s settings');
  assert.match(text(fs, 'settings.ini'), /^fps\s*=\s*120$/m, 'this device\'s own from the start');
  assert.doesNotMatch(text(fs, 'settings.ini'), /Old/);
  assert.deepEqual([text(fs, 'registry.txt'), [...fs.m.get('gamefile.bin')]], ['Res=1\n', [7, 7]], 'what a keyphrase never keeps, from the start');
  assert.equal(fs.m.get(at('OLD')), start[1].data, 'every file of the start the cloud lacks comes too (never one lost)');
  // no cloud saves: the start's, all of it
  const fs2 = folder({}); fs2.writeIfAbsent = absent(fs2);
  assert.deepEqual(await refillSaves(fs2, null, start), { written: start.length, failed: [] });
  for (const f of start) assert.equal(fs2.m.get(f.path), f.data, f.path);
  // a write that fails is tried again after the others; one that fails twice is said, the rest written
  const fs3 = folder({}), tries = {};
  fs3.writeIfAbsent = async (p, d) => { tries[p] = (tries[p] ?? 0) + 1; if (p === at('OLD') || (p === at('TOP') && tries[p] === 1)) throw new Error(`no room for ${p}`); return absent(fs3)(p, d); };
  const r3 = await refillSaves(fs3, null, start);
  assert.deepEqual([r3.written, r3.failed.map(f => f.path)], [start.length - 1, [at('OLD')]]);
  assert.ok(fs3.m.has(at('TOP')) && !fs3.m.has(at('OLD')));
  // the start follows the game's saves (shell.ts onSaved), but a file written to be renamed (registry.txt.tmp)
  const snap = start.slice(), newer = profile('OLD', { career: ['SUPRA'] });
  await snapshotFile(snap, 'x', at('OLD'), newer);
  await snapshotFile(snap, 'x', at('NEW'), profile('NEW'));
  await snapshotFile(snap, 'x', 'registry.txt.tmp', enc.encode('Res=2\n'));
  await snapshotFile(snap, 'x', 'nfsu2000.tga', new Uint8Array(9));
  assert.deepEqual(snap.map(f => f.path), [...start.map(f => f.path), at('NEW')]);
  assert.equal(snap.find(f => f.path === at('OLD')).data, newer);
});
