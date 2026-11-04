// The virtual LAN in a worker of its own, for the page (web/src/app/subsystems.ts, the test pages): the link
// (index.ts Link) with its WebSocket runs in worker.ts, and this is the page's side of it, a NetLink as attachNet's.
//
// On the page's main thread the link waited behind whatever else ran there: each frame's GPU work (the renderer
// encodes and submits there), the menus and overlays, a garbage collection, the timers of a tab in the background.  A
// datagram C queued waited for the main thread to take it (the game's [roam] stats line: "C->page wait"), and one the
// switch sent waited in the main thread's queue before it went into C's RX ring.  The worker does nothing else: the
// datagrams still go through the mailboxes in the shared memory (C's TX and RX rings and bells, as before), and
// only the link's own messages cross between the page and the worker:
//   page -> worker  the shared memory and the options (once), each mailbox the game registers, the game's end, and
//                   NetLink's calls (setAway, say, setRoom, close ...)
//   worker -> page  the callbacks (NetOptions' log, onNotice ... each one's arguments as they were), and the link as
//                   it is now (NetNow: state, address, which mailboxes it took, stats()), with each callback, when it
//                   changes and every second; its peer list when that changed, every half second at most
// So state, vip, peers and stats() read the worker's last word: at most a second old for stats().  say, friend and
// setRoom answer from it too (whether that mailbox is there; whether the link is up), and are done in the worker.
//
// When no worker starts (an error loading it, nothing from it in READY_MS) the link runs on the page as before
// (attachNet), and the calls made meanwhile are made there.
import type { GameProcess } from '../host/process';
import { HostKind } from '../host/abi';
import { attachNet, type NetLink, type NetOptions, type NetPeer, type NetState, type NetStats } from './index';

/** NetOptions' callbacks: the worker sends the page each call of those the page gave */
export const CALLBACKS = ['log', 'onPeers', 'onNotice', 'onLobby', 'onSay', 'onBump', 'onBattle', 'onFriend', 'onBack', 'onQueue',
  'onRemoved', 'onRaces', 'onRename', 'onAttached'] as const;
export type NetCallback = typeof CALLBACKS[number];
/** NetLink's calls, made in the worker */
export type NetCall = 'setAway' | 'goOffline' | 'rename' | 'setSolid' | 'say' | 'friend' | 'setCrews' | 'setRoom' | 'close';
/** the link as the worker last saw it (its peer list only when that changed: worker.ts PEERS_MS) */
export interface NetNow { state: NetState; vip: string | null; peers?: NetPeer[]; roam: boolean; lan: boolean; stats: NetStats }
/** NetOptions without its callbacks (what can go to the worker) */
export type NetPlain = Omit<NetOptions, NetCallback>;
export type ToNet =
  | { t: 'init'; memory: WebAssembly.Memory; o: NetPlain; cbs: NetCallback[] }
  | { t: 'reg'; kind: number; ptr: number; size: number }
  | { t: 'exit' }
  | { t: 'call'; m: NetCall; a: unknown[] };
export type FromNet =
  | { t: 'ready' }
  | { t: 'fail'; why: string }
  | { t: 'now'; now: NetNow }
  | { t: 'cb'; k: NetCallback; a: unknown[]; now: NetNow }
  | { t: 'online'; vip: string; now: NetNow };

/** how long the worker may take to say it runs before the link starts on the page instead */
const READY_MS = 10_000;
/** the worker ends itself after its link closed (worker.ts LINGER_MS); this long after, the page ends it all the same */
const END_MS = 2000;

const NO_STATS: NetStats = {
  state: 'waiting', vip: null, connections: 0, reconnects: 0, framesOut: 0, bytesOut: 0, framesIn: 0, bytesIn: 0, compactIn: 0,
  deltaIn: 0, txOfflineDrop: 0, rxFullDrop: 0, rxOversize: 0, rxFiltered: 0, switchRttMs: 0, switchRttMeanMs: 0, switchRttMaxMs: 0,
  txWaitMeanMs: 0, txWaitMaxMs: 0, snapWaitMeanMs: 0, snapWaitMaxMs: 0, peersOnSwitch: 0, mainMs: 0, mainCalls: 0, loopLagMeanMs: 0, loopLagMaxMs: 0,
  lan: { attached: false, framesOut: 0, framesIn: 0, waited: 0, waitedMax: 0, refusedNoEndpoint: 0, refuseHeld: 0, peersGone: 0 },
};

class RemoteLink implements NetLink {
  private now: NetNow = { state: 'waiting', vip: null, roam: false, lan: false, stats: NO_STATS };
  private peerList: NetPeer[] = [];
  online: Promise<string>;
  private resolveOnline!: (v: string) => void;
  /** the worker said it runs */
  private ready = false;
  /** the link on the page instead (no worker started) */
  private direct: NetLink | null = null;
  /** the calls made before the worker said it runs: made again on the page when it never does */
  private early: { m: NetCall; a: unknown[] }[] = [];
  private readyTimer: ReturnType<typeof setTimeout> | null = null;
  private closed = false;

  constructor(private proc: GameProcess, private o: NetOptions, private w: Worker) {
    this.online = new Promise(r => (this.resolveOnline = r));
    w.onmessage = (e: MessageEvent<FromNet>) => this.heard(e.data);
    w.onerror = (e) => {
      if (!this.ready) { e.preventDefault(); this.onPage(e.message || 'it could not load'); }
      else this.o.log?.(`[net] the network's worker: ${e.message}`);
    };
    this.readyTimer = setTimeout(() => this.onPage(`nothing from it in ${READY_MS / 1000} s`), READY_MS);
    const plain: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(o)) if (typeof v !== 'function') plain[k] = v;
    this.post({ t: 'init', memory: proc.memory, o: plain as NetPlain, cbs: CALLBACKS.filter(k => !!o[k]) });
    for (const kind of [HostKind.Net, HostKind.Lan])
      void proc.registered(kind).then(r => this.post({ t: 'reg', kind, ptr: r.ptr, size: r.size }));
    void proc.exited.then(() => { this.post({ t: 'exit' }); this.ended(); });
  }

  private post(m: ToNet) { if (!this.direct) this.w.postMessage(m); }
  private heard(m: FromNet) {
    if (this.direct) return;
    if (m.t === 'ready') {
      this.ready = true; this.early = [];
      if (this.readyTimer) { clearTimeout(this.readyTimer); this.readyTimer = null; }
      this.o.log?.('[net] the link to the server runs in a worker of its own');
      return;
    }
    if (m.t === 'fail') { this.onPage(m.why); return; }
    this.now = m.now;
    if (m.now.peers) this.peerList = m.now.peers;
    if (m.t === 'online') this.resolveOnline(m.vip);
    else if (m.t === 'cb') (this.o[m.k] as ((...a: unknown[]) => void) | undefined)?.(...m.a);
  }
  /** no worker: the link on the page, as attachNet makes it, with the calls made so far */
  private onPage(why: string) {
    if (this.ready || this.direct) return;
    if (this.readyTimer) { clearTimeout(this.readyTimer); this.readyTimer = null; }
    this.w.terminate();
    if (this.closed) return;
    this.o.log?.(`[net] no worker for the link (${why}): it runs on the page`);
    const d = this.direct = attachNet(this.proc, this.o);
    for (const { m, a } of this.early) (d[m] as (...a: unknown[]) => unknown)(...a);
    this.early = [];
    void d.online.then(v => this.resolveOnline(v));
  }
  /** the game ended or the page closed the link: the worker ends itself once its socket's close went (worker.ts) */
  private ended() {
    if (this.closed) return;
    this.closed = true;
    if (this.readyTimer) { clearTimeout(this.readyTimer); this.readyTimer = null; }
    this.now = { ...this.now, state: 'closed' };
    if (!this.direct) { const w = this.w; setTimeout(() => w.terminate(), END_MS); }
  }
  private call(m: NetCall, a: unknown[]) {
    if (!this.ready) this.early.push({ m, a });
    this.post({ t: 'call', m, a });
  }

  get state(): NetState { return this.direct ? this.direct.state : this.now.state; }
  get vip(): string | null { return this.direct ? this.direct.vip : this.now.vip; }
  get peers(): NetPeer[] { return this.direct ? this.direct.peers : this.peerList; }
  stats(): NetStats { return this.direct ? this.direct.stats() : this.now.stats; }
  setAway(on: boolean) { if (this.direct) this.direct.setAway(on); else this.call('setAway', [on]); }
  goOffline() { if (this.direct) this.direct.goOffline(); else this.call('goOffline', []); }
  rename(name: string) { if (this.direct) this.direct.rename(name); else this.call('rename', [name]); }
  setSolid(collide: 'on' | 'off' | 'none') { if (this.direct) this.direct.setSolid(collide); else this.call('setSolid', [collide]); }
  say(p: number, race = false): boolean {
    if (this.direct) return this.direct.say(p, race);
    this.call('say', [p, race]);
    return race ? this.now.lan : this.now.roam;
  }
  friend(m: Record<string, unknown>): boolean {
    if (this.direct) return this.direct.friend(m);
    if (this.now.state !== 'online') return false;
    this.call('friend', [m]);
    return true;
  }
  setCrews(hashes: readonly number[], rgbs: readonly number[] = []) {
    if (this.direct) this.direct.setCrews(hashes, rgbs); else this.call('setCrews', [[...hashes], [...rgbs]]);
  }
  setRoom(name: string): boolean {
    if (this.direct) return this.direct.setRoom(name);
    this.call('setRoom', [name]);
    return this.now.roam;
  }
  close() {
    if (this.closed && !this.direct) return;
    if (this.direct) { this.direct.close(); return; }
    this.call('close', []);
    this.ended();
  }
}

/** Attach the virtual LAN to a running game process, its link in a worker of its own (on the page when none starts).
 *  Returns at once; the socket opens when the runtime registers its first net mailbox (free roam or LAN play). */
export function attachNetWorker(proc: GameProcess, opts: NetOptions = {}): NetLink {
  let w: Worker;
  try { w = new Worker(new URL('/net/worker.js', import.meta.url), { type: 'module', name: 'net' }); }
  catch (e) {
    opts.log?.(`[net] no worker for the link (${e}): it runs on the page`);
    return attachNet(proc, opts);
  }
  return new RemoteLink(proc, opts, w);
}
