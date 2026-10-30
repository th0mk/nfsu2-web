// A reference decoder of ASTC 4x4 LDR blocks, written from the Khronos Data Format specification (section "ASTC"),
// for the tests of web/src/gpu/astc.ts.  It shares nothing with the encoder: block modes of every layout a 4x4
// footprint allows, one or two planes, weight grids up to 4x4 with the infill, integer sequences of bits, trits and
// quints, the colour and weight unquantization tables, endpoint modes 0, 4, 8 and 12 (the direct ones) with blue
// contraction.  More than one partition, the void extent and the other endpoint modes come back as a string saying so.
//   decodeAstc4x4(block: Uint8Array(16)) -> [[r, g, b, a] x 16] of 16-bit values (C, 0..65535), or a string
//   to8(C) -> the value as 8 bits, as a unorm texture samples it rounded (C / 65535 * 255)

/** the integer sequence ranges a value can have: [levels, trits, quints, bits] */
const RANGES = [[2, 0, 0, 1], [3, 1, 0, 0], [4, 0, 0, 2], [5, 0, 1, 0], [6, 1, 0, 1], [8, 0, 0, 3], [10, 0, 1, 1],
  [12, 1, 0, 2], [16, 0, 0, 4], [20, 0, 1, 2], [24, 1, 0, 3], [32, 0, 0, 5], [40, 0, 1, 3], [48, 1, 0, 4], [64, 0, 0, 6],
  [80, 0, 1, 4], [96, 1, 0, 5], [128, 0, 0, 7], [160, 0, 1, 5], [192, 1, 0, 6], [256, 0, 0, 8]];
const iseBits = ([, t, q, b], n) => b * n + (t ? Math.ceil(8 * n / 5) : q ? Math.ceil(7 * n / 3) : 0);
const bitsOf = (x, i, n = 1) => (x >> i) & ((1 << n) - 1);

/** the trits of an 8-bit packing (the spec's decoding) */
function trits(T) {
  let C, t3, t4;
  if (bitsOf(T, 2, 3) === 7) { C = bitsOf(T, 5, 3) << 2 | bitsOf(T, 0, 2); t4 = 2; t3 = 2; }
  else { C = bitsOf(T, 0, 5); if (bitsOf(T, 5, 2) === 3) { t4 = 2; t3 = bitsOf(T, 7); } else { t4 = bitsOf(T, 7); t3 = bitsOf(T, 5, 2); } }
  let t0, t1, t2;
  if (bitsOf(C, 0, 2) === 3) { t2 = 2; t1 = bitsOf(C, 4); t0 = bitsOf(C, 3) << 1 | (bitsOf(C, 2) & ~bitsOf(C, 3) & 1); }
  else if (bitsOf(C, 2, 2) === 3) { t2 = 2; t1 = 2; t0 = bitsOf(C, 0, 2); }
  else { t2 = bitsOf(C, 4); t1 = bitsOf(C, 2, 2); t0 = bitsOf(C, 1) << 1 | (bitsOf(C, 0) & ~bitsOf(C, 1) & 1); }
  return [t0, t1, t2, t3, t4];
}
/** the quints of a 7-bit packing */
function quints(Q) {
  const q = i => bitsOf(Q, i);
  if (bitsOf(Q, 1, 2) === 3 && bitsOf(Q, 5, 2) === 0) return [4, 4, q(0) << 2 | (q(4) & ~q(0) & 1) << 1 | (q(3) & ~q(0) & 1)];
  let q2, C;
  if (bitsOf(Q, 1, 2) === 3) { q2 = 4; C = bitsOf(Q, 3, 2) << 3 | (~bitsOf(Q, 5, 2) & 3) << 1 | q(0); }
  else { q2 = bitsOf(Q, 5, 2); C = bitsOf(Q, 0, 5); }
  return bitsOf(C, 0, 3) === 5 ? [bitsOf(C, 3, 2), 4, q2] : [bitsOf(C, 0, 3), bitsOf(C, 3, 2), q2];
}

/** n values of range r from a bit reader (read(pos, n), bits past the data 0): [{ D: trit or quint, m: low bits }] */
function readIse(read, start, n, [, t, q, b]) {
  const out = [];
  let pos = start;
  if (t) {
    for (let g = 0; g < n; g += 5) {
      const k = Math.min(5, n - g), m = [], tb = [2, 2, 1, 2, 1];
      let T = 0, tpos = 0;
      for (let i = 0; i < 5; i++) {
        m.push(i < k ? read(pos, b) : 0); if (i < k) pos += b;
        // a short group: its trit bits stop with its last value
        if (i < k) { T |= read(pos, tb[i]) << tpos; pos += tb[i]; }
        tpos += tb[i];
      }
      const ts = trits(T);
      for (let i = 0; i < k; i++) out.push({ D: ts[i], m: m[i] });
    }
  } else if (q) {
    for (let g = 0; g < n; g += 3) {
      const k = Math.min(3, n - g), m = [], qb = [3, 2, 2];
      let Q = 0, qpos = 0;
      for (let i = 0; i < 3; i++) {
        m.push(i < k ? read(pos, b) : 0); if (i < k) pos += b;
        if (i < k) { Q |= read(pos, qb[i]) << qpos; pos += qb[i]; }
        qpos += qb[i];
      }
      const qs = quints(Q);
      for (let i = 0; i < k; i++) out.push({ D: qs[i], m: m[i] });
    }
  } else for (let i = 0; i < n; i++) { out.push({ D: 0, m: read(pos, b) }); pos += b; }
  return out;
}

/** a colour endpoint value to 8 bits */
function unquantColour({ D, m }, [, t, q, b]) {
  if (!t && !q) { let v = 0; for (let s = 8 - b; s > -b; s -= b) v |= s >= 0 ? m << s : m >> -s; return v & 255; }
  const A = m & 1 ? 0x1FF : 0, [bb, c, d, e, f] = [1, 2, 3, 4, 5].map(i => bitsOf(m, i));
  let B, C;
  if (t) {
    if (b === 1) { B = 0; C = 204; }
    else if (b === 2) { B = bb << 8 | bb << 4 | bb << 2 | bb << 1; C = 93; }
    else if (b === 3) { B = c << 8 | bb << 7 | c << 3 | bb << 2 | c << 1 | bb; C = 44; }
    else if (b === 4) { B = d << 8 | c << 7 | bb << 6 | d << 2 | c << 1 | bb; C = 22; }
    else if (b === 5) { B = e << 8 | d << 7 | c << 6 | bb << 5 | e << 1 | d; C = 11; }
    else { B = f << 8 | e << 7 | d << 6 | c << 5 | bb << 4 | f; C = 5; }
  } else {
    if (b === 1) { B = 0; C = 113; }
    else if (b === 2) { B = bb << 8 | bb << 3 | bb << 2; C = 54; }
    else if (b === 3) { B = c << 8 | bb << 7 | c << 2 | bb << 1 | c; C = 26; }
    else if (b === 4) { B = d << 8 | c << 7 | bb << 6 | d << 1 | c; C = 13; }
    else { B = e << 8 | d << 7 | c << 6 | bb << 5 | e; C = 6; }
  }
  let T = D * C + B;
  T ^= A;
  return (A & 0x80) | (T >> 2);
}

/** a weight to 0..64 */
function unquantWeight({ D, m }, [, t, q, b]) {
  let T;
  if (!t && !q) {
    // bits replicated to 6
    T = 0; for (let s = 6 - b; s > -b; s -= b) T |= s >= 0 ? m << s : m >> -s;
    T &= 63;
  } else if (b === 0) T = t ? [0, 32, 63][D] : [0, 16, 32, 47, 63][D];
  else {
    const A = m & 1 ? 0x7F : 0, bb = bitsOf(m, 1), c = bitsOf(m, 2);
    let B, C;
    if (t) {
      if (b === 1) { B = 0; C = 50; } else if (b === 2) { B = bb << 6 | bb << 2 | bb; C = 23; } else { B = c << 6 | bb << 5 | c << 1 | bb; C = 11; }
    } else {
      if (b === 1) { B = 0; C = 28; } else { B = bb << 6 | bb << 1; C = 13; }
    }
    T = D * C + B; T ^= A; T = (A & 0x20) | (T >> 2);
  }
  return T > 32 ? T + 1 : T;
}

export function decodeAstc4x4(blk) {
  const bit = i => i < 128 ? (blk[i >> 3] >> (i & 7)) & 1 : 0;
  const read = (pos, n) => { let v = 0; for (let i = 0; i < n; i++) v |= bit(pos + i) << i; return v; };
  const mode = read(0, 11);
  if ((mode & 0x1FF) === 0x1FC) return 'void extent';
  if ((mode & 3) === 0) return `block mode ${mode.toString(16)}: a grid wider than a 4x4 block`;
  const R = bitsOf(mode, 4) | (mode & 3) << 1, H = bitsOf(mode, 9), D = bitsOf(mode, 10);
  let A = bitsOf(mode, 5, 2), B = bitsOf(mode, 7, 2), gw, gh;
  switch (bitsOf(mode, 2, 2)) {
    case 0: gw = B + 4; gh = A + 2; break;
    case 1: gw = B + 8; gh = A + 2; break;
    case 2: gw = A + 2; gh = B + 8; break;
    default: B &= 1; if (bitsOf(mode, 8)) { gw = B + 2; gh = A + 2; } else { gw = A + 2; gh = B + 6; }
  }
  if (gw > 4 || gh > 4) return `block mode ${mode.toString(16)}: a ${gw}x${gh} grid`;
  if (R < 2) return 'reserved weight range';
  const wr = RANGES[[[2, 3, 4, 5, 6, 8], [10, 12, 16, 20, 24, 32]][H].map(l => RANGES.findIndex(r => r[0] === l))[R - 2]];
  const planes = D + 1, nw = gw * gh * planes, wbits = iseBits(wr, nw);
  if (nw > 64 || wbits < 24 || wbits > 96) return `weights: ${nw}, ${wbits} bits`;
  if (read(11, 2) !== 0) return 'more than one partition';
  const cem = read(13, 4);
  if (![0, 4, 8, 12].includes(cem)) return `endpoint mode ${cem}`;
  const nv = 2 * ((cem >> 2) + 1), avail = 128 - 17 - wbits - (D ? 2 : 0);
  // the largest range (6 levels or more) whose nv values fit
  let cr = null;
  for (const r of RANGES) if (r[0] >= 6 && iseBits(r, nv) <= avail) cr = r;
  if (!cr) return 'no room for the endpoints';
  const v = readIse(read, 17, nv, cr).map(x => unquantColour(x, cr));
  let e0, e1;
  if (cem === 0) { e0 = [v[0], v[0], v[0], 255]; e1 = [v[1], v[1], v[1], 255]; }
  else if (cem === 4) { e0 = [v[0], v[0], v[0], v[2]]; e1 = [v[1], v[1], v[1], v[3]]; }
  else {
    const a0 = cem === 12 ? v[6] : 255, a1 = cem === 12 ? v[7] : 255;
    if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) { e0 = [v[0], v[2], v[4], a0]; e1 = [v[1], v[3], v[5], a1]; }
    else {
      // blue contraction, the endpoints swapped
      const bc = (r, g, b, a) => [(r + b) >> 1, (g + b) >> 1, b, a];
      e0 = bc(v[1], v[3], v[5], a1); e1 = bc(v[0], v[2], v[4], a0);
    }
  }
  const ccs = D ? read(128 - wbits - 2, 2) : -1;
  // the weights, bit i of their sequence at bit 127 - i
  const wread = (pos, n) => { let x = 0; for (let i = 0; i < n; i++) x |= bit(127 - (pos + i)) << i; return x; };
  const w = readIse(wread, 0, nw, wr).map(x => unquantWeight(x, wr));
  // the infill from the grid to the 4x4 texels
  const Ds = Math.floor((1024 + 2) / 3), out = [];
  const gwt = (p, x, y) => (x < gw && y < gh ? w[(y * gw + x) * planes + p] : 0);
  for (let t = 0; t < 4; t++) {
    for (let s = 0; s < 4; s++) {
      const gs = (Ds * s * (gw - 1) + 32) >> 6, gt = (Ds * t * (gh - 1) + 32) >> 6;
      const js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
      const w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
      const wt = p => (gwt(p, js, jt) * w00 + gwt(p, js + 1, jt) * w01 + gwt(p, js, jt + 1) * w10 + gwt(p, js + 1, jt + 1) * w11 + 8) >> 4;
      const wp0 = wt(0), wp1 = D ? wt(1) : 0;
      out.push([0, 1, 2, 3].map(c => {
        const wc = c === ccs ? wp1 : wp0, C0 = e0[c] << 8 | e0[c], C1 = e1[c] << 8 | e1[c];
        return (C0 * (64 - wc) + C1 * wc + 32) >> 6;
      }));
    }
  }
  return out;
}

export const to8 = C => Math.round(C / 65535 * 255);
