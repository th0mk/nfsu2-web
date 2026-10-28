// The I/O worker: the process's file system.  Game threads post WASI file
// calls into their RPC slot and block; this worker serves them asynchronously
// (so a slow read for one thread never holds up another) and writes results
// straight into the shared wasm memory.
//
// Mounts (preopened directories, in this order: fd 3, fd 4):
//   /data  the NFSU2 install folder on the local server, read-only.  Read in
//          1 MiB blocks (compressed from a server that offers it, else
//          HTTP range requests), kept in memory, and
//          persisted in OPFS ("cache/", datacache.ts) so the next launch reads
//          locally ("Preload the game data" fills all of it ahead).
//   /save  the save overlay: OPFS directory, read/write (saves, registry,
//          settings.ini, logs).  Closing a file the game wrote there tells
//          the page ({ t: 'saved', path }: "Progress saved", F9's last save).
import {
  CTL_IO_DOORBELL, E, MAX_THREADS, Op, RPC_DONE, RPC_POSTED, RPC_SLOT, RPC_TAKEN, type BootConfig,
} from './proto';
import { BLOCK, CACHE_DIR, av1Path, blockRuns, cacheKey, dataUrl, openCacheFile, putBlock, runUrl, type CacheFile, type Manifest } from './datacache';
import { movieClose, movieDecode, movieOpen } from './movie';

interface Init { memory: WebAssembly.Memory; ctl: SharedArrayBuffer; rpc: SharedArrayBuffer; cfg: BootConfig }

const FT_DIR = 3, FT_FILE = 4;
const td = new TextDecoder(), te = new TextEncoder();

let memory: WebAssembly.Memory, ctl: Int32Array, rpcI32: Int32Array, rpcI64: BigInt64Array, cfg: BootConfig;
const u8 = () => new Uint8Array(memory.buffer);
const dv = () => new DataView(memory.buffer);

// ---------------------------------------------------------------- /data ----
interface DNode { name: string; dir: boolean; size: number; mtime: number; kids?: Map<string, DNode>; byLower?: Map<string, DNode>; path: string; ino: number }
let dataRoot: DNode;
let inoNext = 16;
let runs: { url: string; max: number } | null = null;     // the server's compressed blocks (datacache.ts blockRuns)
function mkdirNode(name: string, path: string): DNode { return { name, dir: true, size: 0, mtime: 0, kids: new Map(), byLower: new Map(), path, ino: inoNext++ }; }

async function loadManifest() {
  const r = await fetch(cfg.manifestUrl, { cache: 'no-cache' });
  if (!r.ok) throw new Error(`manifest: HTTP ${r.status}`);
  const m = await r.json() as Manifest;
  runs = blockRuns(m, cfg.manifestUrl);
  dataRoot = mkdirNode('', '');
  for (const f of m.files) {
    const parts = f.p.split('/');
    let cur = dataRoot;
    for (let i = 0; i < parts.length - 1; i++) cur = cur.kids!.get(parts[i]) ?? addKid(cur, mkdirNode(parts[i], parts.slice(0, i + 1).join('/')));
    const leaf = parts[parts.length - 1];
    if (f.d) { if (!cur.kids!.has(leaf)) addKid(cur, mkdirNode(leaf, f.p)); }
    else addKid(cur, { name: leaf, dir: false, size: f.s ?? 0, mtime: f.m ?? 0, path: f.p, ino: inoNext++ });
  }
}
function addKid(dir: DNode, n: DNode) { dir.kids!.set(n.name, n); if (!dir.byLower!.has(n.name.toLowerCase())) dir.byLower!.set(n.name.toLowerCase(), n); return n; }
function dataLookup(rel: string): DNode | null {
  let cur = dataRoot;
  for (const part of rel.split('/')) {
    if (!part || part === '.') continue;
    if (part === '..') return null;
    if (!cur.dir) return null;
    const n = cur.kids!.get(part) ?? cur.byLower!.get(part.toLowerCase());
    if (!n) return null;
    cur = n;
  }
  return cur;
}
/** A file the game opens: a movie's AV1 copy in its place where the page plays AV1 (NFSU2_MOVIE_CODEC = av1,
 *  movie.ts) and the folder has one (tools/movies_av1.py), else the file itself. */
function dataFile(rel: string): DNode | null {
  const n = dataLookup(rel);
  if (!n || n.dir || cfg.env.NFSU2_MOVIE_CODEC !== 'av1') return n;
  const a = av1Path(n.path);
  return (a && dataLookup(a)) || n;
}

// block cache: memory LRU over OPFS-persisted blocks over HTTP.  While OPFS keeps the blocks, the memory only saves
// a read from it (a 1 MB block in 0.14 ms in WebKit, M4 Pro): 64 MB, the blocks being read.  The memory is the only
// cache without OPFS (no cache folder, or no room left on the disk): 768 MB then.
const memBlocks = new Map<string, Uint8Array>();
let memBytes = 0;
const MEM_KEPT = 64 << 20, MEM_ALONE = 768 << 20;
const inflight = new Map<string, Promise<Uint8Array>>();
let cacheDir: FileSystemDirectoryHandle | null = null;
// The cache files open now, the least recently used first.  WebKit gives a page 251 sync access handles at once
// (Playwright WebKit 27.2; then "UnknownError: Invalid platform file handle"), and a cache file holds two: kept open
// for good, the files of a long free roam took them all in 13 min, the game data's cache stopped, and so would have
// any file of the save folder the game opened after.  At most CACHE_OPEN stay open; opening one again takes < 1 ms.
const CACHE_OPEN = 64;
const cacheFiles = new Map<string, Promise<CacheFile | null>>();
let keeping = true, kept = 0;                              // blocks still go into the cache (keepBlock)

// Use the file at once, in the same task: a file that is not among the CACHE_OPEN newest is closed.
async function cacheFile(n: DNode) {
  if (!cacheDir) return null;
  const k = cacheKey(n);
  let p = cacheFiles.get(k);
  if (p) { cacheFiles.delete(k); cacheFiles.set(k, p); return p; }
  p = openCacheFile(cacheDir, n).catch(e => { if (keeping) note(`[data] ${n.path} is not kept on this PC: ${e}`); return null; });
  cacheFiles.set(k, p);
  for (const [ok, op] of cacheFiles) {
    if (cacheFiles.size <= CACHE_OPEN) break;
    cacheFiles.delete(ok);
    // after the reactions already waiting for it (a reader that just opened it uses it first)
    void op.then(cf => { if (cf) { cf.h.flush(); cf.h.close(); cf.haveH.flush(); cf.haveH.close(); } }).catch(() => {});
  }
  return p;
}

function memPut(key: string, b: Uint8Array) {
  memBlocks.set(key, b); memBytes += b.length;
  const budget = cacheDir && keeping ? MEM_KEPT : MEM_ALONE;
  for (const [k, v] of memBlocks) { if (memBytes <= budget) break; memBlocks.delete(k); memBytes -= v.length; }
}
// a line for the game's log (F9 > Download log), as "[data] ...": what a read waited for on the server
const note = (s: string) => (self as unknown as Worker).postMessage({ t: 'log', s });
// At most FETCHES block requests in flight (the server holds each client address to 64 requests at once, so a
// household of players behind one address stays well inside it).  A block comes compressed when the server offers
// it (the browser undoes the Content-Encoding), else as a byte range of the file.  A try that fails on the way (no
// connection, a reset, a busy server's 429, 5xx or Cloudflare's 52x) or stalls (no byte for STALL_MS) is made
// again after a pause, for as long as it takes, as the game's read of a slow disc would wait: the game cannot go
// on without the bytes, and a failed read reaches it as one of 0 bytes.  Only an answer that cannot change by
// asking again (another 4xx) fails the read, and a body of the wrong length after three tries.  After two tries
// with no answer at all, every other try asks for the block as a byte range instead: another address, which no
// shared cache keeps, uncompressed, from the server itself.  (A player's game waited minutes on one block whose
// compressed address failed with "Failed to fetch" on every try, while Cloudflare gave it to everyone else.)
// NFSU2_DATA_FAULT (tests, the web suite's datafault): fail:N fails every Nth try as a lost connection, hang:N
// leaves every Nth unanswered until its stall timer, stall:MS sets that timer, deny:N loses every try at the
// compressed address of each file's every Nth block (only its byte range comes), full:N fills this PC's cache
// at the Nth block this game keeps.
// Blocks waiting for a turn go by priority: the track's stream file first (a section the game waits for in the world
// is a "Loading" screen in the middle of a drive), then the game's other reads (a friend's car, music, speech), then
// the reading ahead of the track (readAhead).  That one starts only while the game fetches nothing (on a slow line a
// block read ahead would share it with the game's), FILL_FETCHES at most.
const FETCHES = 8, STALL_MS = 20000, SLOW_MS = 5000;
const P_AHEAD = 0, P_GAME = 1, P_TRACK = 2, FILL_FETCHES = 2;
interface Ticket { pri: number; ahead?: boolean }
let fetching = 0, aheadFetching = 0, tries = 0, fault: { fail: number; hang: number; stall: number; deny: number; full: number } | null = null;
const fetchWait: { t: Ticket; go: () => void }[] = [];
const mayFetch = (t: Ticket) => fetching < FETCHES &&
  (t.pri > P_AHEAD || fetching === aheadFetching && aheadFetching < FILL_FETCHES);
function fetchStart(t: Ticket) { fetching++; if ((t.ahead = t.pri === P_AHEAD)) aheadFetching++; }
// the waiting blocks that may go now, the most urgent (the earliest of those) first
function fetchNext() {
  for (;;) {
    let best = -1;
    for (let i = 0; i < fetchWait.length; i++) if (best < 0 || fetchWait[i].t.pri > fetchWait[best].t.pri) best = i;
    if (best < 0 || !mayFetch(fetchWait[best].t)) return;
    const w = fetchWait.splice(best, 1)[0];
    fetchStart(w.t); w.go();
  }
}
function faults() {
  if (!fault) {
    const kv = new Map((cfg.env?.NFSU2_DATA_FAULT ?? '').split(',').map(p => p.split(':') as [string, string]));
    const num = (k: string) => +(kv.get(k) ?? 0) || 0;
    fault = { fail: num('fail'), hang: num('hang'), stall: num('stall') || STALL_MS, deny: num('deny'), full: num('full') };
  }
  return fault;
}
/** one try: the block (as a byte range of the file, or the server's compressed run of it), or why it failed and
 *  whether asking again can help (lost: no answer at all) */
async function fetchOnce(url: string, start: number, len: number, ranged: boolean, lose: boolean):
    Promise<Uint8Array | { why: string; again: boolean; size?: boolean; lost?: boolean }> {
  const f = faults(), ac = new AbortController();
  let timer: ReturnType<typeof setTimeout> | undefined;
  const alive = () => { clearTimeout(timer); timer = setTimeout(() => ac.abort(), f.stall); };
  alive();
  try {
    const k = ++tries;
    if (lose || f.fail && k % f.fail === 0) throw new TypeError('lost connection (NFSU2_DATA_FAULT)');
    if (f.hang && k % f.hang === 0) await new Promise((_, no) => ac.signal.addEventListener('abort', () => no(new Error('aborted'))));
    const r = await fetch(url, { headers: ranged ? { Range: `bytes=${start}-${start + len - 1}` } : {}, cache: 'no-store', signal: ac.signal });
    if (r.status !== 206 && r.status !== 200) {
      await r.body?.cancel().catch(() => {});
      return { why: `HTTP ${r.status}`, again: r.status === 408 || r.status === 429 || r.status >= 500 };
    }
    // the body as it comes, so that only a stalled one runs into the timer
    const parts: Uint8Array[] = [];
    let got = 0;
    if (r.body) for (const rd = r.body.getReader(); ;) {
      alive();
      const { done, value } = await rd.read();
      if (done) break;
      parts.push(value); got += value.length;
    }
    let b = new Uint8Array(got);
    for (let i = 0, o = 0; i < parts.length; o += parts[i].length, i++) b.set(parts[i], o);
    if (r.status === 200 && ranged) b = b.slice(start, start + len);          // a range answered with the whole file: our block of it
    return b.length === len ? b : { why: `${b.length} of ${len} bytes`, again: true, size: true };
  } catch (e) {
    return { why: ac.signal.aborted ? `no answer for ${f.stall / 1000} s` : String((e as Error)?.message ?? e), again: true, lost: true };
  } finally { clearTimeout(timer); }
}
// A line a minute for the log while blocks come from the server (a first game, a new area): how many, how fast, how
// many had to be asked again, and what this device's cache read meanwhile.  A game the cache has wholly says nothing.
const SUM_MS = 60_000;
let sum = { t: 0, fetched: 0, bytes: 0, ms: 0, maxMs: 0, again: 0, local: 0, ahead: 0 };
function dataSummary(now: number) {
  if (!sum.t) { sum.t = now; return; }
  if (now - sum.t < SUM_MS) return;
  if (sum.fetched || sum.again)
    note(`[data] ${((now - sum.t) / 1000).toFixed(0)} s: ${sum.fetched} blocks from the server (${(sum.bytes / 1048576).toFixed(1)} MB, ` +
         `${sum.fetched ? (sum.ms / sum.fetched).toFixed(0) : 0} ms on average, the slowest ${sum.maxMs.toFixed(0)} ms${sum.again ? `, ${sum.again} tries again` : ''}${sum.ahead ? `, ${sum.ahead} read ahead` : ''}), ` +
         `${sum.local} from this device's cache, ${cacheFiles.size} cache files open, ${(memBytes / 1048576).toFixed(0)} MB in memory${keeping ? '' : ', no longer kept on this device'}`);
  sum = { t: now, fetched: 0, bytes: 0, ms: 0, maxMs: 0, again: 0, local: 0, ahead: 0 };
}
async function fetchBlock(n: DNode, bi: number, start: number, len: number, t: Ticket): Promise<Uint8Array> {
  if (!mayFetch(t) || fetchWait.some(w => w.t.pri >= t.pri)) await new Promise<void>(go => fetchWait.push({ t, go }));
  else fetchStart(t);
  const what = `${n.path} block ${bi}${t.ahead ? ' (read ahead)' : ''}`, t0 = performance.now();
  const slow = setTimeout(() => note(`[data] ${what}: waiting for the server for ${SLOW_MS / 1000} s`), SLOW_MS);
  try {
    const run = runs ? runUrl(runs.url, n.path, bi, 1, n) : null, range = dataUrl(cfg.dataBase, n.path), f = faults();
    for (let k = 1, wait = 250, sized = 0, lost = 0; ; k++, wait = Math.min(wait * 2, 8000)) {
      const ranged = !run || lost >= 2 && k % 2 === 1, by = run && ranged ? ' (as a byte range)' : '';
      const r = await fetchOnce(ranged ? range : run, start, len, ranged, !ranged && f.deny > 0 && (bi + 1) % f.deny === 0);
      if (r instanceof Uint8Array) {
        const s = (performance.now() - t0) / 1000;
        sum.fetched++; if (t.ahead) sum.ahead++; sum.bytes += r.length; sum.ms += s * 1000; if (s * 1000 > sum.maxMs) sum.maxMs = s * 1000;
        if (k > 1 || s >= SLOW_MS / 1000) note(`[data] ${what}: in after ${s.toFixed(1)} s${k > 1 ? `, ${k} tries` : ''}${by}`);
        return r;
      }
      if (r.lost) lost++;
      if (!r.again || (r.size && ++sized >= 3)) throw new Error(`${what}: ${r.why}${by}`);
      sum.again++;
      if (k <= 3 || k % 10 === 0) note(`[data] ${what}: ${r.why}${by}; again in ${wait / 1000} s (try ${k + 1})`);
      await new Promise(res => setTimeout(res, wait));
    }
  } finally { clearTimeout(slow); fetching--; if (t.ahead) aheadFetching--; fetchNext(); }
}
// A block this PC has no room to keep (a full disk, or the browser's share of it used up: "No space available for
// this operation") ends the keeping for this game, not the read: the game has the bytes either way.  (Thrown into
// the read, it gave the game a failed one, and the game crashed.)
function keepBlock(cf: CacheFile, n: DNode, bi: number, b: Uint8Array) {
  if (!keeping) return;
  try {
    const f = faults();
    if (f.full && ++kept >= f.full) throw new DOMException('No space available for this operation (NFSU2_DATA_FAULT)', 'QuotaExceededError');
    putBlock(cf, bi, b);
  } catch (e) {
    keeping = false;
    note(`[data] no room on this PC to keep more of the game data (${n.path} block ${bi}: ${(e as Error)?.message ?? e}); the rest comes from the server`);
  }
}
// the blocks being read: who waits for each (a read of the game asking for a block the reading ahead asked for first
// raises its priority to its own)
const tickets = new Map<string, Ticket>();
async function getBlock(n: DNode, bi: number, pri: number): Promise<Uint8Array> {
  const key = n.path + '#' + bi;
  dataSummary(performance.now());
  const hit = memBlocks.get(key);
  if (hit) { memBlocks.delete(key); memBlocks.set(key, hit); return hit; }
  let p = inflight.get(key);
  if (p) {
    const t = tickets.get(key);
    if (t && t.pri < pri) { t.pri = pri; fetchNext(); }
    return p;
  }
  const t: Ticket = { pri };
  p = (async () => {
    const start = bi * BLOCK, len = Math.min(BLOCK, n.size - start);
    const cf = await cacheFile(n);
    if (cf && cf.have[bi]) {
      try {
        const b = new Uint8Array(len); cf.h.read(b, { at: start }); memPut(key, b); sum.local++; return b;
      } catch (e) { note(`[data] ${n.path} block ${bi}: the cache file did not read (${e}); from the server`); }
    }
    const b = await fetchBlock(n, bi, start, len, t);
    // the file again: the one from before the fetch may have been closed meanwhile (CACHE_OPEN)
    const cf2 = await cacheFile(n);
    if (cf2) keepBlock(cf2, n, bi, b);
    // a block read ahead stays out of the memory while this PC keeps it: the blocks being read stay there
    if (t.pri > P_AHEAD || !cf2 || !keeping) memPut(key, b);
    return b;
  })();
  inflight.set(key, p); tickets.set(key, t);
  try { return await p; } finally { inflight.delete(key); tickets.delete(key); }
}

// ------------------------------------------------------- reading ahead ----
// The world streams in sections of its track's stream file (TRACKS\STREAML4RA.BUN, 113 MB, for the city), read as the
// car comes near.  From a disc a section takes milliseconds; from the server each of its blocks is a request, and a
// car on the freeway could reach a section before it came in: a "Loading" screen in the middle of the drive.  So once
// the game reads a track's stream file, the blocks of it this PC does not have yet come in at the lowest priority
// (above), the nearest ones to the game's latest read first, until this PC has all of it (40 MB on the network for
// the city's), and its sections then read from here.  Only while this PC keeps the game data: the memory alone has no
// room for a whole track.  NFSU2_TRACK_AHEAD=0 turns it off.
const isTrackStream = (n: DNode) => /^TRACKS\/STREAM[^/]*\.BUN$/i.test(n.path);
let ahead: { n: DNode; near: number; t0: number; got: number } | null = null, aheadLoops = 0;
const aheadDone = new Set<DNode>();
function readAhead(n: DNode, bi: number) {
  if (cfg.env?.NFSU2_TRACK_AHEAD === '0' || !cacheDir || !keeping || aheadDone.has(n)) return;
  if (ahead?.n === n) ahead.near = bi;
  else ahead = { n, near: bi, t0: performance.now(), got: 0 };
  while (aheadLoops < FILL_FETCHES) { aheadLoops++; void aheadLoop(); }
}
async function aheadLoop() {
  let a = ahead;
  try {
    while ((a = ahead) && keeping) {
      const cf = await cacheFile(a.n);
      if (!cf) { aheadDone.add(a.n); if (ahead === a) ahead = null; return; }
      let bi = -1, missing = 0;
      for (let i = 0; i < cf.have.length; i++) {
        if (cf.have[i]) continue;
        missing++;
        if (!inflight.has(a.n.path + '#' + i) && (bi < 0 || Math.abs(i - a.near) < Math.abs(bi - a.near))) bi = i;
      }
      if (!missing) {
        if (ahead === a) {
          ahead = null; aheadDone.add(a.n);
          if (a.got) note(`[data] ${a.n.path}: all of it on this device now, ${a.got} blocks read ahead in ${((performance.now() - a.t0) / 1000).toFixed(0)} s`);
        }
        return;
      }
      if (bi < 0) return;                        // the rest is on its way already
      await getBlock(a.n, bi, P_AHEAD); a.got++;
    }
  } catch (e) {
    if (a) { note(`[data] reading ${a.n.path} ahead stopped: ${(e as Error)?.message ?? e}`); aheadDone.add(a.n); if (ahead === a) ahead = null; }
  } finally { aheadLoops--; }
}

async function dataRead(n: DNode, pos: number, dst: { ptr: number; len: number }[]): Promise<number> {
  const total = dst.reduce((a, d) => a + d.len, 0);
  const end = Math.min(n.size, pos + total);
  if (end <= pos) return 0;
  const first = Math.floor(pos / BLOCK), last = Math.floor((end - 1) / BLOCK), track = isTrackStream(n);
  if (track) readAhead(n, first);
  const blocks = await Promise.all(Array.from({ length: last - first + 1 }, (_, i) => getBlock(n, first + i, track ? P_TRACK : P_GAME)));
  const mem = u8();
  let off = pos, di = 0, dOff = 0;
  while (off < end) {
    const b = blocks[Math.floor(off / BLOCK) - first], bo = off % BLOCK;
    const d = dst[di];
    const k = Math.min(b.length - bo, d.len - dOff, end - off);
    mem.set(b.subarray(bo, bo + k), d.ptr + dOff);
    off += k; dOff += k;
    if (dOff === d.len) { di++; dOff = 0; }
  }
  return end - pos;
}

// ---------------------------------------------------------------- /save ----
let saveRoot: FileSystemDirectoryHandle;
async function saveDirAt(parts: string[], create: boolean): Promise<FileSystemDirectoryHandle | null> {
  let d = saveRoot;
  for (const p of parts) {
    if (!p || p === '.') continue;
    if (p === '..') return null;
    try { d = await d.getDirectoryHandle(p, { create }); } catch { return null; }
  }
  return d;
}
async function saveEntry(rel: string): Promise<{ parent: FileSystemDirectoryHandle; name: string; kind: 'file' | 'directory' | null; h: FileSystemHandle | null } | null> {
  const parts = rel.split('/').filter(p => p && p !== '.');
  if (parts.some(p => p === '..')) return null;
  if (!parts.length) return { parent: saveRoot, name: '', kind: 'directory', h: saveRoot };
  const parent = await saveDirAt(parts.slice(0, -1), false);
  if (!parent) return null;
  const name = parts[parts.length - 1];
  try { return { parent, name, kind: 'file', h: await parent.getFileHandle(name) }; } catch {}
  try { return { parent, name, kind: 'directory', h: await parent.getDirectoryHandle(name) }; } catch {}
  return { parent, name, kind: null, h: null };
}

// ------------------------------------------------------------------ fds ----
interface Fd {
  mount: 'data' | 'save' | 'dev'; kind: 'dir' | 'file'; path: string; pos: number; append: boolean;
  node?: DNode;                                   // /data
  sh?: FileSystemSyncAccessHandle; dh?: FileSystemDirectoryHandle; fh?: FileSystemFileHandle;   // /save
  wrote?: boolean;                                // /save: written or truncated since it was opened
  preopen?: string; dirents?: { name: string; type: number; ino: number }[];
}
const fds = new Map<number, Fd>();
let nextFd = 5;
function allocFd(f: Fd) { while (fds.has(nextFd)) nextFd++; fds.set(nextFd, f); return nextFd++; }

function readPath(p: number, l: number) { return td.decode(u8().slice(p, p + l)); }
function joinRel(base: string, rel: string) { return (base ? base + '/' : '') + rel; }
function normRel(s: string) {
  const out: string[] = [];
  for (const p of s.split('/')) { if (!p || p === '.') continue; if (p === '..') { if (!out.length) return null; out.pop(); } else out.push(p); }
  return out.join('/');
}
function iovs(iov: number, n: number) {
  const d = dv(), a: { ptr: number; len: number }[] = [];
  for (let i = 0; i < n; i++) a.push({ ptr: d.getUint32(iov + 8 * i, true), len: d.getUint32(iov + 8 * i + 4, true) });
  return a;
}
function writeFilestat(p: number, ino: number, type: number, size: number, mtimeSec: number) {
  const d = dv(), ns = BigInt(Math.floor(mtimeSec)) * 1000000000n;
  d.setBigUint64(p, 1n, true); d.setBigUint64(p + 8, BigInt(ino), true); d.setUint8(p + 16, type);
  d.setBigUint64(p + 24, 1n, true); d.setBigUint64(p + 32, BigInt(size), true);
  d.setBigUint64(p + 40, ns, true); d.setBigUint64(p + 48, ns, true); d.setBigUint64(p + 56, ns, true);
}
const saveIno = new Map<string, number>();
const inoOf = (path: string) => { let i = saveIno.get(path); if (!i) { i = inoNext++; saveIno.set(path, i); } return i; };

async function statSave(rel: string): Promise<{ type: number; size: number; mtime: number } | null> {
  const e = await saveEntry(rel);
  if (!e || !e.kind) return null;
  if (e.kind === 'directory') return { type: FT_DIR, size: 0, mtime: 0 };
  const open = [...fds.values()].find(f => f.mount === 'save' && f.path === rel && f.sh);
  if (open) return { type: FT_FILE, size: open.sh!.getSize(), mtime: Date.now() / 1000 };
  const file = await (e.h as FileSystemFileHandle).getFile();
  return { type: FT_FILE, size: file.size, mtime: file.lastModified / 1000 };
}

// O flags (path_open oflags): 1 CREAT, 2 DIRECTORY, 4 EXCL, 8 TRUNC; fdflags: 1 APPEND
async function op(o: Op, a: bigint[]): Promise<number> {
  const n = (i: number) => Number(a[i]);
  switch (o) {
    case Op.FdPrestatGet: {
      const f = fds.get(n(0)); if (!f?.preopen) return E.BADF;
      const d = dv(); d.setUint8(n(1), 0); d.setUint32(n(1) + 4, te.encode(f.preopen).length, true); return 0;
    }
    case Op.FdPrestatDirName: {
      const f = fds.get(n(0)); if (!f?.preopen) return E.BADF;
      const b = te.encode(f.preopen); u8().set(b.subarray(0, n(2)), n(1)); return 0;
    }
    case Op.PathOpen: {
      const dir = fds.get(n(0)); if (!dir || dir.kind !== 'dir') return E.BADF;
      const rel = normRel(joinRel(dir.path, readPath(n(2), n(3)))); if (rel === null) return E.PERM;
      const oflags = n(4), fdflags = n(7), rights = a[5];
      const wantWrite = (rights & 0x40n) !== 0n || (oflags & (1 | 8)) !== 0;   // FD_WRITE right, or CREAT/TRUNC
      if (dir.mount === 'dev') {
        const name = rel;
        if (name !== 'null' && name !== 'zero' && name !== 'urandom') return E.NOENT;
        const fd = allocFd({ mount: 'dev', kind: 'file', path: name, pos: 0, append: false });
        dv().setUint32(n(8), fd, true); return 0;
      }
      if (dir.mount === 'data') {
        if (oflags & 1 || wantWrite && (oflags & 8)) return E.ROFS;
        const node = dataFile(rel); if (!node) return E.NOENT;
        if (oflags & 2 && !node.dir) return E.NOTDIR;
        const fd = allocFd({ mount: 'data', kind: node.dir ? 'dir' : 'file', path: node.path, pos: 0, append: false, node });
        dv().setUint32(n(8), fd, true); return 0;
      }
      const e = await saveEntry(rel); if (!e) return E.NOENT;
      if (e.kind === 'directory') {
        if (oflags & 4) return E.EXIST;
        const fd = allocFd({ mount: 'save', kind: 'dir', path: rel, pos: 0, append: false, dh: e.h as FileSystemDirectoryHandle });
        dv().setUint32(n(8), fd, true); return 0;
      }
      if (oflags & 2) return e.kind ? E.NOTDIR : E.NOENT;
      if (!e.kind && !(oflags & 1)) return E.NOENT;
      if (e.kind && oflags & 4) return E.EXIST;
      const fh = e.kind ? e.h as FileSystemFileHandle : await e.parent.getFileHandle(e.name, { create: true });
      let sh: FileSystemSyncAccessHandle;
      try { sh = await (fh as any).createSyncAccessHandle({ mode: 'readwrite-unsafe' }); } catch { return E.ACCES; }
      if (oflags & 8) sh.truncate(0);
      const fd = allocFd({ mount: 'save', kind: 'file', path: rel, pos: 0, append: !!(fdflags & 1), sh, fh, wrote: !!(oflags & 8) });
      dv().setUint32(n(8), fd, true); return 0;
    }
    case Op.FdClose: {
      const f = fds.get(n(0)); if (!f) return E.BADF;
      if (f.preopen) return 0;
      if (f.sh) { f.sh.flush(); f.sh.close(); }
      fds.delete(n(0));
      // posted before the call returns, so the page hears of a save before the game can exit after it
      if (f.wrote) (self as unknown as Worker).postMessage({ t: 'saved', path: f.path });
      return 0;
    }
    case Op.FdRead: case Op.FdPread: {
      const f = fds.get(n(0)); if (!f) return E.BADF; if (f.kind === 'dir') return E.ISDIR;
      const dst = iovs(n(1), n(2)), pread = o === Op.FdPread, pos = pread ? n(3) : f.pos, out = pread ? n(4) : n(3);
      let got = 0;
      if (f.mount === 'dev') {
        if (f.path !== 'null') { const mem = u8(); for (const d of dst) { const b = new Uint8Array(d.len); if (f.path === 'urandom') crypto.getRandomValues(b); mem.set(b, d.ptr); got += d.len; } }
      }
      else if (f.mount === 'data') { try { got = await dataRead(f.node!, pos, dst); } catch (e) { note(`[data] a read of ${f.path} failed: ${(e as Error)?.message ?? e}`); return E.IO; } }
      else {
        const mem = u8(); let p = pos;
        for (const d of dst) { const b = new Uint8Array(d.len); const k = f.sh!.read(b, { at: p }); mem.set(b.subarray(0, k), d.ptr); p += k; got += k; if (k < d.len) break; }
      }
      if (!pread) f.pos += got;
      dv().setUint32(out, got, true); return 0;
    }
    case Op.FdWrite: case Op.FdPwrite: {
      const f = fds.get(n(0)); if (!f) return E.BADF; if (f.mount === 'data') return E.ROFS; if (f.kind === 'dir') return E.ISDIR;
      const src = iovs(n(1), n(2)), pw = o === Op.FdPwrite;
      if (f.mount === 'dev') { dv().setUint32(pw ? n(4) : n(3), src.reduce((a, x) => a + x.len, 0), true); return 0; }
      let pos = pw ? n(3) : (f.append ? f.sh!.getSize() : f.pos), put = 0;
      for (const s of src) { const k = f.sh!.write(u8().slice(s.ptr, s.ptr + s.len), { at: pos }); pos += k; put += k; }
      if (put) f.wrote = true;
      if (!pw) f.pos = pos;
      dv().setUint32(pw ? n(4) : n(3), put, true); return 0;
    }
    case Op.FdSeek: {
      const f = fds.get(n(0)); if (!f) return E.BADF; if (f.kind === 'dir') return E.ISDIR;
      const size = f.mount === 'data' ? f.node!.size : f.mount === 'dev' ? 0 : f.sh!.getSize();
      const wh = n(2), base = wh === 0 ? 0 : wh === 1 ? f.pos : size, np = base + Number(BigInt.asIntN(64, a[1]));
      if (np < 0) return E.INVAL;
      f.pos = np; dv().setBigUint64(n(3), BigInt(np), true); return 0;
    }
    case Op.FdTell: { const f = fds.get(n(0)); if (!f) return E.BADF; dv().setBigUint64(n(1), BigInt(f.pos), true); return 0; }
    case Op.FdSync: { const f = fds.get(n(0)); if (!f) return E.BADF; f.sh?.flush(); return 0; }
    case Op.FdFdstatGet: {
      const f = fds.get(n(0)); if (!f) return E.BADF;
      const d = dv(), p = n(1);
      d.setUint8(p, f.kind === 'dir' ? FT_DIR : FT_FILE); d.setUint16(p + 2, f.append ? 1 : 0, true);
      d.setBigUint64(p + 8, 0x1FFFFFFFn, true); d.setBigUint64(p + 16, 0x1FFFFFFFn, true); return 0;
    }
    case Op.FdFdstatSetFlags: { const f = fds.get(n(0)); if (!f) return E.BADF; f.append = !!(n(1) & 1); return 0; }
    case Op.FdFilestatGet: {
      const f = fds.get(n(0)); if (!f) return E.BADF;
      if (f.mount === 'dev') { writeFilestat(n(1), 3, f.kind === 'dir' ? FT_DIR : 2 /* char device */, 0, 0); return 0; }
      if (f.mount === 'data') { const nd = f.node ?? dataRoot; writeFilestat(n(1), nd.ino, nd.dir ? FT_DIR : FT_FILE, nd.size, nd.mtime); return 0; }
      if (f.kind === 'dir') { writeFilestat(n(1), inoOf(f.path), FT_DIR, 0, 0); return 0; }
      writeFilestat(n(1), inoOf(f.path), FT_FILE, f.sh!.getSize(), Date.now() / 1000); return 0;
    }
    case Op.FdFilestatSetSize: { const f = fds.get(n(0)); if (!f) return E.BADF; if (!f.sh) return E.ROFS; f.sh.truncate(n(1)); f.wrote = true; return 0; }
    case Op.FdAllocate: { const f = fds.get(n(0)); if (!f?.sh) return E.BADF; const need = n(1) + n(2); if (f.sh.getSize() < need) f.sh.truncate(need); return 0; }
    case Op.FdFilestatSetTimes: case Op.PathFilestatSetTimes: return 0;
    case Op.PathFilestatGet: {
      const dir = fds.get(n(0)); if (!dir || dir.kind !== 'dir') return E.BADF;
      const rel = normRel(joinRel(dir.path, readPath(n(2), n(3)))); if (rel === null) return E.PERM;
      if (dir.mount === 'data') { const nd = dataFile(rel); if (!nd) return E.NOENT; writeFilestat(n(4), nd.ino, nd.dir ? FT_DIR : FT_FILE, nd.size, nd.mtime); return 0; }
      const st = await statSave(rel); if (!st) return E.NOENT;
      writeFilestat(n(4), inoOf(rel), st.type, st.size, st.mtime); return 0;
    }
    case Op.FdReaddir: {
      const f = fds.get(n(0)); if (!f) return E.BADF; if (f.kind !== 'dir') return E.NOTDIR;
      if (!f.dirents || a[3] === 0n) {
        const list: { name: string; type: number; ino: number }[] = [{ name: '.', type: FT_DIR, ino: 1 }, { name: '..', type: FT_DIR, ino: 1 }];
        if (f.mount === 'data') { const nd = f.node ?? dataLookup(f.path); for (const k of nd?.kids?.values() ?? []) list.push({ name: k.name, type: k.dir ? FT_DIR : FT_FILE, ino: k.ino }); }
        else for await (const [name, h] of (f.dh as any).entries() as AsyncIterable<[string, FileSystemHandle]>)
          list.push({ name, type: h.kind === 'directory' ? FT_DIR : FT_FILE, ino: inoOf(joinRel(f.path, name)) });
        f.dirents = list;
      }
      const buf = n(1), len = n(2); let used = 0;
      const d = dv(), mem = u8();
      for (let i = Number(a[3]); i < f.dirents.length && used < len; i++) {
        const e = f.dirents[i], nm = te.encode(e.name), hdr = new Uint8Array(24), hv = new DataView(hdr.buffer);
        hv.setBigUint64(0, BigInt(i + 1), true); hv.setBigUint64(8, BigInt(e.ino), true); hv.setUint32(16, nm.length, true); hv.setUint8(20, e.type);
        const rec = new Uint8Array(24 + nm.length); rec.set(hdr); rec.set(nm, 24);
        const k = Math.min(rec.length, len - used); mem.set(rec.subarray(0, k), buf + used); used += k;
      }
      d.setUint32(n(4), used, true); return 0;
    }
    case Op.PathCreateDirectory: {
      const dir = fds.get(n(0)); if (!dir || dir.kind !== 'dir') return E.BADF; if (dir.mount === 'data') return E.ROFS;
      const rel = normRel(joinRel(dir.path, readPath(n(1), n(2)))); if (!rel) return E.PERM;
      const e = await saveEntry(rel); if (!e) return E.NOENT; if (e.kind) return E.EXIST;
      await e.parent.getDirectoryHandle(e.name, { create: true }); return 0;
    }
    case Op.PathRemoveDirectory: case Op.PathUnlinkFile: {
      const dir = fds.get(n(0)); if (!dir || dir.kind !== 'dir') return E.BADF; if (dir.mount === 'data') return E.ROFS;
      const rel = normRel(joinRel(dir.path, readPath(n(1), n(2)))); if (!rel) return E.PERM;
      const e = await saveEntry(rel); if (!e?.kind) return E.NOENT;
      if (o === Op.PathUnlinkFile && e.kind === 'directory') return E.ISDIR;
      if (o === Op.PathRemoveDirectory && e.kind === 'file') return E.NOTDIR;
      for (const [k, f] of fds) if (f.mount === 'save' && f.path === rel && f.sh) { f.sh.close(); fds.delete(k); }
      try { await e.parent.removeEntry(e.name); } catch { return E.NOTEMPTY; }
      return 0;
    }
    case Op.PathRename: {
      const da = fds.get(n(0)), db = fds.get(n(3)); if (!da || !db) return E.BADF;
      if (da.mount === 'data' || db.mount === 'data') return E.ROFS;
      const ra = normRel(joinRel(da.path, readPath(n(1), n(2)))), rb = normRel(joinRel(db.path, readPath(n(4), n(5))));
      if (!ra || !rb) return E.PERM;
      const ea = await saveEntry(ra); if (!ea?.kind) return E.NOENT;
      const pb = await saveDirAt(rb.split('/').slice(0, -1), false); if (!pb) return E.NOENT;
      for (const [k, f] of fds) if (f.mount === 'save' && f.path === ra && f.sh) { f.sh.close(); fds.delete(k); }
      const eb = await saveEntry(rb); if (eb?.kind === 'file') await eb.parent.removeEntry(eb.name);
      await (ea.h as any).move(pb, rb.split('/').pop()); return 0;
    }
    case Op.FdRenumber: { const f = fds.get(n(0)); if (!f) return E.BADF; fds.delete(n(0)); fds.set(n(1), f); return 0; }
    case Op.FdAdvise: return 0;
    case Op.MovieOpen: return movieOpen(n(0), n(1));
    case Op.MovieDecode: return await movieDecode(memory, n(0), n(1), n(2), n(3), n(4), n(5), n(6), n(7), note);
    case Op.MovieClose: movieClose(n(0)); return 0;
  }
  return E.NOSYS;
}

// ------------------------------------------------------------- the loop ----
// The doorbell wait has a timeout.  WebKit (Safari, and every browser on an iPhone) sometimes leaves a waitAsync
// without one pending after its notify (it then comes back 'ok', the waiter already off the list) until something
// else runs on this worker: the request stays unanswered, the game thread waiting for it holds the guest lock, and
// the whole game freezes (a player's iPhone in a career race, a CreateFileA 15.6 s and on; Playwright's WebKit 27.2
// on a Mac: about one race in three, up to 50 s).  With a timeout no wait was late in 18 races there (50 and
// 1000 ms); 50 also bounds one that would be to a few frames.
const DOORBELL_WAIT_MS = 50;
async function serve() {
  let seen = Atomics.load(ctl, CTL_IO_DOORBELL);
  for (;;) {
    for (let s = 0; s < MAX_THREADS; s++) {
      const b = s * (RPC_SLOT / 4);
      if (Atomics.compareExchange(rpcI32, b, RPC_POSTED, RPC_TAKEN) !== RPC_POSTED) continue;
      const opc = rpcI32[b + 1] as Op, nargs = rpcI32[b + 3], b64 = s * (RPC_SLOT / 8), args: bigint[] = [];
      for (let i = 0; i < nargs; i++) args.push(rpcI64[b64 + 2 + i]);
      op(opc, args).catch(e => { console.error('io op', opc, e); return E.IO; }).then(r => {
        rpcI32[b + 2] = r; Atomics.store(rpcI32, b, RPC_DONE); Atomics.notify(rpcI32, b);
      });
    }
    const w = (Atomics as any).waitAsync(ctl, CTL_IO_DOORBELL, seen, DOORBELL_WAIT_MS);
    if (w.async) await w.value;
    seen = Atomics.load(ctl, CTL_IO_DOORBELL);
  }
}

onmessage = async (e: MessageEvent<Init>) => {
  ({ memory, cfg } = e.data);
  ctl = new Int32Array(e.data.ctl); rpcI32 = new Int32Array(e.data.rpc); rpcI64 = new BigInt64Array(e.data.rpc);
  try {
    const opfs = await navigator.storage.getDirectory();
    saveRoot = await opfs.getDirectoryHandle(cfg.saveDir, { create: true });
    try { cacheDir = await opfs.getDirectoryHandle(CACHE_DIR, { create: true }); } catch { cacheDir = null; }
    await loadManifest();
    fds.set(3, { mount: 'data', kind: 'dir', path: '', pos: 0, append: false, node: dataRoot, preopen: '/data' });
    fds.set(4, { mount: 'save', kind: 'dir', path: '', pos: 0, append: false, dh: saveRoot, preopen: '/save' });
    fds.set(5, { mount: 'dev', kind: 'dir', path: '', pos: 0, append: false, preopen: '/dev' });
    (self as unknown as Worker).postMessage({ t: 'io-ready', files: inoNext });
    void serve();
  } catch (err) {
    (self as unknown as Worker).postMessage({ t: 'io-error', msg: String(err) });
  }
};
