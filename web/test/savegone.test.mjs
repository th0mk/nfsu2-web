// Adversarial tests of the save folder the browser deletes during a game (502d7e8f: io-worker.ts recover, shell.ts
// saveGone, vault-ui.ts VaultPanel.refill, saves-ui.ts snapshotSaves / refillSaves), on the page's own code: the real
// VaultPanel and VaultSync over an in-memory OPFS (navigator.storage), localStorage and /vault (crates/vault's rules,
// as vault.test.mjs's server), with gates that hold a request or a write where a slow network or disk would.
//   node --test web/test/savegone.test.mjs
// Each test asserts what keeps the player's saves; on 502d7e8f all but the baseline fail: each is a data-loss path (one,
// marked so, is older than 502d7e8f).  The end-to-end one of the first: web/tools/save-wipe-stale.mjs.  The "second pass"
// ones (the review of 34f8145a and 942c80d0) fail on 942c80d0: the folder's mark and the 'wiped' / 'fewer' asks.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export { VaultPanel } from './web/src/app/vault-ui.ts'; export * as SU from './web/src/app/saves-ui.ts';
  export * as V from './web/src/app/vault.ts'; export * as P from './web/src/app/profile.ts'; export { unzip } from './web/src/app/zip.ts';
  export * as SF from './web/src/app/savefs.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral', logLevel: 'error' })).outputFiles[0].text;
const { VaultPanel, SU, V, P, unzip, SF } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

const enc = new TextEncoder(), dec = new TextDecoder();
const DIR = 'NFS Underground 2', B = 36, SAVE = 'save-wipe-test';
const at = n => `${DIR}/${n}/${n}`;
function profile(name, money = 1000) {
  const f = Buffer.alloc(54966);
  f.write('20CM', 0, 'latin1');
  f.writeUInt32LE(54966, 4); f.writeUInt32LE(8, 8); f.writeUInt32LE(54930, 12);
  f.writeUInt32LE(0x10D, 28); f.writeUInt32LE(54930, 32);
  f.write(name, B + 53761, 'latin1');
  f.writeUInt32LE(money, 0xA16A);
  f.writeUInt32LE(P.checksum(f.subarray(28, 36)), 16); f.writeUInt32LE(P.checksum(f.subarray(36)), 20); f.writeUInt32LE(P.checksum(f.subarray(0, 24)), 24);
  return new Uint8Array(f.buffer, f.byteOffset, f.length);
}
const moneyOf = d => new DataView(d.buffer, d.byteOffset).getUint32(0xA16A, true);
const tick = (ms = 0) => new Promise(r => setTimeout(r, ms));
/** a promise to open later, and one that settles when something reached it */
function gate() { let open, reached; const g = { opened: new Promise(r => { open = r; }), hit: new Promise(r => { reached = r; }) }; g.open = open; g.reach = reached; return g; }

// ---- OPFS in memory: handles of a deleted folder answer NotFoundError, as Chrome's do after the site's data is cleared
const notFound = () => new DOMException('A requested file or directory could not be found at the time an operation was processed.', 'NotFoundError');
const fsx = { root: null, hooks: {} };
class FFile {
  constructor(name, parent) { this.kind = 'file'; this.name = name; this.parent = parent; this.data = new Uint8Array(0); this.mtime = Date.now(); }
  alive() { return this.parent.alive() && this.parent.kids.get(this.name) === this; }
  path() { return [...this.parent.trail(), this.name].join('/'); }
  async getFile() {
    if (!this.alive()) throw notFound();
    const d = this.data, m = this.mtime;
    return { size: d.length, lastModified: m, arrayBuffer: async () => d.slice().buffer, text: async () => dec.decode(d) };
  }
  async createWritable() {
    if (!this.alive()) throw notFound();
    const parts = [], f = this;
    return {
      async write(x) { parts.push(typeof x === 'string' ? enc.encode(x) : x instanceof Blob ? new Uint8Array(await x.arrayBuffer()) : new Uint8Array(x)); },
      async close() {
        await fsx.hooks.write?.(f.path());           // a slow or full disk
        if (!f.alive()) throw notFound();
        const n = parts.reduce((a, p) => a + p.length, 0), d = new Uint8Array(n);
        let o = 0; for (const p of parts) { d.set(p, o); o += p.length; }
        f.data = d; f.mtime = Date.now();
        await fsx.hooks.written?.(f.path());         // the next file not begun yet
      },
    };
  }
}
class FDir {
  constructor(name, parent) { this.kind = 'directory'; this.name = name; this.parent = parent; this.kids = new Map(); }
  alive() { return this.parent ? this.parent.alive() && this.parent.kids.get(this.name) === this : this === fsx.root; }
  trail() { return this.parent ? [...this.parent.trail(), this.name].filter(Boolean) : []; }
  async getDirectoryHandle(n, o = {}) {
    if (!this.alive()) throw notFound();
    let k = this.kids.get(n);
    if (k?.kind === 'file') throw new DOMException('', 'TypeMismatchError');
    if (!k) { if (!o.create) { fsx.hooks.missing?.([...this.trail(), n].join('/')); throw notFound(); } k = new FDir(n, this); this.kids.set(n, k); }
    return k;
  }
  async getFileHandle(n, o = {}) {
    if (!this.alive()) throw notFound();
    let k = this.kids.get(n);
    if (k?.kind === 'directory') throw new DOMException('', 'TypeMismatchError');
    if (!k) {
      if (!o.create) { fsx.hooks.missing?.([...this.trail(), n].join('/')); throw notFound(); }
      k = new FFile(n, this); this.kids.set(n, k);
    }
    return k;
  }
  async removeEntry(n, o = {}) {
    if (!this.alive()) throw notFound();
    const k = this.kids.get(n); if (!k) throw notFound();
    if (k.kind === 'directory' && k.kids.size && !o.recursive) throw new DOMException('', 'InvalidModificationError');
    this.kids.delete(n);
  }
  async *entries() { if (!this.alive()) throw notFound(); for (const e of [...this.kids]) yield e; }
  async *keys() { if (!this.alive()) throw notFound(); for (const k of [...this.kids.keys()]) yield k; }
}
const store = new Map();
globalThis.localStorage = { getItem: k => store.has(k) ? store.get(k) : null, setItem: (k, v) => store.set(k, String(v)), removeItem: k => store.delete(k), clear: () => store.clear() };
Object.defineProperty(globalThis.navigator, 'storage', { value: { getDirectory: async () => fsx.root }, configurable: true });
globalThis.isSecureContext = false;
const el = () => { const e = { hidden: false, disabled: false, value: '', type: 'password', readOnly: false, textContent: '', scrollLeft: 0, className: '', dataset: {}, kids: [],
  classList: { toggle() {}, add() {}, remove() {} }, setAttribute() {}, addEventListener() {}, append(...k) { e.kids.push(...k); }, replaceChildren(...k) { e.kids = k; }, focus() {}, select() {} }; return e; };
globalThis.document = { createElement: el, createDocumentFragment: el };
/** what an element built with el() says, its children's words too */
const textOf = e => !e ? '' : typeof e === 'string' ? e : [e.textContent ?? '', ...(e.kids ?? []).map(textOf)].filter(Boolean).join(' ');

/** the save folder's files: path -> bytes */
async function folder() {
  const out = {};
  async function walk(d, rel) { for (const [n, k] of d.kids) { const r = rel ? `${rel}/${n}` : n; if (k.kind === 'directory') await walk(k, r); else out[r] = k.data; } }
  const s = fsx.root.kids.get(SAVE); if (s) await walk(s, '');
  return out;
}
async function put(p, data) {
  let d = await fsx.root.getDirectoryHandle(SAVE, { create: true });
  const parts = p.split('/');
  for (const x of parts.slice(0, -1)) d = await d.getDirectoryHandle(x, { create: true });
  const f = await d.getFileHandle(parts.at(-1), { create: true });
  f.data = typeof data === 'string' ? enc.encode(data) : data; f.mtime = Date.now();
}
/** put() at once, in the same task (a write of the game's landing between two steps of the page's) */
function putNow(p, data) {
  let d = fsx.root.kids.get(SAVE);
  if (!d) { d = new FDir(SAVE, fsx.root); fsx.root.kids.set(SAVE, d); }
  const parts = p.split('/');
  for (const x of parts.slice(0, -1)) { let k = d.kids.get(x); if (!k) { k = new FDir(x, d); d.kids.set(x, k); } d = k; }
  const f = d.kids.get(parts.at(-1)) ?? new FFile(parts.at(-1), d);
  d.kids.set(parts.at(-1), f); f.data = data; f.mtime = Date.now();
}
/** the browser clears the site's data: OPFS and localStorage (Storage.clearDataForOrigin, all) */
function wipe() { fsx.root = new FDir('', null); store.clear(); }

// ---- /vault in memory (vault.test.mjs's server), with gates: a request held until its gate opens
function server() {
  const kept = new Map(), DAY = 86400000;
  const s = { now: Date.parse('2026-11-02T12:00:00Z'), kept, refuse: 0, gates: [], puts: 0 };
  const tag = n => `"${n.toString(16).padStart(16, '0')}"`;
  let revs = 1;
  const head = (v, earlier) => ({ etag: tag(v.rev), 'x-vault-saved': String(v.saved), ...(earlier ? { 'x-vault-earlier': String(earlier.saved) } : {}) });
  s.fetch = async (url, init = {}) => {
    const h = Object.fromEntries(Object.entries(init.headers ?? {}).map(([k, v]) => [k.toLowerCase(), v]));
    const token = (h.authorization ?? '').replace(/^Bearer /, ''), method = init.method ?? 'GET';
    if (!token) return new Response('A keyphrase\'s token is needed', { status: 401 });
    const g = s.gates.find(x => x.method === method && !x.used);
    if (g) { g.used = true; g.reach(); await g.opened; }
    if (s.refuse) return new Response('no', { status: s.refuse });
    const k = kept.get(token);
    if (method === 'GET') {
      const v = url.endsWith('?earlier') ? k?.earlier : k?.cur;
      if (!v) return new Response('Not found', { status: 404 });
      if (!url.endsWith('?earlier') && h['if-none-match'] === tag(v.rev)) return new Response(null, { status: 304, headers: head(v, k.earlier) });
      return new Response(v.body, { status: 200, headers: head(v, url.endsWith('?earlier') ? null : k.earlier) });
    }
    if (method === 'PUT') {
      if (h['if-none-match'] === '*' ? k : !k || h['if-match'] !== tag(k.cur.rev))
        return new Response('Changed elsewhere', { status: 412, headers: k ? head(k.cur, k.earlier) : {} });
      s.puts++;
      const v = { rev: revs++, saved: s.now, body: new Uint8Array(init.body) };
      const n = !k ? { cur: v, earlier: null } : Math.floor(k.cur.saved / DAY) !== Math.floor(s.now / DAY) ? { cur: v, earlier: k.cur } : { cur: v, earlier: k.earlier };
      kept.set(token, n);
      s.now += 1000;
      if (s.loseNext) { s.loseNext = false; throw new TypeError('Failed to fetch'); }   // kept, its answer lost (issue 33)
      return new Response('Saved', { status: k ? 200 : 201, headers: head(v, n.earlier) });
    }
    return new Response('no', { status: 405 });
  };
  s.hold = method => { const g = gate(); g.method = method; s.gates.push(g); return g; };
  return s;
}
/** the cloud's current version's files: path -> bytes */
async function cloudFiles(srv, phrase) {
  const k = await V.keysOf(phrase), v = srv.kept.get(k.token).cur;
  return Object.fromEntries((await unzip(await V.unpack(await V.unseal(k.key, v.body)))).map(e => [e.name, e.data]));
}

/** a fresh browser: an empty OPFS and localStorage, the server, and the start page's VaultPanel owning the saves */
async function page(srv, o = {}) {
  globalThis.fetch = (u, i) => srv.fetch(u, i);
  const logs = [];
  const st = { playing: !!o.playing };
  const asked = [];                                          // the questions the player was asked, answered yes / the first
  const answers = { confirm: true, choose: 'ok' };           // (a test may answer otherwise: choose '' is Esc)
  const panel = new VaultPanel({ querySelector: (() => { const m = new Map(); return s => m.get(s) ?? (m.set(s, el()), m.get(s)); })() }, {
    saveDir: SAVE, owner: () => true, playing: () => st.playing, busy: () => st.playing,
    confirm: async (title, body, ok, danger) => { asked.push({ title, body: textOf(body), ok, danger: !!danger }); return answers.confirm; },
    choose: async (title, body, ok, cancel, danger) => { asked.push({ title, body: textOf(body), ok, cancel, danger: !!danger }); return answers.choose; },
    onRestored() {}, onLink() {}, log: s => logs.push(s),
  });
  await panel.start();
  await panel.ownerNow();
  const sync = () => panel.sync;
  // what check() started (a pull, then a push) has ended
  const settle = async () => { await sync().pull().catch(() => {}); await sync().push().catch(() => {}); };
  await settle();
  return { panel, sync, settle, logs, st, asked, answers };
}
const reset = () => { fsx.root = new FDir('', null); fsx.hooks = {}; store.clear(); };

// ---------------------------------------------------------------------------------------------------------------------

test('baseline: linked, nothing pending, the wipe: the cloud\'s back, the game\'s save on top goes to the cloud', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put(at('MEH'), profile('MEH', 500)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  const phrase = await p.sync().create();
  p.st.playing = true;
  const start = await SU.snapshotSaves(SAVE);            // Play
  wipe();
  const r = await p.panel.refill(start);
  assert.equal(r.from, 'cloud');
  await put(at('TOP'), profile('TOP', 2000));           // the game's save, after save-ready
  await p.sync().push({ playing: true });
  const c = await cloudFiles(srv, phrase);
  assert.deepEqual(Object.keys(c).sort(), [at('MEH'), at('TOP'), 'settings.ini']);
  assert.equal(moneyOf(c[at('TOP')]), 2000);
  assert.ok(store.get(`nfsu2-keyphrase:${SAVE}`), 'the link kept again');
});

test('loses: sends failed for a while (server full, offline), then the wipe: the cloud\'s older copy replaces this browser\'s newer one', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  const phrase = await p.sync().create();
  // days of play here while every send fails (VAULT-TOO-LARGE, 507, no connection): TOP earns money, MEH is made here
  srv.refuse = 507;
  await put(at('TOP'), profile('TOP', 9000)); await put(at('MEH'), profile('MEH', 4000));
  await p.sync().push().catch(() => {});
  assert.equal(p.sync().state.s, 'error');
  srv.refuse = 0;
  p.st.playing = true;
  const start = await SU.snapshotSaves(SAVE);            // Play: TOP 9000 and MEH are here
  wipe();
  const r = await p.panel.refill(start);
  const after = await folder();
  // what pull() would never do by itself (the files here changed since the link): the cloud's taken over them
  assert.ok(after[at('MEH')], `MEH, made here and never sent, is back (refill took '${r.from}': ${Object.keys(after).join(', ')})`);
  assert.equal(moneyOf(after[at('TOP')]), 9000, 'TOP as this browser had it, not the cloud\'s older copy');
  void phrase;
});

test('loses: the server went back (a lost write): the wipe takes a version older than this browser\'s link, which pull() refuses', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  const phrase = await p.sync().create();
  const k = await V.keysOf(phrase), v1 = srv.kept.get(k.token).cur;
  await put(at('TOP'), profile('TOP', 5000)); await put(at('MEH'), profile('MEH', 700));
  await p.sync().push();
  assert.notEqual(srv.kept.get(k.token).cur.rev, v1.rev);
  // the server's disk restored from the day before: it has v1 again
  srv.kept.set(k.token, { cur: v1, earlier: null });
  // pull() alone: an 'older' conflict, nothing replaced
  await p.sync().pull();
  assert.equal(p.sync().state.s, 'conflict');
  assert.equal(p.sync().state.why, 'older');
  p.sync().reopen();
  p.st.playing = true;
  const start = await SU.snapshotSaves(SAVE);
  wipe();
  await p.panel.refill(start);
  const after = await folder();
  assert.ok(after[at('MEH')], 'MEH (sent, then lost by the server) is back from this browser\'s start');
  assert.equal(moneyOf(after[at('TOP')]), 5000);
});

test('loses: the game\'s save lands before the refill ends (12 s passed): linked as the cloud\'s version, never sent, then replaced by another browser\'s without a conflict', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put(at('MEH'), profile('MEH', 500)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  const phrase = await p.sync().create();
  p.st.playing = true;
  const start = await SU.snapshotSaves(SAVE);
  const slow = srv.hold('GET');                            // the server answers the refill's GET late (under 8 s)
  wipe();
  const refill = p.panel.refill(start);
  await slow.hit;
  // the I/O worker gave up waiting (SAVE_WAIT_MS): the game's save goes into the new folder now
  await put(at('TOP'), profile('TOP', 7000));
  slow.open();
  assert.equal((await refill).from, 'cloud');
  assert.equal(moneyOf((await folder())[at('TOP')]), 7000, 'the game\'s file is not written over');
  const puts = srv.puts;
  await p.sync().push({ playing: true });                  // the game's save's send (10 s after it)
  const sent = srv.puts > puts;
  // the player's other browser (a phone) loads the passphrase and plays MEH there
  const bf = new Map();
  const b = new V.VaultSync({ files: async () => V.kept([...bf].map(([path, data]) => ({ path, data }))), restore: async () => {},
    load: () => null, save: () => {}, fetch: srv.fetch });
  const got = await b.peek(phrase);
  for (const e of await unzip(got.zip)) bf.set(e.name, e.data);
  await b.adopt(phrase, got.remote);
  bf.set(at('MEH'), profile('MEH', 900));
  await b.push();
  // back here, the game ends: newer saves from the other browser come in
  p.st.playing = false;
  await p.sync().pull();
  await p.sync().push();
  const here = await folder(), cloud = await cloudFiles(srv, phrase);
  assert.equal(moneyOf(here[at('TOP')]), 7000, `TOP as this browser's game saved it, here (the game's save was ${sent ? '' : 'never '}sent; state ${p.sync().state.s})`);
  assert.equal(moneyOf(cloud[at('TOP')]), 7000, 'and in the cloud');
});

test('loses: the game writes its profile between the refill\'s look and its write (12 s passed): the cloud\'s older copy lands over it', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  await p.sync().create();
  p.st.playing = true;
  const start = await SU.snapshotSaves(SAVE);
  wipe();
  // the I/O worker stopped holding the game's calls: its save of TOP lands right after the refill looked for TOP
  let raced = false;
  fsx.hooks.missing = pth => { if (!raced && `${SAVE}/${at('TOP')}`.startsWith(pth)) { raced = true; putNow(at('TOP'), profile('TOP', 7000)); } };
  await p.panel.refill(start);
  assert.ok(raced);
  assert.equal(moneyOf((await folder())[at('TOP')]), 7000, 'the game\'s newer save stays');
});

test('loses: a send queued before the hold runs during it (the server slow, the 8 s fallback): the folder half written goes to the cloud', { timeout: 30000 }, async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put(at('MEH'), profile('MEH', 500)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  const phrase = await p.sync().create();
  p.st.playing = true;
  const start = await SU.snapshotSaves(SAVE);
  // the game saved twice, a burst apart: the first send's PUT is slow (a bad connection), the second waits behind it
  await put(at('TOP'), profile('TOP', 2000));
  const slowPut = srv.hold('PUT');
  const first = p.sync().push({ playing: true });
  await slowPut.hit;
  await put(at('TOP'), profile('TOP', 3000));
  const second = p.sync().push({ playing: true });         // called before the wipe: held is still false
  wipe();
  // the refill: the cloud's GET waits behind both sends, so after 8 s it writes the start's files; the disk is slow
  let n = 0;
  fsx.hooks.written = async () => { if (++n === 1) { slowPut.open(); await first.catch(() => {}); await second.catch(() => {}); } };
  const r = await p.panel.refill(start);
  assert.equal(r.from, 'start');
  const c = await cloudFiles(srv, phrase);
  assert.deepEqual(Object.keys(c).sort(), [at('MEH'), at('TOP'), 'settings.ini'], 'the cloud keeps both profiles: nothing is sent while held');
});

test('loses: a refill that stops halfway (no room), with a send answered during it: after a reload the next push drops the cloud\'s other profile', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put(at('MEH'), profile('MEH', 500)); await put('settings.ini', '# settings\n');
  let p = await page(srv);
  const phrase = await p.sync().create();
  p.st.playing = true;
  const start = await SU.snapshotSaves(SAVE);
  // a save's send in flight when the browser clears the site's data; its answer comes after
  await put(at('TOP'), profile('TOP', 2000));
  const slowPut = srv.hold('PUT');
  const inflight = p.sync().push({ playing: true });
  await slowPut.hit;
  wipe();
  fsx.hooks.write = async () => { throw new DOMException('No space available for this operation', 'QuotaExceededError'); };
  const refill = p.panel.refill(start).then(() => 'back', e => `failed: ${e.name}`);
  slowPut.open(); await inflight;
  assert.equal(await refill, 'failed: QuotaExceededError');
  // process.ts posts save-ready anyway: the game saves its profile into the empty folder, which the disk takes now
  fsx.hooks.write = undefined;
  await put(at('TOP'), profile('TOP', 3000));
  assert.ok(store.get(`nfsu2-keyphrase:${SAVE}`), 'the send\'s answer put the link back into localStorage');
  // the player reloads the page (the toast said the saves could not be put back)
  p = await page(srv);
  assert.ok(p.sync().link);
  const c = await cloudFiles(srv, phrase);
  // (the failed write left MEH's file empty: getFileHandle({ create }) made it before the write failed)
  const sizes = Object.entries(c).map(([k, v]) => `${k} ${v.length}`).join(', ');
  assert.equal(c[at('MEH')]?.length, 54966, `the cloud still has MEH whole (it has ${sizes})`);
  assert.equal(moneyOf(c[at('MEH')]), 500);
});

test('loses: no cloud saves, another profile saved earlier in this game: the wipe brings back its version from Play', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put(at('MEH'), profile('MEH', 500)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  p.st.playing = true;
  const start = await SU.snapshotSaves(SAVE);             // Play (shell.ts playStart, kept up by onSaved)
  await put(at('MEH'), profile('MEH', 6000));             // MEH loaded and saved in this game, then TOP loaded
  await SU.snapshotFile(start, SAVE, at('MEH'));          // what shell.ts onSaved does with the game's 'saved'
  wipe();
  assert.equal((await p.panel.refill(start)).from, 'start');
  await put(at('TOP'), profile('TOP', 1500));           // the game's next save: TOP
  assert.equal(moneyOf((await folder())[at('MEH')]), 6000, 'MEH as this game saved it');
});

test('a refill whose cloud write fails falls back to nothing: the start\'s profiles (only in memory) are not written either', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put(at('MEH'), profile('MEH', 500)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  await p.sync().create();
  p.st.playing = true;
  const start = await SU.snapshotSaves(SAVE);
  wipe();
  // one write refused (a transient error: the game's handle on that file, a hiccup of the disk), the rest would go
  let n = 0;
  fsx.hooks.write = async () => { if (++n === 1) throw new DOMException('busy', 'NoModificationAllowedError'); };
  const r = await p.panel.refill(start).then(x => x.from, e => `failed: ${e.name}`);
  const after = await folder(), sizes = Object.entries(after).map(([k, v]) => `${k} ${v.length}`).join(', ') || 'empty';
  assert.ok(Object.values(after).every(v => v.length > 0), `no empty file left by the failed write (folder: ${sizes})`);
  assert.ok(after[at('MEH')]?.length && after[at('TOP')]?.length, `both profiles back after one failed write (refill: ${r}; folder: ${sizes})`);
});

test('loses (before 502d7e8f too): the site\'s data cleared while the start page sits open: the next game starts empty, and its first new profile replaces the cloud\'s', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 90000)); await put(at('MEH'), profile('MEH', 500)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  const phrase = await p.sync().create();
  wipe();                                                  // the page still holds the link in memory
  // Play: the I/O worker makes an empty save folder (no 'save-gone': its folder never went), the game makes a profile
  await p.panel.beforePlay();
  p.st.playing = true;
  await put(at('TOP'), profile('TOP', 0));               // a new career under the same name
  await p.sync().push({ playing: true });
  const c = await cloudFiles(srv, phrase);
  assert.equal(c[at('MEH')]?.length, 54966, `the cloud keeps MEH (it has ${Object.keys(c).join(', ')})`);
  assert.equal(moneyOf(c[at('TOP')]), 90000, 'and TOP\'s career');
});

test('loses: a conflict waiting when the wipe comes (another browser sent meanwhile): settled for the cloud unasked, with no copy kept (Put back)', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  const phrase = await p.sync().create();
  // the other browser sends TOP 1200; this one made MEH and saved TOP 8000 meanwhile: a conflict, the player to pick
  const bf = new Map();
  const b = new V.VaultSync({ files: async () => V.kept([...bf].map(([path, data]) => ({ path, data }))), restore: async () => {},
    load: () => null, save: () => {}, fetch: srv.fetch });
  const got = await b.peek(phrase);
  for (const e of await unzip(got.zip)) bf.set(e.name, e.data);
  await b.adopt(phrase, got.remote);
  bf.set(at('TOP'), profile('TOP', 1200));
  await b.push();
  await put(at('TOP'), profile('TOP', 8000)); await put(at('MEH'), profile('MEH', 3000));
  await p.sync().push();
  assert.equal(p.sync().state.s, 'conflict');
  p.st.playing = true;
  const start = await SU.snapshotSaves(SAVE);
  wipe();
  await p.panel.refill(start);
  const after = await folder();
  const kept = fsx.root.kids.get(`${SAVE}~replaced`);
  assert.ok(after[at('MEH')] || kept, `MEH (only here) back, or kept for Put back (state now ${p.sync().state.s}; folder ${Object.keys(after).join(', ')})`);
  assert.equal(p.sync().state.s, 'conflict', 'the conflict still waits for the player');
});

// ---- second pass (the fixes in 34f8145a, 942c80d0) -------------------------------------------------------------------

test('second pass, loses: a send answered after the site\'s data was cleared (the page open) puts the folder\'s mark back: the next game\'s new career replaces the cloud\'s only profile', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 90000)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  const phrase = await p.sync().create();
  await tick(10);
  assert.ok(p.sync().link.folder, 'the link knows its folder');
  // a setting changed on the page: its send is on the way when the browser clears the site's data
  await put('settings.ini', '# settings\nfps = 60\n');
  const slow = srv.hold('PUT');
  const send = p.sync().push();
  await slow.hit;
  wipe();
  slow.open(); await send; await tick(10);
  // Play: the game starts in the empty folder, and the player starts the career again under the same name
  await p.panel.beforePlay();
  p.st.playing = true;
  await put(at('TOP'), profile('TOP', 0));
  await p.sync().push({ playing: true });
  p.st.playing = false;
  await p.sync().push();                                   // the game's end
  const st = p.sync().state;
  assert.equal(moneyOf((await cloudFiles(srv, phrase))[at('TOP')]), 90000, `the cloud keeps TOP's career (state ${st.s} ${st.why ?? ''})`);
});

test('second pass, loses: a version of this browser\'s own whose answer was lost (relinked) forgets the folder\'s mark and Link.held: a wipe after it goes unnoticed', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 90000)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  const phrase = await p.sync().create();
  await tick(10);
  // a save sent: the server keeps it, its answer is lost (issue 33)
  await put(at('TOP'), profile('TOP', 91000));
  srv.loseNext = true;
  await p.sync().push().catch(() => {});
  assert.equal(p.sync().link.sent?.length, 1);
  wipe();
  await p.panel.beforePlay();                              // its pull finds that version: this browser's own, relinked
  const folderKept = !!p.sync().link.folder;
  p.st.playing = true;
  await put(at('TOP'), profile('TOP', 0));
  await p.sync().push({ playing: true });
  p.st.playing = false;
  await p.sync().push();
  const st = p.sync().state;
  assert.equal(moneyOf((await cloudFiles(srv, phrase))[at('TOP')]), 91000,
    `the cloud keeps TOP's career (link.folder after the relink: ${folderKept ? 'kept' : 'gone'}; state ${st.s} ${st.why ?? ''})`);
});

test('second pass: a profile here that is not whole (a save torn by a crash) makes every send ask, even after the player kept this browser\'s, and nothing goes during a game', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put(at('MEH'), profile('MEH', 500)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  const phrase = await p.sync().create();
  await put(at('MEH'), profile('MEH', 500).slice(0, 30000));   // the tab closed during MEH's save, in an earlier game
  await p.sync().push();
  assert.equal(p.sync().state.why, 'fewer');
  await p.panel.keepMine();                                // the player keeps this browser's
  assert.equal(p.sync().state.s, 'synced');
  // the next game: TOP saved
  p.st.playing = true;
  await put(at('TOP'), profile('TOP', 2000));
  await p.sync().push({ playing: true });
  const duringGame = moneyOf((await cloudFiles(srv, phrase))[at('TOP')]);
  p.st.playing = false;
  await p.sync().push();                                   // the game's end
  const st = p.sync().state;
  assert.equal(st.s, 'synced', `the player's choice stands (state ${st.s} ${st.why ?? ''}; TOP in the cloud during the game: ${duringGame})`);
  assert.equal(moneyOf((await cloudFiles(srv, phrase))[at('TOP')]), 2000, 'TOP\'s game is in the cloud');
});

test('second pass: the "wiped" and "fewer" questions say what Keep this browser\'s deletes in the cloud, its button red', async () => {
  const problems = [];
  // wiped: the site's data cleared while the page sat open, then a game made a new profile
  {
    reset();
    const srv = server();
    await put(at('TOP'), profile('TOP', 90000)); await put(at('MEH'), profile('MEH', 500)); await put('settings.ini', '# settings\n');
    const p = await page(srv);
    await p.sync().create(); await tick(10);
    wipe();
    await put(at('NEWBIE'), profile('NEWBIE', 0));
    await p.sync().push();
    assert.equal(p.sync().state.why, 'wiped');
    await p.panel.askAtPlay();                             // answered with its first button
    const atPlay = p.asked.find(a => 'cancel' in a);
    if (!/Cloud\s[^·]*TOP/.test(atPlay.body)) problems.push(`wiped, Play's question shows none of the cloud's profiles: "${atPlay.body}" (first button: ${atPlay.ok})`);
    const q = p.asked.find(a => a.ok && /Keep this browser/.test(a.ok) && 'danger' in a);
    if (!q) problems.push('wiped: no confirm for Keep this browser\'s');
    else {
      if (!/TOP/.test(q.body)) problems.push(`wiped, Keep this browser's: the cloud's TOP and MEH it deletes are not named: "${q.body}"`);
      if (!q.danger) problems.push('wiped, Keep this browser\'s: not red');
    }
  }
  // fewer: MEH torn here (a crash during its save), the cloud's MEH whole
  {
    reset();
    const srv = server();
    await put(at('TOP'), profile('TOP', 1000)); await put(at('MEH'), profile('MEH', 500)); await put('settings.ini', '# settings\n');
    const p = await page(srv);
    await p.sync().create();
    await put(at('MEH'), profile('MEH', 500).slice(0, 30000));
    await p.sync().push();
    assert.equal(p.sync().state.why, 'fewer');
    await p.panel.keepMine();
    const q = p.asked.at(-1);
    if (!/Cloud\s[^·]*\bMEH \$500/.test(q.body)) problems.push(`fewer, Keep this browser's: the cloud's side (its whole MEH) is not shown: "${q.body}"`);
    if (!q.danger) problems.push('fewer, Keep this browser\'s: not red');
  }
  assert.deepEqual(problems, []);
});

// ---- third pass (f4e3f301) ---------------------------------------------------------------------------------------------

test('third pass: a "thin" question (an older page\'s version without MEH): Use the cloud\'s says it deletes MEH here, and Keep this browser\'s (which keeps it) is not the one marked dangerous', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put(at('MEH'), profile('MEH', 500)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  const phrase = await p.sync().create();
  // the player's other device, on an older page (no zip comment), sends a version without MEH: its folder was deleted
  // and the game saved TOP since, which that page sent by itself
  const k = await V.keysOf(phrase), cur = srv.kept.get(k.token).cur;
  const files = (await unzip(await V.unpack(await V.unseal(k.key, cur.body)))).filter(e => e.name !== at('MEH')).map(e => ({ path: e.name, data: e.data }));
  const tag = `"${cur.rev.toString(16).padStart(16, '0')}"`;
  const r = await srv.fetch('/vault', { method: 'PUT', headers: { authorization: `Bearer ${k.token}`, 'if-match': tag }, body: await V.seal(k.key, await V.pack(files)) });
  assert.equal(r.status, 200);
  await p.sync().pull();
  assert.equal(p.sync().state.why, 'thin');
  const problems = [];
  p.answers.choose = '';                                   // Play's question: Esc, back to the page
  await p.panel.askAtPlay();
  const q = p.asked.find(a => 'cancel' in a);
  if (q.danger) problems.push(`Play's question marks Keep this browser's (which keeps MEH) red, and its focused Use the cloud's deletes MEH here: "${q.body}"`);
  await p.panel.takeTheirs();                              // the Saves tab's Use the cloud's
  const c = p.asked.at(-1);
  if (!/\bMEH\b[^.]*\b(deleted|removed|lost|goes|gone)\b/i.test(c.body)) problems.push(`Use the cloud's: its confirm does not say MEH is deleted here: "${c.body}"`);
  assert.deepEqual(problems, []);
});

test('third pass: Go back (VaultPanel.goBack) to the copy of an earlier day with fewer profiles is sent as the player chose, not asked', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  const phrase = await p.sync().create();
  srv.now += 86400000;                                     // the next day: the version before is the earlier copy
  await put(at('NEW'), profile('NEW', 0));
  await p.sync().push();
  assert.ok(p.sync().link.earlier, 'an earlier copy');
  const rev = p.sync().link.rev;
  await p.panel.goBack();                                  // its question answered yes
  assert.equal(p.sync().state.s, 'synced', `${p.sync().state.s} ${p.sync().state.why ?? ''}`);
  assert.notEqual(p.sync().link.rev, rev, 'the earlier copy is the latest now');
  const c = await cloudFiles(srv, phrase);
  assert.deepEqual([!!c[at('TOP')], !!c[at('NEW')]], [true, false], `the cloud: ${Object.keys(c).join(', ')}`);
});

test('review 4: a Put back that fails (no room to stage) keeps the saves it was to put back', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  await p.sync().create();
  srv.now += 86400000;                                     // the next day: 1000 is the earlier copy
  await put(at('TOP'), profile('TOP', 5000));            // hours played since
  await p.sync().push();
  await p.panel.goBack();                                  // the player goes back a day: 5000 kept for Put back
  const kept = async () => {
    const z = await unzip(fsx.root.kids.get(`${SAVE}~replaced`).kids.get('saves.zip').data);
    return moneyOf(z.find(e => e.name === at('TOP')).data);
  };
  assert.deepEqual([moneyOf((await folder())[at('TOP')]), await kept()], [1000, 5000]);
  // Put back, the browser's storage full: staging the 5000 fails
  fsx.hooks.write = path => { if (path.includes('~incoming')) throw new DOMException('quota', 'QuotaExceededError'); };
  await p.panel.putBack();
  fsx.hooks = {};
  const here = moneyOf((await folder())[at('TOP')]);
  assert.ok(here === 5000 || await kept() === 5000, `the 5000 career somewhere: here ${here}, kept ${await kept()}`);
});

// ---- fourth pass (cae4d8df) --------------------------------------------------------------------------------------------

test('fourth pass, loses: a restore whose undo stops halfway (a move back refused) is undone again later, and that deletes the profiles the first undo put back', async () => {
  reset();
  await put(at('TOP'), profile('TOP', 1000)); await put(at('MEH'), profile('MEH', 500)); await put('settings.ini', '# old\n');
  // a backup restore (or a cloud take): TOP, MEH and settings.ini, all new
  const files = [{ path: at('TOP'), data: profile('TOP', 2000) }, { path: at('MEH'), data: profile('MEH', 2500) },
    { path: 'settings.ini', data: enc.encode('# new\n') }];
  // (this OPFS has no move(): savefs copies, as in a browser without it) settings.ini's move into place is refused
  // once (a file held open), and then, once, the undo's move of MEH back (the disk full, a hiccup)
  let refuseSettings = true, refuseMehBack = false;
  fsx.hooks.write = async pth => {
    if (pth === `${SAVE}/settings.ini` && refuseSettings) { refuseSettings = false; refuseMehBack = true; throw new DOMException('held', 'NoModificationAllowedError'); }
    if (pth === `${SAVE}/${at('MEH')}` && refuseMehBack) { refuseMehBack = false; throw new DOMException('No space available for this operation', 'QuotaExceededError'); }
  };
  await assert.rejects(SF.writeTogether(SAVE, files));
  fsx.hooks.write = undefined;
  const between = await folder();
  // the next restore, or the next page's start, undoes what is left (savefs.ts settleStaged)
  await SF.settleStaged(SAVE);
  const after = await folder();
  const money = f => f ? (f.length === 54966 ? moneyOf(f) : `${f.length} bytes`) : 'gone';
  assert.deepEqual([money(after[at('TOP')]), money(after[at('MEH')]), after['settings.ini'] && dec.decode(after['settings.ini'])],
    [1000, 500, '# old\n'], `the saves as they were (after the first undo: TOP ${money(between[at('TOP')])}, MEH ${money(between[at('MEH')])})`);
});

test('review 5: a page closed after Use the cloud\'s / Go back wrote, before its copy became the kept one, still offers Put back', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 5000)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  await p.sync().create();
  // the player replaces the saves here (Load, Go back, Use the cloud's): the copy written with the restore (since
  // 2026-11-02: before, beside the kept one, then made the kept one); the page closes once it wrote
  const copy = await p.panel.replacedCopy();
  const entries = [{ path: at('TOP'), data: profile('TOP', 1000) }, { path: 'settings.ini', data: enc.encode('# settings\n') }];
  await SU.restoreVault(SAVE, { kind: 'backup', entries, profiles: ['TOP'], skipped: [], refused: [], modded: [], edited: [], repaired: [] }, undefined, copy.files);
  assert.equal(moneyOf((await folder())[at('TOP')]), 1000, 'replaced here');
  // the next page
  const q = await page(srv);
  const kept = q.panel.replaced;
  assert.ok(kept && kept.side.profiles.some(x => x.money === 5000), `Put back offers the 5000 career: ${JSON.stringify(kept?.side ?? null)}`);
});

test('review 5: a replacing that never wrote leaves no copy; the saves stay, and a copy an older page left beside goes', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 5000)); await put('settings.ini', '# settings\n');
  const p = await page(srv);
  await p.sync().create();
  // the restore with its copy, its staging cut (no room): nothing of it written
  const copy = await p.panel.replacedCopy();
  const entries = [{ path: at('TOP'), data: profile('TOP', 1000) }, { path: 'settings.ini', data: enc.encode('# settings\n') }];
  fsx.hooks.write = path => { if (path.includes('~incoming')) throw new DOMException('quota', 'QuotaExceededError'); };
  await assert.rejects(SU.restoreVault(SAVE, { kind: 'backup', entries, profiles: ['TOP'], skipped: [], refused: [], modded: [], edited: [], repaired: [] }, undefined, copy.files));
  fsx.hooks = {};
  // (a copy beside the kept one, as a page of before 2026-11-02 wrote it)
  await SF.writeFile(`${SAVE}~replaced`, 'saves.next.zip', new Uint8Array([1]));
  const q = await page(srv);
  await q.panel.settleKept();                              // (shell.ts: in the writer queue, after settleStaged)
  assert.equal(q.panel.replaced, null, 'no Put back of the saves still here');
  assert.equal(fsx.root.kids.get(`${SAVE}~replaced`)?.kids.has('saves.next.zip') ?? false, false, 'the old copy gone');
  assert.equal(moneyOf((await folder())[at('TOP')]), 5000);
});

test('review 6: a Put back cut during its moves keeps the copy it was putting back, whichever of the page start\'s settles comes first', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put('settings.ini', '# then\n');
  const p = await page(srv);
  await p.sync().create();
  // a kept copy: TOP 1000 (# then), replaced since by TOP 5000 (# here) (since 2026-11-02 written with its restore)
  const copy = await p.panel.replacedCopy();
  await SF.writeTogether(SAVE, [{ path: at('TOP'), data: profile('TOP', 5000) }, { path: 'settings.ini', data: '# here\n' }], copy.files);
  await p.panel.loadReplaced();                            // (as the panel has it after a restore of its own)
  const zipped = async () => {
    const z = await unzip(fsx.root.kids.get(`${SAVE}~replaced`).kids.get('saves.zip').data);
    return moneyOf(z.find(e => e.name === at('TOP')).data);
  };
  assert.equal(await zipped(), 1000, 'the copy to put back');
  // Put back; the page closes during the moves: TOP 1000 is moved in, then nothing more runs (every write fails)
  let intoSave = 0;
  fsx.hooks.write = path => { if (intoSave) throw new DOMException('closed', 'AbortError'); if (path.startsWith(`${SAVE}/`)) intoSave++; };
  await p.panel.putBack();
  fsx.hooks = {};
  assert.equal(moneyOf((await folder())[at('TOP')]), 1000, '(the moves cut halfway: TOP moved in, settings.ini not)');
  // the next page: its VaultPanel starts (the server answered fast) before the shell's settleStaged has undone the moves
  globalThis.fetch = (u, i) => srv.fetch(u, i);
  const q = new VaultPanel({ querySelector: (() => { const m = new Map(); return s => m.get(s) ?? (m.set(s, el()), m.get(s)); })() }, {
    saveDir: SAVE, owner: () => true, playing: () => false, busy: () => false, confirm: async () => true, choose: async () => 'ok',
    onRestored() {}, onLink() {}, log: () => {},
  });
  await q.start();
  await SF.settleStaged(SAVE);
  assert.equal(moneyOf((await folder())[at('TOP')]), 5000, 'the moves undone');
  assert.equal(await zipped(), 1000, 'the copy it was putting back is still the kept one');
});

test('review 7: Put back twice swaps back and forth, its copy fresh each time; a Put back that fails changes neither', async () => {
  reset();
  const srv = server();
  await put(at('TOP'), profile('TOP', 1000)); await put('settings.ini', '# then\n');
  const p = await page(srv);
  await p.sync().create();
  // a kept copy (TOP 1000), the saves here TOP 5000
  const copy = await p.panel.replacedCopy();
  await SF.writeTogether(SAVE, [{ path: at('TOP'), data: profile('TOP', 5000) }, { path: 'settings.ini', data: '# here\n' }], copy.files);
  await p.panel.loadReplaced();
  const keptMoney = async () => moneyOf((await unzip(fsx.root.kids.get(`${SAVE}~replaced`).kids.get('saves.zip').data)).find(e => e.name === at('TOP')).data);
  const here = async () => moneyOf((await folder())[at('TOP')]);
  const seen = [[await here(), await keptMoney()]];
  for (let i = 0; i < 3; i++) { await p.panel.putBack(); seen.push([await here(), await keptMoney()]); }
  assert.deepEqual(seen, [[5000, 1000], [1000, 5000], [5000, 1000], [1000, 5000]]);
  assert.ok(Date.now() - p.panel.replaced.at < 60_000, 'the copy kept now, not the first one\'s time (30 days from now)');
  assert.ok(p.panel.replaced.side.profiles.some(x => x.money === 5000), 'Put back offers what it keeps');
  // no room to stage: nothing changes, the copy stays
  fsx.hooks.write = path => { if (path.includes('~incoming')) throw new DOMException('quota', 'QuotaExceededError'); };
  await p.panel.putBack();
  fsx.hooks = {};
  assert.deepEqual([await here(), await keptMoney()], [1000, 5000]);
});
