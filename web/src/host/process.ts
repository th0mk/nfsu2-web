// Starting the game process from the page: the shared memory, the control
// and RPC blocks, the I/O worker and a pool of game-thread workers.
import { CTL_EXIT, CTL_INTS, CTL_NEXT_TID, CTL_POOL, MAX_THREADS, POOL_IDLE, POOL_NONE, POOL_RUN_MAIN, RPC_BYTES, type BootConfig, type ToPage } from './proto';
import { tzEnv } from './tz';
import { movieCodec } from './movie';
import { abs } from '../base';

export interface ProcessOptions {
  module: WebAssembly.Module;
  cfg: BootConfig;
  pool?: number;                         // game-thread workers at most (started as threads need them; default 24)
  poolStart?: number;                    // ... and at first (default POOL_START)
  initialPages?: number;                 // wasm pages (64 KiB) of the shared memory at start
  onLog?: (s: string, fd: number, tid: number) => void;
  onExit?: (code: number) => void;
  onRegister?: (kind: number, ptr: number, size: number) => void;
  /** the game closed a file of the save overlay it had written (a profile, registry.txt ...), path relative to /save */
  onSaved?: (path: string) => void;
  /** a line of the host's own for the log, from no game thread (the I/O worker's "[data] ...") */
  onNote?: (s: string) => void;
}

export interface GameProcess {
  memory: WebAssembly.Memory;
  ctl: Int32Array;
  exited: Promise<number>;
  terminate(): void;
  /** resolves with the structure a subsystem published (runtime/web/host_abi.h) */
  registered(kind: number): Promise<{ ptr: number; size: number }>;
}

function memoryPagesOf(mod: WebAssembly.Module): number | null {
  // the module imports env.memory; its declared minimum is not exposed by the
  // JS API, so the build writes it into a custom section "nfsu2.mem"
  const s = WebAssembly.Module.customSections(mod, 'nfsu2.mem');
  return s.length ? new DataView(s[0]).getUint32(0, true) : null;
}

/** Game-thread workers: POOL_START at first, then as threads take them so that POOL_SPARE stay idle, up to the pool's
 *  size (a thread start that finds none idle waits for the next: game-worker.ts threadSpawn).  Each takes memory, a
 *  phone's: a race or free roam starts 15 (at most 13 threads at once), hosting a LAN game 18, and all 24 at once held
 *  ~55 MB more of WebKit's page process.  12 at first, fewer than any game ends with: its first second starts 10
 *  threads, and with 8 or 10 workers at first the 10th waited up to 15 ms for one. */
const POOL_START = 12, POOL_SPARE = 2;

export async function startProcess(o: ProcessOptions): Promise<GameProcess> {
  const pool = Math.min(o.pool ?? 24, MAX_THREADS);
  // the page's time zone (WASI has none); an explicit NFSU2_TZ in the config wins
  const cfg: BootConfig = { ...o.cfg, env: { ...tzEnv(), ...o.cfg.env } };
  // which movies the I/O worker opens (the page decides before the game starts: shell.ts; a test page here)
  cfg.env.NFSU2_MOVIE_CODEC = await movieCodec(cfg.env);
  const initial = o.initialPages ?? memoryPagesOf(o.module) ?? 16384;
  const memory = new WebAssembly.Memory({ initial, maximum: 65536, shared: true });
  const ctlBuf = new SharedArrayBuffer(CTL_INTS * 4), rpc = new SharedArrayBuffer(RPC_BYTES);
  const ctl = new Int32Array(ctlBuf);
  ctl[CTL_NEXT_TID] = 1;

  const regs = new Map<number, { ptr: number; size: number }>();
  const regWaiters = new Map<number, ((r: { ptr: number; size: number }) => void)[]>();
  let resolveExit!: (c: number) => void;
  const exited = new Promise<number>(r => (resolveExit = r));
  const workers: Worker[] = [];
  const onMsg = (e: MessageEvent<ToPage | { t: 'register'; kind: number; ptr: number; size: number }>) => {
    const m = e.data;
    if (m.t === 'log') o.onLog?.(m.s, m.fd, m.tid);
    else if (m.t === 'trap') o.onLog?.(`TRAP in thread ${m.tid}: ${m.msg}\n${m.stack ?? ''}`, 2, m.tid);
    else if (m.t === 'pool') topUp();
    else if (m.t === 'exit') {
      o.onNote?.(`[pool] ${started} game workers started, at most ${mostBusy} threads at once`);
      o.onExit?.(m.code); resolveExit(m.code);
    }
    else if (m.t === 'register') {
      regs.set(m.kind, { ptr: m.ptr, size: m.size });
      for (const w of regWaiters.get(m.kind) ?? []) w({ ptr: m.ptr, size: m.size });
      regWaiters.delete(m.kind);
      o.onRegister?.(m.kind, m.ptr, m.size);
    }
  };

  const io = new Worker(abs('host/io-worker.js'), { type: 'module', name: 'io' });
  workers.push(io);
  await new Promise<void>((res, rej) => {
    io.onmessage = (e) => { if (e.data.t === 'io-ready') res(); else if (e.data.t === 'io-error') rej(new Error(e.data.msg)); };
    io.postMessage({ memory, ctl: ctlBuf, rpc, cfg });
  });
  io.onmessage = (e) => { if (e.data.t === 'saved') o.onSaved?.(e.data.path); else if (e.data.t === 'log') o.onNote?.(e.data.s); };

  for (let s = 0; s < pool; s++) ctl[CTL_POOL + 4 * s] = POOL_NONE;   // each worker makes its slot idle when ready
  let started = 0, starting = 0, mostBusy = 0;
  const startWorker = () => {
    const s = started++, w = new Worker(abs('host/game-worker.js'), { type: 'module', name: `game-${s}` });
    starting++;
    const ready = new Promise<void>(res => {
      w.onmessage = (e) => {
        // the worker sets its clock by the page's (game-worker.ts alignClock): one timeline in every worker
        if (e.data.t === 'clock') w.postMessage({ t: 'clock', now: performance.timeOrigin + performance.now() });
        else if (e.data.t === 'ready') { starting--; w.onmessage = onMsg; res(); } else onMsg(e);
      };
    });
    w.onerror = (e) => o.onNote?.(`[pool] game worker ${s}: ${e.message}`);
    w.postMessage({ mod: o.module, memory, ctl: ctlBuf, rpc, slot: s, pool, cfg });
    workers.push(w);
    return ready;
  };
  // a thread took a worker (or found none): POOL_SPARE idle or starting again
  const topUp = () => {
    let idle = 0;
    for (let s = 0; s < started; s++) if (Atomics.load(ctl, CTL_POOL + 4 * s) === POOL_IDLE) idle++;
    mostBusy = Math.max(mostBusy, started - starting - idle);
    for (let n = idle + starting; n < POOL_SPARE && started < pool; n++) void startWorker();
  };
  await Promise.all(Array.from({ length: Math.max(1, Math.min(o.poolStart ?? POOL_START, pool)) }, startWorker));
  // slot 0 runs the process entry
  Atomics.store(ctl, CTL_POOL, POOL_RUN_MAIN); Atomics.notify(ctl, CTL_POOL);

  return {
    memory, ctl, exited,
    registered(kind: number) {
      const r = regs.get(kind);
      if (r) return Promise.resolve(r);
      return new Promise(res => { const l = regWaiters.get(kind) ?? []; l.push(res); regWaiters.set(kind, l); });
    },
    terminate() { for (const w of workers) w.terminate(); if (!Atomics.load(ctl, CTL_EXIT)) resolveExit(-1); },
  };
}
