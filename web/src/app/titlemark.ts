// The site's address on the title screen ("Press Enter Key"), under EA's copyright in the band at its bottom, in the
// look and at the size of the mark on F12 pictures (gpu/mark.ts): the game's own font has no bold italic, so the page
// draws it over the picture, as an image (the stage's canvas is the game's).  The game says where the copyright is
// and how far it has faded in (runtime/intro.c, status "title_a": its alpha 0..1, 0 off the title; "title_x",
// "title_y": its middle in HUD units, 640x480 about the picture's middle; "title_s": its size, 0.89 there), so the
// address fades in and out with it.  On a loading screen (the game's status "loading": into a race or free roam, or
// free roam's world catching up, runtime/intro.c loading_tell; and the race lobby's trip cover, lobby.ts) it sits in
// the picture's bottom right corner as on F12 pictures (png-worker.ts: 2.6 % of its height, as far in).  Not over a
// dialog of the page's.
import type { GameSession } from './game';
import { drawMark, markBox } from '../gpu/mark';
import { pageDialog } from './padnav';

/** per unit of the copyright's size: the address's size, and its ink's middle under the copyright's middle, in HUD
 *  units (28 px and 13.6 units at 1080p, where the copyright is 0.89 at y 194) */
const PX_U = 13.93, DY_U = 15.22;

export class TitleMark {
  private img: HTMLImageElement;
  private c = document.createElement('canvas');  // where it is drawn, not on the page
  private session: GameSession | null = null;
  private raf = 0;
  private drawn = '';                            // the size and pixel ratio the image holds
  private mid = { x: 0, y: 0 };                  // its ink's middle in the image, in CSS px
  private ink = { r: 0, base: 0 };               // ...its ink's right edge and its baseline in it
  private cover: HTMLElement | null;             // the race lobby's trip cover (a loading screen of its own)

  constructor(stage: HTMLElement) {
    const img = this.img = document.createElement('img');
    img.className = 'title-mark'; img.alt = ''; img.hidden = true; img.draggable = false;
    stage.append(img);
    this.cover = stage.querySelector('#lobby-cover');
  }

  /** a game started: the address follows its title screen until stop() */
  start(s: GameSession) {
    this.session = s;
    const tick = () => { this.update(); this.raf = requestAnimationFrame(tick); };
    this.raf = requestAnimationFrame(tick);
  }

  /** the game ended */
  stop() {
    cancelAnimationFrame(this.raf);
    this.session = null;
    this.img.hidden = true;
  }

  private update() {
    const s = this.session;
    const st = s?.status?.read();
    if (!s || !st) return;                       // null while the game writes it: as it was
    const a = st.get('title_a') ?? 0, v = s.viewRect(), loading = (st.get('loading') ?? 0) > 0 || this.cover?.hidden === false;
    if (!(a > 0 || loading) || !v.w || !v.h || pageDialog()) { this.img.hidden = true; return; }
    const style = this.img.style;
    this.img.classList.toggle('corner', !(a > 0));
    if (!(a > 0)) {
      const m = v.h * 0.024;
      this.draw(Math.max(12, v.h * 0.026));
      style.left = `${v.x + v.w - m - this.ink.r}px`;
      style.top = `${v.y + v.h - m - this.ink.base}px`;
      style.opacity = '1';
      this.img.hidden = false;
      return;
    }
    const u = Math.min(v.w / 640, v.h / 480), size = st.get('title_s') ?? 0.89;
    this.draw(PX_U * size * u);
    const cx = v.x + v.w / 2 + (st.get('title_x') ?? 0) * u;
    const cy = v.y + v.h / 2 + ((st.get('title_y') ?? 194) + DY_U * size) * u;
    style.left = `${cx - this.mid.x}px`;
    style.top = `${cy - this.mid.y}px`;
    style.opacity = String(Math.min(1, a));
    this.img.hidden = false;
  }

  /** the address at size px (CSS px), the shadow's margin round its ink */
  private draw(px: number) {
    const dpr = window.devicePixelRatio || 1, key = `${px.toFixed(2)}|${dpr}`;
    if (key === this.drawn) return;
    this.drawn = key;
    const c = this.c, g = c.getContext('2d');
    if (!g) return;
    const b = markBox(g, px), pad = Math.ceil(px * 0.6) + 1;
    const w = b.l + b.r + 2 * pad, h = b.a + b.d + 2 * pad;
    c.width = Math.ceil(w * dpr); c.height = Math.ceil(h * dpr);
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    drawMark(g, px, pad + b.l, pad + b.a);
    this.img.src = c.toDataURL('image/png');
    this.img.style.width = `${c.width / dpr}px`; this.img.style.height = `${c.height / dpr}px`;
    this.mid = { x: pad + (b.l + b.r) / 2, y: pad + (b.a + b.d) / 2 };
    this.ink = { r: pad + b.l + b.r, base: pad + b.a };
  }
}
