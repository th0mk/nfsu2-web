// Valve's controllers read from the controller itself through WebHID, for the game's virtual pad (gamepad.ts): the
// Steam Controller of 2015 (on its cable or through its dongle), the Steam Controller of 2026 (on its cable, over
// Bluetooth or through its dongles) and a Steam Deck's own controls.
//
// None of them is a gamepad to the browsers.  Left alone each acts as a mouse and a keyboard ("lizard mode": a pad
// moves the pointer, the buttons are Enter, Esc and arrow keys), and its controls come in Valve's own reports on an
// interface of its own, which the Gamepad API never lists: the page saw no controller unless Steam made one of it
// (Steam Input's gamepad layout, a virtual Xbox pad).  Chrome and Edge open such an interface through WebHID once the
// player picked the controller in the browser's list (connectSteamPad, from a click: the Controls sheet's), and on
// later visits without asking (navigator.hid.getDevices).  While the game runs (useSteamPads: the PadPoller's) the
// page opens them; while the game also has the page visible and focused, lizard mode is off and each controller is a
// Gamepad with the standard mapping, at index 16 and up beside the browser's own (allPads), which gamepad.ts reads as
// it reads any other.  Without the focus, or once the game ends, lizard mode comes back, so the controller is a mouse
// on the desktop again: the 2015 controller is told so, the others bring it back themselves a few seconds after the
// page stops telling them to keep it off (their firmware's watchdog).
//
// The reports and the commands are SDL's (zlib licence: src/joystick/hidapi/SDL_hidapi_steam.c, SDL_hidapi_steamdeck.c,
// SDL_hidapi_steam_triton.c, steam/controller_structs.h and controller_constants.h), as are the buttons' places:
// Valve labels A, B, X and Y as Xbox does.  The 2015 controller has one stick: its right pad is the right stick (where
// the thumb touches it, as SDL has it), the left pad's click the D-pad, its grips extra buttons.  The Deck's and the
// 2026 controller's pads do nothing here; their back buttons and the "..." button come after the standard mapping's,
// for the page alone (the game's pad has none of them).  Rumble: the Deck's and the 2026 controller's motors (the 2015
// controller's pads only click, SDL leaves them be: no rumble).  Not read: the 2015 controller over Bluetooth (its
// reports come in pieces), gyros, the pads' touch.

/** a WebHID device, as much of it as this file uses (TypeScript's DOM types have none) */
interface HidDevice extends EventTarget {
  readonly opened: boolean;
  readonly vendorId: number;
  readonly productId: number;
  readonly productName: string;
  readonly collections: readonly { usagePage: number; usage: number }[];
  open(): Promise<void>;
  close(): Promise<void>;
  sendReport(reportId: number, data: BufferSource): Promise<void>;
  sendFeatureReport(reportId: number, data: BufferSource): Promise<void>;
  receiveFeatureReport(reportId: number): Promise<DataView>;
}
interface HidReportEvent extends Event { readonly reportId: number; readonly data: DataView }
interface HidConnectionEvent extends Event { readonly device: HidDevice }
interface Hid extends EventTarget {
  getDevices(): Promise<HidDevice[]>;
  requestDevice(o: { filters: { vendorId: number; productId?: number }[] }): Promise<HidDevice[]>;
}
const hid = (): Hid | null => (globalThis.navigator as unknown as { hid?: Hid } | undefined)?.hid ?? null;

const VALVE = 0x28de;
type Kind = 'd0g' | 'deck' | 'triton';
interface Product { kind: Kind; name: string; dongle?: boolean }
/** Valve's controllers this file reads, by USB product id (SDL's controller_list.h) */
export const STEAM_PRODUCTS: Readonly<Record<number, Product>> = {
  0x1102: { kind: 'd0g', name: 'Steam Controller' },                      // 2015, on its cable
  0x1142: { kind: 'd0g', name: 'Steam Controller', dongle: true },        // 2015, its dongle (up to 4)
  0x1205: { kind: 'deck', name: 'Steam Deck' },
  0x1302: { kind: 'triton', name: 'Steam Controller' },                   // 2026, on its cable
  0x1303: { kind: 'triton', name: 'Steam Controller' },                   // 2026, Bluetooth
  0x1304: { kind: 'triton', name: 'Steam Controller', dongle: true },     // 2026, its dongles
  0x1305: { kind: 'triton', name: 'Steam Controller', dongle: true },
};
const productOf = (d: HidDevice): Product | null => d.vendorId === VALVE ? STEAM_PRODUCTS[d.productId] ?? null : null;
const hex4 = (n: number) => n.toString(16).padStart(4, '0');
/** a controller's Gamepad.id, written as Chrome writes a gamepad's (gamepad.ts familyOf and wheel.ts usbIds read it) */
export const steamPadId = (productId: number) =>
  `${STEAM_PRODUCTS[productId]?.name ?? 'Steam Controller'} (STANDARD GAMEPAD Vendor: ${hex4(VALVE)} Product: ${hex4(productId)})`;

/** a button's place in Gamepad.buttons: the standard mapping's, then Valve's extras (the 2015 controller's grips are
 *  L4 and R4, the back buttons the others) */
export const SB = { A: 0, B: 1, X: 2, Y: 3, LB: 4, RB: 5, LT: 6, RT: 7, BACK: 8, START: 9, LS: 10, RS: 11, UP: 12, DOWN: 13,
  LEFT: 14, RIGHT: 15, HOME: 16, L4: 17, R4: 18, L5: 19, R5: 20, QAM: 21 } as const;
const NBUTTONS = 22;
/** a trigger counts as pressed past this (Chrome's threshold for an analog button) */
const PRESSED = 30 / 255;

/** a controller's state as the standard mapping has it: buttons as 1 << SB.*, the sticks -1..1 with +y down, the
 *  triggers 0..1 */
export interface SteamState { buttons: number; lx: number; ly: number; rx: number; ry: number; lt: number; rt: number }
/** what a report said: the state, or that a dongle's controller came ('on') or went ('off') */
export type SteamReport = SteamState | 'on' | 'off' | null;

const axis = (v: number) => v <= -32767 ? -1 : v >= 32767 ? 1 : v / 32767;
const unit = (v: number) => v <= 0 ? 0 : v >= 1 ? 1 : v;
/** bits of a controller's report -> 1 << SB.* */
const mapBits = (m: number, table: readonly (readonly [number, number])[]) => {
  let b = 0;
  for (const [bit, to] of table) if (m & bit) b |= 1 << to;
  return b;
};
/** a trigger's button: pressed past PRESSED or clicked all the way */
const triggerBits = (lt: number, rt: number, lclick: boolean, rclick: boolean) =>
  (lclick || lt > PRESSED ? 1 << SB.LT : 0) | (rclick || rt > PRESSED ? 1 << SB.RT : 0);

// ---- the Steam Controller of 2015 ("D0G"): 64-byte reports, no report id

const D0G = {
  RT: 0x1, LT: 0x2, RB: 0x4, LB: 0x8, Y: 0x10, B: 0x20, X: 0x40, A: 0x80, UP: 0x100, RIGHT: 0x200, LEFT: 0x400, DOWN: 0x800,
  MENU: 0x1000, STEAM: 0x2000, ESCAPE: 0x4000, GRIP_L: 0x8000, GRIP_R: 0x10000, LPAD_CLICK: 0x20000, RPAD_CLICK: 0x40000,
  LPAD_TOUCH: 0x80000, RPAD_TOUCH: 0x100000, STICK_CLICK: 0x400000, LPAD_AND_STICK: 0x800000,
} as const;
const D0G_BUTTONS = [[D0G.A, SB.A], [D0G.B, SB.B], [D0G.X, SB.X], [D0G.Y, SB.Y], [D0G.LB, SB.LB], [D0G.RB, SB.RB],
  [D0G.MENU, SB.BACK], [D0G.ESCAPE, SB.START], [D0G.STEAM, SB.HOME], [D0G.STICK_CLICK, SB.LS], [D0G.RPAD_CLICK, SB.RS],
  [D0G.UP, SB.UP], [D0G.DOWN, SB.DOWN], [D0G.LEFT, SB.LEFT], [D0G.RIGHT, SB.RIGHT], [D0G.GRIP_L, SB.L4],
  [D0G.GRIP_R, SB.R4]] as const;
/** the pads are mounted turned: SDL turns their positions back by 15 degrees (the left one the other way), and moves a
 *  touched one's 1000 up and right */
const TURN = 0.261799;
function turned(x: number, y: number, angle: number, touched: boolean): [number, number] {
  const c = Math.cos(angle), s = Math.sin(angle), off = touched ? 1000 : 0;
  const clamp16 = (v: number) => Math.max(-32768, Math.min(32767, Math.trunc(v)));
  return [clamp16(c * x - s * y + off), clamp16(s * x + c * y + off)];
}
/** a clicked left pad's D-pad direction from where it was clicked (the controller gives it as buttons; this is for a
 *  report that does not), +y up: eight ways, none near the middle */
function padDirs(x: number, y: number): number {
  const r = Math.hypot(x, y);
  if (r < 8000) return 0;
  return (y > 0.38 * r ? 1 << SB.UP : 0) | (y < -0.38 * r ? 1 << SB.DOWN : 0) |
    (x < -0.38 * r ? 1 << SB.LEFT : 0) | (x > 0.38 * r ? 1 << SB.RIGHT : 0);
}

/**
 * A 2015 controller's reports, read as SDL's FormatStatePacketUntilGyro reads them.  The stick and the left pad share
 * their fields: a report carries the pad's position while a thumb is on it, else the stick's, and while both are in use
 * the reports take turns (LPAD_AND_STICK), so each keeps where it was last.  An older controller sends the pad's click
 * for the stick's when nobody touches the pad.  The triggers' analog is 7 bits doubled, full at 26000.
 */
export class D0GReader {
  private stick: [number, number] = [0, 0];
  private pad: [number, number] = [0, 0];
  private packet = -1;
  private last: SteamState | null = null;

  read(d: DataView): SteamReport {
    if (d.byteLength < 24 || d.getUint16(0, true) !== 1) return null;
    const type = d.getUint8(2);
    if (type === 3) return d.getUint8(3) >= 1 ? d.getUint8(4) === 1 ? 'off' : 'on' : null;   // a dongle's: the radio link
    if (type !== 1) return null;
    const packet = d.getUint32(4, true);
    if (packet === this.packet && this.last) return this.last;        // nothing new
    this.packet = packet;
    let m = d.getUint8(8) | d.getUint8(9) << 8 | d.getUint8(10) << 16;
    const x = d.getInt16(16, true), y = d.getInt16(18, true);
    let sx = 0, sy = 0, px = 0, py = 0;
    if (m & D0G.LPAD_TOUCH) {
      [px, py] = this.pad = [x, y];
      if (m & D0G.LPAD_AND_STICK) [sx, sy] = this.stick; else this.stick = [0, 0];
    } else {
      [sx, sy] = this.stick = [x, y];
      if (m & D0G.LPAD_AND_STICK) [px, py] = this.pad;
      else {
        this.pad = [0, 0];
        if (m & D0G.LPAD_CLICK) m = (m & ~D0G.LPAD_CLICK) | D0G.STICK_CLICK;
      }
    }
    if (m & D0G.LPAD_AND_STICK) m |= D0G.LPAD_TOUCH;
    let buttons = mapBits(m, D0G_BUTTONS);
    if ((m & D0G.LPAD_CLICK) && (m & D0G.LPAD_TOUCH) && !(m & (D0G.UP | D0G.DOWN | D0G.LEFT | D0G.RIGHT))) {
      const [tx, ty] = turned(px, py, -TURN, false);
      buttons |= padDirs(tx, ty);
    }
    const [rx, ry] = turned(d.getInt16(20, true), d.getInt16(22, true), TURN, (m & D0G.RPAD_TOUCH) !== 0);
    const trig = (n: number) => unit(((n << 7) | n) / 26000);
    const lt = m & D0G.LT ? 1 : trig(d.getUint8(11)), rt = m & D0G.RT ? 1 : trig(d.getUint8(12));
    buttons |= triggerBits(lt, rt, (m & D0G.LT) !== 0, (m & D0G.RT) !== 0);
    return this.last = { buttons, lx: axis(sx), ly: -axis(sy), rx: axis(rx), ry: -axis(ry), lt, rt };
  }
}

// ---- the Steam Deck ("Jupiter"): 64-byte reports, no report id, of type 9

const DECK_LOW = [[0x80, SB.A], [0x20, SB.B], [0x40, SB.X], [0x10, SB.Y], [0x8, SB.LB], [0x4, SB.RB], [0x1000, SB.BACK],
  [0x4000, SB.START], [0x2000, SB.HOME], [0x400000, SB.LS], [0x4000000, SB.RS], [0x100, SB.UP], [0x800, SB.DOWN],
  [0x400, SB.LEFT], [0x200, SB.RIGHT], [0x8000, SB.L5], [0x10000, SB.R5]] as const;
const DECK_HIGH = [[0x200, SB.L4], [0x400, SB.R4], [0x40000, SB.QAM]] as const;

/** a Steam Deck's report: buttons in two words, the triggers 0..32767, the sticks +y up */
export function readDeck(d: DataView): SteamReport {
  if (d.byteLength < 56 || d.getUint16(0, true) !== 1 || d.getUint8(2) !== 9) return null;
  const low = d.getUint32(8, true), high = d.getUint32(12, true);
  const lt = unit(d.getUint16(44, true) / 32767), rt = unit(d.getUint16(46, true) / 32767);
  const buttons = mapBits(low, DECK_LOW) | mapBits(high, DECK_HIGH) | triggerBits(lt, rt, (low & 2) !== 0, (low & 1) !== 0);
  return { buttons, lx: axis(d.getInt16(48, true)), ly: -axis(d.getInt16(50, true)),
    rx: axis(d.getInt16(52, true)), ry: -axis(d.getInt16(54, true)), lt, rt };
}

// ---- the Steam Controller of 2026 ("Triton"): reports with ids, 0x42/0x45/0x47 its state, 0x46/0x79 a dongle's link

// SDL reads the bit it calls MENU as Back and VIEW as Start
const TRITON_BUTTONS = [[0x1, SB.A], [0x2, SB.B], [0x4, SB.X], [0x8, SB.Y], [0x80000, SB.LB], [0x200, SB.RB],
  [0x4000, SB.BACK], [0x40, SB.START], [0x10000, SB.HOME], [0x8000, SB.LS], [0x20, SB.RS], [0x2000, SB.UP],
  [0x400, SB.DOWN], [0x1000, SB.LEFT], [0x800, SB.RIGHT], [0x20000, SB.L4], [0x80, SB.R4], [0x40000, SB.L5],
  [0x100, SB.R5], [0x10, SB.QAM]] as const;

/** a 2026 controller's report (data without its id): a sequence number, the buttons, the triggers 0..32767, the
 *  sticks +y up */
export function readTriton(reportId: number, d: DataView): SteamReport {
  if (reportId === 0x79 || reportId === 0x46) {
    const s = d.byteLength ? d.getUint8(0) : 0;
    return s === 1 ? 'off' : s === 2 ? 'on' : null;
  }
  if ((reportId !== 0x42 && reportId !== 0x45 && reportId !== 0x47) || d.byteLength < 17) return null;
  const m = d.getUint32(1, true);
  const lt = unit(d.getInt16(5, true) / 32767), rt = unit(d.getInt16(7, true) / 32767);
  const buttons = mapBits(m, TRITON_BUTTONS) | triggerBits(lt, rt, (m & 0x8000000) !== 0, (m & 0x800000) !== 0);
  return { buttons, lx: axis(d.getInt16(9, true)), ly: -axis(d.getInt16(11, true)),
    rx: axis(d.getInt16(13, true)), ry: -axis(d.getInt16(15, true)), lt, rt };
}

// ---- the commands: feature reports (64 bytes; the 2026 controller's report 1, 63) and its output report 0x80

const ID_CLEAR_DIGITAL_MAPPINGS = 0x81, ID_SET_DEFAULT_DIGITAL_MAPPINGS = 0x85, ID_SET_SETTINGS_VALUES = 0x87;
const ID_LOAD_DEFAULT_SETTINGS = 0x8e, ID_DONGLE_GET_WIRELESS_STATE = 0xb4, ID_TRIGGER_RUMBLE_CMD = 0xeb;
const SETTING_LEFT_TRACKPAD_MODE = 7, SETTING_RIGHT_TRACKPAD_MODE = 8, SETTING_LIZARD_MODE = 9;
const SETTING_SMOOTH_ABSOLUTE_MOUSE = 24, SETTING_WIRELESS_PACKET_VERSION = 49;
const SETTING_LEFT_TRACKPAD_CLICK_PRESSURE = 52, SETTING_RIGHT_TRACKPAD_CLICK_PRESSURE = 53;
const TRACKPAD_ABSOLUTE_MOUSE = 0, TRACKPAD_NONE = 7;

/** a message: its type, its payload's length, the payload */
const msg = (type: number, payload: number[] = []) => [type, payload.length, ...payload];
/** ID_SET_SETTINGS_VALUES with [setting, value] pairs (a value is 16 bits) */
const settings = (...s: [number, number][]) => msg(ID_SET_SETTINGS_VALUES, s.flatMap(([n, v]) => [n, v & 0xff, v >> 8 & 0xff]));

/** the feature reports that turn lizard mode off (SDL's ResetSteamController, DisableDeckLizardMode,
 *  DisableSteamTritonLizardMode), and for the 2015 controller those that bring it back (CloseSteamController) */
export const LIZARD = {
  d0gOff: [msg(ID_CLEAR_DIGITAL_MAPPINGS), msg(ID_LOAD_DEFAULT_SETTINGS), settings([SETTING_WIRELESS_PACKET_VERSION, 2],
    [SETTING_LEFT_TRACKPAD_MODE, TRACKPAD_NONE], [SETTING_RIGHT_TRACKPAD_MODE, TRACKPAD_NONE], [SETTING_SMOOTH_ABSOLUTE_MOUSE, 0])],
  d0gOn: [msg(ID_SET_DEFAULT_DIGITAL_MAPPINGS), msg(ID_LOAD_DEFAULT_SETTINGS),
    settings([SETTING_RIGHT_TRACKPAD_MODE, TRACKPAD_ABSOLUTE_MOUSE])],
  deckOff: [msg(ID_CLEAR_DIGITAL_MAPPINGS), settings([SETTING_SMOOTH_ABSOLUTE_MOUSE, 0],
    [SETTING_LEFT_TRACKPAD_MODE, TRACKPAD_NONE], [SETTING_RIGHT_TRACKPAD_MODE, TRACKPAD_NONE],
    [SETTING_LEFT_TRACKPAD_CLICK_PRESSURE, 0xffff], [SETTING_RIGHT_TRACKPAD_CLICK_PRESSURE, 0xffff])],
  // the Deck's watchdog brings lizard mode back unless this comes again (SDL: FeedDeckLizardWatchdog)
  deckKeep: [msg(ID_CLEAR_DIGITAL_MAPPINGS), settings([SETTING_RIGHT_TRACKPAD_MODE, TRACKPAD_NONE])],
  tritonOff: [settings([SETTING_LIZARD_MODE, 0])],
  tritonOn: [settings([SETTING_LIZARD_MODE, 1])],
};
/** how often the Deck and the 2026 controller are told again (ms): SDL's 3 s for the 2026 controller; the Deck's
 *  every 200 reads, about 3 s at 60 frames a second */
const KEEP_MS = { deck: 1000, triton: 2500 } as const;
/** the 2026 controller stops its motors on its own after about 50 ms: told again this often while they run */
const TRITON_RUMBLE_MS = 40;

let log: (s: string) => void = () => {};

/** a Gamepad.buttons entry */
interface Button { pressed: boolean; touched: boolean; value: number }

/** One controller: a HID interface of Valve's (a dongle has one per controller it links), and the Gamepad the page sees
 *  for it once it reported (connected). */
class SteamPad {
  readonly gp: Gamepad;
  readonly product: Product;
  /** a state report came, and a dongle did not say its controller went since */
  connected = false;
  /** the page keeps lizard mode off now */
  private lizardOff = false;
  private keep = 0;
  private reader = new D0GReader();
  private queue: Promise<unknown> = Promise.resolve();
  private firstReport = true;
  private buttons: Button[];
  private axes: number[];
  // rumble: the motors' levels (0..65535), until when, the timer that renews or stops them
  private motors: [number, number] = [0, 0];
  private motorsUntil = 0;
  private motorTimer = 0;

  constructor(readonly dev: HidDevice, index: number) {
    this.product = productOf(dev)!;
    this.buttons = Array.from({ length: NBUTTONS }, () => ({ pressed: false, touched: false, value: 0 }));
    this.axes = [0, 0, 0, 0];
    const self = this;
    const actuator = this.product.kind === 'd0g' ? null : {
      type: 'dual-rumble', effects: ['dual-rumble'],
      playEffect(_type: string, p: { duration?: number; strongMagnitude?: number; weakMagnitude?: number } = {}) {
        self.rumble(p.strongMagnitude ?? 0, p.weakMagnitude ?? 0, p.duration ?? 0);
        return Promise.resolve('complete');
      },
      reset() { self.rumble(0, 0, 0); return Promise.resolve('complete'); },
    };
    this.gp = { id: steamPadId(dev.productId), index, mapping: 'standard', connected: false, timestamp: 0,
      axes: this.axes, buttons: this.buttons, vibrationActuator: actuator, hapticActuators: [] } as unknown as Gamepad;
  }

  get name() { return `${this.product.name} (${hex4(VALVE)}:${hex4(this.dev.productId)})`; }

  /** a report from the interface */
  onReport = (e: Event) => {
    const { reportId, data } = e as HidReportEvent;
    if (this.firstReport) {
      this.firstReport = false;
      const bytes = Array.from(new Uint8Array(data.buffer, data.byteOffset, Math.min(data.byteLength, 32)), b => b.toString(16).padStart(2, '0'));
      log(`[pad] ${this.name}: first report, id ${reportId}, ${data.byteLength} bytes: ${bytes.join(' ')}`);
    }
    const r = this.product.kind === 'd0g' ? reportId === 0 ? this.reader.read(data) : null
      : this.product.kind === 'deck' ? reportId === 0 ? readDeck(data) : null : readTriton(reportId, data);
    if (r === 'off') { this.gone('its controller went away'); return; }
    if (r === null || r === 'on') return;
    if (!this.connected) {
      this.connected = true;
      (this.gp as unknown as { connected: boolean }).connected = true;
      log(`[pad] ${this.name} is controller ${this.gp.index} (WebHID)`);
      announce('gamepadconnected', this.gp);
      this.lizard(active);
    }
    this.show(r);
  };

  private show(s: SteamState) {
    for (let i = 0; i < NBUTTONS; i++) {
      const b = this.buttons[i], on = (s.buttons >> i & 1) === 1;
      b.pressed = on; b.touched = on;
      b.value = i === SB.LT ? s.lt : i === SB.RT ? s.rt : on ? 1 : 0;
    }
    this.axes[0] = s.lx; this.axes[1] = s.ly; this.axes[2] = s.rx; this.axes[3] = s.ry;
    (this.gp as unknown as { timestamp: number }).timestamp = performance.now();
  }

  /** the controller is no more (a dongle's went, the interface went or closed); there: it is still there to be told
   *  to be a mouse again (the page closes it) */
  gone(why: string, there = false) {
    if (there) this.lizard(false);
    else { this.lizardOff = false; clearInterval(this.keep); this.keep = 0; }
    this.rumble(0, 0, 0);
    if (!this.connected) return;
    this.connected = false;
    (this.gp as unknown as { connected: boolean }).connected = false;
    this.show({ buttons: 0, lx: 0, ly: 0, rx: 0, ry: 0, lt: 0, rt: 0 });
    log(`[pad] ${this.name}: ${why}`);
    announce('gamepaddisconnected', this.gp);
  }

  /** feature reports one after another (a failed one tried again: SDL's dongle needs it), then for the Deck's settings
   *  a read (SDL drops the report a change of settings leaves) */
  private send(reports: number[][], what: string, readBack = false) {
    const id = this.product.kind === 'triton' ? 1 : 0, size = id ? 63 : 64;
    this.queue = this.queue.then(async () => {
      for (const r of reports) {
        const buf = new Uint8Array(size);
        buf.set(r);
        for (let tries = 0; ; tries++) {
          try { await this.dev.sendFeatureReport(id, buf); break; }
          catch (err) {
            if (tries >= 4 || !this.dev.opened) { log(`[pad] ${this.name}: ${what}: ${(err as Error)?.message ?? err}`); return; }
            await new Promise(res => setTimeout(res, 2));
          }
        }
      }
      if (readBack) await this.dev.receiveFeatureReport(0).catch(() => {});
    });
    return this.queue;
  }

  /** a dongle asked whether its controller is there (the 2015 one's answers with a link report): on its interfaces with
   *  Valve's reports (vendor-defined), not a keyboard's or a mouse's */
  ask() {
    if (this.product.kind === 'd0g' && this.product.dongle && this.dev.collections.some(c => c.usagePage >= 0xff00))
      void this.send([msg(ID_DONGLE_GET_WIRELESS_STATE)], 'the dongle\'s question');
  }

  /** lizard mode off (on: while the game has the page) or back */
  lizard(off: boolean) {
    off &&= this.connected && this.dev.opened;
    if (off === this.lizardOff) return;
    this.lizardOff = off;
    clearInterval(this.keep); this.keep = 0;
    const k = this.product.kind;
    if (off) {
      void this.send(k === 'd0g' ? LIZARD.d0gOff : k === 'deck' ? LIZARD.deckOff : LIZARD.tritonOff, 'lizard mode off', k === 'deck');
      if (k !== 'd0g') this.keep = setInterval(() => void this.send(k === 'deck' ? LIZARD.deckKeep : LIZARD.tritonOff,
        'lizard mode kept off', k === 'deck'), KEEP_MS[k]) as unknown as number;
    } else if (this.dev.opened && k !== 'deck') {
      void this.send(k === 'd0g' ? LIZARD.d0gOn : LIZARD.tritonOn, 'lizard mode back');
    }
  }

  /** the game's motors, strong (low frequency) and weak (high) 0..1, for ms (0: stop) */
  rumble(strong: number, weak: number, ms: number) {
    const k = this.product.kind;
    if (k === 'd0g') return;
    const to: [number, number] = ms > 0 && this.lizardOff ? [Math.round(unit(strong) * 65535), Math.round(unit(weak) * 65535)] : [0, 0];
    const was = this.motors, now = performance.now();
    this.motors = to;
    this.motorsUntil = to[0] || to[1] ? now + ms : 0;
    if (to[0] !== was[0] || to[1] !== was[1]) this.motorsOut();
    if ((to[0] || to[1]) && !this.motorTimer) {
      this.motorTimer = setInterval(() => {
        if (performance.now() >= this.motorsUntil) {
          this.motors = [0, 0];
          clearInterval(this.motorTimer); this.motorTimer = 0;
          this.motorsOut();
        } else if (k === 'triton') this.motorsOut();
      }, TRITON_RUMBLE_MS) as unknown as number;
    } else if (!to[0] && !to[1] && this.motorTimer) { clearInterval(this.motorTimer); this.motorTimer = 0; }
  }

  private motorsOut() {
    if (!this.dev.opened) return;
    const [l, r] = this.motors;
    if (this.product.kind === 'deck') {
      void this.send([[ID_TRIGGER_RUMBLE_CMD, 0, 0, 0, 0, l & 0xff, l >> 8, r & 0xff, r >> 8, 2, 0]], 'rumble');
    } else {
      // output report 0x80: type, intensity (16 bits), the left motor's speed (16) and gain, the right one's
      const out = new Uint8Array([0, 0, 0, l & 0xff, l >> 8, 0, r & 0xff, r >> 8, 0]);
      this.queue = this.queue.then(() => this.dev.sendReport(0x80, out)).catch(() => {});
    }
  }

  /** stop for good: lizard mode back, then the interface closed */
  close() {
    this.gone('closed', true);
    this.dev.removeEventListener('inputreport', this.onReport);
    this.queue = this.queue.then(() => this.dev.opened ? this.dev.close() : undefined).catch(() => {});
  }
}

/** a gamepad event for one of these, as the browser sends one for its own (the listeners read e.gamepad) */
function announce(type: 'gamepadconnected' | 'gamepaddisconnected', gp: Gamepad) {
  if (typeof dispatchEvent !== 'function') return;
  const e = new Event(type);
  Object.defineProperty(e, 'gamepad', { value: gp });
  dispatchEvent(e);
}

const pads = new Map<HidDevice, SteamPad>();
let users = 0;
/** the game runs and has the page, visible and focused: lizard mode off */
let active = false;
const FIRST_INDEX = 16;

function update() {
  const on = users > 0 && typeof document !== 'undefined' && document.visibilityState === 'visible' && document.hasFocus();
  if (on === active) return;
  active = on;
  for (const p of pads.values()) p.lizard(on);
}

/** open a controller the player picked (in this visit or before), once */
async function add(dev: HidDevice) {
  if (!productOf(dev) || pads.has(dev) || !users) return;
  const used = new Set([...pads.values()].map(p => p.gp.index));
  let index = FIRST_INDEX;
  while (used.has(index)) index++;
  const p = new SteamPad(dev, index);
  pads.set(dev, p);
  try { if (!dev.opened) await dev.open(); }
  catch (err) {
    log(`[pad] ${p.name}: cannot open it: ${(err as Error)?.message ?? err}`);
    pads.delete(dev);
    return;
  }
  if (pads.get(dev) !== p) return;
  log(`[pad] ${p.name}: opened, ${dev.collections.map(c => `${hex4(c.usagePage)}:${hex4(c.usage)}`).join(' ') || 'no collections'}`);
  dev.addEventListener('inputreport', p.onReport);
  p.ask();
}

function remove(dev: HidDevice) {
  const p = pads.get(dev);
  if (!p) return;
  pads.delete(dev);
  p.close();
}

const onHidConnect = (e: Event) => void add((e as HidConnectionEvent).device);
const onHidDisconnect = (e: Event) => {
  const p = pads.get((e as HidConnectionEvent).device);
  if (p) { p.gone('unplugged'); pads.delete(p.dev); }
};
/** the tab closing: the 2015 controller told to be a mouse again (the others do it themselves) */
const onPageHide = () => { for (const p of pads.values()) p.lizard(false); };

/**
 * While the game runs (the PadPoller's, until the function it returns is called): the controllers picked before
 * opened, those plugged in or picked later too, lizard mode off while the page is visible and focused.  logf: the
 * page's log.
 */
export function useSteamPads(logf: (s: string) => void): () => void {
  const h = hid();
  if (!h) return () => {};
  log = logf;
  if (users++ === 0) {
    h.addEventListener('connect', onHidConnect);
    h.addEventListener('disconnect', onHidDisconnect);
    document.addEventListener('visibilitychange', update);
    addEventListener('focus', update);
    addEventListener('blur', update);
    addEventListener('pagehide', onPageHide);
    h.getDevices().then(ds => { for (const d of ds) void add(d); }, err => log(`[pad] WebHID: ${(err as Error)?.message ?? err}`));
    update();
  }
  let done = false;
  return () => {
    if (done) return;
    done = true;
    if (--users > 0) return;
    h.removeEventListener('connect', onHidConnect);
    h.removeEventListener('disconnect', onHidDisconnect);
    document.removeEventListener('visibilitychange', update);
    removeEventListener('focus', update);
    removeEventListener('blur', update);
    removeEventListener('pagehide', onPageHide);
    active = false;
    for (const d of [...pads.keys()]) remove(d);
  };
}

/** The browser's controllers (navigator.getGamepads) with these beside them, each at its index (the places between
 *  are null): what every reader of controllers in the page reads. */
export function allPads(): (Gamepad | null)[] {
  let own: (Gamepad | null)[] = [];
  try { own = typeof navigator.getGamepads === 'function' ? Array.from(navigator.getGamepads()) : []; } catch { /* none */ }
  if (!pads.size) return own;
  for (const p of pads.values()) {
    if (!p.connected) continue;
    while (own.length < p.gp.index) own.push(null);
    own[p.gp.index] = p.gp;
  }
  return own;
}

/** whether this browser can read them (WebHID: Chrome, Edge and Opera on a computer) */
export const steamPadsPossible = () => !!hid();

const FILTERS = Object.keys(STEAM_PRODUCTS).map(p => ({ vendorId: VALVE, productId: +p }));

/** The browser's list of these controllers to pick from (only from a click: Chrome asks for one).  The one picked
 *  works at once while a game runs, else from the next Play, and on later visits without asking.  Resolves to the
 *  names of those picked (none: the list closed). */
export async function connectSteamPad(): Promise<string[]> {
  const h = hid();
  if (!h) return [];
  const got = await h.requestDevice({ filters: FILTERS });
  for (const d of got) void add(d);
  return [...new Set(got.map(d => productOf(d)?.name).filter((n): n is string => !!n))];
}

/** the names of these controllers picked in this browser before (they open on Play) */
export async function steamPadsPicked(): Promise<string[]> {
  const h = hid();
  if (!h) return [];
  try { return [...new Set((await h.getDevices()).map(d => productOf(d)?.name).filter((n): n is string => !!n))]; }
  catch { return []; }
}
