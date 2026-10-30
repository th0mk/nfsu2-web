// Photo mode (runtime/photo.c): F12 in the world, or Photo Mode in the game's pause menu, stops the world, hides the
// HUD and lets the camera loose from the car; F12 again (the panel's button, A on a controller) takes the picture with
// the page's screenshot (shell.ts: a PNG beside the saves, shown in Saves on the start page).  Where photo mode cannot
// open (the menus, a race over the network) F12 takes the screenshot at once, as it always has.
// While it is on, the page keeps the keyboard, the mouse, the touch screen and the controller from the game and sends
// what they do as photo events (input/index.ts photo(): the keys and buttons held as bits, the sticks, drags, the
// wheel and pinches, the sliders' values), hides its other things on the game, and shows a panel: orbit or free, zoom,
// tilt, friends' names (in free roam with them), reset, the picture, leave, and what the keys do.  It reads the game's
// status (runtime/web/status_web.c) every frame: photo (1 it can open, 2 it is on, 4 the free camera, 8 the names on,
// 16 friends' names can show), photo_fov, photo_roll.
import type { GameSession } from './game';
import type { PadSnapshot } from '../input/gamepad';
import {
  PAD_A, PAD_AXIS_LT, PAD_AXIS_LX, PAD_AXIS_LY, PAD_AXIS_RT, PAD_AXIS_RX, PAD_AXIS_RY, PAD_B, PAD_BACK, PAD_DDOWN,
  PAD_DLEFT, PAD_DRIGHT, PAD_DUP, PAD_LB, PAD_LS, PAD_RB, PAD_RS, PAD_START, PAD_X, PAD_Y,
} from '../input/proto';
import { N, t } from './i18n';
import { pageDialog } from './padnav';
import { padSeen, touchFirst } from './device';

// runtime/photo.c: the events (PH_*) and the bits of PH_HOLD (HB_*)
const PH_HELLO = 0, PH_OPEN = 1, PH_HOLD = 2, PH_MOVE = 3, PH_TURN = 4, PH_DRAG = 5, PH_PAN = 6, PH_DOLLY = 7,
  PH_FOV = 8, PH_ROLL = 9, PH_MODE = 10, PH_RESET = 11, PH_NAMES = 12;
const HB_FWD = 1, HB_BACK = 2, HB_LEFT = 4, HB_RIGHT = 8, HB_UP = 16, HB_DOWN = 32, HB_TL = 64, HB_TR = 128,
  HB_TU = 256, HB_TD = 512, HB_ZIN = 4096, HB_ZOUT = 8192, HB_RL = 16384, HB_RR = 32768, HB_FAST = 65536;
const FOV_MIN = 15, FOV_MAX = 110, ROLL_MAX = 45;

/** the keys that steer the camera while they are held */
const HELD: Record<string, number> = {
  KeyW: HB_FWD, KeyS: HB_BACK, KeyA: HB_LEFT, KeyD: HB_RIGHT, KeyE: HB_UP, KeyQ: HB_DOWN,
  ArrowLeft: HB_TL, ArrowRight: HB_TR, ArrowUp: HB_TU, ArrowDown: HB_TD,
  KeyZ: HB_ZIN, KeyX: HB_ZOUT, KeyC: HB_RL, KeyV: HB_RR, ShiftLeft: HB_FAST, ShiftRight: HB_FAST,
};
/** the page's own keys, which stay the page's (shell.ts) */
const PAGE_KEYS = new Set(['F8', 'F9', 'F11', 'F12']);

/** two values of -1..1 as a photo event's int16 pair */
const pack = (x: number, y: number) =>
  (Math.round(Math.max(-1, Math.min(1, x)) * 32767) & 0xffff) | (Math.round(Math.max(-1, Math.min(1, y)) * 32767) << 16);
/** a stick's value past its dead zone, from 0 there */
const dz = (v: number) => (Math.abs(v) < 0.15 ? 0 : (v - Math.sign(v) * 0.15) / 0.85);

type Hint = [string, string];

export class PhotoMode {
  private root: HTMLElement;
  private flash: HTMLElement;
  private zoom: HTMLInputElement;
  private tilt: HTMLInputElement;
  private names: HTMLInputElement;
  private hints: HTMLElement;
  private session: GameSession | null = null;
  private raf = 0;
  private hello = false;
  private can = false;
  private free = false;
  private namesHere = false;             // friends' names can show (free roam with them)
  /** whether photo mode is on (the game's status) */
  on = false;
  private bare = false;                  // the panel hidden (H, the controller's View)
  private keys = 0;
  private padBits = 0;
  private sent = { hold: -1, move: -1, lift: -1, turn: -1 };
  private sliding: HTMLInputElement | null = null;
  private drag: { id: number; x: number; y: number; pan: boolean } | null = null;
  private fingers = new Map<number, [number, number]>();
  private pinch: { d: number; x: number; y: number } | null = null;
  private shooting = false;
  private flashTimer = 0;

  constructor(private stage: HTMLElement, private deps: {
    shoot(): Promise<void>; screenshot(): void; log(s: string): void;
  }) {
    const p = this.root = document.createElement('div');
    p.className = 'photo-panel'; p.hidden = true; p.dataset.nfsu2Ui = '';
    p.innerHTML =
      '<div class="ph-head"><b class="ph-title"></b><span class="ph-mode"><button type="button" data-mode="0"></button>' +
      '<button type="button" data-mode="1"></button></span></div>' +
      '<label class="ph-slider"><span class="ph-zoom-l"></span><input type="range" class="ph-zoom" min="0" max="1000" step="1"></label>' +
      '<label class="ph-slider"><span class="ph-tilt-l"></span><input type="range" class="ph-tilt" min="-450" max="450" step="5"></label>' +
      '<label class="ph-check" hidden><input type="checkbox" class="ph-names"><span class="ph-names-l"></span></label>' +
      '<div class="ph-buttons"><button type="button" data-act="reset"></button>' +
      '<button type="button" class="ph-shoot" data-act="shoot"></button><button type="button" data-act="leave"></button></div>' +
      '<div class="ph-hints"></div>';
    this.zoom = p.querySelector('.ph-zoom')!;
    this.tilt = p.querySelector('.ph-tilt')!;
    this.names = p.querySelector('.ph-names')!;
    this.hints = p.querySelector('.ph-hints')!;
    p.addEventListener('click', e => {
      const b = (e.target as HTMLElement).closest<HTMLButtonElement>('button');
      if (!b) return;
      if (b.dataset.mode) this.send(PH_MODE, +b.dataset.mode);
      else if (b.dataset.act === 'reset') this.send(PH_RESET);
      else if (b.dataset.act === 'shoot') void this.shoot();
      else if (b.dataset.act === 'leave') this.leave();
    });
    // the buttons leave the keyboard with the game's canvas; the sliders give it back when let go
    p.addEventListener('pointerdown', e => { if ((e.target as HTMLElement).closest('button')) e.preventDefault(); });
    for (const s of [this.zoom, this.tilt]) {
      s.addEventListener('pointerdown', () => { this.sliding = s; });
      s.addEventListener('input', () => {
        if (s === this.zoom) this.send(PH_FOV, Math.round((FOV_MAX - (+s.value / 1000) * (FOV_MAX - FOV_MIN)) * 100));
        else this.send(PH_ROLL, Math.round(+s.value * 10));
      });
      s.addEventListener('change', () => { this.sliding = null; this.canvas()?.focus({ preventScroll: true }); });
    }
    this.tilt.addEventListener('dblclick', () => { this.tilt.value = '0'; this.send(PH_ROLL, 0); });
    this.names.addEventListener('change', () => { this.send(PH_NAMES, this.names.checked ? 1 : 0); this.canvas()?.focus({ preventScroll: true }); });
    const f = this.flash = document.createElement('div');
    f.className = 'photo-flash'; f.hidden = true;          // (hidden by a timer: with reduced motion no animation ends)
    stage.append(p, f);
    addEventListener('keydown', e => this.onKey(e as KeyboardEvent, true), { capture: true });
    addEventListener('keyup', e => this.onKey(e as KeyboardEvent, false), { capture: true });
    for (const type of ['pointerdown', 'pointermove', 'pointerup', 'pointercancel'])
      addEventListener(type, e => this.onPointer(e as PointerEvent), { capture: true });
    addEventListener('wheel', e => this.onWheel(e as WheelEvent), { capture: true, passive: false });
    addEventListener('contextmenu', e => { if (this.mine(e)) { e.preventDefault(); e.stopImmediatePropagation(); } }, { capture: true });
    // no key-up comes for a key held while the page loses the keyboard (alt-tab, another tab)
    addEventListener('blur', () => this.letGo());
    document.addEventListener('visibilitychange', () => { if (document.hidden) this.letGo(); });
  }

  /** a game started: photo mode follows its status until stop() */
  start(s: GameSession) {
    this.session = s; this.hello = false;
    const tick = () => { this.update(); this.raf = requestAnimationFrame(tick); };
    this.raf = requestAnimationFrame(tick);
  }

  /** the game ended */
  stop() {
    cancelAnimationFrame(this.raf);
    this.show(false);
    this.session = null; this.can = false;
  }

  /** F12: the picture while photo mode is on; else photo mode where it can open, the screenshot where it cannot */
  f12() {
    if (this.on) void this.shoot();
    else if (this.can && this.send(PH_OPEN, 1)) this.deps.log('[app] photo mode: F12');
    else this.deps.screenshot();
  }

  /** the controller while no dialog of the page is open (gamepad.ts PadPage.capture): photo mode's while it is on */
  pad(s: PadSnapshot, pressed: number): boolean {
    if (!this.on) return false;
    const b = s.buttons, a = s.axes, on = (n: number) => (b & 1 << n) !== 0;
    if (pressed & 1 << PAD_A) void this.shoot();
    if (pressed & (1 << PAD_B | 1 << PAD_START)) this.leave();
    if (pressed & 1 << PAD_Y) this.send(PH_MODE, 2);
    if (pressed & 1 << PAD_X) this.send(PH_RESET);
    if (pressed & 1 << PAD_RS && this.namesHere) this.send(PH_NAMES, 2);
    if (pressed & 1 << PAD_BACK) this.setBare(!this.bare);
    this.padBits = (on(PAD_DUP) ? HB_ZIN : 0) | (on(PAD_DDOWN) ? HB_ZOUT : 0) | (on(PAD_DLEFT) ? HB_RL : 0) |
      (on(PAD_DRIGHT) ? HB_RR : 0) | (on(PAD_LS) ? HB_FAST : 0);
    this.steer(pack(dz(a[PAD_AXIS_LX]), -dz(a[PAD_AXIS_LY])),
      pack((on(PAD_RB) ? 1 : 0) - (on(PAD_LB) ? 1 : 0), dz(a[PAD_AXIS_RT]) - dz(a[PAD_AXIS_LT])),
      pack(dz(a[PAD_AXIS_RX]), dz(a[PAD_AXIS_RY])));
    return true;
  }

  private input() { return this.session?.input ?? null; }
  private canvas() { return this.stage.querySelector('canvas'); }
  private send(what: number, a = 0, b = 0): boolean { return !!this.input()?.photo?.(what, a, b); }
  private leave() { if (this.send(PH_OPEN, 0)) this.deps.log('[app] photo mode: leave'); }

  /** the held keys and buttons, the left stick and shoulders, the right stick: sent when they change */
  private steer(move = this.sent.move < 0 ? 0 : this.sent.move, lift = this.sent.lift < 0 ? 0 : this.sent.lift,
                turn = this.sent.turn < 0 ? 0 : this.sent.turn) {
    const hold = this.keys | this.padBits;
    if (hold !== this.sent.hold && this.send(PH_HOLD, hold)) this.sent.hold = hold;
    if ((move !== this.sent.move || lift !== this.sent.lift) && this.send(PH_MOVE, move, lift)) { this.sent.move = move; this.sent.lift = lift; }
    if (turn !== this.sent.turn && this.send(PH_TURN, turn)) this.sent.turn = turn;
  }

  /** nothing held any more: the keys, the controller, a drag (the page lost the input, or a dialog of its took it) */
  private letGo() {
    if (!this.on) return;
    this.keys = 0; this.padBits = 0; this.drag = null; this.fingers.clear(); this.pinch = null;
    this.steer(0, 0, 0);
  }

  private async shoot() {
    if (this.shooting || !this.on) return;
    this.shooting = true;
    const f = this.flash;
    f.hidden = true; void f.offsetWidth; f.hidden = false;
    clearTimeout(this.flashTimer);
    this.flashTimer = setTimeout(() => { f.hidden = true; }, 400);
    try { await this.deps.shoot(); } finally { this.shooting = false; }
  }

  private update() {
    const s = this.session;
    if (!s) return;
    if (!this.hello && this.send(PH_HELLO)) this.hello = true;
    const st = s.status?.read();                  // null while the game writes it: the last state stands
    if (!st || !st.has('photo')) return;
    const bits = st.get('photo')!;
    this.can = (bits & 1) !== 0;
    if (((bits & 2) !== 0) !== this.on) this.show((bits & 2) !== 0);
    if (!this.on) return;
    if (pageDialog()) this.letGo();                 // the controller goes to the dialog, a key's up to it too
    this.free = (bits & 4) !== 0;
    for (const b of this.root.querySelectorAll<HTMLButtonElement>('[data-mode]')) b.classList.toggle('on', (b.dataset.mode === '1') === this.free);
    if (((bits & 16) !== 0) !== this.namesHere) { this.namesHere = !this.namesHere; this.fill(); }
    this.names.checked = (bits & 8) !== 0;
    const fov = st.get('photo_fov') ?? 70, roll = st.get('photo_roll') ?? 0;
    if (this.sliding !== this.zoom) this.zoom.value = String(Math.round((FOV_MAX - fov) / (FOV_MAX - FOV_MIN) * 1000));
    if (this.sliding !== this.tilt) this.tilt.value = String(Math.round(roll * 10));
  }

  /** photo mode came on or went off in the game */
  private show(on: boolean) {
    if (on === this.on) return;
    this.on = on;
    this.keys = 0; this.padBits = 0; this.drag = null; this.fingers.clear(); this.pinch = null; this.sliding = null;
    this.sent = { hold: -1, move: -1, lift: -1, turn: -1 };
    this.stage.classList.toggle('photo', on);
    if (on) {
      this.input()?.releaseAll?.();                 // what the game held when the page took the keys goes up
      this.steer(0, 0, 0);
      this.fill();
      this.setBare(false);
      this.deps.log('[app] photo mode: on');
    } else {
      this.root.hidden = true; this.flash.hidden = true;
      this.deps.log('[app] photo mode: off');
    }
  }

  private setBare(bare: boolean) {
    this.bare = bare;
    this.root.hidden = !this.on || bare;
  }

  /** the panel's texts in the page's language, the hints for the device in use */
  private fill() {
    const q = (s: string) => this.root.querySelector(s)!;
    q('.ph-title').textContent = t('Photo mode');
    q('[data-mode="0"]').textContent = t('Orbit');
    q('[data-mode="1"]').textContent = t('Free');
    q('.ph-zoom-l').textContent = t('Zoom');
    q('.ph-tilt-l').textContent = t('Tilt');
    q('.ph-names-l').textContent = t('Names');
    (q('.ph-check') as HTMLElement).hidden = !this.namesHere;
    q('[data-act="reset"]').textContent = t('Reset');
    q('[data-act="shoot"]').textContent = t('Take photo');
    q('[data-act="leave"]').textContent = t('Leave');
    const names = (k: string): Hint[] => (this.namesHere ? [[k, N('Names')]] : []);
    const list: Hint[] = touchFirst() && !padSeen()
      ? [[t('Drag'), N('Turn')], [t('Two fingers'), N('Move')], [t('Pinch'), N('Closer, farther')]]
      : padSeen()
        ? [['R', N('Turn')], ['L', N('Move')], ['LB RB', N('Down, up')], ['LT RT', N('Closer, farther')],
           ['↑ ↓', N('Zoom')], ['← →', N('Tilt')], ['Y', N('Orbit or free')], ['X', N('Reset')], ...names('RS'),
           ['A', N('Take photo')], ['B', N('Leave')]]
        : [[t('Drag'), N('Turn')], ['W A S D', N('Move')], ['Q E', N('Down, up')], [t('Wheel'), N('Closer, farther')],
           ['Z X', N('Zoom')], ['C V', N('Tilt')], ['Tab', N('Orbit or free')], ['R', N('Reset')], ...names('N'),
           ['H', N('Hide the panel')], ['F12', N('Take photo')], ['Esc', N('Leave')]];
    this.hints.replaceChildren(...list.map(([k, what]) => {
      const row = document.createElement('span'), kbd = document.createElement('kbd');
      kbd.textContent = k;
      row.append(kbd, document.createTextNode(t(what)));
      return row;
    }));
  }

  // ---- the keyboard, the mouse and the touch screen while photo mode is on ---------------------------------------
  private onKey(e: KeyboardEvent, down: boolean) {
    const bit = HELD[e.code];
    if (!down && bit && this.keys & bit) { this.keys &= ~bit; this.steer(); }   // let go wherever it went (a dialog, Ctrl)
    if (!this.on || PAGE_KEYS.has(e.code) || pageDialog()) return;
    e.stopImmediatePropagation();                   // the game gets none of them
    if (e.metaKey || e.ctrlKey) return;             // the browser's shortcuts stay the browser's
    e.preventDefault();
    if (bit) { if (down) { this.keys |= bit; this.steer(); } return; }
    if (!down || e.repeat) return;
    if (e.code === 'Escape') this.leave();
    else if (e.code === 'Enter' || e.code === 'Space' || e.code === 'NumpadEnter') void this.shoot();
    else if (e.code === 'Tab') this.send(PH_MODE, 2);
    else if (e.code === 'KeyR') this.send(PH_RESET);
    else if (e.code === 'KeyN' && this.namesHere) this.send(PH_NAMES, 2);
    else if (e.code === 'KeyH') this.setBare(!this.bare);
  }

  /** an event on the game's picture (not on the panel or another control of the page's) while photo mode is on */
  private mine(e: Event): boolean {
    const el = e.target as HTMLElement | null;
    return this.on && !!el && this.stage.contains(el) && !el.closest('[data-nfsu2-ui]') && !pageDialog();
  }

  private onPointer(e: PointerEvent) {
    if (e.type === 'pointerdown' ? !this.mine(e) : !this.on || !(this.drag?.id === e.pointerId || this.fingers.has(e.pointerId))) return;
    e.stopImmediatePropagation(); e.preventDefault();
    const h = Math.max(1, this.session?.viewRect().h ?? 1), x = e.clientX, y = e.clientY;
    if (e.pointerType === 'touch') { this.onFinger(e, h); return; }
    if (e.type === 'pointerdown') {
      if (this.drag) return;
      this.drag = { id: e.pointerId, x, y, pan: e.button !== 0 };
      try { (e.target as Element).setPointerCapture(e.pointerId); } catch { /* a synthetic pointer */ }
    } else if (e.type === 'pointermove' && this.drag) {
      const dx = Math.round((x - this.drag.x) / h * 10000), dy = Math.round((y - this.drag.y) / h * 10000);
      this.drag.x = x; this.drag.y = y;
      if (dx || dy) this.send(this.drag.pan ? PH_PAN : PH_DRAG, dx, dy);
    } else if (e.type === 'pointerup' || e.type === 'pointercancel') this.drag = null;
  }

  /** one finger turns the camera; two move it (their middle) and bring it closer or farther (their spread) */
  private onFinger(e: PointerEvent, h: number) {
    if (e.type === 'pointerdown') this.fingers.set(e.pointerId, [e.clientX, e.clientY]);
    else if (e.type === 'pointerup' || e.type === 'pointercancel') this.fingers.delete(e.pointerId);
    const was = this.fingers.get(e.pointerId);
    if (e.type === 'pointermove' && was) {
      if (this.fingers.size === 1) {
        const dx = Math.round((e.clientX - was[0]) / h * 10000), dy = Math.round((e.clientY - was[1]) / h * 10000);
        if (dx || dy) this.send(PH_DRAG, dx, dy);
      }
      this.fingers.set(e.pointerId, [e.clientX, e.clientY]);
    }
    const two = [...this.fingers.values()].slice(0, 2);
    if (two.length < 2) { this.pinch = null; return; }
    const d = Math.hypot(two[0][0] - two[1][0], two[0][1] - two[1][1]), mx = (two[0][0] + two[1][0]) / 2, my = (two[0][1] + two[1][1]) / 2;
    const p = this.pinch;
    this.pinch = { d, x: mx, y: my };
    if (!p || e.type !== 'pointermove') return;
    const dx = Math.round((mx - p.x) / h * 10000), dy = Math.round((my - p.y) / h * 10000);
    if (dx || dy) this.send(PH_PAN, dx, dy);
    if (p.d > 8 && d > 8) {
      const notches = Math.log(d / p.d) / 0.12;      // runtime/photo.c: the distance times e^(-0.12) a notch
      if (Math.abs(notches) > 0.001) this.send(PH_DOLLY, Math.round(notches * 1000));
    }
  }

  private onWheel(e: WheelEvent) {
    if (!this.mine(e)) return;
    e.stopImmediatePropagation(); e.preventDefault();
    const lines = e.deltaMode === 1 ? e.deltaY : e.deltaMode === 2 ? e.deltaY * 3 : e.deltaY / 100;
    if (lines) this.send(PH_DOLLY, Math.round(-lines * 1000));
  }
}
