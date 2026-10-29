// Seven tests of the page (web/src/net/index.ts, bundled as the app bundles it) against the Rust switch, started by
// tests/page.rs, which passes its ports.  What they read of the switch's insides (its peers, info(), reserved) comes
// from the harness's GET /state.
//   node page.mjs <case> <port> [<port> ...]
import assert from 'node:assert/strict';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';
import { WebSocket } from 'ws';
import * as esbuild from 'esbuild';
import * as P from '../../../../src/net/proto.ts';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const WEB = path.join(HERE, '../../../..');
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const ipStr = (v) => [v >>> 24, (v >>> 16) & 255, (v >>> 8) & 255, v & 255].join('.');

/** the switch's insides, as these tests read them: { peers: [{ vip, name, hello, lan }], info, reserved } */
const state = async (port) => (await fetch(`http://127.0.0.1:${port}/state`)).json();
/** until f() says yes (5 s at most), its condition read from the switch (async) */
const until = async (f, ms = 5000) => { const t0 = Date.now(); while (!(await f())) { if (Date.now() - t0 > ms) throw new Error('timed out'); await sleep(20); } };

// a fake browser on the switch's WebSocket (what these tests need of one)
class Client {
  constructor(port, name, query = '', resume = null, { deltas = false, lan, bundle = false } = {}) {
    this.name = name; this.frames = []; this.ctl = []; this.waiters = []; this.resume = resume; this.deltas = deltas;
    this.extra = { ...(deltas ? { deltas: 1 } : {}), ...(lan !== undefined ? { lan } : {}), ...(bundle ? { bundle: 1 } : {}) };
    this.view = null; this.ctlBytes = 0;
    const q = resume ? (query ? query + '&resume' : '?resume') : query;
    this.ws = new WebSocket(`ws://127.0.0.1:${port}/lan${q}`);
    this.ws.binaryType = 'nodebuffer';
    this.ws.on('message', (d, bin) => {
      if (bin) this.frames.push({ buf: Buffer.from(d), t: performance.now() });
      else {
        const m = JSON.parse(String(d));
        this.ctl.push(m); this.ctlBytes += d.length;
        if (m.t === 'peers') this.view = new Map(m.peers.map(p => [p.vip >>> 0, p]));
        if (m.t === 'delta' && this.view) { for (const v of m.gone) this.view.delete(v >>> 0); for (const p of m.peers) this.view.set(p.vip >>> 0, p); }
      }
      for (const w of this.waiters.splice(0)) w();
    });
    this.closed = new Promise(r => this.ws.on('close', r));
  }
  async welcome() {
    const m = await this.until(() => this.ctl.find(m => m.t === 'welcome'));
    this.vip = m.vip >>> 0; this.token = m.token;
    if (!this.resume) this.ws.send(JSON.stringify({ t: 'hello', name: this.name, ...this.extra }));
    return m;
  }
  until(f, ms = 2000) {
    return new Promise((res, rej) => {
      const t = setTimeout(() => rej(new Error(`${this.name}: timed out`)), ms);
      const check = () => { const v = f(); if (v) { clearTimeout(t); res(v); } else this.waiters.push(check); };
      check();
    });
  }
  sendBundle(entries) { this.ws.send(bundleOf(entries)); }
  dgrams() {
    const out = [];
    this.frames.forEach(({ buf }, msg) => {
      const one = b => out.push({ buf: b, msg, kind: b[0], src: b.readUInt32BE(4), dst: b.readUInt32BE(8), data: b.subarray(16).toString() });
      if (buf[0] !== 32) return one(buf);
      for (let at = 4; at + 4 <= buf.length;) { const f = at + 4 + 4 * buf[at], end = f + buf.readUInt16BE(at + 2); one(buf.subarray(f, end)); at = end; }
    });
    return out;
  }
  close() { this.ws.close(); return this.closed; }
}
function frameOf(dst, data, kind = 1, sport = 9999, dport = 9999) {
  const f = Buffer.alloc(16 + Buffer.byteLength(data));
  f[0] = kind; f.writeUInt32BE(dst >>> 0, 8); f.writeUInt16BE(sport, 12); f.writeUInt16BE(dport, 14); Buffer.from(data).copy(f, 16);
  return f;
}
function bundleOf(entries) {
  const parts = [Buffer.from([32, 0, 0, 0])];
  for (const { f, to = [] } of entries) {
    const h = Buffer.alloc(4 + 4 * to.length);
    h[0] = to.length; h.writeUInt16BE(f.length, 2); to.forEach((v, j) => h.writeUInt32BE(v >>> 0, 4 + 4 * j));
    parts.push(h, f);
  }
  return Buffer.concat(parts);
}

// the page (web/src/net/index.ts), bundled as the app bundles it, against a switch on port
let pageJs = null;
async function pageNet(port) {
  pageJs ??= (await esbuild.build({ entryPoints: [path.join(WEB, 'src/net/index.ts')], bundle: true, write: false, format: 'esm', platform: 'neutral' })).outputFiles[0].text;
  globalThis.location ??= new URL(`http://127.0.0.1:${port}/`);
  return import('data:text/javascript;base64,' + Buffer.from(pageJs).toString('base64'));
}
function mailbox(memory, ptr, ver = P.NET_VERSION) {
  const hdr = ver === 2 ? P.NET_V2_HDR_BYTES : P.NET_HDR_BYTES, w = new Int32Array(memory.buffer, ptr, hdr / 4);
  w[P.W_MAGIC] = P.NET_MAGIC; w[P.W_VERSION] = ver; w[P.W_HDR_BYTES] = hdr; w[P.W_SLOT_BYTES] = P.NET_SLOT_BYTES;
  w[P.W_TX_SLOTS] = P.NET_TX_SLOTS; w[P.W_RX_SLOTS] = P.NET_RX_SLOTS;
  w[P.W_TX_OFF] = hdr; w[P.W_RX_OFF] = hdr + P.NET_TX_SLOTS * P.NET_SLOT_BYTES;
  return { w, ptr, size: ver === 2 ? P.NET_V2_MAILBOX_BYTES : P.NET_MAILBOX_BYTES };
}
function want(mb, on) { Atomics.store(mb.w, P.W_WANT, on); Atomics.add(mb.w, P.W_TX_BELL, 1); Atomics.notify(mb.w, P.W_TX_BELL); }
const fakeGame = (memory, boxes) => ({ memory, ctl: new Int32Array(1), exited: new Promise(() => {}),
  registered: k => boxes[k] ? Promise.resolve({ ptr: boxes[k].ptr, size: boxes[k].size }) : new Promise(() => {}) });
function txPush(memory, mb, { kind = 1, dst, sport = 47210, dport = 47210, data }) {
  const w = mb.w, dv = new DataView(memory.buffer), head = Atomics.load(w, P.W_TX_HEAD);
  const s = mb.ptr + w[P.W_TX_OFF] + ((head >>> 0) % P.NET_TX_SLOTS) * P.NET_SLOT_BYTES, d = Buffer.from(data);
  dv.setUint32(s + P.S_LEN, d.length, true); dv.setUint32(s + P.S_KIND, kind, true); dv.setUint32(s + P.S_DST_IP, dst >>> 0, true);
  dv.setUint16(s + P.S_SRC_PORT, sport, true); dv.setUint16(s + P.S_DST_PORT, dport, true);
  dv.setFloat64(s + P.S_T_MS, performance.timeOrigin + performance.now(), true);
  new Uint8Array(memory.buffer).set(d, s + P.S_DATA);
  Atomics.store(w, P.W_TX_HEAD, (head + 1) | 0);
}
const txBell = mb => { Atomics.add(mb.w, P.W_TX_BELL, 1); Atomics.notify(mb.w, P.W_TX_BELL); };
function rxTake(memory, mb) {
  const w = mb.w, dv = new DataView(memory.buffer), out = [];
  let tail = Atomics.load(w, P.W_RX_TAIL);
  for (const head = Atomics.load(w, P.W_RX_HEAD); tail !== head; tail = (tail + 1) | 0) {
    const s = mb.ptr + w[P.W_RX_OFF] + ((tail >>> 0) % P.NET_RX_SLOTS) * P.NET_SLOT_BYTES;
    out.push({ src: dv.getUint32(s + P.S_SRC_IP, true), dst: dv.getUint32(s + P.S_DST_IP, true),
      data: Buffer.from(new Uint8Array(memory.buffer, s + P.S_DATA, dv.getUint32(s + P.S_LEN, true))).toString() });
  }
  Atomics.store(w, P.W_RX_TAIL, tail);
  return out;
}
/** the same with each datagram's bytes and ports, as the game reads them */
function rxTakeRaw(memory, mb) {
  const w = mb.w, dv = new DataView(memory.buffer), out = [];
  let tail = Atomics.load(w, P.W_RX_TAIL);
  for (const head = Atomics.load(w, P.W_RX_HEAD); tail !== head; tail = (tail + 1) | 0) {
    const s = mb.ptr + w[P.W_RX_OFF] + ((tail >>> 0) % P.NET_RX_SLOTS) * P.NET_SLOT_BYTES;
    out.push({ src: dv.getUint32(s + P.S_SRC_IP, true), dst: dv.getUint32(s + P.S_DST_IP, true),
      sport: dv.getUint16(s + P.S_SRC_PORT, true), dport: dv.getUint16(s + P.S_DST_PORT, true),
      data: Buffer.from(new Uint8Array(memory.buffer, s + P.S_DATA, dv.getUint32(s + P.S_LEN, true))) });
  }
  Atomics.store(w, P.W_RX_TAIL, tail);
  return out;
}
/** a free-roam datagram as runtime/roam_core.c rc_encode makes it: "N2FR", version 1, a type, its length and CRC */
function roamDatagram(type, n) {
  const d = Buffer.alloc(16 + n + 4);
  d.write('N2FR', 0, 'latin1'); d[4] = 1; d[5] = type; d.writeUInt16LE(d.length, 6); d.writeUInt32LE(0x12345678, 8); d[12] = 7;
  for (let i = 0; i < n; i++) d[16 + i] = (i * 37 + 11) & 255;
  d.writeUInt32LE(zlib.crc32(d.subarray(0, d.length - 4)), d.length - 4);
  return d;
}

/** a SNAP of one state as roam_core.c rc_encode makes it (its fields from k, so that each differs), with an extension
 *  of x bytes when x is given */
function snapDatagram({ sender = 0xABCD0001, seq, t, p, v, k, x }) {
  const b = 8, d = Buffer.alloc(b + 66 + (x !== undefined ? 1 + x : 0) + 4);
  d.write('N2FR', 0, 'latin1'); d[4] = 1; d[5] = 3; d.writeUInt16LE(d.length, 6);
  d.writeUInt32LE(sender, b); d.writeUInt16LE(seq & 0xffff, b + 4); d.writeUInt16LE((x !== undefined ? 0x40 : 0) | 0x20, b + 6);
  d[b + 8] = 2; d[b + 9] = 7; d.writeUInt16LE(0x0102, b + 10); d[b + 12] = 1; d[b + 13] = 3;
  d.writeUInt32LE(t >>> 0, b + 14);
  p.forEach((f, i) => d.writeFloatLE(f, b + 18 + 4 * i)); v.forEach((f, i) => d.writeFloatLE(f, b + 30 + 4 * i));
  [100 + k, -200, 30000 - k, 5].forEach((q, i) => d.writeInt16LE(q, b + 42 + 2 * i));
  [3 * k, -k, 7].forEach((w, i) => d.writeInt16LE(w, b + 50 + 2 * i));
  d.writeInt16LE((k * 101) << 16 >> 16, b + 56); d[b + 58] = 200; d[b + 59] = k % 3; d.writeUInt16LE((3000 + 7 * k) & 0xffff, b + 60);
  d.writeInt16LE(1500 + k, b + 62); d[b + 64] = 4; d[b + 65] = 0x10;
  if (x !== undefined) {
    d[b + 66] = x;
    if (x > 0) {
      for (let i = 0; i < 4; i++) d.writeInt16LE(-300 + k + i, b + 67 + 2 * i);
      let o = b + 76;
      if (x >= 11) { d[o] = 1; d[o + 1] = 2; o += 2; }
      if (x >= 23) { for (let i = 0; i < 12; i++) d[o + i] = (i * 9 + k) & 255; o += 12; }
      if (x === 16 || x === 28) { d.writeUInt16LE(4000 + k, o); d[o + 2] = 10; d[o + 3] = 20; d[o + 4] = 30; }
    }
  }
  d.writeUInt32LE(zlib.crc32(d.subarray(0, d.length - 4)), d.length - 4);
  return d;
}
/** the same as the game gets it through the fields (nfsu2-lan fpred.rs): the time to 256 us, the position to 1 mm, the
 *  velocity to 1 cm/s (rounded half away from zero, as Rust's round), and the CRC made again */
function roundedDatagram(d) {
  const r = Buffer.from(d), b = 8, away = x => Math.sign(x) * Math.round(Math.abs(x)) + 0;
  r.writeUInt32LE((Math.floor((r.readUInt32LE(b + 14) + 128) / 256) % 2 ** 24) * 256, b + 14);
  for (let i = 0; i < 3; i++) {
    r.writeFloatLE(away(r.readFloatLE(b + 18 + 4 * i) * 1000) / 1000, b + 18 + 4 * i);
    r.writeFloatLE(away(r.readFloatLE(b + 30 + 4 * i) * 100) / 100, b + 30 + 4 * i);
  }
  r.writeUInt32LE(zlib.crc32(r.subarray(0, r.length - 4)), r.length - 4);
  return r;
}

const cases = {
  // the page: in line when the switch is full (where it is, and a notice once), its place kept across a dropped link,
  // in when a place frees up, with a notice (a switch with room for one)
  async full([port]) {
    const { attachNet } = await pageNet(port);
    const game = (name, notices, line) => {
      const memory = new WebAssembly.Memory({ initial: 10, maximum: 10, shared: true }), mb = mailbox(memory, 0);
      want(mb, 1);
      return attachNet(fakeGame(memory, { 4: mb }), { url: `ws://127.0.0.1:${port}/lan`, name, onNotice: t => notices.push(t),
        onQueue: q => line.push(q), pingMs: 60_000 });
    };
    const na = [], nb = [], qa = [], qb = [];
    const a = game('A', na, qa);
    try {
      await a.online;
      const b = game('B', nb, qb);
      try {
        await until(() => qb.length === 1);
        assert.deepEqual(qb[0], { pos: 1, of: 1 });
        assert.equal(b.state, 'queued');
        assert.deepEqual(nb, ['The server is full: you are in line for a place, and join the others by yourself when it is your turn']);
        assert.equal((await state(port)).info.waiting, 1);
        // its link drops: back to the same place, with no second notice
        b['ws'].close(4000, 'dropped');
        await until(async () => (await state(port)).info.rejoined === 1);
        await until(() => qb.length === 2 && b.state === 'queued');
        assert.deepEqual(qb[1], { pos: 1, of: 1 });
        assert.equal(nb.length, 1);
        assert.equal((await state(port)).info.waiting, 1);
        a.close();
        assert.match(await b.online, /^10\.77\.([0-9]|1[0-9])\.\d+$/);
        await until(() => nb.length === 2);
        assert.equal(nb[1], 'A place on the server freed up: you can play with others now');
        assert.equal(qb.at(-1), null, 'out of line');
        assert.deepEqual([na, qa], [[], []], 'the first game heard nothing');
        // a third waits in line, then stops waiting (the page menu's Play offline): out of line for good
        const nc = [], qc = [], c = game('C', nc, qc);
        try {
          await until(() => qc.length === 1 && c.state === 'queued');
          c.goOffline();
          await until(async () => (await state(port)).info.lineLeft === 1);
          assert.deepEqual([c.state, qc.at(-1), (await state(port)).info.waiting], ['away', null, 0]);
          c.setAway(false); await sleep(600);
          assert.deepEqual([c.state, (await state(port)).info.queued], ['away', 2], 'nothing takes the switch again');
        } finally { c.close(); }
      } finally { b.close(); }
    } finally { a.close(); }
  },

  // the page: the switch let go when no mailbox wants it, taken again when one does; LAN play open or not; a module of
  // before
  async mailboxes([port, port2, port3]) {
    const { attachNet } = await pageNet(port);
    const pages = Math.ceil(2 * P.NET_MAILBOX_BYTES / 65536);
    const memory = new WebAssembly.Memory({ initial: pages, maximum: pages, shared: true });
    const roam = mailbox(memory, 0), lanMb = mailbox(memory, P.NET_MAILBOX_BYTES);
    const logs = [];
    const link = attachNet(fakeGame(memory, { 4: roam, 7: lanMb }), { url: `ws://127.0.0.1:${port}/lan`, name: 'P', pingMs: 60_000, idleMs: 300, log: l => logs.push(l) });
    const peers = async (p = port) => (await state(p)).peers;
    try {
      await sleep(300);
      assert.equal((await peers()).length, 0, 'nothing wants the switch yet: no connection');
      want(lanMb, 1);                                                        // the game's WSAStartup
      const v1 = await link.online;
      await until(async () => { const p = await peers(); return p.length === 1 && p[0].hello; });
      assert.equal((await peers())[0].lan, true, 'LAN play open: said in the hello');
      assert.equal(Atomics.load(lanMb.w, P.W_PAGE_STATE), P.NET_PAGE_ONLINE);
      want(roam, 1); want(lanMb, 0);                                         // free roam on, LAN screens left
      await until(async () => (await peers())[0]?.lan === false);
      await sleep(600);
      assert.equal((await peers()).length, 1, 'free roam still wants it');
      want(roam, 0);                                                         // free roam off too
      await sleep(100);
      assert.equal((await peers()).length, 1, 'kept for idleMs');
      await until(async () => (await peers()).length === 0);
      assert.equal(link.state, 'idle');
      const s = await state(port);
      assert.equal(s.info.released, 1, 'its address freed at once');
      assert.equal(s.reserved, 0);
      for (const mb of [roam, lanMb]) assert.equal(Atomics.load(mb.w, P.W_PAGE_STATE), P.NET_PAGE_IDLE);
      // a want back within idleMs keeps the place; after it, the page connects again
      want(lanMb, 1);
      await until(async () => (await peers()).length === 1 && Atomics.load(lanMb.w, P.W_PAGE_STATE) === P.NET_PAGE_ONLINE);
      await until(async () => (await peers())[0]?.lan === true);
      const v2 = P.ipStr(Atomics.load(lanMb.w, P.W_VIP) >>> 0);
      assert.notEqual(v2, v1, 'a new address (the old one went back)');
      want(lanMb, 0); await sleep(100); want(lanMb, 1); await sleep(500);
      assert.equal((await peers()).length, 1); assert.equal((await state(port)).info.released, 1, 'a short break keeps the place');
      // peer bits: one bit per host of the /19 up to the module's 2048 (a peer in another /24 has its own bit; the
      // third lands in 10.77.4.x, past a /22); the seventh lands in 10.77.8.x, past the /21 and the bitmap: no bit
      // (the module asks that host itself, runtime/web/net_lan.c), and nothing written past the bitmap (its header's
      // padding)
      const others = Array.from({ length: 7 }, (_, i) => new Client(port, `O${i + 1}`));
      for (const o of others) await o.welcome();
      const hs = others.map(o => (o.vip & 0x1fff) >>> 0), low = hs.filter(h => h < P.NET_PEER_BITS);
      assert.ok(low.some(h => h >= 1024), `a host past the /22 (${hs.map(h => h.toString(16))})`);
      assert.ok(low.length < hs.length, `a host past the /21 (${hs.map(h => h.toString(16))})`);
      for (const h of low) await until(() => (Atomics.load(lanMb.w, P.W_PEER_BITS + (h >>> 5)) >>> (h & 31)) & 1);
      for (let i = P.NET_PEER_BITS / 32; P.W_PEER_BITS + i < P.NET_HDR_BYTES / 4; i++) assert.equal(Atomics.load(lanMb.w, P.W_PEER_BITS + i), 0, `word ${i} untouched`);
      for (const o of others) await o.close();
      assert.ok(logs.some(l => /goes back/.test(l)), 'the console says why it let go');
    } finally { link.close(); }
    // a module of before: version 2, no want word, always connected
    {
      const mem2 = new WebAssembly.Memory({ initial: 10, maximum: 10, shared: true }), old = mailbox(mem2, 0, 2);
      const l2 = attachNet(fakeGame(mem2, { 7: old }), { url: `ws://127.0.0.1:${port2}/lan`, name: 'Old', pingMs: 60_000, idleMs: 100 });
      try {
        await l2.online;
        await until(async () => { const p = await peers(port2); return p.length === 1 && p[0].hello; });
        assert.equal((await peers(port2))[0].lan, true, 'its LAN mailbox counts as open');
        await sleep(400);
        assert.equal((await peers(port2)).length, 1, 'never let go');
        const c = new Client(port2, 'C'); await c.welcome();
        const h = c.vip & 255;                                               // its bitmap: by the last byte, 8 words
        await until(() => (Atomics.load(old.w, P.W_PEER_BITS + (h >>> 5)) >>> (h & 31)) & 1);
        await c.close();
      } finally { l2.close(); }
    }
    // a module of version 3: bits for a /22 (32 words), nothing written past them (its header's padding)
    {
      const mem3 = new WebAssembly.Memory({ initial: 10, maximum: 10, shared: true }), v3 = mailbox(mem3, 0, 3);
      want(v3, 1);
      const l3 = attachNet(fakeGame(mem3, { 7: v3 }), { url: `ws://127.0.0.1:${port3}/lan`, name: 'V3', pingMs: 60_000, idleMs: 60_000 });
      try {
        await l3.online;
        const cs = [1, 2, 3, 4, 5].map(i => new Client(port3, `C${i}`));
        for (const c of cs) await c.welcome();
        const hs = cs.map(c => (c.vip & 0xfff) >>> 0), low = hs.filter(h => h < 1024);
        assert.ok(low.length && low.length < hs.length, `hosts on both sides of 1024 (${hs.map(h => h.toString(16))})`);
        for (const h of low) await until(() => (Atomics.load(v3.w, P.W_PEER_BITS + (h >>> 5)) >>> (h & 31)) & 1);
        for (let i = P.NET_V3_PEER_BITS / 32; i < P.NET_PEER_BITS / 32; i++) assert.equal(Atomics.load(v3.w, P.W_PEER_BITS + i), 0, `word ${i} untouched`);
        for (const c of cs) await c.close();
      } finally { l3.close(); }
    }
  },

  // the page: away, the place goes back while the game wants it and stays free; back, it is taken again
  async away([port]) {
    const { attachNet } = await pageNet(port);
    const memory = new WebAssembly.Memory({ initial: 10, maximum: 10, shared: true }), roam = mailbox(memory, 0);
    const logs = [];
    const link = attachNet(fakeGame(memory, { 4: roam }), { url: `ws://127.0.0.1:${port}/lan`, name: 'P', pingMs: 60_000, idleMs: 100, log: l => logs.push(l) });
    const size = async () => (await state(port)).peers.length;
    try {
      want(roam, 1);                                                         // free roam runs
      const v1 = await link.online;
      await until(async () => (await size()) === 1);
      link.setAway(true);
      assert.equal(link.state, 'away');
      assert.equal(Atomics.load(roam.w, P.W_PAGE_STATE), P.NET_PAGE_IDLE, 'C hears that the page let go');
      await until(async () => (await size()) === 0);
      const s = await state(port);
      assert.equal(s.info.released, 1, 'its address freed at once');
      assert.equal(s.reserved, 0);
      want(roam, 0); want(roam, 1); await sleep(300);                        // the game wants it again, and again
      assert.equal(await size(), 0, 'nothing connects while away');
      want(roam, 0); await sleep(300);
      assert.equal(link.state, 'away', 'the idle timer does not end away');
      link.setAway(false);
      assert.equal(link.state, 'idle', 'back, with nothing that wants the switch: no connection');
      await sleep(200);
      assert.equal(await size(), 0);
      link.setAway(true); want(roam, 1); link.setAway(false);                // back while free roam runs
      await until(async () => (await size()) === 1 && Atomics.load(roam.w, P.W_PAGE_STATE) === P.NET_PAGE_ONLINE);
      assert.notEqual(P.ipStr(Atomics.load(roam.w, P.W_VIP) >>> 0), v1, 'a new address');
      assert.ok(logs.some(l => /the player is away/.test(l)) && logs.some(l => /here again/.test(l)), 'the console says so');
    } finally { link.close(); }
  },

  // the page: kicked, it tells the game and stays off the switch for the rest of the game, whatever wants it; a page
  // whose id is banned is turned away at its hello the same way
  async removed([port]) {
    const { attachNet } = await pageNet(port);
    const memory = new WebAssembly.Memory({ initial: 10, maximum: 10, shared: true }), roam = mailbox(memory, 0);
    const mem2 = new WebAssembly.Memory({ initial: 10, maximum: 10, shared: true }), roam2 = mailbox(mem2, 0);
    const logs = [], told = [];
    const size = async () => (await state(port)).peers.length;
    const link = attachNet(fakeGame(memory, { 4: roam }), { url: `ws://127.0.0.1:${port}/lan`, name: 'P', id: 'page-p', pingMs: 60_000, idleMs: 100,
      log: l => logs.push(l), onRemoved: r => told.push(r) });
    const banned = attachNet(fakeGame(mem2, { 4: roam2 }), { url: `ws://127.0.0.1:${port}/lan?gate=page-b`, name: 'B', id: 'page-b', pingMs: 60_000,
      onRemoved: r => told.push(r) });
    try {
      want(roam, 1);
      const vip = P.ipNum(await link.online);
      await until(async () => (await size()) === 1);
      assert.equal((await fetch(`http://127.0.0.1:${port}/expel?vip=${vip}&code=4001&msg=Stop`)).status, 200);
      await until(() => told.length === 1);
      assert.deepEqual(told[0], { ban: false, msg: 'Stop' });
      assert.equal(link.state, 'away');
      assert.equal(Atomics.load(roam.w, P.W_PAGE_STATE), P.NET_PAGE_IDLE, 'C hears that the page let go');
      await until(async () => (await size()) === 0);
      assert.equal((await state(port)).reserved, 0, 'its address free at once');
      want(roam, 0); want(roam, 1); link.setAway(true); link.setAway(false); await sleep(300);
      assert.equal(await size(), 0, 'nothing connects again: not a want, not the away check');
      assert.ok(logs.some(l => /the operator kicked this player \("Stop"\): no switch for the rest of this game/.test(l)), logs.join('\n'));
      // its id banned: told at its hello, and off
      want(roam2, 1);
      await until(() => told.length === 2);
      assert.deepEqual(told[1], { ban: true, msg: 'page-b' });
      assert.equal(banned.state, 'away');
      await sleep(300);
      assert.equal(await size(), 0);
    } finally { link.close(); banned.close(); }
  },

  // the page: its hello names the players it blocks (NetOptions.block): page.rs reads what the hub was told
  async block([port]) {
    const { attachNet } = await pageNet(port);
    const memory = new WebAssembly.Memory({ initial: 10, maximum: 10, shared: true }), roam = mailbox(memory, 0);
    const link = attachNet(fakeGame(memory, { 4: roam }), { url: `ws://127.0.0.1:${port}/lan`, name: 'P', block: ['Bob', 'carl'], pingMs: 60_000 });
    try {
      want(roam, 1);
      await link.online;
      await until(async () => (await state(port)).peers.some(p => p.hello));
    } finally { link.close(); }
  },

  // the page: what C queued at once goes as one bundle, a snapshot to several friends once; bundles from the switch are
  // taken apart in order
  async bundles([port]) {
    const { attachNet } = await pageNet(port);
    const memory = new WebAssembly.Memory({ initial: 10, maximum: 10, shared: true }), roam = mailbox(memory, 0);
    want(roam, 1); Atomics.store(roam.w, P.W_MODE, P.NET_MODE_VLAN);
    const link = attachNet(fakeGame(memory, { 4: roam }), { url: `ws://127.0.0.1:${port}/lan`, name: 'P', pingMs: 60_000 });
    try {
      const me = P.ipNum(await link.online);
      const b = new Client(port, 'B'), c = new Client(port, 'C', '', null, { bundle: true });
      await b.welcome(); await c.welcome(); await sleep(100);
      // a tick of free roam: the same snapshot to B and C, another frame to B, the snapshot to the hub's address
      txPush(memory, roam, { dst: b.vip, data: 'snap' }); txPush(memory, roam, { dst: c.vip, data: 'snap' });
      txPush(memory, roam, { dst: b.vip, data: 'more' }); txPush(memory, roam, { dst: P.NET_ROAM_HUB, data: 'snap' });
      txBell(roam);
      await b.until(() => b.dgrams().length >= 2); await c.until(() => c.dgrams().length >= 1);
      assert.deepEqual(b.dgrams().map(g => [g.data, g.src === me, g.dst === b.vip]), [['snap', true, true], ['more', true, true]]);
      assert.deepEqual(c.dgrams().map(g => [g.data, g.src === me, g.dst === c.vip]), [['snap', true, true]]);
      assert.equal((await state(port)).info.bundlesIn, 1, 'the four went as one message');
      // three from C at once (one bundle): the switch sends them to the page as one message, and C hears them in order
      c.sendBundle([{ f: frameOf(me, 'r1', 1, 47210, 47210) }, { f: frameOf(me, 'r2', 1, 47210, 47210) }, { f: frameOf(me, 'r3', 1, 47210, 47210) }]);
      await until(() => Atomics.load(roam.w, P.W_RX_HEAD) - Atomics.load(roam.w, P.W_RX_TAIL) >= 3);
      assert.deepEqual(rxTake(memory, roam).map(r => [r.data, r.src === c.vip, r.dst === me]), [['r1', true, true], ['r2', true, true], ['r3', true, true]]);
      assert.ok((await state(port)).info.bundlesOut >= 1, 'to the page as a bundle');
      const s = link.stats();
      assert.equal(s.framesOut, 4); assert.equal(s.framesIn, 3, 'frames counted one by one');
      // its hello took compact bundles (nfsu2-lan compact.rs): free roam's datagrams come without their magic,
      // version, length and CRC, and the game still reads them byte for byte, as it does a broadcast, other ports,
      // a broken CRC and LAN play's UDP
      const snap = roamDatagram(3, 70), hello = roamDatagram(1, 40), broken = roamDatagram(3, 70);
      broken[broken.length - 1] ^= 0x55;
      const sent = [{ f: frameOf(me, snap, 1, 47210, 47210), dst: me, sport: 47210, dport: 47210, data: snap },
        { f: frameOf(P.NET_BROADCAST, hello, 1, 47210, 47210), dst: P.NET_BROADCAST, sport: 47210, dport: 47210, data: hello },
        { f: frameOf(me, snap, 1, 47211, 47300), dst: me, sport: 47211, dport: 47300, data: snap },
        { f: frameOf(me, broken, 1, 47210, 47210), dst: me, sport: 47210, dport: 47210, data: broken }];
      c.sendBundle(sent.map(({ f }) => ({ f })));
      await until(() => Atomics.load(roam.w, P.W_RX_HEAD) - Atomics.load(roam.w, P.W_RX_TAIL) >= sent.length);
      const got = rxTakeRaw(memory, roam);
      assert.deepEqual(got.map(g => [g.src === c.vip, g.dst >>> 0, g.sport, g.dport, g.data.toString('hex')]),
        sent.map(x => [true, x.dst >>> 0, x.sport, x.dport, x.data.toString('hex')]), 'byte for byte');
      assert.equal(link.stats().framesIn, 3 + sent.length);
      assert.ok(link.stats().compactIn >= 2, 'as compact bundles');
      // its hello took deltas too: a friend's snapshots, a few bytes new each time and now and then longer, come as
      // what changed (nfsu2-lan compact.rs Refs, proto.ts unpackCompact) and still reach the game byte for byte
      const seq = [];
      let snapNow = Buffer.from(roamDatagram(3, 70));
      for (let i = 0; i < 60; i++) {
        const body = Buffer.from(snapNow.subarray(16, snapNow.length - 4));
        body[(i * 7) % body.length] = i; body[(i * 13 + 3) % body.length] ^= 0x5a;
        const d = Buffer.alloc(16 + body.length + (i === 30 ? 1 : 0) + 4);
        snapNow.copy(d, 0, 0, 16); body.copy(d, 16);
        d.writeUInt16LE(d.length, 6); d.writeUInt32LE(zlib.crc32(d.subarray(0, d.length - 4)), d.length - 4);
        seq.push(d); snapNow = d;
      }
      for (let i = 0; i < seq.length; i += 6) {
        c.sendBundle(seq.slice(i, i + 6).map(d => ({ f: frameOf(me, d, 1, 47210, 47210) })));
        await until(() => Atomics.load(roam.w, P.W_RX_HEAD) - Atomics.load(roam.w, P.W_RX_TAIL) >= Math.min(6, seq.length - i));
        assert.deepEqual(rxTakeRaw(memory, roam).map(g => g.data.toString('hex')), seq.slice(i, i + 6).map(d => d.toString('hex')), `snapshots ${i} on`);
      }
      assert.ok(link.stats().deltaIn >= 50, `as deltas (${link.stats().deltaIn} of ${seq.length})`);
      // free roam's own datagrams (a ping, a pong) wait for the next car state to a friend, at most TX_HOLD_MS (50)
      // after the page's last send, and go with it as one message; one long after the last send goes at once
      const ping = roamDatagram(6, 8), pong = roamDatagram(7, 16), carState = roamDatagram(3, 70);
      const ins = async () => (await state(port)).info.bundlesIn, in0 = await ins(), nb = b.dgrams().length, nc = c.dgrams().length;
      await sleep(80);
      txPush(memory, roam, { dst: b.vip, data: ping }); txBell(roam);
      await b.until(() => b.dgrams().length >= nb + 1);
      txPush(memory, roam, { dst: b.vip, data: pong }); txBell(roam);
      await sleep(15);
      assert.equal(b.dgrams().length, nb + 1, 'a pong right after a send waits');
      txPush(memory, roam, { dst: c.vip, data: carState }); txBell(roam);
      await b.until(() => b.dgrams().length >= nb + 2); await c.until(() => c.dgrams().length >= nc + 1);
      assert.equal(await ins(), in0 + 1, 'the ping alone (a frame), then the pong with the car state (a bundle)');
      txPush(memory, roam, { dst: b.vip, data: carState }); txBell(roam);
      await b.until(() => b.dgrams().length >= nb + 3);
      const t0 = performance.now();
      txPush(memory, roam, { dst: b.vip, data: ping }); txBell(roam);
      await b.until(() => b.dgrams().length >= nb + 4);
      const held = performance.now() - t0;
      assert.ok(held >= 25, `without a car state it waits until 50 ms after the last send (${held.toFixed(0)} ms)`);
      await b.close(); await c.close();
    } finally { link.close(); }
  },

  // its hello took friends' car states by their fields (nfsu2-lan fpred.rs, proto.ts fpredTake): each reaches the game
  // rounded, as the switch rounded it, with a CRC that checks out; one of a layout that does not go so (three states)
  // and a HELLO reach it as they were
  async fpred([port]) {
    const { attachNet } = await pageNet(port);
    const memory = new WebAssembly.Memory({ initial: 10, maximum: 10, shared: true }), roam = mailbox(memory, 0);
    want(roam, 1); Atomics.store(roam.w, P.W_MODE, P.NET_MODE_VLAN);
    const link = attachNet(fakeGame(memory, { 4: roam }), { url: `ws://127.0.0.1:${port}/lan`, name: 'P', pingMs: 60_000 });
    try {
      const me = P.ipNum(await link.online);
      const c = new Client(port, 'C', '', null, { bundle: true });
      await c.welcome(); await sleep(100);
      const sent = [];
      let t = 4_294_000_000;
      for (let k = 0; k < 120; k++) {
        t = (t + 50_000 + (k % 7 === 0 ? 16_667 : 0)) >>> 0;
        const a = k * 0.05, moving = k < 90;
        const p = moving ? [1695.123 + 40 * Math.cos(a), -884.5 + 40 * Math.sin(a), 24.25 + 0.01 * k] : [1700.5, -880.25, 25.125];
        const v = moving ? [-40 * Math.sin(a), 40 * Math.cos(a), 0.2] : [0, 0, 0];
        sent.push(snapDatagram({ seq: k, t, p, v, k: moving ? k : 90, x: k % 10 === 5 ? 28 : k === 40 ? 0 : 16 }));
        if (k === 60) {
          const three = snapDatagram({ seq: k, t, p, v, k, x: 16 });
          three[8 + 12] = 3;
          three.writeUInt32LE(zlib.crc32(three.subarray(0, three.length - 4)), three.length - 4);
          sent.push(three, roamDatagram(1, 40));
        }
      }
      const expect = sent.map(d => d[5] === 3 && d[8 + 12] === 1 ? roundedDatagram(d) : d);
      for (let i = 0; i < sent.length; i += 6) {
        const part = sent.slice(i, i + 6);
        c.sendBundle(part.map(d => ({ f: frameOf(me, d, 1, 47210, 47210) })));
        await until(() => Atomics.load(roam.w, P.W_RX_HEAD) - Atomics.load(roam.w, P.W_RX_TAIL) >= part.length);
        const got = rxTakeRaw(memory, roam);
        assert.deepEqual(got.map(g => [g.src === c.vip, g.sport, g.dport, g.data.toString('hex')]),
          expect.slice(i, i + 6).map(d => [true, 47210, 47210, d.toString('hex')]), `datagrams ${i} on`);
        for (const g of got) assert.equal(g.data.readUInt32LE(g.data.length - 4), zlib.crc32(g.data.subarray(0, g.data.length - 4)), 'a CRC that checks out');
      }
      const info = (await state(port)).info;
      assert.ok(info.fpredEntries >= 115 && info.fpredEntries <= info.snapEntries, `by their fields: ${info.fpredEntries} of ${info.snapEntries}`);
      assert.ok(info.snapBytes / info.snapEntries < 40, `${(info.snapBytes / info.snapEntries).toFixed(1)} bytes a car state`);
      await c.close();
    } finally { link.close(); }
  },
};

const [name, ...ports] = process.argv.slice(2);
try {
  await cases[name](ports.map(Number));
  console.log(`page ${name}: ok`);
  process.exit(0);
} catch (e) {
  console.error(`page ${name}: ${e.stack ?? e}`);
  process.exit(1);
}
