// Starting the game process from the page: the shared memory, the control
// and RPC blocks, the I/O worker and a pool of game-thread workers.
import { CTL_EXIT, CTL_INTS, CTL_NEXT_TID, CTL_POOL, MAX_THREADS, POOL_IDLE, POOL_NONE, POOL_RUN_MAIN, RPC_BYTES, type BootConfig, type ReadAnswer, type ReadAsk, type ToPage } from './proto';
import { tzEnv } from './tz';
import { movieCodec } from './movie';
import { allocMemory, memoryOffPage, memoryTries, MemoryBuffer, type GameMemory } from './memory';

export interface ProcessOptions {
  module: WebAssembly.Module;
  cfg: BootConfig;
  pool?: number;                         // game-thread workers at most (started as threads need them; default 24)
  poolStart?: number;                    // ... and at first (default POOL_START)
  initialPages?: number;                 // wasm pages (64 KiB) of the shared memory at start
  /** the page holds the memory's buffer only, not the WebAssembly.Memory (memory.ts; default: in WebKit) */
  memoryOffPage?: boolean;
  onLog?: (s: string, fd: number, tid: number) => void;
  onExit?: (code: number) => void;
  onRegister?: (kind: number, ptr: number, size: number) => void;
  /** the game closed a file of the save overlay it had written (a profile, registry.txt ...), path relative to /save */
  onSaved?: (path: string) => void;
  /** a line of the host's own for the log, from no game thread (the I/O worker's "[data] ...") */
  onNote?: (s: string) => void;
  /** the browser deleted the save folder during the game, and the I/O worker made an empty one: the saves back into
   *  it, before the game's next save there (the I/O worker holds every save call until this resolves, 12 s at most) */
  onSaveGone?: () => Promise<void>;
  /** the game deleted a file of the save overlay: a profile's (its own Delete), path relative to /save */
  onRemoved?: (path: string) => void;
  /** a file the game wrote as the browser deleted the save folder could not be kept (path relative to /save) */
  onLost?: (path: string) => void;
  /** a file of the save overlay the browser would not open or make, the folder still there (the first of a game: its
   *  error, as "Name: message"); its saves may not be kept */
  onSaveTrouble?: (error: string) => void;
}

export interface GameProcess {
  /** the WebAssembly.Memory, or in WebKit its buffer only (memory.ts MemoryBuffer: the newest as it grows) */
  memory: GameMemory;
  /** f gets each new buffer of the memory as it grows, where the page holds the buffer only (the network's worker) */
  onGrow(f: (buf: SharedArrayBuffer) => void): void;
  ctl: Int32Array;
  exited: Promise<number>;
  terminate(): void;
  /** resolves with the structure a subsystem published (runtime/web/host_abi.h) */
  registered(kind: number): Promise<{ ptr: number; size: number }>;
  /** bytes [off, off + len) of a game-folder file, through the game data's cache (the I/O worker); exact: those bytes
   *  alone from the server, not kept */
  readData(path: string, off: number, len: number, exact?: boolean): Promise<Uint8Array<ArrayBuffer>>;
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
/** how long the I/O worker may take to say it is ready (startProcess) */
const IO_START_MS = 45_000;
/** ... and the first pool of game workers after it */
const POOL_START_MS = 45_000;

export async function startProcess(o: ProcessOptions): Promise<GameProcess> {
  const pool = Math.min(o.pool ?? 24, MAX_THREADS);
  // the page's time zone (WASI has none); an explicit NFSU2_TZ in the config wins
  const cfg: BootConfig = { ...o.cfg, env: { ...tzEnv(), ...o.cfg.env } };
  // which movies the I/O worker opens (the page decides before the game starts: shell.ts; a test page here)
  cfg.env.NFSU2_MOVIE_CODEC = await movieCodec(cfg.env);
  const initial = o.initialPages ?? memoryPagesOf(o.module) ?? 16384;
  // the build's size, able to grow to 4 GB, or not growing where the browser refuses that (memory.ts); on a computer
  // 2 GB first, for the guest heap's window (NFSU2_HEAP_WINDOW=1: app/shell.ts, or a test page's env); the log says which
  const tries = memoryTries(initial, cfg.env.NFSU2_HEAP_WINDOW === '1');
  // In WebKit the page holds the memory's buffer only (memory.ts): the I/O worker makes the Memory and sends the page
  // its buffer, and each game thread gets the Memory from there on a port (withMemoryPort).  `kept`: the page's own
  // Memory elsewhere, made here as before
  const offPage = o.memoryOffPage ?? memoryOffPage();
  let kept: WebAssembly.Memory | null = null;
  if (!offPage) {
    let got: ReturnType<typeof allocMemory>;
    try { got = allocMemory(tries); }
    catch (e) { const n = (e as { note?: string } | null)?.note; if (n) o.onNote?.(n); throw e; }
    o.onNote?.(got.note);
    kept = got.memory;
  }
  let memory: GameMemory | null = kept;
  const growers: ((buf: SharedArrayBuffer) => void)[] = [];
  const ctlBuf = new SharedArrayBuffer(CTL_INTS * 4), rpc = new SharedArrayBuffer(RPC_BYTES);
  const ctl = new Int32Array(ctlBuf);
  ctl[CTL_NEXT_TID] = 1;

  const regs = new Map<number, { ptr: number; size: number }>();
  const regWaiters = new Map<number, ((r: { ptr: number; size: number }) => void)[]>();
  let resolveExit!: (c: number) => void;
  const exited = new Promise<number>(r => (resolveExit = r));
  const workers: Worker[] = [];
  const onMsg = (e: MessageEvent<ToPage | { t: 'register'; kind: number; ptr: number; size: number; bytes?: number }>) => {
    const m = e.data;
    if (m.t === 'log') o.onLog?.(m.s, m.fd, m.tid);
    else if (m.t === 'trap') o.onLog?.(`TRAP in thread ${m.tid}: ${m.msg}\n${m.stack ?? ''}`, 2, m.tid);
    else if (m.t === 'pool') topUp();
    else if (m.t === 'memory-please') {
      if (!viaPage) { viaPage = true; o.onNote?.(`[memory] game worker ${m.slot}: ${m.why}; the game threads get it through the page`); }
      io.postMessage({ t: 'give-page', slot: m.slot });
    }
    else if (m.t === 'exit') {
      o.onNote?.(`[pool] ${started} game workers started, at most ${mostBusy} threads at once`);
      o.onExit?.(m.code); resolveExit(m.code);
    }
    else if (m.t === 'register') {
      const done = () => {
        regs.set(m.kind, { ptr: m.ptr, size: m.size });
        for (const w of regWaiters.get(m.kind) ?? []) w({ ptr: m.ptr, size: m.size });
        regWaiters.delete(m.kind);
        o.onRegister?.(m.kind, m.ptr, m.size);
      };
      // a structure (and what its subsystem made before it) in memory grown since the page's buffer: once the I/O worker
      // sent the new buffer (memory.ts), not before, or the page reads past its buffer
      const need = m.bytes ?? 0;
      const buf = memory;
      if (buf instanceof MemoryBuffer && need > buf.buffer.byteLength) void buf.until(need).then(ok => {
        if (!ok) o.onNote?.(`[memory] a grown memory's buffer did not come in time (${Math.round(need / 1048576)} MB)`);
        done();
      });
      else done();
    }
  };

  const io = new Worker(new URL('/host/io-worker.js', import.meta.url), { type: 'module', name: 'io' });
  workers.push(io);
  // Ready in IO_START_MS at most (the game data's list from the server, the save folder from the browser's storage): one
  // that never answers is a game that does not start, said so, never a black stage for good.
  await new Promise<void>((res, rej) => {
    const late = setTimeout(() => {
      io.terminate();
      rej(new Error(`the game's files did not open in ${IO_START_MS / 1000} s (the browser's storage or the network did not answer)`));
    }, IO_START_MS);
    // (its lines as it starts too: playing without saving, what it copied into memory)
    io.onmessage = (e) => {
      if (e.data.t === 'io-ready') { clearTimeout(late); res(); }
      // the memory it made (offPage): its buffer, and the line saying which size
      else if (e.data.t === 'memory') { memory = new MemoryBuffer(e.data.buf); o.onNote?.(e.data.note); }
      else if (e.data.t === 'io-error') {
        clearTimeout(late);
        io.terminate();                      // (and with it a memory it made)
        // (a size refused: as the page's own refusal would be, for its message to the player and the log's line)
        if (e.data.note) o.onNote?.(e.data.note);
        rej(e.data.range ? new RangeError(e.data.msg) : new Error(e.data.msg));
      }
      else if (e.data.t === 'log') o.onNote?.(e.data.s);
    };
    io.postMessage(offPage ? { tries, ctl: ctlBuf, rpc, cfg } : { memory: kept, ctl: ctlBuf, rpc, cfg });
  });
  if (!memory) throw new Error('the I/O worker made no memory');
  const mem: GameMemory = memory;
  const reads = new Map<number, { res: (b: Uint8Array<ArrayBuffer>) => void; rej: (e: Error) => void }>();
  let readId = 0;
  io.onmessage = (e) => {
    if (e.data.t === 'read') {
      const a = e.data as ReadAnswer, r = reads.get(a.id);
      reads.delete(a.id);
      if (r) { if (a.bytes) r.res(a.bytes); else r.rej(new Error(a.error ?? 'no bytes')); }
    }
    else if (e.data.t === 'saved') o.onSaved?.(e.data.path);
    else if (e.data.t === 'removed') o.onRemoved?.(e.data.path);
    else if (e.data.t === 'lost') o.onLost?.(e.data.path);
    else if (e.data.t === 'save-trouble') o.onSaveTrouble?.(e.data.error);
    else if (e.data.t === 'log') o.onNote?.(e.data.s);
    // the Memory for a game thread where the I/O worker could not post it on a port (withMemoryPort)
    else if (e.data.t === 'give-here') bySlot.get(e.data.slot)?.postMessage({ t: 'memory', memory: e.data.memory });
    // the memory grew: its new buffer (where the page holds the buffer only)
    else if (e.data.t === 'mem') {
      if (memory instanceof MemoryBuffer && memory.grew(e.data.buf)) {
        for (const f of growers) f(e.data.buf);
        o.onNote?.(`[memory] ${Math.round(e.data.buf.byteLength / 1048576)} MB now (it grew)`);
      }
    }
    else if (e.data.t === 'save-gone')
      void Promise.resolve().then(() => o.onSaveGone?.()).catch(err => o.onNote?.(`[app] the saves could not be put back: ${err}`))
        .finally(() => io.postMessage({ t: 'save-ready' }));
  };

  // the port first to the worker, then its other end to the I/O worker, which posts the Memory on it
  const withMemoryPort = (slot: number, send: (port: MessagePort) => void) => {
    const ch = new MessageChannel();
    send(ch.port2);
    io.postMessage({ t: 'give', port: ch.port1, slot }, [ch.port1]);
  };
  const bySlot = new Map<number, Worker>();
  let viaPage = false;
  for (let s = 0; s < pool; s++) ctl[CTL_POOL + 4 * s] = POOL_NONE;   // each worker makes its slot idle when ready
  let started = 0, starting = 0, mostBusy = 0;
  const startWorker = () => {
    const s = started++, w = new Worker(new URL('/host/game-worker.js', import.meta.url), { type: 'module', name: `game-${s}` });
    starting++;
    const ready = new Promise<void>(res => {
      w.onmessage = (e) => { if (e.data.t === 'ready') { starting--; w.onmessage = onMsg; res(); } else onMsg(e); };
    });
    w.onerror = (e) => o.onNote?.(`[pool] game worker ${s}: ${e.message}`);
    bySlot.set(s, w);
    if (offPage) withMemoryPort(s, port => w.postMessage({ mod: o.module, memPort: port, ctl: ctlBuf, rpc, slot: s, pool, cfg }, [port]));
    else w.postMessage({ mod: o.module, memory: kept, ctl: ctlBuf, rpc, slot: s, pool, cfg });
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
  // the first pool ready in POOL_START_MS at most: one that never is (a thread whose memory never came) is a game that
  // does not start, said so, never a black stage for good
  let poolLate: ReturnType<typeof setTimeout> | undefined;
  try {
    await Promise.race([Promise.all(Array.from({ length: Math.max(1, Math.min(o.poolStart ?? POOL_START, pool)) }, startWorker)),
      new Promise((_, no) => { poolLate = setTimeout(() => no(new Error(`the game's threads did not start in ${POOL_START_MS / 1000} s`)), POOL_START_MS); })]);
  } catch (e) {
    for (const w of workers) w.terminate();
    throw e;
  } finally { clearTimeout(poolLate); }
  // slot 0 runs the process entry
  Atomics.store(ctl, CTL_POOL, POOL_RUN_MAIN); Atomics.notify(ctl, CTL_POOL);

  return {
    memory: mem, ctl, exited,
    onGrow(f) { growers.push(f); },
    registered(kind: number) {
      const r = regs.get(kind);
      if (r) return Promise.resolve(r);
      return new Promise(res => { const l = regWaiters.get(kind) ?? []; l.push(res); regWaiters.set(kind, l); });
    },
    readData(path: string, off: number, len: number, exact?: boolean) {
      return new Promise<Uint8Array<ArrayBuffer>>((res, rej) => {
        const id = ++readId;
        reads.set(id, { res, rej });
        io.postMessage({ t: 'read', id, path, off, len, exact } satisfies ReadAsk);
      });
    },
    terminate() { for (const w of workers) w.terminate(); if (!Atomics.load(ctl, CTL_EXIT)) resolveExit(-1); },
  };
}
