// The virtual LAN's link run in the net worker (./worker.ts), as the page sees
// it: a NetLink like ./index.ts attachNet's, whose WebSocket and mailbox loops
// are off the main thread.  The page passes the worker the game's shared
// memory and each net mailbox the game registers; what the link reports
// (state, address, peers, counters) comes back as messages and is answered
// from the last one, and its callbacks are called here.  Where the worker
// cannot start (no module workers, or its script failed to load) the link
// runs here on the main thread, as before, with the settings the page made
// meanwhile.
import type { GameProcess } from '../host/process';
import { HostKind } from '../host/abi';
import * as P from './proto';
import { attachNet as attachHere, type NetLink, type NetOptions, type NetPeer, type NetState, type NetStats } from './index';
import type { FromNetWorker, NetCall, NetWorkerOptions, ToNetWorker } from './worker';

const NO_STATS: NetStats = {
  state: 'waiting', vip: null, connections: 0, reconnects: 0, framesOut: 0, bytesOut: 0, framesIn: 0, bytesIn: 0, compactIn: 0, deltaIn: 0,
  txOfflineDrop: 0, rxFullDrop: 0, rxOversize: 0, rxFiltered: 0, switchRttMs: 0, switchRttMeanMs: 0, switchRttMaxMs: 0,
  txWaitMeanMs: 0, txWaitMaxMs: 0, peersOnSwitch: 0, mainMs: 0, mainCalls: 0,
  lan: { attached: false, framesOut: 0, framesIn: 0, waited: 0, waitedMax: 0, refusedNoEndpoint: 0, refuseHeld: 0, peersGone: 0 },
};

class RemoteLink implements NetLink {
  state: NetState = 'waiting';
  vip: string | null = null;
  peers: NetPeer[] = [];
  online: Promise<string>;
  private resolveOnline!: (v: string) => void;
  private w: Worker | null;
  /** the link on this thread, once the worker could not start */
  private here: NetLink | null = null;
  private last: NetStats = NO_STATS;
  private closed = false;
  /** the worker answered (its script loaded): from then on an error is its own, not a reason to run the link here */
  private heard = false;
  /** free roam's mailbox, once the worker's link took it (say, canRoom) */
  private roam: Int32Array | null = null;
  /** what the page set, for a link started here after all */
  private away = false;
  private offline = false;
  private solid: boolean | null = null;
  private crews: [number[], number[]] | null = null;
  private room: string | null = null;

  constructor(private proc: GameProcess, private o: NetOptions, w: Worker) {
    this.online = new Promise(r => (this.resolveOnline = r));
    this.w = w;
    w.onmessage = (e: MessageEvent<FromNetWorker>) => this.message(e.data);
    w.onerror = (e) => {
      if (this.heard || this.closed) { this.o.log?.(`[net] the net worker: ${e.message}`); return; }
      e.preventDefault();
      this.fallBack(`the net worker did not start (${e.message || 'no script'})`);
    };
    const opts: NetWorkerOptions = { name: o.name, id: o.id, was: o.was, claim: o.claim, block: o.block, url: o.url, pingMs: o.pingMs, idleMs: o.idleMs };
    this.send({ t: 'start', memory: proc.memory, opts });
    for (const kind of [HostKind.Net, HostKind.Lan]) void proc.registered(kind).then(r => this.send({ t: 'register', kind, ptr: r.ptr, size: r.size }));
    proc.exited.then(() => this.send({ t: 'exit' }));
  }

  private send(m: ToNetWorker) { if (this.w && !this.closed) this.w.postMessage(m); }
  private call(c: NetCall) { if (this.here) return; this.send({ t: 'call', ...c }); }

  private message(m: FromNetWorker) {
    this.heard = true;
    if (this.closed && m.t !== 'state' && m.t !== 'log') return;
    switch (m.t) {
      case 'log': this.o.log?.(m.s); break;
      case 'notice': this.o.onNotice?.(m.text, m.who); break;
      case 'lobby': this.o.onLobby?.(m.json); break;
      case 'say': this.o.onSay?.(m.json); break;
      case 'back': this.o.onBack?.(); break;
      case 'queue': this.o.onQueue?.(m.q); break;
      case 'removed': this.o.onRemoved?.(m.r); break;
      case 'peers': this.peers = m.peers; this.o.onPeers?.(m.peers); break;
      case 'attached':
        if (m.kind === 'roam') void this.proc.registered(HostKind.Net).then(r => {
          const buf = this.proc.memory.buffer as unknown as SharedArrayBuffer, ver = new Int32Array(buf, r.ptr, 2)[P.W_VERSION];
          this.roam = new Int32Array(buf, r.ptr, (ver === 2 ? P.NET_V2_HDR_BYTES : P.NET_HDR_BYTES) / 4);
        });
        break;
      case 'state':
        this.state = m.state; this.vip = m.vip; this.last = m.stats;
        if (m.state === 'online' && m.vip) this.resolveOnline(m.vip);
        break;
    }
  }

  /** the worker could not start: the link runs on this thread, with what the page set so far */
  private fallBack(why: string) {
    if (this.here || this.closed) return;
    this.o.log?.(`[net] ${why}: the link runs on the page's main thread`);
    this.w?.terminate(); this.w = null;
    const l = this.here = attachHere(this.proc, {
      ...this.o,
      onPeers: p => { this.peers = p; this.o.onPeers?.(p); },
    });
    void l.online.then(v => this.resolveOnline(v));
    if (this.solid !== null) l.setSolid(this.solid);
    if (this.crews) l.setCrews(...this.crews);
    if (this.room !== null) l.setRoom(this.room);
    if (this.offline) l.goOffline(); else if (this.away) l.setAway(true);
  }

  stats(): NetStats {
    if (this.here) return this.here.stats();
    return { ...this.last, state: this.state, vip: this.vip };
  }
  setAway(on: boolean) { this.away = on; if (this.here) this.here.setAway(on); else this.call({ m: 'setAway', a: [on] }); }
  goOffline() { this.offline = true; if (this.here) this.here.goOffline(); else this.call({ m: 'goOffline', a: [] }); }
  setSolid(on: boolean) { this.solid = on; if (this.here) this.here.setSolid(on); else this.call({ m: 'setSolid', a: [on] }); }
  say(p: number): boolean {
    if (this.here) return this.here.say(p);
    if (!this.roam) return false;
    this.call({ m: 'say', a: [p] });
    return true;
  }
  setCrews(hashes: readonly number[], rgbs: readonly number[] = []) {
    this.crews = [hashes.slice(), rgbs.slice()];
    if (this.here) this.here.setCrews(hashes, rgbs); else this.call({ m: 'setCrews', a: this.crews });
  }
  canRoom(): boolean {
    if (this.here) return this.here.canRoom();
    const i32 = this.roam;
    return !!i32 && i32.length > P.W_ROOM_NAME + P.ROOM_WORDS && (Atomics.load(i32, P.W_CAPS) & P.NET_CAP_ROOM) !== 0;
  }
  setRoom(name: string): boolean {
    this.room = name;
    if (this.here) return this.here.setRoom(name);
    this.call({ m: 'setRoom', a: [name] });
    return this.canRoom();
  }
  close() {
    if (this.closed) return;
    if (this.here) { this.here.close(); this.closed = true; return; }
    // the worker sends what C queued last and closes the socket, then ends itself
    this.send({ t: 'close' });
    this.closed = true;
    this.state = 'closed';
    const w = this.w; this.w = null;
    if (w) setTimeout(() => w.terminate(), 2000);
  }
}

/** Attach the virtual LAN to a running game process, its link in the net worker (or here, where that cannot start).
 *  Returns at once, as ./index.ts attachNet does. */
export function attachNet(proc: GameProcess, opts: NetOptions = {}): NetLink {
  let w: Worker;
  try {
    w = new Worker(new URL('/net/net-worker.js', import.meta.url), { type: 'module', name: 'net' });
  } catch (e) {
    opts.log?.(`[net] the net worker did not start (${(e as Error).message}): the link runs on the page's main thread`);
    return attachHere(proc, opts);
  }
  return new RemoteLink(proc, opts, w);
}
