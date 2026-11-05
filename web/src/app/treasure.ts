// The daily treasure hunt in the career's free roam (runtime/treasure.c picks the day's diamonds and finds them as the
// car comes by; this draws the way to the next one and keeps what was found with the achievements).
//
// The game hears the UTC day's number and its diamonds found so far once its input is up (IEV_TREASURE), and again
// at midnight.  It says, in status slots (runtime/treasure.c has their packing): th_state the day, the ones found,
// whether the hunt is on; th_aim the nearest one's distance, bearing, which, the day's prize and its golden one;
// th_screen where it is on the screen.  On the stage: the pill (how many are found, an arrow and the distance), a
// diamond over the street from MARK_M growing as it comes; a chime and a flash at each one found, and cards
// (achievements.ts).  The
// hunt's time is free roam's while it is on, from the first diamond to the last.
import type { GameSession } from './game';
import { bitCount, huntClock, utcDay, type Achievements } from './achievements';
import { huntBoard, reportHunt, showBoard } from './huntboard';
import { count, t } from './i18n';

export const DIAMONDS = 15;
const ALL = (1 << DIAMONDS) - 1;
const MARK_M = 60;    // the marker over the street within this (m), tiny at first; the minimap has it within 500 (runtime/treasure.c)
const STEP_MAX_MS = 250;   // (a frame's time counts up to this: a page in the background counts nothing)
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
    aimed: bits(aim, 41, 1) === 1, gold: bits(aim, 42, 4) - 1,
  };
}

export class TreasureHunt {
  private session: GameSession | null = null;
  private raf = 0;
  private mark: HTMLElement;
  private markDist: HTMLElement;
  private flash: HTMLElement;     // the stage's flash at a diamond found
  private audio: AudioContext | null = null;
  private ms = 0;                 // the hunt's time so far (free roam's, while it is on and a diamond is found)
  private last = 0;               // the last frame's time (performance.now)
  private day = -1;               // the day the game was told
  private found = 0;              // its diamonds found, as last seen
  private done = false;           // its last one's card shown
  private els: { count: HTMLElement; arrow: HTMLElement; dist: HTMLElement; lock: HTMLElement; locked: HTMLElement; later: HTMLElement };

  /** opts.token: the keyphrase's (the board lists a claimed name); opts.board: where the day's board shows */
  constructor(private hud: HTMLElement, private ach: Achievements, private log: (line: string) => void,
    private opts: { token?: () => Promise<string | null>; board?: HTMLElement | null } = {}) {
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
    this.flash = document.createElement('div');
    this.flash.className = 'th-flash'; this.flash.setAttribute('aria-hidden', 'true');
    hud.parentElement?.append(this.mark, this.flash);
    const q = (c: string) => hud.querySelector<HTMLElement>(c)!;
    this.els = { count: q('.th-count'), arrow: q('.th-arrow'), dist: q('.th-dist'), lock: q('.th-lock'), locked: q('.th-locked'), later: q('.th-later') };
  }

  start(s: GameSession) {
    this.session = s; this.day = -1; this.done = false;
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
      const found = this.ach.huntFound(today);
      if (!s.input?.treasure?.(today, found)) return;             // (its input not up yet: the next poll)
      this.day = today; this.found = found; this.done = false; this.ms = this.ach.huntMs(today);
      this.log(`[app] treasure hunt: day ${today}, ${bitCount(found)} of ${DIAMONDS} found`);
    }
    const st = s.status?.read();
    if (!st || !st.has('th_state')) return;
    const h = unpackHunt(st.get('th_state') ?? 0, st.get('th_aim') ?? 0);
    const now = performance.now(), dt = this.last ? Math.min(STEP_MAX_MS, now - this.last) : 0;
    this.last = now;
    if (h.on && this.found && this.found !== ALL && !document.hidden) this.ms += dt;
    if (h.on && this.found === ALL) this.done = true;          // (a day found whole before this game: no card again)
    if (h.day === (today & 0xffff) && h.found !== this.found && (h.found & this.found) === this.found) {
      const fresh = h.found & ~this.found, golden = h.gold >= 0 && (fresh >> h.gold & 1) === 1;
      this.found = h.found;
      this.found_(golden);
      // (the last one pays twice a win of the stage, the golden one a win's: half the day's prize)
      if (h.found === ALL) {
        if (!this.done) {
          this.done = true; this.ach.huntDone(today, h.found, h.prize, Math.round(this.ms), golden ? h.prize / 2 : 0);
          void this.report(today, Math.round(this.ms));
        }
      }
      else this.ach.diamond(today, h.found, DIAMONDS, golden ? h.prize / 2 : 0, Math.round(this.ms), golden);
    }
    const show = h.on && this.found !== ALL && (h.aimed || h.locked > 0);
    this.hud.hidden = !show;
    const gold = h.aimed && h.which === h.gold;
    this.mark.classList.toggle('gold', gold);
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

  /** the day's time to the server's board (hunts.rs), then the board again */
  private async report(day: number, ms: number) {
    const ok = await reportHunt(day, ms, this.opts.token);
    this.log(`[app] treasure hunt: all ${DIAMONDS} in ${huntClock(ms)}${ok ? ', on the day\'s board' : ', not on the board (no answer, or refused)'}`);
    await this.board();
  }
  /** the day's board, in the achievements tab */
  async board() {
    const el = this.opts.board;
    if (!el) return;
    showBoard(el, await huntBoard(utcDay()), huntClock);
  }

  /** a diamond found: a chime (a higher, longer one for the golden one) and the stage's flash */
  private found_(golden: boolean) {
    this.flash.classList.remove('on', 'gold'); void this.flash.offsetWidth;   // (the animation again)
    this.flash.classList.add('on'); this.flash.classList.toggle('gold', golden);
    try {
      this.audio ??= new AudioContext();
      const a = this.audio, t0 = a.currentTime + 0.01, out = a.createGain();
      out.gain.value = 0.12; out.connect(a.destination);
      const notes = golden ? [1319, 1568, 1976, 2637] : [1568, 2093];
      notes.forEach((f, i) => {
        const o = a.createOscillator(), g = a.createGain(), at = t0 + i * 0.07;
        o.type = 'sine'; o.frequency.value = f;
        g.gain.setValueAtTime(0, at); g.gain.linearRampToValueAtTime(1, at + 0.01); g.gain.exponentialRampToValueAtTime(0.001, at + 0.45);
        o.connect(g); g.connect(out); o.start(at); o.stop(at + 0.5);
      });
    } catch { /* (no sound: the flash is enough) */ }
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
