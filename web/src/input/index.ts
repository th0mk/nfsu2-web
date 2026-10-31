// Keyboard, mouse, game pads and the app lifecycle for the game, through the
// HostMailbox in shared memory (runtime/web/input_proto.h, input_web.c).
//
// Keys:  KeyboardEvent.code gives the key's position (DirectInput scan code,
//        layout independent) and its Windows virtual-key code; e.key gives
//        the characters the layout typed (WM_CHAR, cp1252).  The C side
//        applies keyboard_mac.m's delivery rules (Alt and SYS messages,
//        autorepeat, keys whose release was lost).  Every key the game uses
//        has its browser default prevented; Meta combinations stay the
//        browser's, and Meta going down releases every held key because
//        macOS browsers send no keyup for keys released while Cmd is held.
// Mouse: positions relative to the shown image (the C side maps them into
//        the game's client area and HUD), buttons with pointer capture, the
//        wheel in WHEEL_DELTA units.  The OS cursor is hidden over the game.
// Touch: one finger is the mouse, so a tap is a left click (the game's menus
//        take clicks); two are a middle-button drag (the garage turns the
//        car); other fingers are left alone.  Where the first finger lands
//        is marked (IKF_TOUCH): the C side makes the cursor arrive with a
//        move the menus see, also on the spot it already is.
// Text:  the game's text entry while it is up (a seqlock the game writes at
//        each Present), and characters typed with no key (a phone's
//        keyboard, app/textentry.ts) as WM_CHAR.
// Pads:  polled every frame (gamepad.ts), with the page's share (padPage) and
//        the touch controls' virtual controller merged in (touchPad).
// Life:  blur and a hidden page deactivate the game (it parks, as after an
//        alt-tab on Windows) and release everything held; focus and a visible
//        page reactivate it; requestQuit() is WM_CLOSE.  Fullscreen locks the
//        keyboard (navigator.keyboard.lock) so Escape and the browser's
//        reserved shortcuts reach the game; holding Escape leaves fullscreen.
// Use:   lastInput() is when the player last pressed a key, moved or clicked
//        the mouse anywhere on the page, pressed a controller button or moved
//        a stick or trigger (the app's away check, away.ts).
// Test:  the autotest's ns* requests become DOM events (synth.ts).
import type { GameProcess } from '../host/process';
import { HostKind } from '../host/abi';
import { CODES, cp1252, isMeta, keyChars, keyVk } from './keymap';
import { PadPoller, type PadPage, type TouchPad } from './gamepad';
import { SynthDriver } from './synth';
import {
  IEV_FOCUS, IEV_KEY_DOWN, IEV_KEY_TAP, IEV_KEY_UP, IEV_LOG, IEV_MOUSE_DOWN, IEV_MOUSE_MOVE, IEV_MOUSE_UP, IEV_QUIT, IEV_SMS,
  IEV_PHOTO, IEV_RELEASE_ALL, IEV_TEXT, IEV_WHEEL, IKF_ALT, IKF_CTRL, IKF_DEAD, IKF_REPEAT, IKF_SHIFT, IKF_SYNTH, IKF_TOUCH, IM_ATTACHED,
  IM_EV_DROP, IM_EV_R, IM_EV_W, IM_EVENTS, IM_GAME_INACTIVE, IM_HDR_WORDS, IM_KEYS_HELD, IM_MAGIC, IM_MOUSE_X,
  IM_MOUSE_Y, IM_NAMES_GEN, IM_OFF_EV, IM_OFF_NAMES, IM_OFF_SYN, IM_QUIT_REQ, IM_SYN_R, IM_SYN_W, IM_TEXT,
  IM_TEXT_KIND, IM_TEXT_LEN, IM_TEXT_MAX, IM_TEXT_SEQ, IM_MENU_SCALE, IM_VERSION, IM_VIEW_ROLL, INPUT_EV_N, INPUT_MAGIC, INPUT_NAME_LEN,
  INPUT_SYN_N, INPUT_TEXT_BYTES, INPUT_VERSION, IREASON_ELEMENT, IREASON_FULLSCREEN, IREASON_META, IREASON_PAGE,
  IREASON_PAGEHIDE, IREASON_SCRIPT, IREASON_VISIBILITY, IREASON_WINDOW,
} from './proto';

export interface ViewRect { x: number; y: number; w: number; h: number }
export interface InputOptions {
  /** the game image inside the element, CSS px relative to its border box (default: all of it) */
  viewRect?: () => ViewRect;
  hideCursor?: boolean;              // default true
  gamepads?: boolean;                // default true
  /** where key events are listened for (default window, capture phase) */
  keyboardTarget?: EventTarget;
  /** the platform's conventions (Cmd/Option, Caps Lock events); default: detected */
  mac?: boolean;
  /** events the page's own UI should keep (default: editable fields, [data-nfsu2-ui], and
   *  anything while a modal dialog is open or aimed inside an open dialog) */
  ignore?: (e: Event) => boolean;
  /** what Cmd-F (macOS) puts in full screen (default: the element) */
  toggleFullscreen?: () => void;
  /** keys whose browser default is kept (default F11, F12) */
  passThrough?: string[];
  /** the page's own use of the controller: its menu and dialogs (gamepad.ts) */
  padPage?: PadPage;
  log?: (s: string) => void;
}
export interface InputStats {
  attached: boolean; events: number; dropped: number; keysHeld: number; gameInactive: boolean;
  quitRequested: boolean; cursor: [number, number]; pad: string | null;
}
/** the numeric counters (the app's overlay contract: names to numbers) */
export interface InputCounters { [k: string]: number; 'input events': number; 'keys held': number; 'pad connected': number }
/** the game's text entry (runtime/web/input_web.c): the most characters it takes, its kind (which characters
 *  besides letters and digits), its text now */
export interface TextEntry { max: number; kind: number; text: string }
export interface InputHandle {
  /** WM_CLOSE to the game.  Before the game's mailbox exists (the first ~0.2 s
   *  after the start) the request waits and is sent when it registers.  false:
   *  it cannot reach the game (detached, or the event ring is not the page's). */
  requestQuit(): boolean;
  /** whether requestQuit() can still reach the game */
  canQuit(): boolean;
  /** Download log: the game writes how long ago it drew its last frame, after a while what every thread is doing,
   *  then "[watchdog] log download ready" (runtime/watchdog.c).  false: it cannot reach the game */
  logReport(): boolean;
  /** a text message of ours for the game's phone (runtime/sms.c), by its number.  false: it cannot reach the game */
  sms(id: number): boolean;
  /** photo mode (runtime/photo.c): what (its PH_* number) and its values.  false: it cannot reach the game */
  photo(what: number, a?: number, b?: number): boolean;
  enterFullscreen(): Promise<boolean>;
  exitFullscreen(): Promise<void>;
  toggleFullscreen(): Promise<void>;
  releaseAll(): void;
  /** everything, for test pages and logs */
  status(): InputStats;
  /** numbers only, for the app's detailed overlay */
  stats(): InputCounters;
  /** performance.now() of the player's last key, mouse or controller input (the attach before any); null
   *  while the game's own driver writes the event ring (an input script without the page's input) */
  lastInput(): number | null;
  /** a key the page presses or lets go for the player (the touch controls), by KeyboardEvent.code; Enter and Escape
   *  also type their characters, as a keyboard's do */
  key(code: string, down: boolean): void;
  /** the touch controls' virtual controller, merged with a real one (null: none) */
  touchPad(t: TouchPad | null): void;
  /** the touch controls' level horizon: the phone's tilt the game's 3D view turns against, radians, + = clockwise */
  viewRoll(rad: number): void;
  /** the menus' small buttons' size, 1 = the game's own (touch.ts: the Menu button size): the scroller arrows', and
   *  with bar the help bar's too (else it keeps the game's own) */
  menuScale(s: number, bar: boolean): void;
  /** performance.now() of a real controller's last button press (0: none) */
  padPressed(): number;
  /** the game's text entry while it is up, else null */
  textEntry(): TextEntry | null;
  /** characters typed with no key (a phone's keyboard), cp1252: WM_CHAR each, 8 deletes, 13 accepts */
  text(chars: number[]): void;
  /** diagnostic: n empty events, each timed from the page's write until the
   *  input thread has handled it (busy-waits on the main thread, max 20 ms each);
   *  returns the times in microseconds */
  probeLatency(n?: number): number[];
  detach(): void;
}

/** what counts as the player's use of keyboard and mouse (lastInput) */
const USE_EVENTS = ['keydown', 'pointerdown', 'pointermove', 'wheel'];

/** the characters a key the page presses types, as a keyboard's does: the game's text fields and some of its prompts
 *  take Enter and Escape only as characters (runtime/dinput_pad.c fe_keys) */
const PAGE_KEY_CHARS: Record<string, number[]> = { Enter: [13], Escape: [27] };

const detectMac = () => {
  const p = (navigator as unknown as { userAgentData?: { platform?: string } }).userAgentData?.platform ?? navigator.platform ?? '';
  return /mac/i.test(p);
};
interface KeyboardApi { lock?(codes?: string[]): Promise<void>; unlock?(): void; getLayoutMap?(): Promise<Map<string, string>>;
  addEventListener?(t: string, f: () => void): void }
const keyboardApi = () => (navigator as unknown as { keyboard?: KeyboardApi }).keyboard;

export function attachInput(proc: GameProcess, element: HTMLElement, opts: InputOptions = {}): InputHandle {
  const log = opts.log ?? ((s: string) => console.log(s));
  const mac = opts.mac ?? detectMac();
  const kbTarget = opts.keyboardTarget ?? window;
  const pass = new Set(opts.passThrough ?? ['F11', 'F12']);
  let h: Int32Array | null = null, u8: Uint8Array | null = null, i32: Int32Array | null = null, base = 0;
  let layout: Map<string, string> | null = null;
  let active: boolean | null = null;
  let pads: PadPoller | null = null, synth: SynthDriver | null = null;
  let raf = 0, timer = 0, detached = false;
  const f32 = new Float32Array(1), f32Bits = new Int32Array(f32.buffer);   // a float's bits for a mailbox word
  let menu = 0;                     // the menus' size asked for (0: none; negative: the arrows' alone), written once the mailbox is there
  const putMenu = () => { if (h && !detached && menu) { f32[0] = menu; Atomics.store(h, IM_MENU_SCALE, f32Bits[0]); } };
  // the mailbox was not ours (another writer, another layout): requests cannot reach the game
  let refused = false;
  // quit requests made before the mailbox registered: sent once it does
  let quitPending = 0;
  const dropped = { n: 0 };
  // the game's text entry: the last consistent copy (kept while the game is writing it)
  let textNow: TextEntry | null = null;
  const win1252 = new TextDecoder('windows-1252');
  let lastUse = performance.now();
  const onUse = () => { lastUse = performance.now(); };

  // ---- the event ring -------------------------------------------------------
  function push(type: number, dik = 0, vk = 0, flags = 0, chars: number[] = [], a = 0, b = 0) {
    if (!h || !u8 || !i32) return;
    const w = Atomics.load(h, IM_EV_W), r = Atomics.load(h, IM_EV_R);
    if (((w - r) >>> 0) >= INPUT_EV_N) { Atomics.add(h, IM_EV_DROP, 1); dropped.n++; return; }
    const o = base + IM_OFF_EV + (w & (INPUT_EV_N - 1)) * 16;
    u8[o] = type; u8[o + 1] = dik; u8[o + 2] = vk; u8[o + 3] = flags;
    u8[o + 4] = Math.min(chars.length, 3); u8[o + 5] = chars[0] ?? 0; u8[o + 6] = chars[1] ?? 0; u8[o + 7] = chars[2] ?? 0;
    i32[(o + 8) >> 2] = a | 0; i32[(o + 12) >> 2] = b | 0;
    Atomics.store(h, IM_EV_W, (w + 1) | 0);       // publishes the record (a sequentially consistent store)
    Atomics.notify(h, IM_EV_W, 1);
  }
  const setActive = (on: boolean, reason: number) => {
    if (active === on) return;
    active = on;
    push(IEV_FOCUS, 0, 0, 0, [], on ? 1 : 0, reason);
  };

  // ---- keys ------------------------------------------------------------------
  const ignored = (e: Event) => {
    if (opts.ignore) return opts.ignore(e);
    const t = e.target as HTMLElement | null;
    // a modal dialog (the page's menu, a confirmation) has the keyboard: Tab,
    // Enter, Space and Escape must work its buttons, not drive the game
    if (document.querySelector('dialog[open]:modal')) return true;
    if (!t || !(t instanceof HTMLElement)) return false;
    return t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName) || !!t.closest?.('[data-nfsu2-ui], dialog[open]');
  };
  const flagsOf = (e: KeyboardEvent | MouseEvent) =>
    (e.altKey ? IKF_ALT : 0) | (e.ctrlKey ? IKF_CTRL : 0) | (e.shiftKey ? IKF_SHIFT : 0) | (e.isTrusted ? 0 : IKF_SYNTH);
  const vkOf = (code: string, e: KeyboardEvent, vk: number) => keyVk(code, e, vk, layout);
  const charsOf = (code: string, e: KeyboardEvent) => keyChars(code, e, mac, layout);
  function onKeyDown(e: KeyboardEvent) {
    if (detached || ignored(e) || e.isComposing || e.keyCode === 229) return;
    if (isMeta(e.code) || e.key === 'Meta') {
      if (!e.repeat) push(IEV_RELEASE_ALL, 0, 0, 0, [], 0, IREASON_META);
      return;
    }
    if (e.metaKey) {
      // the browser's shortcut, except Cmd-F, which toggled fullscreen in the Mac port
      if (mac && e.code === 'KeyF' && !e.repeat) { e.preventDefault(); if (opts.toggleFullscreen) opts.toggleFullscreen(); else void api.toggleFullscreen(); }
      return;
    }
    const k = CODES[e.code];
    if (!k) return;
    if (!pass.has(e.code)) e.preventDefault();
    if (mac && e.code === 'CapsLock') { push(IEV_KEY_TAP, k.dik, k.vk, flagsOf(e)); return; }
    const { chars, dead, sys } = charsOf(e.code, e);
    push(IEV_KEY_DOWN, k.dik, vkOf(e.code, e, k.vk), flagsOf(e) | (e.repeat ? IKF_REPEAT : 0) | (dead ? IKF_DEAD : 0), chars, 0, sys);
  }
  function onKeyUp(e: KeyboardEvent) {
    // every release goes to the game, even one aimed at the page's own UI:
    // the key may have gone down while the game had the keyboard (the C side
    // ignores releases of keys the game does not hold)
    if (detached) return;
    if (isMeta(e.code) || e.key === 'Meta') return;
    const k = CODES[e.code];
    if (!k) return;
    if (!pass.has(e.code) && !ignored(e)) e.preventDefault();   // also keeps Alt from opening a menu bar
    if (mac && e.code === 'CapsLock') { push(IEV_KEY_TAP, k.dik, k.vk, flagsOf(e)); return; }
    push(IEV_KEY_UP, k.dik, k.vk, flagsOf(e));
  }

  // ---- mouse -------------------------------------------------------------------
  let buttons = 0, wheelAcc = 0;
  function pos(e: MouseEvent): [number, number] {
    const rc = element.getBoundingClientRect();
    const v = opts.viewRect?.() ?? { x: 0, y: 0, w: rc.width, h: rc.height };
    const x = (e.clientX - rc.left - v.x) / Math.max(1, v.w), y = (e.clientY - rc.top - v.y) / Math.max(1, v.h);
    return [Math.round(x * 65536), Math.round(y * 65536)];
  }
  function syncButtons(e: PointerEvent | MouseEvent) {
    // DOM buttons mask: 1 left, 2 right, 4 middle, 8 back, 16 forward = DirectInput's 0, 1, 2, 3, 4
    const now = e.buttons & 31;
    for (let i = 0; i < 5; i++) {
      const bit = 1 << i;
      if ((now & bit) !== (buttons & bit)) push(now & bit ? IEV_MOUSE_DOWN : IEV_MOUSE_UP, 0, 0, flagsOf(e), [], i);
    }
    buttons = now;
  }
  function onPointerMove(e: PointerEvent) {
    if (detached) return;
    if (e.pointerType === 'touch') { onFingerMove(e); return; }
    const [x, y] = pos(e);
    push(IEV_MOUSE_MOVE, 0, 0, flagsOf(e), [], x, y);
    if ((e.buttons & 31) !== buttons) syncButtons(e);
  }
  function onPointerDown(e: PointerEvent) {
    if (detached) return;
    if (e.pointerType === 'touch') { onFingerDown(e); return; }
    try { element.setPointerCapture(e.pointerId); } catch { /* a synthetic pointer */ }
    const [x, y] = pos(e);
    push(IEV_MOUSE_MOVE, 0, 0, flagsOf(e), [], x, y);
    syncButtons(e);
    if (document.activeElement !== element && element.tabIndex >= 0) element.focus({ preventScroll: true });
    e.preventDefault();
  }
  function onPointerUp(e: PointerEvent) {
    if (detached) return;
    if (e.pointerType === 'touch') { onFingerUp(e); return; }
    syncButtons(e);                                        // a release is delivered even outside the image
    try { element.releasePointerCapture(e.pointerId); } catch { /* not captured */ }
  }

  // ---- touch: one finger is the mouse, two turn the car --------------------------------
  // A tap is a move, a press and a release within a frame or two; the game
  // takes mouse changes one a frame (runtime/web/input_web.c), so it sees the
  // cursor arrive, then the button go down, then up, however slow its frames.
  // A second finger makes the two a middle-button drag, the PC game's way to
  // turn the car in the garage (its only use of that button, 0x4a5d90): the
  // first finger's button goes up, the cursor to the point between them, then
  // the middle button goes down and the cursor follows that point.  When one of
  // the two lifts, the button goes up and no finger is the mouse until all are.
  let finger = -1, second = -1;
  const fingers = new Map<number, [number, number]>();
  const between = (): [number, number] => {
    const a = fingers.get(finger)!, b = fingers.get(second)!;
    return [Math.round((a[0] + b[0]) / 2), Math.round((a[1] + b[1]) / 2)];
  };
  function onFingerDown(e: PointerEvent) {
    fingers.set(e.pointerId, pos(e));
    if (!e.isPrimary) {
      if (finger < 0 || second >= 0) return;
      second = e.pointerId;
      push(IEV_MOUSE_UP, 0, 0, flagsOf(e), [], 0);
      push(IEV_MOUSE_MOVE, 0, 0, flagsOf(e), [], ...between());
      push(IEV_MOUSE_DOWN, 0, 0, flagsOf(e), [], 2);
      buttons = (buttons & ~1) | 4;
      return;
    }
    finger = e.pointerId;
    push(IEV_MOUSE_MOVE, 0, 0, flagsOf(e) | IKF_TOUCH, [], ...pos(e));
    push(IEV_MOUSE_DOWN, 0, 0, flagsOf(e), [], 0);
    buttons |= 1;
    if (document.activeElement !== element && element.tabIndex >= 0) element.focus({ preventScroll: true });
    e.preventDefault();
  }
  function onFingerMove(e: PointerEvent) {
    if (!fingers.has(e.pointerId)) return;
    fingers.set(e.pointerId, pos(e));
    if (second >= 0) { if (e.pointerId === finger || e.pointerId === second) push(IEV_MOUSE_MOVE, 0, 0, flagsOf(e), [], ...between()); }
    else if (e.pointerId === finger) push(IEV_MOUSE_MOVE, 0, 0, flagsOf(e), [], ...pos(e));
  }
  function onFingerUp(e: PointerEvent) {
    fingers.delete(e.pointerId);
    if (second >= 0 && (e.pointerId === finger || e.pointerId === second)) {
      finger = second = -1;
      push(IEV_MOUSE_UP, 0, 0, flagsOf(e), [], 2);
      buttons &= ~4;
      return;
    }
    if (e.pointerId !== finger) return;
    finger = -1;
    push(IEV_MOUSE_UP, 0, 0, flagsOf(e), [], 0);
    buttons &= ~1;
  }
  function onWheel(e: WheelEvent) {
    if (detached) return;
    e.preventDefault();                                    // no page scroll or Ctrl+wheel zoom
    const lines = e.deltaMode === 1 ? e.deltaY : e.deltaMode === 2 ? e.deltaY * 3 : e.deltaY / 100;
    wheelAcc += -lines * 120;                              // + = away from the user, as WM_MOUSEWHEEL
    const dz = Math.trunc(wheelAcc);
    wheelAcc -= dz;
    if (dz) push(IEV_WHEEL, 0, 0, flagsOf(e), [], dz);
  }
  const onContextMenu = (e: Event) => e.preventDefault();

  // ---- lifecycle -------------------------------------------------------------------
  const onBlur = () => setActive(false, IREASON_WINDOW);
  const onFocus = () => { if (!document.hidden) setActive(true, IREASON_WINDOW); };
  const onVisibility = () => {
    if (document.hidden) setActive(false, IREASON_VISIBILITY);
    else if (document.hasFocus()) setActive(true, IREASON_VISIBILITY);
  };
  const onPageHide = () => push(IEV_RELEASE_ALL, 0, 0, 0, [], 0, IREASON_PAGEHIDE);
  const onElementBlur = () => push(IEV_RELEASE_ALL, 0, 0, 0, [], 0, IREASON_ELEMENT);
  const onFullscreen = () => {
    if (document.fullscreenElement) return;
    // leaving fullscreen (a held Escape, the browser's own key) can swallow key ups
    push(IEV_RELEASE_ALL, 0, 0, 0, [], 0, IREASON_FULLSCREEN);
    keyboardApi()?.unlock?.();
  };

  // ---- key names of the layout (the Controls page) -----------------------------
  async function loadLayout() {
    try { layout = (await keyboardApi()?.getLayoutMap?.()) ?? null; } catch { layout = null; }
    if (!h || !u8) return;
    const names: string[] = [];
    if (layout) for (const [code, ch] of layout) {
      const k = CODES[code];
      if (!k || ch.length !== 1 || ch.charCodeAt(0) < 0x21) continue;
      names[k.dik] = ch.toUpperCase().length === 1 ? ch.toUpperCase() : ch;
    }
    if (!mac) Object.assign(names, { 0x38: 'Alt', 0xB8: 'Right Alt', 0xDB: 'Windows', 0xDC: 'Right Windows', 0x45: 'Num Lock' });
    for (let dik = 0; dik < 256; dik++) {
      const s = names[dik] ?? '', o = base + IM_OFF_NAMES + dik * INPUT_NAME_LEN;
      let n = 0;
      for (const c of s) { const b = cp1252(c.codePointAt(0)!); if (n >= INPUT_NAME_LEN - 1) break; u8[o + n++] = b > 0 ? b : 0x3F; }
      u8[o + n] = 0;
    }
    Atomics.add(h, IM_NAMES_GEN, 1);
  }

  // ---- synthetic requests and pad polling, every frame -----------------------------
  let synF32: Float32Array | null = null;
  function drainSynth() {
    if (!h || !u8 || !synth) return;
    let r = Atomics.load(h, IM_SYN_R);
    const w = Atomics.load(h, IM_SYN_W);
    const f32 = synF32 ??= new Float32Array(u8.buffer, base + IM_OFF_SYN, INPUT_SYN_N * 4);
    while (r !== w) {
      const o = base + IM_OFF_SYN + (r & (INPUT_SYN_N - 1)) * 16, fo = (o - base - IM_OFF_SYN) >> 2;
      const q = { type: u8[o], dik: u8[o + 1], mods: u8[o + 2], ch: i32![(o + 4) >> 2] >>> 0, a: f32[fo + 2], b: f32[fo + 3] };
      r = (r + 1) | 0;
      Atomics.store(h, IM_SYN_R, r);
      try { synth.handle(q); } catch (x) { log(`[input] synthetic event failed: ${x}`); }
    }
  }
  const frame = () => { if (detached) return; pads?.poll(); drainSynth(); raf = requestAnimationFrame(frame); };
  const tick = () => { drainSynth(); if (document.hidden) pads?.poll(); };

  // ---- fullscreen and quit -------------------------------------------------------------
  const api: InputHandle = {
    requestQuit() {
      if (!api.canQuit()) return false;
      if (h) push(IEV_QUIT, 0, 0, 0, [], 0, IREASON_PAGE);
      else { quitPending++; log('[input] quit requested before the game\'s input is up: sent when it is'); }
      return true;
    },
    canQuit() { return !detached && !refused; },
    logReport() {
      if (!h || !api.canQuit()) return false;
      push(IEV_LOG, 0, 0, 0, [], 0, IREASON_PAGE);
      return true;
    },
    sms(id) {
      if (!h || !api.canQuit()) return false;
      push(IEV_SMS, 0, 0, 0, [], id, IREASON_PAGE);
      return true;
    },
    photo(what, a = 0, b = 0) {
      if (!h || !api.canQuit()) return false;
      push(IEV_PHOTO, what, 0, 0, [], a, b);
      return true;
    },
    async enterFullscreen() {
      try {
        if (document.fullscreenElement !== element) await element.requestFullscreen({ navigationUI: 'hide' });
        // all keys: Escape and the browser's reserved shortcuts (Ctrl+W, Alt+Tab ...)
        // reach the game; holding Escape leaves fullscreen
        try { await keyboardApi()?.lock?.(); } catch (e) { log(`[input] keyboard lock refused: ${e}`); }
        return true;
      } catch (e) { log(`[input] fullscreen refused: ${e}`); return false; }
    },
    async exitFullscreen() { keyboardApi()?.unlock?.(); if (document.fullscreenElement) await document.exitFullscreen(); },
    async toggleFullscreen() { if (document.fullscreenElement) await api.exitFullscreen(); else await api.enterFullscreen(); },
    releaseAll() { push(IEV_RELEASE_ALL, 0, 0, 0, [], 0, IREASON_PAGE); },
    probeLatency(n = 20) {
      const res: number[] = [];
      for (let i = 0; i < n && h; i++) {
        const before = Atomics.load(h, IM_EVENTS), t0 = performance.now();
        push(0);                                          // an event type the input thread ignores
        while (Atomics.load(h, IM_EVENTS) === before && performance.now() - t0 < 20) { /* spin */ }
        res.push(Math.round((performance.now() - t0) * 1000));
      }
      return res;
    },
    status(): InputStats {
      const z = (i: number) => (h ? Atomics.load(h, i) >>> 0 : 0);
      return { attached: !!h, events: z(IM_EVENTS), dropped: dropped.n, keysHeld: z(IM_KEYS_HELD), gameInactive: !!z(IM_GAME_INACTIVE),
        quitRequested: !!z(IM_QUIT_REQ), cursor: [z(IM_MOUSE_X) | 0, z(IM_MOUSE_Y) | 0], pad: pads?.current() ?? null };
    },
    stats(): InputCounters {
      const s = api.status();
      return { 'input events': s.events, 'keys held': s.keysHeld, 'pad connected': s.pad ? 1 : 0, 'input dropped': s.dropped };
    },
    lastInput() { return refused ? null : lastUse; },
    key(code, down) {
      const k = CODES[code];
      if (k && !detached) push(down ? IEV_KEY_DOWN : IEV_KEY_UP, k.dik, k.vk, 0, down ? PAGE_KEY_CHARS[code] ?? [] : []);
    },
    touchPad(t) { pads?.setTouch(t); },
    viewRoll(rad) { if (h && !detached) { f32[0] = rad; Atomics.store(h, IM_VIEW_ROLL, f32Bits[0]); } },
    menuScale(s, bar) { menu = bar ? s : -s; putMenu(); },
    padPressed() { return pads?.lastPress ?? 0; },
    textEntry() {
      if (!h || !u8) return null;
      for (let tries = 0; tries < 4; tries++) {
        const s0 = Atomics.load(h, IM_TEXT_SEQ);
        if (s0 & 1) continue;
        const max = Atomics.load(h, IM_TEXT_MAX), kind = Atomics.load(h, IM_TEXT_KIND);
        const len = Math.min(Atomics.load(h, IM_TEXT_LEN), INPUT_TEXT_BYTES), o = base + IM_TEXT * 4;
        const bytes = u8.slice(o, o + len);
        if (Atomics.load(h, IM_TEXT_SEQ) !== s0) continue;
        textNow = max > 0 ? { max, kind, text: win1252.decode(bytes) } : null;
        break;
      }
      return textNow;
    },
    text(chars) {
      if (detached) return;
      for (let i = 0; i < chars.length; i += 3) push(IEV_TEXT, 0, 0, 0, chars.slice(i, i + 3));
    },
    detach() {
      if (detached) return;
      // whatever the game holds is let go (it may outlive the page's input, e.g. a test page)
      push(IEV_RELEASE_ALL, 0, 0, 0, [], 0, IREASON_PAGE);
      api.viewRoll(0);
      if (h) Atomics.compareExchange(h, IM_ATTACHED, 1, 0);   // only our own claim (2 = the C ns* driver)
      detached = true; quitPending = 0; cancelAnimationFrame(raf); clearInterval(timer);
      pads?.dispose(); pads = null; synth = null;
      h = null; u8 = null; i32 = null; synF32 = null;     // no view of the old process's memory stays reachable
      kbTarget.removeEventListener('keydown', onKeyDown as EventListener, { capture: true });
      kbTarget.removeEventListener('keyup', onKeyUp as EventListener, { capture: true });
      for (const [t, f] of listeners) element.removeEventListener(t, f);
      removeEventListener('blur', onBlur); removeEventListener('focus', onFocus); removeEventListener('pagehide', onPageHide);
      for (const t of USE_EVENTS) removeEventListener(t, onUse, { capture: true });
      document.removeEventListener('visibilitychange', onVisibility);
      document.removeEventListener('fullscreenchange', onFullscreen);
      (keyboardApi() as { removeEventListener?(t: string, f: () => void): void } | undefined)?.removeEventListener?.('layoutchange', onLayout);
    },
  };

  kbTarget.addEventListener('keydown', onKeyDown as EventListener, { capture: true });
  kbTarget.addEventListener('keyup', onKeyUp as EventListener, { capture: true });
  const listeners: [string, EventListener][] = [
    ['pointermove', onPointerMove as EventListener], ['pointerdown', onPointerDown as EventListener],
    ['pointerup', onPointerUp as EventListener], ['pointercancel', onPointerUp as EventListener],
    ['contextmenu', onContextMenu], ['blur', onElementBlur],
  ];
  for (const [t, f] of listeners) element.addEventListener(t, f);
  element.addEventListener('wheel', onWheel, { passive: false });
  listeners.push(['wheel', onWheel as EventListener]);
  addEventListener('blur', onBlur); addEventListener('focus', onFocus); addEventListener('pagehide', onPageHide);
  // the page's own menus and dialogs count too: the player is there
  for (const t of USE_EVENTS) addEventListener(t, onUse, { capture: true, passive: true });
  document.addEventListener('visibilitychange', onVisibility);
  document.addEventListener('fullscreenchange', onFullscreen);
  const onLayout = () => void loadLayout();
  keyboardApi()?.addEventListener?.('layoutchange', onLayout);
  if (opts.hideCursor ?? true) element.style.cursor = 'none';
  element.style.touchAction = 'none';

  proc.registered(HostKind.Input).then(reg => {
    if (detached) return;      // detached before the game registered its mailbox
    const buf = proc.memory.buffer as unknown as SharedArrayBuffer;
    const view = new Int32Array(buf, reg.ptr, IM_HDR_WORDS);
    const unquittable = () => { if (quitPending) log('[input] the quit request cannot reach the game'); quitPending = 0; };
    if (view[IM_MAGIC] !== INPUT_MAGIC || view[IM_VERSION] !== INPUT_VERSION) {
      log(`[input] mailbox at 0x${reg.ptr.toString(16)}: magic 0x${(view[IM_MAGIC] >>> 0).toString(16)} version ${view[IM_VERSION]}, not ours: no input`);
      refused = true; unquittable();
      return;
    }
    // the event ring has one writer: the page, unless the game's own driver
    // for the autotest's ns* input claimed it first (input_web.c: no page input)
    const owner = Atomics.compareExchange(view, IM_ATTACHED, 0, 1);
    if (owner !== 0) {
      log(`[input] the event ring is already written by ${owner === 2 ? 'the game (ns* input)' : 'another page'}: no page input`);
      refused = true; unquittable();
      return;
    }
    h = view; u8 = new Uint8Array(buf); i32 = new Int32Array(buf); base = reg.ptr;
    putMenu();
    synth = new SynthDriver({
      element, mac, layout: () => layout, log,
      // nsfocus: the page's blur/focus path, sent even if the page state already matches
      focus: (on) => { active = on; push(IEV_FOCUS, 0, 0, 0, [], on ? 1 : 0, IREASON_SCRIPT); },
    });
    if (opts.gamepads ?? true)
      pads = new PadPoller(h, u8, base, () => dispatchEvent(new Event('nfsu2-gesture')), log, onUse, opts.padPage);
    void loadLayout();
    // the game starts active (CreateWindowEx posts the activation): tell it
    // if the page is not
    active = true;
    if (document.hidden) setActive(false, IREASON_VISIBILITY);
    else if (!document.hasFocus()) setActive(false, IREASON_WINDOW);
    raf = requestAnimationFrame(frame);
    timer = setInterval(tick, 4) as unknown as number;
    log(`[input] attached: mailbox 0x${reg.ptr.toString(16)}, ${mac ? 'macOS' : 'Windows/Linux'} key conventions, ` +
        `page ${document.hidden ? 'hidden' : 'visible'}, ${document.hasFocus() ? 'focused' : 'not focused'}`);
    if (quitPending) log(`[input] sending the quit request${quitPending > 1 ? `s (${quitPending})` : ''} made before the game's input was up`);
    for (; quitPending > 0; quitPending--) push(IEV_QUIT, 0, 0, 0, [], 0, IREASON_PAGE);
  });
  return api;
}
