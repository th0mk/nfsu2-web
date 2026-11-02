// A small zip writer and reader for save backups: "stored" entries only
// (the saves are 55 KB profiles and a few small text files, compression
// would gain little) plus reading "deflated" entries through the browser's
// DecompressionStream, so a backup re-zipped by an archiver still imports.
// UTF-8 names (flag bit 11), no zip64 (a backup is far below 4 GB).

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
  return t;
})();
export function crc32(b: Uint8Array): number {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < b.length; i++) c = CRC_TABLE[(c ^ b[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

export interface ZipEntry { name: string; data: Uint8Array; mtime?: number }

function dosTime(ms: number): { time: number; date: number } {
  const d = new Date(ms);
  const y = Math.max(1980, d.getFullYear());
  return { time: (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1),
           date: ((y - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate() };
}

/** comment: the archive's comment (its end record's: every reader skips it) */
export function zip(entries: ZipEntry[], comment = ''): Uint8Array {
  const te = new TextEncoder();
  const locals: Uint8Array[] = [], centrals: Uint8Array[] = [];
  let off = 0;
  for (const e of entries) {
    const name = te.encode(e.name), crc = crc32(e.data), { time, date } = dosTime(e.mtime ?? Date.now());
    const lh = new Uint8Array(30 + name.length), lv = new DataView(lh.buffer);
    lv.setUint32(0, 0x04034b50, true); lv.setUint16(4, 20, true); lv.setUint16(6, 0x0800, true); lv.setUint16(8, 0, true);
    lv.setUint16(10, time, true); lv.setUint16(12, date, true); lv.setUint32(14, crc, true);
    lv.setUint32(18, e.data.length, true); lv.setUint32(22, e.data.length, true); lv.setUint16(26, name.length, true);
    lh.set(name, 30);
    const ch = new Uint8Array(46 + name.length), cv = new DataView(ch.buffer);
    cv.setUint32(0, 0x02014b50, true); cv.setUint16(4, 0x031E, true); cv.setUint16(6, 20, true); cv.setUint16(8, 0x0800, true);
    cv.setUint16(12, time, true); cv.setUint16(14, date, true); cv.setUint32(16, crc, true);
    cv.setUint32(20, e.data.length, true); cv.setUint32(24, e.data.length, true); cv.setUint16(28, name.length, true);
    cv.setUint32(38, (0o100644 << 16) >>> 0, true); cv.setUint32(42, off, true);
    ch.set(name, 46);
    locals.push(lh, e.data); centrals.push(ch);
    off += lh.length + e.data.length;
  }
  const cdSize = centrals.reduce((a, c) => a + c.length, 0);
  const note = te.encode(comment).subarray(0, 65535);
  const end = new Uint8Array(22 + note.length), ev = new DataView(end.buffer);
  ev.setUint32(0, 0x06054b50, true); ev.setUint16(8, entries.length, true); ev.setUint16(10, entries.length, true);
  ev.setUint32(12, cdSize, true); ev.setUint32(16, off, true); ev.setUint16(20, note.length, true);
  end.set(note, 22);
  const out = new Uint8Array(off + cdSize + end.length);
  let p = 0;
  for (const b of [...locals, ...centrals, end]) { out.set(b, p); p += b.length; }
  return out;
}

export class ZipError extends Error {}

/** A zip's archive comment ('' for none, or no zip). */
export function zipComment(buf: Uint8Array): string {
  const v = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  for (let i = buf.length - 22; i >= Math.max(0, buf.length - 22 - 65535); i--)
    if (v.getUint32(i, true) === 0x06054b50) { const n = v.getUint16(i + 20, true); return i + 22 + n <= buf.length ? new TextDecoder().decode(buf.subarray(i + 22, i + 22 + n)) : ''; }
  return '';
}

/**
 * A deflated entry, inflated into a buffer of its declared size: a stream that yields more is stopped there
 * (a zip bomb declares a few bytes and inflates to gigabytes; the whole stream used to be inflated into memory
 * before the size was compared, so the declared-size limits never held: the audit's PAGE-4).
 */
async function inflate(raw: Uint8Array, usize: number, name: string): Promise<Uint8Array> {
  const out = new Uint8Array(usize);
  const rd = new Blob([raw as BlobPart]).stream().pipeThrough(new DecompressionStream('deflate-raw')).getReader();
  let n = 0;
  try {
    for (;;) {
      const { done, value } = await rd.read();
      if (done) break;
      if (n + value.length > usize) throw new ZipError(`entry ${name} is larger than it says`);
      out.set(value, n); n += value.length;
    }
  } catch (e) {
    void rd.cancel().catch(() => {});
    if (e instanceof ZipError) throw e;
    throw new ZipError(`entry ${name} is damaged (${e instanceof Error ? e.message : e})`);
  }
  return n === usize ? out : out.subarray(0, n);
}

/** The file entries of a zip (directories skipped).  Checks every CRC.
 *  accept(name, size) sees each entry's declared size before it is inflated;
 *  a refused one is listed in `refused` and never decompressed, and no entry
 *  is inflated past the size it declares. */
export async function unzip(buf: Uint8Array, accept?: (name: string, size: number) => boolean, refused: string[] = []): Promise<ZipEntry[]> {
  const v = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  let eocd = -1;
  for (let i = buf.length - 22; i >= Math.max(0, buf.length - 22 - 65535); i--) if (v.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  if (eocd < 0) throw new ZipError('not a zip file');
  const n = v.getUint16(eocd + 10, true);
  let p = v.getUint32(eocd + 16, true);
  const td = new TextDecoder();
  const out: ZipEntry[] = [];
  for (let i = 0; i < n; i++) {
    if (p + 46 > buf.length || v.getUint32(p, true) !== 0x02014b50) throw new ZipError('damaged zip directory');
    const method = v.getUint16(p + 10, true), crc = v.getUint32(p + 16, true);
    const csize = v.getUint32(p + 20, true), usize = v.getUint32(p + 24, true);
    const nl = v.getUint16(p + 28, true), xl = v.getUint16(p + 30, true), cl = v.getUint16(p + 32, true);
    const lo = v.getUint32(p + 42, true);
    const name = td.decode(buf.subarray(p + 46, p + 46 + nl));
    p += 46 + nl + xl + cl;
    if (name.endsWith('/')) continue;
    if (accept && !accept(name, usize)) { refused.push(name); continue; }
    if (lo + 30 > buf.length || v.getUint32(lo, true) !== 0x04034b50) throw new ZipError(`damaged entry ${name}`);
    const ds = lo + 30 + v.getUint16(lo + 26, true) + v.getUint16(lo + 28, true);
    if (ds + csize > buf.length) throw new ZipError(`truncated entry ${name}`);
    const raw = buf.subarray(ds, ds + csize);
    let data: Uint8Array;
    if (method === 0) {
      if (csize !== usize) throw new ZipError(`entry ${name} is damaged (size)`);
      data = raw.slice();
    } else if (method === 8) data = await inflate(raw, usize, name);
    else throw new ZipError(`entry ${name} uses compression method ${method}`);
    if (data.length !== usize || crc32(data) !== crc) throw new ZipError(`entry ${name} is damaged (CRC)`);
    const dt = v.getUint16(p - nl - xl - cl - 46 + 14, true), tm = v.getUint16(p - nl - xl - cl - 46 + 12, true);
    const mtime = new Date(1980 + (dt >> 9), ((dt >> 5) & 15) - 1, dt & 31, tm >> 11, (tm >> 5) & 63, (tm & 31) * 2).getTime();
    out.push({ name, data, mtime });
  }
  return out;
}

/** The file names in a zip, without inflating anything. */
export async function listZip(buf: Uint8Array): Promise<string[]> {
  const names: string[] = [];
  await unzip(buf, n => { names.push(n); return false; });
  return names;
}
