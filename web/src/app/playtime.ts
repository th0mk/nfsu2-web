// After two hours of play, a text message of ours on the game's phone (runtime/sms.c): "Enjoying the game?", with the
// Discord, crews and Ko-fi; once.  After a hundred hours a second one, "100 hours!", a thank-you (2026-11-05), in a
// later game than the first.  Play counts while a game runs, the tab is in sight and the player gave some input
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
/** the second message's play, as a multiple of the first's (a test page's playtime= scales both) */
const HUNDRED_TIMES = 50;
const PLAYED = 'nfsu2.play.ms', SENT = 'nfsu2.play.sms', GAMES = 'nfsu2.play.games', LAST = 'nfsu2.play.last', FROM = 'nfsu2.play.from';
const HELLO = 1, HUNDRED = 2;           /* sms.c: "Enjoying the game?", "100 hours!" */
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
  /** the message asked for in this game, 0: none */
  private asked = 0;
  /** the last message in the game's inbox (SENT: 1 before 2026-11-05 too): after the last, only the count goes on */
  private sent = Math.max(0, Math.floor(Number(read(SENT)) || 0));
  /** the second message only in a game after the first's (not both at once) */
  private readonly sentBefore = this.sent;

  /** a game started (counted); afterMs: the play before the first message (SMS_AFTER_MS; a test page's address may
   *  give less), the second's HUNDRED_TIMES that */
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
    if (this.sent >= HUNDRED) return;
    const due = this.sent < HELLO ? (this.played >= this.afterMs ? HELLO : 0)
      : this.sentBefore >= HELLO && this.played >= this.afterMs * HUNDRED_TIMES ? HUNDRED : 0;
    if (due && this.asked !== due && this.s.input?.sms?.(due)) {
      this.asked = due;
      this.log(`[app] played ${Math.round(this.played / 60_000)} min: the game's phone gets our message ${due} once the player drives in career free roam`);
    }
    if (this.asked && this.s.status?.read()?.get('sms') === this.asked) {
      write(SENT, String(this.asked));
      this.sent = this.asked;
      this.log(`[app] our message ${this.asked} is in the game's inbox: not asked for again`);
    }
  }

  stop() {
    if (this.timer !== null) clearInterval(this.timer);
    this.timer = null;
  }
}
