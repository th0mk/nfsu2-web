// Where a log's crashes and freezes stop: the functions at the top of each stack.  The same exit code (134) can be two
// bugs and a freeze and a crash in the same functions one, so a look matches threads to reports by these, not by the
// words of a post.

import fs from 'node:fs';
import path from 'node:path';

const TOP = 3;
// frames of the port's own glue, the same under every bug
const GLUE = new Set(['dispatch_indirect', 'guest_hook', 'roam_hook', 'tramp_main', 'guest_main_thread']);
// a stack frame's line is ~100 characters: a longer one is none.  A log is the poster's text, read on the bot's only
// thread: every match here takes one pass over a line, so a log of long lines can't stall the bot
const MAX_LINE = 1000;

/** a crash stack's line -> its function, or null.  Chrome: "    at nfsu2.wasm.sub_00433ad0 (https://…:wasm-function[6150]:0x91068d)";
 *  Firefox: "sub_00433ad0@https://…".  The first name (letters, digits, _ and $, not starting with a digit) right
 *  before "(" or "@" with "wasm-function[" after it */
function crashFrame(line) {
  const end = line.length <= MAX_LINE ? line.lastIndexOf('wasm-function[') : -1;
  if (end < 0) return null;
  // each run of name characters once, whole: the lookahead can't fail, so nothing is tried twice
  for (const m of line.slice(0, end).matchAll(/[\w$]+(?=(\s*[(@])?)/g)) {
    const name = m[1] && m[0].replace(/^\d+/, '');
    if (name) return name;
  }
  return null;
}

/** a log's text -> its stack tops, 'crash a < b < c' (a TRAP's wasm stack) or 'freeze a < b < c' (a watchdog report's
 *  thread that holds the GIL), each once, in the log's order */
export function stackTops(text) {
  const lines = text.split('\n'), out = new Set();
  const add = (kind, fns) => { if (fns.length >= 2) out.add(`${kind} ${fns.join(' < ')}`); };
  const frames = new Map();                      // line -> its function: the 40 lines after a TRAP can hold more TRAPs
  const frame = j => { if (!frames.has(j)) frames.set(j, crashFrame(lines[j])); return frames.get(j); };
  for (let i = 0; i < lines.length; i++) {
    if (lines[i].includes('TRAP in thread')) {
      const fns = [];
      for (let j = i + 1; j < Math.min(lines.length, i + 40) && fns.length < TOP; j++) {
        const fn = frame(j);
        if (fn && !GLUE.has(fn) && !fns.includes(fn)) fns.push(fn);
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
        const m = lines[j].length <= MAX_LINE && /return 0x[0-9a-f]+\s+\(([\w$]+) \+ 0x/.exec(lines[j]);
        if (m && !GLUE.has(m[1]) && !fns.includes(m[1])) fns.push(m[1]);
      }
      add('freeze', fns);
    }
  }
  return [...out];
}

/** the stack tops of a thread's text files (a look's work folder, "<n>-<name>" in the thread's order): the newest 6 */
export function folderTops(dir) {
  const out = new Set();
  const files = fs.readdirSync(dir).sort((a, b) => parseInt(a) - parseInt(b));
  for (const f of files) {
    let text;
    try { text = fs.readFileSync(path.join(dir, f), 'utf8'); } catch { continue; }
    for (const s of stackTops(text)) out.add(s);
  }
  return [...out].slice(-6);
}

export const fnsOf = top => top.split(' ').slice(1).filter(x => x !== '<');
/** two stack tops are the same place when they share two functions (a watchdog report names the stuck function in
 *  one report and starts at its caller in the next) */
export function samePlace(a, b) {
  const fb = new Set(fnsOf(b));
  return fnsOf(a).filter(f => fb.has(f)).length >= 2;
}

/** the tops of a thread's logs that are the crash or freeze its look says the thread is about (the verdict's `crash`,
 *  one of the tops as the context listed them): what other threads are matched against (st.crash).  "" = not about a
 *  crash: none.  A log can hold another game's crash the post never mentions, and a later log another bug: neither is
 *  the thread's.  Without a verdict's word (undefined), every top */
export function crashOf(crash, tops) {
  if (typeof crash !== 'string') return [...tops];
  const c = crash.trim();
  if (!c) return [];
  const named = c.includes(' ') ? c : `crash ${c}`;      // "a < b < c" without its kind
  return tops.filter(t => t === c || samePlace(t, named));
}

