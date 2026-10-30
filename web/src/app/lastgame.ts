// The log of a game the page did not get to end.  Safari stops a page whose memory it needs (an iPhone's, mostly while
// the game plays or soon after it went to the background) without any event, and the log (logs.ts) goes with it, just
// when a player would send it.  So while a game runs the page keeps a short copy (LogRing.snapshot) in localStorage:
// every 10 s when the log grew, at once when the page is hidden or shown, and with the game's end.  The next page finds
// a game that never got to its end and offers that copy with Download log (shell.ts).  Each copy also notes what the
// game held then (its textures, friends' cars): Safari gives the page no memory figure, and the renderer's line comes
// only once a minute.  The copy is kept as it would leave the browser (privacy.ts scrub): no names, no addresses.
import { STORAGE } from '@game/storage';
import type { LogRing } from './logs';

export interface LastGame {
  v: 1;
  /** when the game started (ms since the epoch) and the module's revision */
  started: number; rev: string;
  /** when the copy was last written, and whether the page was hidden then (in the background, on a phone) */
  saved: number; hidden: boolean;
  /** how it ended: the exit code and why ('exit', a stop's code, 'page closed'); null: it did not get to an end */
  ended: { code: number; why: string } | null;
  log: string;
  /** the log went through privacy.ts scrub (copies of pages before it did not) */
  scrubbed?: boolean;
  /** what the game held at that copy, "textures 412 MB (380 unpacked from DXT), 19 friends' cars, ..." ('': nothing known) */
  held?: string;
}

const KEY = STORAGE.LOCAL.LAST_GAME;
const EVERY_MS = 10_000;
/** the copy's size: localStorage holds a few MB for the whole page */
const BUDGET = 160 << 10;

export class LastGameKeeper {
  private rec: LastGame | null = null;
  private timer: ReturnType<typeof setInterval> | undefined;
  private written = -1;
  private failed = false;
  private held: () => string = () => '';
  /** scrub: the log as it may leave the browser (privacy.ts) */
  constructor(private save: string, private log: LogRing, private note: (s: string) => void, private scrub?: (text: string) => string) {}

  /** a game started: a copy now, then every EVERY_MS while the log grows; held: what the game holds now, for each copy */
  start(rev: string, held?: () => string) {
    this.rec = { v: 1, started: Date.now(), rev, saved: 0, hidden: document.hidden, ended: null, log: '' };
    this.held = held ?? (() => '');
    this.write();
    clearInterval(this.timer);
    this.timer = setInterval(() => { if (this.log.added !== this.written) this.write(); }, EVERY_MS);
  }
  /** the page was hidden or shown: a phone may stop a hidden page at any moment */
  hidden(h: boolean) {
    if (!this.rec || this.rec.ended) return;
    this.rec.hidden = h;
    this.write();
  }
  /** the game ended (why: 'exit', a stop's code) or the page is going away with the game running ('page closed') */
  end(code: number, why: string) {
    if (!this.rec || this.rec.ended) return;
    clearInterval(this.timer); this.timer = undefined;
    this.rec.ended = { code, why };
    this.write();
  }

  private write() {
    const r = this.rec!;
    r.saved = Date.now(); this.written = this.log.added;
    r.log = this.log.snapshot(BUDGET);
    if (this.scrub) { r.log = this.scrub(r.log); r.scrubbed = true; }
    // the copy at the end may come after the renderer went: the last figures seen then
    try { r.held = this.held() || r.held || ''; } catch { /* the last ones */ }
    try { localStorage.setItem(KEY + this.save, JSON.stringify(r)); }
    catch (e) {
      // storage off or full: the log stays in memory as before
      if (!this.failed) { this.failed = true; this.note(`[app] the log's copy for a stopped page could not be kept: ${e}`); }
    }
  }
}

/** The last game of this save folder, when it did not end as the game ends (a normal close or a Stop on a page that went
 *  on): stopped with the page, or stopped for a reason (GPU-LOST, a crash) by a page that has been reloaded since. */
export function lastGameToOffer(save: string): LastGame | null {
  let r: LastGame | null = null;
  try { r = JSON.parse(localStorage.getItem(KEY + save) ?? 'null'); } catch { return null; }
  if (!r || r.v !== 1 || typeof r.log !== 'string' || !r.log) return null;
  if (r.ended && r.ended.why === 'exit' && (r.ended.code === 0 || r.ended.code === -1)) return null;
  return r;
}

/** how it ended, for the start page: the browser stopped the page ('stopped'), the player reloaded or closed it
 *  ('left'), or the game stopped for a reason and the page was reloaded since ('error') */
export function lastGameEnd(r: LastGame): 'stopped' | 'left' | 'error' {
  return !r.ended ? 'stopped' : r.ended.why === 'page closed' ? 'left' : 'error';
}

/** what became of it, for the log: one line */
export function lastGameLine(r: LastGame): string {
  const at = (ms: number) => new Date(ms).toTimeString().slice(0, 8);
  const mins = ((r.saved - r.started) / 60000).toFixed(1);
  const what = !r.ended
    ? `ended without the page closing it, ${r.hidden ? 'the page hidden (in the background)' : 'the page on screen'} at its last copy: the browser stopped the page (an iPhone mostly for memory), or the browser or device restarted`
    : r.ended.why === 'page closed' ? `was running when the page was closed or reloaded`
      : `stopped (${r.ended.why}, code ${r.ended.code}), and the page was reloaded since`;
  const held = typeof r.held === 'string' && r.held ? `; at that copy it held ${r.held}` : '';
  return `[app] the last game (game ${r.rev}, started ${at(r.started)}, last copy ${at(r.saved)}, ${mins} min in) ${what}${held}; its log is at the start of this file`;
}
