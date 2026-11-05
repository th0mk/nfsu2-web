// After two hours of play, a text message of ours on the game's phone (runtime/sms.c): "Enjoying the game?", with the
// Discord, crews and Ko-fi; once.  Play counts while a game runs, the tab is in sight and the player gave some input
// in the last few minutes (input/index.ts lastInput), over all the games in this browser (localStorage).  From two
// hours on, each game asks the game for the message (IEV_SMS) until its status "sms" says the inbox has it: the game
// waits for career free roam, the only place with a phone, and a profile that has it already counts.  The count goes
// on after the message: Play's cloud saves question says how long was played here (playedMs, shell.ts askCloud;
// before 2026-10-31 it stopped at the message, near two hours).  The games started here are counted too (since
// 2026-11-03): with the play, Play's question asks once more (vault-ui.ts offerable).  Without storage (a private
// window) each load counts from 0.
// The start page shows the count (2026-11-05, players asked): the whole in the Saves tab, the latest game's under
// Play (lastGameMs, kept as the game goes, so a page Safari stops keeps it too).  The whole is short where the count
// began before the page showed it: it began only on 2026-10-26 and stopped near two hours until 2026-10-31, so there
// the page says "at least" (playedAtLeast; the browser's first page with this code decides, once).
import type { GameSession } from './game';
import { t } from './i18n';

export const SMS_AFTER_MS = 2 * 3600_000;
const PLAYED = 'nfsu2.play.ms', SENT = 'nfsu2.play.sms', GAMES = 'nfsu2.play.games', LAST = 'nfsu2.play.last', FROM = 'nfsu2.play.from';
const MSG = 1;                          /* sms.c: "Enjoying the game?" */
const IDLE_MS = 5 * 60_000, TICK_MS = 5000;

const read = (key: string): string | null => { try { return localStorage.getItem(key); } catch { return null; } };
const write = (key: string, v: string) => { try { localStorage.setItem(key, v); } catch { /* not kept */ } };

/** the play counted in this browser so far, in ms (0 without storage) */
export const playedMs = () => Math.max(0, Number(read(PLAYED)) || 0);
/** the games started in this browser so far (counted since 2026-11-03; 0 without storage) */
export const gamesStarted = () => Math.max(0, Math.floor(Number(read(GAMES)) || 0));
/** the play counted in the latest game in this browser, in ms (0 before a game with this code, or without storage) */
export const lastGameMs = () => Math.max(0, Number(read(LAST)) || 0);
/** whether the count here began before the page showed it, so it may miss play (see the top): decided at the first
 *  call in this browser, by whether there was a count then, and kept */
export function playedAtLeast(): boolean {
  const from = read(FROM);
  if (from === 'old' || from === 'new') return from === 'old';
  const old = playedMs() > 0;
  write(FROM, old ? 'old' : 'new');
  return old;
}
/** a count as the page shows it: "45 min", "3 h 20 min"; '' under a minute */
export function playedText(ms: number): string {
  const m = Math.floor(ms / 60_000);
  if (!m) return '';
  return m < 60 ? t('{n} min', { n: m }) : t('{h} h {m} min', { h: Math.floor(m / 60), m: m % 60 });
}

export class PlayTime {
  private timer: ReturnType<typeof setInterval> | null = null;
  private played = playedMs();
  /** this game's */
  private game = 0;
  private last = performance.now();
  private asked = false;
  /** the message is in the game's inbox: only the count goes on */
  private sent = read(SENT) === String(MSG);

  /** a game started (counted); afterMs: the play before the message (SMS_AFTER_MS; a test page's address may give
   *  less) */
  constructor(private s: GameSession, private afterMs: number, private log: (line: string) => void) {
    playedAtLeast();                      // (decided before this game counts)
    write(GAMES, String(gamesStarted() + 1));
    write(LAST, '0');
    this.timer = setInterval(() => this.tick(), TICK_MS);
  }

  private tick() {
    const now = performance.now(), dt = Math.min(now - this.last, 2 * TICK_MS), input = this.s.input?.lastInput?.() ?? null;
    this.last = now;
    if (!document.hidden && input !== null && now - input < IDLE_MS) {
      this.played += dt; this.game += dt;
      write(PLAYED, String(Math.round(this.played)));
      write(LAST, String(Math.round(this.game)));
    }
    if (this.sent) return;
    if (!this.asked && this.played >= this.afterMs && this.s.input?.sms?.(MSG)) {
      this.asked = true;
      this.log(`[app] played ${Math.round(this.played / 60_000)} min: the game's phone gets our message once the player drives in career free roam`);
    }
    if (this.asked && this.s.status?.read()?.get('sms') === MSG) {
      write(SENT, String(MSG));
      this.sent = true;
      this.log('[app] our message is in the game\'s inbox: not asked for again');
    }
  }

  stop() {
    if (this.timer !== null) clearInterval(this.timer);
    this.timer = null;
  }
}
