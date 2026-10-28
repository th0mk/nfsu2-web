// The predownload worker: the install folder but the movies (datacache.ts
// aheadFiles) into the game data cache ahead of play, so a game never waits
// on the network for its data.  Blocks come compressed from a server that
// offers it (one a request, at the game's own addresses, which Cloudflare can
// keep), else as byte ranges.  The page starts it ("Download all game data")
// and stops it before a game starts, since the I/O worker writes the same files.
//
// Page -> worker: { t: 'start', manifestUrl, dataBase, language, texpack }, { t: 'stop' }
// Worker -> page: { t: 'progress', have, total }   (bytes in the cache; at most every 250 ms)
//                 { t: 'done', have, total }       (all of it is in the cache)
//                 { t: 'stopped' }                 (after 'stop')
//                 { t: 'error', msg }
// Every file is closed before 'done', 'stopped' or 'error'.  What was written
// stays: a stopped or failed download goes on from there the next time.
import {
  BLOCK, CACHE_DIR, aheadFiles, blockCount, blockLen, blockRuns, dataUrl, manifestFiles, openCacheFile, putBlock, runUrl, scanCache,
  type CacheFile, type DataFile, type Manifest,
} from './datacache';
import { fmtBytes } from '../app/checks';

export type ToPredownload = { t: 'start'; manifestUrl: string; dataBase: string; language: string; texpack: string; voices?: string } | { t: 'stop' };
export type FromPredownload =
  | { t: 'progress' | 'done'; have: number; total: number }
  | { t: 'stopped' } | { t: 'error'; msg: string };

// Requests at once (over HTTP/1.1 Chrome holds 6 connections to a server: the page keeps the rest).  From a server
// with compressed blocks, one block a request, at the address the game's own reads use (datacache.ts runUrl), so
// one copy in Cloudflare's cache serves both; byte ranges from an older server take RUN blocks a request (a few
// seconds on a slow line, so a stop is quick and little is fetched twice).
const LANES = 4, RUN = 8;
const ATTEMPTS = 8;

const post = (m: FromPredownload) => (self as unknown as Worker).postMessage(m);
const abort = new AbortController();
let stopping = false;

/** a request's blocks, [first, end) of a file: all missing when it was queued */
interface Run { f: DataFile; first: number; end: number; file: Shared }
/** a file's cache file, open while runs of it are left */
interface Shared { cf: Promise<CacheFile> | null; runs: number }

class Retry extends Error {}

function closeFile(cf: CacheFile | null) { if (cf) { cf.h.flush(); cf.h.close(); cf.haveH.flush(); cf.haveH.close(); } }

async function download(cfg: { manifestUrl: string; dataBase: string; language: string; texpack: string; voices?: string }): Promise<{ have: number; total: number } | null> {
  const r = await fetch(cfg.manifestUrl, { cache: 'no-cache', signal: abort.signal });
  if (!r.ok) throw new Error(`the list of game files did not load (HTTP ${r.status})`);
  const m = await r.json() as Manifest;
  const served = blockRuns(m), runMax = served ? 1 : RUN;
  const scan = await scanCache(aheadFiles(manifestFiles(m), cfg.language, cfg.texpack, cfg.voices));
  const total = scan.total;
  let have = scan.have, shown = 0;
  const progress = (now = false) => {
    const t = performance.now();
    if (now || t - shown >= 250) { shown = t; post({ t: 'progress', have, total }); }
  };
  progress(true);
  if (have >= total) return { have, total };
  // room for the rest: what the cache files do not take yet
  const need = total - scan.allocated;
  const room = await navigator.storage.estimate()
    .then(e => e.quota !== undefined && e.usage !== undefined ? e.quota - e.usage : Infinity, () => Infinity);
  if (room < need) throw new Error(`there is not enough room: the rest needs ${fmtBytes(need)}, and the browser lets this page store ${fmtBytes(Math.max(0, room))} more. Free some disk space and try again`);

  const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle(CACHE_DIR, { create: true });
  const runs: Run[] = [];
  for (const { f, have: m } of scan.files) {
    const file: Shared = { cf: null, runs: 0 };
    for (let bi = 0; bi < blockCount(f.size); bi++) {
      if (m?.[bi]) continue;
      const prev = runs[runs.length - 1];
      if (prev?.f === f && prev.end === bi && prev.end - prev.first < runMax) prev.end++;
      else { runs.push({ f, first: bi, end: bi + 1, file }); file.runs++; }
    }
  }
  const open = new Set<Promise<CacheFile>>();
  async function release(s: Shared) {
    if (--s.runs || !s.cf) return;
    const p = s.cf; s.cf = null; open.delete(p);
    closeFile(await p.catch(() => null));
  }

  // a run's blocks as they arrive, each written and marked once it is whole
  async function fetchRun(run: Run, cf: CacheFile) {
    let first = run.first;
    while (first < run.end && cf.have[first]) first++;
    if (first === run.end) return;
    const start = first * BLOCK, end = Math.min(run.f.size, run.end * BLOCK);
    // compressed runs (the browser undoes the Content-Encoding), or a byte range
    const res = served ? await fetch(runUrl(served.url, run.f.path, first, run.end - first, run.f), { cache: 'no-store', signal: abort.signal })
      : await fetch(dataUrl(cfg.dataBase, run.f.path), { headers: { Range: `bytes=${start}-${end - 1}` }, cache: 'no-store', signal: abort.signal });
    if (res.status === 429 || res.status >= 500) { await res.body?.cancel(); throw new Retry(`HTTP ${res.status}`); }
    if (res.status !== 206 && res.status !== 200) throw new Error(`the server answered HTTP ${res.status} for ${run.f.path}`);
    let at = served || res.status === 206 ? start : 0;   // a 200 to a range is the whole file, from its first block
    const buf = new Uint8Array(BLOCK);
    let fill = 0;
    const rd = res.body!.getReader();
    for (;;) {
      const { done, value } = await rd.read();
      if (done) break;
      for (let o = 0; o < value.length; ) {
        const bi = at / BLOCK, len = blockLen(run.f.size, bi), k = Math.min(len - fill, value.length - o);
        buf.set(value.subarray(o, o + k), fill); fill += k; o += k;
        if (fill < len) continue;
        if (!cf.have[bi]) { putBlock(cf, bi, buf.subarray(0, len)); have += len; progress(); }
        at += len; fill = 0;
        if (at >= end) { await rd.cancel(); return; }
      }
    }
    throw new Retry(`${run.f.path} ended early`);
  }

  let next = 0, failed: unknown = null;
  async function lane() {
    while (!abort.signal.aborted && next < runs.length) {
      const run = runs[next++];
      try {
        if (!run.file.cf) { run.file.cf = openCacheFile(dir, run.f); open.add(run.file.cf); }
        const cf = await run.file.cf;
        for (let attempt = 1, wait = 500; ; attempt++, wait = Math.min(wait * 2, 8000)) {
          try { await fetchRun(run, cf); break; }
          catch (e) {
            // a busy server, a dropped connection, a body cut short: again, from its first block still missing
            if (abort.signal.aborted || attempt >= ATTEMPTS || !(e instanceof Retry || e instanceof TypeError)) throw e;
            await new Promise(res => setTimeout(res, wait));
          }
        }
      } catch (e) { if (!abort.signal.aborted) { failed = e; abort.abort(); } }
      finally { await release(run.file); }
    }
  }
  await Promise.all(Array.from({ length: LANES }, lane));
  // files whose later runs were never taken (a stop, a failure)
  for (const p of open) closeFile(await p.catch(() => null));
  if (failed) throw failed;
  progress(true);
  return stopping ? null : { have, total };
}

function why(e: unknown): string {
  if (e instanceof DOMException && e.name === 'QuotaExceededError') return 'the browser has no more room for it. Free some disk space and try again';
  if (e instanceof TypeError) return 'the server stopped answering. Try again';
  if (e instanceof Retry) return `the server stopped answering (${e.message}). Try again`;
  return e instanceof Error ? e.message : String(e);
}

let started = false;
onmessage = (e: MessageEvent<ToPredownload>) => {
  if (e.data.t === 'stop') { stopping = true; abort.abort(); return; }
  if (started) return;
  started = true;
  download(e.data).then(
    r => post(r ? { t: 'done', ...r } : { t: 'stopped' }),
    err => post(stopping ? { t: 'stopped' } : { t: 'error', msg: why(err) }),
  );
};
