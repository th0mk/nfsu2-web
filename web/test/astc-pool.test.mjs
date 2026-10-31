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

/** the stand-in workers of the last session, in the order made; each keeps what is posted to it until answered */
let workers = [];
globalThis.Worker = class {
  constructor() { this.inbox = []; workers.push(this); setTimeout(() => this.onmessage?.({ data: { ready: true } }), 0); }
  postMessage(m) { this.inbox.push(m); }
  terminate() { this.inbox = []; }
  /** its oldest batch back, each level rewritten; error: the batch does not come back, the worker says why */
  answer({ error } = {}) {
    const m = this.inbox.shift();
    if (error) { this.onmessage({ data: { id: m.id, error } }); return; }
    const u8 = new Uint8Array(m.buf);
    for (let i = 0; i + 3 < m.items.length; i += 4) {
      const off = m.items[i], bw = m.items[i + 1], bh = m.items[i + 2];
      u8.set(dxtAlphaToAstc(m.items[i + 3] !== 0, u8, off, bw * 16, bw, bh), off);
    }
    this.onmessage({ data: { id: m.id, buf: m.buf, ms: 1 } });
  }
};
const settle = () => new Promise(r => setTimeout(r, 1));

const stub = (props = {}) => new Proxy(props, {
  get: (t, k) => k in t ? t[k] : (typeof k === 'symbol' || k === 'then') ? undefined : () => stub() });

/** a DXT5 block (a 4x4 level) of bytes from seed v, and its ASTC block as the page thread makes it */
function block(v) {
  let s = v * 7919 + 1;
  const b = new Uint8Array(16);
  for (let i = 0; i < 16; i++) b[i] = (s = (Math.imul(s, 1103515245) + 12345) >>> 0) >>> 24;
  return b;
}
const astc = v => Array.from(dxtAlphaToAstc(true, block(v), 0, 16, 1, 1));

const DXT5 = 0x35545844, CTL = 1024, RING = 65536, CAP = 1 << 20;
const Op = { TexCreate: 1, TexUpload: 2, TexFree: 3 };

/** an encoder on a GPU with ASTC and no BC, with n stand-in workers; uploads go into its resource queue, drain
 *  applies them, writes are the textures' writeTexture calls (texture label, level, the block) in order */
async function session(n = 2) {
  workers = [];
  const mem = new WebAssembly.Memory({ initial: 32, maximum: 32, shared: true });
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
    create: id => put(Op.TexCreate, [id, 4, 4, 1, DXT5, 0]),
    upload: (id, v) => put(Op.TexUpload, [id, 0, 4, 4, 16, 1], block(v)),
    free: id => put(Op.TexFree, [id]),
    /** apply the queue; true: all of it */
    drain: () => enc.drain(head) === head,
    /** the blocks written to texture id, in order */
    of: id => writes.filter(w => w.tex === `t${id}`).map(w => w.blocks),
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
