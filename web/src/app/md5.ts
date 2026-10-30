// MD5 (RFC 1321), which the browser's WebCrypto does not have: Most Wanted's saves hold two (docs/mw/save-format.md
// section 3).  Pure, no imports.

const S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
const K = new Uint32Array(64);
for (let i = 0; i < 64; i++) K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32) >>> 0;

/** the 16-byte MD5 digest of d */
export function md5(d: Uint8Array): Uint8Array {
  const n = d.length, blocks = ((n + 8) >>> 6) + 1;
  const m = new Uint8Array(blocks * 64);
  m.set(d);
  m[n] = 0x80;
  const dv = new DataView(m.buffer);
  dv.setUint32(blocks * 64 - 8, (n * 8) >>> 0, true);
  dv.setUint32(blocks * 64 - 4, Math.floor(n / 0x20000000), true);
  let a0 = 0x67452301, b0 = 0xEFCDAB89, c0 = 0x98BADCFE, d0 = 0x10325476;
  const w = new Uint32Array(16);
  for (let o = 0; o < m.length; o += 64) {
    for (let j = 0; j < 16; j++) w[j] = dv.getUint32(o + j * 4, true);
    let a = a0, b = b0, c = c0, e = d0;
    for (let i = 0; i < 64; i++) {
      let f: number, g: number;
      if (i < 16) { f = (b & c) | (~b & e); g = i; }
      else if (i < 32) { f = (e & b) | (~e & c); g = (5 * i + 1) & 15; }
      else if (i < 48) { f = b ^ c ^ e; g = (3 * i + 5) & 15; }
      else { f = c ^ (b | ~e); g = (7 * i) & 15; }
      const x = (a + f + K[i] + w[g]) >>> 0, s = S[(i >>> 4) * 4 + (i & 3)];
      a = e; e = c; c = b;
      b = (b + ((x << s) | (x >>> (32 - s)))) >>> 0;
    }
    a0 = (a0 + a) >>> 0; b0 = (b0 + b) >>> 0; c0 = (c0 + c) >>> 0; d0 = (d0 + e) >>> 0;
  }
  const out = new Uint8Array(16), ov = new DataView(out.buffer);
  ov.setUint32(0, a0, true); ov.setUint32(4, b0, true); ov.setUint32(8, c0, true); ov.setUint32(12, d0, true);
  return out;
}
