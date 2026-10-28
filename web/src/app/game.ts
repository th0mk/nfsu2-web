// One run of the game: the process (web/src/host/process.ts) plus the
// subsystems attached to it, and its end.  The wasm runtime is not
// re-enterable, so every Play starts a fresh process from the compiled
// module and every exit terminates all of its workers.

import { startProcess, type GameProcess } from '../host/process';
import { N } from './i18n';
import { HostKind } from '../host/abi';
import {
  attachAudio, attachInput, attachNet, attachRenderer, OtherBuild,
  type AudioSubsystem, type InputSubsystem, type NetSubsystem, type RendererSubsystem, type Subsystem,
} from './subsystems';
import { StatusReader } from './status';
import { DATA_PATH, MANIFEST_PATH } from '../host/datacache';
import { abs } from '../base';
import type { GameInfo } from './loader';
import type { PadPage } from '../input/gamepad';
import type { NetQueue, NetRemoved } from '../net';

export interface SessionOptions {
  module: WebAssembly.Module;
  info: GameInfo;
  saveDir: string;
  env: Record<string, string>;
  canvas: HTMLCanvasElement;
  stage: HTMLElement;
  audio: AudioContext | null;
  displayHz: number;
  /** the name other browsers see on the server's virtual LAN (LAN races, free roam); null: network = off */
  netName: string | null;
  /** free roam: a line for the player (a friend joined, left, went into a race) */
  notice?(text: string, who?: string): void;
  /** free roam: the race lobby's panel, its state as JSON from the game ("{}": none) */
  lobby?(json: string): void;
  /** free roam: quick chat, a phrase said (by a friend or the player), as JSON from the game */
  say?(json: string): void;
  /** every frame drawn: quick chat's places over the cars in it (contract.ts RendererOptions.onMarks) */
  marks?(n: number, m: Float32Array, ids: Uint32Array): void;
  /** the server's LAN took the game back after the link was lost (a server restart, as for a deploy) */
  netBack?(): void;
  /** the server is full: the game's place in line for one (null: not in line) */
  netQueue?(q: NetQueue | null): void;
  /** this browser's own id on the server's LAN (page-id.ts) */
  netId?: string;
  /** the server's operator removed the player from playing with others (a kick or a ban) */
  netRemoved?(r: NetRemoved): void;
  /** the game finished writing a file of the save overlay (path relative to /save) */
  onSaved?(path: string): void;
  /** tests: where frame dumps go, and their scale */
  artifacts?: string; dumpScale?: number;
  toggleFullscreen(): void;
  /** the page's own use of the controller: its menu (Home) and dialogs */
  padPage?: PadPage;
  log(line: string): void;
  /** a subsystem the game cannot do without (the renderer) failed to attach, or stopped working;
   *  what: English (the log; the page translates it); code: its fixed name, the same in every language */
  onFatal(what: string, code?: string): void;
  /** the renderer is of another build than the game module (gpu/index.ts OtherBuild): this page is not the one that
   *  goes with it (without this, onFatal as for any renderer that did not start); why: English, the log's */
  onOtherBuild?(why: string): void;
}

export class GameSession {
  readonly started = performance.now();
  proc!: GameProcess;
  status: StatusReader | null = null;
  private subs: Subsystem[] = [];
  renderer: RendererSubsystem | null = null;
  audio: AudioSubsystem | null = null;
  input: InputSubsystem | null = null;
  net: NetSubsystem | null = null;
  exitCode: number | null = null;
  exited!: Promise<number>;
  private detached = false;
  private canvas!: HTMLCanvasElement;

  static async start(o: SessionOptions): Promise<GameSession> {
    const s = new GameSession();
    s.canvas = o.canvas;
    const tid = (t: number, fd: number) => fd === 2 ? `[${t}!]` : `[${t}]`;
    s.proc = await startProcess({
      module: o.module,
      initialPages: o.info.pages || undefined,
      cfg: { args: ['nfsu2', '/data/SPEED2.EXE'], env: o.env, dataBase: abs(DATA_PATH), manifestUrl: abs(MANIFEST_PATH), saveDir: o.saveDir },
      onLog: (line, fd, t) => o.log(`${tid(t, fd)} ${line}`),
      onSaved: o.onSaved,
      onNote: o.log,
    });
    s.exited = s.proc.exited.then(code => { s.exitCode = code; s.detach(); return code; });
    void s.proc.registered(HostKind.Status).then(r => { if (!s.detached) s.status = new StatusReader(s.proc.memory, r.ptr); });
    const common = { env: o.env, log: o.log };
    // An attach may finish after the game ended (a crash at boot, Stop while the
    // renderer still waits for its registration): such a module is detached at once.
    const attach = async <T extends Subsystem>(name: string, f: () => T | Promise<T>): Promise<T | null> => {
      try {
        const x = await f();
        if (s.detached) { o.log(`[app] ${name} attached after the game ended: detached at once`); try { x.detach(); } catch { /* best effort */ } return null; }
        s.subs.push(x);
        return x;
      } catch (e) {
        o.log(`[app] ${name} could not attach: ${e}`);
        return null;
      }
    };
    // not awaited one by one: a subsystem waiting for its registration must not hold up the others
    let other = '';
    void attach('renderer', async () => {
      try {
        return await attachRenderer(s.proc, o.canvas, {
          ...common, width: o.canvas.width, height: o.canvas.height, displayHz: o.displayHz, artifacts: o.artifacts, dumpScale: o.dumpScale,
          // a frozen picture otherwise: the game stops and the page says why
          onLost: () => { if (!s.detached) o.onFatal(N('the graphics stopped working'), 'GPU-LOST'); },
          onMarks: o.marks,
        });
      } catch (e) { if (e instanceof OtherBuild) other = e.message; throw e; }
    }).then(r => {
      s.renderer = r;
      // without a picture there is no game to play: say so instead of a black screen
      if (r || s.detached) return;
      if (other && o.onOtherBuild) o.onOtherBuild(other);
      else o.onFatal(N('the graphics could not start'), 'GPU-RENDERER');
    });
    void attach('audio', () => attachAudio(s.proc, { ...common, context: o.audio })).then(a => {
      s.audio = a;
      // the audio subsystem closes the context made in the Play click; without one, close it here
      if (!a) void o.audio?.close().catch(() => {});
    });
    void attach('input', () => attachInput(s.proc, o.canvas, {
      ...common, stage: o.stage, toggleFullscreen: o.toggleFullscreen, padPage: o.padPage, viewRect: () => s.viewRect(),
    })).then(i => { s.input = i; });
    if (o.netName !== null)
      void attach('network', () => attachNet(s.proc, {
        ...common, name: o.netName!, id: o.netId, notice: o.notice, lobby: o.lobby, say: o.say, back: o.netBack, queue: o.netQueue, removed: o.netRemoved,
      })).then(n => { s.net = n; });
    return s;
  }

  /** the renderer's aspect-fit image, in CSS px of the canvas (the whole canvas until the renderer is there) */
  viewRect(): { x: number; y: number; w: number; h: number } {
    const c = this.canvas, r = this.renderer?.imageRect();
    if (!r || !c.width || !c.height) return { x: 0, y: 0, w: c.clientWidth, h: c.clientHeight };
    const kx = c.clientWidth / c.width, ky = c.clientHeight / c.height;
    return { x: r.x * kx, y: r.y * ky, w: r.w * kx, h: r.h * ky };
  }

  /** counters the subsystems offer for the detailed overlay */
  stats(): Record<string, number> {
    const out: Record<string, number> = {};
    for (const x of this.subs) Object.assign(out, x.stats?.() ?? {});
    return out;
  }

  /** the game's own quit path, if the input subsystem offers it and it can reach the game */
  canQuit(): boolean { const i = this.input; return !!i?.requestQuit && !this.detached && (i.canQuit?.() ?? true); }
  /** false: the request cannot reach the game (the page can only stop it) */
  requestQuit(): boolean { return this.canQuit() && this.input!.requestQuit!() !== false; }
  /** Download log: asks the game for its stall report (InputSubsystem.logReport); false: it cannot be asked */
  logReport(): boolean { const i = this.input; return !this.detached && !!i?.logReport && i.logReport() !== false; }

  /** true once the game exited or was stopped (every subsystem detached) */
  get ended() { return this.detached; }

  private detach() {
    if (this.detached) return;
    this.detached = true;
    for (const x of this.subs) { try { x.detach(); } catch { /* best effort */ } }
    this.subs = [];
    this.status = null;
  }

  /** Stop at once (every worker), e.g. "Stop the game" or after the exit. */
  terminate() { this.detach(); this.proc.terminate(); }
}
