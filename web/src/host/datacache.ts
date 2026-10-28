// The game data cache in OPFS "cache/", shared by the I/O worker (which fills
// it with the blocks the game reads) and the predownload worker (which fills
// all of it ahead, predownload-worker.ts).  One sparse file per file of the
// install folder, named by its path, size and mtime (a file changed on the
// server is a new name), next to a ".have" map of one byte per 1 MiB block.
// A block's data is written before its byte in the map.

export const BLOCK = 1 << 20;
export const CACHE_DIR = 'cache';
/** where the server has the manifest and the install folder (web/server-rs/src/app/manifest.rs) */
export const MANIFEST_URL = '/data-manifest', DATA_BASE = '/data/';

/** the server's /data-manifest: every file and folder of the install folder it serves, and where it serves runs
 *  of their blocks compressed (an older server has no "blocks": byte ranges of /data/ then) */
export interface Manifest {
  files: { p: string; d?: number; s?: number; m?: number }[];
  blocks?: { url: string; size: number; max: number };
}
/** The manifest's compressed runs, if it has them in this cache's block size: [url, most blocks a request]. */
export function blockRuns(m: Manifest): { url: string; max: number } | null {
  const b = m.blocks;
  return b && typeof b.url === 'string' && b.url.startsWith('/') && b.size === BLOCK && b.max >= 1 ? { url: b.url, max: b.max } : null;
}
/** blocks first .. first+n-1 of a file, compressed: a 200 whose body (decoded by the browser) is exactly their bytes.  The
 *  file's mtime and size (m, s) make a changed file a new address, so the server lets Cloudflare keep the answer for
 *  good. */
export const runUrl = (base: string, path: string, first: number, n: number, f: { mtime: number; size: number }) =>
  `${dataUrl(base, path)}?b=${first}&n=${n}&m=${f.mtime}&s=${f.size}`;
export interface DataFile { path: string; size: number; mtime: number }
export interface CacheFile { h: FileSystemSyncAccessHandle; have: Uint8Array; haveH: FileSystemSyncAccessHandle }

export const manifestFiles = (m: Manifest): DataFile[] => m.files.filter(f => !f.d).map(f => ({ path: f.p, size: f.s ?? 0, mtime: f.m ?? 0 }));
const parts = (f: DataFile) => f.path.toLowerCase().split('/');
/** The files a game in this language reads: the folder's, but where its language pack (LANGPACKS/<language>,
 *  runtime/language.c) has a file of the same path, that one; no other language's pack; of the texture packs
 *  (TEXPACKS/<name>.pak, runtime/web/texrep_web.c) only the one picked (texpack, '' for none).  With voices 'original',
 *  speech banks and dubbed cutscenes from the language pack are ignored so original English audio plays. */
export function gameFiles(files: DataFile[], language: string, texpack: string, voices = 'dubbed'): DataFile[] {
  const skipDub = voices === 'original';
  const isDubFile = (p: string[]) => p[2] === 'speech' || p[2] === 'movies';
  const own = new Set(files.filter(f => {
    const p = parts(f);
    if (p[0] !== 'langpacks' || p[1] !== language) return false;
    if (skipDub && isDubFile(p)) return false;
    return true;
  }).map(f => parts(f).slice(2).join('/')));
  return files.filter(f => {
    const p = parts(f);
    if (p[0] === 'texpacks') return p.length === 2 && p[1] === `${texpack}.pak`;
    if (p[0] === 'langpacks') {
      if (p[1] !== language) return false;
      if (skipDub && isDubFile(p)) return false;
      return true;
    }
    return !own.has(p.join('/'));
  });
}
const isMovie = (f: DataFile) => { const p = parts(f); return (p[0] === 'langpacks' ? p[2] : p[0]) === 'movies'; };
/** The AV1 copy of a movie (tools/movies_av1.py, played where the browser decodes AV1: movie.ts): MOVIES/<name>.vp6 ->
 *  MOVIES/AV1/<name>.vp6, the same in a language pack's MOVIES; null for any other path. */
export function av1Path(path: string): string | null {
  const p = path.split('/'), i = p[0].toLowerCase() === 'langpacks' ? 2 : 0;
  if (p.length !== i + 2 || p[i].toLowerCase() !== 'movies' || !p[i + 1].toLowerCase().endsWith('.vp6')) return null;
  return [...p.slice(0, i + 1), 'AV1', p[i + 1]].join('/');
}
/** What "Preload the game data" fetches: all but the movies (MOVIES/, a third of the folder in VP6 video that
 *  does not compress, and their AV1 copies).  They stream as they play, and with Movies off the game never opens
 *  them. */
export const aheadFiles = (files: DataFile[], language: string, texpack = '', voices = 'dubbed') =>
  gameFiles(files, language, texpack, voices).filter(f => !isMovie(f));
/** The movies a game in this language can open: its VP6 files, or where it plays AV1 (av1) their AV1 copies, the
 *  VP6 file where a movie has none (as the I/O worker opens them, io-worker.ts dataFile). */
export function languageMovies(files: DataFile[], language: string, av1 = false, voices = 'dubbed'): DataFile[] {
  const all = gameFiles(files, language, '', voices).filter(isMovie), byPath = new Map(all.map(f => [f.path.toLowerCase(), f]));
  return all.filter(f => av1Path(f.path) !== null).map(f => (av1 && byPath.get(av1Path(f.path)!.toLowerCase())) || f);
}
export const dataUrl = (base: string, path: string) => base + path.split('/').map(encodeURIComponent).join('/');
export function cacheKey(f: DataFile) { return `${f.path.replace(/[^A-Za-z0-9._-]/g, '_')}.${f.size}.${f.mtime}`; }
export const blockCount = (size: number) => Math.ceil(size / BLOCK);
export const blockLen = (size: number, bi: number) => Math.min(BLOCK, size - bi * BLOCK);

/** A file's sync handles and its map, made empty if it does not fit the file (workers only). */
export async function openCacheFile(dir: FileSystemDirectoryHandle, f: DataFile): Promise<CacheFile> {
  const k = cacheKey(f);
  const fh = await dir.getFileHandle(k, { create: true });
  const mh = await dir.getFileHandle(k + '.have', { create: true });
  const h = await (fh as any).createSyncAccessHandle({ mode: 'readwrite-unsafe' }) as FileSystemSyncAccessHandle;
  let haveH: FileSystemSyncAccessHandle;
  try { haveH = await (mh as any).createSyncAccessHandle({ mode: 'readwrite-unsafe' }) as FileSystemSyncAccessHandle; }
  catch (e) { h.close(); throw e; }
  const nb = blockCount(f.size);
  const have = new Uint8Array(nb);
  if (haveH.getSize() === nb) haveH.read(have, { at: 0 }); else { haveH.truncate(0); haveH.write(have, { at: 0 }); }
  return { h, have, haveH };
}

/** block bi of the file is in its cache file now (throws if it is not: no room for it, say) */
export function putBlock(cf: CacheFile, bi: number, b: Uint8Array) {
  const k = cf.h.write(b, { at: bi * BLOCK });
  if (k !== b.length) throw new Error(`${k} of ${b.length} bytes written`);   // marked as there, a short block would stay
  cf.have[bi] = 1; cf.haveH.write(cf.have.subarray(bi, bi + 1), { at: bi });
}

export interface Scan {
  /** bytes of the manifest's files, and of those that are in the cache */
  total: number; have: number;
  /** what the cache files take already (their sizes: the browser counts a sparse file's whole length) */
  allocated: number;
  /** each file's map (null: nothing of it yet) */
  files: { f: DataFile; have: Uint8Array | null }[];
}

/** What of the manifest's files the cache holds.  Async reads only (any thread), no handle is opened. */
export async function scanCache(files: DataFile[]): Promise<Scan> {
  const s: Scan = { total: 0, have: 0, allocated: 0, files: [] };
  let dir: FileSystemDirectoryHandle | null = null;
  try { dir = await (await navigator.storage.getDirectory()).getDirectoryHandle(CACHE_DIR); } catch { /* no cache yet */ }
  for (const f of files) {
    s.total += f.size;
    let have: Uint8Array | null = null;
    if (dir) try {
      const k = cacheKey(f);
      const m = new Uint8Array(await (await (await dir.getFileHandle(k + '.have')).getFile()).arrayBuffer());
      s.allocated += Math.min(f.size, (await (await dir.getFileHandle(k)).getFile()).size);
      if (m.length === blockCount(f.size)) have = m;
    } catch { /* not cached */ }
    if (have) for (let bi = 0; bi < have.length; bi++) if (have[bi]) s.have += blockLen(f.size, bi);
    s.files.push({ f, have });
  }
  return s;
}
