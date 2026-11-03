// Profile names of up to 15 with spaces, - and _ (runtime/profilename.c, profile.ts isProfileName) between browsers
// with cloud saves, on an OPFS in memory and a /vault of its own (after the review of 04b54184):
//   node --test web/test/profile-names.test.mjs
// - this page keeps such a name as it is on every way saves come in (a backup, a keyphrase's version, a refill after
//   the browser deleted the save folder): never mended, never written twice
// - a page of before 2026-11 in another browser renamed ("mended") such a name to its letters and digits, 7 of them,
//   and sent it: this browser takes the rename, and its own DVD and magazine covers of the profile go with it
//   (saves-ui.ts restoreVault, renamedHere) instead of going; the same for any rename made in another browser
// - a profile deleted in another browser takes its covers here with it, as before
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * as P from './web/src/app/profile.ts'; export * as V from './web/src/app/vault.ts';
  export { readBackup, readVault, restoreVault, leftOut, profilesComing, refillSaves } from './web/src/app/saves-ui.ts';
  export { listFiles, filesOf, readBytes, readBeside, writeBeside } from './web/src/app/savefs.ts';
  export { zip } from './web/src/app/zip.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;

// ---- an OPFS in memory (as saves-review.test.mjs has it) ----
const enc = new TextEncoder(), dec = new TextDecoder();
const opfs = { clock: 1_700_000_000_000 };
const err = name => new DOMException(name, name);
class FileH {
  constructor(name, parent) { this.kind = 'file'; this.name = name; this.parent = parent; this.data = new Uint8Array(0); this.mtime = opfs.clock; }
  async move(a, b) {
    const dest = typeof a === 'string' ? this.parent : a, name = typeof a === 'string' ? a : b, there = dest.kids.get(name);
    if (there && there.kind !== 'file') throw err('TypeMismatchError');
    this.parent.kids.delete(this.name); this.name = name; this.parent = dest; dest.kids.set(name, this);
  }
  async getFile() { const d = this.data, m = this.mtime; return { size: d.length, lastModified: m, arrayBuffer: async () => d.slice().buffer, text: async () => dec.decode(d) }; }
  async createWritable() {
    const parts = [];
    return {
      write: async x => { parts.push(typeof x === 'string' ? enc.encode(x) : x instanceof Blob ? new Uint8Array(await x.arrayBuffer()) : new Uint8Array(x.buffer ? x.buffer.slice(x.byteOffset, x.byteOffset + x.byteLength) : x)); },
      close: async () => { this.data = new Uint8Array(Buffer.concat(parts.map(p => Buffer.from(p)))); this.mtime = opfs.clock += 1000; },
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
    if (k.kind === 'directory' && k.kids.size && !recursive) throw err('InvalidModificationError');
    this.kids.delete(n);
  }
  async *entries() { for (const e of [...this.kids]) yield e; }
}
let root = new DirH('');
const storage = { getDirectory: async () => root };
if (globalThis.navigator) Object.defineProperty(globalThis.navigator, 'storage', { value: storage, configurable: true });
else Object.defineProperty(globalThis, 'navigator', { value: { storage }, configurable: true });

const M = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
const { P, V } = M;
const DIR = 'NFS Underground 2', B = 36, NAME = B + 53761;
const at = n => `${DIR}/${n}/${n}`;
const sorted = a => [...a].sort();
async function seed(dirs) {
  root = new DirH('');
  for (const [dir, files] of Object.entries(dirs)) for (const [p, d] of Object.entries(files)) await M.filesOf(dir).writeFile(p, typeof d === 'string' ? enc.encode(d) : d);
}
async function folderOf(dir) {
  const out = new Map();
  for (const f of await M.listFiles(dir)) out.set(f.path, await M.readBytes(dir, f.path));
  return out;
}
function seal(f) {
  f.writeUInt32LE(P.checksum(f.subarray(28, 36)), 16); f.writeUInt32LE(P.checksum(f.subarray(36)), 20); f.writeUInt32LE(P.checksum(f.subarray(0, 24)), 24);
  return new Uint8Array(f.buffer, f.byteOffset, f.length);
}
function profile(inside, { money = 1000, career = 1 } = {}) {
  const f = Buffer.alloc(54966);
  f.write('20CM', 0, 'latin1');
  f.writeUInt32LE(54966, 4); f.writeUInt32LE(8, 8); f.writeUInt32LE(54930, 12);
  f.writeUInt32LE(0x10D, 28); f.writeUInt32LE(54930, 32);
  for (let i = 0; i < 48; i++) f.writeUInt32LE(i < 31 ? i : 0xFFFFFFFF, B + i * 24 + 20);
  f.writeUInt32LE(31, B + 1152);
  f[B + 30000] = career;                                       // one career from another
  f.write(inside, NAME, 'latin1');
  f.writeUInt32LE(money, 0xA16A);
  return seal(f);
}
/** a profile file played on: its bank changed */
const played = (file, money) => { const f = Buffer.from(file); f.writeUInt32LE(money, 0xA16A); return seal(f); };
const cover = n => { const d = new Uint8Array(1048612); d.set(enc.encode('20CM')); d[40] = n; return d; };
const money = f => f ? new DataView(f.buffer, f.byteOffset, f.byteLength).getUint32(0xA16A, true) : null;
const nameOf = f => { let n = ''; for (let i = NAME; i < NAME + 32 && f[i]; i++) n += String.fromCharCode(f[i]); return n; };
const profilesOf = m => sorted([...m.keys()].filter(p => { const s = p.split('/'); return s.length === 3 && s[0] === DIR && s[1].toLowerCase() === s[2].toLowerCase() && !P.coverOf(s[1]); }).map(p => p.split('/')[1]));
const coversOf = m => sorted([...m.keys()].filter(p => P.coverOf(p.split('/')[1] ?? '')).map(p => p.split('/')[1]));

/** a /vault of its own: one keyphrase file per token, If-Match / If-None-Match as the server has them */
function server() {
  const kept = new Map(), s = { now: Date.parse('2026-11-03T12:00:00Z'), kept };
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
  return s;
}
/** a browser on save folder dir: vault-ui.ts's files() and restore() */
function browser(srv, dir) {
  const b = { link: null, taken: 0 };
  const deps = {
    files: async (o = {}) => {
      const want = o.was ? V.keptBy(o.was) : V.keeps, out = [];
      for (const f of await M.listFiles(dir)) if (want(f.path)) out.push({ path: f.path, data: await M.readBytes(dir, f.path), mtime: f.mtime });
      return V.kept(out, o.was);
    },
    restore: async (zipped, still) => M.restoreVault(dir, await M.readVault(zipped), still),
    load: () => b.link && JSON.parse(b.link), save: l => { b.link = l && JSON.stringify(l); },
    fetch: srv.fetch, log: () => {},
    leftOut: files => M.leftOut(files), named: files => M.profilesComing(files),
    folder: () => M.readBeside(`${dir}~folder`), mark: id => M.writeBeside(`${dir}~folder`, id),
  };
  b.sync = new V.VaultSync(deps);
  b.sync.onTaken = () => b.taken++;
  return b;
}
/** A makes a keyphrase of its saves, B loads it (as vault-ui.ts's "Use this keyphrase") */
async function pair(srv) {
  const a = browser(srv, 'A'), b = browser(srv, 'B');
  const phrase = await a.sync.create();
  const got = await b.sync.peek(phrase);
  await M.restoreVault('B', await M.readVault(got.zip));
  await b.sync.adopt(phrase, got.remote);
  return { a, b };
}

// ---------------------------------------------------------------------------------------------------------------

test('a name of up to 15 with spaces comes in as it is from a keyphrase\'s version, never mended; a backup\'s zip mends it, as the game\'s box makes none yet', async () => {
  const files = { 'registry.txt': 'x', 'settings.ini': '# s\n', [at('Big Bad Wolf')]: profile('Big Bad Wolf', { money: 1 }),
    [at('Zed_the-15chars')]: profile('Zed_the-15chars', { money: 2, career: 2 }), [`${DIR}/Big Bad Wolf/GARAGE.DAT`]: 'g',
    [at('Big Bad Wolf DVD 1')]: cover(1) };
  const zipped = M.zip(Object.entries(files).map(([name, data]) => ({ name, data: typeof data === 'string' ? enc.encode(data) : data })));
  const r = await M.readVault(zipped);
  assert.deepEqual([sorted(r.profiles), r.repaired, r.refused], [['Big Bad Wolf', 'Zed_the-15chars'], [], []]);
  assert.equal(nameOf(r.entries.find(e => e.path === at('Big Bad Wolf')).data), 'Big Bad Wolf');
  const local = await M.readBackup(zipped);
  assert.deepEqual([sorted(local.profiles), local.refused], [['BigBadW', 'Zedthe1'], []], 'a backup\'s zip: the names the game lists');
  assert.ok(local.entries.some(e => e.path === at('BigBadW DVD 1')), 'its cover with it');
  const pc = await M.readBackup(M.zip([{ name: `${DIR}/Big Bad Wolf/Big Bad Wolf`, data: profile('Big Bad Wolf') }]), 'x.zip');
  assert.deepEqual(pc.profiles, ['BigBadW'], 'the PC game\'s profiles too');
  assert.deepEqual(M.leftOut(V.kept(Object.entries(files).map(([path, data]) => ({ path, data: typeof data === 'string' ? enc.encode(data) : data })))), []);
});

test('a page of before 2026-11 mended a long name in another browser and sent it: the career and this browser\'s covers come through', async () => {
  const srv = server(), LONG = 'Big Bad Wolf';
  await seed({ A: { 'settings.ini': '# s\n', [at(LONG)]: profile(LONG, { money: 500000 }), [`${DIR}/${LONG}/GARAGE.DAT`]: new Uint8Array([7, 7]),
    [at(`${LONG} DVD 1`)]: cover(1), [at(`${LONG} Magazine 2`)]: cover(2) }, B: {} });
  const { a, b } = await pair(srv);
  // B's older page took it as "BigBadW" (its fixedName), its game played it, it sent
  const inB = await folderOf('B');
  assert.deepEqual(profilesOf(inB), [LONG], 'this page took it as it is');
  const fs = M.filesOf('B');
  await fs.writeFile(at('BigBadW'), played(P.withName(inB.get(at(LONG)), 'BigBadW'), 600000));
  await fs.writeFile(`${DIR}/BigBadW/GARAGE.DAT`, new Uint8Array([7, 7]));
  await fs.remove(`${DIR}/${LONG}`);
  await b.sync.push();
  await a.sync.pull();
  const inA = await folderOf('A');
  assert.deepEqual(profilesOf(inA), ['BigBadW'], 'A takes the rename');
  assert.equal(money(inA.get(at('BigBadW'))), 600000, 'the career B played');
  assert.deepEqual(coversOf(inA), ['BigBadW DVD 1', 'BigBadW Magazine 2'], 'A\'s covers went with it');
  assert.equal(inA.get(at('BigBadW DVD 1'))[40], 1);
  assert.equal(inA.get(at('BigBadW Magazine 2'))[40], 2);
  assert.ok(![...inA.keys()].some(k => k.includes(LONG)), 'nothing left under the old name');
});

test('a rename in another browser takes this browser\'s own covers of the profile with it (they went before); played since there, they go as before', async () => {
  for (const played2 of [false, true]) {
    const srv = server();
    await seed({ A: { 'settings.ini': '# s\n', [at('TOBI')]: profile('TOBI', { money: 9 }), [at('OTHER')]: profile('OTHER', { career: 3 }) }, B: {} });
    const { a, b } = await pair(srv);
    await M.filesOf('B').writeFile(at('TOBI DVD 1'), cover(9));           // B took a cover shoot of its own
    await M.filesOf('B').writeFile(at('OTHER Magazine 1'), cover(8));
    assert.deepEqual(await P.renameProfile(M.filesOf('A'), DIR, 'TOBI', 'ROBIN'), { files: 1 });
    if (played2) await M.filesOf('A').writeFile(at('ROBIN'), played((await folderOf('A')).get(at('ROBIN')), 77));
    a.sync.intended(); await a.sync.push();
    await b.sync.pull();
    const inB = await folderOf('B');
    assert.deepEqual(profilesOf(inB), ['OTHER', 'ROBIN'], `played after: ${played2}`);
    // played since its rename there: its bytes are another career's as much as its own (careers share most bytes)
    assert.deepEqual(coversOf(inB), played2 ? ['OTHER Magazine 1'] : ['OTHER Magazine 1', 'ROBIN DVD 1'], `played after: ${played2}`);
    if (!played2) assert.equal(inB.get(at('ROBIN DVD 1'))[40], 9);
  }
});

test('covers never go to another career: one deleted there and a new one there; a keyphrase loaded here; two names an older page mends alike', async () => {
  // deleted in A, and A made a new profile: B's cover of the deleted one goes (not to the new one)
  {
    const srv = server();
    await seed({ A: { 'settings.ini': '# s\n', [at('XAVIER')]: profile('XAVIER', { money: 1 }) }, B: {} });
    const { a, b } = await pair(srv);
    await M.filesOf('B').writeFile(at('XAVIER DVD 1'), cover(11));
    await M.filesOf('A').remove(`${DIR}/XAVIER`); a.sync.removed();
    await M.filesOf('A').writeFile(at('NEWBIE'), profile('NEWBIE', { money: 50, career: 9 }));
    a.sync.intended(); await a.sync.push();
    await b.sync.pull();
    const inB = await folderOf('B');
    assert.deepEqual([profilesOf(inB), coversOf(inB)], [['NEWBIE'], []]);
  }
  // Load a keyphrase into a browser with a profile and a cover of its own: its cover stays its own (it goes)
  {
    const srv = server();
    await seed({ A: { 'settings.ini': '# s\n', [at('ALICE')]: profile('ALICE', { money: 1 }) },
      B: { 'settings.ini': '# b\n', [at('BOB')]: profile('BOB', { money: 2, career: 7 }), [at('BOB DVD 1')]: cover(22) } });
    const a = browser(srv, 'A');
    const phrase = await a.sync.create();
    const got = await browser(srv, 'B').sync.peek(phrase);
    await M.restoreVault('B', await M.readVault(got.zip));                     // vault-ui.ts Load: no `still`
    assert.deepEqual(coversOf(await folderOf('B')), []);
  }
  // two long names with the same first 7 letters and digits, both mended by an older page and played: which got
  // BigBadW is the older page's order, so their covers go rather than to the other career
  {
    const srv = server(), W1 = 'Big Bad Wolf', W2 = 'Big Bad Wolfie';
    await seed({ A: { 'settings.ini': '# s\n', [at(W1)]: profile(W1, { money: 100 }), [at(W2)]: profile(W2, { money: 200, career: 2 }) }, B: {} });
    const { a, b } = await pair(srv);
    await M.filesOf('B').writeFile(at(`${W1} DVD 1`), cover(1));
    await M.filesOf('B').writeFile(at(`${W2} DVD 1`), cover(2));
    const inA = await folderOf('A'), fs = M.filesOf('A');
    await fs.writeFile(at('BigBadW'), played(P.withName(inA.get(at(W2)), 'BigBadW'), 201));
    await fs.writeFile(at('BigBad2'), played(P.withName(inA.get(at(W1)), 'BigBad2'), 101));
    await fs.remove(`${DIR}/${W1}`); await fs.remove(`${DIR}/${W2}`);
    await a.sync.push();
    await b.sync.pull();
    const inB = await folderOf('B');
    assert.deepEqual([profilesOf(inB), coversOf(inB)], [['BigBad2', 'BigBadW'], []]);
  }
});

test('a profile deleted in another browser takes this browser\'s covers of it with it, as before; a rename beside it does not confuse them', async () => {
  const srv = server();
  await seed({ A: { 'settings.ini': '# s\n', [at('TOBI')]: profile('TOBI', { money: 9 }), [at('KEEP')]: profile('KEEP', { career: 4 }),
    [at('ANNA')]: profile('ANNA', { career: 5 }) }, B: {} });
  const { a, b } = await pair(srv);
  for (const [p, n] of [['TOBI DVD 1', 1], ['KEEP DVD 1', 2], ['ANNA DVD 1', 3]]) await M.filesOf('B').writeFile(at(p), cover(n));
  await M.filesOf('A').remove(`${DIR}/TOBI`);                      // deleted in A
  assert.deepEqual(await P.renameProfile(M.filesOf('A'), DIR, 'ANNA', 'HANNA'), { files: 1 });   // and another renamed
  a.sync.intended(); await a.sync.push();
  await b.sync.pull();
  const inB = await folderOf('B');
  assert.deepEqual(profilesOf(inB), ['HANNA', 'KEEP']);
  assert.deepEqual(coversOf(inB), ['HANNA DVD 1', 'KEEP DVD 1'], 'the deleted one\'s cover went, the renamed one\'s moved');
  assert.equal(inB.get(at('HANNA DVD 1'))[40], 3);
});

test('mendedBefore: what a page of before 2026-11 made of a name it did not take', () => {
  for (const [from, to] of [['Big Bad Wolf', 'BigBadW'], ['Big Bad Wolf', 'bigbadw'], ['Big Bad Wolf', 'BigBad2'], ['Big Bad Wolf', 'BigBa12'],
    ['Zoë-Racer 2', 'ZoeRace'], ['Dani-99', 'Dani99'], ['a b', 'ab'], ['a b', 'ab2']]) assert.ok(P.mendedBefore(from, to), `${from} -> ${to}`);
  for (const [from, to] of [['Big Bad Wolf', 'BigBadX'], ['TOBI', 'TOBI'], ['TOBI', 'TOBI2'], ['Big Bad Wolf', 'BigBadW Two'], ['Big Bad Wolf', '1234567'],
    ['Dani-99', 'Dani9'], ['a b', 'abc']]) assert.ok(!P.mendedBefore(from, to), `${from} -> ${to}`);
});

test('a refill after the browser deleted the save folder: a long name once, from the cloud or from the game\'s start', async () => {
  const LONG = 'Big Bad Wolf';
  const version = { 'settings.ini': '# cloud\n', [at(LONG)]: profile(LONG, { money: 3 }), [`${DIR}/${LONG}/GARAGE.DAT`]: 'g' };
  const zipped = M.zip(Object.entries(version).map(([name, data]) => ({ name, data: typeof data === 'string' ? enc.encode(data) : data })));
  const cloud = (await M.readVault(zipped)).entries;
  const start = [{ path: at(LONG), data: profile(LONG, { money: 4 }) }, { path: 'settings.ini', data: enc.encode('# mine\n') },
    { path: at('Other One'), data: profile('Other One', { career: 6 }) }];
  await seed({ R: {} });
  const r = await M.refillSaves(M.filesOf('R'), cloud, start);
  const got = await folderOf('R');
  assert.equal(r.failed.length, 0);
  assert.deepEqual(profilesOf(got), [LONG, 'Other One'], 'each once, under its own name');
  assert.equal(money(got.get(at(LONG))), 3, 'the cloud\'s');
});
