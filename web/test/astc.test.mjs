// DXT rewritten as ASTC 4x4 (web/src/gpu/astc.ts), without a GPU:
//   node --test web/test/astc.test.mjs
// A decoder of the few ASTC blocks dxt1ToAstc writes (from the Khronos spec: block modes 0x042, 0x051, 0x451,
// endpoint modes 8 and 12) reads them back, and every texel is compared with the CPU's unpacking (dxt.ts) within what
// the rewriting allows.  The GPU check (web/tools/astc-check.mjs, Chrome and WebKit) gave the same figures on an Apple
// GPU: four colours at most 2 of 255 off, three colours 1, two-plane blocks 1 and their alpha exact.
// DXT3 and DXT5 (dxtAlphaToAstc) are read back by the general reference decoder (astc-decode.mjs), which agrees with
// SwiftShader's ASTC decoder (astc-check.mjs).  ASTC_DUMPS=<dir> also runs them over texture dumps there (<name>.dump:
// width, height, 3 or 5, levels as 16-bit numbers, then the levels' blocks; game content, never in the repository).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { decodeAstc4x4, to8 } from './astc-decode.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export { unpackDxt } from './web/src/gpu/dxt.ts'; export { dxt1ToAstc, dxt1HasPunch, dxtAlphaToAstc } from './web/src/gpu/astc.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const { unpackDxt, dxt1ToAstc, dxt1HasPunch, dxtAlphaToAstc } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
const DXT1 = 0x31545844, DXT3 = 0x33545844, DXT5 = 0x35545844;

/** one ASTC block (16 bytes) as 16 RGBA texels, or a string saying what it is not */
function decode(b) {
  const bit = i => (b[i >> 3] >> (i & 7)) & 1;
  const bits = (pos, n) => { let v = 0; for (let i = 0; i < n; i++) v |= bit(pos + i) << i; return v; };
  const mode = bits(0, 11), dual = (mode & 0x400) !== 0, base = mode & 0x3FF;
  if (base !== 0x042 && base !== 0x051) return `block mode ${mode.toString(16)}`;
  if (bits(11, 2) !== 0) return 'more than one partition';
  const trits = base === 0x051, nw = dual ? 32 : 16, wbits = trits ? Math.ceil(nw * 8 / 5) : nw * 2;
  // the weights: bit i of their sequence at bit 127 - i
  const wbit = i => bit(127 - i), wread = (pos, n) => { let v = 0; for (let i = 0; i < n; i++) v |= wbit(pos + i) << i; return v; };
  const w = [];
  if (!trits) for (let i = 0; i < nw; i++) w.push([0, 21, 43, 64][wread(2 * i, 2)]);
  else {
    for (let g = 0, pos = 0; g < nw; g += 5) {
      const n = Math.min(5, nw - g), len = [0, 2, 4, 5, 7, 8][n], T = wread(pos, len); pos += len;
      const t = (x, i, k = 1) => (x >> i) & ((1 << k) - 1);
      let C, t3, t4;
      if (t(T, 2, 3) === 7) { C = t(T, 5, 3) << 2 | t(T, 0, 2); t4 = 2; t3 = 2; }
      else { C = t(T, 0, 5); if (t(T, 5, 2) === 3) { t4 = 2; t3 = t(T, 7); } else { t4 = t(T, 7); t3 = t(T, 5, 2); } }
      let t0, t1, t2;
      if (t(C, 0, 2) === 3) { t2 = 2; t1 = t(C, 4); t0 = t(C, 3) << 1 | (t(C, 2) & ~t(C, 3) & 1); }
      else if (t(C, 2, 2) === 3) { t2 = 2; t1 = 2; t0 = t(C, 0, 2); }
      else { t2 = t(C, 4); t1 = t(C, 2, 2); t0 = t(C, 1) << 1 | (t(C, 0) & ~t(C, 1) & 1); }
      w.push(...[t0, t1, t2, t3, t4].slice(0, n).map(x => [0, 32, 64][x]));
    }
  }
  const cem = bits(13, 4);
  let e0, e1;
  if (cem === 8 && !dual) {
    const v = Array.from({ length: 6 }, (_, i) => bits(17 + 8 * i, 8));
    if (v[1] + v[3] + v[5] < v[0] + v[2] + v[4]) return 'blue contraction';
    e0 = [v[0], v[2], v[4], 255]; e1 = [v[1], v[3], v[5], 255];
  } else if (cem === 12 && dual) {
    const v = Array.from({ length: 8 }, (_, i) => { const q = bits(17 + 7 * i, 7); return q << 1 | q >> 6; });
    if (v[1] + v[3] + v[5] < v[0] + v[2] + v[4]) return 'blue contraction';
    e0 = [v[0], v[2], v[4], v[6]]; e1 = [v[1], v[3], v[5], v[7]];
  } else return `endpoint mode ${cem}${dual ? ' with two planes' : ''}`;
  const ccs = dual ? bits(128 - wbits - 2, 2) : -1;
  const out = [];
  for (let i = 0; i < 16; i++) {
    const px = [0, 1, 2, 3].map(c => {
      const wt = dual ? w[2 * i + (c === ccs ? 1 : 0)] : w[i];
      const C = Math.floor((e0[c] * 257 * (64 - wt) + e1[c] * 257 * wt + 32) / 64);
      return Math.round(C / 65535 * 255);
    });
    out.push(px);
  }
  return out;
}

const block = (c0, c1, idx) => new Uint8Array([c0 & 255, c0 >> 8, c1 & 255, c1 >> 8, idx & 255, (idx >> 8) & 255, (idx >> 16) & 255, idx >>> 24]);
/** the largest colour difference between the rewritten block and the unpacked one, and whether the alphas match */
function compare(src) {
  const a = dxt1ToAstc(src, 0, 8, 1, 1), got = decode(a);
  if (typeof got === 'string') return { error: got };
  const u = unpackDxt(DXT1, src, 0, 8, 4, 4);
  let max = 0, alpha = true;
  for (let i = 0; i < 16; i++) {
    const want = [u[4 * i + 2], u[4 * i + 1], u[4 * i], u[4 * i + 3]];
    if (got[i][3] !== want[3]) alpha = false;
    if (want[3]) max = Math.max(max, ...[0, 1, 2].map(c => Math.abs(got[i][c] - want[c])));
  }
  return { max, alpha };
}

test('four colours: the endpoints exact, the thirds within 2', () => {
  // c0 = F800 (red), c1 = 001F (blue); first row codes 0 1 2 3
  const src = block(0xF800, 0x001F, 0xE4);
  const a = decode(dxt1ToAstc(src, 0, 8, 1, 1));
  assert.deepEqual(a[0], [255, 0, 0, 255]);
  assert.deepEqual(a[1], [0, 0, 255, 255]);
  assert.deepEqual(a.slice(2, 4), [[171, 0, 84, 255], [84, 0, 171, 255]]);   // BC1: 170/85
  assert.deepEqual(compare(src), { max: 1, alpha: true });
});

test('the endpoints swapped when the second is darker (no blue contraction)', () => {
  // c0 = FFFF (white) > c1 = 0000 (black): the decoder would contract a white second endpoint's blue
  assert.deepEqual(compare(block(0xFFFF, 0x0000, 0x1B)), { max: 1, alpha: true });
});

test('three colours without a hole: the midpoint exact', () => {
  const src = block(0x001F, 0xF800, 0x24);           // c0 < c1, codes 0 1 2 0 ...
  const a = decode(dxt1ToAstc(src, 0, 8, 1, 1));
  assert.deepEqual(a.slice(0, 3), [[0, 0, 255, 255], [255, 0, 0, 255], [128, 0, 128, 255]]);   // BC1: 127/127, rounded
  assert.equal(compare(src).alpha, true);
});

test('a transparent texel: two planes, alpha exact', () => {
  const src = block(0x001F, 0xF800, 0xE4);           // c0 < c1 and code 3 in the first row
  assert.equal(dxt1HasPunch(src, 0, 8, 1, 1), true);
  const punch = { blocks: 0 };
  const a = decode(dxt1ToAstc(src, 0, 8, 1, 1, undefined, punch));
  assert.equal(punch.blocks, 1);
  assert.equal(a[3][3], 0);
  assert.deepEqual(compare(src), { max: 1, alpha: true });
});

test('dxt1HasPunch: only code 3 in a three-colour block', () => {
  assert.equal(dxt1HasPunch(block(0xF800, 0x001F, 0xFFFFFFFF), 0, 8, 1, 1), false);   // four colours: code 3 is a colour
  assert.equal(dxt1HasPunch(block(0x001F, 0xF800, 0xAAAAAAAA), 0, 8, 1, 1), false);   // three colours, codes 2 only
  assert.equal(dxt1HasPunch(block(0x1234, 0x1234, 0xC0000000), 0, 8, 1, 1), true);    // one colour, code 3 at the last texel
  // a level of 2x2 blocks, pitch 24: the hole in the last block
  const lv = new Uint8Array(48);
  lv.set(block(0xF800, 0x001F, 0), 0); lv.set(block(0xF800, 0x001F, 0), 8);
  lv.set(block(0xF800, 0x001F, 0), 24); lv.set(block(0x001F, 0xF800, 3), 32);
  assert.equal(dxt1HasPunch(lv, 0, 24, 2, 2), true);
  assert.equal(dxt1HasPunch(lv, 0, 24, 2, 1), false);
});

test('random blocks of every kind, within the limits', () => {
  let seed = 7;
  const rnd = () => (seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) >>> 8;
  const worst = { four: 0, three: 0, holes: 0 };
  for (let i = 0; i < 20000; i++) {
    let c0 = rnd() & 0xFFFF, c1 = i % 5 === 4 ? c0 : rnd() & 0xFFFF;
    if (i % 7 === 0) c0 = [0, 0xFFFF, 0xF800, 0x07E0, 0x001F][rnd() % 5];
    const idx = (rnd() << 8 ^ rnd()) >>> 0, src = block(c0, c1, idx);
    const r = compare(src);
    assert.ok(!r.error, `block ${i}: ${r.error}`);
    assert.ok(r.alpha, `block ${i}: alpha`);
    const k = c0 > c1 ? 'four' : dxt1HasPunch(src, 0, 8, 1, 1) ? 'holes' : 'three';
    worst[k] = Math.max(worst[k], r.max);
  }
  assert.ok(worst.four <= 2 && worst.three <= 1 && worst.holes <= 1, JSON.stringify(worst));
});

test('the layout: blocks in rows of a wider pitch, an offset, into a buffer kept', () => {
  // 3x2 blocks at offset 5, pitch 30 (6 bytes past the blocks in each row)
  const src = new Uint8Array(5 + 60);
  for (let i = 0; i < 6; i++) src.set(block(0xF800 - i, 0x001F + i, 0x1B * i), 5 + Math.floor(i / 3) * 30 + (i % 3) * 8);
  const buf = new Uint8Array(200), out = dxt1ToAstc(src, 5, 30, 3, 2, buf);
  assert.equal(out.buffer, buf.buffer);
  assert.equal(out.length, 96);
  for (let i = 0; i < 6; i++) {
    const one = dxt1ToAstc(block(0xF800 - i, 0x001F + i, 0x1B * i), 0, 8, 1, 1);
    assert.deepEqual(Array.from(out.subarray(i * 16, i * 16 + 16)), Array.from(one));
  }
  assert.notEqual(dxt1ToAstc(src, 5, 30, 3, 2, new Uint8Array(16)).buffer.byteLength, 16);
});

test('the reference decoder reads the DXT1 blocks as this file\'s decoder does', () => {
  let seed = 3;
  const rnd = () => (seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) >>> 8;
  for (let i = 0; i < 3000; i++) {
    const c0 = rnd() & 0xFFFF, c1 = i % 3 === 0 ? c0 : rnd() & 0xFFFF, idx = (rnd() << 8 ^ rnd()) >>> 0;
    const a = dxt1ToAstc(block(c0, c1, idx), 0, 8, 1, 1);
    assert.deepEqual(decodeAstc4x4(a).map(px => px.map(to8)), decode(a), `block ${i}`);
  }
});

// a DXT3 or DXT5 block: 8 bytes of alpha, then a DXT1 colour block
const block16 = (alpha8, c0, c1, idx) => { const b = new Uint8Array(16); b.set(alpha8, 0); b.set(block(c0, c1, idx), 8); return b; };
const dxt3Alpha = a16 => Uint8Array.from({ length: 8 }, (_, i) => (a16[2 * i] / 17) | (a16[2 * i + 1] / 17) << 4);
/** DXT5 alpha: endpoints and 16 3-bit codes */
const dxt5Alpha = (a0, a1, codes) => {
  let lo = 0, hi = 0;
  for (let i = 0; i < 8; i++) { lo |= codes[i] << 3 * i; hi |= codes[i + 8] << 3 * i; }
  return new Uint8Array([a0, a1, lo & 255, (lo >> 8) & 255, lo >> 16, hi & 255, (hi >> 8) & 255, hi >> 16]);
};
/** the rewritten blocks against the unpacked ones: largest and summed differences, colour per channel */
function compare35(dxt5, src, bw, bh) {
  const dual = { blocks: 0 }, a = dxtAlphaToAstc(dxt5, src, 0, bw * 16, bw, bh, undefined, dual);
  const W = bw * 4, u = unpackDxt(dxt5 ? DXT5 : DXT3, src, 0, bw * 16, W, bh * 4);
  const r = { alphaMax: 0, alphaSum: 0, colourMax: 0, colourSum: 0, constColourMax: 0, texels: 0, dual: dual.blocks, errors: [] };
  for (let b = 0; b < bw * bh; b++) {
    const got = decodeAstc4x4(a.subarray(b * 16, b * 16 + 16));
    if (typeof got === 'string') { r.errors.push(`block ${b}: ${got}`); continue; }
    const at = t => ((Math.floor(b / bw) * 4 + (t >> 2)) * W + (b % bw) * 4 + (t & 3)) * 4;
    const alphas = Array.from({ length: 16 }, (_, t) => u[at(t) + 3]), one = alphas.every(x => x === alphas[0]);
    for (let t = 0; t < 16; t++) {
      const p = at(t), want = [u[p + 2], u[p + 1], u[p]];
      const da = Math.abs(to8(got[t][3]) - u[p + 3]);
      r.alphaMax = Math.max(r.alphaMax, da); r.alphaSum += da; r.texels++;
      for (let c = 0; c < 3; c++) {
        const d = Math.abs(to8(got[t][c]) - want[c]);
        r.colourMax = Math.max(r.colourMax, d); r.colourSum += d;
        if (one) r.constColourMax = Math.max(r.constColourMax, d);
      }
    }
  }
  return { ...r, alphaMean: r.alphaSum / r.texels, colourMean: r.colourSum / r.texels / 3, out: a };
}

test('DXT3 and DXT5 of one alpha: one plane, alpha exact; opaque, the very block DXT1 gets', () => {
  for (const [c0, c1, idx] of [[0xF800, 0x001F, 0xE4], [0xFFFF, 0x0000, 0x1B], [0x1234, 0xFEDC, 0x9C63]]) {
    // opaque: byte for byte dxt1ToAstc's four-colour block (c0 > c1 there)
    const hi = Math.max(c0, c1), lo = Math.min(c0, c1);
    const one = dxt1ToAstc(block(hi, lo, idx), 0, 8, 1, 1);
    const opaque = [[false, new Uint8Array(8).fill(0xFF)], [true, dxt5Alpha(255, 255, new Array(16).fill(0))], [true, dxt5Alpha(0, 0, new Array(16).fill(7))]];
    for (const [dxt5, alpha] of opaque) {
      const a = dxtAlphaToAstc(dxt5, block16(alpha, hi, lo, idx), 0, 16, 1, 1);
      assert.deepEqual(Array.from(a), Array.from(one), `${dxt5 ? 'DXT5' : 'DXT3'} ${hi.toString(16)} ${lo.toString(16)}`);
    }
    // another alpha: the same colours, the alpha exact (endpoint mode 12 at 8 bits)
    const ref = compare35(false, block16(new Uint8Array(8).fill(0xFF), c0, c1, idx), 1, 1);
    for (const v of [0, 17, 128, 254]) {
      const src = block16(v % 17 ? dxt5Alpha(v, v, new Array(16).fill(1)) : dxt3Alpha(new Array(16).fill(v)), c0, c1, idx);
      const r = compare35(v % 17 !== 0, src, 1, 1);
      assert.deepEqual(r.errors, []);
      assert.equal(r.dual, 0);
      assert.equal(r.alphaMax, 0, `alpha ${v}`);
      assert.deepEqual(decodeAstc4x4(r.out).map(px => px.slice(0, 3)), decodeAstc4x4(ref.out).map(px => px.slice(0, 3)));
    }
  }
});

test('DXT3 and DXT5 colour blocks always have four colours (c0 <= c1 too)', () => {
  // c0 < c1, code 3: a colour (a third of the way from c1), not DXT1's transparent black
  const r = compare35(false, block16(new Uint8Array(8).fill(0xFF), 0x001F, 0xF800, 0xFFFFFFFF), 1, 1);
  assert.ok(r.constColourMax <= 2);
  assert.deepEqual(decodeAstc4x4(r.out)[0].map(to8), [171, 0, 84, 255]);         // BC2: 170 0 85
});

test('varying alpha: two planes, alpha the second; four levels for the eight of DXT5', () => {
  // DXT5 eight levels, 255 at code 0 down to 0 at code 1: four levels between ends at most a step of the 48 off (the
  // smallest squared error may move one), each texel at most 255/6 and that step from its own
  const codes = [0, 2, 3, 4, 5, 6, 7, 1, 0, 2, 3, 4, 5, 6, 7, 1];
  const r = compare35(true, block16(dxt5Alpha(255, 0, codes), 0xF800, 0x001F, 0xE4E4E4E4), 1, 1);
  assert.deepEqual(r.errors, []);
  assert.equal(r.dual, 1);
  const got = decodeAstc4x4(r.out);
  assert.ok(to8(got[0][3]) >= 249 && to8(got[7][3]) <= 6, `ends ${to8(got[0][3])} ${to8(got[7][3])}`);
  assert.ok(r.alphaMax <= 48, `alpha max ${r.alphaMax}`);
  assert.equal(new Set(got.map(px => px[3])).size, 4);
  // the block mode: a 4x4 grid of 2-bit weights in two planes, endpoint mode 12, the selector 3 (alpha)
  const bits = (pos, n) => { let v = 0; for (let i = 0; i < n; i++) v |= ((r.out[(pos + i) >> 3] >> ((pos + i) & 7)) & 1) << i; return v; };
  assert.equal(bits(0, 11), 0x442); assert.equal(bits(13, 4), 12); assert.equal(bits(62, 2), 3);
});

test('random DXT3 and DXT5 blocks of every kind, within the limits', () => {
  let seed = 11;
  const rnd = () => (seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) >>> 8;
  const ext = [0, 0xFFFF, 0xF800, 0x07E0, 0x001F];
  for (const dxt5 of [false, true]) {
    const bw = 64, bh = 32, src = new Uint8Array(bw * bh * 16);
    for (let b = 0; b < bw * bh; b++) {
      for (let i = 0; i < 16; i++) src[b * 16 + i] = rnd() & 255;
      if (b % 3 === 0) {
        if (dxt5) { src[b * 16 + 1] = src[b * 16]; src.fill(0, b * 16 + 2, b * 16 + 8); } else src.fill(0x11 * (rnd() % 16), b * 16, b * 16 + 8);
      }
      if (b % 7 === 0) src.set(block(ext[rnd() % 5], ext[rnd() % 5], rnd()), b * 16 + 8);
    }
    const r = compare35(dxt5, src, bw, bh);
    assert.deepEqual(r.errors, []);
    // one alpha: the colour as DXT1's rewriting gives it (2), the alpha exact; else the 4 alpha levels and the colours
    // at range-48 endpoints
    assert.ok(r.constColourMax <= 2 && r.colourMax <= 8 && r.alphaMax <= 48, JSON.stringify({ ...r, out: undefined }));
    assert.ok(r.dual > 0 && r.dual < bw * bh);
  }
});

test('DXT3 and DXT5: blocks in rows of a wider pitch, an offset, into a buffer kept', () => {
  const src = new Uint8Array(3 + 2 * 60);
  for (let i = 0; i < 6; i++) {
    const alpha = dxt3Alpha(Array.from({ length: 16 }, (_, t) => ((t + i) % 16) * 17));
    src.set(block16(alpha, 0xF800 - i, 0x001F + i, 0x1B * i), 3 + Math.floor(i / 3) * 60 + (i % 3) * 16);
  }
  const buf = new Uint8Array(200), out = dxtAlphaToAstc(false, src, 3, 60, 3, 2, buf);
  assert.equal(out.buffer, buf.buffer);
  assert.equal(out.length, 96);
  for (let i = 0; i < 6; i++) {
    const o = 3 + Math.floor(i / 3) * 60 + (i % 3) * 16, one = dxtAlphaToAstc(false, src.slice(o, o + 16), 0, 16, 1, 1);
    assert.deepEqual(Array.from(out.subarray(i * 16, i * 16 + 16)), Array.from(one));
  }
});

// texture dumps of the games (ASTC_DUMPS=<dir>): every level of each, the alpha and colour against the unpacking
const DUMPS = process.env.ASTC_DUMPS;
test('texture dumps (ASTC_DUMPS)', { skip: !DUMPS && 'ASTC_DUMPS not set' }, t => {
  for (const f of fs.readdirSync(DUMPS).filter(n => n.endsWith('.dump')).sort()) {
    const d = fs.readFileSync(path.join(DUMPS, f)), W = d.readUInt16LE(0), H = d.readUInt16LE(2), dxt5 = d.readUInt16LE(4) === 5, mips = d.readUInt16LE(6);
    const sum = { alphaMax: 0, alphaSum: 0, colourMax: 0, colourSum: 0, constColourMax: 0, texels: 0, dual: 0, blocks: 0 };
    for (let l = 0, off = 8; l < mips; l++) {
      const bw = (Math.max(1, W >> l) + 3) >> 2, bh = (Math.max(1, H >> l) + 3) >> 2;
      const r = compare35(dxt5, d.subarray(off, off + bw * bh * 16), bw, bh);
      assert.deepEqual(r.errors, [], f);
      for (const k of ['alphaMax', 'colourMax', 'constColourMax']) sum[k] = Math.max(sum[k], r[k]);
      for (const k of ['alphaSum', 'colourSum', 'texels', 'dual']) sum[k] += r[k];
      sum.blocks += bw * bh; off += bw * bh * 16;
    }
    t.diagnostic(`${f} ${dxt5 ? 'DXT5' : 'DXT3'} ${W}x${H} ${mips} levels: ${sum.dual}/${sum.blocks} blocks two planes; ` +
      `alpha max ${sum.alphaMax} mean ${(sum.alphaSum / sum.texels).toFixed(2)}; colour max ${sum.colourMax} ` +
      `mean ${(sum.colourSum / sum.texels / 3).toFixed(2)}, in blocks of one alpha max ${sum.constColourMax}`);
    assert.ok(sum.alphaMax <= 48 && sum.colourMax <= 8 && sum.constColourMax <= 2, f);
  }
});
