// A game-data read that never comes back from the I/O worker (web/src/host/io-worker.ts), on the worker's own code in
// a worker thread, over an in-memory OPFS and a made-up game folder on a made-up server:
//   node --test web/test/io-stuck.test.mjs
// A player's free roam (2026-11-02, Chrome on Linux, every block from this PC's cache) read nothing more after one
// read went into the worker and never came out: the game's file thread waited in ReadFile for 12 min, its friends'
// cars stopped coming in, and leaving for the garage hung.  The log said nothing of it.  Now a cache file that does
// not open in 10 s is given up (its reads come from the server), and a request unanswered for 20 s is written in the
// log with the file and the step it waits on.  On 2f8d4f44 both tests fail: the read never ends, the log is silent.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { Worker } from 'node:worker_threads';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const bundle = (await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/host/io-worker.ts')], bundle: true, format: 'iife',
  write: false, logLevel: 'error' })).outputFiles[0].text;

// proto.ts's layout (the test speaks to the worker as a game thread does: game-worker.ts io)
const MAX_THREADS = 64, CTL_IO_DOORBELL = 1, CTL_IO_BEAT = 6, CTL_INTS = 64 + 4 * MAX_THREADS + 64, RPC_SLOT = 256;
const RPC_POSTED = 1, RPC_DONE = 3, RPC_IDLE = 0;
const OP = { FdRead: 2, FdPread: 3, PathOpen: 18 };
const M = 1 << 20, SIZE = M + M / 2;

// The worker's world: OPFS in memory (createSyncAccessHandle may never answer: hangOpen), the server's manifest and
// blocks (a block request may never answer: hangFetch), self.postMessage to the test.
const world = `
const { parentPort, workerData: W } = require('node:worker_threads');
globalThis.self = globalThis; globalThis.postMessage = m => parentPort.postMessage(m); globalThis.onmessage = null;
const SIZE = ${SIZE};
const file = new Uint8Array(SIZE); for (let i = 0; i < SIZE; i++) file[i] = (i * 7 + (i >> 11)) & 255;
class Sync {
  constructor(f) { this.f = f; }
  read(b, { at }) { const n = Math.max(0, Math.min(b.length, this.f.data.length - at)); b.set(this.f.data.subarray(at, at + n)); return n; }
  write(b, { at }) { if (at + b.length > this.f.data.length) { const d = new Uint8Array(at + b.length); d.set(this.f.data); this.f.data = d; } this.f.data.set(b, at); return b.length; }
  getSize() { return this.f.data.length; }
  truncate(n) { const d = new Uint8Array(n); d.set(this.f.data.subarray(0, n)); this.f.data = d; }
  flush() {} close() {}
}
class FFile {
  constructor(name) { this.kind = 'file'; this.name = name; this.data = new Uint8Array(0); }
  createSyncAccessHandle() {
    // saveFail: a file of that name will not open, its folder still there (Firefox 159's "Entry not found", 2026-11-04)
    if (W.saveFail && new RegExp(W.saveFail).test(this.name)) return Promise.reject(new DOMException('Entry not found', 'NotFoundError'));
    return W.hangOpen && /\\.BIN\\./.test(this.name) ? new Promise(() => {}) : Promise.resolve(new Sync(this));
  }
  async getFile() {
    if (W.hang === 'read' && this.name === 'settings.ini') await new Promise(() => {});
    if (W.readFail === this.name) throw new DOMException('Unknown failure', 'UnknownError');
    const d = this.data; return { size: d.length, lastModified: 0, arrayBuffer: async () => d.slice().buffer };
  }
}
class FDir {
  constructor(name) { this.kind = 'directory'; this.name = name; this.kids = new Map(); }
  async getDirectoryHandle(n, o = {}) {
    if (W.hang === 'lookup' && n === 'save' && !o.create) await new Promise(() => {});
    let k = this.kids.get(n); if (!k) { if (!o.create) throw new DOMException('', 'NotFoundError'); k = new FDir(n); this.kids.set(n, k); } return k;
  }
  async getFileHandle(n, o = {}) { let k = this.kids.get(n); if (!k) { if (!o.create) throw new DOMException('', 'NotFoundError'); k = new FFile(n); this.kids.set(n, k); } return k; }
  async removeEntry(n) { this.kids.delete(n); }
  async *entries() { if (W.hang === 'list' && this.name === 'save') await new Promise(() => {}); for (const e of [...this.kids]) yield e; }
  async *keys() { for (const k of [...this.kids.keys()]) yield k; }
}
const root = new FDir('');
for (const [path, text] of Object.entries(W.saveFiles ?? {})) {
  const parts = ['save', ...path.split('/')];
  let d = root;
  for (const x of parts.slice(0, -1)) { let k = d.kids.get(x); if (!k) { k = new FDir(x); d.kids.set(x, k); } d = k; }
  const f = new FFile(parts[parts.length - 1]); f.data = new TextEncoder().encode(text); d.kids.set(f.name, f);
}
const getDirectory = async () => {
  if (W.hang === 'root') await new Promise(() => {});
  if (W.noStorage) throw new DOMException('Unknown failure', 'UnknownError');
  return root;
};
Object.defineProperty(globalThis, 'navigator', { value: { userAgent: 'Mozilla/5.0 (X11; Linux x86_64) Chrome/156.0.0.0', storage: { getDirectory } }, configurable: true });
/** the browser's storage as it is: path -> its bytes as text */
function dump(d = root, rel = '', out = {}) {
  for (const [n, k] of d.kids) { const r = rel ? rel + '/' + n : n; if (k.kind === 'directory') dump(k, r, out); else out[r] = new TextDecoder().decode(k.data); }
  return out;
}
// (W.track: a track's stream file of that many blocks too, TRACKS/STREAML4RA.BUN, made of the same bytes; every fetch of
// a block noted with its file and time, for the reading ahead)
const fetched = [];
globalThis.fetch = async (url, o = {}) => {
  if (url === '/data-manifest') return new Response(JSON.stringify({ files: [{ p: 'CARS', d: 1 }, { p: 'CARS/A.BIN', s: SIZE, m: 7 },
    ...W.track ? [{ p: 'TRACKS', d: 1 }, { p: 'TRACKS/STREAML4RA.BUN', s: W.track * ${M}, m: 7 }] : []],
    blocks: { url: '/data-blocks/', size: ${M}, max: 4 } }), { status: 200 });
  if (W.hangFetch) return new Promise((_, no) => o.signal?.addEventListener('abort', () => no(new Error('aborted'))));
  const b = +/[?&]b=(\\d+)/.exec(url)[1], track = /STREAML4RA/.test(url);
  fetched.push({ track, b, t: performance.now() });
  if (track) { await new Promise(r => setTimeout(r, W.trackMs ?? 0)); return new Response(new Uint8Array(${M}), { status: 200 }); }
  return new Response(file.slice(b * ${M}, Math.min(SIZE, (b + 1) * ${M})), { status: 200 });
};
parentPort.on('message', m => m && m.t === 'dump' ? parentPort.postMessage({ t: 'dumped', files: dump() }) :
  m && m.t === 'fetched' ? parentPort.postMessage({ t: 'fetches', list: fetched, now: performance.now() }) : globalThis.onmessage({ data: m }));
`;

/** the worker, as a game process has it: its memory, control words and RPC slots; the notes it writes */
async function start(opts) {
  const memory = new WebAssembly.Memory({ initial: 4, maximum: 4, shared: true });
  const ctlBuf = new SharedArrayBuffer(CTL_INTS * 4), rpc = new SharedArrayBuffer(RPC_SLOT * MAX_THREADS);
  const w = new Worker(world + bundle, { eval: true, workerData: opts });
  const notes = [], msgs = [];
  const ready = new Promise((ok, no) => w.on('message', m => {
    if (m.t === 'log') notes.push(m.s); else msgs.push(m);
    if (m.t === 'io-ready') ok();
    if (m.t === 'io-error') no(new Error(m.msg));
  }));
  w.on('error', e => notes.push(`worker error: ${e}`));
  w.postMessage({ memory, ctl: ctlBuf, rpc, cfg: { args: [], env: {}, dataBase: '/data/', manifestUrl: '/data-manifest', saveDir: 'save',
    ...opts.memory ? { saves: 'memory', memFiles: opts.memFiles ?? [] } : {} } });
  await ready;
  const ctl = new Int32Array(ctlBuf), i32 = new Int32Array(rpc), i64 = new BigInt64Array(rpc), dv = new DataView(memory.buffer);
  /** a request from slot 0, as game-worker.ts io posts it; resolves with the WASI result when the worker answers */
  async function call(op, ...args) {
    i32[1] = op; i32[3] = args.length;
    args.forEach((a, i) => { i64[2 + i] = BigInt(a); });
    Atomics.store(i32, 0, RPC_POSTED);
    Atomics.add(ctl, CTL_IO_DOORBELL, 1); Atomics.notify(ctl, CTL_IO_DOORBELL);
    for (let v; (v = Atomics.load(i32, 0)) !== RPC_DONE;) await Atomics.waitAsync(i32, 0, v, 200).value;
    const r = i32[2]; Atomics.store(i32, 0, RPC_IDLE); return r;
  }
  async function open(p) {
    new Uint8Array(memory.buffer).set(new TextEncoder().encode(p), 1024);
    assert.equal(await call(OP.PathOpen, 3, 0, 1024, p.length, 0, 0, 0, 0, 2048), 0);
    return dv.getUint32(2048, true);
  }
  /** n bytes at pos (pread), into memory at 65536 */
  async function pread(fd, pos, n) {
    dv.setUint32(4096, 65536, true); dv.setUint32(4100, n, true);
    assert.equal(await call(OP.FdPread, fd, 4096, 1, pos, 4200), 0);
    return new Uint8Array(memory.buffer).slice(65536, 65536 + dv.getUint32(4200, true));
  }
  /** a file of /save opened (fd 4) with oflags, for writing: the WASI result, and the fd */
  async function openSave(p, oflags) {
    new Uint8Array(memory.buffer).set(new TextEncoder().encode(p), 1024);
    const r = await call(OP.PathOpen, 4, 0, 1024, p.length, oflags, 0x40, 0, 0, 2048);
    return { r, fd: dv.getUint32(2048, true) };
  }
  const str = (at, text) => { const b = new TextEncoder().encode(text); new Uint8Array(memory.buffer).set(b, at); return b.length; };
  /** a file of /save read whole (its first 4 KB): its text, or the WASI error of its open */
  async function readAll(p) {
    const { r, fd } = await openSave(p, 0);
    if (r) return r;
    const t = new TextDecoder().decode(await pread(fd, 0, 4096));
    await call(1 /* FdClose */, fd);
    return t;
  }
  /** a file of /save written whole, as the game's CREATE_ALWAYS does (made, emptied, written, closed) */
  async function writeAll(p, text) {
    const { r, fd } = await openSave(p, 1 | 8);
    assert.equal(r, 0, `open ${p}`);
    dv.setUint32(4096, 70000, true); dv.setUint32(4100, str(70000, text), true);
    assert.equal(await call(4 /* FdWrite */, fd, 4096, 1, 4200), 0);
    assert.equal(await call(1 /* FdClose */, fd), 0);
  }
  const mkdir = async p => call(20 /* PathCreateDirectory */, 4, 1024, str(1024, p));
  const unlink = async p => call(22 /* PathUnlinkFile */, 4, 1024, str(1024, p));
  const rename = async (a, b) => call(23 /* PathRename */, 4, 1024, str(1024, a), 4, 1536, str(1536, b));
  const dump = () => new Promise(res => { const h = m => { if (m.t === 'dumped') { w.off('message', h); res(m.files); } }; w.on('message', h); w.postMessage({ t: 'dump' }); });
  /** the blocks fetched so far ({ track, b, t }, t on the worker's clock) and the worker's time now */
  const fetches = () => new Promise(res => { const h = m => { if (m.t === 'fetches') { w.off('message', h); res(m); } }; w.on('message', h); w.postMessage({ t: 'fetched' }); });
  return { w, notes, msgs, ctl, open, openSave, call, pread, readAll, writeAll, mkdir, unlink, rename, dump, fetches, stop: () => w.terminate() };
}
const expect = (pos, n) => Uint8Array.from({ length: n }, (_, k) => ((pos + k) * 7 + ((pos + k) >> 11)) & 255);

// The track's reading ahead (readAhead) and a friend's car (a 20 Mbit/s line, 2026-11-05: in a meet's first 30 s a block
// read ahead started in each gap between the car's reads, about half of what the line carried): no block is read ahead
// until the game has fetched nothing for AHEAD_QUIET_MS (2 s), then the rest of the track comes.  Before (db588c0e)
// the rest of the track was asked for at once, beside the car's reads.
test('the track is read ahead only once the game has fetched nothing for 2 s', { timeout: 60_000 }, async () => {
  const io = await start({ track: 6 });
  try {
    const tr = await io.open('TRACKS/STREAML4RA.BUN'), car = await io.open('CARS/A.BIN');
    await io.pread(tr, 0, 64);                       // the game's own read of the track: the reading ahead begins
    // a friend's car: a read, a pause, a read (two blocks of its file)
    await io.pread(car, 0, 64);
    await new Promise(r => setTimeout(r, 600));
    await io.pread(car, M + 5, 64);
    const { list, now } = await io.fetches();
    const lastGame = Math.max(...list.filter(f => !f.track || f.b === 0).map(f => f.t));
    assert.deepEqual(list.filter(f => f.track && f.b > 0), [], `read ahead while the car loaded: ${JSON.stringify(list)}`);
    assert.ok(now - lastGame < 1500);
    // then, quiet: the rest of the track, the first of it about 2 s after the car's last block
    let all;
    for (const t0 = performance.now(); performance.now() - t0 < 10_000;) {
      all = (await io.fetches()).list.filter(f => f.track && f.b > 0);
      if (all.length >= 5) break;
      await new Promise(r => setTimeout(r, 200));
    }
    assert.equal(all.length, 5, `the rest of the track read ahead: ${JSON.stringify(all)}`);
    const first = Math.min(...all.map(f => f.t));
    assert.ok(first - lastGame >= 1900 && first - lastGame < 3000, `read ahead from ${(first - lastGame).toFixed(0)} ms after the game's last block`);
  } finally { await io.stop(); }
});

test('a cache file that never opens: given up after 10 s, its reads come from the server, the log says so', { timeout: 60_000 }, async () => {
  const io = await start({ hangOpen: true });
  try {
    const fd = await io.open('CARS/A.BIN');
    const t0 = performance.now();
    assert.deepEqual(await io.pread(fd, 100, 64), expect(100, 64));
    const s = (performance.now() - t0) / 1000;
    assert.ok(s >= 9 && s < 20, `the first read came after ${s.toFixed(1)} s`);
    assert.ok(io.notes.some(n => /CARS\/A\.BIN: its cache file did not open in 10 s; this game reads it from the server/.test(n)), io.notes.join('\n'));
    // the file stays given up: the next read (another block) does not wait for its cache file again
    const t1 = performance.now();
    assert.deepEqual(await io.pread(fd, M + 5, 32), expect(M + 5, 32));
    assert.ok(performance.now() - t1 < 2000, 'the second read waited for the cache file again');
  } finally { await io.stop(); }
});

test('a read that never comes back: written in the log after 20 s, with its file and what it waits for; the loop runs on', { timeout: 60_000 }, async () => {
  const io = await start({ hangFetch: true });
  try {
    const fd = await io.open('CARS/A.BIN');
    void io.pread(fd, 0, 16).catch(() => {});        // never answered: the worker is stopped at the end
    const beat0 = Atomics.load(io.ctl, CTL_IO_BEAT);
    const t0 = performance.now();
    let line;
    while (!(line = io.notes.find(n => n.startsWith('[io] '))) && performance.now() - t0 < 40_000) await new Promise(r => setTimeout(r, 250));
    assert.ok(line, `no [io] line in 40 s:\n${io.notes.join('\n')}`);
    assert.match(line, /^\[io\] fd_pread of \/data\/CARS\/A\.BIN unanswered for 2\d s; waiting: CARS\/A\.BIN#0: the server for 2\d s/);
    assert.ok(Atomics.load(io.ctl, CTL_IO_BEAT) - beat0 > 100, 'the I/O worker\'s loop did not beat while the read waited');
  } finally { await io.stop(); }
});

test('a file of the save folder that will not open, the folder still there (Firefox 159, 2026-11-04): refused, said once in the log and once to the page, no recovery', { timeout: 60_000 }, async () => {
  const io = await start({ saveFail: '^registry\\.txt\\.tmp$' });
  try {
    // the runtime's registry writes it 29 times at a game's start (win32_ext.c reg_save): every one refused, at once
    const t0 = performance.now();
    for (let i = 0; i < 5; i++) assert.equal((await io.openSave('registry.txt.tmp', 1 | 8)).r, 2 /* E.ACCES */);
    assert.ok(performance.now() - t0 < 2000, 'the refused opens waited');
    assert.deepEqual(io.notes.filter(n => n.startsWith('[save]')), ['[save] a file of the save folder could not be opened: NotFoundError: Entry not found']);
    assert.deepEqual(io.msgs.filter(m => m.t === 'save-trouble' || m.t === 'save-gone'), [{ t: 'save-trouble', error: 'NotFoundError: Entry not found' }]);
    // the folder's other files still open, and their saves still tell the page
    const { r, fd } = await io.openSave('settings.txt', 1 | 8);
    assert.equal(r, 0);
    assert.equal(await io.call(1 /* FdClose */, fd), 0);
    assert.equal(io.msgs.filter(m => m.t === 'save-trouble').length, 1);
  } finally { await io.stop(); }
});

// Playing without saving (shell.ts askNoSave: the save folder did not answer, the player started anyway; review of
// 2f61632b): /save is in the I/O worker's memory.  On 2f61632b both fail: the game's writes went to the browser's storage.
const PAGE_NEWS = ['saved', 'removed', 'lost', 'save-gone', 'save-trouble'];

test('playing without saving: /save in memory, the save folder\'s files that read; the game\'s writes, new folders, renames and deletes stay there, the browser\'s storage untouched', { timeout: 60_000 }, async () => {
  const saveFiles = { 'settings.ini': '# mine\n', 'registry.txt': 'd A=1\n', 'NFS Underground 2/AAA/AAA': 'aaa', 'bad.bin': 'x' };
  const io = await start({ memory: true, readFail: 'bad.bin', saveFiles,
    memFiles: [{ path: 'registry.txt', text: 'd DEFAULT=1\n' }, { path: 'autotest-script.txt', text: 'quit\n', over: true }] });
  try {
    const before = await io.dump();
    // reads: the save folder's own (its registry, not the page's defaults), the page's where it gave none; one that did not read is not there
    assert.equal(await io.readAll('registry.txt'), 'd A=1\n');
    assert.equal(await io.readAll('autotest-script.txt'), 'quit\n');
    assert.equal(await io.readAll('NFS Underground 2/AAA/AAA'), 'aaa');
    assert.equal(await io.readAll('bad.bin'), 44 /* E.NOENT */);
    // the game's writes: a profile over its own, a new profile in a new folder, the registry by its .tmp, a delete
    await io.writeAll('NFS Underground 2/AAA/AAA', 'AAA2');
    assert.equal(await io.mkdir('NFS Underground 2/BBB'), 0);
    await io.writeAll('NFS Underground 2/BBB/BBB', 'bbb');
    await io.writeAll('registry.txt.tmp', 'd A=2\n');
    assert.equal(await io.rename('registry.txt.tmp', 'registry.txt'), 0);
    assert.equal(await io.unlink('settings.ini'), 0);
    assert.deepEqual([await io.readAll('NFS Underground 2/AAA/AAA'), await io.readAll('NFS Underground 2/BBB/BBB'), await io.readAll('registry.txt'),
      await io.readAll('settings.ini'), await io.readAll('registry.txt.tmp')], ['AAA2', 'bbb', 'd A=2\n', 44, 44]);
    // the browser's storage: as before, byte for byte; the page heard of no save (no "Progress saved", nothing for the cloud)
    assert.deepEqual(await io.dump(), before);
    assert.deepEqual(Object.fromEntries(Object.entries(before).filter(([k]) => k.startsWith('save/')).map(([k, v]) => [k.slice(5), v])), saveFiles);
    assert.deepEqual(io.msgs.filter(m => PAGE_NEWS.includes(m.t)), []);
    assert.ok(io.notes.some(n => /^\[save\] playing without saving: \/save is in this game's memory, 3 files of the save folder copied in .*, 1 that did not read left out; nothing is written/.test(n)), io.notes.join('\n'));
  } finally { await io.stop(); }
});

test('playing without saving in a browser whose storage gives nothing at all: the game starts, its /save the page\'s files alone', { timeout: 60_000 }, async () => {
  const io = await start({ memory: true, noStorage: true, memFiles: [{ path: 'registry.txt', text: 'd DEFAULT=1\n' }] });
  try {
    assert.equal(await io.readAll('registry.txt'), 'd DEFAULT=1\n');
    await io.writeAll('NFS Underground 2.txt', 'x');
    assert.equal(await io.readAll('NFS Underground 2.txt'), 'x');
    assert.deepEqual(io.msgs.filter(m => PAGE_NEWS.includes(m.t)), []);
  } finally { await io.stop(); }
});

// Review of 2f61632b, second round: Play without saving is offered where the storage does not answer, so its start must
// not wait for it for good (it gave no "ready" in 8 s, and the page a black stage), and what it copies into memory is
// what the game reads: no screenshots, covers or big files (40 screenshots of 4 MB were 160 MB beside the game's 1 GB).
// On de9b20bb both fail.

for (const hang of ['root', 'lookup', 'list', 'read'])
  test(`playing without saving where the storage never answers (${hang}): ready within its time, with the page's files alone`, { timeout: 30_000 }, async () => {
    const t0 = performance.now();
    const io = await start({ memory: true, hang, saveFiles: { 'settings.ini': '# mine\n', 'registry.txt': 'd A=1\n', 'NFS Underground 2/AAA/AAA': 'aaa' },
      memFiles: [{ path: 'registry.txt', text: 'd DEFAULT=1\n' }] });
    try {
      const s = (performance.now() - t0) / 1000;
      assert.ok(s < 9, `ready after ${s.toFixed(1)} s`);
      assert.deepEqual([await io.readAll('registry.txt'), await io.readAll('NFS Underground 2/AAA/AAA'), await io.readAll('settings.ini')], ['d DEFAULT=1\n', 44, 44]);
      assert.ok(io.notes.some(n => /^\[save\] playing without saving: the save folder did not answer within 2 s a call \(6 s in all\), so \/save is in this game's memory with the page's files alone/.test(n)), io.notes.join('\n'));
      await io.writeAll('registry.txt', 'd A=2\n');
      assert.equal(await io.readAll('registry.txt'), 'd A=2\n');
    } finally { await io.stop(); }
  });

test('playing without saving copies what the game reads: no screenshots, covers or big files, and says so', { timeout: 60_000 }, async () => {
  const io = await start({ memory: true, saveFiles: { 'NFS Underground 2/AAA/AAA': 'aaa', 'registry.txt': 'd A=1\n', 'nfsu2-shot.png': 'png',
    'NFS Underground 2/AAA DVD 1/AAA DVD 1': 'cover', 'big.bin': 'x'.repeat(3 << 20) } });
  try {
    assert.deepEqual([await io.readAll('NFS Underground 2/AAA/AAA'), await io.readAll('registry.txt')], ['aaa', 'd A=1\n']);
    assert.deepEqual([await io.readAll('nfsu2-shot.png'), await io.readAll('NFS Underground 2/AAA DVD 1/AAA DVD 1'), await io.readAll('big.bin')], [44, 44, 44]);
    assert.ok(io.notes.some(n => /2 files of the save folder copied in .*, 3 screenshots, covers or big files left out \(3\.0 MB\); nothing is written/.test(n)), io.notes.join('\n'));
  } finally { await io.stop(); }
});

// Review of 2f61632b, third round: a profile's folder comes whole or not at all (one without its GARAGE.DAT plays as
// another career).  On 2f44a283 it fails: the profile came without its GARAGE.DAT.
test('playing without saving: a profile whose GARAGE.DAT does not read, or with a file over the limit, is left out whole; the others come', { timeout: 60_000 }, async () => {
  const io = await start({ memory: true, readFail: 'GARAGE.DAT', saveFiles: { 'registry.txt': 'd A=1\n',
    'NFS Underground 2/AAA/AAA': 'aaa', 'NFS Underground 2/AAA/GARAGE.DAT': 'g', 'NFS Underground 2/BBB/BBB': 'bbb',
    'NFS Underground 2/CCC/CCC': 'ccc', 'NFS Underground 2/CCC/BIG.DAT': 'x'.repeat(3 << 20) } });
  try {
    assert.deepEqual([await io.readAll('NFS Underground 2/AAA/AAA'), await io.readAll('NFS Underground 2/CCC/CCC'),
      await io.readAll('NFS Underground 2/BBB/BBB'), await io.readAll('registry.txt')], [44, 44, 'bbb', 'd A=1\n']);
    assert.ok(io.notes.some(n => /2 files of the save folder copied in .*, 2 profiles left out whole \(a file of theirs did not read, or was too big\); nothing is written/.test(n)), io.notes.join('\n'));
  } finally { await io.stop(); }
});
