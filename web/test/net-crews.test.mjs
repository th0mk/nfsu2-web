// The crews the page's menu changes while a game runs, in free roam's net mailbox (web/src/net/index.ts setCrews;
// runtime/web/net_proto.h crews_gen, crews[4], crews_rgb[4]):
//   node --test web/test/net-crews.test.mjs
// - nothing is written until the page changes them (crews_gen 0: the game keeps NFSU2_ROAM_CREW and its colours)
// - the crews' four words, then their colours' four (0x00rrggbb; 0: the place's default), then crews_gen (+1 a change,
//   a colour alone too), each with Atomics.store
// - set before the game registered its free-roam mailbox: written once it does
// With a fake game process (as web/test/page-lifecycle.test.mjs has); the mailbox never wants the switch, so no socket.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export { attachNet } from './web/src/net/index.ts'; export * as P from './web/src/net/proto.ts';
export { roomHash } from './web/src/app/players.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const { attachNet, P, roomHash } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

const NET = 4;                                   // HostKind.Net (web/src/host/abi.ts)
const PTR = 65536;
const tick = (ms = 0) => new Promise(r => setTimeout(r, ms));

/** a game process whose free-roam mailbox registers when the test says so */
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
/** the mailbox header as the runtime fills it */
function mailbox(proc) {
  const hdr = P.NET_HDR_BYTES, size = P.NET_MAILBOX_BYTES;
  const i32 = new Int32Array(proc.memory.buffer, PTR, size / 4);
  Object.assign(i32, { [P.W_MAGIC]: P.NET_MAGIC | 0, [P.W_VERSION]: P.NET_VERSION, [P.W_HDR_BYTES]: hdr, [P.W_SLOT_BYTES]: P.NET_SLOT_BYTES,
    [P.W_TX_SLOTS]: P.NET_TX_SLOTS, [P.W_RX_SLOTS]: P.NET_RX_SLOTS, [P.W_TX_OFF]: hdr, [P.W_RX_OFF]: hdr + P.NET_TX_SLOTS * P.NET_SLOT_BYTES });
  return { i32, size };
}
/** the Atomics.store calls on the crews' words of the mailbox at PTR, in order */
function watchStores() {
  const real = Atomics.store, seen = [];
  Atomics.store = function (a, i, v) {
    const at = a.byteOffset + i * 4 - PTR;
    if (at >= P.W_CREWS_GEN * 4 && at < (P.W_CREWS_RGB + P.CREW_WORDS) * 4) seen.push([at / 4, v >>> 0]);
    return real.call(Atomics, a, i, v);
  };
  return { seen, stop: () => { Atomics.store = real; } };
}
const crewWords = i32 => [...i32.subarray(P.W_CREWS, P.W_CREWS + P.CREW_WORDS)].map(v => v >>> 0);
const rgbWords = i32 => [...i32.subarray(P.W_CREWS_RGB, P.W_CREWS_RGB + P.CREW_WORDS)].map(v => v >>> 0);
/** the stores of one change: the crews' words, the colours', then the count */
const stores = (crews, rgbs, gen) => [...[0, 1, 2, 3].map(i => [P.W_CREWS + i, crews[i] ?? 0]), ...[0, 1, 2, 3].map(i => [P.W_CREWS_RGB + i, rgbs[i] ?? 0]),
  [P.W_CREWS_GEN, gen]];

test('the mailbox words are the C header\'s padding words of before, past the peer bits', () => {
  assert.equal(P.W_CREWS_GEN * 4, 400);
  assert.equal(P.W_CREWS, P.W_CREWS_GEN + 1);
  assert.ok(P.W_PEER_BITS + P.NET_PEER_BITS / 32 <= P.W_CREWS_GEN);
  assert.equal(P.W_CREWS_RGB, P.W_CREWS + P.CREW_WORDS);
  assert.equal(P.W_CREWS_RGB * 4, 420);
  assert.ok((P.W_CREWS_RGB + P.CREW_WORDS) * 4 <= P.NET_HDR_BYTES);
});

test('crews set before the mailbox is there: written when it registers, the crews, the colours, then the count', async () => {
  const proc = fakeProc(), logs = [], w = watchStores();
  try {
    const blue = roomHash('Blue Room'), owls = roomHash('Night Owls'), red = roomHash('Red');
    assert.ok(owls > 0x7fffffff, 'a hash with its top bit set goes through as u32');
    const link = attachNet(proc, { log: s => logs.push(s) });
    link.setCrews([blue, owls], [0x3cd2ff, 0xff4646]);
    assert.deepEqual(w.seen, [], 'no mailbox yet: nothing written');
    const { i32 } = mailbox(proc);
    proc.register(NET, PTR, P.NET_MAILBOX_BYTES);
    await tick(20);
    assert.deepEqual(w.seen, [[101, blue], [102, owls], [103, 0], [104, 0], [105, 0x3cd2ff], [106, 0xff4646], [107, 0], [108, 0], [100, 1]]);
    assert.deepEqual(crewWords(i32), [blue, owls, 0, 0]);
    assert.deepEqual(rgbWords(i32), [0x3cd2ff, 0xff4646, 0, 0]);
    assert.equal(i32[P.W_CREWS_GEN], 1);
    // a change while it runs: written at once
    w.seen.length = 0;
    link.setCrews([red], [0xa078ff]);
    assert.deepEqual(w.seen, stores([red], [0xa078ff], 2));
    // a colour alone: a change too
    w.seen.length = 0;
    link.setCrews([red], [0xff9ad5]);
    assert.deepEqual(w.seen, stores([red], [0xff9ad5], 3));
    // no colours given: 0s, the places' defaults; only 24 bits of a colour
    w.seen.length = 0;
    link.setCrews([red]);
    assert.deepEqual(w.seen, stores([red], [], 4));
    w.seen.length = 0;
    link.setCrews([red], [0xff3cd2ff]);
    assert.deepEqual(w.seen, stores([red], [0x3cd2ff], 5));
    // none left: four zeros, the count on
    w.seen.length = 0;
    link.setCrews([], []);
    assert.deepEqual(w.seen, stores([], [], 6));
    // at most 4
    link.setCrews([1, 2, 3, 4, 5], [6, 7, 8, 9, 10]);
    assert.deepEqual(crewWords(i32), [1, 2, 3, 4]);
    assert.deepEqual(rgbWords(i32), [6, 7, 8, 9]);
    assert.equal(i32[P.W_CREWS_GEN], 7);
    assert.ok(logs.includes('[net] crews: 2 (the page menu)'), logs.join('\n'));
    link.close();
  } finally { w.stop(); }
});

test('crews never changed: nothing written (the game keeps NFSU2_ROAM_CREW)', async () => {
  const proc = fakeProc(), w = watchStores();
  try {
    const link = attachNet(proc, {});
    const { i32 } = mailbox(proc);
    proc.register(NET, PTR, P.NET_MAILBOX_BYTES);
    await tick(20);
    link.setSolid(true);
    assert.deepEqual(w.seen, []);
    assert.equal(i32[P.W_CREWS_GEN], 0);
    assert.equal(i32[P.W_SOLID], 2, 'the solid word beside it, as before');
    link.close();
  } finally { w.stop(); }
});
