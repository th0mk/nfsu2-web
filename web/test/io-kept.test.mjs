// Reads of the game data from this device's cache straight into the game's memory (web/src/host/io-worker.ts
// readKept, the I/O worker that makes the memory where the page holds its buffer only), on the worker's own code in a
// worker thread over an in-memory OPFS and a made-up game folder on a made-up server, as io-stuck.test.mjs:
//   node --test web/test/io-kept.test.mjs
// - a read into several buffers, an empty one among them, through the blocks and then from the cache file: the same
//   bytes in each buffer (an empty buffer once ended the read there, the rest left unwritten)
// - a read over a block's end, both blocks kept or one; a read to the file's end, short
// - the worker that makes the memory sends its buffer, and a new one once the memory grew
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { Worker, MessageChannel } from 'node:worker_threads';
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
  let memory = opts.offPage ? null : new WebAssembly.Memory({ initial: 4, maximum: 4, shared: true });
  const ctlBuf = new SharedArrayBuffer(CTL_INTS * 4), rpc = new SharedArrayBuffer(RPC_SLOT * MAX_THREADS);
  const w = new Worker(world + bundle, { eval: true, workerData: opts });
  const notes = [], msgs = [];
  const ready = new Promise((ok, no) => w.on('message', m => {
    if (m.t === 'log') notes.push(m.s); else msgs.push(m);
    if (m.t === 'io-ready') ok();
    if (m.t === 'io-error') no(new Error(m.msg));
  }));
  w.on('error', e => notes.push(`worker error: ${e}`));
  w.postMessage({ ...(opts.offPage ? { tries: [{ initial: 4, maximum: 64 }] } : { memory }), ctl: ctlBuf, rpc, cfg: { args: [], env: {}, dataBase: '/data/', manifestUrl: '/data-manifest', saveDir: 'save',
    ...opts.memory ? { saves: 'memory', memFiles: opts.memFiles ?? [] } : {} } });
  await ready;
  if (opts.offPage) { const ch = new MessageChannel(); const got = new Promise(r => ch.port2.once("message", r)); w.postMessage({ t: "give", port: ch.port1, slot: 0 }, [ch.port1]); memory = await got; ch.port2.close(); }
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
  return { memory, w, notes, msgs, ctl, open, openSave, call, pread, readAll, writeAll, mkdir, unlink, rename, dump, fetches, stop: () => w.terminate() };
}
const expect = (pos, n) => Uint8Array.from({ length: n }, (_, k) => ((pos + k) * 7 + ((pos + k) >> 11)) & 255);

async function readv(io, fd, pos, lens) {
  const memory = io.memory, dv = new DataView(memory.buffer);
  let at = 120000;
  lens.forEach((n, i) => { dv.setUint32(8192 + 8 * i, at, true); dv.setUint32(8192 + 8 * i + 4, n, true); at += n + 64; });
  new Uint8Array(memory.buffer).fill(0xEE, 120000, at);
  assert.equal(await io.call(OP.FdPread, fd, 8192, lens.length, pos, 4300), 0);
  const got = dv.getUint32(4300, true);
  const parts = []; at = 120000;
  for (const n of lens) { parts.push(new Uint8Array(memory.buffer).slice(at, at + n)); at += n + 64; }
  return { got, parts };
}
const want = (pos, lens) => { const out = []; let p = pos; for (const n of lens) { out.push(expect(p, n)); p += n; } return out; };

for (const offPage of [false, true]) {
  test(`readv iovecs [64, 0, 64]: from the server, then from the cache file (offPage ${offPage})`, { timeout: 60_000 }, async () => {
    const io = await start({ offPage });
    try {
      const fd = await io.open('CARS/A.BIN');
      const lens = [64, 0, 64];
      const a = await readv(io, fd, 100, lens);       // blocks path (fetched, kept)
      assert.equal(a.got, 128); assert.deepEqual(a.parts, want(100, lens), 'blocks path');
      const b = await readv(io, fd, 100, lens);       // cache path (readKept)
      assert.equal(b.got, 128);
      assert.deepEqual(b.parts, want(100, lens), 'cache path (readKept)');
    } finally { await io.stop(); }
  });
  test(`a read over a block's end, both kept; one kept and one not (offPage ${offPage})`, { timeout: 60_000 }, async () => {
    const io = await start({ offPage });
    try {
      const fd = await io.open('CARS/A.BIN');
      assert.deepEqual(await io.pread(fd, 10, 16), expect(10, 16));          // block 0 kept
      const lens = [40, 3000];
      const a = await readv(io, fd, M - 20, lens);                            // block 1 not kept: through blocks
      assert.equal(a.got, 3040); assert.deepEqual(a.parts, want(M - 20, lens));
      const b = await readv(io, fd, M - 20, lens);                            // both kept: readKept
      assert.equal(b.got, 3040); assert.deepEqual(b.parts, want(M - 20, lens));
      // to the file's end: short
      const c = await readv(io, fd, SIZE - 10, [64]);
      assert.equal(c.got, 10); assert.deepEqual(c.parts[0].slice(0, 10), expect(SIZE - 10, 10));
      const d = await readv(io, fd, SIZE - 10, [64]);
      assert.equal(d.got, 10); assert.deepEqual(d.parts[0].slice(0, 10), expect(SIZE - 10, 10));
    } finally { await io.stop(); }
  });
}

test('offPage: memory message, then mem after the memory grows; latency', { timeout: 60_000 }, async () => {
  const io = await start({ offPage: true });
  try {
    const m0 = io.msgs.find(m => m.t === 'memory');
    assert.ok(m0 && m0.buf instanceof SharedArrayBuffer && m0.buf.byteLength === 4 * 65536, JSON.stringify(io.msgs.map(m => m.t)));
    const t0 = performance.now();
    io.memory.grow(2);
    while (!io.msgs.some(m => m.t === 'mem') && performance.now() - t0 < 2000) await new Promise(r => setTimeout(r, 2));
    const mm = io.msgs.find(m => m.t === 'mem');
    assert.ok(mm, 'no mem message');
    assert.equal(mm.buf.byteLength, 6 * 65536);
  } finally { await io.stop(); }
});
