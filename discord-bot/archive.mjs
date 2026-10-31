// Every message the bot sees, in every channel it can read, one JSON line each in <state>/archive/<day>.jsonl, kept 30
// days.  The bot only answers in forum threads, but the model can search the rest (its last 7 days): the same problem
// reported in chat, or an answer someone already gave.  The rest is for the developer's own reading.
import fs from 'node:fs';
import path from 'node:path';

const KEEP_DAYS = 30;                       // deletions are never seen: a deleted message stays this long

export class Archive {
  constructor(dir) {
    this.dir = dir;
    fs.mkdirSync(dir, { recursive: true });
    this.prune();
  }

  file(day) { return path.join(this.dir, `${day}.jsonl`); }

  /** one message: { id, at, channel, thread, author, text, files } */
  add(m) {
    const day = m.at.slice(0, 10);
    if (day !== this.today) { this.today = day; this.prune(); }
    fs.appendFileSync(this.file(day), JSON.stringify(m) + '\n');
  }

  prune() {
    const cut = new Date(Date.now() - KEEP_DAYS * 86400_000).toISOString().slice(0, 10);
    for (const f of fs.readdirSync(this.dir)) if (f.endsWith('.jsonl') && f.slice(0, 10) < cut) fs.rmSync(path.join(this.dir, f));
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
