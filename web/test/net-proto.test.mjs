// runtime/web/net_proto.h and web/src/net/proto.ts describe the same mailbox:
//   node --test web/test/net-proto.test.mjs
// Every NET_* number of the header equals the page's constant, and every
// offset the header asserts (_Static_assert(offsetof(...) == n)) is where the
// page reads that field.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as P from '../src/net/proto.ts';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const H = fs.readFileSync(path.join(HERE, '../../runtime/web/net_proto.h'), 'utf8');
// the header's NET_* numbers, each evaluated with the ones before it
const num = {};
const val = (e) => Function(...Object.keys(num), `return (${e.replace(/(\d+|0x[0-9a-fA-F]+)u\b/g, '$1')});`)(...Object.values(num));
for (const m of H.matchAll(/^#define\s+(NET_\w+)\s+(.+?)\s*(?:\/\*.*)?$/gm)) num[m[1]] = val(m[2]) >>> 0;

test('constants', () => {
  assert.equal(Object.keys(num).length, 40, 'header parsed');
  for (const [k, v] of Object.entries(num)) {
    if (!(k in P)) continue;
    assert.equal(P[k] >>> 0, v, k);
  }
  for (const k of ['NET_MAGIC', 'NET_VERSION', 'NET_HDR_BYTES', 'NET_SLOT_BYTES', 'NET_SLOT_HDR', 'NET_MAX_PAYLOAD', 'NET_TX_SLOTS',
    'NET_RX_SLOTS', 'NET_MAILBOX_BYTES', 'NET_KIND_UDP', 'NET_KIND_UDP_LOOP', 'NET_BROADCAST', 'NET_MODE_CLOSED', 'NET_MODE_VLAN',
    'NET_MODE_LOOP', 'NET_PAGE_NONE', 'NET_PAGE_CONNECTING', 'NET_PAGE_ONLINE', 'NET_PAGE_OFFLINE', 'NET_KIND_LAN_UDP',
    'NET_KIND_LAN_TCP', 'NET_KIND_LAN_ICMP', 'NET_KIND_NOTICE', 'NET_KIND_LOBBY', 'NET_KIND_SAY', 'NET_KIND_FRIEND', 'NET_LAN_HDR', 'NET_LAN_TCP_SYN', 'NET_LAN_TCP_RST', 'NET_LAN_TCP_GONE', 'NET_MODE_LAN', 'NET_ROAM_HUB',
    'NET_PAGE_IDLE', 'NET_PEER_BITS', 'NET_UDP_PORTS_SET'])
    assert.ok(k in P && k in num, `${k} on both sides`);
});

test('offsets', () => {
  // field order of net_mailbox: u32 fields every 4 bytes, doubles every 8
  const body = /typedef struct \{([\s\S]*?)\} net_mailbox;/.exec(H)[1];
  const off = {}; let o = 0;
  for (const line of body.split('\n')) {
    const m = /^\s*(uint32_t|double)\s+([^;]+);/.exec(line);
    if (!m) continue;
    const sz = m[1] === 'double' ? 8 : 4;
    for (const f of m[2].split(',').map(s => s.trim())) {
      const arr = /^(\w+)\[([^\]]+)\]$/.exec(f);
      if (arr) { off[arr[1]] = o; o += sz * val(arr[2]); } else { off[f] = o; o += sz; }
    }
  }
  assert.equal(o, P.NET_HDR_BYTES, 'header size');
  // the header's own assertions agree with this walk
  for (const m of H.matchAll(/offsetof\(net_mailbox, (\w+)\) == (\d+)/g)) assert.equal(off[m[1]], +m[2], m[1]);
  const words = { magic: P.W_MAGIC, version: P.W_VERSION, hdr_bytes: P.W_HDR_BYTES, slot_bytes: P.W_SLOT_BYTES, tx_slots: P.W_TX_SLOTS,
    rx_slots: P.W_RX_SLOTS, tx_off: P.W_TX_OFF, rx_off: P.W_RX_OFF, mode: P.W_MODE, port: P.W_PORT, tx_head: P.W_TX_HEAD,
    tx_bell: P.W_TX_BELL, rx_tail: P.W_RX_TAIL, c_alive: P.W_C_ALIVE, want: P.W_WANT, udp_ports: P.W_UDP_PORTS, page_state: P.W_PAGE_STATE, vip: P.W_VIP, netmask: P.W_NETMASK,
    generation: P.W_GENERATION, tx_tail: P.W_TX_TAIL, rx_head: P.W_RX_HEAD, rx_bell: P.W_RX_BELL, rx_full_drop: P.W_RX_FULL_DROP,
    tx_offline_drop: P.W_TX_OFFLINE_DROP, rx_oversize: P.W_RX_OVERSIZE, switch_peers: P.W_SWITCH_PEERS, solid: P.W_SOLID, tx_wait_n: P.W_TX_WAIT_N,
    say: P.W_SAY, peer_bits: P.W_PEER_BITS, crews_gen: P.W_CREWS_GEN, crews: P.W_CREWS, crews_rgb: P.W_CREWS_RGB,
    room_gen: P.W_ROOM_GEN, room_name: P.W_ROOM_NAME };
  for (const [f, w] of Object.entries(words)) assert.equal(off[f], w * 4, f);
  assert.equal(/uint32_t\s+room_gen,\s*room_name\[(\d+)\]/.exec(body)?.[1], String(P.ROOM_WORDS), 'the room\'s name: as many words');
  assert.equal(P.ROOM_BYTES + 1, P.ROOM_WORDS * 4, 'the longest name and its 0 byte fill the words');
  const crewArrays = /uint32_t\s+crews_gen,\s*crews\[(\d+)\],\s*crews_rgb\[(\d+)\]/.exec(body);
  assert.deepEqual(crewArrays?.slice(1), [String(P.CREW_WORDS), String(P.CREW_WORDS)], 'crews and their colours: as many words');
  const dbl = { switch_rtt_ms: P.D_SWITCH_RTT_MS, tx_wait_ms_sum: P.D_TX_WAIT_MS_SUM, tx_wait_ms_max: P.D_TX_WAIT_MS_MAX };
  for (const [f, d] of Object.entries(dbl)) assert.equal(off[f], d * 8, f);
  // net_slot
  const slot = { src_port: P.S_SRC_PORT, t_ms: P.S_T_MS };
  for (const m of H.matchAll(/offsetof\(net_slot, (\w+)\) == (\w+)/g))
    if (m[1] in slot) assert.equal(slot[m[1]], +m[2], m[1]);
  assert.equal(P.S_DATA, P.NET_SLOT_HDR);
  assert.deepEqual([P.S_LEN, P.S_KIND, P.S_SRC_IP, P.S_DST_IP, P.S_SRC_PORT, P.S_DST_PORT], [0, 4, 8, 12, 16, 18]);
  // the peer bits fit the header
  assert.equal(off.peer_bits + P.NET_PEER_BITS / 8 <= P.NET_HDR_BYTES, true);
});

test('addresses', () => {
  assert.equal(P.ipStr(P.ipNum('10.77.0.3')), '10.77.0.3');
  assert.equal(P.ipNum('255.255.255.0'), 0xffffff00);
  assert.equal(P.ipNum('10.77.0.256'), null);
  assert.equal(P.ipNum('host.local'), null);
});
