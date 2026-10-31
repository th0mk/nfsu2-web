// The page side of runtime/web/net_proto.h: the net mailboxes in shared wasm
// memory (free roam: HostKind.Net, LAN play: HostKind.Lan; one layout) and the
// frames on the server's virtual-LAN switch.  Same numbers
// as the C header (web/test/net-proto.test.mjs checks them).  Plain constants
// only, so Node can load this file as it is.

export const NET_MAGIC = 0x314e544e;          // "NTN1"
export const NET_VERSION = 4;                 // 2: peer_bits, the LAN mailbox; 3: want, peer_bits for a /22; 4: for a /21
export const NET_HDR_BYTES = 512;
export const NET_SLOT_BYTES = 1536;
export const NET_SLOT_HDR = 32;
export const NET_MAX_PAYLOAD = 1472;
export const NET_TX_SLOTS = 128;
export const NET_RX_SLOTS = 256;
export const NET_MAILBOX_BYTES = NET_HDR_BYTES + (NET_TX_SLOTS + NET_RX_SLOTS) * NET_SLOT_BYTES;

// frame kinds (byte 0 of a binary frame on the switch)
export const NET_KIND_UDP = 1;                // a UDP datagram between virtual addresses
export const NET_KIND_UDP_LOOP = 2;           // test harness: the emulated loopback (always to the broadcast address)
export const NET_KIND_LAN_UDP = 3;            // LAN play (Winsock): a UDP datagram
export const NET_KIND_LAN_TCP = 4;            // LAN play: a TCP segment of the virtual LAN's stream protocol
export const NET_KIND_LAN_ICMP = 5;           // LAN play: an ICMP message
export const NET_KIND_NOTICE = 16;            // TX ring only, never sent: a line of text the page shows over the game, a friend's name first, ended by 0x1F (none: the game's own line)
export const NET_KIND_LOBBY = 17;             // TX ring only, never sent: the free-roam race lobby's panel, its state as JSON ("{}": none)
export const NET_KIND_SAY = 18;               // TX ring only, never sent: quick chat, a phrase said (a friend's or the player's), as JSON
export const NET_KIND_FRIEND = 19;            // TX ring only, never sent: the player's car and a friend's touched front bumpers for 3 s (or parted after), as JSON
export const NET_KIND_BUNDLE = 32;            // page and switch only (nfsu2-lan): several frames in one message
export const NET_BUNDLE_MAX = 32 << 10;       // bytes in one bundle the page sends at most (the switch takes 64 KB)
export const NET_KIND_COMPACT = 33;           // switch to page only (nfsu2-lan compact.rs): a bundle without what the page puts back
export const NET_BROADCAST = 0xffffffff;
export const NET_ROAM_HUB = 0x0a4d00fd;       // 10.77.0.253: the server's free-roam hub (web/server-rs/crates/roam)

// LAN header: the first NET_LAN_HDR bytes of a NET_KIND_LAN_* payload
//   [0] op  [1] ttl  [2..3] 0  [4..7] connection id (TCP, little-endian)
export const NET_LAN_HDR = 8;
export const NET_LAN_TCP_SYN = 1;
export const NET_LAN_TCP_RST = 3;
export const NET_LAN_TCP_GONE = 6;            // from the page: the source address left the switch

// mode (written by C)
export const NET_MODE_CLOSED = 0;
export const NET_MODE_VLAN = 1;
export const NET_MODE_LOOP = 2;
export const NET_MODE_LAN = 3;

// page_state (written by the page)
export const NET_PAGE_NONE = 0;
export const NET_PAGE_CONNECTING = 1;
export const NET_PAGE_ONLINE = 2;
export const NET_PAGE_OFFLINE = 3;
export const NET_PAGE_IDLE = 4;               // nothing wanted the switch for a while: the page let it go

export const NET_PEER_BITS = 2048;            // peer_bits: one bit per host of the switch's network (at most a /21)

// header words: u32 index = byte offset / 4
export const W_MAGIC = 0, W_VERSION = 1, W_HDR_BYTES = 2, W_SLOT_BYTES = 3;
export const W_TX_SLOTS = 4, W_RX_SLOTS = 5, W_TX_OFF = 6, W_RX_OFF = 7;
export const W_MODE = 8, W_PORT = 9, W_TX_HEAD = 10, W_TX_BELL = 11;          // C -> page
export const W_RX_TAIL = 12, W_C_ALIVE = 13, W_WANT = 14;
// LAN play: NET_UDP_PORTS_SET and bit port % 31 of each UDP port a socket is bound to (0: an older module that does not say)
export const W_UDP_PORTS = 15;
export const NET_UDP_PORTS_SET = 0x80000000;
export const W_PAGE_STATE = 16, W_VIP = 17, W_NETMASK = 18, W_GENERATION = 19;   // page -> C
export const W_TX_TAIL = 20, W_RX_HEAD = 21, W_RX_BELL = 22, W_RX_FULL_DROP = 23;
export const W_TX_OFFLINE_DROP = 24, W_RX_OVERSIZE = 25, W_SWITCH_PEERS = 26;
export const W_SOLID = 27;                    // free roam's solid cars as the page's menu switched them: 0 not, 1 off, 2 on
export const W_TX_WAIT_N = 34;
export const W_SAY = 35;                      // quick chat: the phrase the player picked (bits 0-7) under a count that changes with each pick
export const W_PEER_BITS = 36;                // NET_PEER_BITS / 32 words: bit h = host h of our network is on the switch
// free roam's crews as the page's menu changed them (page -> C): W_CREWS_GEN counts the changes (0: none, the game keeps
// NFSU2_ROAM_CREW and NFSU2_ROAM_CREW_RGB), W_CREWS its 4 words, the crews' hashes in the player's order (0: none),
// W_CREWS_RGB their colours (0x00rrggbb; 0: the place's default); the crews, then the colours, then the count
export const W_CREWS_GEN = 100, W_CREWS = 101, W_CREWS_RGB = 105;
export const CREW_WORDS = 4;
// free roam's room as the page changed it (page -> C, an invite link): W_ROOM_GEN counts the
// changes (0: none, the game keeps NFSU2_ROAM_ROOM), W_ROOM_NAME its 16 words, the name's UTF-8 little-endian and
// ending in a 0 byte (at most ROOM_BYTES; none: no room); the name, then the count
export const W_ROOM_GEN = 109, W_ROOM_NAME = 110;
export const ROOM_WORDS = 16, ROOM_BYTES = 63;
// header doubles: f64 index = byte offset / 8
export const D_SWITCH_RTT_MS = 14, D_TX_WAIT_MS_SUM = 15, D_TX_WAIT_MS_MAX = 16;

// slot fields (byte offsets inside a slot; little-endian)
export const S_LEN = 0, S_KIND = 4, S_SRC_IP = 8, S_DST_IP = 12, S_SRC_PORT = 16, S_DST_PORT = 18, S_T_MS = 24, S_DATA = 32;

// a UDP frame on the switch (big-endian; web/server-rs/crates/lan)
//   [0] kind  [1..3] 0  [4..7] source vip (the switch writes it)  [8..11] destination vip
//   [12..13] source port  [14..15] destination port  [16..] the datagram
export const F_KIND = 0, F_SRC = 4, F_DST = 8, F_SPORT = 12, F_DPORT = 14, F_DATA = 16;

export function ipStr(v: number): string {
  return `${v >>> 24}.${(v >>> 16) & 255}.${(v >>> 8) & 255}.${v & 255}`;
}
export function ipNum(s: string): number | null {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(s.trim());
  if (!m) return null;
  const b = m.slice(1).map(Number);
  if (b.some(x => x > 255)) return null;
  return ((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]) >>> 0;
}

// ---- compact bundles (kind 33, nfsu2-lan compact.rs) ----
// The switch leaves out of each datagram what the page can put back exactly: the page's own address, free roam's
// port, and a free-roam datagram's "N2FR", version, length and CRC (it checked the CRC first).  Each entry: [0] flags
// (bits 0-2 the kind, 0x08 to the broadcast address, 0x10 the ports follow, 0x20 a free-roam datagram cut short,
// 0x40 the whole frame as it is, 0x80 with 0x20 what changed since the last one of its type from the same source),
// [1..2] the source's low 16 bits (10.77.x.y), [3..4] the length of what follows.  An 0x80 entry (only to a page whose
// hello said "delta":1): the type, a bitmap of the body's bytes (bit i % 8 of byte i / 8: byte i is new; the body as
// long as the last one), the new bytes in order.  Every 0x20 entry without 0x08 and 0x10 is its source's and type's
// last for the next; past COMPACT_REFS_MAX of them, a new one clears them all first, as the switch does.
// To a page whose hello said "ldelta":1 too, any other datagram to it (LAN play's UDP: races; the race horn's) may come
// as what changed since the last one of its kind from its source: 0x80 without 0x20 (bits 0-2 its kind; 0x10: its
// length, u16, follows, else it is as long as the last one), then a bitmap of its bytes from its ports on (bit i % 8
// of byte i / 8: byte i is new; past the last one's end always), the new bytes in order.  Every such datagram to the
// page's own address (not 0x08) of at most COMPACT_OTHER_MAX bytes from its ports on, as it came or not, is its
// source's and kind's last for the next, kept apart from free roam's (`others`, COMPACT_REFS_MAX of them too).
const C_BCAST = 0x08, C_PORTS = 0x10, C_ROAM = 0x20, C_WHOLE = 0x40, C_DELTA = 0x80, ROAM_PORT = 47210, ROAM_CUT = 11;
export const COMPACT_REFS_MAX = 256;
/** the longest other datagram (from its ports on) kept as its source's last (nfsu2-lan compact.rs OTHER_MAX) */
export const COMPACT_OTHER_MAX = 1500;
const CRC_TABLE = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c; }
  return t;
})();
/** zlib's CRC-32 of b's first n bytes (runtime/roam_core.c rc_crc32) */
export function crc32(b: Uint8Array, n: number): number {
  let c = -1;
  for (let i = 0; i < n; i++) c = CRC_TABLE[(c ^ b[i]) & 255] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}
// A friend's car state by its fields (an entry to a page whose hello said "fpred":1 too; nfsu2-lan fpred.rs, whose
// twin this is, says how): [0] C_FPRED, [1..2] the source's low 16 bits, [3] the length of what follows; a bitmap
// (LEB128) of the fields that differ from what they are taken to be, and those differences (zigzag LEB128).  The
// state is put back from its source's last one (the rounded one, as the game got it), with the position in mm, the
// velocity in cm/s and the time in steps of 256 us written back as floats and microseconds.  Integers only, as the
// switch does them: one division and a conversion to f32 for each float, the same in both.
export const C_FPRED = 0x61;
const FP_NF = 53, FP_T = 0, FP_SEQ = 1, FP_P = 2, FP_V = 5, FP_Q = 8, FP_W = 12, FP_SPEED = 15, FP_RPM = 16, FP_STEER = 17,
  FP_WZ = 18, FP_ENG = 22, FP_EACC = 23, FP_EDCL = 24, FP_THR = 25, FP_BRK = 26, FP_GEAR = 27, FP_BITS = 28, FP_NOS = 29,
  FP_HYD = 30, FP_THUMPS = 31, FP_EBASE = 32, FP_SLIP = 33, FP_XLEN = 45, FP_CLOCK = 46, FP_STATUS = 47, FP_EPOCH = 48,
  FP_CFG_REV = 49, FP_N = 50, FP_FLAGS = 51, FP_SENDER = 52;
/** a field's width when its difference wraps (bits), 0 for a signed one that does not */
const FP_WIDTH = (() => {
  const w = new Array<number>(FP_NF).fill(0);
  for (const i of [FP_RPM, FP_ENG, FP_SEQ, FP_CFG_REV, FP_FLAGS]) w[i] = 16;
  w[FP_T] = 24; w[FP_SENDER] = 32;
  for (const i of [FP_EACC, FP_EDCL, FP_THR, FP_BRK, FP_GEAR, FP_BITS, FP_NOS, FP_HYD, FP_THUMPS, FP_EBASE, FP_XLEN, FP_CLOCK,
    FP_STATUS, FP_EPOCH, FP_N]) w[i] = 8;
  for (let i = FP_SLIP; i < FP_SLIP + 12; i++) w[i] = 8;
  return w;
})();
/** a source's last state (as the page restored it), and the one before's time and sequence number */
type FpLast = { v: number[]; t2: number; seq2: number; two: boolean };
/** the last car states by source, for a page that takes them by their fields (kept for its connection) */
export type FpredState = Map<number, FpLast>;
const fpMod = (x: number, m: number) => ((x % m) + m) % m;
/** what an extension of x bytes holds past the wheels' heights and the nitrous (roam_core.c get_statex) */
function fpLayout(x: number): [boolean, boolean, boolean] | null {
  const hyd = x >= 11, slip = x >= 23, base = 9 + (hyd ? 2 : 0) + (slip ? 12 : 0), eng = x >= base + 5;
  return x >= 9 && base + (eng ? 5 : 0) === x ? [hyd, slip, eng] : null;
}
/** the value each field is taken to have (fpred.rs `predict`) */
function fpPredict(l: FpLast | undefined, t: number | null): number[] {
  if (!l) return new Array<number>(FP_NF).fill(0);
  const w = l.v.slice();
  if (l.two) { w[FP_T] = fpMod(2 * l.v[FP_T] - l.t2, 2 ** 24); w[FP_SEQ] = fpMod(2 * l.v[FP_SEQ] - l.seq2, 65536); }
  if (t !== null) {
    let dt = fpMod(t - l.v[FP_T], 2 ** 24);
    if (dt >= 2 ** 23) dt -= 2 ** 24;
    if (dt >= 0 && dt <= 4096) for (let i = 0; i < 3; i++) w[FP_P + i] = l.v[FP_P + i] + Math.floor((l.v[FP_V + i] * dt * 16 + 3125) / 6250);
  }
  return w;
}
const fpApply = (i: number, want: number, d: number) => FP_WIDTH[i] ? fpMod(want + d, 2 ** FP_WIDTH[i]) : want + d;
/** a state's body (the datagram from its sender's id on, without its CRC) from its fields; null: no layout */
function fpWrite(v: number[]): Uint8Array | null {
  const statex = (v[FP_FLAGS] & 0x40) !== 0, x = v[FP_XLEN], g = statex && x > 0 ? fpLayout(x) : null;
  if ((statex && x > 0 && !g) || v[FP_N] !== 1 || x < 0 || x > 255) return null;
  const b = new Uint8Array(66 + (statex ? 1 + x : 0)), d = new DataView(b.buffer);
  d.setUint32(0, v[FP_SENDER], true); d.setUint16(4, v[FP_SEQ], true); d.setUint16(6, v[FP_FLAGS], true);
  b[8] = v[FP_STATUS]; b[9] = v[FP_EPOCH]; d.setUint16(10, v[FP_CFG_REV], true); b[12] = 1; b[13] = v[FP_CLOCK];
  d.setUint32(14, v[FP_T] * 256, true);
  for (let i = 0; i < 3; i++) { d.setFloat32(18 + 4 * i, v[FP_P + i] / 1000, true); d.setFloat32(30 + 4 * i, v[FP_V + i] / 100, true); }
  for (let i = 0; i < 4; i++) d.setInt16(42 + 2 * i, v[FP_Q + i], true);
  for (let i = 0; i < 3; i++) d.setInt16(50 + 2 * i, v[FP_W + i], true);
  d.setInt16(56, v[FP_STEER], true); b[58] = v[FP_THR]; b[59] = v[FP_BRK]; d.setUint16(60, v[FP_RPM], true);
  d.setInt16(62, v[FP_SPEED], true); b[64] = v[FP_GEAR]; b[65] = v[FP_BITS];
  if (statex) {
    b[66] = x;
    if (g) {
      for (let i = 0; i < 4; i++) d.setInt16(67 + 2 * i, v[FP_WZ + i], true);
      b[75] = v[FP_NOS];
      let k = 76;
      if (g[0]) { b[76] = v[FP_HYD]; b[77] = v[FP_THUMPS]; k = 78; }
      if (g[1]) { for (let i = 0; i < 12; i++) b[78 + i] = v[FP_SLIP + i]; k = 90; }
      if (g[2]) { d.setUint16(k, v[FP_ENG], true); b[k + 2] = v[FP_EACC]; b[k + 3] = v[FP_EDCL]; b[k + 4] = v[FP_EBASE]; }
    }
  }
  return b;
}
/** an fpred entry's state from source src (e: what follows its 4-byte head) as its body; null when it is broken */
export function fpredTake(st: FpredState, src: number, e: Uint8Array): Uint8Array | null {
  let at = 0;
  const varint = (): number | null => {
    let u = 0, mul = 1;
    for (let k = 0; k < 10; k++) {
      if (at >= e.length) return null;
      const x = e[at++];
      u += (x & 0x7f) * mul; mul *= 128;
      if (!(x & 0x80)) return u;
    }
    return null;
  };
  const set: number[] = [];
  for (let g = 0; ; g++) {
    if (at >= e.length || g > 8) return null;
    const x = e[at++];
    for (let j = 0; j < 7; j++) if ((x >> j) & 1) { if (g * 7 + j >= FP_NF) return null; set.push(g * 7 + j); }
    if (!(x & 0x80)) break;
  }
  const d = new Array<number>(FP_NF).fill(0);
  for (const i of set) { const u = varint(); if (u === null) return null; d[i] = u % 2 === 1 ? -(u + 1) / 2 : u / 2; }
  if (at !== e.length) return null;
  const last = st.get(src), t = fpApply(FP_T, fpPredict(last, null)[FP_T], d[FP_T]), want = fpPredict(last, t);
  const cur = want.map((w, i) => fpApply(i, w, d[i]));
  const body = fpWrite(cur);
  if (!body) return null;
  if (!last && st.size >= COMPACT_REFS_MAX) st.clear();
  st.set(src, { v: cur, t2: last ? last.v[FP_T] : 0, seq2: last ? last.v[FP_SEQ] : 0, two: !!last });
  return body;
}

/** a compact bundle's frames, each put together again byte for byte as the switch had it (a car state by its fields
 *  rounded as the switch rounded it), to `take` in order; `own` the address this page holds; `refs` the last bodies
 *  (a free-roam datagram past its type) by source and type, for a page that takes deltas, and `fp` its friends' last
 *  car states, for one that takes them by their fields (both kept for its connection).  A broken entry ends it.
 *  `others` the last other datagrams (from their ports on) by source and kind, for one that takes those as deltas too.
 *  Returns how many came as deltas (those by their fields too). */
export function unpackCompact(u: Uint8Array, own: number, take: (f: ArrayBuffer) => void, refs?: Map<number, Uint8Array>,
  fp?: FpredState, others?: Map<number, Uint8Array>): number {
  const dv = new DataView(u.buffer, u.byteOffset, u.byteLength);
  let deltas = 0;
  for (let at = 4; at + 5 <= u.length;) {
    if (u[at] === C_FPRED) {
      const src = dv.getUint16(at + 1), end = at + 4 + u[at + 3];
      if (end > u.length || !fp) return deltas;
      const body = fpredTake(fp, src, u.subarray(at + 4, end));
      if (!body) return deltas;
      at = end; deltas++;
      take(roamFrame(3, body, src, own, false));
      continue;
    }
    const flags = u[at], src = dv.getUint16(at + 1), end = at + 5 + dv.getUint16(at + 3);
    let p = at + 5;
    if (end > u.length) return deltas;
    at = end;
    if (flags === C_WHOLE) { take(u.slice(p, end).buffer); continue; }
    const okey = src << 8 | (flags & 7);
    if ((flags & (C_DELTA | C_ROAM)) === C_DELTA) {
      // another datagram: what changed put on its source's last one of its kind, from its ports on
      const last = others?.get(okey);
      if (!last) return deltas;
      let n = last.length;
      if (flags & C_PORTS) { if (p + 2 > end) return deltas; n = dv.getUint16(p); p += 2; }
      let next = p + ((n + 7) >> 3);
      if (next > end) return deltas;
      const f = new Uint8Array(F_SPORT + n), fv = new DataView(f.buffer);
      for (let i = 0; i < n; i++) {
        if ((u[p + (i >> 3)] >> (i & 7)) & 1) f[F_SPORT + i] = u[next++];
        else if (i < last.length) f[F_SPORT + i] = last[i];
        else return deltas;
      }
      if (next !== end) return deltas;
      f[F_KIND] = flags & 7;
      fv.setUint32(F_SRC, (0x0a4d0000 | src) >>> 0);
      fv.setUint32(F_DST, own >>> 0);
      others!.set(okey, f.slice(F_SPORT));
      deltas++;
      take(f.buffer);
      continue;
    }
    const ports = (flags & C_PORTS) !== 0, roam = (flags & C_ROAM) !== 0, d = p + (ports ? 4 : 0);
    if (end < d) return deltas;
    // a free-roam datagram's type and body (as it came, or what changed put on its source's last one)
    let kind = 0, body: Uint8Array | null = null;
    if (roam) {
      if (end - d < 1) return deltas;
      kind = u[d];
      const key = src << 8 | kind;
      if (flags & C_DELTA) {
        const last = refs?.get(key);
        if (!last) return deltas;
        let next = d + 1 + ((last.length + 7) >> 3);
        if (next > end) return deltas;
        body = new Uint8Array(last.length);
        for (let i = 0; i < last.length; i++) body[i] = (u[d + 1 + (i >> 3)] >> (i & 7)) & 1 ? u[next++] : last[i];
        if (next !== end) return deltas;
        deltas++;
      } else body = u.slice(d + 1, end);
      if (refs && !ports && !(flags & C_BCAST)) {
        if (refs.size >= COMPACT_REFS_MAX && !refs.has(key)) refs.clear();
        refs.set(key, body);
      }
    }
    if (body && !ports && (flags & 7) === 1) { take(roamFrame(kind, body, src, own, (flags & C_BCAST) !== 0)); continue; }
    const len = body ? 1 + body.length + ROAM_CUT : end - d, f = new Uint8Array(F_DATA + len), fv = new DataView(f.buffer);
    f[F_KIND] = flags & 7;
    fv.setUint32(F_SRC, (0x0a4d0000 | src) >>> 0);
    fv.setUint32(F_DST, flags & C_BCAST ? NET_BROADCAST : own >>> 0);
    if (ports) f.set(u.subarray(p, p + 4), F_SPORT);
    else { fv.setUint16(F_SPORT, ROAM_PORT); fv.setUint16(F_DPORT, ROAM_PORT); }
    if (body) roamDatagram(f, len, kind, body);
    else {
      f.set(u.subarray(d, end), F_DATA);
      if (others && !(flags & C_BCAST) && f.length - F_SPORT <= COMPACT_OTHER_MAX) {
        if (others.size >= COMPACT_REFS_MAX && !others.has(okey)) others.clear();
        others.set(okey, f.slice(F_SPORT));
      }
    }
    take(f.buffer);
  }
  return deltas;
}
/** a free-roam datagram ("N2FR", version 1, its type, its length, the body, the CRC) into f at F_DATA, len its bytes */
function roamDatagram(f: Uint8Array, len: number, kind: number, body: Uint8Array) {
  const fv = new DataView(f.buffer);
  f.set([0x4e, 0x32, 0x46, 0x52, 1, kind], F_DATA);
  fv.setUint16(F_DATA + 6, len, true);
  f.set(body, F_DATA + 8);
  fv.setUint32(F_DATA + len - 4, crc32(f.subarray(F_DATA), len - 4), true);
}
/** a frame of kind 1 on free roam's ports from 10.77.<src> to this page (or the broadcast address) around a datagram */
function roamFrame(kind: number, body: Uint8Array, src: number, own: number, bcast: boolean): ArrayBuffer {
  const len = 1 + body.length + ROAM_CUT, f = new Uint8Array(F_DATA + len), fv = new DataView(f.buffer);
  f[F_KIND] = 1;
  fv.setUint32(F_SRC, (0x0a4d0000 | src) >>> 0);
  fv.setUint32(F_DST, bcast ? NET_BROADCAST : own >>> 0);
  fv.setUint16(F_SPORT, ROAM_PORT); fv.setUint16(F_DPORT, ROAM_PORT);
  roamDatagram(f, len, kind, body);
  return f.buffer;
}
