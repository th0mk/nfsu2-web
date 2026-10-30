// Touch controls: the controls on the game on a phone or tablet, for driving (races and free roam) and for the menus.
//   steering  three ways, switched with the mode button (top right) or the Controls sheet's touch tab, and kept in
//             this browser:
//               buttons  ◀ ▶ press the arrow keys, so the game's own keyboard steering applies (the default)
//               stick    a thumb stick where the left thumb lands: the controller's left stick
//               tilt     the phone is the wheel, from its motion sensor: the controller's left stick.  Straight ahead
//                        is how the phone is held when driving first starts with tilt, kept through menus and races
//                        until the Straight ahead button sets it anew
//   pedals    Gas and Brake press the up and down arrows; Nitro and the handbrake are the controller's LB and RB
//             (Alt, the nitrous key, would turn the other keys into Alt+key).  Nitro holds Gas too, so the thumb on
//             Gas can go up to it, and a thumb slides between Gas, Brake and Nitro without letting go.  With tilt,
//             Brake and the handbrake go under the left thumb.
//   gears     with a manual gearbox, gear down and up above the left thumb: the controller's X and A, as on a pad
//             (Ctrl and Shift on the keyboard would be modifiers too)
//   small     pause, camera (held: look back), reset; in free roam also the map, messages, drift mode (X, lit while it
//             is on: drift.ts), and the horn and quick chat (quickchat.ts) while a friend's car is within a horn's reach;
//             Start (Engage) only while Engage does something: an event's or a shop's marker, an outrun offer, a race
//             spot or lobby (the game's status "roam_near" and "engage", runtime/web/status_web.c; a game without them
//             shows these always)
//   horizon   while tilt steers, the game's 3D view turns against the phone's tilt from straight ahead, so the road's
//             horizon stays level with the real one (the HUD stays on the phone; runtime/overrides.c sub_005c7ec0).
//             Not in a cutscene (a race's start); faded in and out.  A switch in the Controls sheet's touch tab.
//   menus     the arrow keys as a D-pad bottom left, OK (Enter) and Back (Esc) bottom right, in the bars beside the
//             game's 4:3 menus; a tap on the game stays a click.  The game repeats a held arrow itself.
//   cancels   the controls take every touch themselves (touch-action: none), so a cancelled one is the phone's: a
//             gesture of its own took every finger (on many phones three fingers take a screenshot), and they do
//             nothing more until lifted.  Driving, the first loss of three in this page says so (Nitro gives gas too:
//             a thumb slid up to it from Gas needs no third finger).  A finger whose lift never came at all is let go
//             when a new touch begins with no finger down.
//   size      all of them (and the menu's button) bigger or smaller, from the Controls sheet's touch tab, kept in this
//             browser: auto (autoTouchSize, for the screen) unless 80 to 150 % is picked; style.css --ts on the stage.
//             The game's menus' small buttons likewise (menuSize: the help bar's and the scrollers' arrows, auto
//             big enough for a finger), sent to the game (input menuScale, runtime/menusize.c).
// The controller part is a virtual one the input module merges with a real one (gamepad.ts withTouch).
// The driving controls show while the game says the player drives ("drive" in its status block,
// runtime/web/status_web.c: 1 in a race, 2 in free roam, + 4 with a manual gearbox, + 8 in a cutscene), the menu's
// otherwise once the game draws ("frames"); both once a finger has touched the game (at once on a touch screen), and
// they hide when a keyboard or a controller is used, until the next touch.  The game reads its input once a frame,
// so a key a tap presses stays down HOLD_MS at least.
import { STORAGE } from '@game/storage';
import { PAD_A, PAD_LB, PAD_RB, PAD_X } from '../input/proto';
import type { GameSession } from './game';
import { touchFirst } from './device';
import { N, percent, t } from './i18n';

export type Steering = 'buttons' | 'stick' | 'tilt';
export const STEERING: readonly Steering[] = ['buttons', 'stick', 'tilt'];
const NAMES: Record<Steering, string> = { buttons: N('buttons'), stick: N('thumb stick'), tilt: N('tilt') };
const LIST_NAMES: Record<Steering, string> = { buttons: N('Buttons'), stick: N('Thumb stick'), tilt: N('Tilt') };
const STORE = STORAGE.LOCAL.TOUCH_STEERING;
const HOLD_MS = 150;
/** a control with a hold (Camera): down this long, it presses the hold's keys instead of its own */
const LONG_MS = 250;

const STEER_EVENT = STORE;
let steeringNow: Steering = (() => {
  try { const s = localStorage.getItem(STORE) as Steering; return STEERING.includes(s) ? s : 'buttons'; } catch { return 'buttons'; }
})();
/** how the touch controls steer: the mode button's choice and the Controls sheet's, kept in this browser */
export function steering(): Steering { return steeringNow; }
export function setSteering(to: Steering) {
  if (to === steeringNow || !STEERING.includes(to)) return;
  steeringNow = to;
  try { localStorage.setItem(STORE, to); } catch { /* not kept: a private window */ }
  dispatchEvent(new Event(STEER_EVENT));
}
/** fn runs when the steering changes */
export function onSteering(fn: () => void) { addEventListener(STEER_EVENT, fn); }
/** the steering's name in a list (the Controls sheet), in the page's language */
export function steeringName(s: Steering): string { return t(LIST_NAMES[s]); }
/** iOS asks once whether the page may read the motion sensor, from a tap: false when it was not allowed */
export function askMotion(): Promise<boolean> {
  const D = typeof DeviceOrientationEvent === 'undefined' ? null
    : DeviceOrientationEvent as unknown as { requestPermission?(): Promise<string> };
  if (typeof D?.requestPermission !== 'function') return Promise.resolve(true);
  return D.requestPermission().then(r => r === 'granted', () => false);
}
/** the thumb stick: px from its centre to full lock; tilt: degrees of roll to full lock */
const STICK_PX = 64, TILT_DEG = 30;
/** the level horizon: the most it turns the view (degrees), how long it takes to come and go (ms) */
const LEVEL_MAX_DEG = 60, LEVEL_FADE_MS = 400;

const LEVEL_STORE = STORAGE.LOCAL.TOUCH_LEVEL, LEVEL_EVENT = LEVEL_STORE;
let levelOn = (() => { try { return localStorage.getItem(LEVEL_STORE) !== 'off'; } catch { return true; } })();
/** the level horizon's switch (the Controls sheet): on unless turned off in this browser */
export function levelHorizon(): boolean { return levelOn; }
export function setLevelHorizon(on: boolean) {
  levelOn = on;
  try { localStorage.setItem(LEVEL_STORE, on ? 'on' : 'off'); } catch { /* not kept: a private window */ }
  dispatchEvent(new Event(LEVEL_EVENT));
}
/** fn runs when the switch changes */
export function onLevelHorizon(fn: () => void) { addEventListener(LEVEL_EVENT, fn); }

/** a size: auto (for the screen), or a share of the size at 100 % */
export type Size = 'auto' | number;
/** a size kept in this browser, picked in the Controls sheet's touch tab: auto unless another was picked */
export interface SizeSetting {
  readonly sizes: readonly Size[];
  get(): Size;
  set(to: Size): void;
  /** the size now: the one picked, or auto's for the screen */
  scale(): number;
  /** a size in the Controls sheet's list, in the page's language: auto with what it gives on this screen */
  name(s: Size): string;
  /** fn runs when the choice changes, or what auto gives (the screen's size changed) */
  on(fn: () => void): void;
  /** auto may give another size now (the screen's changed, or what it measures is there) */
  check(): void;
}
function sizeSetting(store: string, sizes: readonly Size[], auto: () => number): SizeSetting {
  let now: Size = (() => {
    try { const n = Number(localStorage.getItem(store)); return sizes.includes(n) ? n : 'auto'; } catch { return 'auto'; }
  })();
  let autoNow = 0;
  const changed = () => dispatchEvent(new Event(store));
  const self: SizeSetting = {
    sizes,
    get: () => now,
    set(to) {
      if (to === now || !sizes.includes(to)) return;
      now = to;
      try { if (to === 'auto') localStorage.removeItem(store); else localStorage.setItem(store, String(to)); } catch { /* not kept: a private window */ }
      changed();
    },
    scale: () => now === 'auto' ? auto() : now,
    name: s => s === 'auto' ? t('Auto ({size})', { size: percent(auto()) }) : percent(s),
    on: fn => addEventListener(store, fn),
    check() { const a = auto(); if (a !== autoNow) { autoNow = a; changed(); } },
  };
  addEventListener('resize', () => self.check());
  return self;
}

/** the px the controls take between their margins at 100 % (style.css), the most they do (free roam with a manual
 *  gearbox): across, the row of small buttons; down, that row over the controls under the left thumb.  Upright, the
 *  space under the picture: across, the steering beside the pedals; down, two rows over the left thumb's */
const ROOM = { across: 608, down: 324 }, ROOM_UPRIGHT = { across: 384, down: 372 };
/** the controls' root, once there: its padding is their margins */
let edges: HTMLElement | null = null;
const upright = () => document.documentElement.hasAttribute('data-touch') && innerHeight >= innerWidth;
/** the touch controls' auto: 100 % (the size they were made at) on a phone, more on a tablet (125 % from 750 px
 *  across), but no more than fits on this screen: in steps of 5 %, 80 % at the least */
function autoTouchSize(): number {
  const w = innerWidth, h = innerHeight;
  const want = Math.min(1.25, Math.max(1, Math.min(w, h) / 600));
  // their margins: the root's padding (style.css .touch), 12 px before it is there; upright, the top's under the picture
  const m = edges ? getComputedStyle(edges) : null, px = (v?: string) => parseFloat(v ?? '') || 12;
  const room = upright() ? ROOM_UPRIGHT : ROOM;
  const fit = Math.min((w - px(m?.paddingLeft) - px(m?.paddingRight)) / room.across,
    (h - px(m?.paddingTop) - px(m?.paddingBottom)) / room.down);
  return Math.max(0.8, Math.floor(Math.min(want, fit) * 20 + 1e-9) / 20);
}
/** the touch controls' size (style.css --ts) */
export const touchSize = sizeSetting(STORAGE.LOCAL.TOUCH_SIZE, ['auto', 0.8, 0.9, 1, 1.1, 1.25, 1.5], autoTouchSize);

/** the game's menus' help bar buttons and scroller arrows, their height in the menus' 640 x 480 units: the bar's
 *  frames, 24 (the arrows are 26) */
const MENU_BUTTON_UNITS = 24, MENU_BUTTON_PX = 36;
/** the menus' auto, on a touch screen: big enough that a help bar button is MENU_BUTTON_PX high, in steps of 25 %,
 *  up to 200 %; with a mouse, the game's own size.  The menus fill the picture's height, or its width where it is
 *  narrower than 4:3 (upright: a 4:5 picture at the top, style.css) */
function autoMenuSize(): number {
  if (!touchFirst()) return 1;
  let w = innerWidth, h = innerHeight;
  if (upright()) { h = Math.min(1.25 * w, 0.6 * h); w = 0.8 * h; }
  const unit = Math.min(w / 640, h / 480);
  return Math.min(2, Math.max(1, Math.floor(MENU_BUTTON_PX / (MENU_BUTTON_UNITS * unit) * 4 + 1e-9) / 4));
}
/** the game's menus' small buttons: the help bar's and the scrollers' arrows (runtime/menusize.c) */
export const menuSize = sizeSetting(STORAGE.LOCAL.MENU_SIZE, ['auto', 1, 1.25, 1.5, 1.75, 2], autoMenuSize);

/** label: what a screen reader says (English, translated where drawn); text: the words on it, if any; keys: the keys
 *  it holds (two controls may hold the same one: it is down while either is) */
interface Ctl {
  id: string; el: HTMLElement; label: string; text?: string;
  keys?: string[]; bit?: number; act?: () => void;
  /** keys a hold presses for as long as it lasts (from LONG_MS); a tap presses the control's own as it lifts */
  hold?: string[];
  /** a finger slides between the controls of a group */
  group?: 'steer' | 'pedal';
  fingers: Set<number>; downAt: number; upTimer: number; holdTimer: number; held: boolean;
}

/** which controls show: the driving ones, the menu's, or none */
type Mode = 'drive' | 'menu' | '';

/** the phone's gesture took three fingers off the controls and the player was told (once in a page) */
let toldCancel = false;

const path = (d: string) => `<svg viewBox="0 0 24 24" aria-hidden="true">${d}</svg>`;
const ICONS = {
  left: '<path class="fill" d="M16 5L7 12l9 7z"/>',
  right: '<path class="fill" d="M8 5l9 7-9 7z"/>',
  pause: '<path d="M9 6v12M15 6v12"/>',
  camera: '<path d="M4 8h3l2-2h6l2 2h3v10H4z"/><circle cx="12" cy="13" r="3"/>',
  reset: '<path d="M6 13a6 6 0 1 0 2-5.2"/><path d="M5 4v4h4"/>',
  map: '<path d="M4 6l5-2 6 2 5-2v14l-5 2-6-2-5 2z"/><path d="M9 4v14M15 6v14"/>',
  msgs: '<path d="M4 6h16v12H4z"/><path d="M4 7l8 6 8-6"/>',
  horn: '<path d="M4 10v4h3l6 4V6l-6 4z"/><path d="M17 9a4 4 0 0 1 0 6"/>',
  chat: '<path d="M5 5h14a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1h-7l-4 3v-3H5a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1z"/>',
  center: '<circle cx="12" cy="12" r="6"/><path d="M12 3v3M12 18v3M3 12h3M18 12h3"/>',
  buttons: '<path d="M10 7l-5 5 5 5M14 7l5 5-5 5"/>',
  stick: '<circle cx="12" cy="12" r="8"/><circle class="fill" cx="12" cy="12" r="3"/>',
  tilt: '<rect x="4" y="8" width="16" height="9" rx="2" transform="rotate(-18 12 12)"/>',
  down: '<path d="M6 9l6 6 6-6"/>',
  up: '<path d="M6 15l6-6 6 6"/>',
  menuUp: '<path class="fill" d="M5 16l7-9 7 9z"/>',
  menuDown: '<path class="fill" d="M5 8l7 9 7-9z"/>',
};

/** a word on a small round control (Drift): 12 px, or as much smaller as a language's longer word needs to fit inside
 *  (WORD_FIT px across at 100 %), 9 px at the least */
const WORD_PX = 12, WORD_FIT = 40;
let measure: CanvasRenderingContext2D | null | undefined;
function wordPx(word: string): number {
  measure ??= document.createElement('canvas').getContext('2d');
  if (!measure) return WORD_PX;
  measure.font = `italic 700 ${WORD_PX}px ${getComputedStyle(document.documentElement).getPropertyValue('--font') || 'sans-serif'}`;
  const w = measure.measureText(word).width;
  return w <= WORD_FIT ? WORD_PX : Math.max(9, Math.floor(WORD_PX * WORD_FIT / w * 2) / 2);
}

/** the roll of the phone in the screen's plane, radians, 0 = upright, + = turned clockwise (a wheel turned right);
 *  null when it lies too flat to tell.  Gravity in the device's frame from beta and gamma (W3C device orientation:
 *  x right, y up, z out of the screen in the natural orientation), then turned into the screen's frame. */
export function rollOf(beta: number, gamma: number, screenAngle: number): number | null {
  const rad = Math.PI / 180, b = beta * rad, g = gamma * rad, a = screenAngle * rad;
  const gx = Math.cos(b) * Math.sin(g), gy = -Math.sin(b);
  const sx = gx * Math.cos(a) - gy * Math.sin(a), sy = gx * Math.sin(a) + gy * Math.cos(a);
  if (Math.hypot(sx, sy) < 0.3) return null;
  return Math.atan2(sx, -sy);
}

export interface TouchDeps {
  toast(msg: string, ms: number): void;
  log(s: string): void;
  /** the chat button: quick chat's phrases (quickchat.ts) */
  chat?(): void;
}

export class TouchControls {
  private root: HTMLElement;
  private ctls = new Map<string, Ctl>();
  private byFinger = new Map<number, Ctl>();
  /** the keys the controls hold, each with how many hold it */
  private keys = new Map<string, number>();
  /** fingers on the controls cancelled together (the phone's gesture): how many the controls held, how many went */
  private cancelled: { held: number; n: number } | null = null;
  private steering = steering();
  private session: GameSession | null = null;
  private raf = 0;
  // the tick under a finger pressing a control (off with the Rumble setting)
  private ticks = true;
  private drive = 0;
  private frames = 0;
  /** free roam: Engage does something, friends' cars within a horn's reach (undefined: a game that does not say),
   *  drift mode (drift.ts: 1 chosen) */
  private engage: number | undefined;
  private near: number | undefined;
  private drift = 0;
  private statusAt = 0;
  /** a finger touched the game; a keyboard or controller was used after the last touch */
  private seen = touchFirst();
  private away = false;
  private touchedAt = 0;
  private mode: Mode = '';
  private padButtons = 0;
  private lx = 0;
  // the thumb stick: its finger and where it landed
  private stickEl: HTMLElement;
  private baseEl: HTMLElement;
  private knobEl: HTMLElement;
  private stickFinger = -1;
  private stickX0 = 0;
  // tilt: the roll that counts as straight ahead (null: the next reading; kept when the driving controls go and come
  // back, cleared when tilt is picked or the game ends), whether the sensor answered
  private center: number | null = null;
  private lastRoll: number | null = null;
  private tiltHeard = false;
  private tiltCheck = 0;
  // the level horizon: the tilt from straight ahead (radians, unsmoothed), how far it is faded in, what the game has
  private tilt = 0;
  private levelK = 0;
  private levelAt = 0;
  private sentRoll = 0;
  /** the controls' size now (touchSize), the menus' small buttons' (menuSize) */
  private scale = 0;
  private menuScale = 0;
  /** the game's input the menus' size went to (it comes some frames after the start) */
  private menuTo: unknown = null;

  constructor(private stage: HTMLElement, private deps: TouchDeps) {
    const root = this.root = document.createElement('div');
    root.className = 'touch'; root.hidden = true; root.dataset.nfsu2Ui = '';
    root.dataset.steering = this.steering;
    // the stick's zone first: the buttons above it take their own touches
    this.stickEl = document.createElement('div'); this.stickEl.className = 'stick';
    this.baseEl = document.createElement('div'); this.baseEl.className = 'stick-base';
    this.knobEl = document.createElement('div'); this.knobEl.className = 'stick-knob';
    this.baseEl.append(this.knobEl); this.stickEl.append(this.baseEl);
    root.append(this.stickEl);
    // inner: an icon (Nitro's is a picture, style.css); or text, the words on it ("\n": a second line)
    const add = (id: string, label: string, inner: string | { text: string }, c: Partial<Ctl>, cls = '') => {
      const el = document.createElement('div');
      el.className = `tc ${cls}`.trim(); el.dataset.ctl = id;
      el.setAttribute('role', 'button');
      if (typeof inner === 'string') el.innerHTML = inner;
      root.append(el);
      this.ctls.set(id, { id, el, label, text: typeof inner === 'string' ? undefined : inner.text, fingers: new Set(), downAt: 0, upTimer: 0,
        holdTimer: 0, held: false, ...c });
    };
    add('pause', N('Pause'), path(ICONS.pause), { keys: ['Escape'] }, 'small');
    add('camera', N('Camera'), path(ICONS.camera), { keys: ['KeyC'], hold: ['KeyB'] }, 'small');
    add('reset', N('Reset'), path(ICONS.reset), { keys: ['KeyR'] }, 'small');
    add('map', N('Map'), path(ICONS.map), { keys: ['KeyM'] }, 'small roam');
    add('msgs', N('Messages'), path(ICONS.msgs), { keys: ['Tab'] }, 'small roam');
    add('drift', N('Drift mode (in free roam)'), { text: N('Drift') }, { keys: ['KeyX'] }, 'small roam');
    add('horn', N('Horn'), path(ICONS.horn), { keys: ['KeyN'] }, 'small roam');
    add('chat', N('Quick chat'), path(ICONS.chat), { act: () => this.deps.chat?.() }, 'small roam');
    add('mode', N('Steering'), path(ICONS[this.steering]), { act: () => this.nextSteering() }, 'small');
    add('center', N('Straight ahead'), path(ICONS.center), { act: () => this.recenter() }, 'small');
    add('start', N('Start an event'), { text: N('Start') }, { keys: ['Enter'] }, 'pill roam');
    add('left', N('Steer left'), path(ICONS.left), { keys: ['ArrowLeft'], group: 'steer' }, 'big');
    add('right', N('Steer right'), path(ICONS.right), { keys: ['ArrowRight'], group: 'steer' }, 'big');
    add('brake', N('Brake'), { text: N('Brake') }, { keys: ['ArrowDown'], group: 'pedal' }, 'big');
    add('gas', N('Gas'), { text: N('Gas') }, { keys: ['ArrowUp'], group: 'pedal' }, 'big');
    add('nitro', N('Nitrous'), '<i class="nitro" aria-hidden="true"></i>', { keys: ['ArrowUp'], bit: 1 << PAD_LB, group: 'pedal' }, 'round');
    add('hb', N('Handbrake'), { text: N('Hand\nbrake') }, { bit: 1 << PAD_RB }, 'round');
    add('down', N('Gear down'), path(ICONS.down), { bit: 1 << PAD_X }, 'gear');
    add('up', N('Gear up'), path(ICONS.up), { bit: 1 << PAD_A }, 'gear');
    // the menus'
    add('m-up', N('Up'), path(ICONS.menuUp), { keys: ['ArrowUp'] }, 'menu dpad');
    add('m-left', N('Left'), path(ICONS.left), { keys: ['ArrowLeft'] }, 'menu dpad');
    add('m-right', N('Right'), path(ICONS.right), { keys: ['ArrowRight'] }, 'menu dpad');
    add('m-down', N('Down'), path(ICONS.menuDown), { keys: ['ArrowDown'] }, 'menu dpad');
    add('m-back', N('Back'), { text: N('Back') }, { keys: ['Escape'] }, 'menu wide');
    add('m-ok', N('OK'), { text: N('OK') }, { keys: ['Enter'] }, 'menu wide');
    this.relabel();
    onSteering(() => this.applySteering(steering()));
    stage.append(root);
    touchSize.on(() => this.applySize());
    menuSize.on(() => this.applyMenuSize());
    edges = root;
    this.applySize();
    touchSize.check();
    menuSize.check();

    root.addEventListener('pointerdown', this.onDown);
    root.addEventListener('pointermove', this.onMove);
    root.addEventListener('pointerup', this.onUp);
    root.addEventListener('pointercancel', this.onUp);
    root.addEventListener('contextmenu', e => e.preventDefault());
    // a finger on the game brings the controls (back); a keyboard or controller sends them away
    stage.addEventListener('pointerdown', e => {
      if (e.pointerType !== 'touch') return;
      this.seen = true; this.away = false; this.touchedAt = performance.now(); this.update();
      // while driving, a finger beside the controls is no mouse for the game (it would draw its cursor in the race)
      if (this.mode === 'drive' && e.target instanceof HTMLCanvasElement) e.stopPropagation();
    }, { capture: true });
    addEventListener('keydown', e => { if (e.isTrusted && this.session) { this.away = true; this.update(); } }, { capture: true });
  }

  /** the controls' words in the page's language (again when it changes) */
  relabel() {
    for (const c of this.ctls.values()) {
      c.el.setAttribute('aria-label', t(c.label));
      if (c.text === undefined) continue;
      const lines = t(c.text).split('\n');
      if (c.el.classList.contains('small')) c.el.style.setProperty('--fs', `${wordPx(lines[0])}px`);
      if (lines.length === 1) { c.el.textContent = lines[0]; continue; }
      const span = document.createElement('span');
      lines.forEach((l, i) => { if (i) span.append(document.createElement('br')); span.append(l); });
      c.el.replaceChildren(span);
    }
  }

  /** a game started: the controls follow its state until stop() (ticks: the phone ticks under a finger) */
  start(s: GameSession, ticks: boolean) {
    this.session = s;
    this.ticks = ticks;
    this.drive = 0;
    this.menuTo = null;
    const tick = () => { this.frame(); this.raf = requestAnimationFrame(tick); };
    this.raf = requestAnimationFrame(tick);
  }

  /** for tests and logs: what the controls do now (shown: the driving ones, menu: the menu's) */
  state() {
    return { shown: this.mode === 'drive', menu: this.mode === 'menu', drive: this.drive, steering: this.steering, lx: this.lx,
      buttons: this.padButtons, keys: [...this.keys.keys()], roll: this.sentRoll, menuScale: this.menuScale, menuSent: !!this.menuTo,
      engage: this.engage, near: this.near, drift: this.drift };
  }

  /** the game ended: everything let go, nothing of it kept */
  stop() {
    cancelAnimationFrame(this.raf);
    this.setMode('');
    this.levelK = this.tilt = 0;
    this.center = this.lastRoll = null;
    if (this.sentRoll) { this.sentRoll = 0; this.session?.input?.viewRoll?.(0); }
    this.session = null;
    this.drive = this.frames = 0;
  }

  private frame() {
    const s = this.session;
    if (!s) return;
    const now = performance.now();
    if (now - this.statusAt > 100) {
      this.statusAt = now;
      const st = s.status?.read();                // null while the game writes it: the last state stands
      if (st) {
        this.drive = st.get('drive') ?? 0; this.frames = st.get('frames') ?? 0;
        this.engage = st.get('engage'); this.near = st.get('roam_near'); this.drift = st.get('drift') ?? 0;
      }
    }
    if (!this.away && (s.input?.padPressed?.() ?? 0) > this.touchedAt) this.away = true;
    if (s.input && s.input !== this.menuTo) { this.menuTo = s.input; this.applyMenuSize(true); }
    this.update();
    this.level(now);
  }

  /** the level horizon: the view turned against the tilt while tilt steers outside a cutscene, faded in and out
   *  (smoothstep) as that starts and ends, the tilt itself as the sensor gives it (a smoothed one would lag) */
  private level(now: number) {
    const on = this.mode === 'drive' && this.steering === 'tilt' && levelOn && (this.drive & 8) === 0;
    const dt = Math.min(100, now - this.levelAt);
    this.levelAt = now;
    this.levelK = Math.max(0, Math.min(1, this.levelK + (on ? dt : -dt) / LEVEL_FADE_MS));
    const k = this.levelK, roll = k * k * (3 - 2 * k) * this.tilt;
    if (roll === this.sentRoll) return;
    this.sentRoll = roll;
    this.session?.input?.viewRoll?.(roll);
  }

  private update() {
    const on = !!this.session && this.seen && !this.away;
    const mode: Mode = !on ? '' : (this.drive & 3) > 0 ? 'drive' : this.frames > 0 ? 'menu' : '';
    this.root.toggleAttribute('data-roam', (this.drive & 3) === 2);
    this.root.toggleAttribute('data-gears', (this.drive & 4) !== 0);
    // free roam's buttons for now and then: Start while Engage does something, Horn and Quick chat with friends near
    // (one that goes from under a finger lets go)
    this.root.toggleAttribute('data-no-engage', this.engage === 0);
    this.root.toggleAttribute('data-alone', this.near === 0);
    if (this.engage === 0) this.letGo('start');
    if (this.near === 0) this.letGo('horn');
    this.ctls.get('drift')!.el.classList.toggle('lit', (this.drift & 1) !== 0);
    this.setMode(mode);
  }

  /** the driving controls, the menu's or none: every control let go on a change, the stick and tilt at rest */
  private setMode(mode: Mode) {
    if (mode === this.mode) return;
    const drove = this.mode === 'drive';
    for (const c of this.ctls.values()) { c.fingers.clear(); this.up(c, true); }
    this.byFinger.clear();
    this.stickUp();
    this.tiltOff();
    this.lx = 0; this.padButtons = 0;
    this.mode = mode;
    this.root.dataset.mode = mode;
    this.root.hidden = !mode;
    if (mode === 'drive' && this.steering === 'tilt') this.tiltOn();
    if (drove || mode === 'drive') this.sendPad();
  }

  private sendPad() {
    this.session?.input?.touchPad?.({ buttons: this.padButtons, lx: this.steering === 'buttons' ? 0 : this.lx });
  }

  // ---- fingers on the controls --------------------------------------------------------------
  private ctlAt(x: number, y: number): Ctl | undefined {
    const el = document.elementFromPoint(x, y)?.closest<HTMLElement>('[data-ctl]');
    return el ? this.ctls.get(el.dataset.ctl!) : undefined;
  }
  private onDown = (e: PointerEvent) => {
    e.preventDefault();                       // the game's canvas keeps the focus, no mouse events follow
    // the first finger of a new touch: no other is down, so one the controls still hold never lifted (the phone's)
    if (e.pointerType === 'touch' && e.isPrimary) this.dropFingers();
    const el = (e.target as HTMLElement).closest<HTMLElement>('[data-ctl]');
    const c = el ? this.ctls.get(el.dataset.ctl!) : undefined;
    if (c) { this.byFinger.set(e.pointerId, c); this.press(c, e.pointerId); return; }
    if ((e.target as HTMLElement).closest('.stick')) this.stickDown(e);
  };
  private onMove = (e: PointerEvent) => {
    if (e.pointerId === this.stickFinger) { this.stickMove(e); return; }
    const c = this.byFinger.get(e.pointerId);
    if (!c?.group) return;
    const to = this.ctlAt(e.clientX, e.clientY);
    if (!to || to === c || to.group !== c.group) return;
    // the next one first: a key both hold (Gas's, Nitro's) stays down
    this.byFinger.set(e.pointerId, to);
    this.press(to, e.pointerId);
    this.release(c, e.pointerId);
  };
  private onUp = (e: PointerEvent) => {
    if (e.type === 'pointercancel') this.noteCancel(e.pointerId);
    if (e.pointerId === this.stickFinger) { this.stickUp(); this.sendPad(); return; }
    const c = this.byFinger.get(e.pointerId);
    if (!c) return;
    this.byFinger.delete(e.pointerId);
    this.release(c, e.pointerId);
    if (c.act && e.type === 'pointerup') c.act();       // the release is a user activation (tilt's permission)
  };
  /** a finger on the controls cancelled: the phone's gesture (above) cancels every finger at once, so those of the
   *  next moment are counted with it; logged, and driving, the first loss of three in this page explained */
  private noteCancel(finger: number) {
    if (!this.byFinger.has(finger) && finger !== this.stickFinger) return;
    if (!this.cancelled) {
      this.cancelled = { held: this.byFinger.size + (this.stickFinger !== -1 ? 1 : 0), n: 0 };
      setTimeout(() => {
        const { held, n } = this.cancelled!;
        this.cancelled = null;
        this.deps.log(`[app] touch controls: ${n} of ${held} finger(s) cancelled by the phone`);
        if (n < 3 || this.mode !== 'drive' || toldCancel) return;
        toldCancel = true;
        this.deps.toast(t('The phone took your fingers off the controls: on many phones, three fingers on the screen take a screenshot. Nitro gives gas too, so slide your thumb up to it from Gas, or turn the three-finger gesture off in the phone\'s settings.'), 10000);
      }, 50);
    }
    this.cancelled.n++;
  }
  /** every finger the controls hold let go: a new touch began with none down, so their lifts never came */
  private dropFingers() {
    const n = this.byFinger.size + (this.stickFinger !== -1 ? 1 : 0);
    if (!n) return;
    for (const [finger, c] of this.byFinger) this.release(c, finger);
    this.byFinger.clear();
    if (this.stickFinger !== -1) { this.stickUp(); this.sendPad(); }
    this.deps.log(`[app] touch controls: ${n} finger(s) let go that never lifted (the phone took them)`);
  }

  private press(c: Ctl, finger: number) {
    c.fingers.add(finger);
    if (c.fingers.size > 1) return;
    c.el.classList.add('on');
    if (c.act) return;
    if (this.ticks) navigator.vibrate?.(8);
    if (c.hold) {                             // a hold from LONG_MS on, else a tap as it lifts (release)
      clearTimeout(c.holdTimer);
      c.holdTimer = setTimeout(() => {
        c.holdTimer = 0;
        if (!c.fingers.size) return;
        c.held = true;
        for (const k of c.hold!) this.keyDown(k);
      }, LONG_MS) as unknown as number;
      return;
    }
    if (c.upTimer) { clearTimeout(c.upTimer); c.upTimer = 0; return; }   // still down from the last tap
    c.downAt = performance.now();
    for (const k of c.keys ?? []) this.keyDown(k);
    if (c.bit) { this.padButtons |= c.bit; this.sendPad(); }
  }
  private release(c: Ctl, finger: number) {
    c.fingers.delete(finger);
    if (c.fingers.size) return;
    c.el.classList.remove('on');
    if (c.act) return;
    if (c.hold) {
      if (c.held) { c.held = false; for (const k of c.hold) this.keyUp(k); return; }
      if (!c.holdTimer) return;
      clearTimeout(c.holdTimer); c.holdTimer = 0;
      // a tap: the control's own keys down now (or still, from the last tap), up HOLD_MS later
      if (c.upTimer) clearTimeout(c.upTimer);
      else { c.downAt = performance.now(); for (const k of c.keys ?? []) this.keyDown(k); }
      c.upTimer = setTimeout(() => { c.upTimer = 0; this.up(c); }, HOLD_MS) as unknown as number;
      return;
    }
    // down HOLD_MS at least: the game reads its input once a frame, and a shorter tap could fall between two frames
    const wait = c.downAt + HOLD_MS - performance.now();
    if (wait > 0) c.upTimer = setTimeout(() => { c.upTimer = 0; if (!c.fingers.size) this.up(c); }, wait) as unknown as number;
    else this.up(c);
  }
  /** a control no finger can reach any more (hidden): its fingers and keys let go */
  private letGo(id: string) {
    const c = this.ctls.get(id)!;
    if (!c.fingers.size) return;
    for (const f of c.fingers) this.byFinger.delete(f);
    c.fingers.clear();
    this.up(c, true);
  }
  /** the control's keys or button up (now: also one waiting for its HOLD_MS, and a hold) */
  private up(c: Ctl, now = false) {
    if (now && c.hold) {
      clearTimeout(c.holdTimer); c.holdTimer = 0;
      if (c.held) { c.held = false; for (const k of c.hold) this.keyUp(k); }
    }
    if (now) { c.el.classList.remove('on'); if (!c.upTimer && !c.downAt) return; }
    clearTimeout(c.upTimer); c.upTimer = 0;
    if (!c.downAt) return;
    c.downAt = 0;
    for (const k of c.keys ?? []) this.keyUp(k);
    if (c.bit) { this.padButtons &= ~c.bit; this.sendPad(); }
  }
  /** a key goes down with the first control that holds it and up with the last */
  private keyDown(code: string) {
    const n = this.keys.get(code) ?? 0;
    this.keys.set(code, n + 1);
    if (!n) this.session?.input?.key?.(code, true);
  }
  private keyUp(code: string) {
    const n = this.keys.get(code) ?? 0;
    if (n > 1) { this.keys.set(code, n - 1); return; }
    if (!n) return;
    this.keys.delete(code);
    this.session?.input?.key?.(code, false);
  }

  // ---- the thumb stick ---------------------------------------------------------------------
  private stickDown(e: PointerEvent) {
    if (this.stickFinger !== -1) return;
    this.stickFinger = e.pointerId; this.stickX0 = e.clientX;
    const rc = this.stickEl.getBoundingClientRect();
    this.baseEl.style.left = `${e.clientX - rc.left}px`; this.baseEl.style.top = `${e.clientY - rc.top}px`;
    this.baseEl.classList.add('on');
    if (this.ticks) navigator.vibrate?.(8);
  }
  private stickMove(e: PointerEvent) {
    const lock = STICK_PX * this.scale;
    const dx = Math.max(-lock, Math.min(lock, e.clientX - this.stickX0));
    this.knobEl.style.setProperty('--dx', `${dx}px`);
    this.lx = dx / lock;
    this.sendPad();
  }
  private stickUp() {
    this.stickFinger = -1;
    this.baseEl.classList.remove('on');
    this.baseEl.style.left = this.baseEl.style.top = ''; this.knobEl.style.removeProperty('--dx');
    if (this.steering === 'stick') this.lx = 0;
  }

  // ---- tilt ----------------------------------------------------------------------------------
  private onOrient = (e: DeviceOrientationEvent) => {
    if (e.beta === null || e.gamma === null) return;
    this.tiltHeard = true;
    const r = rollOf(e.beta, e.gamma, screen.orientation?.angle ?? 0);
    if (r === null) return;
    this.lastRoll = r;
    if (this.center === null) this.center = r;
    let d = r - this.center;
    if (d > Math.PI) d -= 2 * Math.PI; else if (d < -Math.PI) d += 2 * Math.PI;
    const max = LEVEL_MAX_DEG * Math.PI / 180;
    this.tilt = Math.max(-max, Math.min(max, d));
    const v = Math.max(-1, Math.min(1, d / (TILT_DEG * Math.PI / 180)));
    this.lx = this.lx * 0.4 + v * 0.6;                  // a little smoothing of the sensor's jitter
    this.sendPad();
  };
  private tiltOn() {
    removeEventListener('deviceorientation', this.onOrient);
    addEventListener('deviceorientation', this.onOrient);
    this.tiltHeard = false;
    clearTimeout(this.tiltCheck);
    this.tiltCheck = setTimeout(() => {
      if (!this.tiltHeard && this.mode === 'drive' && this.steering === 'tilt')
        this.deps.toast(t('Tilt has no motion sensor to read in this browser. The steering button (top right) switches to buttons.'), 6000);
    }, 2000) as unknown as number;
  }
  private tiltOff() {
    removeEventListener('deviceorientation', this.onOrient);
    clearTimeout(this.tiltCheck);
  }
  private recenter() {
    this.center = this.lastRoll;
    this.deps.toast(t('Straight ahead is where the phone is now.'), 2000);
  }

  // ---- the steering button -------------------------------------------------------------------
  private nextSteering() {
    const to = STEERING[(STEERING.indexOf(this.steering) + 1) % STEERING.length];
    if (to === 'tilt') void askMotion().then(ok => { if (!ok) this.deps.toast(t('Tilt needs the motion sensor, which was not allowed.'), 5000); });
    setSteering(to);
    this.deps.toast(to === 'tilt' ? t('Steering: tilt. Hold the phone like a wheel; the button beside this one sets straight ahead.')
      : t('Steering: {how}', { how: t(NAMES[to]) }), to === 'tilt' ? 5000 : 2000);
  }
  /** the steering changed (here or in the Controls sheet) */
  private applySteering(to: Steering) {
    if (to === this.steering) return;
    for (const c of this.ctls.values()) if (c.group) { c.fingers.clear(); this.up(c, true); }
    this.stickUp(); this.tiltOff();
    this.steering = to; this.lx = 0;
    if (to === 'tilt') this.center = null;
    this.root.dataset.steering = to;
    this.ctls.get('mode')!.el.innerHTML = path(ICONS[to]);
    if (this.mode === 'drive') { if (to === 'tilt') this.tiltOn(); this.sendPad(); }
    this.deps.log(`[app] touch controls: steering with ${NAMES[to]}`);
  }

  // ---- the size --------------------------------------------------------------------------------
  /** the size picked, or auto's for the screen now, on the stage (style.css --ts) */
  private applySize() {
    const s = touchSize.scale();
    if (s === this.scale) return;
    const was = this.scale;
    this.scale = s;
    this.stage.style.setProperty('--ts', String(s));
    if (was) this.deps.log(`[app] touch controls: size ${Math.round(s * 100)} %${touchSize.get() === 'auto' ? ' (auto)' : ''}`);
  }
  /** the menus' small buttons at the size picked, or auto's, for the game (runtime/menusize.c) */
  private applyMenuSize(force = false) {
    const s = menuSize.scale();
    if (s === this.menuScale && !force) return;
    const was = this.menuScale;
    this.menuScale = s;
    this.session?.input?.menuScale?.(s);
    if (was && s !== was) this.deps.log(`[app] menu buttons: size ${Math.round(s * 100)} %${menuSize.get() === 'auto' ? ' (auto)' : ''}`);
  }
}
