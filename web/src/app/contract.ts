// The contract between the app shell and its subsystems (renderer, audio,
// input, network), as the shell calls them.  subsystems.ts adapts the real
// modules (web/src/gpu, web/src/audio, web/src/input, web/src/net) to these
// shapes.  See web/src/host/abi.ts for the shared-memory side (nfsu2_register
// kinds).
import type { GameProcess } from '../host/process';
import type { PhotoLook } from '../gpu/photo-picture';
import type { PadPage, TouchPad } from '../input/gamepad';
import type { TextEntry } from '../input/index';
import type { NetQueue, NetRaces, NetRemoved, NetRename } from '../net';

/** Given to every subsystem. */
export interface CommonOptions {
  /** the resolved process environment of this launch (settings.ini, defaults, page address) */
  env: Readonly<Record<string, string>>;
  /** a line for the game log (Download log) and the console */
  log(line: string): void;
}

export interface RendererOptions extends CommonOptions {
  /** NFSU2_WIDTH/HEIGHT the game was started with: the canvas size in device pixels at Play */
  width: number; height: number;
  /** measured display refresh rate (rAF), Hz */
  displayHz: number;
  /** tests: a URL prefix the autotest's frame dumps are PUT to (the server's /artifacts/) */
  artifacts?: string;
  /** tests: scale of those dumps */
  dumpScale?: number;
  /** the GPU device was lost while the game ran (a graphics driver reset, the browser's graphics process
   *  crashed): nothing is drawn from then on.  why: the browser's reason and message (the log's "[gpu] device lost" line) */
  onLost?(why: string): void;
  /** every frame drawn, in the same task: quick chat's places over the cars in it, n of { id, x, y, scale } (ids in
   *  ids[4i], the rest in m[4i + 1..3]; x, y parts of the game's image; the arrays are reused: read them at once) */
  onMarks?(n: number, m: Float32Array, ids: Uint32Array): void;
}

export interface AudioOptions extends CommonOptions {
  /**
   * Created and resumed inside the Play click (a user gesture), with the
   * latencyHint from settings (audio_latency).  The audio subsystem plays into
   * it and closes it on detach.
   */
  context: AudioContext | null;
}

export interface InputOptions extends CommonOptions {
  /** the element that is (or will be) full screen: the canvas's parent */
  stage: HTMLElement;
  /** where the game's image is inside the canvas, CSS px (the renderer's aspect-fit) */
  viewRect?: () => { x: number; y: number; w: number; h: number };
  /** what Cmd-F does on a Mac: the shell's full screen (stage + Keyboard Lock) */
  toggleFullscreen?: () => void;
  /** the shell's own use of the controller: its menu (Home) and dialogs */
  padPage?: PadPage;
}

export interface NetOptions extends CommonOptions {
  /** the name other browsers see on the server's LAN */
  name: string;
  /** a line for the player from the game (a friend joined, left, went into a race) */
  notice?: (text: string, who?: string) => void;
  /** free roam: the race lobby's panel, its state as JSON from the game ("{}": none) */
  lobby?: (json: string) => void;
  /** free roam: quick chat, a phrase said (by a friend or the player), as JSON from the game */
  say?: (json: string) => void;
  /** free roam: the player's car and another's touched front bumpers for 3 s, or parted, as JSON from the game */
  bump?: (json: string) => void;
  /** free roam: a drift battle's scoreboard, as JSON from the game ("{}": none; drift.ts) */
  battle?: (json: string) => void;
  /** free roam: a friend message from the server */
  friend?: (m: Record<string, unknown>) => void;
  /** the server's LAN took this page back after the link was lost (a server restart, as for a deploy) */
  back?: () => void;
  /** the server is full: this page's place in line for one (null: not in line) */
  queue?: (q: NetQueue | null) => void;
  /** this browser's own id (page-id.ts): the operator's bans name a page by it; and the one its other store held when
   *  that was another */
  id?: string;
  was?: string[];
  /** the token of the keyphrase this page plays with (claim.ts): whose claimed names it may play under */
  claim?: string;
  /** free roam: the names of the players this one blocks: the server's hub links none of them with it */
  block?: string[];
  /** the operator removed this player from playing with others (a kick or a ban) */
  removed?: (r: NetRemoved) => void;
  /** the operator's race ban: this player plays on with others but races none of them, until when and why (null: it
   *  races again) */
  races?: (r: NetRaces | null) => void;
  /** the operator took this player's name away: the page asks for another (NetSubsystem.rename) */
  rename?: (r: NetRename) => void;
}

/** What an attach call returns.  Everything but detach is optional. */
export interface Subsystem {
  /** stop for good: remove listeners, close contexts, drop views of the game's memory;
   *  called when the game exits or is stopped (or at once when it attaches too late) */
  detach(): void;
  /** counters for the detailed frame-rate overlay, e.g. { 'gpu ms': 2.1 } */
  stats?(): Record<string, number>;
}

export interface RendererSubsystem extends Subsystem {
  /** the game's image inside the canvas, in canvas pixels */
  imageRect(): { x: number; y: number; w: number; h: number };
  /** tests, F12: the next frame as PNG (also uploaded as <name>.png with RendererOptions.artifacts); mark: the
   *  site's address in its corner */
  dump?(name: string, mark?: boolean): Promise<Blob>;
  /** photo mode's picture as PNG (gpu/photo-picture.ts: once the still picture has all its frames, with its look and
   *  crop); dump()'s with photo mode off */
  photo?(name: string, mark?: boolean): Promise<Blob>;
  /** photo mode's look and crop, shown from the next frame on and kept by photo() */
  photoLook?(look: PhotoLook): void;
  /** photo mode's still picture so far: frames in it, frames it will have (0: none coming) */
  photoProgress?(): { n: number; target: number };
  /** each frame's time from its submit until the GPU had done it, ms, since the last call (rendercheck.ts) */
  doneMs?(): number[];
  /** the game's textures on the GPU now, bytes, those unpacked from DXT and those DXT1 as ASTC (the log's copy,
   *  lastgame.ts) */
  textures?(): { bytes: number; unpacked: number; astc: number };
}

export interface AudioSubsystem extends Subsystem {
  /** the AudioContext's state: 'running' once sound can play */
  state(): string;
}

export interface InputSubsystem extends Subsystem {
  /** ask the game to quit through its own path (WM_CLOSE, lifecycle_request_quit).  A request
   *  made before the game's input is up is sent when it is.  false: it cannot reach the game */
  requestQuit?(): boolean;
  /** whether requestQuit() can (still) reach the game (default: whether it exists) */
  canQuit?(): boolean;
  /** Download log: the game's line on its last frame (and stall report), then "[watchdog] log download ready".
   *  false: it cannot reach the game */
  logReport?(): boolean;
  /** a text message of ours for the game's phone (runtime/sms.c), by its number; false: it cannot reach the game */
  sms?(id: number): boolean;
  /** photo mode (runtime/photo.c, photo.ts): what (its PH_* number) and its values; false: it cannot reach the game */
  photo?(what: number, a?: number, b?: number): boolean;
  /** a new name for free roam and Online mode, now (the operator took the old one away); false: it cannot reach the
   *  game */
  name?(name: string): boolean;
  /** every key and button the game holds goes up (photo mode takes the keys) */
  releaseAll?(): void;
  /** performance.now() of the player's last key, mouse or controller input; null: the page cannot tell
   *  (the game's own driver plays an input script) */
  lastInput?(): number | null;
  /** the touch controls (touch.ts): a key pressed or let go for the player, by KeyboardEvent.code */
  key?(code: string, down: boolean): void;
  /** the touch controls' virtual controller, merged with a real one (null: none) */
  touchPad?(t: TouchPad | null): void;
  /** the touch controls' level horizon: the phone's tilt the game's 3D view turns against, radians, + = clockwise */
  viewRoll?(rad: number): void;
  /** the menus' small buttons' size, 1 = the game's own (touch.ts: the Menu button size): the scroller arrows', and
   *  with bar the help bar's too (else it keeps the game's own) */
  menuScale?(s: number, bar: boolean): void;
  /** performance.now() of a real controller's last button press (0: none) */
  padPressed?(): number;
  /** the game's text entry while it is up (a new profile's name, the LAN name), else null */
  textEntry?(): TextEntry | null;
  /** characters typed with no key (a phone's keyboard, textentry.ts), cp1252: WM_CHAR each, 8 deletes, 13 accepts */
  text?(chars: number[]): void;
}

export interface NetSubsystem extends Subsystem {
  /** this browser's address on the server's LAN, once connected */
  vip(): string | null;
  /** the other browsers on the server's LAN */
  peers(): string[];
  /** whether the game holds a place on the server's LAN, or is getting one or waits in line for one (free roam, the
   *  LAN and Online screens) */
  holding?(): boolean;
  /** the player is away: the place goes back, and stays free until away(false) */
  away?(on: boolean): void;
  /** the player stops waiting in line for a place (a full server): off the server's LAN for the rest of this game */
  offline?(): void;
  /** the player picked another name after the operator took its name away: back on the server's LAN under it */
  rename?(name: string): void;
  /** free roam's solid cars, switched while the game runs (the page menu), as roam_collide has them */
  solid?(collide: 'on' | 'off' | 'none'): void;
  /** quick chat: the player says phrase p (quickchat.ts; race: in a race between players); false: the game has no free
   *  roam (race: no LAN play) */
  say?(p: number, race?: boolean): boolean;
  /** free roam's friends: a message to the server (friends.ts); false: not connected now */
  friend?(m: Record<string, unknown>): boolean;
  /** free roam's crews, changed while the game runs (the page menu): their hashes, in the player's order, and their
   *  colours (0x00rrggbb) */
  crews?(hashes: readonly number[], rgbs: readonly number[]): void;
  /** free roam's room, changed while the game runs (an invite link): true when this game takes it now */
  room?(name: string): boolean;
}

export type AttachRenderer = (proc: GameProcess, canvas: HTMLCanvasElement, opts: RendererOptions) => RendererSubsystem | Promise<RendererSubsystem>;
export type AttachAudio = (proc: GameProcess, opts: AudioOptions) => AudioSubsystem | Promise<AudioSubsystem>;
export type AttachInput = (proc: GameProcess, element: HTMLElement, opts: InputOptions) => InputSubsystem | Promise<InputSubsystem>;
export type AttachNet = (proc: GameProcess, opts: NetOptions) => NetSubsystem | Promise<NetSubsystem>;
