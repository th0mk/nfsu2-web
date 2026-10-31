// The net worker: the virtual LAN's link (./index.ts Link: the WebSocket to the
// server's switch and the loops between it and the net mailboxes) on a thread
// of its own, off the page's main thread.  The main thread runs the WebGPU
// encoder, the DXT unpacking of textures and the page itself, and a received
// car state, or one C queued, waited behind whatever it was doing: a busy
// frame, and a burst of textures as the city's sections stream in (the
// loading screen's pauses reach ~0.2 s), made friends' cars late and
// uneven on screen.  Here nothing else runs, so a datagram goes to C's RX
// ring as it comes and to the switch as C queues it.  ./remote.ts starts it
// and stands for the link on the page.
//
// Page -> worker: { t: 'start', memory, opts }      (once, first)
//                 { t: 'register', kind, ptr, size } (a net mailbox the game published: HostKind.Net, HostKind.Lan)
//                 { t: 'exit' }                      (the game process ended)
//                 { t: 'call', m, a }                (a NetLink method that changes something: setAway, setSolid ...)
//                 { t: 'close' }
// Worker -> page: { t: 'log', s } | { t: 'notice', text, who } | { t: 'lobby', json } | { t: 'say', json }
//                 { t: 'back' } | { t: 'queue', q } | { t: 'removed', r } | { t: 'peers', peers }
//                 { t: 'attached', kind }            (a mailbox matched this page: 'roam' or 'lan')
//                 { t: 'state', state, vip, stats }  (on every change, and once a second while it runs)
import { attachNet, type NetLink, type NetOptions, type NetPeer, type NetQueue, type NetRemoved, type NetState, type NetStats } from './index';
import type { GameProcess } from '../host/process';

/** the options that cross to the worker: NetOptions without its callbacks */
export type NetWorkerOptions = Omit<NetOptions, 'log' | 'onPeers' | 'onNotice' | 'onLobby' | 'onSay' | 'onBack' | 'onQueue' | 'onRemoved' | 'onAttached'>;
/** the NetLink methods the page calls through the worker (they return nothing it needs, or what it can tell itself) */
export type NetCall =
  | { m: 'setAway'; a: [boolean] } | { m: 'goOffline'; a: [] } | { m: 'setSolid'; a: [boolean] } | { m: 'say'; a: [number] }
  | { m: 'setCrews'; a: [number[], number[]] } | { m: 'setRoom'; a: [string] };
export type ToNetWorker =
  | { t: 'start'; memory: WebAssembly.Memory; opts: NetWorkerOptions }
  | { t: 'register'; kind: number; ptr: number; size: number }
  | { t: 'exit' } | { t: 'close' } | ({ t: 'call' } & NetCall);
export type FromNetWorker =
  | { t: 'log'; s: string } | { t: 'notice'; text: string; who?: string } | { t: 'lobby'; json: string } | { t: 'say'; json: string }
  | { t: 'back' } | { t: 'queue'; q: NetQueue | null } | { t: 'removed'; r: NetRemoved } | { t: 'peers'; peers: NetPeer[] }
  | { t: 'attached'; kind: 'roam' | 'lan' }
  | { t: 'state'; state: NetState; vip: string | null; stats: NetStats };

/** how often the page hears the link's counters while nothing changes (its overlay and the log read them) */
const STATE_MS = 1000;

const post = (m: FromNetWorker) => (self as unknown as Worker).postMessage(m);
let link: NetLink | null = null;
let timer: ReturnType<typeof setInterval> | null = null;
/** what the page heard last: a change is sent at once */
let sent = '';
const regs = new Map<number, { ptr: number; size: number }>();
const regWaiters = new Map<number, ((r: { ptr: number; size: number }) => void)[]>();
let exit!: (code: number) => void;

function report(force = false) {
  if (!link) return;
  const key = `${link.state} ${link.vip}`;
  if (!force && key === sent) return;
  sent = key;
  post({ t: 'state', state: link.state, vip: link.vip, stats: link.stats() });
}

function start(memory: WebAssembly.Memory, opts: NetWorkerOptions) {
  // the game process as the link sees it: the shared memory, and the mailboxes the page passes on
  const proc = {
    memory,
    exited: new Promise<number>(r => (exit = r)),
    registered(kind: number) {
      const r = regs.get(kind);
      if (r) return Promise.resolve(r);
      return new Promise<{ ptr: number; size: number }>(res => { const l = regWaiters.get(kind) ?? []; l.push(res); regWaiters.set(kind, l); });
    },
  } as unknown as GameProcess;
  link = attachNet(proc, {
    ...opts,
    log: s => post({ t: 'log', s }),
    onNotice: (text, who) => post({ t: 'notice', text, who }),
    onLobby: json => post({ t: 'lobby', json }),
    onSay: json => post({ t: 'say', json }),
    onBack: () => { post({ t: 'back' }); report(true); },
    onQueue: q => { post({ t: 'queue', q }); report(); },
    onRemoved: r => { post({ t: 'removed', r }); report(); },
    onPeers: peers => { post({ t: 'peers', peers }); report(); },
    onAttached: kind => { post({ t: 'attached', kind }); report(true); },
  });
  timer = setInterval(() => report(true), STATE_MS);
  report(true);
}

/** the link closed (the page's close, or the game's end): the page hears it at once, and the worker ends */
let ending = false;
function end() {
  if (ending) return;
  ending = true;
  if (timer) { clearInterval(timer); timer = null; }
  link?.close();
  report(true);
  // the socket's close frame, and the BYEs the link sent before it, go out before the worker ends
  setTimeout(() => self.close(), 1000);
}

self.onmessage = (e: MessageEvent<ToNetWorker>) => {
  const m = e.data;
  if (m.t === 'start') start(m.memory, m.opts);
  else if (m.t === 'register') {
    regs.set(m.kind, { ptr: m.ptr, size: m.size });
    for (const w of regWaiters.get(m.kind) ?? []) w({ ptr: m.ptr, size: m.size });
    regWaiters.delete(m.kind);
  } else if (m.t === 'exit') {
    // the link closes itself on proc.exited (index.ts start): then end, once it has
    exit?.(0);
    setTimeout(end, 0);
  } else if (m.t === 'call' && link) {
    const l = link;
    switch (m.m) {
      case 'setAway': l.setAway(m.a[0]); break;
      case 'goOffline': l.goOffline(); break;
      case 'setSolid': l.setSolid(m.a[0]); break;
      case 'say': l.say(m.a[0]); break;
      case 'setCrews': l.setCrews(m.a[0], m.a[1]); break;
      case 'setRoom': l.setRoom(m.a[0]); break;
    }
    report();
  } else if (m.t === 'close') end();
};
