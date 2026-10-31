// The ASTC workers (astc-worker.ts): DXT3 and DXT5 levels the encoder would rewrite as ASTC 4x4 on the page thread
// (TRACKER M6-59) are copied out of the resource queue into batches here, rewritten in a worker, and handed back in
// the order they were queued, each to its texture's writeTexture.  The page thread only copies (a fraction of a
// millisecond a MB, against 25 for the rewrite).  A frame that samples a texture with a level still out waits for it
// (index.ts pump: Encoder.framePending), so no frame draws a level that has not gone up yet, as before; the others
// (a loading screen's, while the track's textures load) go on.
// The blocks are the ones the page thread made: the same function, each level apart.

import type { FromAstc, ToAstc } from './astc-worker';
import { dxtAlphaToAstc } from './astc';

/** a level back from a worker, to its texture (the encoder's texture, level and face); blocks null: the level is lost
 *  (a worker failed) */
export type AstcDone<R> = (ref: R, blocks: Uint8Array | null, off: number, bw: number, bh: number) => void;

interface Batch<R> { id: number; buf: ArrayBuffer; used: number; items: number[]; refs: R[]; back?: ArrayBuffer; worker: number;
  src?: Uint8Array }
interface Slot { w: Worker; ready: boolean; dead: boolean; out: number }

/** a batch is sent when it holds this much (a big level goes alone): several at once spread over the workers */
const BATCH = 512 << 10;

export class AstcPool<R> {
  private slots: Slot[] = [];
  private sent: Batch<R>[] = [];          // in the order sent: applied from the front, each when it is back
  private cur: Batch<R> | null = null;
  private spare: ArrayBuffer[] = [];
  private next = 0;
  private gone = false;
  /** levels and bytes rewritten in the workers, their time there (ms, summed over the workers), and the page thread's
   *  time handing them to their textures */
  levels = 0; bytes = 0; workerMs = 0; applyMs = 0;
  /** diagnostics (verify): each level rewritten here too from a copy of what was sent, and compared byte for byte
   *  with the worker's: levels compared, levels that differ */
  verify: { levels: number; differ: number } | null = null;

  constructor(n: number, url: string, private done: AstcDone<R>, private idle: () => void, private log: (s: string) => void) {
    for (let i = 0; i < n; i++) {
      let w: Worker;
      try { w = new Worker(url, { type: 'module', name: `astc-${i}` }); } catch (x) { this.log(`[gpu] ASTC worker ${i}: ${String(x)}`); continue; }
      const s: Slot = { w, ready: false, dead: false, out: 0 };
      w.onmessage = (e: MessageEvent<FromAstc>) => this.message(s, e.data);
      w.onerror = e => this.fail(s, e.message || 'the worker failed');
      this.slots.push(s);
    }
  }

  /** a worker has started: levels go to the workers from now on (before that the encoder rewrites them itself) */
  get usable(): boolean { return !this.gone && this.slots.some(s => s.ready && !s.dead); }
  /** batches out, or a level not sent yet: the next frame waits */
  get busy(): boolean { return this.sent.length > 0 || !!this.cur; }

  /** one level (bw x bh DXT3 or DXT5 blocks at off in src, rows pitch bytes apart), copied into the batch */
  add(ref: R, dxt5: boolean, src: Uint8Array, off: number, pitch: number, bw: number, bh: number): void {
    const n = bw * bh * 16, row = bw * 16;
    if (this.cur && this.cur.used + n > this.cur.buf.byteLength) this.flush();
    if (!this.cur) {
      const cap = Math.max(BATCH, n);
      const k = this.spare.findIndex(b => b.byteLength >= cap);
      const buf = k >= 0 ? this.spare.splice(k, 1)[0] : new ArrayBuffer(cap);
      this.cur = { id: this.next++, buf, used: 0, items: [], refs: [], worker: -1 };
    }
    const b = this.cur, dst = new Uint8Array(b.buf, b.used, n);
    if (pitch === row) dst.set(src.subarray(off, off + n));
    else for (let y = 0; y < bh; y++) dst.set(src.subarray(off + y * pitch, off + y * pitch + row), y * row);
    b.items.push(b.used, bw, bh, dxt5 ? 1 : 0); b.refs.push(ref);
    b.used += n;
    if (b.used >= BATCH) this.flush();
  }

  /** send the batch being filled to the worker with the fewest out */
  flush(): void {
    const b = this.cur;
    if (!b) return;
    this.cur = null;
    let best = -1;
    for (let i = 0; i < this.slots.length; i++) {
      const s = this.slots[i];
      if (s.ready && !s.dead && (best < 0 || s.out < this.slots[best].out)) best = i;
    }
    if (best < 0) {
      // no worker left: rewritten here, as the encoder does without them
      const u8 = new Uint8Array(b.buf);
      for (let i = 0; i < b.items.length; i += 4) {
        const off = b.items[i], bw = b.items[i + 1], bh = b.items[i + 2];
        dxtAlphaToAstc(b.items[i + 3] !== 0, u8, off, bw * 16, bw, bh, u8.subarray(off, off + bw * bh * 16));
      }
      b.back = b.buf; this.sent.push(b); this.apply();
      return;
    }
    b.worker = best; this.slots[best].out++;
    if (this.verify) b.src = new Uint8Array(b.buf.slice(0, b.used));
    this.sent.push(b);
    const msg: ToAstc = { id: b.id, buf: b.buf, items: Int32Array.from(b.items) };
    this.slots[best].w.postMessage(msg, [b.buf]);
  }

  private message(s: Slot, m: FromAstc): void {
    if ('ready' in m) { s.ready = true; return; }
    s.out--;
    const b = this.sent.find(x => x.id === m.id);
    if (!b) return;
    if ('error' in m) this.lost(b, m.error);
    else { b.back = m.buf; this.workerMs += m.ms; }
    this.apply();
  }

  /** a batch that will not come back: its levels stay as they are (a bug: said once) and the workers are left alone,
   *  the encoder rewrites on the page thread again */
  private lost(b: Batch<R>, why: string): void {
    if (!this.gone) this.log(`[gpu] ASTC worker: ${why}: ${b.refs.length} level(s) lost, DXT3 and DXT5 rewritten on the page thread from now on`);
    this.gone = true;
    b.back = new ArrayBuffer(0);
  }

  private fail(s: Slot, why: string): void {
    s.dead = true;
    const i = this.slots.indexOf(s);
    for (const b of this.sent) if (b.worker === i && !b.back) this.lost(b, why);
    if (!this.slots.some(x => !x.dead)) this.gone = true;
    this.apply();
  }

  /** the batches back at the front, in order, to their textures */
  private apply(): void {
    const t0 = performance.now();
    while (this.sent.length && this.sent[0].back) {
      const b = this.sent.shift()!, u8 = b.back!.byteLength ? new Uint8Array(b.back!) : null;
      for (let i = 0, k = 0; k < b.refs.length; i += 4, k++) {
        const off = b.items[i], bw = b.items[i + 1], bh = b.items[i + 2];
        if (u8 && this.verify && b.src) {
          const want = dxtAlphaToAstc(b.items[i + 3] !== 0, b.src, off, bw * 16, bw, bh);
          let same = true;
          for (let j = 0; j < want.length && same; j++) same = want[j] === u8[off + j];
          this.verify.levels++; if (!same) this.verify.differ++;
        }
        this.done(b.refs[k], u8, off, bw, bh);
        if (u8) { this.levels++; this.bytes += bw * bh * 16; }
      }
      if (b.back!.byteLength && this.spare.length < 6) this.spare.push(b.back!);
    }
    this.applyMs += performance.now() - t0;
    if (!this.busy) this.idle();
  }

  terminate(): void {
    this.gone = true;
    for (const s of this.slots) s.w.terminate();
    this.sent = []; this.cur = null; this.spare = [];
  }
}
