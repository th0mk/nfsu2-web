// KeyboardEvent.code -> the key's DirectInput scan code (its position, the
// same on every layout) and Windows virtual-key code, as keyboard_mac.m's
// table does for Mac key codes.  The extended-key bit of lParam is DIK bit 7.  Plus the US
// characters of each key, which the synthetic (autotest) events use when the
// browser cannot tell the layout.

export interface KeyDef { dik: number; vk: number }
const T: Record<string, [number, number]> = {
  Escape: [0x01, 0x1B],
  Digit1: [0x02, 0x31], Digit2: [0x03, 0x32], Digit3: [0x04, 0x33], Digit4: [0x05, 0x34], Digit5: [0x06, 0x35],
  Digit6: [0x07, 0x36], Digit7: [0x08, 0x37], Digit8: [0x09, 0x38], Digit9: [0x0A, 0x39], Digit0: [0x0B, 0x30],
  Minus: [0x0C, 0xBD], Equal: [0x0D, 0xBB], Backspace: [0x0E, 0x08], Tab: [0x0F, 0x09],
  KeyQ: [0x10, 0x51], KeyW: [0x11, 0x57], KeyE: [0x12, 0x45], KeyR: [0x13, 0x52], KeyT: [0x14, 0x54],
  KeyY: [0x15, 0x59], KeyU: [0x16, 0x55], KeyI: [0x17, 0x49], KeyO: [0x18, 0x4F], KeyP: [0x19, 0x50],
  BracketLeft: [0x1A, 0xDB], BracketRight: [0x1B, 0xDD], Enter: [0x1C, 0x0D], ControlLeft: [0x1D, 0x11],
  KeyA: [0x1E, 0x41], KeyS: [0x1F, 0x53], KeyD: [0x20, 0x44], KeyF: [0x21, 0x46], KeyG: [0x22, 0x47],
  KeyH: [0x23, 0x48], KeyJ: [0x24, 0x4A], KeyK: [0x25, 0x4B], KeyL: [0x26, 0x4C],
  Semicolon: [0x27, 0xBA], Quote: [0x28, 0xDE], Backquote: [0x29, 0xC0], ShiftLeft: [0x2A, 0x10], Backslash: [0x2B, 0xDC],
  KeyZ: [0x2C, 0x5A], KeyX: [0x2D, 0x58], KeyC: [0x2E, 0x43], KeyV: [0x2F, 0x56], KeyB: [0x30, 0x42],
  KeyN: [0x31, 0x4E], KeyM: [0x32, 0x4D], Comma: [0x33, 0xBC], Period: [0x34, 0xBE], Slash: [0x35, 0xBF],
  ShiftRight: [0x36, 0x10], NumpadMultiply: [0x37, 0x6A], AltLeft: [0x38, 0x12], Space: [0x39, 0x20], CapsLock: [0x3A, 0x14],
  F1: [0x3B, 0x70], F2: [0x3C, 0x71], F3: [0x3D, 0x72], F4: [0x3E, 0x73], F5: [0x3F, 0x74],
  F6: [0x40, 0x75], F7: [0x41, 0x76], F8: [0x42, 0x77], F9: [0x43, 0x78], F10: [0x44, 0x79],
  NumLock: [0x45, 0x90], ScrollLock: [0x46, 0x91],
  Numpad7: [0x47, 0x67], Numpad8: [0x48, 0x68], Numpad9: [0x49, 0x69], NumpadSubtract: [0x4A, 0x6D],
  Numpad4: [0x4B, 0x64], Numpad5: [0x4C, 0x65], Numpad6: [0x4D, 0x66], NumpadAdd: [0x4E, 0x6B],
  Numpad1: [0x4F, 0x61], Numpad2: [0x50, 0x62], Numpad3: [0x51, 0x63], Numpad0: [0x52, 0x60], NumpadDecimal: [0x53, 0x6E],
  IntlBackslash: [0x56, 0xE2], F11: [0x57, 0x7A], F12: [0x58, 0x7B],
  F13: [0x64, 0x7C], F14: [0x65, 0x7D], F15: [0x66, 0x7E],
  KanaMode: [0x70, 0x15], Lang1: [0x70, 0x15], IntlRo: [0x73, 0xE2], Lang2: [0x7B, 0x1D], IntlYen: [0x7D, 0xDC],
  NumpadEqual: [0x8D, 0x92], NumpadEnter: [0x9C, 0x0D], ControlRight: [0x9D, 0x11], NumpadComma: [0xB3, 0x6C],
  NumpadDivide: [0xB5, 0x6F], PrintScreen: [0xB7, 0x2C], AltRight: [0xB8, 0x12], Pause: [0xC5, 0x13],
  Home: [0xC7, 0x24], ArrowUp: [0xC8, 0x26], PageUp: [0xC9, 0x21], ArrowLeft: [0xCB, 0x25], ArrowRight: [0xCD, 0x27],
  End: [0xCF, 0x23], ArrowDown: [0xD0, 0x28], PageDown: [0xD1, 0x22], Insert: [0xD2, 0x2D], Delete: [0xD3, 0x2E],
  Help: [0xD2, 0x2D], MetaLeft: [0xDB, 0x5B], MetaRight: [0xDC, 0x5C], OSLeft: [0xDB, 0x5B], OSRight: [0xDC, 0x5C],
  ContextMenu: [0xDD, 0x5D],
};
export const CODES: Record<string, KeyDef> = Object.fromEntries(Object.entries(T).map(([k, [dik, vk]]) => [k, { dik, vk }]));
/** the touch controls' Drift and Horn (app/touch.ts, through the input layer's key()): keys no keyboard has and the
 *  game never maps (no name in runtime/dikeys.c), so they work whatever keys the player gave those (runtime/drift.c,
 *  horn.c DIK_TOUCH_*); no window message (vk 0) */
export const PAGE_CODES: Record<string, KeyDef> = { Nfsu2Drift: { dik: 0xF1, vk: 0 }, Nfsu2Horn: { dik: 0xF2, vk: 0 } };
/** the first code of every DIK, for synthetic events */
export const CODE_OF_DIK: string[] = [];
for (const [code, [dik]] of Object.entries(T)) if (!CODE_OF_DIK[dik]) CODE_OF_DIK[dik] = code;

export const isMeta = (code: string) => code === 'MetaLeft' || code === 'MetaRight' || code === 'OSLeft' || code === 'OSRight';
export const isLetterCode = (code: string) => code.length === 4 && code.startsWith('Key');

/** Unicode -> Windows code page 1252, -1 if it has no place there */
export function cp1252(u: number): number {
  if (u < 0x80 || (u >= 0xA0 && u <= 0xFF)) return u;
  const hi = [0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039,
    0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC,
    0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178];
  const i = hi.indexOf(u);
  return i >= 0 && u !== 0 ? 0x80 + i : -1;
}

// US layout: [plain, shifted] characters
export const US: Record<string, [string, string]> = {
  Backquote: ['`', '~'], Digit1: ['1', '!'], Digit2: ['2', '@'], Digit3: ['3', '#'], Digit4: ['4', '$'], Digit5: ['5', '%'],
  Digit6: ['6', '^'], Digit7: ['7', '&'], Digit8: ['8', '*'], Digit9: ['9', '('], Digit0: ['0', ')'], Minus: ['-', '_'],
  Equal: ['=', '+'], BracketLeft: ['[', '{'], BracketRight: [']', '}'], Backslash: ['\\', '|'], Semicolon: [';', ':'],
  Quote: ["'", '"'], Comma: [',', '<'], Period: ['.', '>'], Slash: ['/', '?'], Space: [' ', ' '], IntlBackslash: ['§', '±'],
};
for (let c = 0; c < 26; c++) { const l = String.fromCharCode(97 + c); US['Key' + l.toUpperCase()] = [l, l.toUpperCase()]; }
const NUMPAD: Record<string, string> = {
  Numpad0: '0', Numpad1: '1', Numpad2: '2', Numpad3: '3', Numpad4: '4', Numpad5: '5', Numpad6: '6', Numpad7: '7',
  Numpad8: '8', Numpad9: '9', NumpadDecimal: '.', NumpadAdd: '+', NumpadSubtract: '-', NumpadMultiply: '*',
  NumpadDivide: '/', NumpadEqual: '=', NumpadComma: ',',
};
const NAMED: Record<string, string> = {
  Enter: 'Enter', NumpadEnter: 'Enter', Tab: 'Tab', Escape: 'Escape', Backspace: 'Backspace',
  ArrowUp: 'ArrowUp', ArrowDown: 'ArrowDown', ArrowLeft: 'ArrowLeft', ArrowRight: 'ArrowRight',
  Home: 'Home', End: 'End', PageUp: 'PageUp', PageDown: 'PageDown', Insert: 'Insert', Delete: 'Delete',
  ShiftLeft: 'Shift', ShiftRight: 'Shift', ControlLeft: 'Control', ControlRight: 'Control', AltLeft: 'Alt', AltRight: 'Alt',
  MetaLeft: 'Meta', MetaRight: 'Meta', CapsLock: 'CapsLock', ContextMenu: 'ContextMenu', NumLock: 'NumLock',
  ScrollLock: 'ScrollLock', Pause: 'Pause', PrintScreen: 'PrintScreen', KanaMode: 'KanaMode', Lang1: 'Lang1', Lang2: 'Lang2',
};

/** KeyboardEvent.key for a synthetic event on code, given the modifiers;
 *  layout: navigator.keyboard.getLayoutMap() when the browser has it */
export function keyFor(code: string, shift: boolean, caps: boolean, mac: boolean, layout: Map<string, string> | null): string {
  if (NAMED[code]) return NAMED[code];
  if (/^F\d+$/.test(code)) return code;
  if (NUMPAD[code]) return NUMPAD[code];
  const base = layout?.get(code) ?? US[code]?.[0];
  if (base === undefined) return 'Unidentified';
  if (isLetterCode(code) && base.length === 1 && base.toLowerCase() !== base.toUpperCase()) {
    // macOS: Caps Lock gives capitals with or without Shift; Windows inverts
    const upper = mac ? shift || caps : shift !== caps;
    return upper ? base.toUpperCase() : base.toLowerCase();
  }
  if (!shift) return base;
  return layout?.has(code) && layout.get(code) !== US[code]?.[0] ? base.toUpperCase() : US[code]?.[1] ?? base;
}

/** what index.ts needs of a KeyboardEvent (a plain object in the unit tests) */
export interface KeyEventLike {
  key: string; ctrlKey: boolean; altKey: boolean; metaKey: boolean;
  getModifierState(k: string): boolean;
}

/** the key's own (unshifted) character on the layout, '' for a named key */
export function baseChar(code: string, e: KeyEventLike, layout: Map<string, string> | null): string {
  return layout?.get(code) ?? US[code]?.[0] ?? (e.key.length === 1 ? e.key.toLowerCase() : '');
}

/** the Windows virtual-key code: letters follow the layout (AZERTY's A key gives VK_A), as on Windows */
export function keyVk(code: string, e: KeyEventLike, vk: number, layout: Map<string, string> | null): number {
  if (!isLetterCode(code)) return vk;
  const b = baseChar(code, e, layout), u = b.toUpperCase();
  if (u.length === 1 && u >= 'A' && u <= 'Z') return u.charCodeAt(0);
  return ({ ',': 0xBC, '.': 0xBE, '-': 0xBD, ';': 0xBA } as Record<string, number>)[b] ?? vk;
}

/**
 * WM_CHAR bytes for a key down; sys = the plain character for WM_SYSCHAR when
 * Alt makes it a command.  runtime/web/input_web.c (cs_key) does the same for
 * the autotest's keys on a page without this layer, for synthetic events on
 * the US layout with macOS rules (web/test/input-chars.test.mjs compares them).
 */
export function keyChars(code: string, e: KeyEventLike, mac: boolean, layout: Map<string, string> | null):
    { chars: number[]; dead: boolean; sys: number } {
  switch (code) {
    case 'Enter': case 'NumpadEnter': return { chars: [0x0D], dead: false, sys: 0 };
    case 'Backspace': return { chars: [0x08], dead: false, sys: 0 };
    case 'Escape': return { chars: [0x1B], dead: false, sys: 0 };
    case 'Tab': return { chars: [0x09], dead: false, sys: 0 };
  }
  if (e.key === 'Dead') return { chars: [0x3F], dead: true, sys: 0 };       // the spacing character is not exposed
  const cps = [...e.key];
  if (cps.length !== 1 || e.metaKey) return { chars: [], dead: false, sys: 0 };
  const altGr = e.getModifierState('AltGraph');
  const plain = baseChar(code, e, layout);
  const sysc = plain.length === 1 ? Math.max(0, cp1252(plain.codePointAt(0)!)) : 0;
  if (e.ctrlKey && !altGr && !e.altKey) {
    // Ctrl+letter types a control character, as TranslateMessage does
    const u = e.key.toUpperCase();
    return { chars: u.length === 1 && u >= 'A' && u <= 'Z' ? [u.charCodeAt(0) - 0x40] : [], dead: false, sys: 0 };
  }
  if (e.altKey && !altGr && !e.ctrlKey) {
    // Windows/Linux: Alt+key is a command (WM_SYSKEYDOWN + WM_SYSCHAR).  macOS:
    // Option+key that types something else than the key's own character is text
    if (!mac || e.key === plain || e.key === plain.toUpperCase()) return { chars: [], dead: false, sys: sysc };
  }
  const c = cp1252(cps[0].codePointAt(0)!);
  return { chars: [c >= 0 ? c : 0x3F], dead: false, sys: sysc };
}

/** the key (and Shift) that types ch, for nstype; null if none on this layout */
export function strokeFor(ch: string, layout: Map<string, string> | null): { code: string; shift: boolean } | null {
  switch (ch) {
    case '\r': case '\n': return { code: 'Enter', shift: false };
    case '\b': return { code: 'Backspace', shift: false };
    case '\t': return { code: 'Tab', shift: false };
    case '\x1b': return { code: 'Escape', shift: false };
  }
  const codes = Object.keys(US);
  for (const code of codes) {
    const base = layout?.get(code) ?? US[code][0];
    if (base === ch) return { code, shift: false };
  }
  for (const code of codes) {
    const base = layout?.get(code) ?? US[code][0];
    const shifted = isLetterCode(code) ? base.toUpperCase() : layout?.has(code) && base !== US[code][0] ? base.toUpperCase() : US[code][1];
    if (shifted === ch) return { code, shift: true };
  }
  return null;
}
