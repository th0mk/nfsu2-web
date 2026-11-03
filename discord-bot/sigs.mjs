// Where a log's crashes and freezes stop: the functions at the top of each stack.  The same exit code (134) can be two
// bugs and a freeze and a crash in the same functions one, so a look matches threads to reports by these, not by the
// words of a post.

import fs from 'node:fs';
import path from 'node:path';

const TOP = 3;
// frames of the port's own glue, the same under every bug
const GLUE = new Set(['dispatch_indirect', 'guest_hook', 'roam_hook', 'tramp_main', 'guest_main_thread']);

/** a log's text -> its stack tops, 'crash a < b < c' (a TRAP's wasm stack) or 'freeze a < b < c' (a watchdog report's
 *  thread that holds the GIL), each once, in the log's order */
export function stackTops(text) {
  const lines = text.split('\n'), out = [];
  const add = (kind, fns) => {
    const s = `${kind} ${fns.join(' < ')}`;
    if (fns.length >= 2 && !out.includes(s)) out.push(s);
  };
  for (let i = 0; i < lines.length; i++) {
    if (lines[i].includes('TRAP in thread')) {
      // Chrome: "    at nfsu2.wasm.sub_00433ad0 (https://…:wasm-function[6150]:0x91068d)";
      // Firefox: "sub_00433ad0@https://…"
      const fns = [];
      for (let j = i + 1; j < Math.min(lines.length, i + 40) && fns.length < TOP; j++) {
        const m = /(?:nfsu2\.wasm\.)?([A-Za-z_$][\w$]*)\s*[(@][^\n]*wasm-function\[/.exec(lines[j]);
        if (m && !GLUE.has(m[1]) && !fns.includes(m[1])) fns.push(m[1]);
      }
      add('crash', fns);
    } else if (lines[i].includes('[watchdog] no Present')) {
      // "  thread 0 tid=1 (holds the GIL): …", then its frames
      // "[ 0] +0x0000  return 0x00433c9a   (sub_00433ad0 + 0x1ca)"
      let j = i + 1;
      const stop = l => l.includes('holds the GIL') || l.includes('[watchdog]');
      while (j < lines.length && j < i + 200 && !stop(lines[j])) j++;
      if (!lines[j]?.includes('holds the GIL')) continue;
      const fns = [];
      for (j++; j < lines.length && fns.length < TOP; j++) {
        if (/\bthread \d+ tid=|critical sections|\[watchdog\]/.test(lines[j])) break;
        const m = /return 0x[0-9a-f]+\s+\(([\w$]+) \+ 0x/.exec(lines[j]);
        if (m && !GLUE.has(m[1]) && !fns.includes(m[1])) fns.push(m[1]);
      }
      add('freeze', fns);
    }
  }
  return out;
}

/** the stack tops of a thread's text files (a look's work folder, "<n>-<name>" in the thread's order): the newest 6 */
export function folderTops(dir) {
  const out = [];
  const files = fs.readdirSync(dir).sort((a, b) => parseInt(a) - parseInt(b));
  for (const f of files) {
    let text;
    try { text = fs.readFileSync(path.join(dir, f), 'utf8'); } catch { continue; }
    for (const s of stackTops(text)) if (!out.includes(s)) out.push(s);
  }
  return out.slice(-6);
}

const fnsOf = top => top.split(' ').slice(1).filter(x => x !== '<');
/** two stack tops are the same place when they share two functions (a watchdog report names the stuck function in
 *  one report and starts at its caller in the next) */
export function samePlace(a, b) {
  const fb = new Set(fnsOf(b));
  return fnsOf(a).filter(f => fb.has(f)).length >= 2;
}
