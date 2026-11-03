// The page's link reads C's waits on C's clock (web/src/net/index.ts Endpoint.clock): the slots' stamps (S_T_MS) are
// each thread's performance.timeOrigin + performance.now(), and a thread started after the computer's clock was set
// reads another time (WSL sets it about a second every 35 s; the link's worker started after the game's threads):
//   node --test web/test/net-clock.test.mjs
// - C's stamps a second behind or ahead of the link's clock: a free-roam ping's send time (sentNow) moves on by the
//   time it waited here, not by the second; the car states' waits (snapWaitMeanMs) are the link's own
// - a frame that comes is stamped on C's clock (C's page->C wait)
// - the clocks a few milliseconds apart (a frame that waited): read as they are
// With a fake game process (web/test/net-crews.test.mjs) and a fake switch (ws): hello -> welcome, then it takes
// what the page sends and sends one datagram.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { WebSocketServer } from 'ws';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export { attachNet } from './web/src/net/index.ts'; export * as P from './web/src/net/proto.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const { attachNet, P } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
globalThis.location ??= new URL('http://127.0.0.1/');      // the page's address (the link's default switch)

const NET = 4;                                   // HostKind.Net (web/src/host/abi.ts)
const PTR = 65536, PORT = 47300;
const VIP = 0x0a4d0002, FRIEND = 0x0a4d0003;     // 10.77.0.2, 10.77.0.3
const RC_MSG_SNAP = 3, RC_MSG_PING = 6;
const now = () => performance.timeOrigin + performance.now();
const tick = (ms = 0) => new Promise(r => setTimeout(r, ms));
async function until(f, ms = 3000) {
  for (const t0 = Date.now(); !f(); await tick(5)) if (Date.now() - t0 > ms) throw new Error('timed out');
}

function fakeProc() {
  const memory = new WebAssembly.Memory({ initial: 16, maximum: 16, shared: true });
  let exit; const exited = new Promise(r => (exit = r));
  const regs = new Map(), waiters = new Map();
  return {
    memory, exited, exit,
    registered(kind) {
      if (regs.has(kind)) return Promise.resolve(regs.get(kind));
      return new Promise(r => (waiters.get(kind) ?? waiters.set(kind, []).get(kind)).push(r));
    },
    register(kind, ptr, size) { regs.set(kind, { ptr, size }); for (const r of waiters.get(kind) ?? []) r({ ptr, size }); waiters.delete(kind); },
  };
}
/** free roam's mailbox as the runtime fills it: it wants the switch, on the virtual LAN */
function mailbox(proc) {
  const hdr = P.NET_HDR_BYTES, size = P.NET_MAILBOX_BYTES;
  const i32 = new Int32Array(proc.memory.buffer, PTR, size / 4), dv = new DataView(proc.memory.buffer, PTR, size);
  Object.assign(i32, { [P.W_MAGIC]: P.NET_MAGIC | 0, [P.W_VERSION]: P.NET_VERSION, [P.W_HDR_BYTES]: hdr, [P.W_SLOT_BYTES]: P.NET_SLOT_BYTES,
    [P.W_TX_SLOTS]: P.NET_TX_SLOTS, [P.W_RX_SLOTS]: P.NET_RX_SLOTS, [P.W_TX_OFF]: hdr, [P.W_RX_OFF]: hdr + P.NET_TX_SLOTS * P.NET_SLOT_BYTES,
    [P.W_MODE]: P.NET_MODE_VLAN, [P.W_PORT]: PORT, [P.W_WANT]: 1 });
  return { i32, dv };
}
/** a free-roam datagram of `type` (the core's header: "N2FR", byte 5 the type; a ping's send time t0 at 16, in us; a
 *  car state's status there, 2: driving in free roam), its CRC at the end */
function roamDgram(type, t0us = 0) {
  const d = new Uint8Array(48), v = new DataView(d.buffer);
  v.setUint32(0, 0x4e324652, false); d[5] = type;
  if (type === RC_MSG_PING) v.setUint32(16, t0us >>> 0, true);
  if (type === RC_MSG_SNAP) d[16] = 2;
  v.setUint32(d.length - 4, P.crc32(d, d.length - 4), true);
  return d;
}
/** C queues a datagram to the friend, stamped `at` (its clock), and rings */
function queue({ i32, dv }, data, at) {
  const head = Atomics.load(i32, P.W_TX_HEAD), s = i32[P.W_TX_OFF] + ((head >>> 0) % P.NET_TX_SLOTS) * P.NET_SLOT_BYTES;
  dv.setUint32(s + P.S_LEN, data.length, true); dv.setUint32(s + P.S_KIND, P.NET_KIND_UDP, true);
  dv.setUint32(s + P.S_SRC_IP, VIP, true); dv.setUint32(s + P.S_DST_IP, FRIEND, true);
  dv.setUint16(s + P.S_SRC_PORT, PORT, true); dv.setUint16(s + P.S_DST_PORT, PORT, true);
  dv.setFloat64(s + P.S_T_MS, at, true);
  new Uint8Array(dv.buffer, PTR + s + P.S_DATA, data.length).set(data);
  Atomics.store(i32, P.W_TX_HEAD, (head + 1) | 0);
  Atomics.add(i32, P.W_TX_BELL, 1); Atomics.notify(i32, P.W_TX_BELL);
}
/** the free-roam datagrams in a message from the page (a frame alone, or a bundle's entries) */
function dgrams(b) {
  if (b[0] !== P.NET_KIND_BUNDLE) return [b.subarray(P.F_DATA)];
  const out = [], v = new DataView(b.buffer, b.byteOffset, b.length);
  for (let at = 4; at < b.length;) {
    const k = b[at], n = v.getUint16(at + 2), f = at + 4 + 4 * k;
    out.push(b.subarray(f + P.F_DATA, f + n));
    at = f + n;
  }
  return out;
}
/** a fake switch: welcomes the page, keeps what it sends */
async function fakeSwitch() {
  const wss = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await new Promise(r => wss.once('listening', r));
  const got = [], socks = [];
  wss.on('connection', ws => {
    socks.push(ws);
    ws.on('message', (m, binary) => {
      if (!binary) { if (JSON.parse(String(m)).t === 'hello') ws.send(JSON.stringify({ t: 'welcome', vip: VIP, mask: '255.255.0.0', token: 't' })); }
      else got.push(...dgrams(new Uint8Array(m)));
    });
  });
  return { url: `ws://127.0.0.1:${wss.address().port}/lan`, got, socks, close: () => new Promise(r => wss.close(r)) };
}

for (const [label, off] of [['a second behind', -1000], ['a second ahead', 1000]]) {
  test(`C's clock ${label} the link's: its waits read on its own clock`, async () => {
    const sw = await fakeSwitch(), proc = fakeProc(), logs = [];
    const link = attachNet(proc, { url: sw.url, pingMs: 60_000, log: s => logs.push(s) });
    const mb = mailbox(proc);
    proc.register(NET, PTR, P.NET_MAILBOX_BYTES);
    try {
      await link.online;
      // car states, as the core sends them, each taken at once
      for (let i = 0; i < 5; i++) { queue(mb, roamDgram(RC_MSG_SNAP), now() + off); await tick(10); }
      await until(() => sw.got.filter(d => d[5] === RC_MSG_SNAP).length === 5);
      const s = link.stats();
      assert.ok(s.snapWaitMeanMs >= 0 && s.snapWaitMeanMs < 30, `car states waited ${s.snapWaitMeanMs} ms`);
      assert.ok(logs.some(l => l.includes(`clock reads`) && l.includes(off < 0 ? 'ahead of' : 'behind')), logs.join('\n'));
      // a ping waits here for the next car state (TX_HOLD_MS at most): its send time moves on by that, on C's clock
      queue(mb, roamDgram(RC_MSG_PING, 1_000_000), now() + off);
      await until(() => sw.got.some(d => d[5] === RC_MSG_PING));
      const ping = sw.got.find(d => d[5] === RC_MSG_PING), moved = new DataView(ping.buffer, ping.byteOffset).getUint32(16, true) - 1_000_000;
      assert.ok(moved > 0 && moved < 200_000, `the ping's send time moved ${moved} us`);
      // a datagram that comes: stamped on C's clock
      const f = new Uint8Array(P.F_DATA + 48), fv = new DataView(f.buffer);
      f[P.F_KIND] = P.NET_KIND_UDP; fv.setUint32(P.F_SRC, FRIEND); fv.setUint32(P.F_DST, VIP); fv.setUint16(P.F_SPORT, PORT); fv.setUint16(P.F_DPORT, PORT);
      f.set(roamDgram(RC_MSG_SNAP), P.F_DATA);
      const sent = now();
      sw.socks[0].send(f);
      await until(() => Atomics.load(mb.i32, P.W_RX_HEAD) === 1);
      const stamp = mb.dv.getFloat64(mb.i32[P.W_RX_OFF] + P.S_T_MS, true);
      assert.ok(Math.abs(stamp - (sent + off)) < 100, `stamped ${stamp - sent} ms from the link's clock`);
    } finally { link.close(); for (const ws of sw.socks) ws.terminate(); await sw.close(); }
  });
}

test('the clocks a few milliseconds apart: a wait is read as it is', async () => {
  const sw = await fakeSwitch(), proc = fakeProc(), logs = [];
  const link = attachNet(proc, { url: sw.url, pingMs: 60_000, log: s => logs.push(s) });
  const mb = mailbox(proc);
  proc.register(NET, PTR, P.NET_MAILBOX_BYTES);
  try {
    await link.online;
    // car states that waited 20 ms each (a busy thread): their waits, not another clock
    for (let i = 0; i < 5; i++) { queue(mb, roamDgram(RC_MSG_SNAP), now() - 20); await tick(10); }
    await until(() => sw.got.filter(d => d[5] === RC_MSG_SNAP).length === 5);
    const s = link.stats();
    assert.ok(s.snapWaitMeanMs >= 19 && s.snapWaitMeanMs < 50, `car states waited ${s.snapWaitMeanMs} ms`);
    assert.ok(!logs.some(l => l.includes('clock reads')), logs.join('\n'));
  } finally { link.close(); for (const ws of sw.socks) ws.terminate(); await sw.close(); }
});
