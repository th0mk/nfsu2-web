// stage-extras.mjs -- the folder pack-dat.mjs packs: the PC game folder (hard links, nothing copied) plus the pieces of the
// server's install that a Firefox/Chrome "nfsu2.xyz" storage export holds and the PC folder lacks: the music (pfdata), the English
// voices (speech), the Corsa and the Peugeot 106.  Not the upscaled HUD (TEXPACKS), the language packs or the AV1 movies.
//   node runtime/switch/stage-extras.mjs <game folder> <storage folder (https+++nfsu2.xyz)> <staging folder>
// The browser's cache is OPFS: fs/metadata.sqlite names the files, each file is fs/<2 letters>/<base32 id>, and a cache file is
// named <path with / as _>.<size>.<mtime>; the manifest (Cache API entry /data-manifest, snappy framed) gives the true paths.
import fs from 'node:fs';
import path from 'node:path';
import { DatabaseSync } from 'node:sqlite';

const [game, store, stage] = process.argv.slice(2);
if (!game || !store || !stage) { console.error('usage: node stage-extras.mjs <game folder> <storage folder> <staging folder>'); process.exit(2); }

// ---- snappy (framing format, as Firefox's Cache API stores bodies) ----
function snappyRaw(b) {
  let p = 0, len = 0, sh = 0;
  for (;;) { const c = b[p++]; len |= (c & 127) << sh; if (c < 128) break; sh += 7; }
  const o = Buffer.alloc(len); let q = 0;
  while (p < b.length) {
    const t = b[p++], k = t & 3;
    if (k === 0) { let n = t >> 2; if (n >= 60) { const e = n - 59; n = 0; for (let i = 0; i < e; i++) n |= b[p++] << (8 * i); } n++; b.copy(o, q, p, p + n); p += n; q += n; }
    else {
      let n, off;
      if (k === 1) { n = ((t >> 2) & 7) + 4; off = ((t >> 5) << 8) | b[p++]; }
      else if (k === 2) { n = (t >> 2) + 1; off = b.readUInt16LE(p); p += 2; }
      else { n = (t >> 2) + 1; off = b.readUInt32LE(p); p += 4; }
      for (let i = 0; i < n; i++) { o[q] = o[q - off]; q++; }
    }
  }
  return o;
}
function snappyFramed(b) {
  let p = 0; const out = [];
  while (p < b.length) {
    const t = b[p], l = b[p + 1] | (b[p + 2] << 8) | (b[p + 3] << 16), body = b.subarray(p + 4, p + 4 + l); p += 4 + l;
    if (t === 0) out.push(snappyRaw(body.subarray(4))); else if (t === 1) out.push(Buffer.from(body.subarray(4)));
  }
  return Buffer.concat(out);
}

// ---- the manifest: Cache API entry https://nfsu2.xyz/data-manifest ----
const cdb = new DatabaseSync(path.join(store, 'cache/caches.sqlite'), { readOnly: true });
const row = cdb.prepare("select response_body_id id from entries where request_url_no_query like '%/data-manifest'").get();
if (!row) throw new Error('no /data-manifest in the Cache API');
let body = null;
for (const d of fs.readdirSync(path.join(store, 'cache/morgue'))) {
  const f = path.join(store, 'cache/morgue', d, row.id + '.final');
  if (fs.existsSync(f)) { body = snappyFramed(fs.readFileSync(f)); break; }
}
if (!body) throw new Error('the manifest body is not in the morgue');
const manifest = JSON.parse(body.toString('utf8'));
const byFlat = new Map();
for (const f of manifest.files) if (!f.d) byFlat.set(f.p.replace(/\//g, '_').toLowerCase() + '.' + f.s, f.p);

// ---- OPFS: the cache folder's files ----
const db = new DatabaseSync(path.join(store, 'fs/metadata.sqlite'), { readOnly: true });
const hex = (b) => Buffer.from(b).toString('hex');
const u16 = (b) => Buffer.from(b).toString('utf16le');
const dirs = new Map(), parent = new Map();
for (const d of db.prepare('select handle,name from Directories').all()) dirs.set(hex(d.handle), u16(d.name));
for (const e of db.prepare('select handle,parent from Entries').all()) parent.set(hex(e.handle), e.parent ? hex(e.parent) : null);
const b32 = (b) => { const A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'; let bits = 0, v = 0, o = ''; for (const x of Buffer.from(b)) { v = (v << 8) | x; bits += 8; while (bits >= 5) { o += A[(v >>> (bits - 5)) & 31]; bits -= 5; } v &= (1 << bits) - 1; } if (bits) o += A[(v << (5 - bits)) & 31]; return o; };
const dirPath = (h) => { const p = []; for (let c = h; c; c = parent.get(c)) p.unshift(dirs.get(c)); return p.join('/'); };
const cache = new Map();                       // "name.size.mtime" -> { file, have }
for (const r of db.prepare('select f.handle, f.name, m.fileId from Files f join MainFiles m on m.handle=f.handle').all()) {
  if (dirPath(parent.get(hex(r.handle))) !== 'root/cache') continue;
  const id = b32(r.fileId);
  cache.set(u16(r.name), path.join(store, 'fs', id.slice(0, 2), id));
}

// ---- the staging folder: the game's files as hard links ----
const SKIP_TOP = new Set(['uninstall', 'support']);
let linked = 0;
(function walk(src, dst, top) {
  fs.mkdirSync(dst, { recursive: true });
  for (const e of fs.readdirSync(src, { withFileTypes: true })) {
    if (top && SKIP_TOP.has(e.name.toLowerCase())) continue;
    if (e.isDirectory()) walk(path.join(src, e.name), path.join(dst, e.name), false);
    else if (e.isFile()) { const t = path.join(dst, e.name); if (!fs.existsSync(t)) { fs.linkSync(path.join(src, e.name), t); linked++; } }
  }
})(game, stage, true);

// ---- the extras ----
const WANT = (p) => { const q = p.toLowerCase(); return q.startsWith('pfdata/') || q.startsWith('speech/') || q.startsWith('cars/corsa/') || q.startsWith('cars/peugot106/'); };
let added = 0, bytes = 0; const problems = [];
for (const [name, file] of cache) {
  if (name.endsWith('.have')) continue;
  const m = name.match(/^(.*)\.(\d+)\.(\d+)$/); if (!m) continue;
  const p = byFlat.get(m[1].toLowerCase() + '.' + m[2]); if (!p || !WANT(p)) continue;
  const have = cache.get(name + '.have');
  const size = fs.statSync(file).size;
  if (size !== +m[2]) { problems.push(`${p}: ${size} bytes, expected ${m[2]}`); continue; }
  if (have && fs.readFileSync(have).some((v) => !v)) { problems.push(`${p}: blocks missing in the browser's cache`); continue; }
  const t = path.join(stage, ...p.split('/'));
  fs.mkdirSync(path.dirname(t), { recursive: true });
  fs.copyFileSync(file, t);
  added++; bytes += size; console.log('  +', p, size);
}
console.log(`staged: ${linked} files linked from the game folder, ${added} added from the browser's cache (${(bytes / 1e6).toFixed(0)} MB)`);
if (problems.length) { console.error('PROBLEMS:\n' + problems.join('\n')); process.exit(1); }
