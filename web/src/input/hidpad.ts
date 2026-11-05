// Controllers the browser gives no standard mapping whose layout the page knows, for the game's virtual pad
// (gamepad.ts reads any other as if it had the standard mapping): 8BitDo's and pads with their layout (SAME_AS_8BITDO).
//
// In D-input mode, and in their Android and Bluetooth modes, 8BitDo's controllers show up as plain HID gamepads of
// vendor 2dc8.  Chrome maps two of them (the Ultimate 2C's and the Ultimate 2's Bluetooth, on macOS), Firefox none
// outside Linux, so their buttons came in their report's order read as the standard mapping's: A and B swapped, the
// shoulder buttons were the triggers, the triggers' own buttons Back and Start, Start a stick's button, and the right
// stick and the analog triggers sat where no stick was read (a player's report: the controls did not work).  Every
// model reports the same buttons (SDL_GameControllerDB, SDL's own database): by their labels A, B, -, X, Y, -, L1, R1,
// L2, R2, Select, Start, Home, L3, R3 (the two gaps the Pro 2's and the Ultimate's back paddles or an older pad's
// Home, left out so a paddle does not open the page's menu); the left stick X and Y, the right stick Z and Rz, analog
// triggers where the model has them as the Accelerator (R2) and the Brake (L2) of the Simulation Controls page, and
// the D-pad a hat switch.  The game's pad is positional (runtime/dinput_pad.c names its buttons so): A is the bottom
// face button.  Most 8BitDo pads are labelled as Nintendo's (A right, B at the bottom, X at the top) and are the
// Nintendo family to the game; the Ultimate line and a few others are labelled as Xbox's (XBOX_FACE).  In X-input and
// Switch mode they are an Xbox 360 and a Switch Pro controller to the browsers, which map those.  Chrome on Android
// puts them in the standard mapping's places itself without calling that standard (gamepad.ts androidLayout): not
// read here.
//
// The axes are placed as each browser places a HID device's (wheel.ts Placement).  By usage (Chrome on Windows and
// macOS, Firefox on Windows): the right stick on 2 and 5, the hat switch on 9, and Chrome's analog triggers in the
// places left free, R2 on 3 and L2 on 4 (where Chromium's own mapping of the Ultimate 2C reads them; Firefox shows
// none, and a place left free reads 0: the triggers' buttons then).  Otherwise one after another: the left stick, the
// right, R2 and L2 where the model has them, and the hat switch where the browser put it, as one axis anywhere that
// reads outside -1..1 let go (Firefox on macOS), as two last that rest at 0 (Linux: ABS_HAT0X and HAT0Y), or not at
// all (Safari).

import { PAD_A, PAD_B, PAD_BACK, PAD_HOME, PAD_LB, PAD_LS, PAD_RB, PAD_RS, PAD_START, PAD_X, PAD_Y } from './proto';
import { dpadDirs, findHat, hatDirs, usbIds } from './wheel';

/** how the face buttons are labelled: as Nintendo's (A right, B at the bottom) or as Xbox's (A at the bottom) */
export type Face = 'nintendo' | 'xbox';

// the 8BitDo pads labelled as Xbox's, by product id (SDL_GameControllerDB maps their A as the bottom button): the
// Ultimate, Ultimate C, 2C and 2 and those made for Xbox, the Xbox One SN30 Pro, 64, P30, NEOGEO, an M30, the N30
// (NES) and F30 Arcade stick, and the USB adapters (another maker's controller through them)
const XBOX_FACE = new Set(['1003', '1080', '2000', '2002', '2003', '200a', '2019', '2025', '2100', '2101', '2810',
  '3011', '3012', '3013', '3015', '3016', '3017', '3019', '301b', '301c', '301d', '3100', '3105', '3106', '310a', '310b',
  '5006', '5107', '5108', '6012', '9025', '9026']);

// the buttons in the report's order as the pad's (-1: none); L2 and R2 are the triggers' own buttons
const NINTENDO_BUTTONS = [PAD_B, PAD_A, -1, PAD_Y, PAD_X, -1, PAD_LB, PAD_RB, -1, -1, PAD_BACK, PAD_START, PAD_HOME,
  PAD_LS, PAD_RS];
const XBOX_BUTTONS = [PAD_A, PAD_B, -1, PAD_X, PAD_Y, -1, PAD_LB, PAD_RB, -1, -1, PAD_BACK, PAD_START, PAD_HOME,
  PAD_LS, PAD_RS];
const L2 = 8, R2 = 9;

// other makers' HID pads with the same report, as vendor:product, and their face labels: the Zikway HID gamepad (a
// GameSir-made pad; read on Linux: A, B, -, X, Y, -, LB, RB, LT, RT, Select, Start, -, LS, RS, the triggers on
// ABS_GAS and ABS_BRAKE; read as the standard mapping its RB was the right trigger, the throttle)
const SAME_AS_8BITDO: Record<string, Face> = { '3537:1041': 'xbox' };

/** an 8BitDo controller's face buttons (its model's), or another with 8BitDo's layout, from Gamepad.id; null: neither */
export function eightBitDoFace(id: string): Face | null {
  const u = usbIds(id);
  if (u?.vendor !== '2dc8') return SAME_AS_8BITDO[`${u?.vendor}:${u?.product}`] ?? null;
  return XBOX_FACE.has(u.product) ? 'xbox' : 'nintendo';
}

/** where a pad's axes are: the sticks, the triggers (-1: none), the D-pad as one axis (-1: none) or two */
export interface HidAxes {
  lx: number; ly: number; rx: number; ry: number; rt: number; lt: number; hat: number; dpad?: [number, number];
}

/** an 8BitDo pad's axes placed as the browser does, from where they read (null: every axis 0, not read yet) */
export function placeHidAxes(axes: readonly number[]): HidAxes | null {
  if (axes.length >= 10) return { lx: 0, ly: 1, rx: 2, ry: 5, rt: 3, lt: 4, hat: 9 };
  if (axes.every(v => v === 0)) return null;
  const hat = findHat(axes);
  let list = axes.map((_, i) => i).filter(i => i !== hat), dpad: [number, number] | undefined;
  const n = list.length, centred = (i: number) => Math.abs(axes[i]) < 0.5;
  if (hat < 0 && n >= 6 && centred(n - 2) && centred(n - 1)) { dpad = [n - 2, n - 1]; list = list.slice(0, -2); }
  const at = (i: number) => list[i] ?? -1;
  return { lx: at(0), ly: at(1), rx: at(2), ry: at(3), rt: at(4), lt: at(5), hat, ...(dpad ? { dpad } : {}) };
}

const clamp = (v: number, lo: number, hi: number) => v < lo ? lo : v > hi ? hi : v;

/** An 8BitDo pad without the standard mapping read as the pad.  Its axes are placed once they have been read (Chrome
 *  on macOS shows every axis 0 before the first report); a trigger's axis counts once it has read anything but 0,
 *  until then its button is the trigger, pressed all the way. */
export class HidPadReader {
  private at: HidAxes | null;
  private live = [false, false];             // R2's axis, L2's

  constructor(readonly face: Face, axes0: readonly number[]) { this.at = placeHidAxes(axes0); }

  /** the pad's buttons (1 << PAD_*) and axes LX, LY, RX, RY, LT, RT from the controller's */
  read(axes: readonly number[], pressed: readonly boolean[]): { buttons: number; axes: number[] } {
    const a = this.at ??= placeHidAxes(axes), table = this.face === 'xbox' ? XBOX_BUTTONS : NINTENDO_BUTTONS;
    let buttons = 0;
    for (let i = 0; i < table.length && i < pressed.length; i++) if (pressed[i] && table[i] >= 0) buttons |= 1 << table[i];
    if (a && a.hat >= 0) buttons |= hatDirs(axes[a.hat]);
    if (a?.dpad) buttons |= dpadDirs(axes[a.dpad[0]], axes[a.dpad[1]]);
    const stick = (i: number) => {
      const v = axes[i];
      return typeof v === 'number' && isFinite(v) && Math.abs(v) <= 1.05 ? clamp(v, -1, 1) : 0;
    };
    const trigger = (k: number, i: number, button: number) => {
      const v = axes[i];
      if (typeof v === 'number' && isFinite(v) && v !== 0 && Math.abs(v) <= 1.05) this.live[k] = true;
      return this.live[k] ? clamp((stick(i) + 1) / 2, 0, 1) : pressed[button] ? 1 : 0;
    };
    return { buttons, axes: [stick(a ? a.lx : 0), stick(a ? a.ly : 1), stick(a?.rx ?? -1), stick(a?.ry ?? -1),
      trigger(1, a?.lt ?? -1, L2), trigger(0, a?.rt ?? -1, R2)] };
  }

  /** a short line for the log: where its axes are */
  describe(): string {
    const a = this.at, ax = (i: number) => i >= 0 ? `${i}` : '-';
    if (!a) return `an 8BitDo controller (${this.face} buttons), its axes not read yet`;
    return `an 8BitDo controller (${this.face} buttons), sticks on axes ${a.lx} ${a.ly} and ${ax(a.rx)} ${ax(a.ry)}, ` +
      `triggers L2 ${ax(a.lt)} R2 ${ax(a.rt)} (or their buttons), D-pad ${a.dpad ? `axes ${a.dpad.join(' and ')}` :
      a.hat >= 0 ? `axis ${a.hat}` : 'none'}`;
  }
}
