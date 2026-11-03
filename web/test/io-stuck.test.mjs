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
  createSyncAccessHandle() { return W.hangOpen && /\\.BIN\\./.test(this.name) ? new Promise(() => {}) : Promise.resolve(new Sync(this)); }
  async getFile() { const d = this.data; return { size: d.length, arrayBuffer: async () => d.slice().buffer }; }
}
class FDir {
  constructor(name) { this.kind = 'directory'; this.name = name; this.kids = new Map(); }
  async getDirectoryHandle(n, o = {}) { let k = this.kids.get(n); if (!k) { if (!o.create) throw new DOMException('', 'NotFoundError'); k = new FDir(n); this.kids.set(n, k); } return k; }
  async getFileHandle(n, o = {}) { let k = this.kids.get(n); if (!k) { if (!o.create) throw new DOMException('', 'NotFoundError'); k = new FFile(n); this.kids.set(n, k); } return k; }
  async removeEntry(n) { this.kids.delete(n); }
  async *entries() { for (const e of [...this.kids]) yield e; }
  async *keys() { for (const k of [...this.kids.keys()]) yield k; }
}
const root = new FDir('');
Object.defineProperty(globalThis, 'navigator', { value: { userAgent: 'Mozilla/5.0 (X11; Linux x86_64) Chrome/156.0.0.0', storage: { getDirectory: async () => root } }, configurable: true });
globalThis.fetch = async (url, o = {}) => {
  if (url === '/data-manifest') return new Response(JSON.stringify({ files: [{ p: 'CARS', d: 1 }, { p: 'CARS/A.BIN', s: SIZE, m: 7 }],
    blocks: { url: '/data-blocks/', size: ${M}, max: 4 } }), { status: 200 });
  if (W.hangFetch) return new Promise((_, no) => o.signal?.addEventListener('abort', () => no(new Error('aborted'))));
  const b = +/[?&]b=(\\d+)/.exec(url)[1];
  return new Response(file.slice(b * ${M}, Math.min(SIZE, (b + 1) * ${M})), { status: 200 });
};
parentPort.on('message', m => globalThis.onmessage({ data: m }));
`;

/** the worker, as a game process has it: its memory, control words and RPC slots; the notes it writes */
async function start(opts) {
  const memory = new WebAssembly.Memory({ initial: 4, maximum: 4, shared: true });
  const ctlBuf = new SharedArrayBuffer(CTL_INTS * 4), rpc = new SharedArrayBuffer(RPC_SLOT * MAX_THREADS);
  const w = new Worker(world + bundle, { eval: true, workerData: opts });
  const notes = [];
  const ready = new Promise((ok, no) => w.on('message', m => {
    if (m.t === 'log') notes.push(m.s);
    if (m.t === 'io-ready') ok();
    if (m.t === 'io-error') no(new Error(m.msg));
  }));
  w.on('error', e => notes.push(`worker error: ${e}`));
  w.postMessage({ memory, ctl: ctlBuf, rpc, cfg: { args: [], env: {}, dataBase: '/data/', manifestUrl: '/data-manifest', saveDir: 'save' } });
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
  return { w, notes, ctl, open, pread, stop: () => w.terminate() };
}
const expect = (pos, n) => Uint8Array.from({ length: n }, (_, k) => ((pos + k) * 7 + ((pos + k) >> 11)) & 255);

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
