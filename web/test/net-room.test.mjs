// The room an invite link changes while a game runs, in free roam's net mailbox (web/src/net/index.ts setRoom;
// runtime/web/net_proto.h room_gen, room_name[16]):
//   node --test web/test/net-room.test.mjs
// - the name's 16 words (UTF-8, little-endian, 0 bytes after it), then room_gen (+1 a change), each with Atomics.store
// - set before the game registered its free-roam mailbox: written once it does
// - a name longer than 63 bytes is cut where a character starts
// With a fake game process (as web/test/net-crews.test.mjs has); the mailbox never wants the switch, so no socket.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export { attachNet } from './web/src/net/index.ts'; export * as P from './web/src/net/proto.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const { attachNet, P } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

const NET = 4;                                   // HostKind.Net (web/src/host/abi.ts)
const PTR = 65536;
const tick = (ms = 0) => new Promise(r => setTimeout(r, ms));

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
  const i32 = new Int32Array(proc.memory.buffer, PTR, P.NET_MAILBOX_BYTES / 4);
  i32.fill(0);
  Object.assign(i32, { [P.W_MAGIC]: P.NET_MAGIC | 0, [P.W_VERSION]: P.NET_VERSION, [P.W_HDR_BYTES]: P.NET_HDR_BYTES, [P.W_SLOT_BYTES]: P.NET_SLOT_BYTES,
    [P.W_TX_SLOTS]: P.NET_TX_SLOTS, [P.W_RX_SLOTS]: P.NET_RX_SLOTS, [P.W_TX_OFF]: P.NET_HDR_BYTES, [P.W_RX_OFF]: P.NET_HDR_BYTES + P.NET_TX_SLOTS * P.NET_SLOT_BYTES });
  return i32;
}
/** the Atomics.store calls on the room's words of the mailbox at PTR, in order */
function watchStores() {
  const real = Atomics.store, seen = [];
  Atomics.store = function (a, i, v) {
    const at = a.byteOffset + i * 4 - PTR;
    if (at >= P.W_ROOM_GEN * 4 && at < (P.W_ROOM_NAME + P.ROOM_WORDS) * 4) seen.push(at / 4);
    return real.call(Atomics, a, i, v);
  };
  return { seen, stop: () => { Atomics.store = real; } };
}
/** the name the words hold, as roam_web.c roam_plat_room reads it */
const nameOf = i32 => {
  const b = new Uint8Array(P.ROOM_WORDS * 4);
  for (let i = 0; i < P.ROOM_WORDS; i++) for (let k = 0; k < 4; k++) b[4 * i + k] = i32[P.W_ROOM_NAME + i] >>> 8 * k & 0xff;
  return new TextDecoder().decode(b.subarray(0, b.indexOf(0) < 0 ? 63 : b.indexOf(0)));
};
const ORDER = [...Array(P.ROOM_WORDS).keys()].map(i => P.W_ROOM_NAME + i).concat(P.W_ROOM_GEN);

test('the room\'s words: the header\'s padding after the crews\' colours, within the 512 bytes', () => {
  assert.equal(P.W_ROOM_GEN, P.W_CREWS_RGB + P.CREW_WORDS);
  assert.equal(P.W_ROOM_GEN * 4, 436);
  assert.equal(P.W_ROOM_NAME, P.W_ROOM_GEN + 1);
  assert.ok((P.W_ROOM_NAME + P.ROOM_WORDS) * 4 <= P.NET_HDR_BYTES);
});

test('a room set before the mailbox is there: written when it registers, the name, then the count', async () => {
  const proc = fakeProc(), logs = [], w = watchStores();
  try {
    const link = attachNet(proc, { log: s => logs.push(s) });
    assert.equal(link.setRoom('Blue Room'), false, 'not taken yet');
    assert.deepEqual(w.seen, []);
    const i32 = mailbox(proc);
    proc.register(NET, PTR, P.NET_MAILBOX_BYTES);
    await tick(20);
    assert.deepEqual(w.seen, ORDER, 'the name\'s 16 words, then the count');
    assert.equal(nameOf(i32), 'Blue Room');
    assert.equal(i32[P.W_ROOM_GEN], 1);
    // a change while it runs: at once, the words after the name 0
    w.seen.length = 0;
    assert.equal(link.setRoom('Комната'), true);
    assert.deepEqual(w.seen, ORDER);
    assert.equal(nameOf(i32), 'Комната');
    assert.equal(i32[P.W_ROOM_GEN], 2);
    // no room: every word 0
    link.setRoom('');
    assert.deepEqual([...i32.subarray(P.W_ROOM_NAME, P.W_ROOM_NAME + P.ROOM_WORDS)], new Array(P.ROOM_WORDS).fill(0));
    assert.equal(i32[P.W_ROOM_GEN], 3);
    // 63 bytes at most, cut where a character starts (a 0 byte always after it)
    link.setRoom('я'.repeat(40));
    assert.equal(nameOf(i32), 'я'.repeat(31), '31 two-byte letters fit in 63 bytes');
    assert.equal(i32[P.W_ROOM_NAME + P.ROOM_WORDS - 1] >>> 24, 0);
    assert.ok(logs.includes('[net] room: changed (the page)'), logs.join('\n'));
    link.close();
  } finally { w.stop(); }
});
