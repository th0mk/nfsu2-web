// The virtual LAN on the page: one WebSocket to the server's switch
// (web/server-rs/crates/lan, /lan on this page's origin) and the net mailboxes the C
// runtime publishes (runtime/web/net_proto.h, one layout):
//   HostKind.Net  free roam (runtime/web/roam_web.c): frame kinds 1 and 2
//   HostKind.Lan  LAN play, the Winsock HLE's virtual LAN (runtime/web/net_lan.c):
//                 frame kinds 3 (UDP), 4 (TCP), 5 (ICMP)
//
//   C (TX rings) --> this file --> WebSocket --> switch --> other browsers
//   C (RX rings) <-- this file <-- WebSocket <-- switch       (routed by frame kind)
//
// The page runs this in a worker of its own (remote.ts starts worker.ts, which
// runs this file's Link), so that nothing on the page's main thread (each
// frame's GPU work, the menus, a tab in the background) makes a datagram wait
// either way; on the main thread (attachNet) when no worker can start, and in
// the test harnesses that load this file alone.  It never
// blocks: it sleeps on each TX ring's bell with Atomics.waitAsync (C rings it
// when it queued frames) and writes received frames straight into the right RX
// ring, then rings C's network thread with Atomics.notify.  The switch assigns
// a virtual IPv4 address; this writes it into every mailbox, keeps the
// connection (reconnects with back-off and asks for the same address again,
// with the token the switch gave with it: /lan?resume, then the address and
// the token in the first message, never in the URL),
// writes the switch's peer list as a bitmap into the mailboxes, tells the LAN
// side when a browser left (its TCP connections are reset), and pings the
// switch for its round-trip time.  The server's own online host (the game's
// Online mode, web/server-rs/crates/online) is on the peer list as server: true: in the
// bitmap, never among the players.  Nothing connects until the runtime
// registers a mailbox, which it does only when a network feature is used (free
// roam: roam = on; LAN play: the game's first WSAStartup, its LAN screens).
//
// A place on the switch is held only while something needs it: each mailbox
// says so in its want word (free roam while it runs; LAN play from the game's
// WSAStartup to its last WSACleanup, so the LAN and Online screens).  When no
// mailbox has wanted it for idleMs the page closes the socket ("idle": the
// switch frees the address at once) and tells C (NET_PAGE_IDLE); the next
// want connects again.  The switch also hears whether LAN play is open
// ({"t":"lan"}): LAN broadcasts go only to the browsers that have it open,
// and with "udp" (the bound UDP ports as bits, Endpoint.udpPorts) only to
// those of them with a socket on the broadcast's port.
// A player who is away (the app's check, web/src/app/away.ts) gives the place
// back the same way, wanted or not: setAway(true) closes the socket ("idle")
// and tells C NET_PAGE_IDLE (Online and LAN play lose their connections, free
// roam its friends), and nothing connects until setAway(false).
//
// What C queued at once goes to the switch as one message (a bundle, nfsu2-lan):
// free roam's snapshot to each friend, the same bytes, once with the friends'
// addresses (TxBundle); and the switch sends this page its datagrams as compact
// bundles (kind 33: the same frames, about a quarter fewer bytes, proto.ts
// unpackCompact; the hello's "bundle":1 and "compact":1).
//
// The operator may remove a player (web/server-rs/crates/lan, Kicks and bans):
// the switch sends {"t":"kick"} or {"t":"ban"} with the operator's message,
// then closes with 4001 or 4003.  The page tells the player (onRemoved) and
// stays off the switch for the rest of this game, as after goOffline.  The
// hello names this browser's own id (NetOptions.id, and NetOptions.was), which
// a ban names besides the player's address, and the token of the keyphrase it
// plays with (NetOptions.claim), which says whose claimed names it may play
// under, and which a ban names too.  A race ban keeps the player on the switch
// ({"t":"races"}: onRaces; the switch drops its races).  A name the operator
// took away ({"t":"rename"}, then the close 4004: onRename) keeps the page off
// the switch until the player picks another (rename), under which it goes back.
//
// TCP frames are never dropped here: while the LAN RX ring is full they wait
// in order (the C side's streams have no retransmission; the WebSocket and the
// switch are reliable and ordered).  A TCP connect to this browser while it has
// no LAN endpoint is answered with a reset (refused), as a host without a
// listener would.
import type { GameProcess } from '../host/process';
import { HostKind } from '../host/abi';
import * as P from './proto';
import { N } from '../app/i18n';

export interface NetOptions {
  /** the name other players see on the switch's peer list */
  name?: string;
  /** this browser's own id (app/page-id.ts), in the hello: the operator's bans name a page by it; and the one its
   *  other store held when that was another (the hello's "was": a ban of either holds) */
  id?: string;
  was?: string[];
  /** the token of the keyphrase the page plays with (app/claim.ts), in the hello: the claimed names it may play under
   *  (the server keeps a hash of it, never the token) */
  claim?: string;
  /** free roam: the names of the players this one blocks, in the hello: the server's hub links none of them with it */
  block?: string[];
  /** the switch (default: /lan on this page's origin, wss on https) */
  url?: string;
  /** state changes, one line each */
  log?: (s: string) => void;
  /** the switch's peer list changed */
  onPeers?: (peers: NetPeer[]) => void;
  /** a line for the player: from the game (NET_KIND_NOTICE: a friend joined, left, went into a race; who: the
   *  friend's name the text starts with), or the server full */
  onNotice?: (text: string, who?: string) => void;
  /** the free-roam race lobby's panel from the game (NET_KIND_LOBBY): its state as JSON, "{}" for none */
  onLobby?: (json: string) => void;
  /** quick chat from the game (NET_KIND_SAY): a phrase said, by a friend or the player, as JSON */
  onSay?: (json: string) => void;
  /** free roam's friends: the player's car and another's touched front bumpers for 3 s, or parted after that
   *  (NET_KIND_FRIEND), as JSON from the game */
  onBump?: (json: string) => void;
  /** a drift battle's scoreboard (NET_KIND_BATTLE), as JSON from the game ("{}": none) */
  onBattle?: (json: string) => void;
  /** free roam's friends: a {"t":"friend",...} message from the server (server-rs src/app/friends.rs) */
  onFriend?: (m: Record<string, unknown>) => void;
  /** the switch welcomed this page again after the link was lost (a server restart, as for a deploy) */
  onBack?: () => void;
  /** the server is full and this page waits in line for a place: where it is, as the switch last said; null: not
   *  (any more) */
  onQueue?: (q: NetQueue | null) => void;
  /** the operator removed this player from playing with others (a kick or a ban), with its message: the page stays
   *  off the switch for the rest of this game */
  onRemoved?: (r: NetRemoved) => void;
  /** the operator's race ban (the switch's {"t":"races"}): this player plays on with others but races none of them
   *  (the switch drops its races), why and until when; again each time LAN play opens (the LAN and Online screens);
   *  null when it was lifted */
  onRaces?: (r: NetRaces | null) => void;
  /** the operator took this player's name away (the switch's {"t":"rename"}, then the close 4004): the page stays off
   *  the switch until the player picks another (rename) */
  onRename?: (r: NetRename) => void;
  /** a mailbox of the game matched this page and the link took it: free roam's ('roam') or LAN play's ('lan') */
  onAttached?: (kind: 'roam' | 'lan') => void;
  /** milliseconds between pings to the switch (default 10000): their round trip is only shown (the F9 log's
   *  "lan rtt ms"); the switch times free roam's paths with pings of its own.  Each ping and its answer were a
   *  message each way through cloudflared for every page, 2 s apart */
  pingMs?: number;
  /** milliseconds no mailbox wants the switch before the page lets it go (default 10000) */
  idleMs?: number;
  /** the longest a wait for C's next TX frames lasts, in ms (default 1000): a wake-up of WebKit's that comes late
   *  (worker.ts) is not later than this */
  waitMs?: number;
}
export interface NetPeer { vip: string; name: string; self: boolean }
/** a kick or a ban, as the switch said it: the operator's message (maybe ""), and a ban's end (ms since 1970; none:
 *  for good) */
export interface NetRemoved { ban: boolean; msg: string; until?: number }
/** a race ban, as the switch said it: the operator's message (maybe ""), and its end (ms since 1970; none: for good) */
export interface NetRaces { msg: string; until?: number }
/** a name the operator took away, and the message with it (maybe "") */
export interface NetRename { name: string; msg: string }
/** the switch's close codes for a kick, a ban and a name taken away */
const CLOSE_KICKED = 4001, CLOSE_BANNED = 4003, CLOSE_RENAME = 4004;
/** why a player whose car flew in free roam for minutes goes off the switch (`flying`), over the game as a kick's */
export const FLYING_MSG = "You're not an airplane, get down to the ground like the rest of us";
/** a place in line: pos 1 is the next in, of everyone in line, eta about how many seconds until its turn (when the
 *  switch can tell: a few have gone in) */
export interface NetQueue { pos: number; of: number; eta?: number }
export type NetState = 'waiting' | 'connecting' | 'online' | 'offline' | 'queued' | 'idle' | 'away' | 'closed';
export interface NetStats {
  state: NetState; vip: string | null; connections: number; reconnects: number;
  framesOut: number; bytesOut: number; framesIn: number; bytesIn: number;
  /** compact bundles from the switch (kind 33: free roam's datagrams in about a quarter fewer bytes) */
  compactIn: number;
  /** of their free-roam datagrams, those that came as what changed since their source's last one */
  deltaIn: number;
  /** frames C queued while the switch was not connected (dropped, as UDP would) */
  txOfflineDrop: number;
  /** received frames dropped: RX ring full / larger than a slot / nobody listening on that kind */
  rxFullDrop: number; rxOversize: number; rxFiltered: number;
  switchRttMs: number; switchRttMeanMs: number; switchRttMaxMs: number;
  /** C queued -> sent on the socket */
  txWaitMeanMs: number; txWaitMaxMs: number;
  /** the same for free roam's car states to friends alone, which never wait on purpose (txHold): the thread's delay */
  snapWaitMeanMs: number; snapWaitMaxMs: number;
  peersOnSwitch: number;
  /** time spent moving frames (both directions), ms in total, and calls */
  mainMs: number; mainCalls: number;
  /** how late a task posted to the thread this runs on ran, ms, mean and most (a received message waits as long for
   *  its turn before it goes to C): every LOOP_LAG_MS while the link is up */
  loopLagMeanMs: number; loopLagMaxMs: number;
  /** LAN play: frames each way, TCP frames that waited for RX ring room (most at once),
   *  TCP connects refused here because the game had no LAN endpoint (and left unanswered, over the limit of such
   *  answers), peers that left */
  lan: { attached: boolean; framesOut: number; framesIn: number; waited: number; waitedMax: number; refusedNoEndpoint: number;
    refuseHeld: number; peersGone: number };
}
export interface NetLink {
  readonly state: NetState;
  readonly vip: string | null;
  readonly peers: NetPeer[];
  /** resolves with our virtual address once the switch first assigned one */
  readonly online: Promise<string>;
  stats(): NetStats;
  /** the player is away: the place on the switch goes back, and stays free until setAway(false) */
  setAway(on: boolean): void;
  /** the player stops waiting in line for a place: no switch for the rest of this game */
  goOffline(): void;
  /** the player picked another name after the operator took its name away (onRename): back on the switch under it */
  rename(name: string): void;
  /** free roam's solid cars, switched while the game runs (the page menu; runtime/roam_game.c solid_follow), as
   *  roam_collide has them: on, off (Ghost mode: Players) or none (Ghost mode: Everything) */
  setSolid(collide: 'on' | 'off' | 'none'): void;
  /** quick chat: the player says phrase p (runtime/roam_game.c say_frame sends it; race: in a race between players,
   *  runtime/racehorn.c qc_say); false: no free roam (race: no LAN play) */
  say(p: number, race?: boolean): boolean;
  /** free roam's friends: a message to the server ({"t":"friend",...m}, or m's own "t": "room"); false: the link is
   *  not up now */
  friend(m: Record<string, unknown>): boolean;
  /** free roam's crews, changed while the game runs (the page menu): their hashes (players.ts roomHash), in the
   *  player's order, at most 4, and their colours (0x00rrggbb; 0 or none: the place's default) */
  setCrews(hashes: readonly number[], rgbs?: readonly number[]): void;
  /** free roam's room, changed while the game runs (an invite link): its name ('' for none), as the game is given it
   *  (players.ts roomText); the game leaves its friends of the old room and the server links it in the new one once
   *  the player is not in an outrun, a race lobby or a race.  false: no free-roam mailbox yet (it takes it as it
   *  registers one) */
  setRoom(name: string): boolean;
  close(): void;
}

/** The page's own answers (a RST to a connect when the game has no LAN endpoint): at most AUTO_SRC bytes a second
 *  to one sender and AUTO_ALL to all of them, a burst of 2 s each, an answer its frame's size and AUTO_COST more, as
 *  the runtime's (runtime/web/net_lan.c auto_ok): stray SYNs cannot make this browser spend its share of the switch
 *  on answers, and its free-roam snapshots dropped then */
const AUTO_COST = 64, AUTO_SRC = 4096, AUTO_ALL = 32768;
class AutoBucket {
  private t = -1; v = 0;
  /** filled up to now; n bytes in it? */
  has(rate: number, n: number, t: number) {
    this.v = this.t < 0 ? 2 * rate : Math.min(2 * rate, this.v + (t - this.t) * rate);
    this.t = t;
    return this.v >= n;
  }
}

/** Free roam's own datagrams to friends and the hub (pings, pongs, HELLOs, the SNAP to the hub) wait for the next car
 *  state to its friends, this long after the last send at most: the core runs every 10 ms and answers as datagrams
 *  come (runtime/web/roam_web.c), so a page sent a message for nearly every pass, about 37 a second against the
 *  switch's 20 (web/tools/roam-crowd.mjs --loop-ms 10, 2026-10-24); the car states themselves never wait */
const TX_HOLD_MS = 50;
/** The same while the game is in free roam but not driving (a race, a shop, the menus, loading: runtime/roam_core.h
 *  RC_ST_*, from its own HELLO or SNAP): it draws no other car then and no one draws its, yet its friends' pings, pongs
 *  and HELLOs kept a message going nearly every 50 ms each way (2026-10-30: about half of nfsu2.xyz's players in free
 *  roam, 70 % of a driving player's messages).  The server holds what its friends send it as long (nfsu2-lan away.rs);
 *  a HELLO or SNAP that says it drives again goes at once. */
const TX_HOLD_AWAY_MS = 500;
/** LAN play's TX ring waits this long after its first frame came: a race's creator sends each joiner a datagram every
 *  frame, one after the other, and the page woke at the first, so the rest of the frame's went in messages of their
 *  own (web/tools/lan-pair.mjs, 4 players: the creator's page sent 74 messages a second for 178 datagrams, about a
 *  quarter fewer this way); the switch puts what comes for a page in 4 ms together (nfsu2-lan `lan_ms`) */
const TX_HOLD_LAN_MS = 1;
/** runtime/roam_core.h: a datagram's type (header byte 5): HELLO, the car state, a ping and its answer */
const RC_MSG_HELLO = 1, RC_MSG_SNAP = 3, RC_MSG_PING = 6, RC_MSG_PONG = 7;

/** Whether a free-roam datagram of the game's own says it is not driving (a HELLO's or a SNAP's status, RC_ST_*: 0 the
 *  menus, 1 loading, 3 a race, 4 a shop; not 2 in free roam or 5 paused there); undefined for any other datagram.  d:
 *  where the datagram starts in dv */
function roamAway(dv: DataView, d: number, len: number): boolean | undefined {
  const type = dv.getUint8(d + 5);
  // after its 16-byte header: a HELLO's core version and platform, then its status; a SNAP's status first
  const at = type === RC_MSG_HELLO ? 19 : type === RC_MSG_SNAP ? 16 : -1;
  if (at < 0 || len <= at + 4 || dv.getUint32(d, false) !== 0x4e324652) return undefined;
  const st = dv.getUint8(d + at);
  return st === 0 || st === 1 || st === 3 || st === 4;
}

/** A ping's send time (t0) or a pong's (t2) moved on by the time it waited here since the core made it (its slot's
 *  S_T_MS, the same clock): the round trip a friend's solid car is drawn ahead by (roam_game.c render_delay) is the
 *  network's, not this page's TX_HOLD_MS.  f: a free-roam frame, len its datagram's bytes; the CRC made again */
function sentNow(f: Frame, len: number, waitedMs: number) {
  const d = P.F_DATA, type = f[d + 5];
  if (len < 24 || (type !== RC_MSG_PING && type !== RC_MSG_PONG) || f[d] !== 0x4e || f[d + 1] !== 0x32 || f[d + 2] !== 0x46 || f[d + 3] !== 0x52) return;
  const us = Math.round(waitedMs * 1000), at = d + (type === RC_MSG_PING ? 16 : 24);
  if (!(us > 0) || us > 1e6 || at + 4 > d + len - 4) return;
  const fv = new DataView(f.buffer);
  fv.setUint32(at, (fv.getUint32(at, true) + us) >>> 0, true);
  fv.setUint32(d + len - 4, P.crc32(f.subarray(d), len - 4), true);
}

/** What one drain of a TX ring sends, as bundles (kind 32, nfsu2-lan): a free-roam datagram (kind 1) of the same
 *  bytes as one before it goes in that one's entry with its destination added (when nothing for that destination
 *  came between, so each destination gets its frames in order), everything else in an entry of its own, in the
 *  order C queued it */
const MULTI_MAX = 64;
/** a frame of our own (a socket cannot send shared memory) */
type Frame = Uint8Array<ArrayBuffer>;
class TxBundle {
  private e: { f: Frame; dst: number[] }[] = [];
  /** destination -> its last entry */
  private last = new Map<number, number>();
  add(f: Frame, dst: number) {
    if (f[P.F_KIND] === P.NET_KIND_UDP && dst !== P.NET_BROADCAST) {
      for (let i = (this.last.get(dst) ?? -1) + 1; i < this.e.length; i++) {
        const e = this.e[i];
        if (e.f.length === f.length && e.f[P.F_KIND] === P.NET_KIND_UDP && e.dst[0] !== P.NET_BROADCAST && e.dst.length < MULTI_MAX && sameBut(e.f, f)) {
          e.dst.push(dst); this.last.set(dst, i); return;
        }
      }
    }
    this.last.set(dst, this.e.length);
    this.e.push({ f, dst: [dst] });
  }
  /** the messages: one frame alone as it is, else bundles of NET_BUNDLE_MAX bytes at most */
  messages(): Frame[] {
    if (this.e.length === 1 && this.e[0].dst.length === 1) return [this.e[0].f];
    const out: Frame[] = [];
    let part: { f: Frame; dst: number[] }[] = [], bytes = 4;
    const done = () => {
      const b = new Uint8Array(bytes), dv = new DataView(b.buffer);
      b[0] = P.NET_KIND_BUNDLE;
      let at = 4;
      for (const { f, dst } of part) {
        const k = dst.length > 1 ? dst.length : 0;
        b[at] = k; dv.setUint16(at + 2, f.length);
        for (let j = 0; j < k; j++) dv.setUint32(at + 4 + 4 * j, dst[j]);
        b.set(f, at + 4 + 4 * k);
        at += 4 + 4 * k + f.length;
      }
      out.push(b); part = []; bytes = 4;
    };
    for (const x of this.e) {
      const n = 4 + (x.dst.length > 1 ? 4 * x.dst.length : 0) + x.f.length;
      if (part.length && bytes + n > P.NET_BUNDLE_MAX) done();
      part.push(x); bytes += n;
    }
    if (part.length) done();
    return out;
  }
}
/** two frames the same but for their destination (bytes 8-11) */
function sameBut(a: Uint8Array, b: Uint8Array) {
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i] && (i < P.F_DST || i >= P.F_DST + 4)) return false;
  return true;
}

/** A frame that never waits on purpose (a car state to a friend, LAN play's) waits no longer than this here, not each
 *  of them for seconds (the link's thread takes C's frames at once; the main thread's tasks are shorter): what C's
 *  stamps say over it, or under 0, is another clock's (Endpoint.clock) */
const CLOCK_STEP_MS = 50;
/** the clocks are compared over this long, and the time before */
const CLOCK_WINDOW_MS = 5000;

/** One mailbox (free roam's or LAN play's) in shared wasm memory. */
class Endpoint {
  i32: Int32Array; f64: Float64Array; dv: DataView; u8: Uint8Array;
  /** LAN: received TCP frames waiting for RX ring room, in order */
  waiting: ArrayBuffer[] = [];
  /** when its TX ring last went to the switch (performance.now()) */
  sent = 0;
  /** free roam: its game is not driving, as its last HELLO or SNAP said (a race, a shop, the menus, loading) */
  away = false;
  /** LAN play: when its TX ring got its first frame since the last drain (performance.now(); 0: empty) */
  firstAt = 0;
  /** this thread's clock less the clock of the game thread that stamps this mailbox's frames (S_T_MS), when the two
   *  differ by a step (CLOCK_STEP_MS), else 0: each reads performance.timeOrigin + performance.now(), and a context's
   *  time origin is the wall clock when it started, so one started after the wall clock was set (a time sync: WSL
   *  sets it about a second on every 35 s, and a test's link worker read 950 ms ahead of its game's threads; the page's
   *  own main thread, from its load to Play, the same) reads another time.  The ping and pong stamps (sentNow) would
   *  carry that difference.  From the frames' own stamps: the least time between C's stamp and the drain here, in the
   *  last CLOCK_WINDOW_MS or two */
  clock = 0;
  /** the clock the log last said */
  told = 0;
  private lo = Infinity;
  private loPrev = Infinity;
  private loAt = -Infinity;
  constructor(readonly kind: 'roam' | 'lan', buf: SharedArrayBuffer, ptr: number, size: number) {
    // shared memory is never detached: views made now stay valid after the
    // memory grows (the mailbox lies inside the current buffer)
    this.i32 = new Int32Array(buf, ptr, P.NET_HDR_BYTES / 4);
    this.f64 = new Float64Array(buf, ptr, P.NET_HDR_BYTES / 8);
    this.dv = new DataView(buf, ptr, size);
    this.u8 = new Uint8Array(buf, ptr, size);
  }
  mode() { return Atomics.load(this.i32, P.W_MODE); }
  /** C needs the switch now */
  wants() { return Atomics.load(this.i32, P.W_WANT) !== 0; }
  /** LAN play: the UDP ports its sockets are bound to, as bits (P.W_UDP_PORTS), or 0 when the module does not say */
  udpPorts() { return Atomics.load(this.i32, P.W_UDP_PORTS) >>> 0; }
  ring() { Atomics.add(this.i32, P.W_RX_BELL, 1); Atomics.notify(this.i32, P.W_RX_BELL); }
  /** a frame C stamped `at` (its clock) taken here at `t` (this thread's): how long it waited, in C's clock.  plain:
   *  a frame that never waits on purpose (free roam's car states to friends, LAN play's), which alone set `clock` */
  waited(t: number, at: number, plain: boolean): number {
    const d = t - at;
    if (plain) {
      if (t - this.loAt > CLOCK_WINDOW_MS) { this.loPrev = this.lo; this.lo = Infinity; this.loAt = t; }
      if (d < this.lo) this.lo = d;
      const m = Math.min(this.lo, this.loPrev);
      this.clock = m < 0 || m > CLOCK_STEP_MS ? m : 0;
    }
    return d - this.clock;
  }
  /** a received frame into the RX ring; false = the ring is full */
  put(b: ArrayBuffer): boolean {
    const i32 = this.i32, fv = new DataView(b), len = b.byteLength - P.F_DATA;
    const head = Atomics.load(i32, P.W_RX_HEAD), tail = Atomics.load(i32, P.W_RX_TAIL);
    if (((head - tail) >>> 0) >= P.NET_RX_SLOTS) return false;
    const s = i32[P.W_RX_OFF] + ((head >>> 0) % P.NET_RX_SLOTS) * P.NET_SLOT_BYTES, dv = this.dv;
    dv.setUint32(s + P.S_LEN, len, true);
    dv.setUint32(s + P.S_KIND, fv.getUint8(P.F_KIND), true);
    dv.setUint32(s + P.S_SRC_IP, fv.getUint32(P.F_SRC), true);
    dv.setUint32(s + P.S_DST_IP, fv.getUint32(P.F_DST), true);
    dv.setUint16(s + P.S_SRC_PORT, fv.getUint16(P.F_SPORT), true);
    dv.setUint16(s + P.S_DST_PORT, fv.getUint16(P.F_DPORT), true);
    dv.setFloat64(s + P.S_T_MS, now() - this.clock, true);
    this.u8.set(new Uint8Array(b, P.F_DATA, len), s + P.S_DATA);
    Atomics.store(i32, P.W_RX_HEAD, (head + 1) | 0);
    return true;
  }
}

/** what the link takes of the game process: its memory, its end and its mailboxes (worker.ts makes one of its own) */
export type NetProcess = Pick<GameProcess, 'memory' | 'exited' | 'registered'>;
/** how often, while the link is up, the thread it runs on is asked how late its tasks run (NetStats.loopLagMeanMs) */
const LOOP_LAG_MS = 250;

export class Link implements NetLink {
  state: NetState = 'waiting';
  vip: string | null = null;
  peers: NetPeer[] = [];
  online: Promise<string>;
  private resolveOnline!: (v: string) => void;
  private roam: Endpoint | null = null;
  private lan: Endpoint | null = null;
  private ws: WebSocket | null = null;
  private vipNum = 0;
  /** the switch's proof that vipNum is ours (welcome.token): only with it does a reconnect get the address back */
  private token = '';
  private netmask = 0xffffff00;
  private generation = 0;
  private backoff = 250;
  private closed = false;
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private retryTimer: ReturnType<typeof setTimeout> | null = null;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private idleTimer: ReturnType<typeof setTimeout> | null = null;
  /** whether LAN play is open, as the switch last heard it (hello or {"t":"lan"}) */
  private lanSent = false;
  /** the UDP ports the switch last heard (lanPorts), 0 none said */
  private udpSent = 0;
  /** the last body of each source's free-roam datagrams of each type in this connection's compact bundles (the
   *  switch's deltas are made against them: proto.ts unpackCompact) */
  private refs = new Map<number, Uint8Array>();
  /** friends' last car states, for the compact bundles' entries by their fields (proto.ts fpredTake) */
  private fp: P.FpredState = new Map();
  /** the last other datagrams (LAN play's UDP: races) it was sent by source and kind, which the switch's deltas of
   *  those are made against (proto.ts unpackCompact) */
  private others = new Map<number, Uint8Array>();
  /** while a bundle from the switch is taken apart: the endpoints to ring once it is */
  private ringLater: Set<Endpoint> | null = null;
  private pings = new Map<number, number>();
  private pingSeq = 0;
  private failed = 0;
  /** the link went without the page letting it go (onclose): the next welcome is onBack */
  private lost = false;
  private lastDownLog = -Infinity;
  /** the switch refused us as full (close 1013 "LAN full"), or put us in line: told once, and again when a place frees
   *  up */
  private full = false;
  /** our place in line (the switch's ticket for it: the same place again after a dropped link), and since when */
  private ticket = '';
  private queuedAt = 0;
  /** the player chose to stop waiting and play offline (goOffline), or the operator removed them: nothing takes the
   *  switch again this game */
  private stayOff = false;
  /** the switch's kick or ban message, until its close comes */
  private removal: NetRemoved | null = null;
  /** the race ban this player is under now (onRaces), and the name taken away the switch named before its close */
  private races: NetRaces | null = null;
  private renaming: NetRename | null = null;
  /** off the switch for a name taken away (and nothing since that keeps it off): rename brings it back */
  private offForName = false;
  /** the roam mailbox's solid word (setSolid): 0 not switched, 1 off, 2 on, 3 none */
  private solid = 0;
  /** quick chat's picks so far (say): the count in the roam mailbox's say word */
  private picks = 0;
  /** the roam mailbox's crews (setCrews): their hashes and colours, and how many times the page changed them (0: never) */
  private crews: number[] = [];
  private crewsRgb: number[] = [];
  private crewsGen = 0;
  /** the roam mailbox's room (setRoom): its name's UTF-8, and how many times the page changed it (0: never) */
  private room = new Uint8Array(0);
  private roomGen = 0;
  /** the switch's peer list (the online host on it too), as its last whole list and the changes since made it */
  private peerMap = new Map<number, { name: string; server: boolean }>();
  private peerVips = new Set<number>();
  /** the page's answers so far (AUTO_*): per sender (a vip of the switch, which writes the source), and all */
  private autoSrc = new Map<number, AutoBucket>();
  private autoAll = new AutoBucket();
  private st = { connections: 0, reconnects: 0, framesOut: 0, bytesOut: 0, framesIn: 0, bytesIn: 0, compactIn: 0, deltaIn: 0, rxFiltered: 0,
    rttSum: 0, rttN: 0, rttMax: 0, mainMs: 0, mainCalls: 0, rxFull: 0, rxOversize: 0,
    lanOut: 0, lanIn: 0, lanWaited: 0, lanWaitedMax: 0, lanRefused: 0, lanRefuseHeld: 0, lanGone: 0,
    lagSum: 0, lagN: 0, lagMax: 0, snapWaitSum: 0, snapWaitN: 0, snapWaitMax: 0 };
  /** the loop-lag sampler while the link is up (LOOP_LAG_MS): its channel, timer and when it last posted */
  private lagPort: MessagePort | null = null;
  private lagTimer: ReturnType<typeof setInterval> | null = null;
  private lagAt = 0;

  constructor(private proc: NetProcess, private o: NetOptions) {
    this.online = new Promise(r => (this.resolveOnline = r));
  }

  private log(s: string) { this.o.log?.(`[net] ${s}`); }
  private eps(): Endpoint[] { return [this.roam, this.lan].filter((e): e is Endpoint => !!e); }
  /** which mailboxes the link took (worker.ts tells the page) */
  attached() { return { roam: !!this.roam, lan: !!this.lan }; }
  /** the loop-lag sampler on (the socket opened) and off (it went): a message to itself every LOOP_LAG_MS */
  private lagStart() {
    if (this.lagTimer || typeof MessageChannel === 'undefined') return;
    const ch = new MessageChannel();
    ch.port1.onmessage = () => {
      const d = performance.now() - this.lagAt;
      this.st.lagSum += d; this.st.lagN++; if (d > this.st.lagMax) this.st.lagMax = d;
    };
    this.lagPort = ch.port1;
    this.lagTimer = setInterval(() => { this.lagAt = performance.now(); ch.port2.postMessage(0); }, LOOP_LAG_MS);
  }
  private lagStop() {
    if (this.lagTimer) { clearInterval(this.lagTimer); this.lagTimer = null; }
    // (an open port would keep node's test runs going)
    if (this.lagPort) { this.lagPort.onmessage = null; this.lagPort.close(); this.lagPort = null; }
  }

  start() {
    this.proc.exited.then(() => this.close());
    void this.attach(HostKind.Net, 'roam');
    void this.attach(HostKind.Lan, 'lan');
  }
  private async attach(kind: HostKind, name: 'roam' | 'lan') {
    const r = await this.proc.registered(kind);
    if (this.closed) return;
    const buf = this.proc.memory.buffer as unknown as SharedArrayBuffer;
    const i32 = new Int32Array(buf, r.ptr, 8);
    const magic = i32[P.W_MAGIC] >>> 0, ver = i32[P.W_VERSION];
    if (magic !== P.NET_MAGIC || ver !== P.NET_VERSION || r.size !== P.NET_MAILBOX_BYTES || i32[P.W_HDR_BYTES] !== P.NET_HDR_BYTES ||
        i32[P.W_SLOT_BYTES] !== P.NET_SLOT_BYTES || i32[P.W_TX_SLOTS] !== P.NET_TX_SLOTS || i32[P.W_RX_SLOTS] !== P.NET_RX_SLOTS) {
      this.log(`${name} mailbox does not match this page (magic ${magic.toString(16)}, version ${ver}, ${r.size} bytes): no ${name === 'lan' ? 'LAN play' : 'free roam'}`);
      return;
    }
    const ep = new Endpoint(name, buf, r.ptr, r.size);
    if (name === 'roam') this.roam = ep; else this.lan = ep;
    if (name === 'roam' && this.solid) Atomics.store(ep.i32, P.W_SOLID, this.solid);
    if (name === 'roam') { this.publishCrews(ep); this.publishRoom(ep); }
    if (name === 'lan') this.log('LAN play attached (the game opened its LAN screens)');
    this.o.onAttached?.(name);
    this.publishPeers(ep);
    if (this.state === 'online') this.publishOnline(ep);
    else Atomics.store(ep.i32, P.W_PAGE_STATE, this.state === 'connecting' ? P.NET_PAGE_CONNECTING
      : this.state === 'offline' || this.state === 'queued' ? P.NET_PAGE_OFFLINE
      : this.state === 'idle' || this.state === 'away' ? P.NET_PAGE_IDLE : P.NET_PAGE_NONE);
    this.checkWant();
    void this.txLoop(ep);
  }

  // ---- the switch connection ----
  private setPage(s: number) { for (const ep of this.eps()) Atomics.store(ep.i32, P.W_PAGE_STATE, s); }
  private publishOnline(ep: Endpoint) {
    Atomics.store(ep.i32, P.W_VIP, this.vipNum | 0);
    Atomics.store(ep.i32, P.W_NETMASK, this.netmask | 0);
    Atomics.store(ep.i32, P.W_GENERATION, this.generation);
    Atomics.store(ep.i32, P.W_PAGE_STATE, P.NET_PAGE_ONLINE);
    ep.ring();                                     // wake C's network thread so it sees the address now
  }
  private publishPeers(ep: Endpoint) {
    // bit h: the host h of our network
    const words = P.NET_PEER_BITS / 32, bits = new Uint32Array(words);
    for (const v of this.peerVips) {
      if (this.vipNum && ((v ^ this.vipNum) & this.netmask) !== 0) continue;
      const h = (v & ~this.netmask) >>> 0;
      if (h < words * 32) bits[h >>> 5] |= 1 << (h & 31);
    }
    for (let i = 0; i < words; i++) Atomics.store(ep.i32, P.W_PEER_BITS + i, bits[i] | 0);
    Atomics.store(ep.i32, P.W_SWITCH_PEERS, this.peers.length);
  }
  /** LAN play is open: the game's LAN or Online screens */
  private lanOpen() { return !!this.lan && this.lan.wants(); }
  /** the UDP ports LAN play's sockets are bound to (Endpoint.udpPorts): the switch sends a LAN broadcast only to a
   * page with a socket on its port, the others would drop it (0: the module does not say, every broadcast) */
  private lanPorts() { return (this.lan && (this.lan.udpPorts() & P.NET_UDP_PORTS_SET) ? this.lan.udpPorts() : 0); }
  /** connect when a mailbox wants the switch, let it go idleMs after none does, and tell the switch about LAN play */
  private checkWant() {
    if (this.closed) return;
    const want = this.eps().some(ep => ep.wants());
    if (want) {
      if (this.idleTimer) { clearTimeout(this.idleTimer); this.idleTimer = null; }
      if (!this.ws && (this.state === 'waiting' || this.state === 'idle')) this.connect();
    } else if (!this.idleTimer && this.state !== 'waiting' && this.state !== 'idle' && this.state !== 'away') {
      this.idleTimer = setTimeout(() => this.goIdle(), this.o.idleMs ?? 10_000);
    }
    const lan = this.lanOpen(), ws = this.ws;
    const udp = lan ? this.lanPorts() : 0;
    if ((lan !== this.lanSent || udp !== this.udpSent) && ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ t: 'lan', on: lan ? 1 : 0, ...(udp ? { udp } : {}) }));
      // the LAN or Online screens opened under a race ban: the player hears it again there
      if (lan && !this.lanSent && this.races) this.o.onRaces?.(this.races);
      this.lanSent = lan; this.udpSent = udp;
    }
  }
  /** nothing wanted the switch for idleMs: its place goes back (the switch frees the address at once) */
  private goIdle() {
    this.idleTimer = null;
    if (this.closed || this.state === 'idle' || this.state === 'away') return;
    // C stores want, then reads page_state; this stores page_state, then reads want: one of the two sees the other
    for (const ep of this.eps()) Atomics.store(ep.i32, P.W_PAGE_STATE, P.NET_PAGE_IDLE);
    if (this.eps().some(ep => ep.wants())) {
      if (this.state === 'online') for (const ep of this.eps()) this.publishOnline(ep);
      else this.setPage(this.state === 'connecting' ? P.NET_PAGE_CONNECTING : P.NET_PAGE_OFFLINE);
      return;
    }
    this.release('idle');
    this.log('no network feature in use: the place on the server goes back until one is');
  }
  /** the socket goes with 1000 "idle" (the switch frees the address at once) and C hears NET_PAGE_IDLE */
  private release(to: 'idle' | 'away') {
    if (this.idleTimer) { clearTimeout(this.idleTimer); this.idleTimer = null; }
    for (const ep of this.eps()) this.drainTx(ep);                 // what C queued last (free roam's BYEs)
    if (this.pingTimer) { clearInterval(this.pingTimer); this.pingTimer = null; }
    this.lagStop();
    if (this.reconnectTimer) { clearTimeout(this.reconnectTimer); this.reconnectTimer = null; }
    const ws = this.ws; this.ws = null;
    if (ws) { ws.onopen = null; ws.onmessage = null; ws.onerror = null; ws.onclose = null; ws.close(1000, 'idle'); }
    this.pings.clear();
    this.state = to; this.token = ''; this.full = false; this.backoff = 250; this.failed = 0;
    // (a page in line leaves it: its close is 1000 "idle")
    if (this.ticket || this.queuedAt) { this.ticket = ''; this.queuedAt = 0; this.o.onQueue?.(null); }
    this.setPage(P.NET_PAGE_IDLE);
    for (const ep of this.eps()) { Atomics.add(ep.i32, P.W_RX_BELL, 1); Atomics.notify(ep.i32, P.W_RX_BELL); }
  }
  /** the player stops waiting for a place (the page menu, a full server): the line and the switch go for the rest of
   *  this game, as for an away player who never comes back */
  goOffline() {
    if (this.closed || this.stayOff) return;
    this.stayOff = true;
    if (this.state !== 'away') this.release('away');
    this.log('the player stopped waiting for a place: no switch for the rest of this game');
  }
  /** the game: the player's car was in the air in free roam for `secs` (NET_KIND_FLYING, roam_game.c fly_frame): off
   *  the switch for the rest of this game, as an operator's kick takes a player, with FLYING_MSG over the game; the
   *  server hears it first ({"t":"flying"}: its log; a server of before lets it be) */
  private flying(secs: number) {
    if (this.closed || this.stayOff) return;
    const ws = this.ws;
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ t: 'flying', s: secs }));
    this.stayOff = true; this.offForName = false;
    if (this.state !== 'away') this.release('away');
    this.log(`the player's car was in the air for ${secs} s: no switch for the rest of this game ("${FLYING_MSG}")`);
    this.o.onRemoved?.({ ban: false, msg: FLYING_MSG });
  }
  /** the player picked another name after the operator took its name away (CLOSE_RENAME): back on the switch under it
   *  as soon as a mailbox wants it (a page off it for any other reason stays off) */
  rename(name: string) {
    if (this.closed || !this.offForName) return;
    this.o.name = name;
    this.stayOff = false; this.offForName = false; this.removal = null; this.renaming = null;
    if (this.state === 'away') this.state = 'idle';
    this.log('back to the switch under another name');
    this.checkWant();
  }
  setAway(on: boolean) {
    if (this.closed || this.stayOff || on === (this.state === 'away')) return;
    if (on) {
      this.release('away');
      this.log('the player is away: the place on the server goes back until they are here again');
    } else {
      this.state = 'idle';
      this.log('the player is here again');
      this.checkWant();
    }
  }
  setSolid(collide: 'on' | 'off' | 'none') {
    this.solid = collide === 'on' ? 2 : collide === 'none' ? 3 : 1;
    if (this.roam) Atomics.store(this.roam.i32, P.W_SOLID, this.solid);
    this.log(`solid cars ${collide === 'on' ? 'on' : collide === 'none' ? 'off, traffic and tuners pass through too' : 'off'} (the page menu)`);
  }
  /** quick chat: the player says phrase p (the game sends it while they drive in free roam, into the free-roam
   *  mailbox; race: in a race between players, into the LAN mailbox, whose game sends it to the race's players); false:
   *  no such mailbox */
  say(p: number, race = false): boolean {
    const ep = race ? this.lan : this.roam;
    if (!ep) return false;
    this.picks = (this.picks + 1) & 0xffffff;
    Atomics.store(ep.i32, P.W_SAY, (this.picks << 8 | (p & 0xff)) | 0);
    return true;
  }
  friend(m: Record<string, unknown>): boolean {
    const ws = this.ws;
    if (!ws || ws.readyState !== WebSocket.OPEN || this.state !== 'online') return false;
    ws.send(JSON.stringify({ t: 'friend', ...m }));
    return true;
  }
  setCrews(hashes: readonly number[], rgbs: readonly number[] = []) {
    this.crews = hashes.slice(0, P.CREW_WORDS);
    this.crewsRgb = rgbs.slice(0, P.CREW_WORDS).map(v => v & 0xffffff);
    this.crewsGen = (this.crewsGen + 1) >>> 0 || 1;
    if (this.roam) this.publishCrews(this.roam);
    this.log(`crews: ${this.crews.length} (the page menu)`);
  }
  /** the crews into the roam mailbox once the page changed them: their four words first, then the colours' four,
   *  then the count, which the game watches (it takes the words when the count moves). */
  private publishCrews(ep: Endpoint) {
    if (!this.crewsGen) return;
    for (let i = 0; i < P.CREW_WORDS; i++) Atomics.store(ep.i32, P.W_CREWS + i, (this.crews[i] ?? 0) | 0);
    for (let i = 0; i < P.CREW_WORDS; i++) Atomics.store(ep.i32, P.W_CREWS_RGB + i, this.crewsRgb[i] ?? 0);
    Atomics.store(ep.i32, P.W_CREWS_GEN, this.crewsGen | 0);
  }
  setRoom(name: string): boolean {
    let b = new TextEncoder().encode(name);
    // at most ROOM_BYTES, cut where a character starts (the settings keep a room to 63 bytes already)
    if (b.length > P.ROOM_BYTES) { let n = P.ROOM_BYTES; while (n > 0 && (b[n] & 0xc0) === 0x80) n--; b = b.slice(0, n); }
    this.room = b;
    this.roomGen = (this.roomGen + 1) >>> 0 || 1;
    if (!this.roam) return false;
    this.publishRoom(this.roam);
    this.log(`room: ${b.length ? 'changed' : 'none'} (the page)`);
    return true;
  }
  /** the room into the roam mailbox once the page changed it: its 16 words (the name, then 0 bytes), then the count,
   *  which the game watches. */
  private publishRoom(ep: Endpoint) {
    if (!this.roomGen) return;
    for (let i = 0; i < P.ROOM_WORDS; i++) {
      let w = 0;
      for (let k = 0; k < 4; k++) w |= (this.room[4 * i + k] ?? 0) << 8 * k;
      Atomics.store(ep.i32, P.W_ROOM_NAME + i, w | 0);
    }
    Atomics.store(ep.i32, P.W_ROOM_GEN, this.roomGen | 0);
  }
  private connect() {
    if (this.closed) return;
    const def = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/lan`;
    const u = new URL(this.o.url ?? def, location.href);
    // the same address again: the switch waits for the hello with the token (it keeps the address for it a minute), or
    // the same place in line with the ticket; queue: this page waits in line when the server is full (nfsu2-lan
    // queue.rs), where a page of before was refused (a switch of before ignores it)
    const resume = !!(this.vipNum && this.token) || !!this.ticket;
    u.searchParams.set('queue', '');
    if (resume) u.searchParams.set('resume', '');
    this.state = 'connecting'; this.setPage(P.NET_PAGE_CONNECTING);
    this.refs = new Map();
    this.fp = new Map();
    this.others = new Map();
    const ws = new WebSocket(u.href);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    ws.onopen = () => {
      // lan: LAN play is open, so it takes LAN broadcasts and the peer list (nfsu2-lan); bundle, compact: this page takes compact bundles; delta: and in those
      // a free-roam datagram as what changed since its source's last one (proto.ts unpackCompact; `refs`, kept for this
      // connection); fpred: and a friend's car state by its fields, rounded (proto.ts fpredTake; `fp`); ldelta: and
      // every other datagram (a race's) as what changed since its source's last one of its kind (`others`)
      this.lanSent = this.lanOpen();
      this.udpSent = this.lanSent ? this.lanPorts() : 0;
      ws.send(JSON.stringify({ t: 'hello', name: this.o.name ?? 'player', lan: this.lanSent ? 1 : 0, ...(this.udpSent ? { udp: this.udpSent } : {}), bundle: 1, compact: 1, delta: 1, fpred: 1, ldelta: 1,
        ...(this.o.id ? { id: this.o.id } : {}), ...(this.o.id && this.o.was?.length ? { was: this.o.was } : {}),
        ...(this.o.claim ? { claim: this.o.claim } : {}), ...(this.o.block?.length ? { block: this.o.block } : {}),
        ...(this.vipNum && this.token ? { vip: P.ipStr(this.vipNum), token: this.token } : {}), ...(this.ticket ? { ticket: this.ticket } : {}) }));
      this.pingTimer = setInterval(() => this.ping(), this.o.pingMs ?? 10_000);
      this.lagStart();
    };
    ws.onmessage = (e) => {
      if (typeof e.data === 'string') { let m: any; try { m = JSON.parse(e.data); } catch { return; } this.control(m); }
      else this.rx(e.data as ArrayBuffer);
    };
    ws.onclose = (e) => {
      if (this.ws !== ws) return;
      this.ws = null;
      if (this.pingTimer) { clearInterval(this.pingTimer); this.pingTimer = null; }
      this.lagStop();
      this.pings.clear();
      if (this.closed) return;
      if (e.code === CLOSE_KICKED || e.code === CLOSE_BANNED) {
        const r = this.removal ?? { ban: e.code === CLOSE_BANNED, msg: '' };
        this.removal = null;
        this.stayOff = true; this.offForName = false;
        this.release('away');
        this.log(`the operator ${r.ban ? `banned this player${r.until ? ` until ${new Date(r.until).toISOString()}` : ' for good'}` : 'kicked this player'}` +
          `${r.msg ? ` ("${r.msg}")` : ''}: no switch for the rest of this game`);
        this.o.onRemoved?.(r);
        return;
      }
      if (e.code === CLOSE_RENAME) {
        const r = this.renaming ?? { name: this.o.name ?? '', msg: '' };
        this.renaming = null;
        this.stayOff = true; this.offForName = true;
        this.release('away');
        this.log(`the operator took the name "${r.name}" away${r.msg ? ` ("${r.msg}")` : ''}: no switch until the player picks another`);
        this.o.onRename?.(r);
        return;
      }
      const was = this.state;
      this.state = 'offline'; this.setPage(P.NET_PAGE_OFFLINE);
      this.lost = true;
      if (e.code === 1013 && !this.full) {
        this.full = true;
        this.log('the server is full (LAN full); trying again until a place frees up');
        this.o.onNotice?.(N('The server is full: playing with others waits for a free place.'));
      }
      // one line when the link goes, then one every 30 s while it stays away
      const t = now();
      if (was === 'online' || was === 'queued' || t - this.lastDownLog > 30000) {
        this.log(was === 'online' ? `lost the switch (${e.code}${e.reason ? ' ' + e.reason : ''}); reconnecting`
          : was === 'queued' ? `lost the switch while in line (${e.code}); reconnecting to the same place in line (kept a minute)`
          : `still no switch (${this.failed} attempts); reconnecting every ${this.backoff / 1000} s`);
        this.lastDownLog = t;
      }
      this.failed++;
      this.reconnectTimer = setTimeout(() => { this.reconnectTimer = null; this.st.reconnects++; this.connect(); }, this.backoff);
      this.backoff = Math.min(this.backoff * 2, 5000);
    };
    ws.onerror = () => { /* onclose follows */ };
  }
  private control(m: any) {
    if (m.t === 'welcome' && typeof m.vip === 'number') {
      const vip = m.vip >>> 0, mask = P.ipNum(String(m.mask ?? '255.255.255.0')) ?? 0xffffff00;
      const again = this.vipNum === vip;
      this.vipNum = vip; this.vip = P.ipStr(vip); this.netmask = mask; this.generation++;
      this.token = typeof m.token === 'string' ? m.token : '';
      this.state = 'online'; this.backoff = 250; this.failed = 0; this.st.connections++;
      for (const ep of this.eps()) { this.publishPeers(ep); this.publishOnline(ep); }
      this.log(`on the virtual LAN as ${this.vip}${this.st.connections > 1 ? (again ? ' (the same address again)' : ' (a new address)') : ''}`);
      if (this.queuedAt) {
        const s = Math.round((now() - this.queuedAt) / 1000);
        this.log(`in from the line after ${s < 60 ? `${s} s` : `${Math.round(s / 60)} min`}`);
        this.ticket = ''; this.queuedAt = 0; this.o.onQueue?.(null);
      }
      if (this.full) { this.full = false; this.o.onNotice?.(N('A place on the server freed up: you can play with others now.')); }
      if (this.lost) { this.lost = false; this.o.onBack?.(); }
      this.resolveOnline(this.vip);
    } else if (m.t === 'queue' && Number.isInteger(m.pos) && Number.isInteger(m.of)) {
      // the server is full: in line for a place (its welcome comes when it is our turn)
      if (typeof m.ticket === 'string') this.ticket = m.ticket;
      if (this.state !== 'queued') {
        this.state = 'queued'; this.setPage(P.NET_PAGE_OFFLINE); this.backoff = 250; this.failed = 0;
        // (no notice: the line over the game, onQueue, says it)
        if (!this.queuedAt) {
          this.queuedAt = now(); this.full = true;
          this.log(`the server is full: in line for a place, ${m.pos} of ${m.of}`);
        } else this.log(`back in line, ${m.pos} of ${m.of}`);
      }
      this.o.onQueue?.({ pos: m.pos, of: m.of, ...(typeof m.eta === 'number' ? { eta: m.eta } : {}) });
    } else if (m.t === 'races') {
      // a race ban (or its end): the switch drops this player's races from now on; free roam goes on
      const r: NetRaces | null = m.lifted ? null : { msg: typeof m.msg === 'string' ? m.msg : '', ...(typeof m.until === 'number' ? { until: m.until } : {}) };
      const same = JSON.stringify(r) === JSON.stringify(this.races);
      this.races = r;
      if (!same) {
        this.log(r ? `the operator's race ban${r.until ? ` until ${new Date(r.until).toISOString()}` : ' for good'}${r.msg ? ` ("${r.msg}")` : ''}: no races with others`
          : 'the race ban was lifted: races with others again');
        this.o.onRaces?.(r);
      }
    } else if (m.t === 'rename') {
      // its close follows
      this.renaming = { name: typeof m.name === 'string' ? m.name : '', msg: typeof m.msg === 'string' ? m.msg : '' };
    } else if (m.t === 'kick' || m.t === 'ban') {
      // its close follows
      this.removal = { ban: m.t === 'ban', msg: typeof m.msg === 'string' ? m.msg : '', ...(typeof m.until === 'number' ? { until: m.until } : {}) };
    } else if (m.t === 'peers' && Array.isArray(m.peers)) {
      const old = this.peerMap;
      this.peerMap = new Map();
      for (const p of m.peers) this.peerMap.set(p.vip >>> 0, { name: String(p.name ?? ''), server: !!p.server });
      for (const v of old.keys()) if (!this.peerMap.has(v)) this.peerLeft(v);
      this.peersUpdated();
    } else if (m.t === 'delta' && Array.isArray(m.peers) && Array.isArray(m.gone)) {
      for (const g of m.gone) if (this.peerMap.delete(g >>> 0)) this.peerLeft(g >>> 0);
      for (const p of m.peers) this.peerMap.set(p.vip >>> 0, { name: String(p.name ?? ''), server: !!p.server });
      this.peersUpdated();
    } else if (m.t === 'friend') {
      this.o.onFriend?.(m);
    } else if (m.t === 'pong' && typeof m.id === 'number') {
      const t0 = this.pings.get(m.id);
      if (t0 === undefined) return;
      this.pings.delete(m.id);
      const rtt = now() - t0;
      for (const ep of this.eps()) ep.f64[P.D_SWITCH_RTT_MS] = rtt;
      this.st.rttSum += rtt; this.st.rttN++; if (rtt > this.st.rttMax) this.st.rttMax = rtt;
    }
  }
  /** a browser that left: its TCP connections to this one are reset (LAN play) */
  private peerLeft(vip: number) { if (vip !== this.vipNum) this.peerGone(vip); }
  private peersUpdated() {
    this.peers = [...this.peerMap].filter(([, p]) => !p.server).map(([v, p]) => ({ vip: P.ipStr(v), name: p.name, self: v === this.vipNum }));
    this.peerVips = new Set(this.peerMap.keys());
    for (const ep of this.eps()) this.publishPeers(ep);
    this.o.onPeers?.(this.peers);
  }
  private peerGone(vip: number) {
    const ep = this.lan;
    if (!ep || ep.mode() !== P.NET_MODE_LAN) return;
    this.st.lanGone++;
    const f = new Uint8Array(P.F_DATA + P.NET_LAN_HDR), fv = new DataView(f.buffer);
    f[P.F_KIND] = P.NET_KIND_LAN_TCP;
    fv.setUint32(P.F_SRC, vip); fv.setUint32(P.F_DST, this.vipNum);
    f[P.F_DATA] = P.NET_LAN_TCP_GONE;
    this.lanIn(ep, f.buffer, true);
  }
  private ping() {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    const id = ++this.pingSeq;
    this.pings.set(id, now());
    if (this.pings.size > 16) this.pings.delete(this.pings.keys().next().value!);
    this.ws.send(JSON.stringify({ t: 'ping', id }));
  }

  // ---- C -> switch ----
  private async txLoop(ep: Endpoint) {
    let sync = 0;                       // turns without a real wait: yield to the event loop now and then
    while (!this.closed) {
      const bell = Atomics.load(ep.i32, P.W_TX_BELL);
      this.checkWant();
      const hold = this.txHold(ep);
      if (!hold) this.drainTx(ep);
      const w = !hold && Atomics.load(ep.i32, P.W_TX_HEAD) !== Atomics.load(ep.i32, P.W_TX_TAIL) ? null
        : waitAsync(ep.i32, P.W_TX_BELL, bell, Math.min(hold || 1000, this.o.waitMs ?? 1000));
      if (w && w.async) { sync = 0; await w.value; }
      else if (++sync >= 32) { sync = 0; await new Promise(r => setTimeout(r, 0)); }
    }
  }
  /** how long free roam's TX ring may wait still (ms; 0: send it now): while it holds only free roam's own datagrams,
   *  until the next car state to a friend comes or TX_HOLD_MS after the last send, or TX_HOLD_AWAY_MS while the game
   *  is not driving (half the ring at most) */
  private txHold(ep: Endpoint): number {
    if (ep.kind === 'lan' && this.state === 'online') return this.lanHold(ep);
    if (ep.kind !== 'roam' || this.state !== 'online') return 0;
    const i32 = ep.i32, dv = ep.dv, txOff = i32[P.W_TX_OFF], head = Atomics.load(i32, P.W_TX_HEAD);
    let tail = Atomics.load(i32, P.W_TX_TAIL);
    if (tail === head || ((head - tail) | 0) >= P.NET_TX_SLOTS / 2) return 0;
    for (; tail !== head; tail = (tail + 1) | 0) {
      const s = txOff + ((tail >>> 0) % P.NET_TX_SLOTS) * P.NET_SLOT_BYTES, len = dv.getUint32(s + P.S_LEN, true);
      if (dv.getUint32(s + P.S_KIND, true) !== P.NET_KIND_UDP || len < 6) return 0;
      // a car state to a friend, or the word that the game drives again: now
      if (ep.away && roamAway(dv, s + P.S_DATA, len) === false) return 0;
      if (dv.getUint8(s + P.S_DATA + 5) === RC_MSG_SNAP && dv.getUint32(s + P.S_DST_IP, true) !== P.NET_ROAM_HUB) return 0;
    }
    const left = ep.sent + (ep.away ? TX_HOLD_AWAY_MS : TX_HOLD_MS) - performance.now();
    return left > 0 ? Math.ceil(left) : 0;
  }
  /** how long LAN play's TX ring may wait still (ms; 0: send it now): TX_HOLD_LAN_MS from its first frame (half the
   *  ring at most) */
  private lanHold(ep: Endpoint): number {
    const head = Atomics.load(ep.i32, P.W_TX_HEAD), tail = Atomics.load(ep.i32, P.W_TX_TAIL);
    if (tail === head || ((head - tail) | 0) >= P.NET_TX_SLOTS / 2) return 0;
    const t = performance.now();
    if (!ep.firstAt) ep.firstAt = t;
    const left = ep.firstAt + TX_HOLD_LAN_MS - t;
    return left > 0 ? Math.ceil(left) : 0;
  }
  private drainTx(ep: Endpoint) {
    ep.firstAt = 0;
    const i32 = ep.i32, dv = ep.dv;
    let tail = Atomics.load(i32, P.W_TX_TAIL);
    const head = Atomics.load(i32, P.W_TX_HEAD);
    if (tail === head) return;
    const c0 = performance.now();
    const txOff = i32[P.W_TX_OFF], ws = this.ws, open = !!ws && ws.readyState === WebSocket.OPEN && this.state === 'online';
    const t = now(), bundle = new TxBundle();
    let waitSum = 0, waitMax = ep.f64[P.D_TX_WAIT_MS_MAX], n = 0;
    while (tail !== head) {
      const s = txOff + ((tail >>> 0) % P.NET_TX_SLOTS) * P.NET_SLOT_BYTES;
      const len = dv.getUint32(s + P.S_LEN, true);
      if (dv.getUint32(s + P.S_KIND, true) === P.NET_KIND_NOTICE) {
        // for this page only, online or not; a copy: TextDecoder does not read shared memory
        if (len <= P.NET_MAX_PAYLOAD) {
          const t = new TextDecoder().decode(ep.u8.slice(s + P.S_DATA, s + P.S_DATA + len)), k = t.indexOf('\x1f');
          this.o.onNotice?.(k < 0 ? t : t.slice(0, k) + t.slice(k + 1), k < 0 ? undefined : t.slice(0, k));
        }
      } else if (dv.getUint32(s + P.S_KIND, true) === P.NET_KIND_LOBBY) {
        // the same: for this page only
        if (len <= P.NET_MAX_PAYLOAD) this.o.onLobby?.(new TextDecoder().decode(ep.u8.slice(s + P.S_DATA, s + P.S_DATA + len)));
      } else if (dv.getUint32(s + P.S_KIND, true) === P.NET_KIND_SAY) {
        // quick chat: the same
        if (len <= P.NET_MAX_PAYLOAD) this.o.onSay?.(new TextDecoder().decode(ep.u8.slice(s + P.S_DATA, s + P.S_DATA + len)));
      } else if (dv.getUint32(s + P.S_KIND, true) === P.NET_KIND_FRIEND) {
        // front bumpers: the same
        if (len <= P.NET_MAX_PAYLOAD) this.o.onBump?.(new TextDecoder().decode(ep.u8.slice(s + P.S_DATA, s + P.S_DATA + len)));
      } else if (dv.getUint32(s + P.S_KIND, true) === P.NET_KIND_BATTLE) {
        // a drift battle's scoreboard: the same
        if (len <= P.NET_MAX_PAYLOAD) this.o.onBattle?.(new TextDecoder().decode(ep.u8.slice(s + P.S_DATA, s + P.S_DATA + len)));
      } else if (dv.getUint32(s + P.S_KIND, true) === P.NET_KIND_FLYING) {
        // a car in the air for minutes: off the switch, once this drain is done (it sends what came before)
        let secs = 0;
        try { secs = Number(JSON.parse(new TextDecoder().decode(ep.u8.slice(s + P.S_DATA, s + P.S_DATA + Math.min(len, 64)))).s) || 0; } catch { /* the line says 0 */ }
        queueMicrotask(() => this.flying(secs));
      } else if (open && len <= P.NET_MAX_PAYLOAD) {
        const f = new Uint8Array(P.F_DATA + len), fv = new DataView(f.buffer), dst = dv.getUint32(s + P.S_DST_IP, true);
        f[P.F_KIND] = dv.getUint32(s + P.S_KIND, true);
        fv.setUint32(P.F_SRC, this.vipNum);                      // the switch writes the real one
        fv.setUint32(P.F_DST, dst);
        fv.setUint16(P.F_SPORT, dv.getUint16(s + P.S_SRC_PORT, true));
        fv.setUint16(P.F_DPORT, dv.getUint16(s + P.S_DST_PORT, true));
        f.set(ep.u8.subarray(s + P.S_DATA, s + P.S_DATA + len), P.F_DATA);   // a copy: a socket cannot send shared memory
        const roam = ep.kind === 'roam' && f[P.F_KIND] === P.NET_KIND_UDP;
        const snap = roam && len >= 6 && f[P.F_DATA + 5] === RC_MSG_SNAP && dst !== P.NET_ROAM_HUB;
        const w = ep.waited(t, dv.getFloat64(s + P.S_T_MS, true), snap || ep.kind === 'lan');
        if (roam) {
          sentNow(f, len, w);
          ep.away = roamAway(dv, s + P.S_DATA, len) ?? ep.away;
        }
        bundle.add(f, dst >>> 0);
        this.st.framesOut++; this.st.bytesOut += len;
        if (ep.kind === 'lan') this.st.lanOut++;
        if (w >= 0 && w < 60000) {
          waitSum += w; n++; if (w > waitMax) waitMax = w;
          if (snap) { this.st.snapWaitSum += w; this.st.snapWaitN++; if (w > this.st.snapWaitMax) this.st.snapWaitMax = w; }
        }
      } else Atomics.add(i32, P.W_TX_OFFLINE_DROP, 1);
      tail = (tail + 1) | 0;
      Atomics.store(i32, P.W_TX_TAIL, tail);
    }
    if (open) for (const m of bundle.messages()) ws!.send(m);
    if (open) ep.sent = performance.now();
    if (n) {
      ep.f64[P.D_TX_WAIT_MS_SUM] += waitSum; ep.f64[P.D_TX_WAIT_MS_MAX] = waitMax;
      Atomics.add(i32, P.W_TX_WAIT_N, n);
    }
    if (Math.abs(ep.clock - ep.told) > CLOCK_STEP_MS || (!ep.clock && ep.told)) {
      this.log(ep.clock ? `this thread's clock reads ${Math.abs(ep.clock).toFixed(0)} ms ${ep.clock > 0 ? 'ahead of' : 'behind'} the game's (${ep.kind === 'lan' ? 'LAN play' : 'free roam'}): its waits are read without that`
        : `this thread's clock and the game's agree again (${ep.kind === 'lan' ? 'LAN play' : 'free roam'})`);
      ep.told = ep.clock;
    }
    this.st.mainMs += performance.now() - c0; this.st.mainCalls++;
  }

  // ---- switch -> C ----
  private rx(b: ArrayBuffer) {
    const c0 = performance.now();
    const u = new Uint8Array(b);
    if (u.length >= 4 && u[0] === P.NET_KIND_COMPACT) {
      // each frame on its own, put together again first (proto.ts unpackCompact), C rung once for all of them
      const ring = this.ringLater = new Set();
      this.st.compactIn++;
      this.st.deltaIn += P.unpackCompact(u, this.vipNum, f => this.rxFrame(f), this.refs, this.fp, this.others);
      this.ringLater = null;
      for (const ep of ring) ep.ring();
    } else this.rxFrame(b);
    this.st.mainMs += performance.now() - c0; this.st.mainCalls++;
  }
  /** C's network thread hears of what came: now, or once the bundle it came in is taken apart */
  private rang(ep: Endpoint) { if (this.ringLater) this.ringLater.add(ep); else ep.ring(); }
  private rxFrame(b: ArrayBuffer) {
    if (b.byteLength < P.F_DATA) return;
    const kind = new Uint8Array(b, 0, 1)[0];
    const len = b.byteLength - P.F_DATA;
    if (len > P.NET_MAX_PAYLOAD) { this.st.rxOversize++; for (const ep of this.eps()) Atomics.add(ep.i32, P.W_RX_OVERSIZE, 1); return; }
    if (kind === P.NET_KIND_LAN_UDP || kind === P.NET_KIND_LAN_TCP || kind === P.NET_KIND_LAN_ICMP) { this.lanRx(b, kind); return; }
    // free roam: a player's endpoint takes only UDP, a test endpoint only the
    // emulated loopback (C checks port and address)
    const ep = this.roam, mode = ep ? ep.mode() : P.NET_MODE_CLOSED;
    if (!ep || (kind !== P.NET_KIND_UDP && kind !== P.NET_KIND_UDP_LOOP) || mode === P.NET_MODE_CLOSED ||
        (kind === P.NET_KIND_UDP) !== (mode === P.NET_MODE_VLAN)) { this.st.rxFiltered++; return; }
    if (!ep.put(b)) { Atomics.add(ep.i32, P.W_RX_FULL_DROP, 1); this.st.rxFull++; return; }
    this.rang(ep);
    this.st.framesIn++; this.st.bytesIn += len;
  }
  private lanRx(b: ArrayBuffer, kind: number) {
    const ep = this.lan;
    if (!ep || ep.mode() !== P.NET_MODE_LAN) {
      // no LAN endpoint in this game (it never opened its LAN screens): a
      // connect is refused, as by a host with no listener; the rest is dropped
      const d = new Uint8Array(b);
      if (kind === P.NET_KIND_LAN_TCP && b.byteLength >= P.F_DATA + P.NET_LAN_HDR && d[P.F_DATA] === P.NET_LAN_TCP_SYN) this.refuse(b);
      this.st.rxFiltered++;
      return;
    }
    this.st.framesIn++; this.st.bytesIn += b.byteLength - P.F_DATA; this.st.lanIn++;
    this.lanIn(ep, b, kind === P.NET_KIND_LAN_TCP);
  }
  /** into the LAN RX ring; TCP frames wait (in order) when it is full, others are dropped like UDP */
  private lanIn(ep: Endpoint, b: ArrayBuffer, keep: boolean) {
    if (ep.waiting.length && keep) { this.queue(ep, b); return; }
    if (ep.put(b)) { this.rang(ep); return; }
    if (keep) this.queue(ep, b);
    else { Atomics.add(ep.i32, P.W_RX_FULL_DROP, 1); this.st.rxFull++; }
  }
  private queue(ep: Endpoint, b: ArrayBuffer) {
    ep.waiting.push(b); this.st.lanWaited++;
    if (ep.waiting.length > this.st.lanWaitedMax) this.st.lanWaitedMax = ep.waiting.length;
    if (!this.retryTimer) this.retryTimer = setTimeout(() => this.flushWaiting(), 1);
  }
  private flushWaiting() {
    this.retryTimer = null;
    const ep = this.lan;
    if (!ep || this.closed) return;
    let n = 0;
    while (ep.waiting.length && ep.put(ep.waiting[0])) { ep.waiting.shift(); n++; }
    if (n) ep.ring();
    if (ep.waiting.length) this.retryTimer = setTimeout(() => this.flushWaiting(), 1);
  }
  private refuse(b: ArrayBuffer) {
    const ws = this.ws;
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const fv = new DataView(b), src = fv.getUint32(P.F_SRC), n = P.F_DATA + P.NET_LAN_HDR + AUTO_COST, t = performance.now() / 1000;
    let sb = this.autoSrc.get(src);
    if (!sb) this.autoSrc.set(src, (sb = new AutoBucket()));
    if (!sb.has(AUTO_SRC, n, t) || !this.autoAll.has(AUTO_ALL, n, t)) { this.st.lanRefuseHeld++; return; }
    sb.v -= n; this.autoAll.v -= n;
    const r = new Uint8Array(P.F_DATA + P.NET_LAN_HDR), rv = new DataView(r.buffer);
    r[P.F_KIND] = P.NET_KIND_LAN_TCP;
    rv.setUint32(P.F_SRC, this.vipNum); rv.setUint32(P.F_DST, src);
    rv.setUint16(P.F_SPORT, fv.getUint16(P.F_DPORT)); rv.setUint16(P.F_DPORT, fv.getUint16(P.F_SPORT));
    r[P.F_DATA] = P.NET_LAN_TCP_RST; r[P.F_DATA + 1] = 64;
    r.set(new Uint8Array(b, P.F_DATA + 4, 4), P.F_DATA + 4);              // the connection id
    ws.send(r);
    this.st.lanRefused++;
  }

  stats(): NetStats {
    const f = this.roam?.f64 ?? this.lan?.f64, s = this.st;
    let txn = 0, tws = 0, twm = 0, off = 0;
    for (const ep of this.eps()) {
      txn += Atomics.load(ep.i32, P.W_TX_WAIT_N); tws += ep.f64[P.D_TX_WAIT_MS_SUM];
      twm = Math.max(twm, ep.f64[P.D_TX_WAIT_MS_MAX]); off += Atomics.load(ep.i32, P.W_TX_OFFLINE_DROP);
    }
    return {
      state: this.state, vip: this.vip, connections: s.connections, reconnects: s.reconnects,
      framesOut: s.framesOut, bytesOut: s.bytesOut, framesIn: s.framesIn, bytesIn: s.bytesIn, compactIn: s.compactIn, deltaIn: s.deltaIn,
      txOfflineDrop: off, rxFullDrop: s.rxFull, rxOversize: s.rxOversize, rxFiltered: s.rxFiltered,
      switchRttMs: f ? f[P.D_SWITCH_RTT_MS] : 0, switchRttMeanMs: s.rttN ? s.rttSum / s.rttN : 0, switchRttMaxMs: s.rttMax,
      txWaitMeanMs: txn ? tws / txn : 0, txWaitMaxMs: twm,
      peersOnSwitch: this.peers.length, mainMs: s.mainMs, mainCalls: s.mainCalls,
      loopLagMeanMs: s.lagN ? s.lagSum / s.lagN : 0, loopLagMaxMs: s.lagMax,
      snapWaitMeanMs: s.snapWaitN ? s.snapWaitSum / s.snapWaitN : 0, snapWaitMaxMs: s.snapWaitMax,
      lan: { attached: !!this.lan, framesOut: s.lanOut, framesIn: s.lanIn, waited: s.lanWaited, waitedMax: s.lanWaitedMax,
        refusedNoEndpoint: s.lanRefused, refuseHeld: s.lanRefuseHeld, peersGone: s.lanGone },
    };
  }
  close() {
    if (this.closed) return;
    // send what C queued last (free roam's BYEs, LAN play's FIN/RST) before the socket goes
    for (const ep of this.eps()) this.drainTx(ep);
    this.closed = true; this.state = 'closed';
    if (this.queuedAt) { this.queuedAt = 0; this.o.onQueue?.(null); }
    if (this.pingTimer) { clearInterval(this.pingTimer); this.pingTimer = null; }
    this.lagStop();
    if (this.retryTimer) { clearTimeout(this.retryTimer); this.retryTimer = null; }
    if (this.reconnectTimer) { clearTimeout(this.reconnectTimer); this.reconnectTimer = null; }
    if (this.idleTimer) { clearTimeout(this.idleTimer); this.idleTimer = null; }
    // every handler goes: a closing socket lives on until the server answers the
    // close, and a handler would keep this link and the game's memory with it
    const ws = this.ws; this.ws = null;
    if (ws) { ws.onopen = null; ws.onmessage = null; ws.onerror = null; ws.onclose = null; ws.close(1000, 'closed'); }
    for (const ep of this.eps()) {
      Atomics.store(ep.i32, P.W_PAGE_STATE, P.NET_PAGE_NONE);
      Atomics.add(ep.i32, P.W_TX_BELL, 1); Atomics.notify(ep.i32, P.W_TX_BELL);   // ends txLoop's wait
    }
  }
}

const now = () => performance.timeOrigin + performance.now();   // the runtime's monotonic clock
// Atomics.waitAsync (ES2024; the project's TypeScript lib is ES2023)
const waitAsync = (Atomics as unknown as { waitAsync(a: Int32Array, i: number, v: number, t?: number):
  { async: false; value: 'not-equal' | 'timed-out' } | { async: true; value: Promise<'ok' | 'timed-out'> } }).waitAsync;

/** Attach the virtual LAN to a running game process, on this thread (the page's own: remote.ts attachNetWorker
 *  runs it in a worker instead).  Returns at once; the socket opens when the runtime registers its first net mailbox
 *  (free roam or LAN play). */
export function attachNet(proc: NetProcess, opts: NetOptions = {}): NetLink {
  const l = new Link(proc, opts);
  l.start();
  return l;
}
