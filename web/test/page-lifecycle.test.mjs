// The page's subsystems at the end of a game, with fake browser objects (no
// Chrome): what each one lets go of, and what happens to requests made too
// early or too late (phase-2 integrate review, findings 1, 3, 4 and 5).
//   node --test web/test/page-lifecycle.test.mjs
// The same rules in a real Chrome: web/tools/app-test.mjs steps leak, late,
// slowgpu and quitstages.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const src = p => JSON.stringify(path.join(ROOT, 'web/src', p));
const entry = `
export { attachAudio } from ${src('audio/index.ts')};
export { attachInput } from ${src('input/index.ts')};
export { attachRenderer } from ${src('gpu/index.ts')};
export * as IP from ${src('input/proto.ts')};
export * as GP from ${src('gpu/proto.ts')};
export * as AP from ${src('audio/proto.ts')};
`;
const bundle = async (stdin) => {
  const js = (await esbuild.build({ stdin: { contents: stdin, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false,
    // the page resolves its worklet against its own address; a data: URL cannot be a base
    loader: { '.txt': 'text' }, define: { 'import.meta.url': '"http://localhost/main.js"' } })).outputFiles[0].text;
  return import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
};

// ---- a minimal browser: events, timers, document, window -------------------
class Target {
  constructor() { this.l = new Map(); }
  addEventListener(t, f) { (this.l.get(t) ?? this.l.set(t, new Set()).get(t)).add(f); }
  removeEventListener(t, f) { this.l.get(t)?.delete(f); }
  dispatchEvent(e) { for (const f of this.l.get(e.type) ?? []) f(e); return true; }
  count() { let n = 0; for (const s of this.l.values()) n += s.size; return n; }
}
const win = new Target(), doc = new Target();
Object.assign(doc, { hidden: false, visibilityState: 'visible', fullscreenElement: null, hasFocus: () => true, querySelector: () => null });
const g = globalThis;
g.window = win; g.document = doc;
g.addEventListener = (t, f) => win.addEventListener(t, f);
g.removeEventListener = (t, f) => win.removeEventListener(t, f);
g.dispatchEvent = e => win.dispatchEvent(e);
g.requestAnimationFrame = () => 1; g.cancelAnimationFrame = () => {};
g.devicePixelRatio = 1;
g.ResizeObserver = class { observe() {} disconnect() {} };
g.GPUTextureUsage = { RENDER_ATTACHMENT: 16 };
g.HTMLElement = class {};
const nav = { platform: 'MacIntel', userAgent: 'node' };
Object.defineProperty(globalThis, 'navigator', { value: nav, configurable: true, writable: true });

const { attachAudio, attachInput, attachRenderer, IP, GP, AP } = await bundle(entry);

const tick = (ms = 0) => new Promise(r => setTimeout(r, ms));
/** a game process that registers its blocks and exits when the test says so */
function fakeProc() {
  const memory = new WebAssembly.Memory({ initial: 4, maximum: 8, shared: true });
  let exit; const exited = new Promise(r => (exit = r));
  const regs = new Map(), waiters = new Map();
  return {
    memory, exited, exit,
    registered(kind) {
      if (regs.has(kind)) return Promise.resolve(regs.get(kind));
      return new Promise(r => (waiters.get(kind) ?? waiters.set(kind, []).get(kind)).push(r));
    },
    register(kind, ptr, size) { regs.set(kind, { ptr, size }); for (const r of waiters.get(kind) ?? []) r({ ptr, size }); waiters.delete(kind); },
    terminate() { exit(-1); },
  };
}

// ---- audio ------------------------------------------------------------------------
test('audio: detach ends the worklet (stop, then close after it stopped) and leaves nothing on the context', async () => {
  const made = [];
  g.AudioWorkletNode = class {
    constructor(ctx, name, o) {
      this.ctx = ctx; this.options = o; this.sent = []; this.disconnected = false; made.push(this);
      const node = this;
      this.port = { onmessage: null, closed: false, postMessage(m) { node.sent.push(m); }, close() { this.closed = true; } };
    }
    connect() {} disconnect() { this.disconnected = true; }
  };
  const ctx = {
    sampleRate: 44100, state: 'running', baseLatency: 0, outputLatency: 0, currentTime: 0, destination: {}, onstatechange: null, closes: 0,
    audioWorklet: { addModule: async () => {} },
    createGain() { return { gain: { value: 1, setTargetAtTime() {} }, connect() {} }; },
    async close() { this.closes++; this.state = 'closed'; },
    async resume() {},
  };
  const proc = fakeProc();
  const ptr = 4096;
  const h = new Int32Array(proc.memory.buffer, ptr, AP.AR_HDR_WORDS);
  h[AP.AR_MAGIC] = AP.AUDIO_MAGIC; h[AP.AR_VERSION] = AP.AUDIO_VERSION;
  const logs = [];
  const before = win.count() + doc.count();
  const a = await attachAudio(proc, { context: ctx, gestureTarget: win, log: s => logs.push(s) });
  proc.register(2 /* HostKind.Audio */, ptr, 4096);
  await tick();
  assert.equal(made.length, 1, 'the worklet node is made once the ring registers');
  const node = made[0];
  assert.equal(typeof ctx.onstatechange, 'function');
  a.detach();
  assert.equal(ctx.onstatechange, null, 'the handler (it closes over the game) is gone at once');
  assert.deepEqual(node.sent, ['stop'], 'the processor is told to stop');
  assert.equal(ctx.closes, 0, 'the context still runs until the processor returned false');
  node.port.onmessage({ data: 'stopped' });
  await tick();
  assert.equal(ctx.closes, 1);
  assert.ok(node.disconnected && node.port.closed && node.port.onmessage === null);
  assert.ok(logs.includes('[audio] context closed'));
  assert.equal(win.count() + doc.count(), before, 'no listener left on window or document');
});

test('audio: a context that is not running is closed at once (the stop is still sent)', async () => {
  const made = [];
  g.AudioWorkletNode = class {
    constructor() { this.sent = []; made.push(this); const n = this; this.port = { onmessage: null, postMessage(m) { n.sent.push(m); }, close() {} }; }
    connect() {} disconnect() {}
  };
  const ctx = {
    sampleRate: 44100, state: 'running', baseLatency: 0, outputLatency: 0, currentTime: 0, destination: {}, onstatechange: null, closes: 0,
    audioWorklet: { addModule: async () => {} }, createGain() { return { gain: { value: 1, setTargetAtTime() {} }, connect() {} }; },
    async close() { this.closes++; }, async resume() {},
  };
  const proc = fakeProc();
  const h = new Int32Array(proc.memory.buffer, 4096, AP.AR_HDR_WORDS);
  h[AP.AR_MAGIC] = AP.AUDIO_MAGIC; h[AP.AR_VERSION] = AP.AUDIO_VERSION;
  const a = await attachAudio(proc, { context: ctx, gestureTarget: win, log: () => {} });
  proc.register(2, 4096, 4096);
  await tick();
  ctx.state = 'suspended';
  a.detach();
  assert.deepEqual(made[0].sent, ['stop']);
  assert.equal(ctx.closes, 1);
  assert.equal(ctx.onstatechange, null);
});

test('audio worklet: after "stop" the processor drops its views, says "stopped" and process() returns false', async () => {
  const posted = [];
  let Proc = null;
  g.sampleRate = 44100;
  g.AudioWorkletProcessor = class { constructor() { this.port = { onmessage: null, postMessage: m => posted.push(m) }; } };
  g.registerProcessor = (_name, c) => { Proc = c; };
  await bundle(`import ${src('audio/worklet.ts')};`);
  const p = new Proc({ processorOptions: { buf: new SharedArrayBuffer(1 << 20), ptr: 0 } });
  const out = [[new Float32Array(128), new Float32Array(128)]];
  assert.equal(p.process([], out), true, 'running: true (an underrun, silence)');
  p.port.onmessage({ data: 'stop' });
  assert.equal(p.h, null); assert.equal(p.pcm, null);
  assert.equal(p.process([], out), false);
  assert.deepEqual(posted, ['stopped']);
  assert.equal(p.port.onmessage, null);
});

// ---- input ------------------------------------------------------------------------
function mailbox(proc, ptr) {
  const h = new Int32Array(proc.memory.buffer, ptr, IP.IM_HDR_WORDS);
  h[IP.IM_MAGIC] = IP.INPUT_MAGIC; h[IP.IM_VERSION] = IP.INPUT_VERSION;
  return h;
}
const events = (proc, ptr, h) => {
  const u8 = new Uint8Array(proc.memory.buffer), out = [];
  for (let i = 0; i < Atomics.load(h, IP.IM_EV_W); i++) out.push(u8[ptr + IP.IM_OFF_EV + i * 16]);
  return out;
};
const element = () => Object.assign(new Target(), { style: {}, tabIndex: 0, getBoundingClientRect: () => ({ left: 0, top: 0, width: 100, height: 100 }) });

test('input: a quit requested before the mailbox registers is sent when it does', async () => {
  const proc = fakeProc(), ptr = 8192, h = mailbox(proc, ptr), logs = [];
  const i = attachInput(proc, element(), { log: s => logs.push(s), gamepads: false });
  assert.equal(i.canQuit(), true);
  assert.equal(i.requestQuit(), true, 'accepted: it waits for the mailbox');
  assert.deepEqual(events(proc, ptr, h), []);
  proc.register(3 /* HostKind.Input */, ptr, 65536);
  await tick();
  assert.ok(events(proc, ptr, h).includes(IP.IEV_QUIT), 'the waiting quit reached the event ring');
  assert.ok(logs.some(l => /sending the quit request made before/.test(l)));
  i.detach();
});

test('input: with the event ring taken by the game (ns* input) a quit cannot reach it, and says so', async () => {
  const proc = fakeProc(), ptr = 8192, h = mailbox(proc, ptr), logs = [];
  Atomics.store(h, IP.IM_ATTACHED, 2);
  const i = attachInput(proc, element(), { log: s => logs.push(s), gamepads: false });
  assert.equal(i.requestQuit(), true);
  proc.register(3, ptr, 65536);
  await tick();
  assert.equal(i.canQuit(), false);
  assert.equal(i.requestQuit(), false);
  assert.ok(logs.some(l => /the quit request cannot reach the game/.test(l)));
  assert.deepEqual(events(proc, ptr, h), []);
  i.detach();
});

test('input: detach removes every listener and refuses later quits', async () => {
  const proc = fakeProc(), ptr = 8192; mailbox(proc, ptr);
  const before = win.count() + doc.count(), el = element();
  const i = attachInput(proc, el, { log: () => {}, gamepads: false });
  proc.register(3, ptr, 65536);
  await tick();
  i.detach();
  assert.equal(win.count() + doc.count(), before);
  assert.equal(el.count(), 0);
  assert.equal(i.requestQuit(), false);
});

/** the event ring's records but the focus changes: type, flags, a, b */
const ring = (proc, ptr, h) => {
  const dv = new DataView(proc.memory.buffer), out = [];
  for (let i = 0; i < Atomics.load(h, IP.IM_EV_W); i++) {
    const o = ptr + IP.IM_OFF_EV + i * 16;
    out.push({ type: dv.getUint8(o), flags: dv.getUint8(o + 3), a: dv.getInt32(o + 8, true), b: dv.getInt32(o + 12, true) });
  }
  return out.filter(e => e.type !== IP.IEV_FOCUS);
};
const mouseEv = (type, x, y, more = {}) => ({ type, clientX: x, clientY: y, isTrusted: true, altKey: false, ctrlKey: false,
  shiftKey: false, preventDefault() {}, ...more });

test('input: a wheel turn sends the place it turned at first (IKF_WHEEL_AT): the pointer may be there without a move', async () => {
  const proc = fakeProc(), ptr = 8192, h = mailbox(proc, ptr), el = element();
  const i = attachInput(proc, el, { log: () => {}, gamepads: false });
  proc.register(3, ptr, 65536);
  await tick();
  el.dispatchEvent(mouseEv('wheel', 25, 75, { deltaY: -100, deltaMode: 0 }));   // a notch up, a quarter across, 3/4 down
  assert.deepEqual(ring(proc, ptr, h), [
    { type: IP.IEV_MOUSE_MOVE, flags: IP.IKF_WHEEL_AT, a: 16384, b: 49152 },
    { type: IP.IEV_WHEEL, flags: 0, a: 120, b: 0 },
  ]);
  i.detach();
});

// ---- renderer ---------------------------------------------------------------------
function fakeGpu() {
  const rec = { devices: [], unconfigured: 0 };
  nav.gpu = {
    getPreferredCanvasFormat: () => 'bgra8unorm',
    async requestAdapter() {
      return {
        features: new Set(['texture-compression-bc']), limits: { maxBufferSize: 1 << 28, maxStorageBufferBindingSize: 1 << 27 }, info: {},
        async requestDevice() {
          let lose;
          const d = Object.assign(new Target(), { destroyed: false, features: new Set(['texture-compression-bc']), lost: new Promise(r => { lose = r; }) });
          // as a browser does: destroy() loses the device too, reason 'destroyed'
          d.lose = (reason, message = '') => lose({ reason, message });
          d.destroy = () => { d.destroyed = true; d.lose('destroyed'); };
          rec.devices.push(d);
          return d;
        },
      };
    },
  };
  const canvas = { width: 64, height: 64, getContext: () => ({ configure() {}, unconfigure() { rec.unconfigured++; } }) };
  return { rec, canvas };
}

test('renderer: a game that ends before it registers its graphics block ends the wait and gives the device back', async () => {
  const { rec, canvas } = fakeGpu();
  const proc = fakeProc();
  const p = attachRenderer(proc, canvas, { log: () => {} });
  await tick(10);
  assert.equal(rec.devices.length, 1);
  proc.exit(-1);
  await assert.rejects(p, /the game ended \(code -1\) before it registered its graphics/);
  assert.equal(rec.devices[0].destroyed, true);
  assert.equal(rec.unconfigured, 1);
  assert.equal(rec.devices[0].count(), 0, 'no uncapturederror listener left');
});

test('renderer: a lost device says so (the page stops the game); its own destroy() does not', async () => {
  const { rec, canvas } = fakeGpu();
  const proc = fakeProc(), logs = [];
  let lost = 0;
  const p = attachRenderer(proc, canvas, { log: l => logs.push(l), onLost: () => lost++ });
  await tick(10);
  rec.devices[0].lose('unknown', 'GPU process crashed');
  await tick();
  assert.equal(lost, 1);
  assert.match(logs.join('\n'), /\[gpu\] device lost: unknown GPU process crashed/);
  proc.exit(-1);
  await assert.rejects(p, /before it registered its graphics/);
  assert.equal(lost, 1, 'not again for the destroy');

  const second = fakeGpu(), proc2 = fakeProc();
  let lost2 = 0;
  const p2 = attachRenderer(proc2, second.canvas, { log: () => {}, onLost: () => lost2++ });
  await tick(10);
  proc2.exit(-1);
  await assert.rejects(p2, /before it registered its graphics/);
  await tick();
  assert.equal(second.rec.devices[0].destroyed, true);
  assert.equal(lost2, 0, 'giving the device back is no loss');
});

test('renderer: a block the game marked detached (it stopped waiting) refuses a late renderer', async () => {
  const { rec, canvas } = fakeGpu();
  const proc = fakeProc();
  const ptr = 16384, u = new Uint32Array(proc.memory.buffer);
  // const enums are inlined by esbuild and not exported: word indices from gpu_proto.h
  const cw = ptr >> 2; u[cw + 0] = GP.GW_MAGIC; u[cw + 2] = GP.LAYOUT; u[cw + 6] = GP.DETACHED;
  proc.register(1 /* Gpu */, ptr, 4096); proc.register(6 /* Vsync */, ptr + 8192, 64);
  await assert.rejects(attachRenderer(proc, canvas, { log: () => {} }), /takes no renderer any more/);
  assert.equal(rec.devices[0].destroyed, true);
  assert.equal(Atomics.load(new Int32Array(proc.memory.buffer), cw + 6), GP.DETACHED, 'the block stays detached');
});
