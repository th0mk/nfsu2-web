// Photo mode (runtime/photo.c): F12 in the world, or Photo Mode in the game's pause menu, stops the world, hides the
// HUD and lets the camera loose from the car; F12 again (the panel's button, A on a controller) takes the picture with
// the page's screenshot (shell.ts: a PNG beside the saves, shown in Saves on the start page).  Where photo mode cannot
// open (the menus, a race over the network) F12 takes the screenshot at once, as it always has.
// While it is on, the page keeps the keyboard, the mouse, the touch screen and the controller from the game and sends
// what they do as photo events (input/index.ts photo(): the keys and buttons held as bits, the sticks, drags, the
// wheel and pinches, the sliders' values), hides its other things on the game, and shows a panel in four tabs:
//   Camera    orbit or free, zoom, tilt, the camera's speed and smoothing (it glides and comes to a stop softly), turning
//             by itself, the free camera keeping its height, up and down turned round, the car the camera is on (the
//             player's, or the car in the middle of the picture), and a path of points it plays through
//   Picture   the lens (blur, focus, shutter, fisheye, shift), the look (brightness, vignette, filter), crop (lines of
//             thirds over it with Grid), the picture's size, the site's address on the pictures (photoMark: every F12
//             picture, menus too)
//   Scene     time standing or running on, the kinds of cars shown, the other cars' headlights, friends' names (in free
//             roam with them), horns, the weather
//   Controls  what the keys, the buttons or the fingers do
// and under each the picture, reset and leave.  Once the camera stands still the renderer adds up several frames into
// one picture (gpu/photo-picture.ts: smooth edges, the lens's blur; the Take photo button fills as they come); the look
// and the crop are the renderer's (Renderer.photoLook), shown on the game as they are kept.  The panel's choices stay
// for the next time (localStorage).  It reads the game's status (runtime/web/status_web.c) every frame: photo (1 it can
// open, 2 it is on, 4 the free camera, 8 the names on, 16 friends' names can show, 32 the focus follows the car, 256
// time stands, 512 other players' cars can be here), photo_fov, photo_roll, photo_focus, photo_path (its points, + 16
// playing), photo_target (the kind of car the camera is on), photo_pic (the picture's options allowed now).
// Freeze time off: the world runs on (traffic, racers, friends' cars), each frame is the picture, no Blur or Focus.
import type { GameSession } from './game';
import type { PadSnapshot } from '../input/gamepad';
import { CROPS, FILTERS, cropRect, focusM, focusV, type Filter } from './photolook';
import type { PhotoLook } from '../gpu/photo-picture';
import {
  PAD_A, PAD_AXIS_LT, PAD_AXIS_LX, PAD_AXIS_LY, PAD_AXIS_RT, PAD_AXIS_RX, PAD_AXIS_RY, PAD_B, PAD_BACK, PAD_DDOWN,
  PAD_DLEFT, PAD_DRIGHT, PAD_DUP, PAD_LB, PAD_LS, PAD_RB, PAD_RS, PAD_START, PAD_X, PAD_Y,
} from '../input/proto';
import { N, t, tn } from './i18n';
import { pageDialog } from './padnav';
import { padSeen, touchFirst } from './device';
import { keyName, menuKeyOf, siteKey } from './sitekeys';

// runtime/photo.c: the events (PH_*) and the bits of PH_HOLD (HB_*)
const PH_HELLO = 0, PH_OPEN = 1, PH_HOLD = 2, PH_MOVE = 3, PH_TURN = 4, PH_DRAG = 5, PH_PAN = 6, PH_DOLLY = 7,
  PH_FOV = 8, PH_ROLL = 9, PH_MODE = 10, PH_RESET = 11, PH_NAMES = 12, PH_FOCUS = 13, PH_BLUR = 14, PH_FREEZE = 16,
  PH_SPEED = 17, PH_SMOOTH = 18, PH_LEVEL = 19, PH_SPIN = 20, PH_PATH = 21, PH_SHOW = 22, PH_LIGHTS = 23, PH_HORNS = 24,
  PH_WEATHER = 25, PH_TARGET = 26, PH_SHUTTER = 27, PH_SIZE = 28, PH_SHIFT = 29, PH_LENS = 30;
const HB_FWD = 1, HB_BACK = 2, HB_LEFT = 4, HB_RIGHT = 8, HB_UP = 16, HB_DOWN = 32, HB_TL = 64, HB_TR = 128,
  HB_TU = 256, HB_TD = 512, HB_ZIN = 4096, HB_ZOUT = 8192, HB_RL = 16384, HB_RR = 32768, HB_FAST = 65536;
const PATH_CLEAR = 0, PATH_ADD = 1, PATH_PLAY = 2, PATH_STOP = 3;
const TARGET_MINE = 0, TARGET_MIDDLE = 1;
/** the kinds of cars (photo.c KIND_*), and their bits in PH_SHOW */
const SHOW_MINE = 1, SHOW_FRIENDS = 2, SHOW_CREWS = 4, SHOW_PLAYERS = 8, SHOW_TRAFFIC = 16, SHOW_RACERS = 32, SHOW_ALL = 63;
const FOV_MIN = 15, FOV_MAX = 110, ROLL_MAX = 45;
/** the shutter's times, ms (0: none) */
const SHUTTERS = [0, 17, 33, 67, 125, 250, 500, 1000];
const PATH_TIMES = [3, 5, 10, 20, 30, 60];

type Tab = 'camera' | 'picture' | 'scene' | 'controls';
const TABS: Tab[] = ['camera', 'picture', 'scene', 'controls'];

/** the panel's choices kept between photo modes */
interface Kept {
  tab: Tab; filter: Filter; exposure: number; vignette: number; crop: string; grid: boolean; blur: number; freeze: boolean;
  speed: number; smooth: number; spin: number; level: boolean; invert: boolean; pathS: number;
  show: number; lights: boolean; horns: boolean; weather: number;
  shutter: number; size: number; shiftX: number; shiftY: number; lens: number;
}
const KEPT_STORE = 'nfsu2-photo';
const KEPT0: Kept = {
  tab: 'camera', filter: 'none', exposure: 0, vignette: 0, crop: 'full', grid: false, blur: 0, freeze: true,
  speed: 500, smooth: 0, spin: 0, level: false, invert: false, pathS: 10,
  show: SHOW_ALL, lights: true, horns: true, weather: -1,
  shutter: 0, size: 1, shiftX: 0, shiftY: 0, lens: 0,
};
const clamp = (v: unknown, lo: number, hi: number, d: number) => { const n = Number(v); return Number.isFinite(n) ? Math.max(lo, Math.min(hi, n)) : d; };
function loadKept(): Kept {
  try {
    const raw = JSON.parse(localStorage.getItem(KEPT_STORE) ?? '{}') as Partial<Kept> & { cars?: number };
    const k = { ...KEPT0, ...raw } as Kept;
    if (!TABS.includes(k.tab)) k.tab = 'camera';
    if (!(k.filter in FILTERS)) k.filter = 'none';
    if (!(k.crop in CROPS)) k.crop = 'full';
    k.exposure = clamp(k.exposure, -200, 200, 0); k.vignette = clamp(k.vignette, 0, 1000, 0); k.blur = clamp(k.blur, 0, 1000, 0);
    k.speed = clamp(k.speed, 0, 1000, 500); k.smooth = clamp(k.smooth, 0, 1000, 0); k.spin = clamp(k.spin, -600, 600, 0);
    k.pathS = PATH_TIMES.includes(k.pathS) ? k.pathS : 10;
    // the Cars list before the kinds' boxes: all, only mine, none, friends and crews
    if (raw.show === undefined && raw.cars !== undefined) k.show = [SHOW_ALL, SHOW_MINE, 0, SHOW_MINE | SHOW_FRIENDS | SHOW_CREWS][raw.cars] ?? SHOW_ALL;
    k.show = clamp(k.show, 0, SHOW_ALL, SHOW_ALL) & SHOW_ALL;
    k.weather = [-1, 0, 1].includes(k.weather) ? k.weather : -1;
    k.shutter = SHUTTERS.includes(k.shutter) ? k.shutter : 0; k.size = [1, 2, 3, 4].includes(k.size) ? k.size : 1;
    k.shiftX = clamp(k.shiftX, -500, 500, 0); k.shiftY = clamp(k.shiftY, -500, 500, 0); k.lens = k.lens === 1 ? 1 : 0;
    for (const b of ['grid', 'level', 'invert'] as const) k[b] = !!k[b];
    for (const b of ['freeze', 'lights', 'horns'] as const) k[b] = k[b] !== false;
    delete (k as { cars?: number }).cars;
    return k;
  } catch { return { ...KEPT0 }; }
}
/** the Speed slider (0 .. 1000) as the camera's speed: a tenth at the left, as it was in the middle, 4 times at the right */
const speedOf = (v: number) => (v < 500 ? 0.1 ** ((500 - v) / 500) : 4 ** ((v - 500) / 500));

/** the keys that steer the camera while they are held */
const HELD: Record<string, number> = {
  KeyW: HB_FWD, KeyS: HB_BACK, KeyA: HB_LEFT, KeyD: HB_RIGHT, KeyE: HB_UP, KeyQ: HB_DOWN,
  ArrowLeft: HB_TL, ArrowRight: HB_TR, ArrowUp: HB_TU, ArrowDown: HB_TD,
  KeyZ: HB_ZIN, KeyX: HB_ZOUT, KeyC: HB_RL, KeyV: HB_RR, ShiftLeft: HB_FAST, ShiftRight: HB_FAST,
};
/** the page's own keys, which stay the page's (shell.ts): the menu's, photo mode's, the next song's, the counter's (the
 *  player's: sitekeys.ts) and F11 */
const pageKey = (code: string) => code === 'F11' || menuKeyOf(code) !== null;

/** two values of -1..1 as a photo event's int16 pair */
const pack = (x: number, y: number) =>
  (Math.round(Math.max(-1, Math.min(1, x)) * 32767) & 0xffff) | (Math.round(Math.max(-1, Math.min(1, y)) * 32767) << 16);
/** a stick's value past its dead zone, from 0 there */
const dz = (v: number) => (Math.abs(v) < 0.15 ? 0 : (v - Math.sign(v) * 0.15) / 0.85);
/** a drag this near the window's edge locks the mouse (it turns on past the edge: the cursor stops there) */
const EDGE_PX = 24;

type Hint = [string, string];

const MARK_STORE = 'nfsu2-photo-mark';
/** F12 pictures carry the site's address in a corner (png-worker.ts), unless the panel's box turned it off */
export function photoMark(): boolean {
  try { return localStorage.getItem(MARK_STORE) !== 'off'; } catch { return true; }
}

export class PhotoMode {
  private root: HTMLElement;
  private flash: HTMLElement;
  private sl: Record<'zoom' | 'tilt' | 'speed' | 'smooth' | 'spin' | 'blur' | 'focus' | 'exposure' | 'vignette' | 'shiftX' | 'shiftY', HTMLInputElement>;
  private pk: Record<'target' | 'pathS' | 'shutter' | 'lens' | 'filter' | 'crop' | 'size' | 'weather', HTMLSelectElement>;
  private ck: Record<'level' | 'invert' | 'grid' | 'mark' | 'freeze' | 'lights' | 'names' | 'horns', HTMLInputElement>;
  private kinds: HTMLInputElement[];
  private shootBtn: HTMLButtonElement;
  private playBtn: HTMLButtonElement;
  private pathNote: HTMLElement;
  private hints: HTMLElement;
  private session: GameSession | null = null;
  private raf = 0;
  private hello = false;
  private can = false;
  private free = false;
  private namesHere = false;             // friends' names can show (free roam with them)
  private playersHere = false;           // other players' cars can be here (free roam with them)
  /** whether photo mode is on (the game's status) */
  on = false;
  private bare = false;                  // the panel hidden (H, the controller's View, a double tap)
  private keys = 0;
  private padBits = 0;
  private sent = { hold: -1, move: -1, lift: -1, turn: -1 };
  private sliding: HTMLInputElement | null = null;
  private drag: { id: number; x: number; y: number; pan: boolean; locked: boolean } | null = null;
  private fingers = new Map<number, [number, number]>();
  private pinch: { d: number; x: number; y: number } | null = null;
  private tap: { t: number; x: number; y: number; moved: boolean } | null = null;
  private lastTap = { t: 0, x: 0, y: 0 };
  private shooting = false;
  private flashTimer = 0;
  private kept = loadKept();
  private lookSent = '';
  private progress = -1;
  private path = -1;
  private target = -1;
  private pic = -1;                      // photo_pic: what the picture's options allow now (1 Size, 2 Shutter, 4 Fisheye,
                                         // 8 Lens shift; 16 a picture being taken)

  constructor(private stage: HTMLElement, private deps: {
    shoot(): Promise<void>; screenshot(): void; log(s: string): void;
  }) {
    const p = this.root = document.createElement('div');
    p.className = 'photo-panel'; p.hidden = true; p.dataset.nfsu2Ui = '';
    const slider = (cls: string, min: number, max: number, step = 1) =>
      `<label class="ph-slider"><span class="${cls}-l"></span><input type="range" class="${cls}" min="${min}" max="${max}" step="${step}"></label>`;
    const pick = (cls: string) => `<label class="ph-pick"><span class="${cls}-l"></span><select class="${cls}"></select></label>`;
    const check = (cls: string, extra = '') => `<label class="ph-check${extra}"><input type="checkbox" class="${cls}"><span class="${cls}-l"></span></label>`;
    const kinds = ['mine', 'friends', 'crews', 'players', 'traffic', 'racers'];
    p.innerHTML =
      '<b class="ph-title"></b>' +
      '<div class="ph-tabs" role="tablist">' + TABS.map(k => `<button type="button" class="ph-tab" role="tab" data-tab="${k}"></button>`).join('') + '</div>' +
      '<div class="ph-pane" data-pane="camera"><span class="ph-mode"><button type="button" data-mode="0"></button><button type="button" data-mode="1"></button></span>' +
        slider('ph-zoom', 0, 1000) + slider('ph-tilt', -450, 450, 5) + slider('ph-speed', 0, 1000, 10) + slider('ph-smooth', 0, 1000, 10) +
        slider('ph-spin', -600, 600, 10) + check('ph-level') + check('ph-invert') + pick('ph-target') +
        '<div class="ph-sec ph-path-l"></div>' +
        '<div class="ph-row3"><button type="button" data-act="point"></button><button type="button" data-act="clear"></button>' +
        '<button type="button" data-act="play"></button></div><div class="ph-note"></div>' + pick('ph-pathS') + '</div>' +
      '<div class="ph-pane" data-pane="picture">' +
        slider('ph-blur', 0, 1000) + slider('ph-focus', 0, 1000) + pick('ph-shutter') + pick('ph-lens') +
        slider('ph-shiftY', -500, 500, 5) + slider('ph-shiftX', -500, 500, 5) +
        slider('ph-exposure', -200, 200, 5) + slider('ph-vignette', 0, 1000) + pick('ph-filter') + pick('ph-crop') + pick('ph-size') +
        check('ph-grid') + check('ph-mark') + '</div>' +
      '<div class="ph-pane" data-pane="scene">' + check('ph-freeze') + '<div class="ph-sec ph-cars-l"></div><div class="ph-grid2">' +
        kinds.map(k => check(`ph-show-${k}`, k === 'friends' || k === 'crews' || k === 'players' ? ' ph-people' : '')).join('') + '</div>' +
        check('ph-lights') + check('ph-names', ' ph-names-c') + check('ph-horns') + pick('ph-weather') + '</div>' +
      '<div class="ph-pane ph-hints" data-pane="controls"></div>' +
      '<div class="ph-buttons"><button type="button" data-act="reset"></button>' +
      '<button type="button" class="ph-shoot" data-act="shoot"></button><button type="button" data-act="leave"></button></div>';
    const q = <E extends Element>(s: string) => p.querySelector<E>(s)!;
    this.sl = {
      zoom: q('.ph-zoom'), tilt: q('.ph-tilt'), speed: q('.ph-speed'), smooth: q('.ph-smooth'), spin: q('.ph-spin'),
      blur: q('.ph-blur'), focus: q('.ph-focus'), exposure: q('.ph-exposure'), vignette: q('.ph-vignette'),
      shiftX: q('.ph-shiftX'), shiftY: q('.ph-shiftY'),
    };
    this.pk = {
      target: q('.ph-target'), pathS: q('.ph-pathS'), shutter: q('.ph-shutter'), lens: q('.ph-lens'), filter: q('.ph-filter'),
      crop: q('.ph-crop'), size: q('.ph-size'), weather: q('.ph-weather'),
    };
    this.ck = {
      level: q('.ph-level'), invert: q('.ph-invert'), grid: q('.ph-grid'), mark: q('.ph-mark'), freeze: q('.ph-freeze'),
      lights: q('.ph-lights'), names: q('.ph-names'), horns: q('.ph-horns'),
    };
    this.kinds = kinds.map(k => q<HTMLInputElement>(`.ph-show-${k}`));
    this.shootBtn = q('.ph-shoot'); this.playBtn = q('[data-act="play"]'); this.pathNote = q('.ph-note'); this.hints = q('.ph-hints');
    const k = this.kept, s = this.sl, c = this.ck;
    c.mark.checked = photoMark();
    s.blur.value = String(k.blur); s.exposure.value = String(k.exposure); s.vignette.value = String(k.vignette);
    s.speed.value = String(k.speed); s.smooth.value = String(k.smooth); s.spin.value = String(k.spin);
    s.shiftX.value = String(k.shiftX); s.shiftY.value = String(k.shiftY);
    c.grid.checked = k.grid; c.freeze.checked = k.freeze; c.level.checked = k.level; c.invert.checked = k.invert;
    c.lights.checked = k.lights; c.horns.checked = k.horns;
    this.kinds.forEach((b, i) => { b.checked = (k.show & 1 << i) !== 0; });
    for (const v of Object.keys(CROPS)) this.pk.crop.add(new Option('', v));
    for (const v of Object.keys(FILTERS)) this.pk.filter.add(new Option('', v));
    for (const v of SHUTTERS) this.pk.shutter.add(new Option('', String(v)));
    for (const v of PATH_TIMES) this.pk.pathS.add(new Option('', String(v)));
    for (const v of [0, 1]) this.pk.lens.add(new Option('', String(v)));
    for (const v of [1, 2, 3, 4]) this.pk.size.add(new Option('', String(v)));
    for (const v of [-1, 0, 1]) this.pk.weather.add(new Option('', String(v)));
    this.pk.crop.value = k.crop; this.pk.filter.value = k.filter; this.pk.shutter.value = String(k.shutter);
    this.pk.pathS.value = String(k.pathS); this.pk.lens.value = String(k.lens); this.pk.size.value = String(k.size);
    this.pk.weather.value = String(k.weather);                     // (the camera's car: fill(), by where it is)
    p.addEventListener('click', e => {
      const b = (e.target as HTMLElement).closest<HTMLButtonElement>('button');
      if (!b) return;
      if (b.dataset.mode) this.send(PH_MODE, +b.dataset.mode);
      else if (b.dataset.tab) this.setTab(b.dataset.tab as Tab);
      else if (b.dataset.act === 'reset') this.send(PH_RESET);
      else if (b.dataset.act === 'shoot') void this.shoot();
      else if (b.dataset.act === 'leave') this.leave();
      else if (b.dataset.act === 'point') this.send(PH_PATH, PATH_ADD);
      else if (b.dataset.act === 'play') this.playPath();
      else if (b.dataset.act === 'clear') this.send(PH_PATH, PATH_CLEAR);
      back();
    });
    // the buttons leave the keyboard with the game's canvas; the sliders and lists give it back when let go
    const back = () => this.canvas()?.focus({ preventScroll: true });
    p.addEventListener('pointerdown', e => { if ((e.target as HTMLElement).closest('button')) e.preventDefault(); });
    for (const x of Object.values(s)) {
      x.addEventListener('pointerdown', () => { this.sliding = x; });
      x.addEventListener('input', () => this.slid(x));
      x.addEventListener('change', () => { this.sliding = null; back(); });
    }
    // a double click puts a slider back where it starts
    const home: [HTMLInputElement, number][] = [[s.tilt, 0], [s.exposure, 0], [s.speed, 500], [s.spin, 0], [s.shiftX, 0], [s.shiftY, 0]];
    for (const [x, v] of home) x.addEventListener('dblclick', () => { x.value = String(v); this.slid(x); });
    s.focus.addEventListener('dblclick', () => this.send(PH_FOCUS, -1));          // the car again
    for (const x of Object.values(this.pk)) x.addEventListener('change', () => { this.picked(x); back(); });
    for (const x of Object.values(c)) x.addEventListener('change', () => { this.checked(x); back(); });
    for (const x of this.kinds) x.addEventListener('change', () => { this.sendShow(); back(); });
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
    this.setTab(k.tab, false);
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
    const b = s.buttons, a = s.axes, on = (n: number) => (b & 1 << n) !== 0, inv = this.kept.invert ? -1 : 1;
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
      pack(dz(a[PAD_AXIS_RX]), inv * dz(a[PAD_AXIS_RY])));
    return true;
  }

  private input() { return this.session?.input ?? null; }
  private canvas() { return this.stage.querySelector('canvas'); }
  private send(what: number, a = 0, b = 0): boolean { return !!this.input()?.photo?.(what, a, b); }
  private leave() { if (this.send(PH_OPEN, 0)) this.deps.log('[app] photo mode: leave'); }
  private keep() { try { localStorage.setItem(KEPT_STORE, JSON.stringify(this.kept)); } catch { /* not kept: a private window */ } }

  /** the tab shown (kept for the next time) */
  private setTab(tab: Tab, keep = true) {
    for (const b of this.root.querySelectorAll<HTMLElement>('[data-tab]')) b.setAttribute('aria-selected', String(b.dataset.tab === tab));
    for (const x of this.root.querySelectorAll<HTMLElement>('[data-pane]')) x.hidden = x.dataset.pane !== tab;
    if (keep && tab !== this.kept.tab) { this.kept.tab = tab; this.keep(); }
  }

  /** the kinds of cars shown, from their boxes (the player kinds' only where there are players: else as they were) */
  private sendShow() {
    let m = 0;
    this.kinds.forEach((b, i) => { if (b.checked) m |= 1 << i; });
    this.kept.show = m; this.keep();
    this.send(PH_SHOW, m);
  }

  /** the camera's settings to the game (photo mode came on: all of them) */
  private sendCamera() {
    const k = this.kept;
    this.send(PH_SPEED, Math.round(speedOf(k.speed) * 100)); this.send(PH_SMOOTH, k.smooth);
    this.send(PH_SPIN, Math.abs(k.spin) < 20 ? 0 : k.spin); this.send(PH_LEVEL, k.level ? 1 : 0);
  }
  private sendScene() {
    const k = this.kept;
    this.send(PH_FREEZE, k.freeze ? 1 : 0); this.send(PH_SHOW, k.show); this.send(PH_LIGHTS, k.lights ? 1 : 0);
    this.send(PH_HORNS, k.horns ? 1 : 0); this.send(PH_WEATHER, k.weather);
  }
  private sendPicture() {
    const k = this.kept;
    this.send(PH_BLUR, k.blur); this.send(PH_SHUTTER, k.shutter); this.send(PH_SIZE, k.size);
    this.send(PH_SHIFT, k.shiftX, k.shiftY); this.send(PH_LENS, k.lens);
  }

  /** what needs time to stand (the picture's frames add up) and what goes only with something else (the game says:
   *  photo_pic) */
  private greyed() {
    const k = this.kept, pic = this.pic < 0 ? 15 : this.pic;
    this.sl.blur.disabled = !k.freeze; this.sl.focus.disabled = !k.freeze || k.blur <= 0;
    this.pk.size.disabled = !(pic & 1); this.pk.shutter.disabled = !(pic & 2) || !k.freeze; this.pk.lens.disabled = !(pic & 4);
    this.sl.shiftX.disabled = this.sl.shiftY.disabled = !(pic & 8);
    this.ck.level.disabled = !this.free;
  }

  /** the path: plays, or stops if it plays */
  private playPath() {
    if (this.path > 0 && this.path & 16) this.send(PH_PATH, PATH_STOP);
    else this.send(PH_PATH, PATH_PLAY, this.kept.pathS * 1000);
  }

  /** a slider moved */
  private slid(x: HTMLInputElement) {
    const v = +x.value, s = this.sl, k = this.kept;
    if (x === s.zoom) this.send(PH_FOV, Math.round((FOV_MAX - (v / 1000) * (FOV_MAX - FOV_MIN)) * 100));
    else if (x === s.tilt) this.send(PH_ROLL, Math.round(v * 10));
    else if (x === s.focus) this.send(PH_FOCUS, Math.round(focusM(v) * 100));
    else {
      if (x === s.blur) { k.blur = v; this.send(PH_BLUR, v); }
      else if (x === s.exposure) k.exposure = v;
      else if (x === s.vignette) k.vignette = v;
      else if (x === s.speed) k.speed = v;
      else if (x === s.smooth) k.smooth = v;
      else if (x === s.spin) k.spin = v;
      else if (x === s.shiftX) { k.shiftX = v; this.send(PH_SHIFT, k.shiftX, k.shiftY); }
      else if (x === s.shiftY) { k.shiftY = v; this.send(PH_SHIFT, k.shiftX, k.shiftY); }
      if (x === s.speed || x === s.smooth || x === s.spin) this.sendCamera();
      this.greyed(); this.keep();
    }
  }

  /** a list's choice */
  private picked(x: HTMLSelectElement) {
    const k = this.kept, p = this.pk, v = x.value;
    if (x === p.target) { this.send(PH_TARGET, v === 'mine' ? TARGET_MINE : TARGET_MIDDLE); this.target = -1; return; }
    if (x === p.filter) k.filter = v as Filter;
    else if (x === p.crop) k.crop = v;
    else if (x === p.pathS) k.pathS = +v;
    else if (x === p.shutter) { k.shutter = +v; this.send(PH_SHUTTER, k.shutter); }
    else if (x === p.lens) { k.lens = +v; this.send(PH_LENS, k.lens); }
    else if (x === p.size) { k.size = +v; this.send(PH_SIZE, k.size); }
    else if (x === p.weather) { k.weather = +v; this.send(PH_WEATHER, k.weather); }
    this.keep();
  }

  /** a box ticked or not */
  private checked(x: HTMLInputElement) {
    const k = this.kept, c = this.ck, on = x.checked;
    if (x === c.names) { this.send(PH_NAMES, on ? 1 : 0); return; }
    if (x === c.mark) {
      try { if (on) localStorage.removeItem(MARK_STORE); else localStorage.setItem(MARK_STORE, 'off'); } catch { /* not kept: a private window */ }
      this.deps.log(`[app] photo mode: the site's address on pictures ${on ? 'on' : 'off'}`);
      return;
    }
    if (x === c.grid) k.grid = on;
    else if (x === c.level) { k.level = on; this.send(PH_LEVEL, on ? 1 : 0); }
    else if (x === c.invert) k.invert = on;
    else if (x === c.freeze) { k.freeze = on; this.send(PH_FREEZE, on ? 1 : 0); }
    else if (x === c.lights) { k.lights = on; this.send(PH_LIGHTS, on ? 1 : 0); }
    else if (x === c.horns) { k.horns = on; this.send(PH_HORNS, on ? 1 : 0); }
    this.greyed(); this.keep();
  }

  /** the picture's look and crop to the renderer, when they change */
  private sendLook() {
    const r = this.session?.renderer;
    if (!r?.photoLook) return;
    const k = this.kept, rc = r.imageRect(), f = FILTERS[k.filter];
    const look: PhotoLook = { ...f, exposure: 2 ** (k.exposure / 100), vignette: k.vignette / 1000 * 0.8,
      crop: cropRect(CROPS[k.crop], rc.w / Math.max(1, rc.h)), grid: k.grid };
    const key = JSON.stringify(look);
    if (key === this.lookSent) return;
    this.lookSent = key;
    r.photoLook(look);
  }

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
    this.keys = 0; this.padBits = 0; this.endDrag(); this.fingers.clear(); this.pinch = null; this.tap = null;
    this.steer(0, 0, 0);
  }

  /** the picture: the flash once it is kept (a big one or the shutter's takes a few seconds; the button fills meanwhile) */
  private async shoot() {
    if (this.shooting || !this.on) return;
    this.shooting = true;
    try { await this.deps.shoot(); } finally { this.shooting = false; }
    const f = this.flash;
    f.hidden = true; void f.offsetWidth; f.hidden = !this.on;
    clearTimeout(this.flashTimer);
    this.flashTimer = setTimeout(() => { f.hidden = true; }, 400);
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
    const free = (bits & 4) !== 0;
    if (free !== this.free) { this.free = free; this.greyed(); }
    for (const b of this.root.querySelectorAll<HTMLButtonElement>('[data-mode]')) b.classList.toggle('on', (b.dataset.mode === '1') === this.free);
    if (((bits & 16) !== 0) !== this.namesHere || ((bits & 512) !== 0) !== this.playersHere) {
      this.namesHere = (bits & 16) !== 0; this.playersHere = (bits & 512) !== 0; this.fill();
    }
    this.ck.names.checked = (bits & 8) !== 0;
    const fov = st.get('photo_fov') ?? 70, roll = st.get('photo_roll') ?? 0, focus = st.get('photo_focus') ?? 10;
    if (this.sliding !== this.sl.zoom) this.sl.zoom.value = String(Math.round((FOV_MAX - fov) / (FOV_MAX - FOV_MIN) * 1000));
    if (this.sliding !== this.sl.tilt) this.sl.tilt.value = String(Math.round(roll * 10));
    if (this.sliding !== this.sl.focus) this.sl.focus.value = String(focusV(focus));
    const path = st.get('photo_path') ?? 0, target = st.get('photo_target') ?? 0, pic = st.get('photo_pic') ?? -1;
    if (pic !== this.pic) { this.pic = pic; this.greyed(); }
    if (path !== this.path) { this.path = path; this.fillPath(); }
    if (target !== this.target) { this.target = target; this.fillTarget(); }
    this.sendLook();
    // the picture's frames so far: the Take photo button fills as they come
    const pr = s.renderer?.photoProgress?.(), p = pr && pr.target > 0 ? Math.min(1, pr.n / pr.target) : 0;
    if (p !== this.progress) { this.progress = p; this.shootBtn.style.setProperty('--ph-p', String(p)); }
  }

  /** photo mode came on or went off in the game */
  private show(on: boolean) {
    if (on === this.on) return;
    this.on = on;
    this.keys = 0; this.padBits = 0; this.endDrag(); this.fingers.clear(); this.pinch = null; this.tap = null; this.sliding = null;
    this.sent = { hold: -1, move: -1, lift: -1, turn: -1 };
    this.stage.classList.toggle('photo', on);
    if (on) {
      this.input()?.releaseAll?.();                 // what the game held when the page took the keys goes up
      this.steer(0, 0, 0);
      this.sendCamera(); this.sendScene(); this.sendPicture();
      this.path = -1; this.target = -1; this.pic = -1;
      this.greyed();
      this.lookSent = ''; this.sendLook();
      this.fill();
      this.setBare(false);
      const k = this.kept;
      this.deps.log(`[app] photo mode: on (blur ${k.blur / 10} %, ${k.filter}, crop ${k.crop}, cars ${k.show}, speed ${speedOf(k.speed).toFixed(2)}, ` +
        `smoothing ${k.smooth / 10} %, turn ${k.spin / 10}, weather ${k.weather}, size ${k.size}, shutter ${k.shutter})`);
    } else {
      this.root.hidden = true; this.flash.hidden = true;
      this.deps.log('[app] photo mode: off');
    }
  }

  private setBare(bare: boolean) {
    this.bare = bare;
    this.root.hidden = !this.on || bare;
    if (!this.root.hidden) this.fitTabs();
  }

  /** the tabs on one line, smaller where the language's words do not fit, else in two rows (as tabs.ts does) */
  private fitTabs() {
    const l = this.root.querySelector<HTMLElement>('.ph-tabs')!;
    for (const fit of ['', 'small', 'rows']) {
      if (fit) l.dataset.fit = fit; else delete l.dataset.fit;
      if (fit === 'rows' || l.scrollWidth <= l.clientWidth) break;
    }
  }

  /** the path's points and its Play button */
  private fillPath() {
    const n = Math.max(0, this.path) & 15, playing = this.path > 0 && (this.path & 16) !== 0;
    this.pathNote.textContent = n ? tn(n, '{n} point', '{n} points') : t('Add two points or more');
    this.playBtn.textContent = playing ? t('Stop') : t('Play path');
    this.playBtn.disabled = !playing && n < 2;
    (this.root.querySelector('[data-act="point"]') as HTMLButtonElement).disabled = playing || n >= 8;
    (this.root.querySelector('[data-act="clear"]') as HTMLButtonElement).disabled = !n;
  }

  /** the car the camera is on: the list shows its kind, and offers the player's and the car in the middle */
  private fillTarget() {
    const kind = [t('Your car'), t("A friend's car"), t("A crew-mate's car"), t("A player's car"), t('A traffic car'), t("A racer's car")];
    const k = Math.max(0, this.target), o = (v: string, s: string) => new Option(s, v);
    this.pk.target.replaceChildren(o('mine', kind[0]), ...(k ? [o('on', kind[k] ?? kind[4])] : []), o('middle', t('The car in the middle')));
    this.pk.target.value = k ? 'on' : 'mine';
  }

  /** the panel's texts in the page's language, the hints for the device in use */
  private fill() {
    const q = (s: string) => this.root.querySelector(s)!;
    const text: [string, string][] = [
      ['.ph-title', N('Photo mode')], ['[data-tab="camera"]', N('Camera')], ['[data-tab="picture"]', N('Picture')],
      ['[data-tab="scene"]', N('Scene')], ['[data-tab="controls"]', N('Controls')],
      ['[data-mode="0"]', N('Orbit')], ['[data-mode="1"]', N('Free')], ['.ph-zoom-l', N('Zoom')], ['.ph-tilt-l', N('Tilt')],
      ['.ph-speed-l', N('Speed')], ['.ph-smooth-l', N('Smoothing')], ['.ph-spin-l', N('Auto-turn')], ['.ph-level-l', N('Keep height')],
      ['.ph-invert-l', N('Invert up and down')], ['.ph-target-l', N('Camera on')], ['.ph-path-l', N('Path')],
      ['[data-act="point"]', N('Add point')], ['[data-act="clear"]', N('Clear')], ['.ph-pathS-l', N('Path time')],
      ['.ph-blur-l', N('Blur')], ['.ph-focus-l', N('Focus')], ['.ph-shutter-l', N('Shutter')], ['.ph-lens-l', N('Lens')],
      ['.ph-shiftY-l', N('Shift up, down')], ['.ph-shiftX-l', N('Shift sideways')],
      ['.ph-exposure-l', N('Brightness')], ['.ph-vignette-l', N('Vignette')], ['.ph-filter-l', N('Filter')], ['.ph-crop-l', N('Crop')],
      ['.ph-size-l', N('Size')], ['.ph-grid-l', N('Grid')], ['.ph-mark-l', N('nfsu2.xyz on pictures')],
      ['.ph-freeze-l', N('Freeze time')], ['.ph-cars-l', N('Cars')], ['.ph-show-mine-l', N('Your car')], ['.ph-show-friends-l', N('Friends')],
      ['.ph-show-crews-l', N('Crews')], ['.ph-show-players-l', N('Players')], ['.ph-show-traffic-l', N('Traffic')],
      ['.ph-show-racers-l', N('Racers')], ['.ph-lights-l', N("Other cars' headlights")], ['.ph-names-l', N('Names')],
      ['.ph-horns-l', N('Horns')], ['.ph-weather-l', N('Weather')],
      ['[data-act="reset"]', N('Reset')], ['[data-act="shoot"]', N('Take photo')], ['[data-act="leave"]', N('Leave')],
    ];
    for (const [s, en] of text) q(s).textContent = t(en);
    // the players' kinds only in free roam with them, their names where they can show
    for (const x of this.root.querySelectorAll<HTMLElement>('.ph-people')) x.hidden = !this.playersHere;
    (q('.ph-names-c') as HTMLElement).hidden = !this.namesHere;
    const filterName: Record<Filter, string> = { none: t('Original'), bw: t('Black and white'), warm: t('Warm'), cool: t('Cool'),
      vivid: t('Vivid'), faded: t('Faded') };
    for (const o of this.pk.filter.options) o.text = filterName[o.value as Filter];
    for (const o of this.pk.crop.options) o.text = o.value === 'full' ? t('Whole picture') : o.value;
    for (const o of this.pk.shutter.options) o.text = o.value === '0' ? t('Off') : o.value === '1000' ? '1 s' : `1/${Math.round(1000 / +o.value)} s`;
    for (const o of this.pk.pathS.options) o.text = `${o.value} s`;
    for (const o of this.pk.lens.options) o.text = o.value === '1' ? t('Fisheye') : t('Normal');
    for (const o of this.pk.size.options) o.text = o.value === '1' ? t('Screen') : `${o.value}×`;
    for (const o of this.pk.weather.options) o.text = o.value === '1' ? t('Rain') : o.value === '0' ? t('Dry') : t('As it is');
    this.sl.focus.title = t('Double-click: focus on your car');
    this.fillPath(); this.fillTarget();
    if (!this.root.hidden) this.fitTabs();
    const names = (k: string): Hint[] => (this.namesHere ? [[k, N('Names')]] : []);
    const list: Hint[] = touchFirst() && !padSeen()
      ? [[t('Drag'), N('Turn')], [t('Two fingers'), N('Move')], [t('Pinch'), N('Closer, farther')], [t('Double tap'), N('Hide the panel')]]
      : padSeen()
        ? [['R', N('Turn')], ['L', N('Move')], ['LB RB', N('Down, up')], ['LT RT', N('Closer, farther')], ['L3', N('Faster')],
           ['↑ ↓', N('Zoom')], ['← →', N('Tilt')], ['Y', N('Orbit or free')], ['X', N('Reset')], ...names('RS'),
           ['View', N('Hide the panel')], ['A', N('Take photo')], ['B', N('Leave')]]
        : [[t('Drag'), N('Turn')], ['W A S D', N('Move')], ['Q E', N('Down, up')], [t('Wheel'), N('Closer, farther')],
           ['Shift', N('Faster')], ['Z X', N('Zoom')], ['C V', N('Tilt')], ['Tab', N('Orbit or free')],
           ['F', N('Camera on the car in the middle')], ['K', N('Add a path point')], ['P', N('Play the path')],
           ['R', N('Reset')], ...names('N'), ['H', N('Hide the panel')], [keyName(siteKey('photo') ?? 'Enter'), N('Take photo')],
           ['Esc', N('Leave')]];
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
    if (!this.on || pageKey(e.code) || pageDialog()) return;
    e.stopImmediatePropagation();                   // the game gets none of them
    if (e.metaKey || e.ctrlKey) return;             // the browser's shortcuts stay the browser's
    e.preventDefault();
    if (bit) { if (down) { this.keys |= bit; this.steer(); } return; }
    if (!down || e.repeat) return;
    if (e.code === 'Escape') this.leave();
    else if (e.code === 'Enter' || e.code === 'Space' || e.code === 'NumpadEnter') void this.shoot();
    else if (e.code === 'Tab') this.send(PH_MODE, 2);
    else if (e.code === 'KeyR') this.send(PH_RESET);
    else if (e.code === 'KeyF') this.send(PH_TARGET, TARGET_MIDDLE);
    else if (e.code === 'KeyK') this.send(PH_PATH, PATH_ADD);
    else if (e.code === 'KeyP') this.playPath();
    else if (e.code === 'KeyN' && this.namesHere) this.send(PH_NAMES, 2);
    else if (e.code === 'KeyH') this.setBare(!this.bare);
  }

  /** an event on the game's picture (not on the panel or another control of the page's) while photo mode is on */
  private mine(e: Event): boolean {
    const el = e.target as HTMLElement | null;
    return this.on && !!el && this.stage.contains(el) && !el.closest('[data-nfsu2-ui]') && !pageDialog();
  }

  /** a mouse drag turns the camera (the left button) or moves it (another); near the window's edge it locks the mouse,
   *  so that it turns on past the edge, where the cursor would stop (the browser may say how to get the cursor back) */
  private onPointer(e: PointerEvent) {
    if (e.type === 'pointerdown' ? !this.mine(e) : !this.on || !(this.drag?.id === e.pointerId || this.fingers.has(e.pointerId))) return;
    e.stopImmediatePropagation(); e.preventDefault();
    const h = Math.max(1, this.session?.viewRect().h ?? 1);
    if (e.pointerType === 'touch') { this.onFinger(e, h); return; }
    if (e.type === 'pointerdown') {
      if (this.drag) return;
      this.drag = { id: e.pointerId, x: e.clientX, y: e.clientY, pan: e.button !== 0, locked: false };
      try { (e.target as Element).setPointerCapture(e.pointerId); } catch { /* a synthetic pointer */ }
    } else if (e.type === 'pointermove' && this.drag) {
      // the cursor's move (locked: the mouse's own, the cursor standing still)
      const locked = !!document.pointerLockElement, mx = locked ? e.movementX : e.clientX - this.drag.x, my = locked ? e.movementY : e.clientY - this.drag.y;
      this.drag.x = e.clientX; this.drag.y = e.clientY;
      const inv = this.drag.pan || !this.kept.invert ? 1 : -1;
      const dx = Math.round(mx / h * 10000), dy = Math.round(inv * my / h * 10000);
      if (dx || dy) this.send(this.drag.pan ? PH_PAN : PH_DRAG, dx, dy);
      const c = this.canvas();
      if (!this.drag.locked && c && (e.clientX < EDGE_PX || e.clientY < EDGE_PX || e.clientX > innerWidth - EDGE_PX || e.clientY > innerHeight - EDGE_PX)) {
        this.drag.locked = true;
        try { void Promise.resolve(c.requestPointerLock()).catch(() => { /* refused: the drag goes on to the edge */ }); } catch { /* none */ }
      }
    } else if (e.type === 'pointerup' || e.type === 'pointercancel') this.endDrag();
  }
  private endDrag() {
    if (this.drag?.locked && document.pointerLockElement) document.exitPointerLock();
    this.drag = null;
  }

  /** one finger turns the camera; two move it (their middle) and bring it closer or farther (their spread); a double
   *  tap hides the panel or shows it again */
  private onFinger(e: PointerEvent, h: number) {
    if (e.type === 'pointerdown') {
      this.fingers.set(e.pointerId, [e.clientX, e.clientY]);
      this.tap = this.fingers.size === 1 ? { t: e.timeStamp, x: e.clientX, y: e.clientY, moved: false } : null;
    } else if (e.type === 'pointerup' || e.type === 'pointercancel') {
      this.fingers.delete(e.pointerId);
      const tp = this.tap;
      if (e.type === 'pointerup' && tp && !tp.moved && e.timeStamp - tp.t < 250) {
        const l = this.lastTap;
        if (e.timeStamp - l.t < 350 && Math.hypot(e.clientX - l.x, e.clientY - l.y) < 40) { this.setBare(!this.bare); this.lastTap.t = 0; }
        else this.lastTap = { t: e.timeStamp, x: e.clientX, y: e.clientY };
      }
      this.tap = null;
    }
    const was = this.fingers.get(e.pointerId);
    if (e.type === 'pointermove' && was) {
      if (this.tap && Math.hypot(e.clientX - this.tap.x, e.clientY - this.tap.y) > 8) this.tap.moved = true;
      if (this.fingers.size === 1) {
        const inv = this.kept.invert ? -1 : 1;
        const dx = Math.round((e.clientX - was[0]) / h * 10000), dy = Math.round(inv * (e.clientY - was[1]) / h * 10000);
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
