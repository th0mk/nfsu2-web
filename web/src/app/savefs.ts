// The save overlay as the page sees it: the OPFS directory the I/O worker
// mounts as /save (web/src/host/io-worker.ts).  Main-thread, async API only;
// nothing here ever touches /data (the game folder is served read-only by
// the local server and never written).
//
// Writes from the page happen only while no game runs in this tab (the
// shell enforces it), so they never race the I/O worker's sync handles.
// But an F12 screenshot (shots.ts saveScreenshot): a new file of its own,
// which the game never opens.  While a game plays without saving (shell.ts,
// setWritesOff) nothing here writes or removes anything at all.

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

/**
 * Whether an error says the file or folder is not there (NotFoundError, in every browser).  Any other error is a
 * storage that did not answer, never "not there": Firefox 159 on a Mac answered every call in the save folder with
 * "UnknownError: Unknown failure" (Discord 2026-11-04), and the page took its settings.ini for missing and wrote the
 * template over it (the write failed too, that time).  So reads say null or false only for this, and throw otherwise.
 */
export const missing = (e: unknown) => (e as { name?: string } | null)?.name === 'NotFoundError';
/** what the page says where the save folder does not answer (Play, Saves, Settings, an invite's Join) */
export const UNREADABLE = N('This browser can\'t read or write your saves right now ({error}).');
/** what the page says while a game plays without saving (shell.ts askNoSave: as it starts; a screenshot, a setting) */
export const NO_SAVE = N('Playing without saving: nothing from this game is kept.');

// Only a look-up (getFileHandle, getDirectoryHandle) that answers NotFoundError says "not there".  A file or folder that
// was found and then does not read with NotFoundError is looked up again: gone meanwhile, it is not there; still there,
// the error is thrown.  (Firefox 159 answered "NotFoundError: Entry not found" for files its storage still listed:
// taken for missing, settings.ini would get the template written over it, and a listing would leave a profile out.)
/** whether name is (still) in d; throws what its look-up answered but NotFoundError */
async function found(d: FileSystemDirectoryHandle, name: string, kind: 'file' | 'directory'): Promise<boolean> {
  try { await (kind === 'file' ? d.getFileHandle(name) : d.getDirectoryHandle(name)); return true; }
  catch (e) { if (missing(e)) return false; throw e; }
}
/** what read makes of a file of d, or null where the file is not there (missing, found) */
async function readIn<T>(d: FileSystemDirectoryHandle, name: string, read: (f: File) => Promise<T>): Promise<T | null> {
  let h: FileSystemFileHandle;
  try { h = await d.getFileHandle(name); } catch (e) { if (missing(e)) return null; throw e; }
  try { return await read(await h.getFile()); }
  catch (e) { if (missing(e) && !await found(d, name, 'file')) return null; throw e; }
}
/** the folder at path under base, or null where one of them is not there */
async function dirIn(base: FileSystemDirectoryHandle, path: string[]): Promise<FileSystemDirectoryHandle | null> {
  try { return await dirOf(base, path, false); } catch (e) { if (missing(e)) return null; throw e; }
}

/**
 * Playing without saving (shell.ts: the save folder did not answer, and the player started the game anyway): nothing in
 * this file writes, moves or removes anything in the browser's storage until it is off again, whoever asks (Autosave, a
 * restore, cloud saves, Put back, a screenshot, a setting): each throws SavesOff instead.
 */
let writesOff = false;
export function setWritesOff(on: boolean) { writesOff = on; }
export const writesAreOff = () => writesOff;
export class SavesOff extends Error {
  constructor() { super('playing without saving: nothing is written'); this.name = 'SavesOff'; }
}
function writable() { if (writesOff) throw new SavesOff(); }

/** The text of a file in the save dir, or null if it (or the save dir) is not there.  Throws when it may be there but
 *  did not read (missing, found). */
export async function readText(saveDir: string, path: string): Promise<string | null> {
  const p = parts(path);
  const top = await dirIn(await root(), [saveDir]), d = top && await dirIn(top, p.slice(0, -1));
  return d ? readIn(d, p[p.length - 1], f => f.text()) : null;
}

/** Whether a file is at path (a folder there is none); throws when the storage did not say (missing). */
export async function exists(saveDir: string, path: string): Promise<boolean> {
  const p = parts(path);
  try {
    const d = await dirOf(await saveRoot(saveDir, false), p.slice(0, -1), false);
    await d.getFileHandle(p[p.length - 1]);
    return true;
  } catch (e) { if (missing(e) || (e as DOMException)?.name === 'TypeMismatchError') return false; throw e; }
}

/** Write a whole file (creating its folders).  A file it made, whose write then failed (the browser's storage full),
 *  goes again: an empty profile left behind would be sent to the cloud as one.  Only a file it knows it made: one the
 *  storage would not say about (missing) may be the player's, and stays. */
export async function writeFile(saveDir: string, path: string, data: string | Uint8Array | Blob): Promise<void> {
  writable();
  const p = parts(path);
  if (!p.length) throw new Error('empty path');
  const d = await dirOf(await saveRoot(saveDir), p.slice(0, -1), true), name = p[p.length - 1];
  let had = true;
  try { await d.getFileHandle(name); } catch (e) { if (missing(e)) had = false; }
  await writeInto(d, name, data, had);
}
async function writeInto(d: FileSystemDirectoryHandle, name: string, data: string | Uint8Array | Blob, had: boolean) {
  try { const w = await (await d.getFileHandle(name, { create: true })).createWritable(); await w.write(data as FileSystemWriteChunkType); await w.close(); }
  catch (e) { if (!had) await d.removeEntry(name).catch(() => {}); throw e; }
}

/** Write a file only where none is, or an empty one (the game's own of that path, written meanwhile, stays): false when
 *  one was there.  Looked for again once made, just before the write. */
export async function writeIfAbsent(saveDir: string, path: string, data: string | Uint8Array): Promise<boolean> {
  writable();
  const p = parts(path);
  if (!p.length) throw new Error('empty path');
  const d = await dirOf(await saveRoot(saveDir), p.slice(0, -1), true), name = p[p.length - 1];
  const sized = () => readIn(d, name, async f => f.size);
  const was = await sized();
  if (was) return false;
  const had = was !== null;
  await d.getFileHandle(name, { create: true });
  // (a size that does not read now leaves no empty file this made)
  try { if (await sized()) return false; }
  catch (e) { if (!had) await d.removeEntry(name).catch(() => {}); throw e; }
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
  // (a browser without move(): copied, then removed; a file there that its look-up did not answer for stays)
  let had = true;
  try { await to.getFileHandle(as); } catch (e) { if (missing(e)) had = false; }
  await writeInto(to, as, new Uint8Array(await (await h.getFile()).arrayBuffer()), had);
  await from.removeEntry(name);
}
/** whether a file is there; throws what its look-up answered but NotFoundError (taken for none, a restore without
 *  move() wrote over a profile it had not moved aside, and the start's settle removed one it had) */
const there = (d: FileSystemDirectoryHandle, name: string) => found(d, name, 'file');
/** A file of the save dir renamed (moved over the one of the new name: no room needed). */
export async function renameFile(saveDir: string, from: string, to: string): Promise<void> {
  writable();
  const base = await saveRoot(saveDir);
  await moveFile(base, from, base, to);
}
/** the file there is the plan's incoming copy (its size and CRC): not one written there since */
async function incoming(d: FileSystemDirectoryHandle, name: string, f: PlanFile): Promise<boolean> {
  const b = await readIn(d, name, async x => new Uint8Array(await x.arrayBuffer()));    // (one that does not read: throws)
  return b !== null && b.length === f.size && crc32(b) === f.crc;
}

/**
 * A writeTogether whose moves did not all end, undone, the last first.  Each step says by itself what is left of it,
 * so an undo cut short and run again (the tab closed, a move refused) does only what is left: the incoming copy still
 * at its path (n<i> not staged, that file the plan's) goes back into the staging folder, and the file it replaced comes
 * back where nothing or only the incoming copy is.  Nothing is removed: a file written there since (a game's save)
 * stays.  False: some step failed (the plan and what is staged stay, for the next try).
 */
async function rollBack(saveDir: string, stage: FileSystemDirectoryHandle): Promise<boolean> {
  let text: string | null, plan: PlanFile[];
  try { text = await readIn(stage, PLAN, f => f.text()); } catch { return false; }    // (one that does not read: kept)
  if (text === null) return true;                      // no plan: no move began
  try { plan = JSON.parse(text) as PlanFile[]; } catch { return false; }             // (one that cannot be read: kept)
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
  writable();
  let stage: FileSystemDirectoryHandle;
  try { stage = await (await root()).getDirectoryHandle(stagingDir(saveDir)); } catch (e) { if (missing(e)) return true; throw e; }
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
  writable();
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

/**
 * Every file under the save dir, depth first, sorted by path ([] with no save dir yet).  A file or folder gone while it
 * lists is left out; any other error throws (missing): a list with files left out would be a backup, a cloud version or
 * a "no profile here" that lacks them.  (Before 2026-11-04 any error ended the list where it was, quietly.)
 */
export async function listFiles(saveDir: string): Promise<Entry[]> {
  /** d's files, or null where d is gone meanwhile (its parent's look-up of name says so) */
  async function walk(parent: FileSystemDirectoryHandle, name: string, d: FileSystemDirectoryHandle, rel: string): Promise<Entry[] | null> {
    const out: Entry[] = [];
    try {
      for await (const [n, h] of (d as any).entries() as AsyncIterable<[string, FileSystemHandle]>) {
        const r = rel ? rel + '/' + n : n;
        if (h.kind === 'directory') out.push(...await walk(d, n, h as FileSystemDirectoryHandle, r) ?? []);
        else if (!isStaged(n)) {
          let f: File;
          try { f = await (h as FileSystemFileHandle).getFile(); }
          catch (e) { if (missing(e) && !await found(d, n, 'file')) continue; throw e; }    // (gone meanwhile: left out)
          out.push({ path: r, size: f.size, mtime: f.lastModified });
        }
      }
    } catch (e) { if (missing(e) && !await found(parent, name, 'directory')) return null; throw e; }
    return out;
  }
  const r = await root(), top = await dirIn(r, [saveDir]);
  if (!top) return [];
  return (await walk(r, saveDir, top, '') ?? []).sort((a, b) => a.path < b.path ? -1 : a.path > b.path ? 1 : 0);
}

export async function readBytes(saveDir: string, path: string): Promise<Uint8Array> {
  const p = parts(path);
  const d = await dirOf(await saveRoot(saveDir, false), p.slice(0, -1), false);
  return new Uint8Array(await (await (await d.getFileHandle(p[p.length - 1])).getFile()).arrayBuffer());
}

/** Remove a file, or a folder with everything in it; nothing if it is not there. */
export async function removePath(saveDir: string, path: string): Promise<void> {
  writable();
  const p = parts(path);
  if (!p.length) throw new Error('empty path');
  const top = await dirIn(await root(), [saveDir]), d = top && await dirIn(top, p.slice(0, -1));
  if (!d) return;
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
  writable();
  const top = await saveRoot(saveDir), d = under ? await dirIn(top, parts(under)) : top;
  if (!d) return;                                                                 // not there: nothing in it
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
  writable();
  const d = await dirIn(await root(), [saveDir]);
  if (d) await walk(d, '');
}

/** A small file beside the save dirs (in none: no backup, cloud save or restore touches it), or null. */
export async function readBeside(name: string): Promise<string | null> { return readIn(await root(), name, f => f.text()); }
export async function writeBeside(name: string, text: string): Promise<void> {
  writable();
  const w = await (await (await root()).getFileHandle(name, { create: true })).createWritable();
  await w.write(text); await w.close();
}

/** what checkSaves found: after how many tries, and (not ok) the step and the error of the last one */
export interface SavesCheck { ok: boolean; tries: number; step?: 'list' | 'read' | 'write'; error?: unknown }
/** how long checkSaves asks at most, all its tries together: a call the browser never answers is a failed check */
export const CHECK_MS = 8000;
/** the files checkSaves writes and removes beside the save folder (in none: no listing, backup or cloud save sees them):
 *  a new name each try, as one left by a failed try may stay broken in a browser whose storage was (Firefox's "Entry
 *  not found"), and would fail every check after it */
const checkPrefix = (saveDir: string) => `${saveDir}~check`;
const checkFile = (saveDir: string) => `${checkPrefix(saveDir)}-${Date.now().toString(36)}${Math.random().toString(36).slice(2, 6)}`;
/** the names under the save folder, listed (no file read: a healthy folder of 300 files took Firefox 0.4-1.7 s to read
 *  every file's size); a folder gone meanwhile is left out, any other error thrown (missing, found) */
async function listNames(saveDir: string): Promise<number> {
  async function walk(parent: FileSystemDirectoryHandle, name: string, d: FileSystemDirectoryHandle): Promise<number> {
    let n = 0;
    try {
      for await (const [k, h] of (d as any).entries() as AsyncIterable<[string, FileSystemHandle]>)
        n += h.kind === 'directory' ? await walk(d, k, h as FileSystemDirectoryHandle) : 1;
    } catch (e) { if (missing(e) && !await found(parent, name, 'directory')) return 0; throw e; }
    return n;
  }
  const r = await root(), top = await dirIn(r, [saveDir]);
  return top ? walk(r, saveDir, top) : 0;
}

/**
 * Whether the save folder answers as one that works, before a game starts on it: its names listed, settings.ini and
 * registry.txt read, and a small file written and removed beside it (in the storage's top folder, not in the save
 * folder: nothing of the save folder is written or removed; those an earlier check left there go first).  One that does not is asked again after each
 * of waits, every handle fresh from navigator.storage (a lock another tab or a closing game still holds, a browser busy
 * for a moment); ok after any try.  All of it within limitMs: a call the browser never answers is a failed check
 * (TimeoutError).  Not there is no failure (a new player's folder), nor a full disk on the write (QuotaExceededError:
 * the game's own saves say that; a browser that says UnknownError for one fails the check, and the page's message says
 * to look at the disk's free space).
 * Firefox 159 on a Mac (Discord 2026-11-04) answered "UnknownError: Unknown failure" to every call there, after a
 * Firefox restart too: Play stopped with that bare error.  The day before, in the game that ran last, every file new to
 * its storage could not be opened ("NotFoundError: Entry not found"), so none of that game's new files was written.
 */
export async function checkSaves(saveDir: string, waits: number[] = [300, 1000, 2500],
    sleep = (ms: number) => new Promise<void>(r => setTimeout(r, ms)), limitMs = CHECK_MS): Promise<SavesCheck> {
  const until = Date.now() + limitMs;
  for (let i = 0; ; i++) {
    let step: SavesCheck['step'] = 'list';
    const once = (async () => {
      await listNames(saveDir);
      step = 'read';
      for (const f of ['settings.ini', 'registry.txt']) await readText(saveDir, f);
      step = 'write';
      try {
        const r = await root(), name = checkFile(saveDir);
        // (what earlier checks left: gone if it goes; one that does not is left, and this check writes beside it)
        try {
          const old: string[] = [];
          const pre = checkPrefix(saveDir);
          for await (const k of (r as any).keys() as AsyncIterable<string>) if (k === pre || k.startsWith(`${pre}-`)) old.push(k);
          for (const k of old) await r.removeEntry(k).catch(() => {});
        } catch { /* not listed: left */ }
        const w = await (await r.getFileHandle(name, { create: true })).createWritable();
        await w.write('ok'); await w.close();
        await r.removeEntry(name).catch(() => {});
      } catch (e) { if ((e as DOMException)?.name !== 'QuotaExceededError') throw e; }
    })();
    once.catch(() => {});                              // (a try that answers after its time is up: no one waits for it)
    let timer: ReturnType<typeof setTimeout> | undefined;
    const late = new Promise<'late'>(r => { timer = setTimeout(() => r('late'), Math.max(0, until - Date.now())); });
    try {
      if (await Promise.race([once.then(() => 'done' as const), late]) === 'late')
        return { ok: false, tries: i + 1, step, error: new DOMException(`no answer in ${limitMs / 1000} s`, 'TimeoutError') };
      return { ok: true, tries: i + 1 };
    } catch (error) {
      if (i >= waits.length || Date.now() + waits[i] >= until) return { ok: false, tries: i + 1, step, error };
    } finally { clearTimeout(timer); }
    await sleep(waits[i]);
  }
}

/** Remove the whole save dir (test runs: &fresh). */
export async function removeSaveDir(saveDir: string): Promise<void> {
  writable();
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
