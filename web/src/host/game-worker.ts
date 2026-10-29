// A game thread.  The page starts a pool of these; each instantiates the
// game module on the shared memory and parks until it is handed either the
// process entry (_start) or a thread started through wasi thread-spawn.
// Everything that blocks (sleeps, waits, synchronous file I/O) blocks this
// worker only, never the page.
import {
  CTL_EXIT, CTL_EXIT_CODE, CTL_IO_DOORBELL, CTL_NEXT_TID, CTL_POOL, CTL_POOL_READY, CTL_SLEEP, E, MAX_THREADS, Op,
  POOL_IDLE, POOL_NONE, POOL_RUN_MAIN, RPC_DONE, RPC_IDLE, RPC_POSTED, RPC_SLOT, type BootConfig, type ToPage,
} from './proto';

interface Init { mod: WebAssembly.Module; memory: WebAssembly.Memory; ctl: SharedArrayBuffer; rpc: SharedArrayBuffer;
  slot: number; pool: number; cfg: BootConfig; extraImports?: string[] }

class ProcExit { constructor(public code: number) {} }

let memory: WebAssembly.Memory, ctl: Int32Array, slot = 0, POOL = 0, cfg: BootConfig;
let rpcI32: Int32Array, rpcI64: BigInt64Array;
let tid = 0;
// time this thread spent blocked in the host (file I/O round trips, sleeps),
// in ms: the runtime subtracts it from the time it held the guest lock to get
// the guest thread's busy time (runtime/win32_ext.c, [status] gcpu)
let blockedMs = 0;
const post = (m: ToPage) => (self as unknown as Worker).postMessage(m);

const u8 = () => new Uint8Array(memory.buffer);
const dv = () => new DataView(memory.buffer);
const te = new TextEncoder(), td = new TextDecoder();

// ---- logging: stdout/stderr lines go to the page ----
const pending: Record<number, string> = { 1: '', 2: '' };
function writeOut(fd: number, bytes: Uint8Array) {
  pending[fd] += td.decode(bytes, { stream: true });
  let k: number;
  while ((k = pending[fd].indexOf('\n')) >= 0) { post({ t: 'log', s: pending[fd].slice(0, k), fd, tid }); pending[fd] = pending[fd].slice(k + 1); }
}

// ---- synchronous call into the I/O worker ----
function io(op: Op, ...args: (number | bigint)[]): number {
  const b = slot * (RPC_SLOT / 4), b64 = slot * (RPC_SLOT / 8);
  rpcI32[b + 1] = op; rpcI32[b + 3] = args.length;
  for (let i = 0; i < args.length; i++) rpcI64[b64 + 2 + i] = BigInt(args[i]);
  Atomics.store(rpcI32, b, RPC_POSTED);
  Atomics.add(ctl, CTL_IO_DOORBELL, 1); Atomics.notify(ctl, CTL_IO_DOORBELL);
  const t0 = performance.now();
  // wait on the value just checked: re-loading it for Atomics.wait could read
  // RPC_DONE (the I/O worker finished in between) and then sleep on DONE
  // forever, the notify already gone (seen: a CreateFileA that never returned)
  for (let v; (v = Atomics.load(rpcI32, b)) !== RPC_DONE;) Atomics.wait(rpcI32, b, v);
  blockedMs += performance.now() - t0;
  const r = rpcI32[b + 2];
  Atomics.store(rpcI32, b, RPC_IDLE);
  return r;
}

// The clock every guest thread reads (clock_time_get, now_ms): the page's, performance.timeOrigin + performance.now()
// there.  A worker's own timeOrigin is the wall clock when it was made, so a worker made after a step of the system
// clock (WSL steps it: 1.92 s in a LAN run of 2026-10-30) runs that far from the others, and a guest thread on it
// saw time jump when it read another's stamps: the game's LAN link (DirtySock, its threads on two workers) took a
// peer for broken and gave up ("Could not connect to the other player").  So each worker asks the page for its clock
// before it runs a thread (alignClock) and keeps the offset: T0MS is the page's clock at this worker's
// performance.now() 0, read once, not on each of the guest's ~1,000 timer reads a frame.
let T0MS = performance.timeOrigin;
let T0US = Math.round(T0MS * 1e3);
const monoNs = () => BigInt(Math.round(performance.now() * 1e3)) * 1000n + BigInt(T0US) * 1000n;
// clock_time_get is the guest's most frequent host import (QueryPerformanceCounter, timeGetTime: ~100 calls
// in a race frame).  It writes the same values as BigInt µs × 1000 would, but from integer microseconds held in doubles
// (exact below 2^53) as two u32 halves: no BigInt and no DataView per call.
let clockReply: ((pageNow: number | null) => void) | null = null;
/** the page's clock against this worker's: the round trip of a 'clock' message, the shortest of up to 5 (a page that
 *  does not answer in 2 s leaves this worker's own clock); a note when the offset is worth a line */
async function alignClock(): Promise<string> {
  let best = Infinity, off = 0;
  for (let k = 0; k < 5 && best > 2; k++) {
    const t1 = performance.now();
    const pageNow = await new Promise<number | null>(res => { clockReply = res; post({ t: 'clock' }); setTimeout(() => res(null), 2000); });
    clockReply = null;
    const t2 = performance.now();
    if (pageNow === null) break;
    if (t2 - t1 < best) { best = t2 - t1; off = pageNow - (performance.timeOrigin + (t1 + t2) / 2); }
  }
  if (best === Infinity) return '';
  T0MS = performance.timeOrigin + off; T0US = Math.round(T0MS * 1e3);
  return Math.abs(off) >= 20 ? `[clock] game worker ${slot}: its own clock was ${off.toFixed(1)} ms off the page's `
    + `(a step of the system clock); it keeps the page's (round trip ${best.toFixed(1)} ms)` : '';
}
let u32v: Uint32Array | null = null;
function writeUsAsNs(p: number, us: number) {
  if (p & 7) { dv().setBigUint64(p, BigInt(us) * 1000n, true); return; }
  if (!u32v || u32v.buffer !== memory.buffer) u32v = new Uint32Array(memory.buffer);
  const hi = Math.floor(us / 4294967296), lo = (us - hi * 4294967296) * 1000, carry = Math.floor(lo / 4294967296);
  u32v[p >>> 2] = lo - carry * 4294967296; u32v[(p >>> 2) + 1] = hi * 1000 + carry;
}

// NFSU2_TEST_TIMER_MS=15.625 (tests): every sleep ends on that grid of the clock, as a wait does in Chrome on
// Windows while the OS timer ticks every 15.6 ms (runtime/win32_ext.c, the frame pacer's sleep)
let timerGrid = -1;
function sleepNs(ns: bigint) {
  let ms = Number(ns) / 1e6;
  if (timerGrid < 0) timerGrid = Number(cfg.env.NFSU2_TEST_TIMER_MS) || 0;
  if (ms > 0 && timerGrid > 0) { const now = T0MS + performance.now(); ms = Math.ceil((now + ms) / timerGrid) * timerGrid - now; }
  if (ms > 0) { const t0 = performance.now(); Atomics.wait(ctl, CTL_SLEEP, 0, ms); blockedMs += performance.now() - t0; }
}

function strings(list: string[]) { return list.map(s => te.encode(s + '\0')); }

function wasi(): Record<string, (...a: any[]) => any> {
  const argv = strings(cfg.args);
  const envv = strings(Object.entries(cfg.env).map(([k, v]) => `${k}=${v}`));
  const sizes = (l: Uint8Array[], pc: number, ps: number) => {
    dv().setUint32(pc, l.length, true); dv().setUint32(ps, l.reduce((a, b) => a + b.length, 0), true); return 0; };
  const fill = (l: Uint8Array[], pv: number, pb: number) => {
    const d = dv(), m = u8(); let o = pb;
    l.forEach((b, i) => { d.setUint32(pv + 4 * i, o, true); m.set(b, o); o += b.length; }); return 0; };
  return {
    args_sizes_get: (pc: number, ps: number) => sizes(argv, pc, ps),
    args_get: (pv: number, pb: number) => fill(argv, pv, pb),
    environ_sizes_get: (pc: number, ps: number) => sizes(envv, pc, ps),
    environ_get: (pv: number, pb: number) => fill(envv, pv, pb),
    clock_res_get: (_id: number, p: number) => { dv().setBigUint64(p, 1000n, true); return 0; },
    clock_time_get: (id: number, _prec: bigint, p: number) => {
      // 0 realtime, 1 monotonic, 2/3 process/thread CPU time (wall time here)
      writeUsAsNs(p, id === 0 ? Math.round((T0MS + performance.now()) * 1e3) : Math.round(performance.now() * 1e3) + T0US);
      return 0; },
    random_get: (p: number, n: number) => {
      const b = new Uint8Array(n); for (let o = 0; o < n; o += 65536) crypto.getRandomValues(b.subarray(o, Math.min(n, o + 65536)));
      u8().set(b, p); return 0; },
    sched_yield: () => 0,
    proc_exit: (code: number) => { throw new ProcExit(code); },
    proc_raise: () => E.NOSYS,
    poll_oneoff: (inp: number, outp: number, n: number, pn: number) => {
      // clock subscriptions only (nanosleep, usleep): sleep until the earliest one
      const d = dv(); let wait = -1n; const now = monoNs();
      for (let i = 0; i < n; i++) {
        const s = inp + 48 * i;
        if (d.getUint8(s + 8) !== 0) continue;
        let t = d.getBigUint64(s + 24, true);
        if (d.getUint16(s + 40, true) & 1) t = t > now ? t - now : 0n;       // abstime
        if (wait < 0n || t < wait) wait = t;
      }
      if (wait > 0n) sleepNs(wait);
      let k = 0;
      for (let i = 0; i < n; i++) {
        const s = inp + 48 * i, o = outp + 32 * k++;
        d.setBigUint64(o, d.getBigUint64(s, true), true); d.setUint16(o + 8, 0, true); d.setUint8(o + 10, d.getUint8(s + 8));
      }
      d.setUint32(pn, k, true); return 0;
    },
    fd_write: (fd: number, iov: number, n: number, pw: number) => {
      if (fd === 1 || fd === 2) {
        const d = dv(); let t = 0;
        for (let i = 0; i < n; i++) { const b = d.getUint32(iov + 8 * i, true), l = d.getUint32(iov + 8 * i + 4, true);
          writeOut(fd, u8().slice(b, b + l)); t += l; }
        d.setUint32(pw, t, true); return 0;
      }
      return io(Op.FdWrite, fd, iov, n, pw);
    },
    fd_read: (fd: number, iov: number, n: number, pr: number) => fd === 0 ? (dv().setUint32(pr, 0, true), 0) : io(Op.FdRead, fd, iov, n, pr),
    fd_pread: (fd: number, iov: number, n: number, off: bigint, pr: number) => io(Op.FdPread, fd, iov, n, off, pr),
    fd_pwrite: (fd: number, iov: number, n: number, off: bigint, pw: number) => io(Op.FdPwrite, fd, iov, n, off, pw),
    fd_seek: (fd: number, off: bigint, wh: number, p: number) => io(Op.FdSeek, fd, off, wh, p),
    fd_tell: (fd: number, p: number) => io(Op.FdTell, fd, p),
    fd_close: (fd: number) => (fd <= 2 ? 0 : io(Op.FdClose, fd)),
    fd_sync: (fd: number) => (fd <= 2 ? 0 : io(Op.FdSync, fd)),
    fd_datasync: (fd: number) => (fd <= 2 ? 0 : io(Op.FdSync, fd)),
    fd_fdstat_get: (fd: number, p: number) => {
      if (fd <= 2) { const d = dv(); d.setUint8(p, 2); d.setUint16(p + 2, 0, true); d.setBigUint64(p + 8, 0xFFFFFFFFn, true); d.setBigUint64(p + 16, 0n, true); return 0; }
      return io(Op.FdFdstatGet, fd, p);
    },
    fd_fdstat_set_flags: (fd: number, fl: number) => io(Op.FdFdstatSetFlags, fd, fl),
    fd_fdstat_set_rights: () => 0,
    fd_filestat_get: (fd: number, p: number) => io(Op.FdFilestatGet, fd, p),
    fd_filestat_set_size: (fd: number, sz: bigint) => io(Op.FdFilestatSetSize, fd, sz),
    fd_filestat_set_times: (fd: number, a: bigint, m: bigint, fl: number) => io(Op.FdFilestatSetTimes, fd, a, m, fl),
    fd_readdir: (fd: number, buf: number, len: number, cookie: bigint, pu: number) => io(Op.FdReaddir, fd, buf, len, cookie, pu),
    fd_prestat_get: (fd: number, p: number) => io(Op.FdPrestatGet, fd, p),
    fd_prestat_dir_name: (fd: number, p: number, l: number) => io(Op.FdPrestatDirName, fd, p, l),
    fd_advise: () => 0,
    fd_allocate: (fd: number, off: bigint, len: bigint) => io(Op.FdAllocate, fd, off, len),
    fd_renumber: (a: number, b: number) => io(Op.FdRenumber, a, b),
    path_open: (dfd: number, dfl: number, p: number, pl: number, of: number, rb: bigint, ri: bigint, ff: number, pfd: number) =>
      io(Op.PathOpen, dfd, dfl, p, pl, of, rb, ri, ff, pfd),
    path_filestat_get: (dfd: number, fl: number, p: number, pl: number, out: number) => io(Op.PathFilestatGet, dfd, fl, p, pl, out),
    path_filestat_set_times: (dfd: number, fl: number, p: number, pl: number, a: bigint, m: bigint, f2: number) =>
      io(Op.PathFilestatSetTimes, dfd, fl, p, pl, a, m, f2),
    path_create_directory: (dfd: number, p: number, pl: number) => io(Op.PathCreateDirectory, dfd, p, pl),
    path_remove_directory: (dfd: number, p: number, pl: number) => io(Op.PathRemoveDirectory, dfd, p, pl),
    path_unlink_file: (dfd: number, p: number, pl: number) => io(Op.PathUnlinkFile, dfd, p, pl),
    path_rename: (a: number, ap: number, al: number, b: number, bp: number, bl: number) => io(Op.PathRename, a, ap, al, b, bp, bl),
    path_link: () => E.NOSYS, path_symlink: () => E.NOSYS, path_readlink: () => E.NOSYS,
    sock_accept: () => E.NOSYS, sock_recv: () => E.NOSYS, sock_send: () => E.NOSYS, sock_shutdown: () => E.NOSYS,
  };
}

// The page keeps a few workers idle beyond the threads running (process.ts) and starts more as threads take them.  A
// thread that finds none idle all the same asks for one and waits for it, as a slow CreateThread would (a worker takes
// ~50 ms to start; at most 10 s here).
const SPAWN_WAIT_MS = 10_000;
function threadSpawn(arg: number): number {
  const t = Atomics.add(ctl, CTL_NEXT_TID, 1), t0 = performance.now();
  for (let asked = false; ;) {
    const ready = Atomics.load(ctl, CTL_POOL_READY);
    for (let s = 0; s < POOL; s++) {
      const i = CTL_POOL + 4 * s;
      if (Atomics.compareExchange(ctl, i, POOL_IDLE, 1) === POOL_IDLE) {   // claim the slot
        ctl[i + 1] = t; ctl[i + 2] = arg;
        Atomics.store(ctl, i, 2); Atomics.notify(ctl, i);
        post({ t: 'pool' });
        if (asked) post({ t: 'log', s: `thread-spawn: thread ${t} waited ${(performance.now() - t0).toFixed(0)} ms for a pool worker`, fd: 2, tid });
        return t;
      }
    }
    const left = SPAWN_WAIT_MS - (performance.now() - t0);
    if (left <= 0) break;
    if (!asked) { asked = true; post({ t: 'pool' }); }
    Atomics.wait(ctl, CTL_POOL_READY, ready, left);
  }
  post({ t: 'log', s: `thread-spawn: no pool worker for thread ${t} within ${SPAWN_WAIT_MS / 1000} s (at most ${POOL})`, fd: 2, tid });
  return -6; // EAGAIN
}

// Engine call depth (runtime/web/stackprobe.c, NFSU2_STACKPROBE): the number
// of frames on this thread's stack (wasm and JS: every lifted guest call is a
// wasm frame), from a stack trace.  Bytes would be the better measure, but a
// recursion probe is not stable in Chrome (the same context fit 4536 to 7352
// frames of an interpreted probe function), and frame sizes are not exposed.
const V8Error = Error as ErrorConstructor & { stackTraceLimit: number };   // V8's knob
// A trap's stack (the 'trap' message below, "TRAP in thread" in the log) keeps this many frames: V8's default of
// 10 cut the lifted guest calls off before the caller that mattered.
V8Error.stackTraceLimit = 100;
function stackFrames(): number {
  const lim = V8Error.stackTraceLimit;
  V8Error.stackTraceLimit = 1e6;
  const n = (new Error().stack ?? '').split('\n').length - 2;   // minus the message line and this function
  V8Error.stackTraceLimit = lim;
  return n;
}

// Host imports the runtime may call by name ("nfsu2" module).  The page adds
// subsystems (gpu, audio, input, net) by registering shared-memory addresses.
function hostImports(): Record<string, (...a: any[]) => any> {
  return {
    // nfsu2_host_register(kind, ptr, size): tells the page where a shared structure lives
    register: (kind: number, ptr: number, size: number) => { (self as unknown as Worker).postMessage({ t: 'register', kind, ptr, size }); return 0; },
    // milliseconds since the page's time origin (same clock in every worker)
    now_ms: () => T0MS + performance.now(),
    // ms this thread has spent blocked in host waits (I/O, sleeps) so far
    blocked_ms: () => blockedMs,
    stack_frames: () => stackFrames(),
    // the movies' AV1 pictures, decoded by the I/O worker (movie.ts, runtime/movie.c)
    movie_open: (w: number, h: number) => io(Op.MovieOpen, w, h),
    movie_decode: (m: number, p: number, n: number, y: number, u: number, v: number, ys: number, uvs: number) =>
      io(Op.MovieDecode, m, p, n, y, u, v, ys, uvs),
    movie_close: (m: number) => io(Op.MovieClose, m),
  };
}

onmessage = async (e: MessageEvent<Init | { t: 'clock'; now: number }>) => {
  if ('t' in e.data) { clockReply?.(e.data.now); return; }     // the page's clock (alignClock)
  const m = e.data;
  memory = m.memory; ctl = new Int32Array(m.ctl); slot = m.slot; POOL = m.pool; cfg = m.cfg;
  const clockNote = await alignClock();
  if (clockNote) post({ t: 'log', s: clockNote, fd: 2, tid: 0 });
  rpcI32 = new Int32Array(m.rpc); rpcI64 = new BigInt64Array(m.rpc);
  const imports: WebAssembly.Imports = {
    env: { memory },
    wasi_snapshot_preview1: wasi(),
    wasi: { 'thread-spawn': threadSpawn },
    nfsu2: hostImports(),
  };
  // stub any import the module wants that we do not provide, loudly
  for (const im of WebAssembly.Module.imports(m.mod)) {
    const ns = (imports[im.module] ??= {}) as Record<string, unknown>;
    if (im.kind === 'function' && !(im.name in ns))
      ns[im.name] = (...a: unknown[]) => { post({ t: 'log', s: `unimplemented import ${im.module}.${im.name}(${a.join(',')})`, fd: 2, tid }); return E.NOSYS; };
  }
  const inst = await WebAssembly.instantiate(m.mod, imports);
  const ex = inst.exports as Record<string, any>;
  const i = CTL_POOL + 4 * slot;
  // ready: a thread may take this slot now (one waiting in threadSpawn is woken)
  Atomics.compareExchange(ctl, i, POOL_NONE, POOL_IDLE);
  Atomics.add(ctl, CTL_POOL_READY, 1); Atomics.notify(ctl, CTL_POOL_READY);
  post({ t: 'ready', slot });
  for (;;) {
    let st: number;
    while ((st = Atomics.load(ctl, i)) === POOL_IDLE || st === 1) Atomics.wait(ctl, i, st);
    try {
      if (st === POOL_RUN_MAIN) {
        tid = 0;
        try { ex._start(); Atomics.store(ctl, CTL_EXIT_CODE, 0); }
        catch (x) { if (x instanceof ProcExit) Atomics.store(ctl, CTL_EXIT_CODE, x.code); else throw x; }
        Atomics.store(ctl, CTL_EXIT, 1); Atomics.notify(ctl, CTL_EXIT);
        post({ t: 'exit', code: ctl[CTL_EXIT_CODE] });
        return;
      }
      tid = ctl[i + 1];
      try { ex.wasi_thread_start(tid, ctl[i + 2]); }
      catch (x) {
        if (x instanceof ProcExit) { Atomics.store(ctl, CTL_EXIT_CODE, x.code); Atomics.store(ctl, CTL_EXIT, 1); post({ t: 'exit', code: x.code }); return; }
        throw x;
      }
    } catch (x: any) {
      post({ t: 'trap', tid, msg: String(x), stack: x?.stack });
      // a trap in any thread takes the process down, as an unhandled fault would on Windows
      Atomics.store(ctl, CTL_EXIT_CODE, 134); Atomics.store(ctl, CTL_EXIT, 1); post({ t: 'exit', code: 134 });
      return;
    }
    Atomics.store(ctl, i, POOL_IDLE);
  }
};
void MAX_THREADS;
