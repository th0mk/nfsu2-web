// Drift mode in free roam (runtime/drift.c): X in the game turns the game's own drift-event handling, drift camera
// and drift scoring on and off.  The free-roam HUD has no place for the score (the drift event's panel is HUD_Drift's),
// so while drift mode is on the page shows that panel -- Best, the multiplier with the points its next level needs,
// Total -- under the game's Bank panel and as wide, and the drift event's DRIFT counter: a drift's points counting up
// while it counts, then in lime when it banks or in red when a wall loses it, fading out, in the middle as there (under
// free roam's arrow, which has the drift event's place), and a note at each X: on, off, or that it is for free roam.
// Not over a dialog of the page's.  It reads the game's status (runtime/web/status_web.c), as the pause hint does, every
// frame while drift mode is on: drift (1 chosen, 2 on now, 4 the last X was elsewhere), drift_key (X presses), drift_x,
// drift_gain, drift_total, drift_best, drift_st (0 a drift that counts) and drift_pend (its points).
// In a drift battle with another player (roam_game.c db_*) a scoreboard goes under the panel, as a drift event's
// standings: the time left, then both players with their points, the more first, the player's own row green; the game
// sends it (roam_game.c db_panel, NET_KIND_BATTLE) as {"t","me","a","vs","b"} while the points count, "{}" after.
import type { GameSession } from './game';
import { t } from './i18n';
import { pageDialog } from './padnav';

/** the game's Bank panel, in HUD units (640x480 in the middle of the picture; hud = wide moves its side out by up to
 *  109): its right edge 299 right of the middle, its bottom 167 above it; the panel goes 4 under it, as wide (176) */
const RIGHT_U = 299, TOP_U = -163, WIDE_U = 176;
/** the DRIFT counter: centred, from 92 above the middle (free roam's arrow ends 98 above it; the drift event's counter,
 *  from 154 above, is where the arrow is) */
const POP_TOP_U = -92;

export class DriftPanel {
  private root: HTMLElement;
  private best: HTMLElement;
  private mult: HTMLElement;
  private gain: HTMLElement;
  private total: HTMLElement;
  private pop: HTMLElement;
  private session: GameSession | null = null;
  private raf = 0;
  private readAt = 0;
  private keys = -1;
  private on = false;
  private shown = false;
  private filled = '';
  private banked = -1;
  private counting = false;
  private pend = 0;
  private board: HTMLElement;
  private bs: { t: number; me: string; a: number; vs: string; b: number } | null = null;
  private boardShown = false;

  constructor(stage: HTMLElement, private deps: { toast(msg: string, ms: number): void; wide(): boolean; log(s: string): void }) {
    const p = this.root = document.createElement('div');
    p.className = 'drift-panel'; p.hidden = true; p.dataset.nfsu2Ui = '';
    p.innerHTML = '<div><span></span><b class="drift-best"></b></div>'
      + '<div><i class="drift-x"></i><span class="drift-gain"><span></span><b></b></span></div>'
      + '<div class="drift-total"><span></span><b></b></div>';
    this.best = p.querySelector('.drift-best')!;
    this.mult = p.querySelector('.drift-x')!;
    this.gain = p.querySelector('.drift-gain')!;
    this.total = p.querySelector('.drift-total')!;
    const q = this.pop = document.createElement('div');
    q.className = 'drift-pop'; q.hidden = true; q.dataset.nfsu2Ui = '';
    q.innerHTML = '<span></span><b></b>';
    q.addEventListener('animationend', () => { q.hidden = true; });
    const b = this.board = document.createElement('div');
    b.className = 'drift-panel drift-board'; b.hidden = true; b.dataset.nfsu2Ui = '';
    stage.append(p, q, b);
  }

  /** a game started: the panel follows its drift mode until stop() */
  start(s: GameSession) {
    this.session = s; this.keys = -1; this.on = false; this.filled = ''; this.banked = -1; this.counting = false;
    const tick = () => { this.update(); this.raf = requestAnimationFrame(tick); };
    this.raf = requestAnimationFrame(tick);
  }

  /** the game ended */
  stop() {
    cancelAnimationFrame(this.raf);
    this.session = null;
    this.show(false);
    this.pop.hidden = true;
    this.bs = null; this.board.hidden = true; this.boardShown = false;
  }

  /** a drift battle's scoreboard from the game ("{}": none) */
  battle(json: string) {
    let v: Record<string, unknown> = {};
    try { v = JSON.parse(json) as Record<string, unknown>; } catch { /* none */ }
    const n = (x: unknown) => (typeof x === 'number' && Number.isFinite(x) ? Math.max(0, Math.floor(x)) : 0);
    const s = (x: unknown) => (typeof x === 'string' ? x : '');
    this.bs = typeof v.t === 'number' ? { t: n(v.t), me: s(v.me), a: n(v.a), vs: s(v.vs), b: n(v.b) } : null;
    if (this.bs) this.fillBoard(this.bs);
  }

  /** Time: m:ss, then both players, the more points first (the player first on a tie) */
  private fillBoard(bs: { t: number; me: string; a: number; vs: string; b: number }) {
    const rows: [string, number, boolean][] = [[bs.me, bs.a, true], [bs.vs, bs.b, false]];
    if (bs.b > bs.a) rows.reverse();
    const time = document.createElement('div'), label = document.createElement('span'), left = document.createElement('b');
    time.className = 'drift-time';
    label.textContent = t('Time:');
    left.textContent = `${Math.floor(bs.t / 60)}:${String(bs.t % 60).padStart(2, '0')}`;
    time.append(label, left);
    const lines = rows.map(([name, pts, me], k) => {
      const d = document.createElement('div'), i = document.createElement('i'), w = document.createElement('span'), p = document.createElement('b');
      d.className = me ? 'drift-who me' : 'drift-who';
      i.textContent = `${k + 1}:`; w.textContent = name; p.textContent = String(pts);
      d.append(i, w, p);
      return d;
    });
    this.board.replaceChildren(time, ...lines);
  }

  private update() {
    const s = this.session;
    if (!s) return;
    const now = performance.now();
    if (this.on || now - this.readAt > 100) {
      this.readAt = now;
      const st = s.status?.read();                // null while the game writes it: the last state stands
      if (st && st.has('drift')) {
        const d = st.get('drift')!, k = st.get('drift_key') ?? 0;
        if (this.keys >= 0 && k !== this.keys) {
          this.deps.toast(d & 4 ? t('Drift mode only works in free roam') : d & 1 ? t('Drift mode: on') : t('Drift mode: off'), 2500);
          this.deps.log(`[app] drift mode: X ${d & 4 ? 'outside free roam' : d & 1 ? 'on' : 'off'}`);
        }
        this.keys = k;
        this.on = (d & 2) !== 0;
        const total = st.get('drift_total') ?? 0, pend = st.get('drift_pend') ?? 0, was = this.counting;
        const grew = this.banked >= 0 && total > this.banked;
        this.counting = this.on && st.get('drift_st') === 0 && pend > 0;
        if (this.counting) this.counter('live', pend);
        else if (this.on && grew) this.counter('banked', total - this.banked);
        else if (this.on && was) this.counter('lost', this.pend);   // over without banking: a wall took it
        this.pend = pend; this.banked = total;
        this.fill(st.get('drift_best') ?? 0, st.get('drift_x') ?? 1, st.get('drift_gain') ?? 0, total);
      }
    }
    this.show(this.on && !pageDialog());
    const board = this.shown && this.bs !== null;
    if (board !== this.boardShown) { this.boardShown = board; this.board.hidden = !board; }
    if (this.shown) this.place(s);
    else this.pop.hidden = true;
  }

  /** DRIFT and the points: live while the drift counts, then banked or lost, fading out (the animation's end hides it) */
  private counter(kind: 'live' | 'banked' | 'lost', points: number) {
    const q = this.pop, again = kind !== 'live' || !q.classList.contains('live') || q.hidden;
    q.firstElementChild!.textContent = t('DRIFT');
    q.lastElementChild!.textContent = String(Math.floor(points));
    if (!again) return;
    q.className = `drift-pop ${kind}`;
    q.hidden = true; void q.offsetWidth; q.hidden = !this.shown;    // a fade from its start
  }

  private show(on: boolean) {
    if (on === this.shown) return;
    this.shown = on;
    this.root.hidden = !on;
  }

  /** the numbers, and the labels in the page's language */
  private fill(best: number, x: number, gain: number, total: number) {
    const key = `${best} ${x} ${gain} ${total} ${t('Total')}`;
    if (key === this.filled) return;
    this.filled = key;
    this.best.previousElementSibling!.textContent = t('Best:');
    this.best.textContent = String(best);
    this.mult.textContent = `X${x}`;
    this.gain.hidden = x >= 5 || gain <= 0;
    this.gain.firstElementChild!.textContent = t('Gain {n}X with:', { n: x + 1 });
    this.gain.lastElementChild!.textContent = String(gain);
    this.total.firstElementChild!.textContent = t('Total');
    this.total.lastElementChild!.textContent = String(total);
  }

  /** under the game's Bank panel, its right edge on the Bank panel's */
  private place(s: GameSession) {
    const v = s.viewRect();
    if (!v.w || !v.h) return;
    const u = Math.min(v.w / 640, v.h / 480), e169 = 320 * (16 / 9 * 0.75 - 1);
    const out = this.deps.wide() ? Math.min(Math.max(320 * (v.w / v.h * 0.75 - 1), 0), e169) * 109 / e169 : 0;
    const st = this.root.style, ps = this.pop.style;
    st.left = `${v.x + v.w / 2 + (RIGHT_U + out - WIDE_U) * u}px`;
    st.top = `${v.y + v.h / 2 + TOP_U * u}px`;
    st.setProperty('--u', `${u}px`);
    ps.left = `${v.x + v.w / 2}px`;                                      // its middle (the style's translate)
    ps.top = `${v.y + v.h / 2 + POP_TOP_U * u}px`;
    ps.setProperty('--u', `${u}px`);
    if (this.boardShown) {                                               // the scoreboard: 4 under the panel, as wide
      const bs = this.board.style;
      bs.left = st.left;
      bs.top = `${v.y + v.h / 2 + TOP_U * u + this.root.offsetHeight + 4 * u}px`;
      bs.setProperty('--u', `${u}px`);
    }
  }
}
