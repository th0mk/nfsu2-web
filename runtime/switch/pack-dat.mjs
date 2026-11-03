// pack-dat.mjs -- makes nfsu2.dat (the layout is in dat_index.h) from the game's folder.
// usage: node runtime/switch/pack-dat.mjs <game folder> <out.dat>
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';

const [src, out] = process.argv.slice(2);
if (!src || !out) { console.error('usage: node pack-dat.mjs <game folder> <out.dat>'); process.exit(2); }

// what the console does not need: the uninstaller, the support folder, the second exe, the page's own files
const SKIP_TOP = new Set(['uninstall', 'support']);
const SKIP_FILE = new Set(['uninstall.exe', 'speed22.exe']);
const files = [];
(function walk(dir, rel) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const r = rel ? rel + '/' + e.name : e.name;
    if (!rel && SKIP_TOP.has(e.name.toLowerCase())) continue;
    if (e.isDirectory()) walk(path.join(dir, e.name), r);
    else if (e.isFile() && !SKIP_FILE.has(e.name.toLowerCase())) files.push(r);
  }
})(src, '');
files.sort((a, b) => a.toLowerCase() < b.toLowerCase() ? -1 : 1);

const fd = fs.openSync(out, 'w');
fs.writeSync(fd, Buffer.alloc(64));                       // the header, written last
let pos = 64;
const idx = [];
const buf = Buffer.alloc(4 << 20);
for (const f of files) {
  const pad = (16 - (pos % 16)) % 16;
  if (pad) { fs.writeSync(fd, Buffer.alloc(pad)); pos += pad; }
  const ifd = fs.openSync(path.join(src, f), 'r');
  let crc = 0, size = 0, n;
  while ((n = fs.readSync(ifd, buf, 0, buf.length, null)) > 0) {
    crc = zlib.crc32(buf.subarray(0, n), crc);
    fs.writeSync(fd, buf, 0, n); size += n;
  }
  fs.closeSync(ifd);
  idx.push({ f, off: pos, size, crc });
  pos += size;
}
const parts = [];
for (const e of idx) {
  const p = Buffer.from(e.f, 'utf8');
  const h = Buffer.alloc(2 + p.length + 20);
  h.writeUInt16LE(p.length, 0); p.copy(h, 2);
  h.writeBigUInt64LE(BigInt(e.off), 2 + p.length); h.writeBigUInt64LE(BigInt(e.size), 10 + p.length); h.writeUInt32LE(e.crc >>> 0, 18 + p.length);
  parts.push(h);
}
const index = Buffer.concat(parts);
const indexOff = pos;
fs.writeSync(fd, index);
const total = indexOff + index.length;
const hdr = Buffer.alloc(64);
hdr.write('NFU2DAT1', 0, 'ascii'); hdr.writeUInt32LE(1, 8); hdr.writeUInt32LE(idx.length, 12);
hdr.writeBigUInt64LE(BigInt(indexOff), 16); hdr.writeBigUInt64LE(BigInt(index.length), 24); hdr.writeBigUInt64LE(BigInt(total), 32);
hdr.writeUInt32LE(zlib.crc32(index) >>> 0, 40);
fs.writeSync(fd, hdr, 0, 64, 0);
fs.closeSync(fd);
console.log(`${out}: ${idx.length} files, ${(total / 1048576).toFixed(1)} MB`);
