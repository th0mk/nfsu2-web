// The save overlay as the page sees it: the OPFS directory the I/O worker
// mounts as /save (web/src/host/io-worker.ts).  Main-thread, async API only;
// nothing here ever touches /data (the game folder is served read-only by
// the local server and never written).
//
// Writes from the page happen only while no game runs in this tab (the
// shell enforces it), so they never race the I/O worker's sync handles.
// But an F12 screenshot (shots.ts saveScreenshot): a new file of its own,
// which the game never opens.

import { BLOCK, CACHE_DIR } from '../host/datacache';
import { coverOf } from './profile';
import { crc32 } from './zip';
import { N } from './i18n';

export const PROFILES_DIR = 'NFS Underground 2';   // what the game creates under the save root
/** an F12 screenshot: an image file at the top of the save folder (nfsu2000.tga ...) */
export const isScreenshot = (path: string) => !path.includes('/') && /\.(tga|bmp|png|jpe?g)$/i.test(path);

async function root(): Promise<FileSystemDirectoryHandle> { return navigator.storage.getDirectory(); }

export async function saveRoot(saveDir: string, create = true): Promise<FileSystemDirectoryHandle> {
  return (await root()).getDirectoryHandle(saveDir, { create });
}

function parts(path: string): string[] {
  const p = path.split('/').filter(x => x && x !== '.');
  if (p.some(x => x === '..')) throw new Error(`bad path ${path}`);
  return p;
}

async function dirOf(base: FileSystemDirectoryHandle, path: string[], create: boolean) {
  let d = base;
  for (const p of path) d = await d.getDirectoryHandle(p, { create });
  return d;
}

/** The text of a file in the save dir, or null if it does not exist. */
export async function readText(saveDir: string, path: string): Promise<string | null> {
  try {
    const p = parts(path);
    const d = await dirOf(await saveRoot(saveDir), p.slice(0, -1), false);
    return await (await (await d.getFileHandle(p[p.length - 1])).getFile()).text();
  } catch { return null; }
}

export async function exists(saveDir: string, path: string): Promise<boolean> {
  try {
    const p = parts(path);
    const d = await dirOf(await saveRoot(saveDir), p.slice(0, -1), false);
    await d.getFileHandle(p[p.length - 1]);
    return true;
  } catch { return false; }
}

/** Write a whole file (creating its folders).  A file it made, whose write then failed (the browser's storage full),
 *  goes again: an empty profile left behind would be sent to the cloud as one. */
export async function writeFile(saveDir: string, path: string, data: string | Uint8Array | Blob): Promise<void> {
  const p = parts(path);
  if (!p.length) throw new Error('empty path');
  const d = await dirOf(await saveRoot(saveDir), p.slice(0, -1), true), name = p[p.length - 1];
  let had = true;
  try { await d.getFileHandle(name); } catch { had = false; }
  await writeInto(d, name, data, had);
}
async function writeInto(d: FileSystemDirectoryHandle, name: string, data: string | Uint8Array | Blob, had: boolean) {
  try { const w = await (await d.getFileHandle(name, { create: true })).createWritable(); await w.write(data as FileSystemWriteChunkType); await w.close(); }
  catch (e) { if (!had) await d.removeEntry(name).catch(() => {}); throw e; }
}

/** Write a file only where none is, or an empty one (the game's own of that path, written meanwhile, stays): false when
 *  one was there.  Looked for again once made, just before the write. */
export async function writeIfAbsent(saveDir: string, path: string, data: string | Uint8Array): Promise<boolean> {
  const p = parts(path);
  if (!p.length) throw new Error('empty path');
  const d = await dirOf(await saveRoot(saveDir), p.slice(0, -1), true), name = p[p.length - 1];
  const sized = async () => (await (await d.getFileHandle(name)).getFile()).size;
  let had = true;
  try { if (await sized() > 0) return false; } catch { had = false; }
  await d.getFileHandle(name, { create: true });
  if (await sized() > 0) return false;
  await writeInto(d, name, data, had);
  return true;
}

/** the folder beside a save folder where writeTogether stages its files (no listing, backup or cloud save sees it) */
export const stagingDir = (saveDir: string) => `${saveDir}~incoming`;
/** in the staging folder: the files of a writeTogether whose moves began (PlanFile; n<i>: file i staged, o<i>: the file
 *  it replaces, moved aside); gone once all of them are in place */
const PLAN = 'plan.json';
/** a file of a writeTogether's plan: its path in the save folder (dir: in that folder beside it instead), and its
 *  CRC-32 and size (what tells the incoming copy from a file written there since) */
interface PlanFile { path: string; crc: number; size: number; dir?: string }
/** a file a writeTogether writes into another folder beside the save folder (the copy Put back brings back) */
export interface BesideFile { dir: string; path: string; data: string | Uint8Array }
/** the folder a plan's file goes into */
async function baseOf(saveDir: string, f: { dir?: string }): Promise<FileSystemDirectoryHandle> {
  return f.dir ? (await root()).getDirectoryHandle(f.dir, { create: true }) : saveRoot(saveDir);
}
/** a staged file's name before f4e3f301's staging folder (beside the file it replaced): never listed */
const isStaged = (name: string) => name.endsWith('.incoming');

type MovableFile = FileSystemFileHandle & { move?(dir: FileSystemDirectoryHandle, name: string): Promise<void> };
/** a file moved into another folder of the OPFS, under a name, over one there (refused for a file a worker holds open) */
async function moveFile(from: FileSystemDirectoryHandle, name: string, to: FileSystemDirectoryHandle, as: string) {
  const h = await from.getFileHandle(name) as MovableFile;
  if (typeof h.move === 'function') { await h.move(to, as); return; }
  // (a browser without move(): copied, then removed)
  let had = true;
  try { await to.getFileHandle(as); } catch { had = false; }
  await writeInto(to, as, new Uint8Array(await (await h.getFile()).arrayBuffer()), had);
  await from.removeEntry(name);
}
const there = (d: FileSystemDirectoryHandle, name: string) => d.getFileHandle(name).then(() => true, () => false);
/** A file of the save dir renamed (moved over the one of the new name: no room needed). */
export async function renameFile(saveDir: string, from: string, to: string): Promise<void> {
  const base = await saveRoot(saveDir);
  await moveFile(base, from, base, to);
}
/** the file there is the plan's incoming copy (its size and CRC): not one written there since */
async function incoming(d: FileSystemDirectoryHandle, name: string, f: PlanFile): Promise<boolean> {
  try {
    const b = new Uint8Array(await (await (await d.getFileHandle(name)).getFile()).arrayBuffer());
    return b.length === f.size && crc32(b) === f.crc;
  } catch { return false; }
}

/**
 * A writeTogether whose moves did not all end, undone, the last first.  Each step says by itself what is left of it,
 * so an undo cut short and run again (the tab closed, a move refused) does only what is left: the incoming copy still
 * at its path (n<i> not staged, that file the plan's) goes back into the staging folder, and the file it replaced comes
 * back where nothing or only the incoming copy is.  Nothing is removed: a file written there since (a game's save)
 * stays.  False: some step failed (the plan and what is staged stay, for the next try).
 */
async function rollBack(saveDir: string, stage: FileSystemDirectoryHandle): Promise<boolean> {
  let plan: PlanFile[];
  try { plan = JSON.parse(await (await (await stage.getFileHandle(PLAN)).getFile()).text()) as PlanFile[]; }
  catch { return !await there(stage, PLAN); }          // no plan: no move began (one that cannot be read: kept)
  // (a plan of paths only, cae4d8df's: no file there is known as its incoming copy, so none moves)
  plan = plan.map(f => typeof f === 'string' ? { path: f, crc: -1, size: -1 } : f);
  let ok = true;
  for (let i = plan.length - 1; i >= 0; i--) {
    try {
      const f = plan[i], p = parts(f.path), name = p[p.length - 1], d = await dirOf(await baseOf(saveDir, f), p.slice(0, -1), true);
      if (!await there(stage, `n${i}`) && await incoming(d, name, f)) await moveFile(d, name, stage, `n${i}`);
      if (await there(stage, `o${i}`) && (!await there(d, name) || await incoming(d, name, f))) await moveFile(stage, `o${i}`, d, name);
    } catch { ok = false; }
  }
  if (ok) await stage.removeEntry(PLAN).catch(() => { ok = false; });
  await pruneSaveDir(saveDir, () => true).catch(() => {});   // the folders it made, empty again
  return ok;
}

/**
 * The staging folder at the page's start (by the tab that owns the saves), and before each writeTogether: a
 * writeTogether a closed tab left halfway through its moves is undone, then what it staged goes.  False: it could not
 * all be undone (the plan stays: tried again next time).
 */
export async function settleStaged(saveDir: string): Promise<boolean> {
  let stage: FileSystemDirectoryHandle;
  try { stage = await (await root()).getDirectoryHandle(stagingDir(saveDir)); } catch { return true; }
  if (!await rollBack(saveDir, stage)) return false;
  await (await root()).removeEntry(stagingDir(saveDir), { recursive: true }).catch(() => {});
  return true;
}

/** A restore refused while one that stopped halfway is not undone yet (settleStaged false): nothing was written. */
export class UndoPending extends Error {
  readonly why = N('a restore that stopped earlier is not undone yet, so nothing was written');
  readonly code = 'SAVES-UNDO-PENDING';
  constructor() { super('a restore that stopped earlier is not undone yet, so nothing was written'); }
}

/**
 * Write files all or none (a restore's, a cloud take's: a profile and its GARAGE.DAT of two versions lost the cars of
 * slots 6-10).  Each is written into the staging folder beside the save folder first (stagingDir); then, the files
 * noted there (PLAN), each file it replaces is moved aside and the new one moved into place.  A write that fails (the
 * browser's storage full) changes nothing here; a move refused (a file another tab's game holds open) undoes the
 * moves before it.  Either way it throws, the saves as they were.  The moves need no room, and a tab closed during
 * them is undone at the next page's start (settleStaged).  While an earlier one is not undone it writes nothing.
 */
export async function writeTogether(saveDir: string, files: { path: string; data: string | Uint8Array }[], beside: BesideFile[] = []): Promise<void> {
  if (!await settleStaged(saveDir)) throw new UndoPending();
  const stage = await (await root()).getDirectoryHandle(stagingDir(saveDir), { create: true });
  const all: { path: string; data: string | Uint8Array; dir?: string }[] = [...files, ...beside];
  const clear = () => root().then(r => r.removeEntry(stagingDir(saveDir), { recursive: true })).catch(() => {});
  const enc = new TextEncoder(), plan: PlanFile[] = [];
  try {
    for (const [i, f] of all.entries()) {
      if (!parts(f.path).length) throw new Error('empty path');
      const b = typeof f.data === 'string' ? enc.encode(f.data) : f.data;
      await writeInto(stage, `n${i}`, b, false);
      plan.push({ path: f.path, crc: crc32(b), size: b.length, ...f.dir ? { dir: f.dir } : {} });
    }
    await writeInto(stage, PLAN, JSON.stringify(plan), false);
  } catch (e) {
    await clear();
    throw e;
  }
  try {
    for (const [i, f] of all.entries()) {
      const p = parts(f.path), name = p[p.length - 1], d = await dirOf(await baseOf(saveDir, f), p.slice(0, -1), true);
      if (await there(d, name)) await moveFile(d, name, stage, `o${i}`);
      await moveFile(stage, `n${i}`, d, name);
    }
  } catch (e) {
    if (await rollBack(saveDir, stage)) await clear();
    throw e;
  }
  await stage.removeEntry(PLAN);                       // all in place: nothing to undo from here on
  await clear();
}

export interface Entry { path: string; size: number; mtime: number }

/** Every file under the save dir, depth first, sorted by path. */
export async function listFiles(saveDir: string): Promise<Entry[]> {
  const out: Entry[] = [];
  async function walk(d: FileSystemDirectoryHandle, rel: string) {
    for await (const [name, h] of (d as any).entries() as AsyncIterable<[string, FileSystemHandle]>) {
      const r = rel ? rel + '/' + name : name;
      if (h.kind === 'directory') await walk(h as FileSystemDirectoryHandle, r);
      else if (!isStaged(name)) { const f = await (h as FileSystemFileHandle).getFile(); out.push({ path: r, size: f.size, mtime: f.lastModified }); }
    }
  }
  try { await walk(await saveRoot(saveDir, false), ''); } catch { /* no save dir yet */ }
  return out.sort((a, b) => a.path < b.path ? -1 : a.path > b.path ? 1 : 0);
}

export async function readBytes(saveDir: string, path: string): Promise<Uint8Array> {
  const p = parts(path);
  const d = await dirOf(await saveRoot(saveDir), p.slice(0, -1), false);
  return new Uint8Array(await (await (await d.getFileHandle(p[p.length - 1])).getFile()).arrayBuffer());
}

/** Remove a file, or a folder with everything in it; nothing if it is not there. */
export async function removePath(saveDir: string, path: string): Promise<void> {
  const p = parts(path);
  if (!p.length) throw new Error('empty path');
  let d: FileSystemDirectoryHandle;
  try { d = await dirOf(await saveRoot(saveDir, false), p.slice(0, -1), false); } catch { return; }
  try { await d.removeEntry(p[p.length - 1], { recursive: true }); }
  catch (e) { if ((e as DOMException).name !== 'NotFoundError') throw e; }
}

/** The save dir's file functions, bound to it (profile.ts turnOnAutosave, renameProfile). */
export function filesOf(saveDir: string) {
  return {
    listFiles: () => listFiles(saveDir), readBytes: (path: string) => readBytes(saveDir, path),
    readText: (path: string) => readText(saveDir, path), writeFile: (path: string, data: string | Uint8Array) => writeFile(saveDir, path, data),
    remove: (path: string) => removePath(saveDir, path), exists: (path: string) => exists(saveDir, path),
    writeIfAbsent: (path: string, data: Uint8Array) => writeIfAbsent(saveDir, path, data),
  };
}

/** Remove everything inside the save dir, or its folder `under` (the folder itself stays), but what is at its top under
 *  a name `keep` takes. */
export async function clearSaveDir(saveDir: string, keep: (name: string) => boolean = () => false, under = ''): Promise<void> {
  let d = await saveRoot(saveDir);
  if (under) try { d = await dirOf(d, parts(under), false); } catch { return; }   // not there: nothing in it
  const names: string[] = [];
  for await (const [name] of (d as any).entries() as AsyncIterable<[string, FileSystemHandle]>) if (!keep(name)) names.push(name);
  for (const n of names) await d.removeEntry(n, { recursive: true });
}

/** Remove every file of the save dir keep() does not take (its path), and the folders that leaves empty (the game
 *  would list an empty profile folder): what a restore replaced, after it wrote its own. */
export async function pruneSaveDir(saveDir: string, keep: (path: string) => boolean): Promise<void> {
  async function walk(d: FileSystemDirectoryHandle, rel: string): Promise<boolean> {     // true: d is empty now
    const kids: [string, FileSystemHandle][] = [];
    for await (const e of (d as any).entries() as AsyncIterable<[string, FileSystemHandle]>) kids.push(e);
    let empty = true;
    for (const [name, h] of kids) {
      const r = rel ? `${rel}/${name}` : name;
      if (h.kind === 'directory') { if (await walk(h as FileSystemDirectoryHandle, r)) await d.removeEntry(name, { recursive: true }); else empty = false; }
      else if (keep(r)) empty = false;
      else await d.removeEntry(name);
    }
    return empty;
  }
  let d: FileSystemDirectoryHandle;
  try { d = await saveRoot(saveDir, false); } catch { return; }
  await walk(d, '');
}

/** A small file beside the save dirs (in none: no backup, cloud save or restore touches it), or null. */
export async function readBeside(name: string): Promise<string | null> {
  try { return await (await (await (await root()).getFileHandle(name)).getFile()).text(); } catch { return null; }
}
export async function writeBeside(name: string, text: string): Promise<void> {
  const w = await (await (await root()).getFileHandle(name, { create: true })).createWritable();
  await w.write(text); await w.close();
}

/** Remove the whole save dir (test runs: &fresh). */
export async function removeSaveDir(saveDir: string): Promise<void> {
  try { await (await root()).removeEntry(saveDir, { recursive: true }); } catch { /* absent */ }
}

export interface Profile { name: string; size: number; mtime: number; files: number }

/**
 * Profiles the game saved: "NFS Underground 2/<NAME>/<NAME>" (54,966 bytes
 * each).  A folder without its file is skipped:
 * the game would not list it either.  So are the cover shoots' saves beside
 * them ("<NAME> DVD 1", profile.ts coverOf): they belong to their profile.
 */
export async function listProfiles(saveDir: string, files?: Entry[]): Promise<Profile[]> {
  files ??= await listFiles(saveDir);
  const by = new Map<string, Profile>();
  for (const f of files) {
    const p = f.path.split('/');
    if (p.length < 3 || p[0] !== PROFILES_DIR || coverOf(p[1])) continue;
    const name = p[1];
    const pr = by.get(name) ?? { name, size: 0, mtime: 0, files: 0 };
    pr.files++;
    if (p.length === 3 && p[2].toLowerCase() === name.toLowerCase()) { pr.size = f.size; pr.mtime = f.mtime; }
    by.set(name, pr);
  }
  return [...by.values()].filter(p => p.size > 0).sort((a, b) => b.mtime - a.mtime);
}

/**
 * Game data the browser keeps locally (OPFS "cache/", host/datacache.ts), of
 * any version of the server's files: every file there is sparse, with a
 * ".have" map of one byte per 1 MiB block, so the cached amount is the number
 * of blocks present (the last block of a file may be shorter; the file size
 * bounds it).
 */
export async function cacheBytes(): Promise<{ bytes: number; files: number }> {
  let bytes = 0, files = 0;
  try {
    const d = await (await root()).getDirectoryHandle(CACHE_DIR);
    for await (const [name, h] of (d as any).entries() as AsyncIterable<[string, FileSystemHandle]>) {
      if (h.kind !== 'file' || !name.endsWith('.have')) continue;
      const have = new Uint8Array(await (await (h as FileSystemFileHandle).getFile()).arrayBuffer());
      let n = 0; for (const b of have) n += b ? 1 : 0;
      if (!n) continue;
      let size = n * BLOCK;
      try { size = Math.min(size, (await (await d.getFileHandle(name.slice(0, -5))).getFile()).size); } catch { /* data file gone */ }
      bytes += size; files++;
    }
  } catch { /* no cache yet */ }
  return { bytes, files };
}

export async function clearCache(): Promise<void> {
  try { await (await root()).removeEntry(CACHE_DIR, { recursive: true }); } catch { /* absent */ }
}
