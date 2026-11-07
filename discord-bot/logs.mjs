// A page log's gist, made by code before a look: each game's version, browser, device and graphics, the settings the
// player changed, how each game ended, and the lines that say something went wrong (each kind once, with its line
// number and how often it came).  Looks spent most of their turns finding these with searches, the same ones in
// every log (Nov 5: looks that ran their 8 turns were 15% of the looks and 43% of the money, nearly all of them
// reading logs); with the gist in the thread, a look reads the log itself only for what the gist does not settle.
import { stackTops } from './sigs.mjs';

const MAX_LINE = 1000;                           // a longer line is no line of the page's: never matched (a pasted blob)
const KINDS = 14;                                // problem kinds shown per game, the first ones in the log
const SHOWN = 220;                               // characters of a line shown
const TAIL = 4;                                  // a game's last lines shown

// the lines that say something went wrong, as the page and the game write them (web/src, runtime/)
const PROBLEM = new RegExp([
  'TRAP in thread', '\\[watchdog\\] no Present', 'device lost', 'graphics stopped', 'graphics could not', 'could not start',
  'could not attach', '\\[app\\] warning ', 'exited with code', '\\bGPU-[A-Z]', 'DEVICE_HUNG', 'out of (device )?memory',
  'still no switch', 'not reachable', 'lost the switch', 'switch connection', '\\bunderrun', 'SAVES-', 'STORAGE-',
  'FAILED', '\\bfailed\\b', '\\berror\\b', 'no room', 'refused', 'blocked',
].join('|'), 'i');
// lines that match PROBLEM but say nothing went wrong: the counters of the virtual LAN print "send errors 0" every minute
const BENIGN = /virtual LAN stats|\bstats: |\bno (worklet )?underrun|errors? 0\b|0 errors|no errors|without errors|\[autotest\]/i;

const tag = l => l.replace(/^\[\d+!?\] /, '');   // the game's thread tag ("[12!] ")
const cut = s => (s.length > SHOWN ? `${s.slice(0, SHOWN)}…` : s);
/** a line's kind: its words without the numbers, addresses and quoted names that change from line to line */
const kindOf = l => tag(l).replace(/0x[0-9a-f]+|\d+(\.\d+)?/gi, '#').replace(/"[^"]*"|'[^']*'/g, '…').slice(0, 90);

/** is this text a log the page wrote (F9, Download log)? */
export function isPageLog(text) {
  return /^=== NFSU2 web, /m.test(text.slice(0, 400_000));
}

/** the gist of a page log, as lines (empty for a text that is no page log) */
export function logGist(text) {
  if (!isPageLog(text)) return [];
  const lines = text.split('\n');
  // the log's parts: "=== the last game's log ..." (an earlier game the page kept), "=== this page's log ===", and
  // a "=== NFSU2 web, <time>, game <rev>, server <kind> ===" at each game's start
  const games = [];
  let part = '', g = null;
  for (let i = 0; i < lines.length; i++) {
    const l = lines[i].length <= MAX_LINE ? lines[i] : '';
    if (l.startsWith("=== the last game's log")) { part = 'kept'; g = null; continue; }
    if (l.startsWith("=== this page's log")) { part = 'page'; g = null; continue; }
    const head = /^=== NFSU2 web, (\S+), game (\S+), server (\S+) ===/.exec(l);
    if (head) {
      g = { at: head[1].slice(0, 16).replace('T', ' '), rev: head[2], server: head[3], line: i + 1, part, from: i,
        what: [], kinds: new Map(), last: [], set: [] };
      games.push(g);
      continue;
    }
    const t = tag(l);
    // how the game before this page's ended: the page's first lines, before its own game starts
    if (t.startsWith('[app] the last game (')) { (g ?? games.at(-1) ?? { what: [] }).what.push(`line ${i + 1}: ${cut(t)}`); continue; }
    if (!g) continue;
    g.to = i;
    if (t.startsWith('[app] save folder: ')) g.browser = t.replace(/^\[app\] save folder: [^;]*;[^;]*;\s*/, '').replace(/; movies in \w+$/, '');
    else if (t.startsWith('[app] device: ')) g.device = t.slice('[app] device: '.length);
    else if (t.startsWith('[app] settings.ini:')) {
      // a name, crew or flag says who they are, nothing about the problem (older logs have them as typed)
      const m = /-> ([A-Z0-9_]+)=(.*)$/.exec(t);
      if (m) g.set.push(/NAME|CREW|FLAG/.test(m[1]) ? `${m[1]}=…` : `${m[1]}=${m[2]}`);
    }
    else if (/^\[app\] (the game exited with code|the page was hidden for|away: )/.test(t)) g.what.push(`line ${i + 1}: ${cut(t)}`);
    if (PROBLEM.test(t) && !BENIGN.test(t)) {
      const k = kindOf(t), had = g.kinds.get(k);
      if (had) { had.n++; had.lastLine = i + 1; } else g.kinds.set(k, { line: i + 1, text: t, n: 1, lastLine: i + 1 });
    }
    if (t.trim()) g.last.push(i);
  }
  if (!games.length) return [];

  const out = [];
  for (const [n, g] of games.entries()) {
    const which = g.part === 'kept' ? 'an earlier game, kept by the page' : n === games.length - 1 ? 'the newest game' : 'a game';
    out.push(`Game ${n + 1} (${which}), line ${g.line}: ${g.at} UTC, game ${g.rev}, server ${g.server}`);
    if (g.browser) out.push(`  browser: ${cut(g.browser)}`);
    if (g.device) out.push(`  device: ${cut(g.device)}`);
    if (g.set.length) out.push(`  settings changed: ${g.set.join(', ').slice(0, 2 * SHOWN)}`);
    for (const w of g.what) out.push(`  ${w}`);
    const kinds = [...g.kinds.values()];
    if (kinds.length) {
      out.push(`  problem lines (${kinds.length > KINDS ? `the first ${KINDS} of ${kinds.length} kinds` : `${kinds.length} kind(s)`}):`);
      for (const k of kinds.slice(0, KINDS)) {
        out.push(`    line ${k.line}${k.n > 1 ? ` (x${k.n}, the last at line ${k.lastLine})` : ''}: ${cut(k.text)}`);
      }
    } else out.push('  problem lines: none');
    const tops = stackTops(lines.slice(g.from, (g.to ?? g.from) + 1).join('\n'));
    if (tops.length) out.push(`  where it crashed or froze: ${tops.join('; ')}`);
    const tail = g.last.slice(-TAIL);
    if (tail.length) out.push(`  its last lines (${tail[0] + 1}-${tail.at(-1) + 1}):`, ...tail.map(i => `    ${cut(tag(lines[i]))}`));
  }
  return out;
}
