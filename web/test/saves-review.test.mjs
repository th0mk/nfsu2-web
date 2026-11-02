// Adversarial checks of the saves code around profiles whose name inside is broken (a530a5a, cc3e413): restores,
// cloud takes and renames against an in-memory OPFS (savefs.ts runs on it as in the page), two browsers on a fake
// /vault (vault.test.mjs's rules), and real profiles when RV_REAL_DIR names a folder of them (<name>.bin, as in the
// page's backups; repo tests stay synthetic otherwise).
//   node --test web/test/saves-review.test.mjs
//   RV_REAL_DIR=/path/to/profiles node --test web/test/saves-review.test.mjs
// Each data-loss path found in review is a test (they were marked todo until fixed: saves-ui.ts, vault.ts, 2026-11-02).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * as P from './web/src/app/profile.ts'; export * as V from './web/src/app/vault.ts';
  export { readBackup, readVault, restoreVault, restoreSaves, addProfiles, leftOut, profilesComing } from './web/src/app/saves-ui.ts';
  export { listFiles, listProfiles, filesOf, readBytes, readBeside, writeBeside, removeSaveDir, settleStaged, writeTogether } from './web/src/app/savefs.ts';
  export { zip, unzip } from './web/src/app/zip.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const { P, V, readBackup, readVault, restoreVault, restoreSaves, addProfiles, leftOut, profilesComing, listFiles, listProfiles, filesOf, readBytes, readBeside, writeBeside, removeSaveDir, settleStaged, writeTogether, zip, unzip } =
  await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

const DIR = 'NFS Underground 2', B = 36, NAME = B + 53761;
const enc = new TextEncoder(), dec = new TextDecoder();
const at = n => `${DIR}/${n}/${n}`;
const sorted = a => [...a].sort();

// ------------------------------------------------------------------ an OPFS in memory --
// What savefs.ts uses: getDirectoryHandle / getFileHandle ({ create }), removeEntry ({ recursive }), entries(),
// getFile() (size, lastModified, arrayBuffer, text), createWritable() (write, close: the file changes at close, as
// OPFS's swap file does).  failAfter: that many more writes succeed, then each throws QuotaExceededError.
const opfs = { failAfter: Infinity, clock: 1_700_000_000_000, held: new Set() };   // held: files a worker holds open
const err = name => new DOMException(name, name);
class FileH {
  constructor(name, parent) { this.kind = 'file'; this.name = name; this.parent = parent; this.data = new Uint8Array(0); this.mtime = opfs.clock; }
  /** as Chrome 156 and Firefox 157 do (checked, rv-move.mjs): over an existing file; refused over one held open, and
   *  for one held open itself (Chrome 156 checked: sr-chromecheck2.mjs) */
  async move(a, b) {
    const dest = typeof a === 'string' ? this.parent : a, name = typeof a === 'string' ? a : b, there = dest.kids.get(name);
    if (there && there.kind !== 'file') throw err('TypeMismatchError');
    if (there && opfs.held.has(there) || opfs.held.has(this)) throw err('NoModificationAllowedError');
    this.parent.kids.delete(this.name); this.name = name; this.parent = dest; dest.kids.set(name, this);
  }
  async getFile() { const d = this.data, m = this.mtime; return { size: d.length, lastModified: m, arrayBuffer: async () => d.slice().buffer, text: async () => dec.decode(d) }; }
  async createWritable() {
    const parts = [];
    return {
      write: async x => {
        if (opfs.failAfter-- <= 0) throw err('QuotaExceededError');
        parts.push(typeof x === 'string' ? enc.encode(x) : x instanceof Blob ? new Uint8Array(await x.arrayBuffer()) : new Uint8Array(x.buffer ? x.buffer.slice(x.byteOffset, x.byteOffset + x.byteLength) : x));
      },
      close: async () => { this.data = Buffer.concat(parts.map(p => Buffer.from(p))); this.data = new Uint8Array(this.data); this.mtime = opfs.clock += 1000; },
    };
  }
}
class DirH {
  constructor(name) { this.kind = 'directory'; this.name = name; this.kids = new Map(); }
  async getDirectoryHandle(n, { create = false } = {}) {
    const k = this.kids.get(n);
    if (k) { if (k.kind !== 'directory') throw err('TypeMismatchError'); return k; }
    if (!create) throw err('NotFoundError');
    const d = new DirH(n); this.kids.set(n, d); return d;
  }
  async getFileHandle(n, { create = false } = {}) {
    const k = this.kids.get(n);
    if (k) { if (k.kind !== 'file') throw err('TypeMismatchError'); return k; }
    if (!create) throw err('NotFoundError');
    const f = new FileH(n, this); this.kids.set(n, f); return f;
  }
  async removeEntry(n, { recursive = false } = {}) {
    const k = this.kids.get(n);
    if (!k) throw err('NotFoundError');
    if (opfs.held.has(k)) throw err('NoModificationAllowedError');
    if (k.kind === 'directory' && k.kids.size && !recursive) throw err('InvalidModificationError');
    this.kids.delete(n);
  }
  async *entries() { for (const e of [...this.kids]) yield e; }
}
let root = new DirH('');
const storage = { getDirectory: async () => root };
if (globalThis.navigator) Object.defineProperty(globalThis.navigator, 'storage', { value: storage, configurable: true });
else Object.defineProperty(globalThis, 'navigator', { value: { storage }, configurable: true });
/** a fresh OPFS with these save folders: { saveDir: { path: bytes | text } } */
async function seedOpfs(dirs) {
  root = new DirH(''); opfs.failAfter = Infinity; opfs.held.clear();
  for (const [dir, files] of Object.entries(dirs)) for (const [p, d] of Object.entries(files)) await filesOf(dir).writeFile(p, typeof d === 'string' ? enc.encode(d) : d);
}
/** a save folder's files: path -> bytes */
async function folderOf(dir) {
  const out = new Map();
  for (const f of await listFiles(dir)) out.set(f.path, await readBytes(dir, f.path));
  return out;
}

// ------------------------------------------------------------------------- profiles --
function seal(f) {
  f.writeUInt32LE(P.checksum(f.subarray(28, 36)), 16); f.writeUInt32LE(P.checksum(f.subarray(36)), 20); f.writeUInt32LE(P.checksum(f.subarray(0, 24)), 24);
  return new Uint8Array(f.buffer, f.byteOffset, f.length);
}
/** a profile as the game writes one (as far as the page reads it), with this name inside (latin1: a byte a character) */
function profile(inside, { money = 1000, stale = false } = {}) {
  const f = Buffer.alloc(54966);
  f.write('20CM', 0, 'latin1');
  f.writeUInt32LE(54966, 4); f.writeUInt32LE(8, 8); f.writeUInt32LE(54930, 12);
  f.writeUInt32LE(0x10D, 28); f.writeUInt32LE(54930, 32);
  for (let i = 0; i < 48; i++) f.writeUInt32LE(i < 31 ? i : 0xFFFFFFFF, B + i * 24 + 20);
  f.writeUInt32LE(31, B + 1152);
  f.write(inside, NAME, 'latin1');
  f.writeUInt32LE(money, 0xA16A);
  if (stale) return new Uint8Array(f.buffer, f.byteOffset, f.length);
  return seal(f);
}
const cover = n => { const d = new Uint8Array(1048612); d.set(enc.encode('20CM')); d[40] = n; return d; };
const money = f => new DataView(f.buffer, f.byteOffset, f.byteLength).getUint32(0xA16A, true);
const nameOf = f => { let n = ''; for (let i = NAME; i < NAME + 32 && f[i]; i++) n += String.fromCharCode(f[i]); return n; };
/** the folder the game saves a profile named so in: its first 31 bytes, as the I/O worker reads a path (UTF-8, U+FFFD) */
const folderFor = inside => dec.decode(Buffer.from(inside.slice(0, 31), 'latin1'));
/** the Chinese pack's NAME (D6 8B 89) kept in front of what was typed (Discord 2026-11-02) */
const ZH = Buffer.from([0xd6, 0x8b, 0x89]).toString('latin1') + 'ads', ZH_DIR = folderFor(ZH);
/** the heap bytes a failed cover shoot's save named a profile with (2026-10-27; pcsave.test.mjs has them too): 32 bytes,
 *  no NUL, with 0x1D and 0x1E among them; the game's folder for it is their first 31, decoded */
const HEAP = Buffer.from('a09c1d25f846232520d8252528d82525c4501e2530d82525cc501e25a89c1d25', 'hex').toString('latin1'), HEAP_DIR = folderFor(HEAP);
const z = files => zip(Object.entries(files).map(([name, data]) => ({ name, data: typeof data === 'string' ? enc.encode(data) : data })));
const diff = (a, b) => { const d = []; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) d.push(i); return d; };
const nameOrSums = i => (i >= 16 && i < 28) || (i >= NAME && i < NAME + 32);

// the real profiles (RV_REAL_DIR): name -> bytes
const REAL = process.env.RV_REAL_DIR;
const real = REAL && fs.existsSync(REAL) ? Object.fromEntries(fs.readdirSync(REAL).filter(f => f.endsWith('.bin') && !/DVD|Magazine/.test(f))
  .map(f => [f.slice(0, -4), new Uint8Array(fs.readFileSync(path.join(REAL, f)))])) : null;
// a real cover shoot's file among them (<profile>_DVD_1.bin), else a made-up one
const coverFile = REAL && fs.existsSync(REAL) ? fs.readdirSync(REAL).find(f => /_DVD_1\.bin$/.test(f)) : undefined;
const realCover = coverFile ? new Uint8Array(fs.readFileSync(path.join(REAL, coverFile))) : cover(7);
const withInside = (f, inside) => { const g = Buffer.from(f); g.fill(0, NAME, NAME + 32); g.write(inside, NAME, 'latin1'); return seal(g); };

// ------------------------------------------------------------- the heap-named profile --

test('the folder the game makes for the heap-named profile has control characters (what safeEntryPath refuses)', () => {
  assert.ok(/[\x00-\x1f]/.test(HEAP_DIR), JSON.stringify(HEAP_DIR));
  assert.deepEqual(P.readSaveMode(profile(HEAP), HEAP_DIR).bad, undefined, 'the page\'s own checks take it in the save folder (2e9ecee)');
});

test('a heap-named profile in its own folder comes in from a cloud version, a backup and a Put back, mended', async () => {
  const h = profile(HEAP, { money: 777777 });
  const files = { 'settings.ini': '# s\n', [at(HEAP_DIR)]: h, [`${DIR}/${HEAP_DIR}/GARAGE.DAT`]: new Uint8Array([9, 9]) };
  const lost = [];
  // (a cloud version, Go back and Put back all read it with readVault; a backup with readBackup)
  for (const [how, r] of [['cloud', await readVault(z(files))], ['backup', await readBackup(z({ 'registry.txt': 'x', ...files }))]]) {
    const main = r.profiles.length === 1 && r.entries.find(e => e.path === at(r.profiles[0]));
    if (!main || money(main.data) !== 777777) lost.push(`${how}: profiles ${JSON.stringify(r.profiles)}, skipped ${r.skipped.length} (${r.skipped.map(s => JSON.stringify(s.slice(-12))).join(', ')})`);
  }
  assert.deepEqual(lost, [], 'the career comes in every way');
});

test('two browsers: B takes a version with a heap-named profile, plays another, A takes it back: the career is gone', async () => {
  const srv = server();
  await seedOpfs({
    A: { 'settings.ini': '# s\n', [at(HEAP_DIR)]: profile(HEAP, { money: 500000 }), [at('MEH')]: profile('MEH', { money: 10 }) },
    B: {},
  });
  const a = browser(srv, 'A'), b = browser(srv, 'B');
  const phrase = await a.sync.create();
  const got = await b.sync.peek(phrase);
  await restoreVault('B', await readVault(got.zip));
  await b.sync.adopt(phrase, got.remote);
  // B plays MEH
  await filesOf('B').writeFile(at('MEH'), profile('MEH', { money: 20 }));
  await b.sync.push();
  // A has not played since: it takes B's version by itself
  await a.sync.pull();
  const left = await folderOf('A');
  const careers = [...left].filter(([p, d]) => p.split('/').length === 3 && d.length === 54966).map(([, d]) => money(d));
  assert.ok(careers.includes(500000), `A keeps the 500,000 career: ${JSON.stringify(careers)}`);
});

// ------------------------------------------------------- covers of a mended profile --

test('a cloud take keeps this browser\'s cover shoots of a profile whose name it mends', async () => {
  await seedOpfs({ C: {
    'settings.ini': '# s\n', [at(ZH_DIR)]: profile(ZH, { money: 4242 }), [`${DIR}/${ZH_DIR}/GARAGE.DAT`]: new Uint8Array([1, 2]),
    [at(`${ZH_DIR} DVD 1`)]: cover(1), [at(`${ZH_DIR} Magazine 2`)]: cover(2),
  } });
  // the version is this browser's own (Go back, Use the cloud's, Put back) or another browser's of the same saves
  const sent = V.kept([...await folderOf('C')].map(([path, data]) => ({ path, data })));
  const r = await readVault(zip(sent.map(f => ({ name: f.path, data: f.data }))));
  assert.deepEqual(r.repaired, [{ name: ZH_DIR, to: 'ads' }]);
  await restoreVault('C', r);
  const left = await folderOf('C');
  assert.equal(money(left.get(at('ads'))), 4242);
  const covers = [...left.keys()].filter(p => P.coverOf(p.split('/')[1]));
  assert.equal(covers.length, 2, `its 2 cover shoots stay: ${JSON.stringify([...left.keys()])}`);
});

// ------------------------------------------------------------ names that collide --

test('a backup with two folders of one name in different case: the mended one never lands on the other', async () => {
  const good = profile('abc', { money: 111 }), broken = profile(ZH, { money: 222 });
  const r = await readVault(z({ [at('abc')]: good, [at('ABC')]: broken }));
  const ps = r.entries.map(e => e.path);
  assert.equal(new Set(ps).size, ps.length, `no path twice: ${JSON.stringify(ps)}`);
  assert.equal(money(r.entries.find(e => e.path === at('abc'))?.data ?? new Uint8Array(54966)), 111, 'abc stays itself');
});

test('a mended profile never shares a folder of the restore that holds files without a profile', async () => {
  const r = await readVault(z({ [`${DIR}/ads/GARAGE.DAT`]: new Uint8Array([1]), [at(ZH_DIR)]: profile(ZH), [`${DIR}/${ZH_DIR}/GARAGE.DAT`]: new Uint8Array([2]) }));
  const ps = r.entries.map(e => e.path);
  assert.equal(new Set(ps).size, ps.length, `no path twice: ${JSON.stringify(ps)}`);
});

test('the PC game\'s profiles: a mended one and one here of its name, a second copy of it, and its covers', async () => {
  // added beside the profiles here: never one of theirs replaced; a second copy of a mended profile comes in too
  const zh = profile(ZH, { money: 5 });
  const r = await readBackup(z({ [`a/${DIR}/${ZH_DIR}/${ZH_DIR}`]: zh, [`b/${DIR}/${ZH_DIR}/${ZH_DIR}`]: zh, [`a/${DIR}/${ZH_DIR} DVD 1/${ZH_DIR} DVD 1`]: cover(3) }), 'x.zip', ['ADS']);
  assert.deepEqual(sorted(r.profiles), ['ads2', 'ads3'], 'neither takes ADS here');
  assert.equal(r.entries.filter(e => P.coverOf(e.path.split('/')[1])).length, 1);
  await seedOpfs({ D: { [at('ADS')]: profile('ADS', { money: 99 }), [at('ADS DVD 1')]: cover(4) } });
  await addProfiles(filesOf('D'), r);
  const left = await folderOf('D');
  assert.equal(money(left.get(at('ADS'))), 99, 'ADS here untouched');
  assert.ok(left.has(at('ADS DVD 1')), 'and its cover');
});

// ------------------------------------------------------------------------ rename --

test('rename: a profile whose name inside is another profile\'s leaves that one as it is', async () => {
  const other = profile('BOB', { money: 1 });
  await seedOpfs({ R: { [at('ALICE')]: profile('BOB', { money: 2 }), [at('BOB')]: other, [at('ALICE DVD 1')]: cover(5) } });
  assert.deepEqual(await P.renameProfile(filesOf('R'), DIR, 'ALICE', 'BOB'), { refused: 'There is a profile BOB already.' });
  assert.deepEqual(await P.renameProfile(filesOf('R'), DIR, 'ALICE', 'CAROL'), { files: 2 });
  const left = await folderOf('R');
  assert.deepEqual(sorted(left.keys()), sorted([at('BOB'), at('CAROL'), at('CAROL DVD 1')]));
  assert.deepEqual(left.get(at('BOB')), other, 'BOB untouched');
  assert.deepEqual([nameOf(left.get(at('CAROL'))), money(left.get(at('CAROL')))], ['CAROL', 2]);
});

test('rename stays strict: a broken name with stale checksums is refused and nothing is written', async () => {
  for (const inside of [ZH, HEAP]) {
    const dir = folderFor(inside), stale = profile(inside, { stale: true });
    await seedOpfs({ S: { [at(dir)]: stale, [`${DIR}/${dir}/GARAGE.DAT`]: new Uint8Array([1]) } });
    const r = await P.renameProfile(filesOf('S'), DIR, dir, 'NEW');
    assert.match(r.refused ?? '', /checksums do not match/);
    assert.deepEqual(sorted((await folderOf('S')).keys()), sorted([at(dir), `${DIR}/${dir}/GARAGE.DAT`]));
  }
});

test('rename of the heap-named profile (control characters in its folder) moves everything', async () => {
  await seedOpfs({ H: { [at(HEAP_DIR)]: profile(HEAP, { money: 31337 }), [`${DIR}/${HEAP_DIR}/GARAGE.DAT`]: new Uint8Array([7]), [at(`${HEAP_DIR} DVD 1`)]: cover(6) } });
  // coverOf takes "<heap> DVD 1" (the heap folder has a space and "%" in it, never " DVD n" at its end)
  assert.deepEqual(await P.renameProfile(filesOf('H'), DIR, HEAP_DIR, 'MINE'), { files: 3 });
  const left = await folderOf('H');
  assert.deepEqual(sorted(left.keys()), sorted([at('MINE'), `${DIR}/MINE/GARAGE.DAT`, at('MINE DVD 1')]));
  assert.deepEqual(P.readSaveMode(left.get(at('MINE')), 'MINE').bad, undefined);
  assert.equal(money(left.get(at('MINE'))), 31337);
});

// ------------------------------------------------------------ restores that stop --

test('a cloud take or a backup restore that stops halfway (no room) leaves the profiles here', async () => {
  const incoming = await readVault(z({ 'settings.ini': '# s\n', [at('NEW1')]: profile('NEW1'), [at('NEW2')]: profile('NEW2') }));
  for (const [how, run] of [['cloud', () => restoreVault('Q', incoming)], ['backup', () => restoreSaves('Q', incoming)]]) {
    await seedOpfs({ Q: { 'settings.ini': '# mine\n', [at('TOP')]: profile('TOP', { money: 123456 }) } });
    opfs.failAfter = 1;
    await assert.rejects(run(), /QuotaExceeded/);
    opfs.failAfter = Infinity;
    const left = await folderOf('Q');
    const whole = ['NEW1', 'NEW2'].every(n => left.has(at(n)) && left.get(at(n)).length === 54966);
    assert.ok(left.has(at('TOP')) || whole, `${how}: TOP gone and the restore not complete: ${JSON.stringify([...left.keys()])}`);
  }
});

test('a cloud take that stops halfway is never sent over the cloud\'s version', async () => {
  const srv = server();
  await seedOpfs({ A: { 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 1 }), [at('MEH')]: profile('MEH', { money: 1 }) }, B: {} });
  const a = browser(srv, 'A'), b = browser(srv, 'B');
  const phrase = await a.sync.create();
  const got = await b.sync.peek(phrase);
  await restoreVault('B', await readVault(got.zip));
  await b.sync.adopt(phrase, got.remote);
  await filesOf('A').writeFile(at('TOP'), profile('TOP', { money: 2 }));
  await filesOf('A').writeFile(at('MEH'), profile('MEH', { money: 2 }));
  await a.sync.push();
  const rev = a.sync.link.rev;
  // B takes it, and its writes stop after the first
  opfs.failAfter = 1;
  await b.sync.pull().catch(() => {});
  opfs.failAfter = Infinity;
  await b.sync.push().catch(() => {});
  assert.equal(srv.revOf(phrase, a), rev, 'the cloud keeps A\'s version');
  // (written all or none: savefs.ts writeTogether) B's saves are its own version still, whole, and A's comes in next
  const before = await folderOf('B');
  assert.deepEqual([money(before.get(at('TOP'))), money(before.get(at('MEH')))], [1, 1], 'B\'s version whole: nothing of A\'s written');
  assert.equal([...before.keys()].some(k => k.endsWith('.incoming')), false, 'no file left half way');
  await b.sync.pull();
  const after = await folderOf('B');
  assert.deepEqual([money(after.get(at('TOP'))), money(after.get(at('MEH'))), b.sync.state.s], [2, 2, 'synced'], 'A\'s version at the next pull');
});

// -------------------------------------------------------------------- real profiles --

test('real profiles with a broken name come in mended from a cloud version, a backup and the PC game: name and checksums only', { skip: !real && 'RV_REAL_DIR not set' }, async () => {
  for (const [n, f] of Object.entries(real)) {
    assert.deepEqual(P.profileName(f).name, n, `${n} is whole`);
    // the Chinese NAME in front, a lone font byte in front, and another name than its folder's
    for (const [inside, dir] of [[ZH, ZH_DIR], ['\x8B' + n.slice(0, 6), folderFor('\x8B' + n.slice(0, 6))], ['OTHER', n]]) {
      const broken = withInside(f, inside);
      const vault = await readVault(z({ [at(dir)]: broken, [`${DIR}/${dir}/GARAGE.DAT`]: new Uint8Array([3]) }));
      const pc = await readBackup(broken, `${dir}`, []);
      for (const [how, r] of [['cloud', vault], ['pc', pc]]) {
        if (how === 'pc' && P.isProfileName(inside)) { assert.deepEqual([r.repaired, r.profiles], [[], [inside]], `${n}/${how}: found by the name inside`); continue; }
        if (how === 'cloud') assert.ok(r.entries.some(e => e.path === `${DIR}/${r.profiles[0]}/GARAGE.DAT`), `${n}: GARAGE.DAT goes with it`);
        assert.equal(r.profiles.length, 1, `${n}/${how}`);
        const to = r.profiles[0], got = r.entries.find(e => e.path === at(to)).data;
        assert.ok(P.isProfileName(to), `${n}/${how}: ${to}`);
        assert.ok(diff(f, got).every(nameOrSums), `${n}/${how}: only the name and the checksums change`);
        assert.equal(P.readSaveMode(got, to).bad, undefined, `${n}/${how}: Autosave and Rename take it as it came in`);
        assert.deepEqual(got.subarray(B, B + 39230), f.subarray(B, B + 39230), `${n}/${how}: GARAGE.DAT's fingerprint part`);
      }
    }
  }
});

test('real profiles: rename mends a broken name in the save folder, its cover with it; an edited one stays refused', { skip: !real && 'RV_REAL_DIR not set' }, async () => {
  for (const [n, f] of Object.entries(real)) {
    const broken = withInside(f, ZH);
    await seedOpfs({ W: { [at(ZH_DIR)]: broken, [`${DIR}/${ZH_DIR}/GARAGE.DAT`]: new Uint8Array([5]), [at(`${ZH_DIR} DVD 1`)]: realCover } });
    const to = n.slice(0, 7).replace(/[^A-Za-z0-9]/g, '') || 'X';
    assert.deepEqual(await P.renameProfile(filesOf('W'), DIR, ZH_DIR, to), { files: 3 }, n);
    const left = await folderOf('W');
    assert.deepEqual(sorted(left.keys()), sorted([at(to), `${DIR}/${to}/GARAGE.DAT`, at(`${to} DVD 1`)]), n);
    assert.ok(diff(f, left.get(at(to))).every(nameOrSums), `${n}: only the name and the checksums`);
    assert.deepEqual(left.get(at(`${to} DVD 1`)), realCover);
    const edited = Buffer.from(broken); edited[0xA16A] ^= 0x40;
    await seedOpfs({ W: { [at(ZH_DIR)]: new Uint8Array(edited) } });
    assert.match((await P.renameProfile(filesOf('W'), DIR, ZH_DIR, to)).refused ?? '', /checksums/, `${n}: an edited one is not repaired by a rename`);
  }
});

// ----------------------------------------------------------- the fake server, browsers --

function server() {
  const kept = new Map(), s = { now: Date.parse('2026-11-02T12:00:00Z'), kept };
  const tag = n => `"${n.toString(16).padStart(16, '0')}"`;
  let revs = 1;
  const head = v => ({ etag: tag(v.rev), 'x-vault-saved': String(v.saved) });
  s.fetch = async (url, init = {}) => {
    const h = Object.fromEntries(Object.entries(init.headers ?? {}).map(([k, v]) => [k.toLowerCase(), v]));
    const token = (h.authorization ?? '').replace(/^Bearer /, '');
    if (!token) return new Response('token', { status: 401 });
    const k = kept.get(token), method = init.method ?? 'GET';
    if (method === 'GET') {
      const v = k?.cur;
      if (!v || url.endsWith('?earlier')) return new Response('Not found', { status: 404 });
      if (h['if-none-match'] === tag(v.rev)) return new Response(null, { status: 304, headers: head(v) });
      return new Response(v.body, { status: 200, headers: head(v) });
    }
    if (method === 'PUT') {
      if (h['if-none-match'] === '*' ? k : !k || h['if-match'] !== tag(k.cur.rev)) return new Response('Changed', { status: 412, headers: k ? head(k.cur) : {} });
      s.now += 60_000;
      const v = { rev: revs++, saved: s.now, body: new Uint8Array(init.body) };
      kept.set(token, { cur: v });
      return new Response('Saved', { status: k ? 200 : 201, headers: head(v) });
    }
    return new Response('no', { status: 405 });
  };
  s.revOf = (phrase, b) => { for (const v of kept.values()) return tag(v.cur.rev); };
  return s;
}

/** a browser on save folder `dir` of the OPFS: vault-ui.ts's files() and restore() (readVault, restoreVault) */
function browser(srv, dir) {
  const b = { link: null, taken: 0 };
  b.deps = {
    files: async (o = {}) => {
      const want = o.was ? V.keptBy(o.was) : V.keeps, out = [];
      for (const f of await listFiles(dir)) if (want(f.path)) out.push({ path: f.path, data: await readBytes(dir, f.path), mtime: f.mtime });
      return V.kept(out, o.was);
    },
    restore: async (zipped, still) => restoreVault(dir, await readVault(zipped), still),
    load: () => b.link && JSON.parse(b.link),
    save: l => { b.link = l && JSON.stringify(l); },
    fetch: srv.fetch,
    log: () => {},
    leftOut: files => leftOut(files), named: files => profilesComing(files),
    folder: () => readBeside(`${dir}~folder`), mark: id => writeBeside(`${dir}~folder`, id),
  };
  b.sync = new V.VaultSync(b.deps);
  b.sync.onTaken = () => b.taken++;
  return b;
}

// ----------------------------------------------- the cloud never takes or gets less by itself --

test('a version another browser sent with a profile this one cannot take in: asked, never taken by itself', async () => {
  const srv = server();
  await seedOpfs({ A: { 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 7 }) }, B: {} });
  const a = browser(srv, 'A'), b = browser(srv, 'B');
  const phrase = await a.sync.create();
  const got = await b.sync.peek(phrase);
  await restoreVault('B', await readVault(got.zip));
  await b.sync.adopt(phrase, got.remote);
  // B gets a profile of another version of the game (a copy from elsewhere put in its folder) and sends it
  const old = Buffer.from(profile('OLDV')); old.writeUInt32LE(0x10C, 28);
  await filesOf('B').writeFile(at('OLDV'), new Uint8Array(old));
  await filesOf('B').writeFile(at('TOP'), profile('TOP', { money: 8 }));
  await b.sync.push();
  // (B sends no profile that is not whole by itself either: its player keeps B's, or an older page sent it)
  assert.deepEqual([b.sync.state.s, b.sync.state.why], ['conflict', 'fewer']);
  await b.sync.keepMine();
  assert.deepEqual(leftOut((await unzip((await b.sync.peek(phrase)).zip)).map(e => ({ path: e.name, data: e.data }))), ['OLDV']);
  const before = await folderOf('A');
  await a.sync.pull();
  assert.deepEqual([a.sync.state.s, a.sync.state.why], ['conflict', 'drops']);
  const after = await folderOf('A');
  assert.deepEqual(sorted(after.keys()), sorted(before.keys()), 'nothing here changed');
  assert.equal(money(after.get(at('TOP'))), 7);
});

test('a save folder deleted since it synced (its mark gone) is never sent by itself; profiles deleted or renamed on purpose are', async () => {
  const srv = server();
  await seedOpfs({ A: { 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 1 }), [at('MEH')]: profile('MEH', { money: 1 }),
    [at('ZED')]: profile('ZED', { money: 1 }) } });
  const a = browser(srv, 'A');
  const phrase = await a.sync.create();
  await a.sync.markFolder();
  assert.ok(a.sync.link.folder && await readBeside('A~folder') === a.sync.link.folder, 'the folder marked');
  // a profile the game deleted (its own Delete: the I/O worker's 'removed', shell.ts onRemoved): sent
  let rev = srv.revOf(phrase);
  await filesOf('A').remove(`${DIR}/ZED`);
  a.sync.removed();
  await a.sync.push();
  assert.notEqual(srv.revOf(phrase), rev, 'the deletion went out');
  assert.equal(a.sync.state.s, 'synced');
  // one gone without a word of it: asked once, and sent as the player keeps it
  rev = srv.revOf(phrase);
  await filesOf('A').remove(`${DIR}/MEH`);
  await a.sync.push();
  assert.deepEqual([srv.revOf(phrase), a.sync.state.why], [rev, 'fewer']);
  await a.sync.keepMine();
  assert.notEqual(srv.revOf(phrase), rev);
  assert.equal(a.sync.state.s, 'synced');
  // a rename on the Saves tab (VaultPanel.changedSaves): sent
  rev = srv.revOf(phrase);
  assert.deepEqual(await P.renameProfile(filesOf('A'), DIR, 'TOP', 'BOB'), { files: 1 });
  a.sync.intended();
  await a.sync.push();
  assert.notEqual(srv.revOf(phrase), rev, 'the rename went out');
  // the browser deletes the site's data (the folder and its mark); the game saves one profile into a new folder
  await filesOf('A').writeFile(at('MEH'), profile('MEH', { money: 2 }));
  await a.sync.push();
  rev = srv.revOf(phrase);
  await removeSaveDir('A'); await root.removeEntry('A~folder');
  await filesOf('A').writeFile(at('BOB'), profile('BOB', { money: 3 }));
  await a.sync.push();
  assert.equal(srv.revOf(phrase), rev, 'the one profile does not replace the cloud\'s two');
  assert.deepEqual([a.sync.state.s, a.sync.state.why], ['conflict', 'wiped']);
  assert.deepEqual(a.sync.state.cloud.profiles.map(x => x.name).sort(), ['BOB', 'MEH'], 'the question shows the cloud\'s side');
  // Use the cloud's: both back, the new mark taken
  await a.sync.takeTheirs();
  const left = await folderOf('A');
  assert.deepEqual(sorted([...left.keys()].filter(k => k.startsWith(DIR))), sorted([at('BOB'), at('MEH')]));
  assert.equal(a.sync.state.s, 'synced');
  await new Promise(r => setTimeout(r, 0));
  assert.equal(await readBeside('A~folder'), a.sync.link.folder);
});

test('the saves back after a folder deleted during a game (shell.ts saveGone): the folder marked again, sends go on', async () => {
  const srv = server();
  const start = { 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 1 }), [at('MEH')]: profile('MEH', { money: 1 }) };
  await seedOpfs({ A: start });
  const a = browser(srv, 'A');
  const phrase = await a.sync.create();
  await a.sync.markFolder();
  const rev = srv.revOf(phrase);
  // deleted during the game; the start's saves back (VaultPanel.refill without the cloud), the mark with them
  await removeSaveDir('A'); await root.removeEntry('A~folder');
  a.sync.hold(true);
  for (const [path, d] of Object.entries(start)) await filesOf('A').writeFile(path, typeof d === 'string' ? enc.encode(d) : d);
  await a.sync.keepLink();
  a.sync.hold(false);
  // the game's save on top
  await filesOf('A').writeFile(at('TOP'), profile('TOP', { money: 9 }));
  await a.sync.push();
  assert.notEqual(srv.revOf(phrase), rev);
  assert.equal(a.sync.state.s, 'synced');
  const sent = await unzip((await a.sync.peek(phrase)).zip);
  assert.deepEqual(sorted(sent.map(e => e.name).filter(n => n.startsWith(DIR))), sorted([at('MEH'), at('TOP')]));
  assert.equal(money(sent.find(e => e.name === at('TOP')).data), 9);
});

// ------------------------------------------------- second pass (34f8145a, 942c80d0) --

/** A and B linked to one passphrase, A's folder as given (B loads it); B's then changed by change() and sent */
async function twoBrowsers(aFiles, change) {
  const srv = server();
  await seedOpfs({ A: aFiles, B: {} });
  const a = browser(srv, 'A'), b = browser(srv, 'B');
  const phrase = await a.sync.create();
  await a.sync.markFolder();
  const got = await b.sync.peek(phrase);
  await restoreVault('B', await readVault(got.zip));
  await b.sync.adopt(phrase, got.remote);
  await b.sync.markFolder();
  await change(filesOf('B'), b);
  return { srv, a, b, phrase };
}

test('a version that removed a profile (the game\'s own Delete elsewhere) is taken by itself: no \'drops\'', async () => {
  const { a, b } = await twoBrowsers({ 'settings.ini': '# s\n', [at('TOP')]: profile('TOP'), [at('MEH')]: profile('MEH') }, async (fs, b) => {
    await fs.remove(`${DIR}/MEH`); b.sync.removed();
    await fs.writeFile(at('TOP'), profile('TOP', { money: 5 }));
    await b.sync.push();
  });
  assert.equal(b.sync.state.s, 'synced');
  await a.sync.pull();
  assert.equal(a.sync.state.s, 'synced');
  const left = await folderOf('A');
  assert.deepEqual([left.has(at('MEH')), money(left.get(at('TOP')))], [false, 5]);
});

test('leftOut is empty for every profile a restore takes in: edited, heap-named, Chinese NAME, case twins, an orphan folder beside', () => {
  const edited = Buffer.from(profile('ED')); edited[0xA16A] ^= 1;
  const files = [
    [at('ED'), new Uint8Array(edited)], [at(HEAP_DIR), profile(HEAP)], [`${DIR}/${HEAP_DIR}/GARAGE.DAT`, new Uint8Array([1])],
    [at(ZH_DIR), profile(ZH)], [at('abc'), profile('abc')], [at('ABC'), profile('ABC')], [`${DIR}/ads/GARAGE.DAT`, new Uint8Array([2])],
    [at(`${HEAP_DIR} DVD 1`), cover(1)], ['settings.ini', enc.encode('# s\n')],
  ].map(([path, data]) => ({ path, data }));
  assert.deepEqual(leftOut(files), []);
});

test('an automatic take that stops halfway leaves no profile with another version\'s GARAGE.DAT', async () => {
  const { a } = await twoBrowsers({ 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 1 }), [`${DIR}/TOP/GARAGE.DAT`]: new Uint8Array([1, 1]) },
    async (fs, b) => {
      await fs.writeFile(at('TOP'), profile('TOP', { money: 2 }));
      await fs.writeFile(`${DIR}/TOP/GARAGE.DAT`, new Uint8Array([2, 2]));
      await b.sync.push();
    });
  // A takes B's version by itself; its storage fills after the first write (GARAGE.DAT sorts before TOP)
  opfs.failAfter = 1;
  await a.sync.pull().catch(() => {});
  opfs.failAfter = Infinity;
  const left = await folderOf('A');
  const pair = [money(left.get(at('TOP'))), left.get(`${DIR}/TOP/GARAGE.DAT`)[0]];
  assert.ok(pair[0] === pair[1], `the profile and its GARAGE.DAT of one version: profile of ${pair[0]}, GARAGE.DAT of ${pair[1]}`);
});

test('Use the cloud\'s on a \'drops\' conflict keeps this browser\'s copy of the profile the cloud\'s cannot bring, and its covers', async () => {
  const { a } = await twoBrowsers({ 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 1 }), [at('XYZ')]: profile('XYZ', { money: 4242 }),
    [at('XYZ DVD 1')]: cover(3) }, async (fs, b) => {
    // B's XYZ becomes one this game cannot load (another version's, as a copy from elsewhere), its player keeps B's
    const old = Buffer.from(profile('XYZ')); old.writeUInt32LE(0x10C, 28);
    await fs.writeFile(at('XYZ'), new Uint8Array(old));
    await fs.writeFile(at('TOP'), profile('TOP', { money: 2 }));
    await b.sync.push();
    await b.sync.keepMine();
  });
  await a.sync.pull();
  assert.deepEqual([a.sync.state.s, a.sync.state.why], ['conflict', 'drops']);
  await a.sync.takeTheirs();
  const left = await folderOf('A');
  assert.equal(money(left.get(at('TOP'))), 2, 'the cloud\'s TOP came in');
  assert.equal(left.has(at('XYZ')) && money(left.get(at('XYZ'))), 4242, `XYZ's career stays here: ${JSON.stringify([...left.keys()])}`);
  assert.ok(left.has(at('XYZ DVD 1')), 'and its cover');
});

test('a mended profile\'s covers moved by a cloud take never overwrite a cover already here', async () => {
  await seedOpfs({ C: {
    'settings.ini': '# s\n', [at('ads')]: profile('ads'), [at('ads DVD 1')]: cover(1),
    [at(ZH_DIR)]: profile(ZH), [at(`${ZH_DIR} DVD 1`)]: cover(2),
  } });
  // a version (another browser's, or Go back) with the broken profile but not ads
  const r = await readVault(z({ 'settings.ini': '# s\n', [at(ZH_DIR)]: profile(ZH, { money: 9 }) }));
  assert.deepEqual(r.repaired, [{ name: ZH_DIR, to: 'ads' }]);
  await restoreVault('C', r);
  const covers = [...(await folderOf('C'))].filter(([p]) => P.coverOf(p.split('/')[1])).map(([p, d]) => `${p.split('/')[1]}:${d[40]}`);
  assert.deepEqual(sorted(covers.map(c => c.split(':')[1])), ['1', '2'], `both cover shoots are still somewhere: ${JSON.stringify(covers)}`);
});

test('a backup restore and a cloud take at once leave the saves of one of them whole', async () => {
  const backup = await readBackup(z({ 'registry.txt': 'r', 'settings.ini': '# b\n', [at('BAK1')]: profile('BAK1'), [at('BAK2')]: profile('BAK2') }));
  const cloud = await readVault(z({ 'settings.ini': '# c\n', [at('CLD1')]: profile('CLD1'), [at('CLD2')]: profile('CLD2') }));
  await seedOpfs({ T: { 'settings.ini': '# t\n', [at('OLD')]: profile('OLD') } });
  // the Saves tab's restore confirmed while an automatic take (VaultSync.pull at the page's start) writes: nothing
  // makes one wait for the other (SavesPanel.busy, VaultPanel.restore)
  const ends = (await Promise.allSettled([restoreSaves('T', backup), restoreVault('T', cloud)])).map(x => x.status === 'rejected' ? String(x.reason?.name ?? x.reason) : 'done');
  const ps = (await listProfiles('T')).map(p => p.name);
  const whole = n => ['1', '2'].every(i => ps.includes(n + i));
  assert.ok(whole('BAK') || whole('CLD'), `restore ${ends[0]}, take ${ends[1]}; profiles left: ${JSON.stringify(ps)}`);
});

// (Go back to a copy with fewer profiles: savegone.test.mjs, through VaultPanel.goBack)

test('a version from an older page (no GUARDED note) that lacks a profile here is asked, never taken by itself', async () => {
  const srv = server();
  await seedOpfs({ A: { 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 1 }), [at('MEH')]: profile('MEH', { money: 1 }) } });
  const a = browser(srv, 'A');
  const phrase = await a.sync.create();
  // another device's cached page of before this note sends TOP alone (its folder deleted, a new career)
  const k = await V.keysOf(phrase), cur = [...srv.kept.values()][0].cur;
  const thin = await V.seal(k.key, await V.pack([{ path: 'settings.ini', data: enc.encode('# s\n') }, { path: at('TOP'), data: profile('TOP', { money: 0 }) }]));
  const put = await srv.fetch('/vault', { method: 'PUT', headers: { authorization: `Bearer ${k.token}`, 'if-match': `"${cur.rev.toString(16).padStart(16, '0')}"` }, body: thin });
  assert.equal(put.status, 200);
  await a.sync.pull();
  assert.deepEqual([a.sync.state.s, a.sync.state.why], ['conflict', 'thin']);
  const left = await folderOf('A');
  assert.deepEqual([money(left.get(at('TOP'))), left.has(at('MEH'))], [1, true], 'nothing here replaced');
  // the same from this page (its note on it) is taken: a profile deleted on purpose there
  await seedOpfs({ A2: { 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 1 }), [at('MEH')]: profile('MEH', { money: 1 }) }, B2: {} });
  const a2 = browser(srv, 'A2'), b2 = browser(srv, 'B2');
  const p2 = await a2.sync.create();
  const got = await b2.sync.peek(p2);
  await restoreVault('B2', await readVault(got.zip)); await b2.sync.adopt(p2, got.remote);
  await filesOf('B2').remove(`${DIR}/MEH`); b2.sync.removed();   // the game's own Delete there
  await b2.sync.push();
  await a2.sync.pull();
  assert.equal(a2.sync.state.s, 'synced');
  assert.equal((await folderOf('A2')).has(at('MEH')), false);
});

test('an automatic take decided before a Saves-tab restore wrote is not written over it: the restore stays, the next push asks', async () => {
  const srv = server();
  await seedOpfs({ A: { 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 1 }) }, B: {} });
  const a = browser(srv, 'A'), b = browser(srv, 'B');
  const phrase = await a.sync.create();
  const got = await b.sync.peek(phrase);
  await restoreVault('B', await readVault(got.zip)); await b.sync.adopt(phrase, got.remote);
  await filesOf('A').writeFile(at('TOP'), profile('TOP', { money: 2 }));
  await a.sync.push();
  // B's pull reads its saves (unchanged since it synced: A's version is taken), and the player's backup restore is
  // confirmed just then: it writes first
  const backup = await readBackup(z({ 'settings.ini': '# b\n', [at('BAK')]: profile('BAK', { money: 9 }) }));
  const files = b.deps.files;
  let restoring = null;
  b.deps.files = async o => { const f = await files(o); if (!restoring) restoring = restoreSaves('B', backup); return f; };
  await b.sync.pull();
  await restoring;
  b.deps.files = files;
  const left = await folderOf('B');
  assert.deepEqual([money(left.get(at('BAK'))), left.has(at('TOP'))], [9, false], 'the backup the player restored stays');
  assert.equal(b.taken, 0, 'nothing taken');
  b.sync.intended();                                             // (VaultPanel.changedSaves after the restore)
  await b.sync.push();
  assert.deepEqual([b.sync.state.s, b.sync.state.why], ['conflict', 'both'], 'asked: the cloud has A\'s newer version');
});

test('profiles deleted on purpose are kept in the link: a send that failed and a reload ask nothing', async () => {
  const srv = server();
  await seedOpfs({ A: { 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 1 }), [at('MEH')]: profile('MEH', { money: 1 }) } });
  const a = browser(srv, 'A');
  const phrase = await a.sync.create();
  const rev = srv.revOf(phrase);
  await filesOf('A').remove(`${DIR}/MEH`);
  a.sync.removed();                                              // the game's own Delete (shell.ts onRemoved)
  const fetch = srv.fetch;
  a.deps.fetch = async () => { throw new TypeError('offline'); };
  await a.sync.push().catch(() => {});
  assert.equal(srv.revOf(phrase), rev, 'nothing sent (offline)');
  // the page reloads: a new VaultSync on the link it kept
  a.deps.fetch = fetch;
  const again = new V.VaultSync(a.deps);
  assert.equal(again.link.dropped, 1);
  await again.push();
  assert.equal(again.state.s, 'synced', `${again.state.s} ${again.state.why ?? ''}`);
  assert.notEqual(srv.revOf(phrase), rev, 'the deletion went out');
  assert.equal(again.link.dropped, undefined, 'and the count starts again');
  // a profile emptied after that (a failed write) still asks
  await filesOf('A').writeFile(at('TOP'), new Uint8Array(0));
  await again.push();
  assert.deepEqual([again.state.s, again.state.why], ['conflict', 'fewer']);
});

// ------------------------------------------------------------- third pass (f4e3f301) --

/** the handle of a file of a save folder (the mock's), to hold it open as a worker would */
async function handleOf(dir, path) {
  let d = await root.getDirectoryHandle(dir);
  const p = path.split('/');
  for (const x of p.slice(0, -1)) d = await d.getDirectoryHandle(x);
  return d.getFileHandle(p[p.length - 1]);
}

test('a take whose staging stops (no room) changes nothing and leaves nothing staged, with move() as the browsers have it', async () => {
  await seedOpfs({ S: { 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 1 }), [`${DIR}/TOP/GARAGE.DAT`]: new Uint8Array([1]) } });
  const before = await folderOf('S');
  const r = await readVault(z({ 'settings.ini': '# c\n', [at('TOP')]: profile('TOP', { money: 2 }), [`${DIR}/TOP/GARAGE.DAT`]: new Uint8Array([2]), [at('NEW')]: profile('NEW') }));
  opfs.failAfter = 2;
  await assert.rejects(restoreVault('S', r), /QuotaExceeded/);
  opfs.failAfter = Infinity;
  const after = await folderOf('S');
  assert.deepEqual(sorted(after.keys()), sorted(before.keys()), 'no file more, none less (no .incoming, no NEW folder)');
  for (const [k, v] of before) assert.deepEqual(after.get(k), v, k);
});

test('files a restore staged and never moved (the tab closed while it wrote) are never sent to the cloud', async () => {
  // a backup restore with screenshots stages for seconds (up to 512 MB): closed then, its staged files stay
  const shot = new Uint8Array(1 << 20); for (let i = 0; i < shot.length; i++) shot[i] = (i * 2654435761) >>> 24;
  await seedOpfs({ L: { 'settings.ini': '# s\n', [at('TOP')]: profile('TOP'), 'nfsu2000.png.incoming': shot,
    [`${DIR}/TOP/TOP.incoming`]: profile('TOP', { money: 9 }) } });
  const a = browser(server(), 'L');
  const sent = await a.deps.files();
  assert.deepEqual(sent.map(f => f.path).filter(x => x.endsWith('.incoming')), [], 'no staged file among the files a passphrase keeps');
  await a.sync.create();                                          // (a 1 MB staged screenshot: VAULT-TOO-LARGE, at every send)
  assert.ok(a.sync.link, `linked: ${a.sync.state.s} ${a.sync.state.code ?? ''}`);
});

test('a move that is refused halfway (a file held open) leaves the saves as they were', async () => {
  await seedOpfs({ M: { 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 1 }), [`${DIR}/TOP/GARAGE.DAT`]: new Uint8Array([1]) } });
  // (in the order a sender's listFiles has them: GARAGE.DAT before TOP)
  const r = await readVault(z({ [`${DIR}/TOP/GARAGE.DAT`]: new Uint8Array([2]), [at('TOP')]: profile('TOP', { money: 2 }), 'settings.ini': '# c\n' }));
  opfs.held.add(await handleOf('M', at('TOP')));                 // GARAGE.DAT moves, TOP is refused
  await restoreVault('M', r).catch(() => {});
  opfs.held.clear();
  const left = await folderOf('M');
  assert.deepEqual([money(left.get(at('TOP'))), left.get(`${DIR}/TOP/GARAGE.DAT`)[0]], [1, 1], 'the profile and its GARAGE.DAT as they were');
  assert.deepEqual([...left.keys()].filter(k => k.endsWith('.incoming')), [], 'nothing staged left');
});

test('an older page\'s version of a profile this browser mended (its folder still broken there) is no \'thin\' one', async () => {
  const srv = server();
  // this browser took the profile mended (ads); the other device, an older page, still has it under its broken folder
  await seedOpfs({ A: { 'settings.ini': '# s\n', [at('ads')]: profile('ads', { money: 1 }) } });
  const a = browser(srv, 'A');
  const phrase = await a.sync.create();
  const k = await V.keysOf(phrase), cur = [...srv.kept.values()][0].cur;
  const old = await V.seal(k.key, await V.pack([{ path: 'settings.ini', data: enc.encode('# s\n') }, { path: at(ZH_DIR), data: profile(ZH, { money: 50 }) }]));
  const put = await srv.fetch('/vault', { method: 'PUT', headers: { authorization: `Bearer ${k.token}`, 'if-match': `"${cur.rev.toString(16).padStart(16, '0')}"` }, body: old });
  assert.equal(put.status, 200);
  await a.sync.pull();
  assert.equal(a.sync.state.s, 'synced', `${a.sync.state.s} ${a.sync.state.why ?? ''}`);
  assert.equal(money((await folderOf('A')).get(at('ads'))), 50, 'the newer career came in, mended again');
});

test('a mark that cannot be written (storage full) is said in the log, and the player is not asked \'wiped\' at every send', async () => {
  const srv = server();
  await seedOpfs({ U: { 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 1 }), [at('MEH')]: profile('MEH', { money: 1 }) } });
  const u = browser(srv, 'U'), logs = [];
  u.deps.log = line => logs.push(line);
  await u.sync.create();
  assert.ok(await readBeside('U~folder'), 'marked');
  // the browser deletes the folder and its mark; the game saves both profiles again
  await removeSaveDir('U'); await root.removeEntry('U~folder');
  await filesOf('U').writeFile(at('TOP'), profile('TOP', { money: 2 })); await filesOf('U').writeFile(at('MEH'), profile('MEH', { money: 2 }));
  await u.sync.push();
  assert.equal(u.sync.state.why, 'wiped');
  // Keep this browser's, and the mark cannot be written
  const mark = u.deps.mark;
  u.deps.mark = async () => { throw err('QuotaExceededError'); };
  await u.sync.keepMine();
  assert.equal(u.sync.state.s, 'synced');
  assert.ok(logs.some(l => /mark could not be written/.test(l)), 'said in the log');
  assert.equal(u.sync.link.unmarked, true);
  const rev = srv.revOf();
  await filesOf('U').writeFile(at('TOP'), profile('TOP', { money: 3 }));
  await u.sync.push();
  assert.equal(u.sync.state.s, 'synced', `sent, not asked again: ${u.sync.state.s} ${u.sync.state.why ?? ''}`);
  assert.notEqual(srv.revOf(), rev);
  // a profile lost still asks (by its count)
  await filesOf('U').writeFile(at('MEH'), new Uint8Array(0));
  await u.sync.push();
  assert.deepEqual([u.sync.state.s, u.sync.state.why], ['conflict', 'fewer']);
  // the mark written again at the next marking (keepLink): the link's own
  u.deps.mark = mark;
  await u.sync.keepLink();
  assert.equal(await readBeside('U~folder'), u.sync.link.folder);
  assert.equal(u.sync.link.unmarked, undefined);
});

test('a restore whose moves a closed tab left halfway is undone at the next page\'s start (settleStaged)', async () => {
  await seedOpfs({ H: { 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 1 }), [`${DIR}/TOP/GARAGE.DAT`]: new Uint8Array([1]) } });
  const r = await readVault(z({ [`${DIR}/TOP/GARAGE.DAT`]: new Uint8Array([2]), [at('TOP')]: profile('TOP', { money: 2 }), [at('NEW')]: profile('NEW') }));
  // the tab "closes" at the third move: the move throws, and the undo itself is not let run (as a closed tab's)
  let moves = 0;
  const move = FileH.prototype.move;
  FileH.prototype.move = async function (...a) { if (++moves === 3) { FileH.prototype.move = async () => { throw err('AbortError'); }; throw err('AbortError'); } return move.apply(this, a); };
  await restoreVault('H', r).catch(() => {});
  FileH.prototype.move = move;
  const mid = await folderOf('H'), was = [at('TOP'), `${DIR}/TOP/GARAGE.DAT`].map(k => mid.get(k)?.length === 1 ? mid.get(k)[0] : mid.get(k) ? money(mid.get(k)) : null);
  assert.notDeepEqual(was, [1, 1], `(left halfway: ${JSON.stringify(was)})`);
  await settleStaged('H');
  const left = await folderOf('H');
  assert.deepEqual([money(left.get(at('TOP'))), left.get(`${DIR}/TOP/GARAGE.DAT`)[0], left.has(at('NEW'))], [1, 1, false], 'as before the restore');
  assert.equal(await root.getDirectoryHandle('H~incoming').then(() => true, () => false), false, 'nothing staged left');
});

// ------------------------------------------------------------ fourth pass (cae4d8df) --

test('an undo cut short and run again never removes a file it had already put back', async () => {
  const files = { 'settings.ini': '# s\n', [at('AAA')]: profile('AAA', { money: 1 }), [at('BBB')]: profile('BBB', { money: 1 }), [at('CCC')]: profile('CCC', { money: 1 }) };
  await seedOpfs({ D: files });
  const r = await readVault(z({ [at('AAA')]: profile('AAA', { money: 2 }), [at('BBB')]: profile('BBB', { money: 2 }), [at('CCC')]: profile('CCC', { money: 2 }), 'settings.ini': '# c\n' }));
  // the tab closes once every file is moved in, before the plan goes: the next page's start undoes it
  const remove = DirH.prototype.removeEntry, move = FileH.prototype.move;
  DirH.prototype.removeEntry = async function (n, o) { if (n === 'plan.json') throw err('AbortError'); return remove.call(this, n, o); };
  await restoreVault('D', r).catch(() => {});
  DirH.prototype.removeEntry = remove;
  // that undo is cut short too (the page closed again as it ran, or a step refused): BBB's original does not come back
  FileH.prototype.move = async function (...a) { if (this.name === 'o1') throw err('AbortError'); return move.apply(this, a); };
  await settleStaged('D').catch(() => {});
  FileH.prototype.move = move;
  // the page after it undoes the rest
  await settleStaged('D');
  const left = await folderOf('D');
  const got = ['AAA', 'BBB', 'CCC'].map(n => left.has(at(n)) ? money(left.get(at(n))) : null);
  assert.deepEqual([got, left.has('settings.ini') && dec.decode(left.get('settings.ini'))], [[1, 1, 1], '# s\n'], `as before the restore: ${JSON.stringify(got)}`);
});

test('an undo that stops keeps its plan: no restore writes until it is undone, and a game save made since at one of its paths stays', async () => {
  await seedOpfs({ K: { 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 1 }), [at('MEH')]: profile('MEH', { money: 1 }) } });
  const r = await readVault(z({ [at('MEH')]: profile('MEH', { money: 2 }), [at('TOP')]: profile('TOP', { money: 2 }), 'settings.ini': '# c\n' }));
  // the last file's move refused (settings.ini held open): the undo runs, and MEH's original is refused its way back
  const move = FileH.prototype.move;
  let refuseMeh = true;
  FileH.prototype.move = async function (a, b) {
    if (refuseMeh && /^o\d+$/.test(this.name) && (typeof a === 'string' ? a : b) === 'MEH') throw err('NoModificationAllowedError');
    return move.call(this, a, b);
  };
  opfs.held.add(await handleOf('K', 'settings.ini'));
  await assert.rejects(restoreVault('K', r));
  opfs.held.clear();
  assert.equal((await folderOf('K')).has(at('MEH')), false, '(MEH not back yet)');
  // the plan stays: no restore writes meanwhile
  await assert.rejects(restoreVault('K', r), /not undone yet/);
  // the game saves MEH meanwhile (a new career under that name): the next undo leaves it
  await filesOf('K').writeFile(at('MEH'), profile('MEH', { money: 9 }));
  refuseMeh = false; FileH.prototype.move = move;
  assert.equal(await settleStaged('K'), true);
  const left = await folderOf('K');
  assert.deepEqual([money(left.get(at('TOP'))), money(left.get(at('MEH'))), dec.decode(left.get('settings.ini'))], [1, 9, '# s\n']);
  assert.equal(await root.getDirectoryHandle('K~incoming').then(() => true, () => false), false, 'nothing staged left');
});

test('a mark that could not be written is written at the next send', async () => {
  const srv = server();
  await seedOpfs({ W: { 'settings.ini': '# s\n', [at('TOP')]: profile('TOP', { money: 1 }) } });
  const w = browser(srv, 'W'), mark = w.deps.mark;
  w.deps.mark = async () => { throw err('QuotaExceededError'); };
  await w.sync.create();
  assert.equal(w.sync.link.unmarked, true);
  w.deps.mark = mark;
  await filesOf('W').writeFile(at('TOP'), profile('TOP', { money: 2 }));
  await w.sync.push();
  assert.equal(w.sync.state.s, 'synced');
  assert.equal(w.sync.link.unmarked, undefined);
  assert.equal(await readBeside('W~folder'), w.sync.link.folder);
});

// ------------------------------------------------------------- fifth pass (3dbf65a0) --

test('a restore cut at every move, and its undo cut at every step any number of times, ends as before the restore', async () => {
  const before = { 'settings.ini': '# s\n', [at('AAA')]: profile('AAA', { money: 1 }), [`${DIR}/AAA/GARAGE.DAT`]: new Uint8Array([1]),
    [at('BBB')]: profile('BBB', { money: 1 }) };
  const version = z({ [`${DIR}/AAA/GARAGE.DAT`]: new Uint8Array([2]), [at('AAA')]: profile('AAA', { money: 2 }), [at('BBB')]: profile('BBB', { money: 2 }),
    [at('CCC')]: profile('CCC', { money: 2 }), 'settings.ini': '# c\n' });
  const move = FileH.prototype.move, remove = DirH.prototype.removeEntry;
  /** moves let through before the page "closes" (every move after it throws, as no code runs any more) */
  const cutAfter = n => { let k = 0; FileH.prototype.move = async function (...a) { if (++k > n) throw err('AbortError'); return move.apply(this, a); }; };
  const whole = () => { FileH.prototype.move = move; DirH.prototype.removeEntry = remove; };
  const state = async () => { const f = await folderOf('E'); return JSON.stringify(Object.keys(before).map(k => f.has(k) ? (k.endsWith('.DAT') || k.endsWith('.ini') ? [...f.get(k)].join() : money(f.get(k))) : null).concat([f.has(at('CCC'))])); };
  await seedOpfs({ E: before });
  const want = await state();
  const bad = [];
  // 5 files: up to 9 moves (4 aside, 5 in); 'plan': every move done, the page closed before the plan went
  for (const m of [0, 1, 2, 3, 4, 5, 6, 7, 8, 'plan']) {
    for (const cuts of [[0], [1], [2], [3], [5], [1, 1], [2, 0, 3], [1, 2, 1, 1]]) {
      await seedOpfs({ E: before });
      const r = await readVault(version);
      if (m === 'plan') DirH.prototype.removeEntry = async function (n, o) { if (n === 'plan.json') throw err('AbortError'); return remove.call(this, n, o); };
      else cutAfter(m);
      await restoreVault('E', r).catch(() => {});
      whole();
      for (const c of cuts) { cutAfter(c); await settleStaged('E').catch(() => {}); whole(); }
      const done = await settleStaged('E');
      const got = await state(), staged = await root.getDirectoryHandle('E~incoming').then(() => true, () => false);
      if (got !== want || !done || staged) bad.push(`restore cut after ${m}, undos cut after ${cuts}: ${got}${done ? '' : ' (not undone)'}${staged ? ' (staging left)' : ''}`);
    }
  }
  whole();
  assert.deepEqual(bad, []);
});

// ------------------------------------------------------------ seventh pass (429555ea) --

test('a restore with the Put back copy beside it, cut at every move and its undo cut any number of times, leaves saves and copy as they were', async () => {
  const save = { 'settings.ini': '# s\n', [at('AAA')]: profile('AAA', { money: 1 }), [`${DIR}/AAA/GARAGE.DAT`]: new Uint8Array([1]) };
  const incoming = [{ path: `${DIR}/AAA/GARAGE.DAT`, data: new Uint8Array([2]) }, { path: at('AAA'), data: profile('AAA', { money: 2 }) },
    { path: at('BBB'), data: profile('BBB', { money: 2 }) }, { path: 'settings.ini', data: '# c\n' }];
  const beside = [{ dir: 'E~replaced', path: 'saves.zip', data: 'new copy' }, { dir: 'E~replaced', path: 'about.json', data: 'new about' }];
  const move = FileH.prototype.move, remove = DirH.prototype.removeEntry;
  const cutAfter = n => { let k = 0; FileH.prototype.move = async function (...a) { if (++k > n) throw err('AbortError'); return move.apply(this, a); }; };
  const whole = () => { FileH.prototype.move = move; DirH.prototype.removeEntry = remove; };
  const state = async () => {
    const f = await folderOf('E'), d = await root.getDirectoryHandle('E~replaced').catch(() => null);
    const saves = [...f.keys()].sort().map(k => `${k}=${f.get(k).length === 54966 ? money(f.get(k)) : dec.decode(f.get(k))}`).join(' ');
    const copy = !d ? 'no copy' : ['saves.zip', 'about.json'].map(n => d.kids.has(n) ? dec.decode(d.kids.get(n).data) : '-').join('|');
    return `${saves} || ${copy}`;
  };
  const bad = [];
  for (const had of [false, true]) {                          // a copy kept there before, or no ~replaced folder yet
    const seed = async () => {
      await seedOpfs({ E: save });
      if (had) { const d = await root.getDirectoryHandle('E~replaced', { create: true }); for (const n of ['saves.zip', 'about.json']) (await d.getFileHandle(n, { create: true })).data = enc.encode(`old ${n}`); }
    };
    await seed();
    const want = await state();
    for (const m of [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 'plan']) {
      for (const cuts of [[0], [1], [3], [1, 2], [2, 0, 4]]) {
        await seed();
        if (m === 'plan') DirH.prototype.removeEntry = async function (n, o) { if (n === 'plan.json') throw err('AbortError'); return remove.call(this, n, o); };
        else cutAfter(m);
        const finished = await writeTogether('E', incoming, beside).then(() => true, () => false);
        whole();
        for (const c of cuts) { cutAfter(c); await settleStaged('E').catch(() => {}); whole(); }
        const done = await settleStaged('E'), got = await state();
        // a write that finished: the new saves with the new copy; one that did not: the saves and the copy as before
        const wantNow = finished ? 'new copy|new about' : want;
        if (finished ? !got.endsWith(wantNow) : got.replace(/ \|\| no copy$/, ' || -|-') !== want.replace(/ \|\| no copy$/, ' || -|-'))
          bad.push(`copy ${had ? 'kept before' : 'none before'}, cut after ${m}, undos cut after ${cuts}: ${got}`);
        if (!done) bad.push(`not undone: cut after ${m}, ${cuts}`);
      }
    }
  }
  whole();
  assert.deepEqual(bad, []);
});
