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
export const CTL_IO_BEAT = 6;        // bumped by the I/O worker's loop (every 50 ms at most): a thread whose request
                                     // goes unanswered tells a stuck request from a stopped worker (game-worker.ts io)
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
  // the I/O worker's clock (its time origin + now, ms) as a float64 into the asking slot's scratch (arg 0: the slot):
  // the clock every game worker reads (game-worker.ts alignClock)
  Clock,
}
/** the ops' names for the log, by number (Op is a const enum: no names at run time); in Op's order */
export const OP_NAMES = ['', 'fd_close', 'fd_read', 'fd_pread', 'fd_write', 'fd_pwrite', 'fd_seek', 'fd_tell', 'fd_sync',
  'fd_fdstat_get', 'fd_fdstat_set_flags', 'fd_filestat_get', 'fd_filestat_set_size', 'fd_readdir', 'fd_prestat_get',
  'fd_prestat_dir_name', 'fd_advise', 'fd_allocate', 'path_open', 'path_filestat_get', 'path_create_directory',
  'path_remove_directory', 'path_unlink_file', 'path_rename', 'path_filestat_set_times', 'fd_filestat_set_times',
  'fd_renumber', 'movie_open', 'movie_decode', 'movie_close', 'clock'];
export const opName = (op: number) => OP_NAMES[op] ?? `op ${op}`;
/** A request to the I/O worker unanswered this long is written in the log (by the asking thread, game-worker.ts io,
 *  and by the I/O worker while its loop runs), and again at each doubling: a read that never comes back stops the
 *  game's file thread, and every read after it waits (2026-11-02, a player's free roam: 12 min, then a hang). */
export const IO_SLOW_MS = 20000;

// WASI errno values used by the host
export const E = {
  SUCCESS: 0, ACCES: 2, AGAIN: 6, BADF: 8, EXIST: 20, FAULT: 21, INVAL: 28, IO: 29, ISDIR: 31, MFILE: 33,
  NAMETOOLONG: 37, NOENT: 44, NOSPC: 51, NOSYS: 52, NOTDIR: 54, NOTEMPTY: 55, NOTSUP: 58, PERM: 63, ROFS: 69, SPIPE: 70,
} as const;

export interface BootConfig {
  args: string[];
  env: Record<string, string>;
  dataBase: string;        // URL prefix of the install folder (".../data/")
  manifestUrl: string;
  saveDir: string;         // OPFS directory name for the save overlay
}

export type ToPage =
  | { t: 'log'; s: string; fd: number; tid: number }
  | { t: 'exit'; code: number }
  | { t: 'trap'; tid: number; msg: string; stack?: string }
  | { t: 'ready'; slot: number }
  // a thread took a pool worker, or found none idle: the page starts more (process.ts)
  | { t: 'pool' };
