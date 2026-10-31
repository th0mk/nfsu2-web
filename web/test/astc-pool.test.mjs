// The ASTC workers (web/src/gpu/astc-pool.ts) as the encoder uses them (encoder.ts drain, texUpload): DXT5 levels
// written into a resource queue, drained by an encoder on a fake GPUDevice without texture-compression-bc, rewritten
// by stand-in workers that answer when a test says (as astc-worker.ts does: the same function) and written to their
// textures in the order they were uploaded, whatever the workers do.
//   node --test web/test/astc-pool.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const out = await esbuild.build({ stdin: { contents: "export { Encoder } from './encoder'; export * as P from './proto'; export { dxtAlphaToAstc } from './astc';",
  resolveDir: path.join(HERE, '../src/gpu'), loader: 'ts' }, bundle: true, write: false, format: 'esm', platform: 'neutral' });
const { Encoder, P, dxtAlphaToAstc } = await import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));

globalThis.GPUShaderStage ??= { VERTEX: 1, FRAGMENT: 2, COMPUTE: 4 };
globalThis.GPUBufferUsage ??= { MAP_READ: 1, MAP_WRITE: 2, COPY_SRC: 4, COPY_DST: 8, INDEX: 16, VERTEX: 32, UNIFORM: 64, STORAGE: 128,
  INDIRECT: 256, QUERY_RESOLVE: 512 };
globalThis.GPUTextureUsage ??= { COPY_SRC: 1, COPY_DST: 2, TEXTURE_BINDING: 4, STORAGE_BINDING: 8, RENDER_ATTACHMENT: 16 };

/** the stand-in workers of the last session, in the order made; each keeps what is posted to it until answered;
 *  the next noLoad of them cannot be loaded (onerror, as a worker script the network does not give) */
let workers = [], noLoad = 0;
globalThis.Worker = class {
  constructor() {
    this.inbox = []; workers.push(this);
    if (noLoad > 0) { noLoad--; setTimeout(() => this.onerror?.({ message: 'NetworkError' }), 0); }
    else setTimeout(() => this.onmessage?.({ data: { ready: true } }), 0);
  }
  postMessage(m) { this.inbox.push(m); }
  terminate() { this.inbox = []; }
  /** its oldest batch back, each level rewritten (those in bad left as they came, as a rewrite that threw); error:
   *  the batch does not come back, the worker says why */
  answer({ error, bad } = {}) {
    const m = this.inbox.shift();
    if (error) { this.onmessage({ data: { id: m.id, error } }); return; }
    const u8 = new Uint8Array(m.buf);
    for (let i = 0; i + 3 < m.items.length; i += 4) {
      if (bad?.includes(i >> 2)) continue;
      const off = m.items[i], bw = m.items[i + 1], bh = m.items[i + 2];
      u8.set(dxtAlphaToAstc(m.items[i + 3] !== 0, u8, off, bw * 16, bw, bh), off);
    }
    this.onmessage({ data: { id: m.id, buf: m.buf, ms: 1, ...(bad ? { bad, why: 'RangeError: test' } : {}) } });
  }
  /** it dies (onerror) with what it holds */
  crash(why) { this.inbox = []; this.onerror({ message: why }); }
};
const settle = () => new Promise(r => setTimeout(r, 1));

const stub = (props = {}) => new Proxy(props, {
  get: (t, k) => k in t ? t[k] : (typeof k === 'symbol' || k === 'then') ? undefined : () => stub() });

/** n bytes of DXT5 blocks from seed v (16: a 4x4 level), and a 4x4 level's ASTC block as the page thread makes it */
function block(v, n = 16) {
  let s = v * 7919 + 1;
  const b = new Uint8Array(n);
  for (let i = 0; i < n; i++) b[i] = (s = (Math.imul(s, 1103515245) + 12345) >>> 0) >>> 24;
  return b;
}
const astc = v => Array.from(dxtAlphaToAstc(true, block(v), 0, 16, 1, 1));

const DXT5 = 0x35545844, CTL = 1024, RING = 65536, CAP = 4 << 20;
const Op = { TexCreate: 1, TexUpload: 2 };

/** an encoder on a GPU with ASTC and no BC, with n stand-in workers; uploads go into its resource queue, drain
 *  applies them, writes are the textures' writeTexture calls (texture label, level, the block) in order */
async function session(n = 2) {
  workers = [];
  const mem = new WebAssembly.Memory({ initial: 80, maximum: 80, shared: true });   // 5 MB: the queue's 4 at 64 KB
  const u32 = new Uint32Array(mem.buffer), u8 = new Uint8Array(mem.buffer);
  u32[(CTL >> 2) + P.C.rqPtr] = RING; u32[(CTL >> 2) + P.C.rqCap] = CAP;
  const writes = [], log = [];
  const device = stub({
    features: new Set(['texture-compression-astc']),
    queue: stub({ writeTexture: (dst, data, layout) => {
      const o = layout.offset ?? 0;
      writes.push({ tex: dst.texture.label, level: dst.mipLevel, blocks: Array.from(data.subarray(o, o + 16)) });
    } }),
    createTexture: d => stub({ label: d.label, createView: () => stub(), destroy() {} }),
    createShaderModule: d => stub({ label: d.label, getCompilationInfo: async () => ({ messages: [] }) }),
    pushErrorScope() {}, popErrorScope: async () => null,
  });
  const enc = new Encoder(device, mem, CTL, s => log.push(s));
  let head = 0;
  const put = (op, a, data = new Uint8Array(0)) => {
    const len = (P.CMD_HDR + data.length + 15) & ~15, p = RING + head, w = p >> 2;
    u32[w] = op; u32[w + 1] = len;
    for (let i = 0; i < 6; i++) u32[w + 2 + i] = a[i] ?? 0;
    u8.set(data, p + P.CMD_HDR); head += len;
  };
  const s = {
    enc, writes, log, backs: 0,
    pool: () => enc.astcPool,
    /** texture id, size x size, one level */
    create: (id, size = 4) => put(Op.TexCreate, [id, size, size, 1, DXT5, 0]),
    /** its level 0 (of that size) from seed v */
    upload: (id, v, size = 4) => { const bw = size >> 2; put(Op.TexUpload, [id, 0, size, size, bw * 16, bw], block(v, bw * bw * 16)); },
    /** apply the queue; true: all of it */
    drain: () => enc.drain(head) === head,
    /** the blocks written to texture id, in order */
    of: id => writes.filter(w => w.tex === `t${id}`).map(w => w.blocks),
    /** frame 0: a draw for each texture id, sampling it on stage 0 */
    frame: ids => {
      const fw = (CTL >> 2) + P.C.frame, at = 16384;
      u32[fw + P.F.dn] = ids.length; u32[fw + P.F.draws] = at;
      ids.forEach((id, i) => {
        const w = (at >> 2) + i * P.DRAW_WORDS;
        u32.fill(0, w, w + P.DRAW_WORDS); u32[w + P.D.kind] = P.Kind.Draw; u32[w + P.D.tex] = id;
      });
    },
  };
  enc.startAstcWorkers(n, 'astc-worker.js', () => s.backs++);
  await settle();
  return s;
}

test('the blocks of different uploads differ (what the other tests tell apart)', () => {
  const all = [1, 2, 3, 4, 9].map(v => astc(v).join());
  assert.equal(new Set(all).size, all.length);
});

test('batches back out of order go up in the order queued', async () => {
  const s = await session(2);
  s.create(1); s.upload(1, 1); s.drain();
  s.upload(1, 2); s.drain();
  assert.equal(s.of(1).length, 0, 'nothing written while out');
  workers[1].answer(); workers[0].answer();
  assert.deepEqual(s.of(1), [astc(1), astc(2)]);
  assert.equal(s.pool().busy, false);
});

test('a worker fails while another holds a level: the level uploaded again goes up after it', async () => {
  const s = await session(2);
  s.create(1); s.create(2);
  s.upload(2, 9); s.drain();                     // worker 0
  s.upload(1, 1); s.drain();                     // worker 1: texture 1's level 0, the first upload
  workers[0].answer({ error: 'boom' });          // worker 0 fails: the page thread rewrites from now on
  assert.ok(s.log.some(l => /ASTC worker: boom/.test(l)), 'said');
  s.upload(1, 2); s.drain();                     // the same level again, while the first is still out
  assert.equal(s.of(1).length, 0, 'it waits for the one out');
  workers[1].answer();
  assert.deepEqual(s.of(1), [astc(1), astc(2)], 'the first upload, then the second');
  // nothing out: the page thread writes at once, as without workers
  s.upload(1, 3); s.drain();
  assert.deepEqual(s.of(1), [astc(1), astc(2), astc(3)]);
  assert.equal(workers[1].inbox.length, 0, 'not sent to the worker left');
});

test('levels whose rewrite threw in a worker are rewritten on the page thread, in their turn', async () => {
  const s = await session(2);
  s.create(1); s.create(2); s.create(3);
  s.upload(1, 1); s.upload(2, 2); s.drain();     // worker 0: one batch of two levels
  s.upload(3, 3); s.drain();                     // worker 1
  workers[1].answer();                           // back first: it waits for the batch before it
  assert.equal(s.of(1).length + s.of(2).length + s.of(3).length, 0);
  workers[0].answer({ bad: [0] });               // texture 1's level threw in the worker
  assert.deepEqual([s.of(1), s.of(2), s.of(3)], [[astc(1)], [astc(2)], [astc(3)]]);
  assert.deepEqual(s.writes.map(w => w.tex).filter(t => /^t\d/.test(t)), ['t1', 't2', 't3'], 'in the order uploaded');
  assert.ok(s.log.some(l => /RangeError: test: 1 level\(s\) rewritten on the page thread/.test(l)), s.log.join('\n'));
  // the page thread from now on
  s.upload(1, 4); s.drain();
  assert.deepEqual(s.of(1), [astc(1), astc(4)]);
  assert.equal(workers[0].inbox.length + workers[1].inbox.length, 0);
});

test('a worker that dies with a batch: its levels are lost and said, nothing waits for them, later uploads go up', async () => {
  const s = await session(2);
  s.create(1); s.create(2);
  s.upload(1, 1); s.drain();                     // worker 0
  s.upload(2, 2); s.drain();                     // worker 1
  workers[0].crash('gone');
  assert.ok(s.log.some(l => /ASTC worker: gone: 1 level\(s\) lost/.test(l)), s.log.join('\n'));
  s.upload(1, 3); s.drain();                     // behind worker 1's batch
  workers[1].answer();
  assert.deepEqual([s.of(1), s.of(2)], [[astc(3)], [astc(2)]]);
  assert.equal(s.pool().busy, false);
});

test('a frame waits only for its own textures: every batch handed on pumps again', async () => {
  const s = await session(2);
  s.create(1); s.create(2);
  s.upload(1, 1); s.drain();                     // worker 0
  s.upload(2, 2); s.drain();                     // worker 1
  s.frame([1]);                                  // a frame that samples texture 1
  assert.equal(s.enc.framePending(0), true, 'its level is out');
  workers[0].answer();                           // texture 1's level is up, texture 2's still out
  assert.equal(s.enc.framePending(0), false);
  assert.equal(s.pool().busy, true);
  assert.equal(s.backs, 1, 'pumped again with a batch still out');
  workers[1].answer();
  assert.equal(s.backs, 2);
  s.frame([1, 2]);
  assert.equal(s.enc.framePending(0), false);
});

test('at most outMax bytes out: the queue waits, and goes on in order as batches come back', async () => {
  const s = await session(2);
  s.pool().outMax = 32;                          // two 4x4 levels
  s.create(1);
  for (let v = 1; v <= 5; v++) s.upload(1, v);
  assert.equal(s.drain(), false, 'stopped short');
  assert.equal(workers[0].inbox.length + workers[1].inbox.length, 1, 'one batch out');
  assert.equal(s.drain(), false, 'still full');
  const answerAll = () => { for (const w of workers) while (w.inbox.length) w.answer(); };
  for (let rounds = 0; !s.drain(); rounds++) { assert.ok(rounds < 5); answerAll(); }
  answerAll();
  assert.deepEqual(s.of(1), [1, 2, 3, 4, 5].map(astc));
  assert.equal(s.pool().full, false);
  assert.ok(s.enc.astcWork.holds >= 3, `${s.enc.astcWork.holds}`);
});

test('spare buffers: batch-sized ones only, at most 6; a big level\'s own buffer is let go', async () => {
  const s = await session(1);
  const BATCH = 512 << 10;
  s.create(1, 768); s.create(2);
  s.upload(1, 7, 768); s.drain();                // 192 x 192 blocks: 576 KB, a buffer of its own
  workers[0].answer();
  assert.equal(s.of(1).length, 1);
  assert.deepEqual(s.pool().spare.map(b => b.byteLength), [], 'not kept');
  for (let v = 1; v <= 8; v++) { s.upload(2, v); s.drain(); }   // 8 batches out at once
  while (workers[0].inbox.length) workers[0].answer();
  assert.deepEqual(s.of(2), [1, 2, 3, 4, 5, 6, 7, 8].map(astc));
  assert.deepEqual(s.pool().spare.map(b => b.byteLength), Array(6).fill(BATCH));
});

test('the workers are said once one has started; one that cannot be loaded says so, and the page thread rewrites', async () => {
  const a = await session(2);
  assert.equal(a.log.filter(l => /\[gpu\] 2 ASTC worker\(s\) rewrite DXT3 and DXT5 off the page thread/.test(l)).length, 1, a.log.join('\n'));
  noLoad = 2;
  const s = await session(2);
  assert.ok(!s.log.some(l => /off the page thread/.test(l)), s.log.join('\n'));
  assert.ok(s.log.some(l => /ASTC worker 0 did not start: NetworkError$/.test(l)), s.log.join('\n'));
  assert.ok(s.log.some(l => /ASTC worker 1 did not start: NetworkError; DXT3 and DXT5 rewritten on the page thread/.test(l)), s.log.join('\n'));
  s.create(1); s.upload(1, 1); s.drain();
  assert.deepEqual(s.of(1), [astc(1)], 'written at once');
});
