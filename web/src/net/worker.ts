// The virtual LAN's worker (remote.ts starts it): the link to the server's switch (index.ts Link) with its
// WebSocket, on a thread that does nothing else, so that no datagram waits for the page's main thread either way.
// The link reads and writes the game's net mailboxes in the shared memory itself, as it did on the page; the page
// hears its callbacks and how it is (remote.ts NetNow), and its calls are made here.
//
// The stamps in the mailboxes' slots (S_T_MS: when C queued a frame, when one came) are each thread's
// performance.timeOrigin + performance.now(), and a worker started after the computer's clock was set reads another
// time than the game's threads (index.ts Endpoint.clock, which the link takes from the stamps themselves).
import { Link, type NetOptions, type NetProcess } from './index';
import type { FromNet, NetNow, ToNet } from './remote';

/** this worker's global (a test's stand-in has one of its own: web/test/net-worker.test.mjs) */
const me = self as unknown as DedicatedWorkerGlobalScope;
const post = (m: FromNet) => me.postMessage(m);
/** The longest a wait for C's next TX frames lasts here (NetOptions.waitMs).  WebKit (Safari, and every browser on an
 *  iPhone) sometimes leaves an Atomics.waitAsync that was woken with nothing pending until something else runs on
 *  its worker (io-worker.ts DOORBELL_WAIT_MS): on the page, every frame's work ran soon after; here a frame C queued
 *  would wait for the next message from the switch or the wait's end.  This bounds such a wait to about a frame's
 *  time, for 50 short wake-ups a second while nothing goes. */
const WAIT_MS = 20;
/** how often the link's state and address are looked at for a change (the page reads them: remote.ts; a mailbox it
 *  took goes at once, onAttached) */
const LOOK_MS = 100;
/** how often the page gets the link's stats() anyway */
const STATS_MS = 1000;
/** the peer list goes to the page at most this often (with LAN play open it names every player who has it open, a
 *  list that changes with each of them coming and going), unless the page listens to onPeers itself */
const PEERS_MS = 500;
/** how long the worker stays after the link closed: its socket's close goes out first */
const LINGER_MS = 1000;

// without these the page runs the link itself (remote.ts: an error before 'ready')
if (!me.crossOriginIsolated) throw new Error('the worker is not cross-origin isolated (no shared memory)');
if (typeof (Atomics as { waitAsync?: unknown }).waitAsync !== 'function') throw new Error('no Atomics.waitAsync in a worker');

let link: Link | null = null;
let exited: (code: number) => void = () => {};
const regs = new Map<number, { ptr: number; size: number }>();
const waiting = new Map<number, ((r: { ptr: number; size: number }) => void)[]>();
let last = '';
let peersDue = false, peersAt = 0;
let peersTimer: ReturnType<typeof setTimeout> | null = null;
const timers: ReturnType<typeof setInterval>[] = [];

/** the link now: with its peer list only when that is to go (`peers`) */
function nowOf(l: Link, peers: boolean): NetNow {
  return { state: l.state, vip: l.vip, ...(peers ? { peers: l.peers } : {}), ...l.attached(), stats: l.stats() };
}
/** the link as it is now, to the page: always, or (quiet) only when its state, address or mailboxes changed */
function tell(quiet = false) {
  if (!link) return;
  const a = link.attached(), key = `${link.state} ${link.vip} ${a.roam} ${a.lan}`;
  if (quiet && key === last) return;
  last = key;
  post({ t: 'now', now: nowOf(link, false) });
}
/** the peer list to the page, PEERS_MS after the last time at most */
function tellPeers() {
  peersTimer = null;
  if (!link || !peersDue) return;
  const wait = peersAt + PEERS_MS - performance.now();
  if (wait > 0) { peersTimer = setTimeout(tellPeers, wait); return; }
  peersDue = false; peersAt = performance.now();
  post({ t: 'now', now: nowOf(link, true) });
}
/** the link closed: nothing more to tell, and the worker ends LINGER_MS later */
function closing() {
  for (const t of timers.splice(0)) clearInterval(t);
  if (peersTimer) { clearTimeout(peersTimer); peersTimer = null; }
  setTimeout(() => me.close(), LINGER_MS);
}

// the shared memory could not come over: the page runs the link itself
me.onmessageerror = () => post({ t: 'fail', why: 'the shared memory could not reach it' });
me.onmessage = (e: MessageEvent<ToNet>) => {
  const m = e.data;
  if (m.t === 'init') {
    if (link) return;
    const proc: NetProcess = {
      memory: m.memory,
      exited: new Promise<number>(r => (exited = r)),
      registered(kind: number) {
        const r = regs.get(kind);
        if (r) return Promise.resolve(r);
        return new Promise(res => { const l = waiting.get(kind) ?? []; l.push(res); waiting.set(kind, l); });
      },
    };
    const o: NetOptions = { ...m.o, waitMs: WAIT_MS };
    const page = new Set(m.cbs);
    for (const k of page) {
      if (k === 'onPeers') continue;
      (o as Record<string, unknown>)[k] = (...a: unknown[]) => { if (link) post({ t: 'cb', k, a, now: nowOf(link, false) }); };
    }
    // a mailbox the link took: the page hears it at once (say and setRoom answer from it), whether it listens or not
    o.onAttached = (kind) => {
      if (!link) return;
      if (page.has('onAttached')) post({ t: 'cb', k: 'onAttached', a: [kind], now: nowOf(link, false) });
      else tell();
    };
    // NetLink.peers: every change when the page listens to onPeers, else at most every PEERS_MS
    o.onPeers = (peers) => {
      if (!link) return;
      if (page.has('onPeers')) { post({ t: 'cb', k: 'onPeers', a: [peers], now: nowOf(link, true) }); return; }
      peersDue = true;
      if (!peersTimer) tellPeers();
    };
    const l = link = new Link(proc, o);
    l.start();
    void l.online.then(vip => post({ t: 'online', vip, now: nowOf(l, false) }));
    timers.push(setInterval(() => tell(true), LOOK_MS), setInterval(() => tell(), STATS_MS));
    post({ t: 'ready' });
    tell();
  } else if (m.t === 'reg') {
    const r = { ptr: m.ptr, size: m.size };
    regs.set(m.kind, r);
    for (const w of waiting.get(m.kind) ?? []) w(r);
    waiting.delete(m.kind);
  } else if (m.t === 'exit') {
    exited(0);
    // (the link closes as the game's end resolves, before this)
    void Promise.resolve().then(() => { tell(); closing(); });
  } else if (m.t === 'call' && link) {
    (link[m.m] as (...a: unknown[]) => unknown).apply(link, m.a);
    tell();
    if (m.m === 'close') closing();
  }
};
