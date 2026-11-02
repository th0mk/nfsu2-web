// The game's Save option set to Autosave once per profile before Play
// (web/src/app/profile.ts, settings.ini "autosave"):
//   node --test web/test/autosave.test.mjs
// - the profile checksum is CRC-32/BZIP2 of all but the last 4 bytes XOR those
//   4 bytes read big-endian, checked against zlib's CRC-32 on bit-reversed
//   bytes (an implementation of its own) and the CRC catalogue's check value
// - a profile is changed only when every check holds (size, header, the three
//   checksums, version, the name inside, Save bytes 0 or 1); the change is the
//   two Save bytes and the two checksums it moves, nothing else
// - each profile is set once: Autosave or Off stay, a Prompt chosen in the game
//   after the change stays, a file the checks refuse is left and looked at again
// - renaming a profile (profile.ts renameProfile) moves its folder, file and
//   cover shoots, changes the name inside and the two checksums it moves, and
//   nothing else; a refused rename, or one that fails halfway, leaves the saves as they were
// - a first profile's name field starts with Your name's letters and digits (profile.ts newProfileName), never NAME,
//   Player or a slur
// The profiles here are synthetic (the layout of profile.ts's header comment).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * as P from './web/src/app/profile.ts'; export * as S from './web/src/app/settings.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const { P, S } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

const DIR = 'NFS Underground 2';
const rev8 = b => { let r = 0; for (let k = 0; k < 8; k++) r |= ((b >> k) & 1) << (7 - k); return r; };
const rev32 = x => { let r = 0; for (let k = 0; k < 32; k++) r = (r << 1) | ((x >>> k) & 1); return r >>> 0; };
/** CRC-32/BZIP2 (MSB first, init and xorout all ones) from zlib's reflected CRC-32 */
const bzip2 = d => rev32(zlib.crc32(Buffer.from([...d].map(rev8))));
const sum = d => d.length < 4 ? 0 : (bzip2(d.subarray(0, d.length - 4)) ^ Buffer.from(d.subarray(d.length - 4)).readUInt32BE(0)) >>> 0;

/** A profile file as the game writes one: Save bytes (autosave, prompt), the name inside, everything else filler. */
function profile(name, autosave = 0, prompt = 1, { inside = name, version = 0x10D } = {}) {
  const f = Buffer.alloc(54966);
  for (let i = 36; i < f.length; i++) f[i] = (i * 7 + 3) & 0xff;      // career, cars, options: not zero
  f.write('20CM', 0, 'latin1');
  f.writeUInt32LE(54966, 4); f.writeUInt32LE(8, 8); f.writeUInt32LE(54930, 12);
  f.writeUInt32LE(version, 28); f.writeUInt32LE(54930, 32);
  f.fill(0, 36 + 53761, 36 + 53761 + 32); f.write(inside, 36 + 53761, 'latin1');
  f[36 + 50987] = autosave; f[36 + 50988] = prompt;
  return seal(f);
}
function seal(f) {
  f.writeUInt32LE(sum(f.subarray(28, 36)), 16); f.writeUInt32LE(sum(f.subarray(36)), 20); f.writeUInt32LE(sum(f.subarray(0, 24)), 24);
  return new Uint8Array(f.buffer, f.byteOffset, f.length);
}
const mode = (f, name) => P.readSaveMode(f, name);

test('checksum: CRC-32/BZIP2 but the last 4 bytes, XOR them (the catalogue check value, zlib)', () => {
  assert.equal(P.checksum(Buffer.from('123456789\0\0\0\0')), 0xFC891918);
  assert.equal(P.checksum(Buffer.from('abc')), 0);
  for (let n = 4; n < 600; n += 7) {
    const d = Buffer.alloc(n); for (let i = 0; i < n; i++) d[i] = (i * 131 + n * 7) & 0xff;
    assert.equal(P.checksum(d), sum(d), `${n} bytes`);
  }
  const a = Buffer.alloc(8); a.writeUInt32LE(0x10D, 0); a.writeUInt32LE(54930, 4);
  assert.equal(P.checksum(a), 0xF287C0BB, 'block A of every profile');
});

test('the Save option of a profile the checks accept', () => {
  assert.deepEqual(mode(profile('THOM', 0, 1), 'THOM'), { mode: 'prompt' });
  assert.deepEqual(mode(profile('THOM', 1, 1), 'THOM'), { mode: 'autosave' });
  assert.deepEqual(mode(profile('THOM', 1, 0), 'THOM'), { mode: 'autosave' });
  assert.deepEqual(mode(profile('THOM', 0, 0), 'THOM'), { mode: 'off' });
  assert.deepEqual(mode(profile('Thom', 0, 1), 'THOM'), { mode: 'prompt' }, 'the name in any case');
});

test('a file the checks refuse is never changed', () => {
  const bad = (f, name = 'THOM') => { const r = mode(f, name); assert.ok('bad' in r, JSON.stringify(r)); return r.bad; };
  const good = profile('THOM');
  assert.match(bad(good.subarray(0, 54965)), /54965 bytes/);
  const magic = Buffer.from(good); magic[0] = 0x33; assert.match(bad(seal(magic)), /20CM/);
  const edited = Buffer.from(good); edited[100] ^= 1; assert.match(bad(edited), /checksums/);    // a save editor's money change
  const head = Buffer.from(good); head[24] ^= 1; assert.match(bad(head), /checksums/);
  assert.match(bad(profile('THOM', 0, 1, { version: 0x10C })), /version/);
  assert.match(bad(good, 'OTHER'), /name inside/);
  assert.match(bad(profile('THOM', 2, 1)), /Save bytes/);
  assert.match(bad(profile('THOM', 0, 255)), /Save bytes/);
});

test('withAutosave changes the two Save bytes and the checksums they move, nothing else', () => {
  const f = profile('THOM', 0, 1), g = P.withAutosave(f);
  assert.deepEqual(mode(g, 'THOM'), { mode: 'autosave' });
  const changed = [];
  for (let i = 0; i < f.length; i++) if (f[i] !== g[i]) changed.push(i);
  assert.ok(changed.every(i => (i >= 20 && i < 28) || i === 36 + 50987), `changed bytes ${changed}`);
  assert.equal(g[36 + 50987], 1); assert.equal(g[36 + 50988], 1);
  const b = Buffer.from(g);
  assert.equal(b.readUInt32LE(20), sum(b.subarray(36))); assert.equal(b.readUInt32LE(24), sum(b.subarray(0, 24)));
  assert.equal(b.readUInt32LE(16), sum(b.subarray(28, 36)));
  assert.equal(f[36 + 50987], 0, 'the original is not touched');
});

/** the save folder in memory, as savefs.ts filesOf gives it */
function folder(files) {
  const m = new Map(Object.entries(files));
  const writes = [];
  return {
    m, writes,
    listFiles: async () => [...m.keys()].sort().map(p => ({ path: p })),
    readBytes: async p => { if (!m.has(p)) throw new Error(`no ${p}`); const v = m.get(p); return typeof v === 'string' ? new TextEncoder().encode(v) : v; },
    readText: async p => { const v = m.get(p); return v === undefined ? null : typeof v === 'string' ? v : new TextDecoder().decode(v); },
    writeFile: async (p, d) => { writes.push(p); m.set(p, d); },
    remove: async p => { writes.push(`-${p}`); for (const k of [...m.keys()]) if (k === p || k.startsWith(p + '/')) m.delete(k); },
  };
}
const at = n => `${DIR}/${n}/${n}`;

test('each profile is set once, and a choice made in the game afterwards stays', async () => {
  const corrupt = Buffer.from(profile('BROKEN')); corrupt[500] ^= 0xff;
  const fs = folder({
    [at('THOM')]: profile('THOM', 0, 1), [at('OFFP')]: profile('OFFP', 0, 0), [at('AUTO')]: profile('AUTO', 1, 1),
    [at('BROKEN')]: corrupt, 'registry.txt': 'NotFirstTime=1\n', [`${DIR}/THOM/notes.txt`]: 'x', [`${DIR}/EMPTY/other`]: 'y',
  });
  const r1 = await P.turnOnAutosave(fs, DIR);
  assert.deepEqual(r1.turnedOn, ['THOM']);
  assert.deepEqual(mode(fs.m.get(at('THOM')), 'THOM'), { mode: 'autosave' });
  assert.deepEqual(mode(fs.m.get(at('OFFP')), 'OFFP'), { mode: 'off' }, 'Off is a choice: left');
  assert.equal(fs.m.get(at('BROKEN')), corrupt, 'a refused file is not written');
  assert.ok(r1.notes.some(n => /^BROKEN left as it is: .*checksums/.test(n)), r1.notes.join('\n'));
  assert.deepEqual(fs.writes.sort(), [P.AUTOSAVE_SEEN, at('THOM')].sort());
  const seen = () => fs.m.get(P.AUTOSAVE_SEEN).split('\n').filter(l => l && !l.startsWith('#'));
  assert.deepEqual(seen(), ['AUTO', 'OFFP', 'THOM'], 'BROKEN is looked at again next time');

  // the player picks Prompt in the game: the game writes the profile with prompt, and it stays so
  fs.m.set(at('THOM'), profile('THOM', 0, 1));
  fs.writes.length = 0;
  const r2 = await P.turnOnAutosave(fs, DIR);
  assert.deepEqual(r2.turnedOn, []);
  assert.deepEqual(fs.writes, [], 'nothing changed, nothing written');
  assert.deepEqual(mode(fs.m.get(at('THOM')), 'THOM'), { mode: 'prompt' });

  // BROKEN saved again by the game (its checksums hold now), OFFP deleted, a new THOM after a delete
  fs.m.set(at('BROKEN'), profile('BROKEN', 0, 1));
  fs.m.delete(at('OFFP'));
  const r3 = await P.turnOnAutosave(fs, DIR);
  assert.deepEqual(r3.turnedOn, ['BROKEN']);
  assert.deepEqual(seen(), ['AUTO', 'BROKEN', 'THOM']);
  fs.m.delete(at('THOM'));
  await P.turnOnAutosave(fs, DIR);
  fs.m.set(at('THOM'), profile('THOM', 0, 1));
  assert.deepEqual((await P.turnOnAutosave(fs, DIR)).turnedOn, ['THOM'], 'a new profile of a deleted one\'s name');
});

test('no profiles: nothing is written', async () => {
  const fs = folder({ 'registry.txt': 'x', 'settings.ini': 'y' });
  assert.deepEqual(await P.turnOnAutosave(fs, DIR), { turnedOn: [], notes: [] });
  assert.deepEqual(fs.writes, []);
});

test('settings.ini: autosave is on unless the file says off, and older files get its paragraph', () => {
  assert.equal(S.DEFAULTS.get('autosave'), 'on');
  assert.equal(S.resolveEnv('autosave = off\n', {}, {}).env.NFSU2_AUTOSAVE, '0');
  assert.equal(S.resolveEnv('autosave = on\n', {}, {}).env.NFSU2_AUTOSAVE, '1');
  assert.equal(S.resolveEnv(S.TEMPLATE, {}, {}).env.NFSU2_AUTOSAVE, undefined, 'unset: the page treats it as on');
  const old = S.TEMPLATE.split('\n').filter(l => !/autosave|Autosave|gets it at the first Play|game's Options, Gameplay, Save and|what the game has/.test(l)).join('\n');
  assert.ok(!/autosave/.test(old));
  const up = S.settingsUpgrade(old);
  assert.ok(up && up.added.includes('autosave'), JSON.stringify(up?.added));
  assert.match(up.text, /\n#autosave = on\n/);
});

const name = f => { let n = ''; for (let i = 36 + 53761; f[i]; i++) n += String.fromCharCode(f[i]); return n; };
const cover = () => { const c = new Uint8Array(1048612); c.set([0x32, 0x30, 0x43, 0x4D]); c[5000] = 7; return c; };

test('a rename moves the profile, its cover shoots and its autosave line, and changes the name inside', async () => {
  const dvd = cover(), mag = cover();
  const fs = folder({
    [at('THOM')]: profile('THOM', 0, 0), [`${DIR}/THOM/notes.txt`]: 'x', [`${DIR}/THOM DVD 1/THOM DVD 1`]: dvd,
    [`${DIR}/THOM Magazine 12/THOM Magazine 12`]: mag, [at('AUTO')]: profile('AUTO', 1, 1), [`${DIR}/AUTO DVD 1/AUTO DVD 1`]: cover(),
    [P.AUTOSAVE_SEEN]: '# head\nAUTO\nTHOM\nTHOMAS\n', 'registry.txt': 'r',
  });
  const before = new Map(fs.m);
  assert.deepEqual(await P.renameProfile(fs, DIR, 'THOM', 'Bob7'), { files: 4 });
  const f = before.get(at('THOM')), g = fs.m.get(at('Bob7'));
  assert.deepEqual(mode(g, 'Bob7'), { mode: 'off' }, 'the checks hold under the new name, the Save option as it was');
  assert.equal(name(g), 'Bob7');
  const changed = [];
  for (let i = 0; i < f.length; i++) if (f[i] !== g[i]) changed.push(i);
  assert.ok(changed.every(i => (i >= 20 && i < 28) || (i >= 36 + 53761 && i < 36 + 53761 + 32)), `changed bytes ${changed}`);
  assert.equal(new TextDecoder().decode(fs.m.get(`${DIR}/Bob7/notes.txt`)), 'x');
  assert.equal(fs.m.get(`${DIR}/Bob7 DVD 1/Bob7 DVD 1`), dvd);
  assert.equal(fs.m.get(`${DIR}/Bob7 Magazine 12/Bob7 Magazine 12`), mag);
  assert.equal(fs.m.get(P.AUTOSAVE_SEEN), '# head\nAUTO\nBob7\nTHOMAS\n');
  assert.ok(![...fs.m.keys()].some(k => k.startsWith(`${DIR}/THOM`)), [...fs.m.keys()].join('\n'));
  for (const k of [at('AUTO'), `${DIR}/AUTO DVD 1/AUTO DVD 1`, 'registry.txt']) assert.equal(fs.m.get(k), before.get(k), `${k} untouched`);
  assert.equal(fs.writes.findIndex(w => w.startsWith('-')), fs.writes.length - 3, 'the old folders go last');
});

test('a change of case only is a rename; a longer name inside is cleared', async () => {
  const fs = folder({ [at('thom')]: profile('thom', 1, 1, { inside: 'THOM' }) });
  assert.deepEqual(await P.renameProfile(fs, DIR, 'thom', 'Thom'), { files: 1 });
  assert.deepEqual([...fs.m.keys()], [at('Thom')]);
  assert.deepEqual(mode(fs.m.get(at('Thom')), 'Thom'), { mode: 'autosave' });
  const fs2 = folder({ [at('ABCDEFG')]: profile('ABCDEFG') });
  await P.renameProfile(fs2, DIR, 'ABCDEFG', 'X');
  const g = fs2.m.get(at('X'));
  assert.equal(name(g), 'X');
  assert.ok(g.subarray(36 + 53762, 36 + 53761 + 32).every(b => b === 0), 'NUL-padded');
});

test('a profile the game named with stale heap bytes gets a name again', async () => {
  // a failed cover shoot's save before runtime/overrides.c sub_004e8b70: 32 bytes of heap addresses, no NUL; the game's
  // folder takes 31 of them, read as UTF-8 by the I/O worker (U+FFFD for the bytes that are not)
  const stale = Buffer.from('a09c1d25f846232520d8252528d82525c4501e2530d82525cc501e25a89c1d25', 'hex');
  const dir = new TextDecoder().decode(stale.subarray(0, 31));
  assert.ok(dir.includes(String.fromCharCode(0xfffd)) && !dir.endsWith('%'));
  const f = profile('X', 1, 1, { inside: stale.toString('latin1') });
  assert.deepEqual(mode(f, dir), { mode: 'autosave' });
  assert.match(mode(f, new TextDecoder().decode(stale)).bad ?? '', /name inside/, 'all 32 bytes: not the game\'s folder');
  const fs = folder({ [at(dir)]: f, [at('OTHER')]: profile('OTHER') });
  assert.deepEqual(await P.renameProfile(fs, DIR, dir, 'NAME2'), { files: 1 });
  assert.deepEqual([...fs.m.keys()].sort(), [at('NAME2'), at('OTHER')]);
  const g = fs.m.get(at('NAME2'));
  assert.equal(name(g), 'NAME2');
  assert.ok(g.subarray(36 + 53766, 36 + 53761 + 32).every(b => b === 0), 'NUL-padded');
  assert.deepEqual(mode(g, 'NAME2'), { mode: 'autosave' });
});

test('a refused rename writes nothing', async () => {
  const corrupt = Buffer.from(profile('BROKEN')); corrupt[500] ^= 0xff;
  const files = { [at('THOM')]: profile('THOM'), [at('AUTO')]: profile('AUTO'), [at('BROKEN')]: corrupt, [`${DIR}/NEW DVD 2/NEW DVD 2`]: cover() };
  const refused = async (from, to, re) => {
    const fs = folder(files);
    const r = await P.renameProfile(fs, DIR, from, to);
    assert.match(r.refused ?? '', re, `${from} -> ${to}: ${JSON.stringify(r)}`);
    assert.deepEqual(fs.writes, []);
  };
  for (const bad of ['', 'ABCDEFGH', 'A B', 'A-B', 'Thé', 'A/B', '..']) await refused('THOM', bad, /1 to 7 letters and digits/);
  await refused('THOM', 'auto', /There is a profile AUTO already/);
  await refused('THOM', 'new', /cover shoot NEW DVD 2/);
  await refused('BROKEN', 'FIXED', /BROKEN cannot be renamed here: its checksums/);
  await refused('NOBODY', 'X', /no profile NOBODY/);
  assert.deepEqual(await P.renameProfile(folder(files), DIR, 'THOM', 'THOM'), { files: 0 });
});

test('a rename that fails halfway leaves the profile as it was', async () => {
  const fs = folder({ [at('THOM')]: profile('THOM'), [`${DIR}/THOM DVD 1/THOM DVD 1`]: cover() });
  const before = new Map(fs.m);
  const write = fs.writeFile;
  let n = 0;
  fs.writeFile = async (p, d) => { if (++n === 2) throw new Error('disk full'); return write(p, d); };
  await assert.rejects(P.renameProfile(fs, DIR, 'THOM', 'BOB'), /disk full/);
  assert.deepEqual([...fs.m.keys()].sort(), [...before.keys()].sort());
  for (const [k, v] of before) assert.equal(fs.m.get(k), v);
});

test('cover shoots are told from profiles by their folder name', () => {
  assert.deepEqual(P.coverOf('CLAUDE DVD 1'), { profile: 'CLAUDE', cover: 'DVD 1' });
  assert.deepEqual(P.coverOf('A B Magazine 10'), { profile: 'A B', cover: 'Magazine 10' });
  for (const f of ['CLAUDE', 'DVD 1', 'CLAUDE DVD', 'CLAUDE dvd 1', 'CLAUDE DVD 1x']) assert.equal(P.coverOf(f), null, f);
});

test('a first profile\'s name field starts with Your name, as the game\'s field takes it', () => {
  const cases = [['Alex', 'Alex'], ['Alex_the_Great', 'Alexthe'], ['Dani-99', 'Dani99'], ['RapidRacer', 'RapidRa'],
    ['NAME', ''], ['player', ''], ['N_A_M_E', ''], ['__', ''], ['', ''], ['Nigger', ''], ['N-i-g-g-e-r', '']];
  for (const [i, o] of cases) assert.equal(P.newProfileName(i), o, JSON.stringify(i));
  for (const [, o] of cases) if (o) assert.ok(P.isProfileName(o), o);
});
