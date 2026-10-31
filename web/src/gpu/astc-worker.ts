// DXT3 and DXT5 levels rewritten as ASTC 4x4 blocks (astc.ts dxtAlphaToAstc) here instead of on the page thread,
// which draws the game: the rewrite costs about four times what unpacking them did (25 against 7 ms a MB), over a
// second of a Most Wanted race load and every texture streamed while driving (astc-pool.ts sends the work).
// In: a batch of levels packed one after another (each bw * bh blocks of 16 bytes, rows of blocks bw * 16 bytes
// apart) and per level its offset, bw, bh and 1 for DXT5; out: the same buffer, each level rewritten over its DXT
// blocks (an ASTC block takes 16 bytes as a DXT block does).  A level is rewritten into a scratch array and copied
// over whole, so one whose rewrite throws goes back as it came, named in bad: the page thread rewrites it.

import { dxtAlphaToAstc } from './astc';

export interface ToAstc { id: number; buf: ArrayBuffer; items: Int32Array }
/** the batch back (bad: the levels, by index, whose rewrite threw, as they came; why: the first one's error), or
 *  error: it could not be sent back */
export type FromAstc = { ready: true } | { id: number; buf: ArrayBuffer; ms: number; bad?: number[]; why?: string } | { id: number; error: string };

const post = (m: FromAstc, t: Transferable[] = []) => (self as unknown as Worker).postMessage(m, t);
/** a level's blocks before they are copied over its DXT blocks (a bigger level gets an array of its own) */
const scratch = new Uint8Array(512 << 10);

onmessage = (e: MessageEvent<ToAstc>) => {
  const { id, buf, items } = e.data;
  const t0 = performance.now();
  const u8 = new Uint8Array(buf);
  let bad: number[] | undefined, why: string | undefined;
  for (let i = 0; i + 3 < items.length; i += 4) {
    const off = items[i], bw = items[i + 1], bh = items[i + 2];
    try { u8.set(dxtAlphaToAstc(items[i + 3] !== 0, u8, off, bw * 16, bw, bh, scratch), off); }
    catch (x) { if (!bad) { bad = []; why = String(x); } bad.push(i >> 2); }
  }
  try { post(bad ? { id, buf, ms: performance.now() - t0, bad, why } : { id, buf, ms: performance.now() - t0 }, [buf]); }
  catch (x) { post({ id, error: String(x) }); }
};
post({ ready: true });
