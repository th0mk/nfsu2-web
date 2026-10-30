// DXT1 (BC1) blocks rewritten as ASTC 4x4 blocks, for a GPU without texture-compression-bc that has ASTC (Apple's:
// iPhones, iPads).  Unpacked to BGRA8 (dxt.ts) a DXT1 texture takes 8x its size; as ASTC 4x4 it takes 2x, and a
// phone's memory is what runs out (free roam: 159 MB of opaque DXT1 unpacked, ~40 MB as ASTC).
//
// A DXT1 block is two RGB565 colours and a 2-bit index per texel.  ASTC can say the same in one partition: colour
// endpoint mode 8 (LDR RGB direct) at 8 bits a value keeps both colours exactly (expanded to 8 bits as BC1 does), and
// the weights pick the points between them:
//   four colours (c0 > c1): weights of 2 bits, 0 21 43 64 of 64, against BC1's thirds: at most 1.3 of 255 off;
//   three colours (c0 <= c1): trit weights, 0 32 64 of 64: the midpoint exactly.
// The fourth index of a three-colour block is transparent black, which one plane cannot give: such a block goes into
// a texture unpacked instead (dxt1HasPunch, the encoder's choice at a texture's first upload), or, met in a later
// level of a texture already ASTC, into a dual-plane block (endpoint mode 12, RGBA at 7 bits, alpha its own plane):
// the alpha is right, the colour under a transparent texel the darker endpoint, not black.
//
// Layout (Khronos Data Format spec, ASTC): bits 0-10 the block mode, 11-12 partitions - 1, 13-16 the endpoint mode,
// then the endpoint values from bit 17 up; the weights from bit 127 down (bit i of their sequence at bit 127 - i), a
// dual-plane block's colour component selector just below them.
//   0x042: a 4x4 grid of 2-bit weights, one plane          0x051: a 4x4 grid of trit weights, one plane
//   0x451: 0x051 with two planes (32 weights, interleaved: plane 0 then plane 1 for each texel)
// The endpoint range is not stored: the decoder takes the largest whose values fit in the bits the weights leave
// (8 bits for 6 values in both one-plane modes; 7 bits for the 8 of the dual-plane one: 128 - 17 - 2 - 52 = 57).
import { C5, C6 } from './dxt';

/** trit quintuples (t0 + 3 t1 + 9 t2 + 27 t3 + 81 t4) to their 8-bit packing, from the spec's decoding */
const TRITS = new Uint8Array(243);
{
  const seen = new Uint8Array(243);
  for (let T = 255; T >= 0; T--) {         // downwards: the smallest packing wins (a short last group needs it)
    const b = (i: number, n = 1) => (T >> i) & ((1 << n) - 1);
    let C: number, t3: number, t4: number;
    if (b(2, 3) === 7) { C = b(5, 3) << 2 | b(0, 2); t4 = 2; t3 = 2; }
    else { C = b(0, 5); if (b(5, 2) === 3) { t4 = 2; t3 = b(7); } else { t4 = b(7); t3 = b(5, 2); } }
    let t0: number, t1: number, t2: number;
    const c = (i: number, n = 1) => (C >> i) & ((1 << n) - 1);
    if (c(0, 2) === 3) { t2 = 2; t1 = c(4); t0 = c(3) << 1 | (c(2) & ~c(3) & 1); }
    else if (c(2, 2) === 3) { t2 = 2; t1 = 2; t0 = c(0, 2); }
    else { t2 = c(4); t1 = c(2, 2); t0 = c(1) << 1 | (c(0) & ~c(1) & 1); }
    const k = t0 + 3 * t1 + 9 * t2 + 27 * t3 + 81 * t4;
    TRITS[k] = T; seen[k] = 1;
  }
  if (seen.indexOf(0) >= 0) throw new Error('astc: trit table incomplete');
}

// a block being built: four 32-bit words, low bits first
const blk = new Uint32Array(4), wts = new Uint8Array(32);
/** v (below 2 ** n, n at most 32) into the block at bit pos */
function put(pos: number, n: number, v: number) {
  const w = pos >> 5, b = pos & 31;
  blk[w] |= v << b;
  if (b + n > 32) blk[w + 1] |= v >>> (32 - b);
}
/** the weights' bits from the top down: 2 bits each, or trits packed 5 to 8 bits (a short last group, its bits only) */
function putWeights(n: number, trits: boolean) {
  let pos = 127;
  const top = (v: number, bits: number) => { for (let i = 0; i < bits; i++, pos--) if ((v >> i) & 1) blk[pos >> 5] |= 1 << (pos & 31); };
  if (!trits) { for (let i = 0; i < n; i++) top(wts[i], 2); return pos; }
  for (let g = 0; g < n; g += 5) {
    const t = (i: number) => g + i < n ? wts[g + i] : 0;
    const k = t(0) + 3 * t(1) + 9 * t(2) + 27 * t(3) + 81 * t(4);
    top(TRITS[k], [0, 2, 4, 5, 7, 8][Math.min(5, n - g)]);
  }
  return pos;
}

function rev32(x: number): number {
  x = (x >>> 1 & 0x55555555) | (x & 0x55555555) << 1;
  x = (x >>> 2 & 0x33333333) | (x & 0x33333333) << 2;
  x = (x >>> 4 & 0x0F0F0F0F) | (x & 0x0F0F0F0F) << 4;
  x = (x >>> 8 & 0x00FF00FF) | (x & 0x00FF00FF) << 8;
  return x >>> 16 | x << 16;
}

/** whether any block of a DXT1 level (bw x bh blocks, pitch bytes a row of blocks) has a transparent texel */
export function dxt1HasPunch(src: Uint8Array, off: number, pitch: number, bw: number, bh: number): boolean {
  for (let by = 0; by < bh; by++) {
    for (let bx = 0; bx < bw; bx++) {
      const b = off + by * pitch + bx * 8;
      if ((src[b] | src[b + 1] << 8) > (src[b + 2] | src[b + 3] << 8)) continue;
      const idx = (src[b + 4] | src[b + 5] << 8 | src[b + 6] << 16 | src[b + 7] << 24) >>> 0;
      // a 2-bit index 3: both bits set
      if ((idx & (idx >>> 1) & 0x55555555) !== 0) return true;
    }
  }
  return false;
}

const W3 = [0, 2, 1, 0];   // a three-colour block's codes to trit weights: c0 = 0, c1 = 2, the midpoint 1
/** an 8-bit value at 7 bits, as the decoder unquantizes it (v << 1 | v >> 6) */
const q7 = (c: number) => (c & ~1) | (c >> 7);

/** One DXT1 level (bw x bh blocks, pitch bytes a row of blocks) as ASTC 4x4 blocks, 16 bytes each, bw * 16 bytes a
 *  row; into the start of `into` when that has room, else into a new array.  punch counts the blocks that needed two
 *  planes (a transparent texel). */
export function dxt1ToAstc(src: Uint8Array, off: number, pitch: number, bw: number, bh: number, into?: Uint8Array,
  punch?: { blocks: number }): Uint8Array {
  const n = bw * bh * 16, out = into && into.length >= n ? into.subarray(0, n) : new Uint8Array(n);
  const o32 = new Uint32Array(out.buffer, out.byteOffset, n >> 2);
  for (let by = 0; by < bh; by++) {
    for (let bx = 0; bx < bw; bx++) {
      const b = off + by * pitch + bx * 8;
      const c0 = src[b] | src[b + 1] << 8, c1 = src[b + 2] | src[b + 3] << 8;
      const idx = (src[b + 4] | src[b + 5] << 8 | src[b + 6] << 16 | src[b + 7] << 24) >>> 0;
      let r0 = C5[c0 >> 11], g0 = C6[(c0 >> 5) & 63], b0 = C5[c0 & 31];
      let r1 = C5[c1 >> 11], g1 = C6[(c1 >> 5) & 63], b1 = C5[c1 & 31];
      const four = c0 > c1, holes = !four && (idx & (idx >>> 1) & 0x55555555) !== 0;
      // a dual-plane block's endpoints at 7 bits: the decoder compares (and gives) what they unquantize to
      if (holes) { r0 = q7(r0); g0 = q7(g0); b0 = q7(b0); r1 = q7(r1); g1 = q7(g1); b1 = q7(b1); }
      // the decoder wants the second endpoint's sum the larger (else it swaps them and blends blue in): swap here,
      // and turn the weights round
      const swap = r1 + g1 + b1 < r0 + g0 + b0;
      if (swap) { let x = r0; r0 = r1; r1 = x; x = g0; g0 = g1; g1 = x; x = b0; b0 = b1; b1 = x; }
      const o = (by * bw + bx) * 4;
      if (!holes) {
        // the weights' sequence, lowest bit first (at most 32 bits: the top word of the block, its bits reversed)
        let ws: number;
        if (four) {
          // codes 0 1 2 3 to weights 0 3 1 2, all 16 at once: high bit the code's low bit, low bit the two bits' xor
          const lo = idx & 0x55555555, hi = (idx >>> 1) & 0x55555555;
          ws = swap ? ~(lo << 1 | (lo ^ hi)) : lo << 1 | (lo ^ hi);
        } else {
          for (let i = 0; i < 16; i++) { const w = W3[(idx >>> 2 * i) & 3]; wts[i] = swap ? 2 - w : w; }
          const g = (i: number) => TRITS[wts[i] + 3 * wts[i + 1] + 9 * wts[i + 2] + 27 * wts[i + 3] + 81 * wts[i + 4]];
          ws = g(0) | g(5) << 8 | g(10) << 16 | TRITS[wts[15]] << 24;
        }
        // block mode, one partition, endpoint mode 8, then r0 r1 g0 g1 b0 b1 at 8 bits from bit 17
        o32[o] = (four ? 0x042 : 0x051) | 8 << 13 | r0 << 17 | r1 << 25;
        o32[o + 1] = r1 >> 7 | g0 << 1 | g1 << 9 | b0 << 17 | b1 << 25;
        o32[o + 2] = b1 >> 7;
        o32[o + 3] = rev32(ws);
        continue;
      }
      // alpha 255 at the first endpoint, 0 at the second; a transparent texel takes the first's colour (the darker)
      blk.fill(0);
      for (let i = 0; i < 16; i++) {
        const k = (idx >>> 2 * i) & 3, w = W3[k];
        wts[2 * i] = k === 3 ? 0 : swap ? 2 - w : w;
        wts[2 * i + 1] = k === 3 ? 2 : 0;
      }
      put(0, 11, 0x451); put(13, 4, 12);
      const v = [r0, r1, g0, g1, b0, b1, 255, 0];
      for (let i = 0; i < 8; i++) put(17 + 7 * i, 7, v[i] >> 1);   // q7 values: >> 1 is their 7 bits
      const below = putWeights(32, true);
      put(below - 1, 2, 3);                  // the second plane is alpha
      if (punch) punch.blocks++;
      o32[o] = blk[0]; o32[o + 1] = blk[1]; o32[o + 2] = blk[2]; o32[o + 3] = blk[3];
    }
  }
  return out;
}

// DXT3 and DXT5 (BC2, BC3): a DXT1-like colour block (always four colours) after 8 bytes of alpha, 16 explicit 4-bit
// values or two endpoints and 3-bit codes.  Unpacked to BGRA8 they take 4x their size; as ASTC 4x4, 1x.  Block by block:
//   one alpha over the whole block (85 % of the blocks in both games, 60 % opaque): one plane, the alpha exact.
//     Opaque, the block dxt1ToAstc writes for its colours; otherwise endpoint mode 12 (RGBA direct) at 8 bits, alpha
//     the same at both ends.  The colour is what the DXT1 rewriting gives, not exact: the thirds at 21 and 43 of 64,
//     at most 2 of 255 off.
//   alphas that vary: two planes of 2-bit weights (block mode 0x442), endpoint mode 12, alpha the second plane.  The
//     64 weight bits leave 45 for the 8 endpoint values: range 48 (a trit and 4 bits each), so the colours are no
//     longer exact (at most 7 of 255 off, 0.7 on average in the games' textures) and the alpha has 4 levels a block
//     where DXT5 has 8 and DXT3 16 (at most 43 off; 0.5 on average over all the texels of those textures).
// The endpoints are picked as the measurement of this choice did: for each channel the nearest of the 48 values to
// the block's ends and their neighbours, the pair with the smallest squared error.

/** range 48's values as the decoder unquantizes them (the spec's colour unquantization: a trit D and 4 bits dcba ->
 *  T = D * 22 + dcb000dcb, xor a's replication, then (A & 0x80) | T >> 2), in rising order (U48), each one's encoding
 *  (E48: trit * 16 + bits), and the nearest of them to each 8-bit value (N48) */
const U48 = new Uint8Array(48), E48 = new Uint8Array(48), N48 = new Uint8Array(256);
{
  const all: [number, number][] = [];
  for (let D = 0; D < 3; D++) {
    for (let m = 0; m < 16; m++) {
      const A = m & 1 ? 0x1FF : 0, b = (m >> 1) & 7;            // b = dcb
      let T = D * 22 + (b << 6 | b);
      T ^= A; T = (A & 0x80) | (T >> 2);
      all.push([T, D * 16 + m]);
    }
  }
  all.sort((x, y) => x[0] - y[0]);
  all.forEach(([v, e], k) => { U48[k] = v; E48[k] = e; });
  for (let v = 0, k = 0; v < 256; v++) {
    while (k < 47 && Math.abs(U48[k + 1] - v) < Math.abs(U48[k] - v)) k++;
    N48[v] = k;
  }
}
/** a texel's 16-bit value between two range-48 endpoints (indices into U48) at each 2-bit weight: (e0 * 257 * (64 - w)
 *  + e1 * 257 * w + 32) >> 6, the decoder's interpolation */
const LERP48 = new Int32Array(48 * 48 * 4);
for (let lo = 0; lo < 48; lo++) {
  for (let hi = 0; hi < 48; hi++) {
    [0, 21, 43, 64].forEach((w, i) => { LERP48[(lo * 48 + hi) * 4 + i] = (U48[lo] * 257 * (64 - w) + U48[hi] * 257 * w + 32) >> 6; });
  }
}
/** for each endpoint pair and 8-bit alpha: the smallest squared error of the four values between them (16-bit units)
 *  and which weight gives it; made at the first two-plane block (3 MB, a few ms), only GPUs without BC need them */
let AERR: Uint32Array | null = null, AW: Uint8Array | null = null;
function alphaTables() {
  AERR = new Uint32Array(48 * 48 * 256); AW = new Uint8Array(48 * 48 * 256);
  for (let pr = 0; pr < 48 * 48; pr++) {
    for (let v = 0; v < 256; v++) {
      let be = Infinity, bw = 0;
      for (let w = 0; w < 4; w++) { const e = LERP48[pr * 4 + w] - v * 257; if (e * e < be) { be = e * e; bw = w; } }
      AERR[pr * 256 + v] = be; AW[pr * 256 + v] = bw;
    }
  }
}
const W4 = [0, 3, 1, 2];            // a four-colour code (c0, c1, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1) to its 2-bit weight
const a16 = new Uint8Array(16), aw = new Uint8Array(16), at8 = new Uint8Array(8), cnt = new Uint32Array(4);
/** a block's distinct alphas and how many texels have each, for the alpha fit */
const av = new Uint8Array(16), ac = new Uint8Array(16), hist = new Uint8Array(16);
const lohi = new Uint8Array(8);
/** where each value of a trit group has its share of the group's 8-bit packing: [first bit, bits] */
const TB = [[0, 2], [2, 2], [4, 1], [5, 2], [7, 1]];

/** one colour channel's endpoints (indices into U48): of the values next to the nearest of the 48 to the block's two
 *  colours (each up or down one), the pair whose points at the codes' weights are nearest the codes' colours (as dxt.ts
 *  gives them, the thirds rounded down), squared and times the codes' texels; into lohi[2 * ch], lohi[2 * ch + 1] */
function fitColour(ch: number, e0: number, e1: number) {
  const k0 = N48[e0], k1 = N48[e1];
  // the codes' targets in 16-bit units (codes 0 1 2 3 at weights 0 64 21 43: LERP48's 0 3 1 2)
  const t0 = e0 * 257, t1 = e1 * 257, t2 = ((2 * e0 + e1) / 3 | 0) * 257, t3 = ((e0 + 2 * e1) / 3 | 0) * 257;
  const n0 = cnt[0], n1 = cnt[1], n2 = cnt[2], n3 = cnt[3];
  let best = Infinity, bl = k0, bh = k1;
  for (let dl = -1; dl <= 1; dl++) {
    const lo = k0 + dl;
    if (lo < 0 || lo > 47) continue;
    for (let dh = -1; dh <= 1; dh++) {
      const hi = k1 + dh;
      if (hi < 0 || hi > 47) continue;
      const base = (lo * 48 + hi) * 4;
      const d0 = LERP48[base] - t0, d1 = LERP48[base + 3] - t1, d2 = LERP48[base + 1] - t2, d3 = LERP48[base + 2] - t3;
      const s = n0 * d0 * d0 + n1 * d1 * d1 + n2 * d2 * d2 + n3 * d3 * d3;
      if (s < best) { best = s; bl = lo; bh = hi; }
    }
  }
  lohi[2 * ch] = bl; lohi[2 * ch + 1] = bh;
}

/** the alpha plane: endpoints from the block's lowest and highest alpha (each up or down one of the 48), each texel the
 *  nearest of the four values between them, the pair with the smallest squared error; into lohi[6], lohi[7] and aw */
function fitAlpha(mn: number, mx: number, nd: number) {
  const k0 = N48[mn], k1 = N48[mx];
  let best = Infinity, bl = k0, bh = k1;
  for (let dl = -1; dl <= 1; dl++) {
    const lo = k0 + dl;
    if (lo < 0 || lo > 47) continue;
    for (let dh = -1; dh <= 1; dh++) {
      const hi = k1 + dh;
      if (hi < 0 || hi > 47) continue;
      const pr = (lo * 48 + hi) * 256;
      let s = 0;
      // over the distinct alphas (DXT5 has at most 8), each times its texels
      for (let k = 0; k < nd && s < best; k++) s += ac[k] * AERR![pr + av[k]];
      if (s < best) { best = s; bl = lo; bh = hi; }
    }
  }
  lohi[6] = bl; lohi[7] = bh;
  const pr = (bl * 48 + bh) * 256;
  for (let i = 0; i < 16; i++) aw[i] = AW![pr + a16[i]];
}

/** One DXT3 or DXT5 level (bw x bh blocks of 16 bytes, pitch bytes a row of blocks) as ASTC 4x4 blocks, 16 bytes
 *  each, bw * 16 bytes a row; into the start of `into` when that has room, else into a new array.  dual counts the
 *  blocks whose alpha varies (two planes). */
export function dxtAlphaToAstc(dxt5: boolean, src: Uint8Array, off: number, pitch: number, bw: number, bh: number,
  into?: Uint8Array, dual?: { blocks: number }): Uint8Array {
  const n = bw * bh * 16, out = into && into.length >= n ? into.subarray(0, n) : new Uint8Array(n);
  const o32 = new Uint32Array(out.buffer, out.byteOffset, n >> 2);
  for (let by = 0; by < bh; by++) {
    for (let bx = 0; bx < bw; bx++) {
      const b = off + by * pitch + bx * 16;
      // the alphas, as dxt.ts unpacks them
      if (dxt5) {
        const a0 = src[b], a1 = src[b + 1];
        at8[0] = a0; at8[1] = a1;
        if (a0 > a1) for (let i = 1; i < 7; i++) at8[i + 1] = ((7 - i) * a0 + i * a1) / 7 | 0;
        else { for (let i = 1; i < 5; i++) at8[i + 1] = ((5 - i) * a0 + i * a1) / 5 | 0; at8[6] = 0; at8[7] = 255; }
        const lo = src[b + 2] | src[b + 3] << 8 | src[b + 4] << 16, hi = src[b + 5] | src[b + 6] << 8 | src[b + 7] << 16;
        for (let k = 0; k < 8; k++) hist[k] = 0;
        for (let i = 0; i < 8; i++) {
          const k = (lo >> 3 * i) & 7, l = (hi >> 3 * i) & 7;
          a16[i] = at8[k]; a16[i + 8] = at8[l]; hist[k]++; hist[l]++;
        }
      } else {
        for (let k = 0; k < 16; k++) hist[k] = 0;
        for (let i = 0; i < 8; i++) {
          const v = src[b + i];
          a16[2 * i] = (v & 15) * 17; a16[2 * i + 1] = (v >> 4) * 17; hist[v & 15]++; hist[v >> 4]++;
        }
      }
      let mn = 255, mx = 0;
      for (let i = 0; i < 16; i++) { const v = a16[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
      const c = b + 8;
      const c0 = src[c] | src[c + 1] << 8, c1 = src[c + 2] | src[c + 3] << 8;
      const idx = (src[c + 4] | src[c + 5] << 8 | src[c + 6] << 16 | src[c + 7] << 24) >>> 0;
      let r0 = C5[c0 >> 11], g0 = C6[(c0 >> 5) & 63], b0 = C5[c0 & 31];
      let r1 = C5[c1 >> 11], g1 = C6[(c1 >> 5) & 63], b1 = C5[c1 & 31];
      const o = (by * bw + bx) * 4;
      if (mn === mx) {
        // one plane: the colours at 8 bits, swapped (the weights turned round) when the second's sum is the smaller
        const swap = r1 + g1 + b1 < r0 + g0 + b0;
        if (swap) { let x = r0; r0 = r1; r1 = x; x = g0; g0 = g1; g1 = x; x = b0; b0 = b1; b1 = x; }
        const lo = idx & 0x55555555, hi = (idx >>> 1) & 0x55555555;
        const ws = swap ? ~(lo << 1 | (lo ^ hi)) : lo << 1 | (lo ^ hi);
        o32[o] = 0x042 | (mn === 255 ? 8 : 12) << 13 | r0 << 17 | r1 << 25;
        o32[o + 1] = r1 >> 7 | g0 << 1 | g1 << 9 | b0 << 17 | b1 << 25;
        o32[o + 2] = b1 >> 7 | (mn === 255 ? 0 : mn << 1 | mn << 9);     // endpoint mode 12: a0 = a1 = the alpha
        o32[o + 3] = rev32(ws);
        continue;
      }
      // two planes: how many texels have each colour code (each code's error counts that often), then each channel's
      // endpoints, then the alpha's over its distinct values
      cnt[0] = cnt[1] = cnt[2] = cnt[3] = 0;
      for (let i = 0; i < 16; i++) cnt[(idx >>> 2 * i) & 3]++;
      fitColour(0, r0, r1); fitColour(1, g0, g1); fitColour(2, b0, b1);
      let nd = 0;
      for (let k = 0; k < (dxt5 ? 8 : 16); k++) if (hist[k]) { av[nd] = dxt5 ? at8[k] : k * 17; ac[nd++] = hist[k]; }
      if (!AERR) alphaTables();
      fitAlpha(mn, mx, nd);
      // the decoder compares the unquantized sums: the second endpoint's the smaller, swap all four channels' ends and
      // turn both planes' weights round
      const swap = U48[lohi[1]] + U48[lohi[3]] + U48[lohi[5]] < U48[lohi[0]] + U48[lohi[2]] + U48[lohi[4]];
      if (swap) for (let k = 0; k < 8; k += 2) { const x = lohi[k]; lohi[k] = lohi[k + 1]; lohi[k + 1] = x; }
      blk[0] = blk[1] = blk[2] = blk[3] = 0;
      put(0, 11, 0x442); put(13, 4, 12);
      // the 8 values (r0 r1 g0 g1 b0 b1 a0 a1), integer sequence encoded: two groups of trits, 5 and 3, each value's 4
      // bits followed by its share of the group's 8-bit trit packing (2 2 1 2 1 bits; the short group's first 5 only)
      let pos = 17;
      for (let g = 0; g < 8; g += 5) {
        const k = Math.min(5, 8 - g);
        let tk = 0;
        for (let i = k - 1; i >= 0; i--) tk = tk * 3 + (E48[lohi[g + i]] >> 4);
        const T = TRITS[tk];
        for (let i = 0; i < k; i++) {
          put(pos, 4, E48[lohi[g + i]] & 15); pos += 4;
          put(pos, TB[i][1], (T >> TB[i][0]) & ((1 << TB[i][1]) - 1)); pos += TB[i][1];
        }
      }
      // the weights, texel by texel: colour then alpha, from bit 127 down; the component selector (3: alpha) below them
      let w0 = 0, w1 = 0;
      for (let i = 0; i < 16; i++) {
        const k = (idx >>> 2 * i) & 3;
        let wc = W4[k], wa = aw[i];
        if (swap) { wc = 3 - wc; wa = 3 - wa; }
        const pair = wc | wa << 2;
        if (i < 8) w0 |= pair << 4 * i; else w1 |= pair << 4 * (i - 8);
      }
      blk[3] = rev32(w0); blk[2] = rev32(w1);
      put(62, 2, 3);
      if (dual) dual.blocks++;
      o32[o] = blk[0]; o32[o + 1] = blk[1]; o32[o + 2] = blk[2]; o32[o + 3] = blk[3];
    }
  }
  return out;
}
