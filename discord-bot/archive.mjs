// Every message the bot sees, in every channel it can read, one JSON line each in <state>/archive/<day>.jsonl, kept 30
// days.  The bot only answers in forum threads, but the model can search the rest (its last 7 days): the same problem
// reported in chat, or an answer someone already gave.  The rest is for the developer's own reading.
//
// The messages' text files (logs, long pastes, chrome://gpu reports) and pictures are kept too, in
// <state>/archive/files/<day>/<message id>-<n>.txt.gz or .webp (n: the attachment's place in the line's `files`):
// text gzipped, pictures shrunk as the API shrinks a picture for the model.  At most FILES_CAP in all: past it the
// oldest days go first.  Videos, zips and the rest: their names only.
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { fitted } from './investigate.mjs';

const KEEP_DAYS = 30;                       // deletions are never seen: a deleted message stays this long
const FILES_CAP = 3 * 2 ** 30;              // the saved files of every day; ~120 MB a day (mostly pictures) in 2026-10
const MAX_TEXT = 10 << 20, MAX_PICTURE = 25 << 20;   // larger ones: their names only
const PICTURE_NAME = /\.(png|jpe?g|gif|webp|avif|heic)$/i;
const NOT_TEXT_NAME = /\.(zip|7z|rar|gz|tgz|exe|apk|msi|dmg|pdf|mp4|mov|webm|mkv|avi|mp3|ogg|wav|m4a)$/i;

/** what a message's attachment is kept as: 'picture', 'text' (if it turns out to be text) or null */
export function kind(a) {
  const type = (a.type ?? '').split(';')[0];
  if (type.startsWith('image/') || (!type && PICTURE_NAME.test(a.name))) return a.size <= MAX_PICTURE ? 'picture' : null;
  if (/^(video|audio)\//.test(type) || NOT_TEXT_NAME.test(a.name)) return null;
  return a.size <= MAX_TEXT ? 'text' : null;
}

/** a picture as WebP, the first frame of an animation, at the size the API would shrink it to; null: not a picture */
async function shrink(buf) {
  let sharp;
  try { sharp = (await import('sharp')).default; } catch (e) { console.error(`[archive] sharp: ${e.message}`); return null; }
  try {
    const m = await sharp(buf).metadata();
    const fh = m.pages > 1 && m.pageHeight ? m.pageHeight : m.height;
    const [w, h] = (m.orientation ?? 1) >= 5 ? [fh, m.width] : [m.width, fh];
    const [tw, th] = fitted(w, h);
    return await sharp(buf).rotate().resize(tw, th, { fit: 'fill' }).webp({ quality: 80 }).toBuffer();
  } catch { return null; }
}

/** bytes of the files in a folder */
function size(dir) {
  let n = 0;
  for (const f of fs.readdirSync(dir)) { try { n += fs.statSync(path.join(dir, f)).size; } catch { /* just removed */ } }
  return n;
}

export class Archive {
  constructor(dir, { cap = FILES_CAP } = {}) {
    this.dir = dir;
    this.filesDir = path.join(dir, 'files');
    this.cap = cap;
    this.queue = Promise.resolve();          // the files are fetched one at a time, after their message's line
    fs.mkdirSync(this.filesDir, { recursive: true });
    this.prune();
  }

  file(day) { return path.join(this.dir, `${day}.jsonl`); }

  /** one message: { id, at, channel, thread, author, text, files }; `attachments` ([{ url, name, size, type }], in the
   *  order of `files`): its text files and pictures, saved after the line */
  add(m, attachments = []) {
    const day = m.at.slice(0, 10);
    if (day !== this.today) { this.today = day; this.prune(); }
    fs.appendFileSync(this.file(day), JSON.stringify(m) + '\n');
    attachments.forEach((a, i) => {
      const k = kind(a);
      if (k) this.queue = this.queue.then(() => this.keep(day, `${m.id}-${i}`, a, k))
        .catch(e => console.error(`[archive] ${m.id} ${a.name}: ${e.message}`));
    });
  }

  /** one attachment into files/<day>/; written under another name first, so a reader never sees half a file */
  async keep(day, name, a, k) {
    if (this.bytes > this.cap) return;       // today's files alone past the cap: the rest of the day by name only
    const r = await fetch(a.url, { signal: AbortSignal.timeout(60_000) });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const data = Buffer.from(await r.arrayBuffer());
    let out, ext;
    if (k === 'picture') { out = await shrink(data); ext = 'webp'; }
    else if (!data.subarray(0, 8192).includes(0)) { out = zlib.gzipSync(data); ext = 'txt.gz'; }
    if (!out) return;                        // not a picture after all, or not text
    const dir = path.join(this.filesDir, day), to = path.join(dir, `${name}.${ext}`);
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(`${to}.part`, out);
    fs.renameSync(`${to}.part`, to);
    this.bytes += out.length;
    if (this.bytes > this.cap) this.trim();
  }

  /** the days older than KEEP_DAYS out, the messages and their files; then the files' bytes counted again */
  prune() {
    const cut = new Date(Date.now() - KEEP_DAYS * 86400_000).toISOString().slice(0, 10);
    for (const f of fs.readdirSync(this.dir)) if (f.endsWith('.jsonl') && f.slice(0, 10) < cut) fs.rmSync(path.join(this.dir, f));
    for (const d of fs.readdirSync(this.filesDir)) if (d < cut) fs.rmSync(path.join(this.filesDir, d), { recursive: true, force: true });
    this.bytes = fs.readdirSync(this.filesDir).reduce((n, d) => n + size(path.join(this.filesDir, d)), 0);
    if (this.bytes > this.cap) this.trim();
  }

  /** the oldest days' files out until FILES_CAP holds; today's stay */
  trim() {
    for (const d of fs.readdirSync(this.filesDir).sort()) {
      if (this.bytes <= this.cap || d >= (this.today ?? new Date().toISOString().slice(0, 10))) break;
      const at = path.join(this.filesDir, d);
      this.bytes -= size(at);
      fs.rmSync(at, { recursive: true, force: true });
      console.log(`[archive] files of ${d} removed: past ${Math.round(this.cap / 2 ** 20)} MB`);
    }
  }

  /** a message of the staff channel: marked when saved, or (saved before that) in the channel of its name */
  staff(m) { return !!m.staff || (!!this.staffName && m.channel === this.staffName); }

  /** messages of the last `days` days whose text, author or file names match `re`, oldest first */
  search(re, days, limit) {
    const cut = new Date(Date.now() - days * 86400_000).toISOString();
    const hits = [];
    let more = 0;
    const files = fs.readdirSync(this.dir).filter(f => f.endsWith('.jsonl') && f.slice(0, 10) >= cut.slice(0, 10)).sort();
    for (const f of files) {
      for (const line of fs.readFileSync(path.join(this.dir, f), 'utf8').split('\n')) {
        if (!line) continue;
        let m; try { m = JSON.parse(line); } catch { continue; }
        if (m.at < cut || !re.test(`${m.author}\n${m.text}\n${(m.files ?? []).join(' ')}`)) continue;
        if (hits.length < limit) hits.push(m); else more++;
      }
    }
    return { hits, more };
  }
}
