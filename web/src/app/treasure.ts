// The daily treasure hunt in the career's free roam (runtime/treasure.c picks the day's diamonds and finds them as the
// car comes by; this draws the way to the next one and keeps what was found with the achievements).
//
// The game hears the UTC day's number and its diamonds found so far once its input is up (IEV_TREASURE), and again
// at midnight.  It says, in two status slots, th_state: the day | found << 16 (a bit each) | on << 31 (the career's
// free roam) | the prize it paid, $ / 100, << 32; and th_aim: the nearest one's distance (m) | its bearing from the
// car's nose << 16 (degrees clockwise) | which << 25.  On the stage: a diamond, how many are found, an arrow and the
// distance, at the top while the hunt can be played and not all are found; each one found and the last are cards
// (achievements.ts).
import type { GameSession } from './game';
import { bitCount, utcDay, type Achievements } from './achievements';
import { count, t } from './i18n';

export const DIAMONDS = 15;
const ALL = (1 << DIAMONDS) - 1;
const MARK_M = 60;    // the marker over the street within this (m), tiny at first; the minimap has it within 500 (runtime/treasure.c)
const bits = (v: number, at: number, n: number) => Math.floor(v / 2 ** at) % 2 ** n;

/** th_screen as runtime/treasure.c packs it: where the nearest open diamond is on the screen, in half the picture's
 *  height from its middle (y up), or null behind the camera */
export function unpackScreen(v: number): { x: number; y: number } | null {
  if (!bits(v, 30, 1)) return null;
  return { x: bits(v, 0, 15) / 2048 - 8, y: bits(v, 15, 15) / 2048 - 8 };
}

/** th_state and th_aim as runtime/treasure.c packs them */
export function unpackHunt(state: number, aim: number) {
  return {
    day: bits(state, 0, 16), found: bits(state, 16, DIAMONDS), on: bits(state, 31, 1) === 1, paid: bits(state, 32, 16) * 10,
    locked: bits(state, 48, 4),
    meters: bits(aim, 0, 16), bearing: bits(aim, 16, 9), which: bits(aim, 25, 4), prize: bits(aim, 29, 12) * 10,
    aimed: bits(aim, 41, 1) === 1,
  };
}

export class TreasureHunt {
  private session: GameSession | null = null;
  private raf = 0;
  private mark: HTMLElement;
  private markDist: HTMLElement;
  private day = -1;               // the day the game was told
  private found = 0;              // its diamonds found, as last seen
  private done = false;           // its last one's card shown
  private reset = false;          // ?hunt=reset: today's found ones forgotten at the next start (for trying the hunt again)
  private els: { count: HTMLElement; arrow: HTMLElement; dist: HTMLElement; lock: HTMLElement; locked: HTMLElement; later: HTMLElement };

  constructor(private hud: HTMLElement, private ach: Achievements, private log: (line: string) => void) {
    hud.innerHTML = '<svg class="th-gem" viewBox="0 0 24 24" aria-hidden="true"><path d="M7 4h10l4 5-9 11L3 9z"/>' +
      '<path d="M3 9h18M9.5 4 8 9l4 11 4-11-1.5-5"/></svg><span class="th-count"></span>' +
      '<svg class="th-arrow" viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3l7 16-7-4-7 4z"/></svg><span class="th-dist"></span>' +
      '<span class="th-later"></span><svg class="th-lock" viewBox="0 0 24 24" aria-hidden="true"><rect x="5" y="11" width="14" height="10" rx="2"/>' +
      '<path d="M8 11V8a4 4 0 0 1 8 0v3"/></svg><span class="th-locked"></span>';
    // the marker over the street where the nearest open diamond is (#stage, as the pill)
    this.mark = document.createElement('div');
    this.mark.className = 'th-mark'; this.mark.hidden = true; this.mark.setAttribute('aria-hidden', 'true');
    this.mark.innerHTML = '<svg viewBox="0 0 24 24"><path d="M7 4h10l4 5-9 11L3 9z"/><path d="M3 9h18M9.5 4 8 9l4 11 4-11-1.5-5"/></svg><span></span>';
    this.markDist = this.mark.querySelector('span')!;
    hud.parentElement?.append(this.mark);
    const q = (c: string) => hud.querySelector<HTMLElement>(c)!;
    this.els = { count: q('.th-count'), arrow: q('.th-arrow'), dist: q('.th-dist'), lock: q('.th-lock'), locked: q('.th-locked'), later: q('.th-later') };
  }

  start(s: GameSession) {
    this.session = s; this.day = -1; this.done = false;
    const url = new URL(location.href);
    if (url.searchParams.get('hunt') === 'reset') {
      this.reset = true;
      url.searchParams.delete('hunt'); history.replaceState(history.state, '', url);   // (once: not again at a reload)
    }
    cancelAnimationFrame(this.raf);
    const tick = () => { this.poll(); this.raf = requestAnimationFrame(tick); };   // (each frame: the marker keeps to the street)
    this.raf = requestAnimationFrame(tick);
  }
  stop() {
    cancelAnimationFrame(this.raf); this.raf = 0;
    this.session = null; this.hud.hidden = true; this.mark.hidden = true;
  }

  private poll() {
    const s = this.session;
    if (!s) return;
    const today = utcDay();
    if (this.day !== today) {                                   // the game is told the day (again at midnight)
      const forget = this.reset;
      const found = forget ? 0 : this.ach.huntFound(today);
      if (!s.input?.treasure?.(today, found)) return;             // (its input not up yet: the next poll)
      this.day = today; this.found = found; this.done = false;
      if (forget) { this.reset = false; this.ach.huntReset(today); this.log('[app] treasure hunt: today\'s diamonds found forgotten (?hunt=reset)'); }
      this.log(`[app] treasure hunt: day ${today}, ${bitCount(found)} of ${DIAMONDS} found`);
    }
    const st = s.status?.read();
    if (!st || !st.has('th_state')) return;
    const h = unpackHunt(st.get('th_state') ?? 0, st.get('th_aim') ?? 0);
    if (h.on && this.found === ALL) this.done = true;          // (a day found whole before this game: no card again)
    if (h.day === (today & 0xffff) && h.found !== this.found && (h.found & this.found) === this.found) {
      this.found = h.found;
      // (only the last one pays: twice a win of the stage)
      if (h.found === ALL) { if (!this.done) { this.done = true; this.ach.huntDone(today, h.found, h.prize); } }
      else this.ach.diamond(today, h.found, DIAMONDS, 0);
    }
    const show = h.on && this.found !== ALL && (h.aimed || h.locked > 0);
    this.hud.hidden = !show;
    this.place(show && h.aimed && h.meters <= MARK_M ? unpackScreen(st.get('th_screen') ?? 0) : null, h.meters);
    if (!show) return;
    this.els.count.textContent = `${count(bitCount(this.found))} / ${count(DIAMONDS)}`;
    // (an svg has no hidden: its display)
    this.els.arrow.style.display = h.aimed ? '' : 'none';
    this.els.dist.hidden = !h.aimed;
    this.els.later.hidden = h.aimed;
    this.els.lock.style.display = h.locked ? '' : 'none';
    this.els.locked.hidden = !h.locked;
    this.els.locked.textContent = count(h.locked);
    if (h.aimed) {
      this.els.arrow.style.transform = `rotate(${h.bearing}deg)`;
      this.els.dist.textContent = h.meters >= 1000 ? t('{n} km', { n: count(Math.round(h.meters / 100) / 10) }) : t('{n} m', { n: count(h.meters) });
    } else this.els.later.textContent = t('The rest open as your career goes on.');
  }

  /** the marker at p (half heights from the middle, y up), bigger as it comes near; hidden off the screen */
  private place(p: { x: number; y: number } | null, meters: number) {
    const W = this.mark.parentElement?.clientWidth ?? 0, H = this.mark.parentElement?.clientHeight ?? 0;
    const x = p ? W / 2 + p.x * H / 2 : 0, y = p ? H / 2 - p.y * H / 2 : 0;
    if (!p || !H || x < -40 || x > W + 40 || y < -40 || y > H + 40) { this.mark.hidden = true; return; }
    const near = Math.min(1, Math.max(0, 1 - meters / MARK_M)), size = H * (0.015 + 0.075 * near * near);   // (grows as it comes)
    this.mark.hidden = false;
    this.mark.style.transform = `translate(${x.toFixed(1)}px, ${y.toFixed(1)}px) translate(-50%, -100%)`;
    this.mark.style.setProperty('--size', `${size.toFixed(1)}px`);
    this.markDist.textContent = meters >= 1000 ? t('{n} km', { n: count(Math.round(meters / 100) / 10) }) : t('{n} m', { n: count(meters) });
  }
}
