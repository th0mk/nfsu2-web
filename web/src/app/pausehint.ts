// The game's pause menu (Esc) knows nothing of the page's own menu (F9), where Ghost mode, crews, full screen and the
// controls are.  So while the pause menu is up (the game's status: runtime/web/status_web.c "pause", not its options
// or a question over it), a line of the page's under its panel says what opens that menu here (F9, the ☰ button on a
// touch screen, Home on a controller) and what is in it; a click or a tap on the line opens it.  Not over a dialog
// of the page's.  The panel is the game's, in the middle of the picture: its bottom edge 81 HUD units under the
// middle, its width 596, at any aspect.
import type { GameSession } from './game';
import { padSeen, touchFirst } from './device';
import { N, t } from './i18n';
import { pageDialog } from './padnav';
import { keyName, siteKey } from './sitekeys';

/** where the line goes, in HUD units (640x480 in the picture): its top under the middle, its widest */
const TOP_U = 92, WIDE_U = 596;

export class PauseHint {
  private root: HTMLButtonElement;
  private session: GameSession | null = null;
  private raf = 0;
  private readAt = 0;
  private paused = false;
  private shown = false;

  constructor(stage: HTMLElement, private deps: { open(): void; roam(): boolean; fullscreen(): boolean; log(s: string): void }) {
    const b = this.root = document.createElement('button');
    b.type = 'button'; b.className = 'pause-hint'; b.tabIndex = -1; b.hidden = true; b.dataset.nfsu2Ui = '';
    b.onpointerdown = e => e.preventDefault();     // the canvas keeps the focus (and with it the keyboard)
    b.onclick = () => { if (this.session) this.deps.open(); };
    stage.append(b);
  }

  /** a game started: the line follows its pause menu until stop() */
  start(s: GameSession) {
    this.session = s;
    const tick = () => { this.update(); this.raf = requestAnimationFrame(tick); };
    this.raf = requestAnimationFrame(tick);
  }

  /** the game ended */
  stop() {
    cancelAnimationFrame(this.raf);
    this.session = null; this.paused = false;
    this.show(false);
  }

  private update() {
    const s = this.session;
    if (!s) return;
    const now = performance.now();
    if (now - this.readAt > 100) {
      this.readAt = now;
      const st = s.status?.read();                // null while the game writes it: the last state stands
      const p = st ? st.get('pause') === 1 : this.paused;
      if (p && !this.paused) this.deps.log(`[app] the game's pause menu is up: the line to the page's menu shows`);
      this.paused = p;
    }
    this.show(this.paused && !pageDialog());
    if (this.shown) this.place(s);
  }

  private show(on: boolean) {
    if (on === this.shown) return;
    this.shown = on;
    if (on) this.fill();
    this.root.hidden = !on;
  }

  /** what opens the menu here and what is in it, as it is when the line comes up */
  private fill() {
    // (the player's key for the menu: sitekeys.ts)
    const keys = [touchFirst() ? '☰' : keyName(siteKey('menu') ?? 'F9'), ...(padSeen() ? ['Home'] : [])].map(k => {
      const kbd = document.createElement('kbd');
      kbd.textContent = k;
      return kbd;
    });
    const fs = this.deps.fullscreen(), roam = this.deps.roam();
    const list = document.createElement('span');
    list.className = 'pause-hint-list';
    list.textContent = t(roam ? (fs ? N('Ghost mode, crews, friends, full screen, controls') : N('Ghost mode, crews, friends, controls'))
      : fs ? N('Full screen, frame-rate counter, controls') : N('Frame-rate counter, controls'));
    this.root.replaceChildren(...keys, document.createTextNode(t('More options')), list);
  }

  /** under the game's panel, in the middle of the picture */
  private place(s: GameSession) {
    const v = s.viewRect();
    if (!v.w || !v.h) return;
    const u = Math.min(v.w / 640, v.h / 480), st = this.root.style;
    st.left = `${v.x + v.w / 2}px`;
    st.top = `${v.y + v.h / 2 + TOP_U * u}px`;
    st.maxWidth = `${WIDE_U * u}px`;
    st.setProperty('--u', `${u}px`);   // (the cards' size, .ov-card)
  }
}
