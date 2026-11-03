// The virtual LAN's link in its worker (web/src/net/worker.ts) and the page's side of it (web/src/net/remote.ts
// attachNetWorker):
//   node --test web/test/net-worker.test.mjs
// - what the page sets (room, crews, solid cars, quick chat) reaches the game's mailbox through the worker, set
//   before the game registered it too; say and setRoom answer once the worker's link took the mailbox
// - the link's state and counters come back to the page; the page's close and the game's end close the worker's
//   link, and the worker ends itself
// - no worker (the constructor throws, its script fails before it says it runs, a worker without shared memory): the
//   link runs on the page's thread, with every call the page made meanwhile
// With a fake game process and a fake Worker that runs the worker's bundle in this process (its messages a turn
// later, as a real one's); the mailbox never wants the switch, so no socket.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
// (import.meta.url: the page's script's address, as in the player's page; a data: URL is no base for the worker's)
const bundle = async contents => (await esbuild.build({ stdin: { contents, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false,
  platform: 'neutral', define: { 'import.meta.url': '"http://localhost:8080/main.js"' } })).outputFiles[0].text;
const page = await bundle(`export { attachNetWorker } from './web/src/net/remote.ts'; export * as P from './web/src/net/proto.ts';`);
const { attachNetWorker, P } = await import('data:text/javascript;base64,' + Buffer.from(page).toString('base64'));
const worker = await bundle(`import './web/src/net/worker.ts';`);

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
function mailbox(proc) {
  const i32 = new Int32Array(proc.memory.buffer, PTR, P.NET_MAILBOX_BYTES / 4);
  i32.fill(0);
  Object.assign(i32, { [P.W_MAGIC]: P.NET_MAGIC | 0, [P.W_VERSION]: P.NET_VERSION, [P.W_HDR_BYTES]: P.NET_HDR_BYTES, [P.W_SLOT_BYTES]: P.NET_SLOT_BYTES,
    [P.W_TX_SLOTS]: P.NET_TX_SLOTS, [P.W_RX_SLOTS]: P.NET_RX_SLOTS, [P.W_TX_OFF]: P.NET_HDR_BYTES, [P.W_RX_OFF]: P.NET_HDR_BYTES + P.NET_TX_SLOTS * P.NET_SLOT_BYTES });
  return i32;
}

/** Worker as the page sees it, running a fresh copy of the worker's bundle here (each worker its own module state: a
 *  new module address each, as one imported again would be the same module).  throws: the constructor throws (no
 *  module workers); fail: its script "fails to load" (onerror, and nothing runs); isolated: its global's
 *  crossOriginIsolated (false: the script throws as it starts, onerror) */
let workers = [], made = 0;
function fakeWorkers({ throws = false, fail = false, isolated = true } = {}) {
  workers = [];
  globalThis.Worker = class {
    constructor(url, o) {
      if (throws) throw new Error('no module workers');
      this.url = String(url); this.name = o?.name; this.terminated = false; this.closed = false;
      this.onmessage = null; this.onerror = null;
      const inbox = [];
      let deliver = null;
      workers.push(this);
      const error = message => setTimeout(() => this.onerror?.({ message, preventDefault() {} }), 0);
      this.postMessage = m => { if (!this.terminated) setTimeout(() => deliver ? deliver(m) : inbox.push(m), 0); };
      if (fail) { error('worker.js: 404'); return; }
      // the worker's global: its handlers, postMessage to the page, close
      const me = { crossOriginIsolated: isolated, onmessage: null, onmessageerror: null,
        postMessage: m => { if (!this.terminated) setTimeout(() => this.onmessage?.({ data: m }), 0); },
        close: () => { this.closed = true; } };
      globalThis.self = me;
      void import(`data:text/javascript;base64,${Buffer.from(`${worker}\n//${++made}`).toString('base64')}`).then(() => {
        deliver = m => me.onmessage?.({ data: m });
        for (const m of inbox.splice(0)) deliver(m);
      }, e => error(e.message));
    }
    terminate() { this.terminated = true; }
  };
}

test('the page\'s settings reach the mailbox through the worker, and its state comes back', async () => {
  fakeWorkers();
  const proc = fakeProc(), logs = [];
  const link = attachNetWorker(proc, { name: 'p1', log: s => logs.push(s) });
  assert.equal(workers.length, 1);
  assert.equal(workers[0].url, 'http://localhost:8080/net/worker.js');
  assert.equal(workers[0].name, 'net');
  // before the game registered its mailbox: taken, but nothing there yet
  assert.equal(link.say(3), false, 'no free roam yet');
  assert.equal(link.setRoom('Blue Room'), false);
  link.setCrews([0x1234, 0x5678], [0xff0000]);
  link.setSolid('off');
  const i32 = mailbox(proc);
  proc.register(NET, PTR, P.NET_MAILBOX_BYTES);
  await tick(50);
  assert.ok(logs.includes('[net] the link to the server runs in a worker of its own'), logs.join('\n'));
  assert.equal(i32[P.W_ROOM_GEN], 1);
  assert.equal(i32[P.W_CREWS_GEN], 1);
  assert.deepEqual([i32[P.W_CREWS] >>> 0, i32[P.W_CREWS + 1] >>> 0], [0x1234, 0x5678]);
  assert.equal(i32[P.W_CREWS_RGB] >>> 0, 0xff0000);
  assert.equal(i32[P.W_SOLID], 1, 'solid cars off');
  assert.equal(link.say(7), true, 'the worker\'s link took the mailbox');
  await tick(20);
  assert.equal(i32[P.W_SAY] & 0xff, 7);
  assert.equal(i32[P.W_SAY] >>> 8, 1, 'the first pick');
  // a room changed while it runs: its count moves on
  assert.equal(link.setRoom('Red'), true);
  await tick(20);
  assert.equal(i32[P.W_ROOM_GEN], 2);
  // the link's state and counters, from the worker
  assert.equal(link.state, 'waiting', 'the mailbox never wanted the switch');
  assert.equal(link.stats().state, 'waiting');
  assert.equal(typeof link.stats().framesIn, 'number');
  assert.equal(link.friend({ op: 'list' }), false, 'not on the switch');
  assert.ok(logs.includes('[net] room: changed (the page)'), logs.join('\n'));
  link.close();
  assert.equal(link.state, 'closed');
  await tick(1100);
  assert.equal(workers[0].closed, true, 'the worker ended itself once its link closed');
  await tick(1000);
  assert.equal(workers[0].terminated, true, 'and the page ended it all the same');
  link.close();
});

test('the game\'s end closes the worker\'s link', async () => {
  fakeWorkers();
  const proc = fakeProc();
  const link = attachNetWorker(proc, {});
  const i32 = mailbox(proc);
  proc.register(NET, PTR, P.NET_MAILBOX_BYTES);
  await tick(50);
  proc.exit(0);
  await tick(50);
  assert.equal(link.state, 'closed');
  assert.equal(Atomics.load(i32, P.W_PAGE_STATE), P.NET_PAGE_NONE);
  await tick(1100);
  assert.equal(workers[0].closed, true);
});

test('no module workers: the link runs on the page\'s thread', async () => {
  fakeWorkers({ throws: true });
  const proc = fakeProc(), logs = [];
  const link = attachNetWorker(proc, { log: s => logs.push(s) });
  const i32 = mailbox(proc);
  proc.register(NET, PTR, P.NET_MAILBOX_BYTES);
  await tick(20);
  assert.equal(link.setRoom('Blue Room'), true);
  assert.equal(i32[P.W_ROOM_GEN], 1);
  assert.ok(logs.some(s => s.includes('no worker for the link (Error: no module workers): it runs on the page')), logs.join('\n'));
  link.close();
});

for (const [what, o, why] of [['whose script fails', { fail: true }, 'worker.js: 404'],
  ['without shared memory', { isolated: false }, 'not cross-origin isolated']]) {
  test(`a worker ${what}: the link runs here, with what the page did meanwhile`, async () => {
    fakeWorkers(o);
    const proc = fakeProc(), logs = [];
    const link = attachNetWorker(proc, { log: s => logs.push(s) });
    link.setRoom('Blue Room');
    link.setSolid('on');
    link.setCrews([0x77], []);
    await tick(50);
    assert.equal(workers[0].terminated, true);
    assert.ok(logs.some(s => s.includes('no worker for the link') && s.includes(why)), logs.join('\n'));
    const i32 = mailbox(proc);
    proc.register(NET, PTR, P.NET_MAILBOX_BYTES);
    await tick(20);
    assert.equal(i32[P.W_ROOM_GEN], 1, 'the room set before');
    assert.equal(i32[P.W_SOLID], 2, 'solid cars on');
    assert.equal(i32[P.W_CREWS] >>> 0, 0x77);
    assert.equal(link.say(2), true);
    assert.equal(i32[P.W_SAY] & 0xff, 2);
    link.close();
    assert.equal(link.state, 'closed');
  });
}
