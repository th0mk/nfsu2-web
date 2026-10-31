// DXT3 and DXT5 levels rewritten as ASTC 4x4 blocks (astc.ts dxtAlphaToAstc) here instead of on the page thread,
// which draws the game: the rewrite costs about four times what unpacking them did (25 against 7 ms a MB), over a
// second of a Most Wanted race load and every texture streamed while driving (astc-pool.ts sends the work).
// In: a batch of levels packed one after another (each bw * bh blocks of 16 bytes, rows of blocks bw * 16 bytes
// apart) and per level its offset, bw, bh and 1 for DXT5; out: the same buffer, each level rewritten in place (an
// ASTC block takes its DXT block's 16 bytes, and dxtAlphaToAstc reads a block whole before it writes it).

import { dxtAlphaToAstc } from './astc';

export interface ToAstc { id: number; buf: ArrayBuffer; items: Int32Array }
export type FromAstc = { ready: true } | { id: number; buf: ArrayBuffer; ms: number } | { id: number; error: string };

const post = (m: FromAstc, t: Transferable[] = []) => (self as unknown as Worker).postMessage(m, t);

onmessage = (e: MessageEvent<ToAstc>) => {
  const { id, buf, items } = e.data;
  const t0 = performance.now();
  try {
    const u8 = new Uint8Array(buf);
    for (let i = 0; i + 3 < items.length; i += 4) {
      const off = items[i], bw = items[i + 1], bh = items[i + 2];
      dxtAlphaToAstc(items[i + 3] !== 0, u8, off, bw * 16, bw, bh, u8.subarray(off, off + bw * bh * 16));
    }
    post({ id, buf, ms: performance.now() - t0 }, [buf]);
  } catch (x) { post({ id, error: String(x) }); }
};
post({ ready: true });
