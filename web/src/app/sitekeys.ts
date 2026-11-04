// This site's own controls beside the game's: drift mode, the trunk, hood and doors, the horn, the headlights, quick
// chat, Message a friend, Add friend, photo mode, the page's menu, the next song and the frame-rate counter.  Each has
// a key and, where one goes, a controller button, which the player sets on the start page (Controls, Site) by pressing
// it, or turns off.  settings.ini keeps them (key_*, button_*: settings.ts, this device's); the runtime reads the ones
// it handles (runtime/dinput_kbd.c dikbd_site_key: drift mode, the trunk, the horn, the headlights, and Message a
// friend's and Add friend's, whose clashes it tells the page: gamekeys.ts; dinput_pad.c the buttons), the page the
// others at Play (applySiteKeys with the game's environment) and on the start page as they change.  (The headlights'
// controller has no button to set: the look's stick pushed straight up, runtime/lightkey.c.)
//   offered   a key the game has one of its controls on by default (its driving map's keys, its menus' for the ones
//             that work in the menus too), the browser's F5 and F11, quick chat's 1 to 4 and another of ours are
//             refused; the ones that work in the menus too (the page's menu, photo mode, the next song, the counter)
//             take only keys that type nothing (F6 to F12, Page Up, Page Down, Pause, Scroll Lock, Delete), and the
//             page's menu is never off.  A controller's: a D-pad direction held a second (drift mode, the trunk: a
//             short press stays the game's) or a stick's click (the horn, quick chat); Home and both sticks pressed in
//             always open the menu.
//   bound     a key the player put one of the game's controls on (its Options, Controls: the game keeps those in
//             registry.txt, runtime/dinput_kbd.c's user maps) stays the game's: its row says so.
import { CODE_OF_DIK, CODES } from '../input/keymap';
import { N, t } from './i18n';

export type OurId = 'drift' | 'showoff' | 'horn' | 'lights' | 'chat' | 'message' | 'friend' | 'photo' | 'menu' | 'song' | 'fps';
export type ButtonKey = 'button_hold' | 'button_horn' | 'button_chat';

export interface Our {
  id: OurId;
  /** its name (in what a refusal says, and for a screen reader) and what it does (its row's); English, translated where
   *  drawn */
  name: string; line: string;
  /** settings.ini's key for its key and the default (a KeyboardEvent.code); none: its keys are fixed (quick chat's) */
  key?: string; def?: string;
  /** what stands for its keys where they are fixed */
  fixed?: string;
  /** it works in the menus too: only keys that type nothing */
  menus?: boolean;
  /** never off (the page's menu: the way back) */
  always?: boolean;
  /** its controller button's settings.ini key */
  button?: ButtonKey;
}

/** ours, in the rows' order (each one's settings.ini key is in settings.ts too) */
export const OURS: readonly Our[] = [
  { id: 'drift', name: N('Drift mode'), line: N('Drift mode (in free roam)'), key: 'key_drift', def: 'KeyX', button: 'button_hold' },
  { id: 'showoff', name: N('Trunk, hood, doors'), line: N('Open trunk, then hood, then doors (parked, in free roam)'), key: 'key_showoff', def: 'KeyO',
    button: 'button_hold' },
  { id: 'horn', name: N('Horn'), line: N('Horn (with other players)'), key: 'key_horn', def: 'KeyN', button: 'button_horn' },
  { id: 'lights', name: N('Headlights'), line: N('Headlights off or on; hold for high beam'), key: 'key_lights', def: 'KeyG' },
  { id: 'chat', name: N('Quick chat'), line: N('Quick chat (with other players): group, then phrase'), fixed: '1–4', button: 'button_chat' },
  { id: 'message', name: N('Message a friend'), line: N('Message a friend (in free roam)'), key: 'key_message', def: 'KeyI' },
  { id: 'friend', name: N('Add friend'), line: N('Add a friend (front bumpers touching)'), key: 'key_friend', def: 'KeyF' },
  { id: 'photo', name: N('Photo mode'), line: N('Photo mode, then the photo'), key: 'key_photo', def: 'F12', menus: true },
  { id: 'menu', name: N('Menu'), line: N('Menu: full screen, controls, log, quit'), key: 'key_menu', def: 'F9', menus: true, always: true },
  { id: 'song', name: N('Next song'), line: N('Next song'), key: 'key_song', def: 'F7', menus: true },
  { id: 'fps', name: N('Frame-rate counter'), line: N('Frame-rate counter'), key: 'key_fps', def: 'F8', menus: true },
];
export const OUR = Object.fromEntries(OURS.map(o => [o.id, o])) as Record<OurId, Our>;
/** each button's choices, the default first, and what each is in the controller's own words */
export const BUTTONS: Record<ButtonKey, readonly string[]> = {
  button_hold: ['ddown', 'dup', 'dleft', 'dright'],
  button_horn: ['ls', 'rs'],
  button_chat: ['rs', 'ls'],
};
/** settings.ini's keys of ours (settings.ts has them too) */
export const SITE_KEYS: readonly string[] = [...OURS.flatMap(o => o.key ? [o.key] : []), ...Object.keys(BUTTONS)];

// ---- what is set now ------------------------------------------------------------------------------------------
/** settings.ini key -> its environment variable's name (key_drift -> NFSU2_KEY_DRIFT: settings.ts envNameOf's way) */
const envOf = (k: string) => `NFSU2_${k.toUpperCase()}`;
let now: Record<string, string> = {};
const listeners: (() => void)[] = [];
/** the game's environment (or the start page's settings resolved): what ours are from now on */
export function applySiteKeys(env: Record<string, string | undefined>) {
  const next: Record<string, string> = {};
  for (const k of SITE_KEYS) { const v = env[envOf(k)]; if (v) next[k] = v; }
  if (JSON.stringify(next) === JSON.stringify(now)) return;
  now = next;
  for (const f of listeners) f();
}
/** f after every change of ours (the hints that name a key) */
export function onSiteKeys(f: () => void) { listeners.push(f); }
/** a control's key now (a KeyboardEvent.code), null: off or none */
export function siteKey(id: OurId): string | null {
  const o = OUR[id];
  if (!o.key) return null;
  const v = now[o.key] ?? o.def!;
  if (v === 'off') return o.always ? o.def! : null;
  return CODES[v] ? v : o.def!;
}
/** the control a key is now (one of ours that works in the menus too: they are the page's at any time) */
export function menuKeyOf(code: string): OurId | null {
  for (const id of ['menu', 'photo', 'song', 'fps'] as const) if (siteKey(id) === code) return id;
  return null;
}
/** a button's setting now ('off': none) */
export function siteButton(k: ButtonKey): string {
  const v = now[k], ok = BUTTONS[k];
  return v === 'off' ? 'off' : v && ok.includes(v) ? v : ok[0];
}

// ---- names ------------------------------------------------------------------------------------------------------
const PUNCT: Record<string, string> = {
  Minus: '-', Equal: '=', BracketLeft: '[', BracketRight: ']', Semicolon: ';', Quote: '\'', Backquote: '`', Backslash: '\\',
  IntlBackslash: '<', Comma: ',', Period: '.', Slash: '/', NumpadAdd: 'Num +', NumpadSubtract: 'Num -', NumpadMultiply: 'Num *',
  NumpadDivide: 'Num /', NumpadDecimal: 'Num .', PageUp: 'Page Up', PageDown: 'Page Down', ScrollLock: 'Scroll Lock',
};
/** a key's name on its cap: X, 5, F12, Num 7, Page Up (the US layout's) */
export function keyName(code: string): string {
  if (/^Key[A-Z]$/.test(code)) return code.slice(3);
  if (/^Digit\d$/.test(code)) return code.slice(5);
  if (/^Numpad\d$/.test(code)) return `Num ${code.slice(6)}`;
  return PUNCT[code] ?? code;
}
/** a controller button's name: the D-pad's arrows, a stick as Xbox or PlayStation names it */
export function buttonName(v: string, ps = false): string {
  switch (v) {
    case 'ddown': return t('D-pad {arrow}', { arrow: '↓' });
    case 'dup': return t('D-pad {arrow}', { arrow: '↑' });
    case 'dleft': return t('D-pad {arrow}', { arrow: '←' });
    case 'dright': return t('D-pad {arrow}', { arrow: '→' });
    case 'ls': return ps ? 'L3' : 'LS';
    case 'rs': return ps ? 'R3' : 'RS';
    default: return t('Off');
  }
}
/** a D-pad direction's cap: a small cross with that arm filled (kbd .glyph, as controls.ts draws the pad's names), the
 *  same in every language, its name said (buttonName); a stick's its name; none: Off, dimmed */
const DPAD = 'M5.5 1.5h3v4h4v3h-4v4h-3v-4h-4v-3h4z';
const ARM: Record<string, string> = { dup: 'M5.5 1.5h3v4h-3z', ddown: 'M5.5 8.5h3v4h-3z', dleft: 'M1.5 5.5h4v3h-4z', dright: 'M8.5 5.5h4v3h-4z' };
export function buttonCap(v: string, ps = false): HTMLElement {
  if (v === 'off') return offCap();
  if (!(v in ARM)) return kbd(buttonName(v, ps));
  const k = document.createElement('kbd'), NS = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(NS, 'svg');
  svg.setAttribute('class', 'glyph'); svg.setAttribute('viewBox', '0 0 14 14'); svg.setAttribute('role', 'img');
  svg.setAttribute('aria-label', buttonName(v));
  for (const [d, fill] of [[DPAD, ''], [ARM[v], 'currentColor']]) {
    const p = document.createElementNS(NS, 'path'); p.setAttribute('d', d);
    if (fill) p.setAttribute('fill', fill);
    svg.append(p);
  }
  k.append(svg);
  return k;
}
function offCap(): HTMLElement { const k = kbd(t('Off')); k.classList.add('off'); return k; }
/** the standard mapping's buttons for a button choice (navigator.getGamepads: 10 LS, 11 RS, 12-15 the D-pad) */
const PAD_INDEX: Record<string, number> = { ls: 10, rs: 11, dup: 12, ddown: 13, dleft: 14, dright: 15 };

// ---- what may be picked ------------------------------------------------------------------------------------------
/** the keys the game has its controls on by default: its driving map's (0x7fcac0, every action but none: its debug
 *  camera's too), and for ours that work in the menus too its menus' (0x7fd7e0) */
const GAME_DRIVING = ['ArrowUp', 'KeyW', 'ArrowDown', 'KeyS', 'ArrowLeft', 'KeyA', 'ArrowRight', 'KeyD', 'ShiftLeft', 'Semicolon',
  'ControlLeft', 'Period', 'Space', 'End', 'AltLeft', 'AltRight', 'KeyQ', 'Equal', 'KeyC', 'Insert', 'KeyB', 'KeyK', 'Escape', 'KeyP',
  'KeyR', 'Home', 'Enter', 'F1', 'KeyM', 'F2', 'Tab', 'F3', 'KeyT', 'F4', 'KeyJ', 'KeyU', 'KeyH', 'KeyY', 'Digit5', 'Numpad8',
  'Numpad2', 'Numpad4', 'Numpad6', 'Numpad7', 'Numpad9', 'KeyL', 'Numpad1', 'Numpad3', 'BracketLeft', 'BracketRight', 'KeyG'];
const GAME_MENUS = ['NumpadEnter', 'Comma'];
/** ours that work in the menus too take only these: they type nothing in the game's text fields */
const NO_TEXT = ['F6', 'F7', 'F8', 'F9', 'F10', 'F12', 'F13', 'F14', 'F15', 'PageUp', 'PageDown', 'Pause', 'ScrollLock', 'Delete'];
/** never: the browser's (reload, full screen), the modifiers, keys that are the system's */
const NEVER = ['F5', 'F11', 'ShiftRight', 'ControlRight', 'MetaLeft', 'MetaRight', 'OSLeft', 'OSRight', 'CapsLock', 'NumLock',
  'PrintScreen', 'ContextMenu', 'Backspace', 'NumpadEnter'];
/** quick chat's, fixed */
const CHAT_KEYS = ['Digit1', 'Digit2', 'Digit3', 'Digit4', 'Numpad1', 'Numpad2', 'Numpad3', 'Numpad4'];

/** why our control cannot have this key ('' it can); others: the keys the other rows have now */
export function refusal(id: OurId, code: string, others: (id: OurId) => string | null = siteKey): string {
  // (its own default always may: the headlights' G is one of the game's debug camera's)
  const o = OUR[id], k = keyName(code), own = code === o.def;
  if (!CODES[code] || NEVER.includes(code)) return t('{key} can\'t be used here.', { key: k });
  if (CHAT_KEYS.includes(code)) return t('{key} is quick chat\'s.', { key: k });
  if (!own && (GAME_DRIVING.includes(code) || (o.menus && GAME_MENUS.includes(code)))) return t('{key} is one of the game\'s keys.', { key: k });
  if (o.menus && !NO_TEXT.includes(code)) return t('This one works in menus too: pick a key that types nothing, such as F6 to F12.');
  for (const p of OURS) if (p.id !== id && others(p.id) === code) return t('{key} is already {what}.', { key: k, what: t(p.name) });
  return '';
}
/** why a button cannot have this choice ('' it can): the horn and quick chat on one click */
export function buttonRefusal(k: ButtonKey, v: string, others: (k: ButtonKey) => string = siteButton): string {
  const other = k === 'button_horn' ? 'button_chat' : k === 'button_chat' ? 'button_horn' : null;
  if (other && v !== 'off' && others(other) === v)
    return t('{button} is already {what}.', { button: buttonName(v), what: t(other === 'button_horn' ? 'Horn' : 'Quick chat') });
  return '';
}

/** the keys the player put one of the game's controls on (registry.txt's user maps, runtime/dinput_kbd.c:
 *  "s DIMap.{map}.<actions>.<action>=<semantic> <action id> <key's object>", the key in its second byte) */
export function gameBoundKeys(registry: string | null): Set<string> {
  const out = new Set<string>();
  for (const m of (registry ?? '').matchAll(/^s DIMap\.[^=\n]*=\s*[0-9a-f]{8} [0-9a-f]{8} ([0-9a-f]{8})\s*$/gim)) {
    const dik = (parseInt(m[1], 16) >>> 8) & 0xff, code = dik ? CODE_OF_DIK[dik] : undefined;
    if (code) out.add(code);
  }
  return out;
}

// ---- the rows ----------------------------------------------------------------------------------------------------
export interface SiteKeysDeps {
  /** settings.ini's line of a key (settings-ui.ts SettingsPanel set: null is the default), and what it says now */
  set(key: string, value: string | null): Promise<void> | void;
  current(key: string): string;
  /** another tab has the saves: nothing can be changed here */
  readOnly(): boolean;
  /** registry.txt of the saves (the game's own controls' keys), null: none */
  registry(): Promise<string | null>;
  /** PlayStation names for the sticks */
  ps(): boolean;
}

/**
 * The rows into box (the Controls sheet's Site tab), as the keys' list: what each of ours does, its key's cap and its
 * button's at the right.  editable: the start page's, where each cap is a button: a press waits for the new key (Esc:
 * as it was; Off: none) or the controller's press; else (the menu's copy while a game runs) only what they are.  On
 * touch screens too: phones and tablets play with controllers.
 */
export class SiteKeysView {
  private box: HTMLElement | null = null;
  private editable = false;
  private bound = new Set<string>();
  private waiting: { id: OurId; button: boolean } | null = null;
  private note: { id: OurId; text: string; cls: string } | null = null;
  private stopWait: (() => void) | null = null;

  constructor(private deps: SiteKeysDeps) {}

  /** draws into box (again: a change of language, of settings.ini) */
  render(box?: HTMLElement, editable?: boolean) {
    if (box) { this.box = box; this.editable = !!editable; }
    if (!this.box) return;
    if (this.editable) void this.deps.registry().then(r => { const b = gameBoundKeys(r); if ([...b].join() !== [...this.bound].join()) { this.bound = b; this.draw(); } });
    this.draw();
  }

  /** the key's setting now (as the file says: the start page's own view of it) */
  private keyNow(o: Our): string | null {
    if (!o.key) return null;
    const v = this.deps.current(o.key) || o.def!;
    return v === 'off' ? (o.always ? o.def! : null) : CODES[v] ? v : o.def!;
  }
  private buttonNow(k: ButtonKey): string {
    const v = this.deps.current(k) || BUTTONS[k][0];
    return v === 'off' || BUTTONS[k].includes(v) ? v : BUTTONS[k][0];
  }

  private draw() {
    const box = this.box!;
    const root = Object.assign(document.createElement('dl'), { className: 'keys site-keys' });
    for (const o of OURS) root.append(this.row(o));
    // (Home or both sticks for the menu, and holding Esc to leave full screen, are not set here: index.html's lines
    // under the box)
    box.replaceChildren(root);
    if (this.editable) {
      const p = document.createElement('p'); p.className = 'hint';
      p.textContent = t('Click a key or a button, then press the new one. Applies the next time you press Play.');
      box.prepend(p);
    }
  }

  /** a row: what it does, the caps at the right (the keys' list's look) */
  private row(o: Our): HTMLElement {
    const row = document.createElement('div'); row.dataset.our = o.id;
    const dt = document.createElement('dt'), dd = document.createElement('dd');
    dt.append(...this.cells(o));
    dd.textContent = t(o.line);
    row.append(dt, dd);
    this.state(row, o);
    return row;
  }

  /** the key's and the button's caps (a button each that can be set; a plain cap for quick chat's fixed keys) */
  private cells(o: Our): HTMLElement[] {
    const out: HTMLElement[] = [];
    const k = this.keyNow(o);
    if (o.fixed) out.push(kbd(o.fixed));
    else if (o.key) out.push(this.cap_(o, false, k ? keyName(k) : t('Off'), k ? kbd(keyName(k)) : offCap()));
    if (o.button) {
      const b = this.buttonNow(o.button);
      out.push(this.cap_(o, true, buttonName(b, this.deps.ps()), buttonCap(b, this.deps.ps())));
    }
    return out;
  }
  /** a cap: text its name (said), cap what it shows (a key's name, the D-pad's glyph: buttonCap); a button round it
   *  where it can be set, its frame lime while it waits for a press */
  private cap_(o: Our, button: boolean, text: string, cap: HTMLElement): HTMLElement {
    const waiting = this.waiting?.id === o.id && this.waiting.button === button;
    const what = button ? t('{what}: controller button', { what: t(o.name) }) : t('{what}: key', { what: t(o.name) });
    if (!this.editable) { cap.setAttribute('aria-label', `${what}: ${text}`); return cap; }
    const b = document.createElement('button'); b.type = 'button'; b.className = 'keycap';
    b.setAttribute('aria-label', `${what}: ${waiting ? '' : text}`);
    if (waiting) {
      b.classList.add('selected');
      b.append(kbd(button ? t('Press a button') : t('Press a key')));
    } else b.append(cap);
    b.disabled = this.deps.readOnly();
    b.onclick = () => { if (waiting) this.cancel(); else this.wait(o, button); };
    return b;
  }
  /** under the row: why the last press was not taken, or that the game has its key */
  private state(row: HTMLElement, o: Our) {
    let text = '', cls = '';
    if (this.note?.id === o.id) ({ text, cls } = this.note);
    else if (this.waiting?.id === o.id) {
      text = this.waiting.button ? t('Press D-pad or a stick on the controller. Esc: as it was.') : t('Esc: as it was.');
      cls = 'free';
    } else {
      const k = this.keyNow(o);
      if (k && this.bound.has(k)) { text = t('One of the game\'s controls is on {key} (its Options, Controls): the game gets it.', { key: keyName(k) }); cls = 'env'; }
    }
    if (!text && !(this.waiting?.id === o.id)) return;
    const p = document.createElement('p'); p.className = `set-state ${cls}`; p.textContent = text;
    if (this.waiting?.id === o.id && this.editable && !(OUR[o.id].always && !this.waiting.button)) {
      const off = document.createElement('button'); off.type = 'button'; off.className = 'text-btn'; off.textContent = t('Off');
      off.onclick = () => { const w = this.waiting!; this.cancel(); void this.take(o, w.button, 'off'); };
      p.append(' ', off);
    }
    row.append(p);
  }

  /** a press on a box: the next key (or controller button) is the control's */
  private wait(o: Our, button: boolean) {
    this.cancel();
    this.waiting = { id: o.id, button }; this.note = null;
    this.draw();
    const keydown = (e: KeyboardEvent) => {
      if (e.repeat || e.metaKey || e.ctrlKey || e.altKey) return;
      e.preventDefault(); e.stopImmediatePropagation();
      if (e.code === 'Escape') { this.cancel(); return; }
      if (button) return;
      this.cancel();
      void this.take(o, false, e.code);
    };
    window.addEventListener('keydown', keydown, { capture: true });
    let raf = 0;
    const was = new Set<string>();
    const poll = () => {
      // a controller's press of a choice it offers (the standard mapping's places), not one held since the wait began
      const pads = typeof navigator.getGamepads === 'function' ? navigator.getGamepads() : [];
      const down = new Set<string>();
      for (const g of pads) if (g?.connected) for (const [v, i] of Object.entries(PAD_INDEX)) if (g.buttons[i]?.pressed) down.add(v);
      const fresh = [...down].find(v => !was.has(v) && BUTTONS[o.button!].includes(v));
      for (const v of [...was]) if (!down.has(v)) was.delete(v);
      if (!fresh) { for (const v of down) was.add(v); raf = requestAnimationFrame(poll); return; }
      this.cancel();
      void this.take(o, true, fresh);
    };
    if (button) { for (const g of navigator.getGamepads?.() ?? []) if (g?.connected) for (const [v, i] of Object.entries(PAD_INDEX)) if (g.buttons[i]?.pressed) was.add(v);
      raf = requestAnimationFrame(poll); }
    this.stopWait = () => { window.removeEventListener('keydown', keydown, { capture: true }); cancelAnimationFrame(raf); };
    (this.box?.querySelector<HTMLElement>(`[data-our="${o.id}"] .selected`))?.focus();
  }
  private cancel() {
    this.stopWait?.(); this.stopWait = null;
    if (this.waiting) { this.waiting = null; this.draw(); }
  }

  /** the key (a code, or off) or the button picked: saved, or why not under the row */
  private async take(o: Our, button: boolean, v: string) {
    if (button) {
      const why = v === 'off' ? '' : buttonRefusal(o.button!, v, k => this.buttonNow(k));
      if (why) { this.note = { id: o.id, text: why, cls: 'bad' }; this.draw(); return; }
      this.note = null;
      await this.deps.set(o.button!, v === BUTTONS[o.button!][0] ? null : v);
    } else {
      const why = v === 'off' ? '' : refusal(o.id, v, id => this.keyNow(OUR[id]));
      if (why) { this.note = { id: o.id, text: why, cls: 'bad' }; this.draw(); return; }
      this.note = null;
      await this.deps.set(o.key!, v === o.def ? null : v);
    }
    this.draw();
  }
}

function kbd(text: string): HTMLElement { const k = document.createElement('kbd'); k.textContent = text; return k; }

/** the keys' list (index.html's Keyboard tab, and its copy in the menu): each row of ours (data-key-of) with the key
 *  the player gave it, hidden without one */
export function siteKeyList(root: ParentNode) {
  for (const row of root.querySelectorAll<HTMLElement>('[data-key-of]')) {
    const k = siteKey(row.dataset.keyOf as OurId), cap = row.querySelector('kbd');
    row.hidden = !k;
    if (k && cap) cap.textContent = keyName(k);
  }
}
