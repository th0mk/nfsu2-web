// Shared layout of the host's control memory (a SharedArrayBuffer separate
// from the wasm memory) and the message types between the page and workers.

export const MAX_THREADS = 64;

// ctl: Int32Array over the control SAB
export const CTL_NEXT_TID = 0;       // next thread id handed out by thread-spawn
export const CTL_IO_DOORBELL = 1;    // bumped by a thread that posted an I/O request
export const CTL_EXIT = 2;           // 1 once the process exited (proc_exit / _start returned)
export const CTL_EXIT_CODE = 3;
export const CTL_SLEEP = 4;          // a word nobody notifies: Atomics.wait on it = sleep
export const CTL_POOL_READY = 5;     // bumped and notified as each pool worker becomes ready (thread-spawn waits on it)
export const CTL_POOL = 64;          // pool slot i: [CTL_POOL + 4*i + 0] state, +1 tid, +2 start arg
// a slot's state: idle (its worker waits for a thread), 1 claimed, 2 running a thread, the process entry, or no worker
// there (yet: the page starts workers as threads need them, process.ts)
export const POOL_IDLE = 0, POOL_RUN_MAIN = -1, POOL_NONE = -2;
export const CTL_INTS = CTL_POOL + 4 * MAX_THREADS + 64;

// rpc: one slot per pool slot (a thread only ever uses its own slot)
//   i32[0] state  (0 idle, 1 posted, 2 taken by the I/O worker, 3 done)
//   i32[1] op     i32[2] result (WASI errno)   i32[3] nargs
//   i64[2..9] args (BigInt64Array index 2 = byte 16)
//   bytes 128..RPC_SLOT: scratch (unused for now; every pointer goes into wasm memory)
export const RPC_SLOT = 256;
export const RPC_BYTES = RPC_SLOT * MAX_THREADS;
export const RPC_IDLE = 0, RPC_POSTED = 1, RPC_TAKEN = 2, RPC_DONE = 3;

// file operations forwarded to the I/O worker (WASI preview1 semantics)
export const enum Op {
  FdClose = 1, FdRead, FdPread, FdWrite, FdPwrite, FdSeek, FdTell, FdSync, FdFdstatGet, FdFdstatSetFlags,
  FdFilestatGet, FdFilestatSetSize, FdReaddir, FdPrestatGet, FdPrestatDirName, FdAdvise, FdAllocate,
  PathOpen, PathFilestatGet, PathCreateDirectory, PathRemoveDirectory, PathUnlinkFile, PathRename,
  PathFilestatSetTimes, FdFilestatSetTimes, FdRenumber,
  // the movies' AV1 pictures (movie.ts, runtime/movie.c): not WASI
  MovieOpen, MovieDecode, MovieClose,
}

// WASI errno values used by the host
export const E = {
  SUCCESS: 0, ACCES: 2, AGAIN: 6, BADF: 8, EXIST: 20, FAULT: 21, INVAL: 28, IO: 29, ISDIR: 31, MFILE: 33,
  NAMETOOLONG: 37, NOENT: 44, NOSPC: 51, NOSYS: 52, NOTDIR: 54, NOTEMPTY: 55, NOTSUP: 58, PERM: 63, ROFS: 69, SPIPE: 70,
} as const;

export interface BootConfig {
  args: string[];
  env: Record<string, string>;
  dataBase: string;        // URL prefix of the install folder ("https://<host><base>data/"), absolute
  manifestUrl: string;     // the data-manifest's URL, absolute: blocks.url is resolved against it
  saveDir: string;         // OPFS directory name for the save overlay
}

export type ToPage =
  | { t: 'log'; s: string; fd: number; tid: number }
  | { t: 'exit'; code: number }
  | { t: 'trap'; tid: number; msg: string; stack?: string }
  | { t: 'ready'; slot: number }
  // a thread took a pool worker, or found none idle: the page starts more (process.ts)
  | { t: 'pool' };
