// The saves section of the start page: the profiles in this browser (each can
// be renamed: profile.ts renameProfile), a backup of the whole save
// overlay as a zip (profiles, registry.txt, settings.ini, screenshots) and
// restoring one, or adding the PC game's profiles beside the ones here
// (readBackup tells them apart), and the F12 screenshots (shots.ts); a keyphrase's saves come in as a
// backup does (readVault, restoreVault: vault-ui.ts).  And the Other tab's game data on
// this device: how much of it, all of it downloaded ahead (predownload.ts), or
// deleted.  The game data is never part of a backup and never written back:
// it stays on the server.

import { cacheBytes, clearCache, filesOf, pruneSaveDir, UndoPending, writeTogether, type BesideFile, isScreenshot, listFiles, listProfiles, PROFILES_DIR, readBytes, readText, writeFile, type Profile } from './savefs';
import { AUTOSAVE_SEEN, coverOf, fixedName, isProfileFile, isProfileName, modCars, NAME_MAX, profileInside, renameProfile, sealed, withName, type RenameFiles } from './profile';
import { listZip, unzip, zip, ZipError, type ZipEntry } from './zip';
import { downloadBlob, stamp } from './logs';
import { fmtBytes } from './checks';
import { resolveEnv, withDevice } from './settings';
import { gameDataKept, type DownloadState, type DownloadStatus, type Predownload } from './predownload';
import { ScreenshotsPart } from './shots';
import { GAMEFILE, isCover, keeps, kept } from './vault';
import { dateLocale, listOf, live, N, t, tn, tvars, type Msg } from './i18n';

export interface SavesDeps {
  saveDir: string;
  /** true while a game runs in this tab: no rename, no restore, no cache delete */
  busy(): boolean;
  confirm(title: string, body: Node | string, ok: string, danger?: boolean): Promise<boolean>;
  /** after a restore: settings.ini may have changed */
  onRestored(): void;
  /** the download of the game data (all but the movies) ahead of play */
  download: Predownload;
  /** the saves here changed (a rename, a restore): a keyphrase sends them */
  onChanged?(): void;
}

// Limits for a restore, judged by each entry's declared size before anything is
// inflated, and held while inflating (zip.ts stops an entry at its declared
// size: a 4 MB zip that declares 100 bytes and inflates to 4 GB is refused
// after 100, the audit's PAGE-4).  Screenshots (F12, about 24 MB each at this Mac's native size) have
// their own budget, so however many a backup holds they never crowd out the
// profiles, registry.txt or settings.ini (phase-1 app review, finding 5).
const MAX_ENTRY = 64 << 20, MAX_SHOTS = 512 << 20, MAX_OTHER = 256 << 20;
export { isScreenshot };

/**
 * What a backup's settings.ini would change that a player should know before restoring it: the network
 * (free roam on, the name shown to others), variables set by their own names, and lines the page ignores
 * (files and automation are the page's own).  [] when it holds none of these.
 */
export function settingsWarnings(text: string): string[] {
  const r = resolveEnv(text, {}, {});
  const set = (k: string) => r.source[k] === 'settings.ini';
  const out: string[] = [];
  const net = Object.keys(r.env).filter(k => set(k) && /^NFSU2_(ROAM|NET|LAN)/.test(k)).map(k => `${k}=${r.env[k]}`);
  if (set('NFSU2_ROAM') && parseInt(r.env.NFSU2_ROAM, 10) === 1)
    out.push(set('NFSU2_ROAM_NAME') ? t('It turns on free-roam multiplayer: your profile name (shown as "{name}"), car and position go to the other players on this server.', { name: r.env.NFSU2_ROAM_NAME })
      : t('It turns on free-roam multiplayer: your profile name, car and position go to the other players on this server.'));
  if (net.length) out.push(t('Network settings: {list}', { list: net.join(', ') }));
  const raw = r.other.filter(o => !o.refused).map(o => `${o.key} = ${o.value}`);
  if (raw.length) out.push(t('Variables set by their own names: {list}', { list: raw.slice(0, 6).join(', ') + (raw.length > 6 ? ' …' : '') }));
  const ignored = r.other.filter(o => o.refused === 'only the page sets it').map(o => o.key);
  if (ignored.length) out.push(t('Ignored, only the page sets them: {list}', { list: ignored.join(', ') }));
  return out;
}

/** A path from a backup that may be written into the save overlay, or null. */
export function safeEntryPath(name: string): string | null { return entryPath(name, false); }
/**
 * safeEntryPath's rules; loose: control characters but NUL let through.  A profile the game named with heap bytes (a
 * failed cover shoot's save before 2e9ecee, 2026-10-27) lives in a folder of them (0x1D, 0x1E), which the browser
 * keeps: a restore takes it in loose, and renames it (backupIn, profilesIn: fixedName) before finalPaths checks the
 * paths it writes as safeEntryPath does.
 */
function entryPath(name: string, loose: boolean): string | null {
  const n = name.replace(/\\/g, '/');
  if (n.startsWith('/') || /^[A-Za-z]:/.test(n)) return null;
  const parts = n.split('/').filter(p => p !== '' && p !== '.');
  if (!parts.length || parts.some(p => p === '..' || p.length > 255 || (loose ? /\x00/ : /[\x00-\x1f]/).test(p))) return null;
  return parts.join('/');
}
/** A restore's entries whose path, as it would be written, safeEntryPath does not take (control characters left in
 *  what the restore did not rename): skipped. */
function finalPaths(r: Restore): Restore {
  const ok: Restore['entries'] = [];
  for (const e of r.entries) if (safeEntryPath(e.path) === e.path) ok.push(e); else r.skipped.push(e.path);
  r.entries = ok;
  return r;
}

export async function exportSaves(saveDir: string): Promise<{ zip: Uint8Array; files: number; name: string; shotBytes: number }> {
  const files = await listFiles(saveDir);
  const entries: ZipEntry[] = [];
  let shotBytes = 0;
  for (const f of files) {
    if (f.path === 'autotest-script.txt') continue;          // a test run's input script, not a save
    const data = await readBytes(saveDir, f.path);
    if (isScreenshot(f.path)) shotBytes += data.length;
    entries.push({ name: f.path, data, mtime: f.mtime });
  }
  return { zip: zip(entries), files: entries.length, name: `nfsu2-saves-${stamp()}.zip`, shotBytes };
}

export interface Restore {
  /**
   * 'backup': a backup of this page's (registry.txt or settings.ini at its top), which replaces everything here;
   * 'profiles': profiles alone, as the PC game keeps them (its NFS Underground 2 folder zipped in any shape, or one
   * profile's file), added beside the ones here (addProfiles)
   */
  kind: 'backup' | 'profiles';
  entries: { path: string; data: Uint8Array; mtime?: number }[];
  profiles: string[];
  /** entries left out: unsafe names, over the size limits, or (profiles) none of a profile's */
  skipped: string[];
  /** profile files the game would not load (profile.ts profileName), left out with their cover shoots */
  refused: { name: string; why: string }[];
  /** profiles with cars a mod added (profile.ts modCars), restored with a warning */
  modded: string[];
  /** profiles a save editor changed (profile.ts profileName's edited): sealed again, restored with a warning */
  edited: string[];
  /** profiles whose only fault was the name inside (not one the game's field makes, or not their file's): they come in
   *  as `to` (profile.ts fixedName), that name written inside and their checksums made again, with a note; name: as
   *  found (their folder's, or their own file's) */
  repaired: { name: string; to: string }[];
}

export type FileIn = { path: string; data: Uint8Array; mtime?: number };
const lower = (s: string) => s.toLowerCase();

/** A zip of this page's backup or of the PC game's profiles, or a profile's own file (fileName: its name, for the
 *  messages); here: the profiles in this browser, whose names a profile added beside them with its name mended does not take. */
export async function readBackup(buf: Uint8Array, fileName = 'the file', here: string[] = []): Promise<Restore> {
  if (isProfileFile(buf)) return finalPaths(profilesIn([{ path: fileName, data: buf }], [], here));
  // a backup zipped from a folder ("saves/NFS Underground 2/...") still restores: strip one common top folder
  const listed = await listZip(buf);
  const names = listed.map(n => entryPath(n, true)).filter((x): x is string => !!x);
  const top = names.length && names.every(n => n.includes('/') && n.split('/')[0] === names[0].split('/')[0]) &&
    !names.some(n => n.startsWith(PROFILES_DIR + '/')) ? names[0].split('/')[0] + '/' : '';
  const local = (name: string) => { let p = entryPath(name, true); if (p && top && p.startsWith(top)) p = p.slice(top.length); return p; };
  let shots = 0, other = 0;
  const refused: string[] = [];
  const all = await unzip(buf, (name, size) => {
    const p = local(name);
    if (!p || size > MAX_ENTRY) return false;
    if (isScreenshot(p)) return (shots += size) <= MAX_SHOTS || ((shots -= size), false);
    return (other += size) <= MAX_OTHER || ((other -= size), false);
  }, refused);
  const files = all.map(e => ({ path: local(e.name)!, data: e.data, mtime: e.mtime }));
  return finalPaths(files.some(e => e.path === 'registry.txt' || e.path === 'settings.ini') ? backupIn(files, refused) : profilesIn(files, refused, here));
}

/**
 * A backup of this page's: every entry, but a profile the game would not load and its cover shoots.  A profile whose
 * only fault is the name inside (one the game's field does not make, or not its folder's) comes in under fixedName's
 * name, with its folder's other files and its cover shoots.
 */
function backupIn(files: FileIn[], skipped: string[]): Restore {
  const out: Restore = { kind: 'backup', entries: [], profiles: [], skipped, refused: [], modded: [], edited: [], repaired: [] };
  // by each profile's folder as it is (two may differ in case only: each its own)
  const gone = new Set<string>(), moved = new Map<string, string>();
  const mains = files.filter(e => { const s = e.path.split('/'); return s.length === 3 && s[0] === PROFILES_DIR && lower(s[2]) === lower(s[1]) && !coverOf(s[1]); });
  const mend: { e: FileIn; folder: string; inside: string; edited: boolean }[] = [];
  for (const e of mains) {
    const folder = e.path.split('/')[1], r = profileInside(e.data);
    if ('bad' in r) { out.refused.push({ name: folder, why: r.bad }); gone.add(folder); continue; }
    if (!isProfileName(r.inside) || lower(r.inside) !== lower(folder)) { mend.push({ e, folder, inside: r.inside, edited: r.edited }); continue; }
    out.profiles.push(folder);
    if (r.edited) { out.edited.push(folder); e.data = sealed(e.data); }
    if (modCars(e.data)) out.modded.push(folder);
  }
  // the names taken: every folder of the profiles' (a profile's, a cover shoot's profile, files without a profile:
  // an orphan GARAGE.DAT), but those of the profiles mended
  const mending = new Set(mend.map(m => m.folder)), taken = new Set<string>();
  for (const e of files) {
    const s = e.path.split('/');
    if (s.length < 3 || s[0] !== PROFILES_DIR) continue;
    const of = coverOf(s[1])?.profile ?? s[1];
    if (!mending.has(of)) taken.add(lower(of));
  }
  for (const { e, folder, inside, edited } of mend) {
    const to = fixedName(folder, inside, taken);
    taken.add(lower(to)); moved.set(folder, to);
    e.data = withName(e.data, to);
    out.profiles.push(to); out.repaired.push({ name: folder, to });
    if (edited) out.edited.push(to);
    if (modCars(e.data)) out.modded.push(to);
  }
  out.entries = files.flatMap(e => {
    const s = e.path.split('/');
    if (s.length < 3 || s[0] !== PROFILES_DIR) return [e];
    const c = coverOf(s[1]), of = c?.profile ?? s[1];
    if (gone.has(of)) return [];
    const to = moved.get(of);
    if (to === undefined) return [e];
    // a mended profile's folder and cover shoots under its new name, each folder's own file named as it
    const folder = c ? `${to} ${c.cover}` : to;
    return [{ ...e, path: [PROFILES_DIR, folder, ...s.length === 3 && lower(s[2]) === lower(s[1]) ? [folder] : s.slice(2)].join('/') }];
  });
  return out;
}

/**
 * Profiles found by what they hold, wherever they are in the zip: each one the game
 * would load goes to "NFS Underground 2/<the name inside>/", and its cover shoots
 * ("<NAME> DVD 1/<NAME> DVD 1" anywhere) beside it.  Everything else is left out.
 * One whose only fault is the name inside (one the game's field does not make) comes
 * in under fixedName's name, which neither those nor the profiles here have.
 */
function profilesIn(files: FileIn[], skipped: string[], here: string[] = []): Restore {
  const out: Restore = { kind: 'profiles', entries: [], profiles: [], skipped, refused: [], modded: [], edited: [], repaired: [] };
  const found = new Map<string, FileIn & { name: string; edited: boolean; mended?: boolean }>();
  const mend: (FileIn & { own: string; inside: string; edited: boolean })[] = [];
  const covers: (FileIn & { profile: string; cover: string })[] = [];
  for (const e of files) {
    const s = e.path.split('/'), file = s[s.length - 1], folder = s.length > 1 ? s[s.length - 2] : '';
    const c = coverOf(file);                            // named as its folder; "20CM" too, but no profile
    if (c) { covers.push({ ...e, ...c }); continue; }
    if (!isProfileFile(e.data)) { out.skipped.push(e.path); continue; }
    const r = profileInside(e.data);
    if ('bad' in r) out.refused.push({ name: folder || file, why: r.bad });
    else if (!isProfileName(r.inside)) mend.push({ ...e, own: file, inside: r.inside, edited: r.edited });
    else if (found.has(lower(r.inside))) out.skipped.push(e.path);          // a second copy of one
    else found.set(lower(r.inside), { ...e, name: r.inside, edited: r.edited });
  }
  // the mended ones' names: none of the others', none here (an added profile of a name here replaces it)
  const taken = new Set([...found.keys(), ...here.map(lower)]), was = new Map<string, string>();
  for (const m of mend) {
    const to = fixedName(m.own, m.inside, taken);
    taken.add(lower(to)); was.set(lower(m.own), lower(to));
    found.set(lower(to), { ...m, name: to, data: withName(m.data, to), mended: true });
    out.repaired.push({ name: m.own, to });
  }
  for (const p of found.values()) {
    out.entries.push({ path: `${PROFILES_DIR}/${p.name}/${p.name}`, data: p.edited && !p.mended ? sealed(p.data) : p.data, mtime: p.mtime });
    out.profiles.push(p.name);
    if (p.edited) out.edited.push(p.name);
    if (modCars(p.data)) out.modded.push(p.name);
  }
  for (const c of covers) {
    const p = found.get(lower(c.profile)) ?? found.get(was.get(lower(c.profile)) ?? '');
    if (!p) { out.skipped.push(c.path); continue; }
    const folder = `${p.name} ${c.cover}`;
    out.entries.push({ path: `${PROFILES_DIR}/${folder}/${folder}`, data: c.data, mtime: c.mtime });
  }
  return out;
}

/** the restores of the save folder, one at a time: a backup's restore and a cloud take at once pruned what the other
 *  wrote */
let writer: Promise<unknown> = Promise.resolve();
export function oneWriter<T>(job: () => Promise<T>): Promise<T> {
  const p = writer.then(job, job);
  writer = p.catch(() => {});
  return p;
}
/** every restore asked for so far has ended (a game starts after them: shell.ts startGame, SavesPanel.settled) */
export const writesDone = (): Promise<void> => writer.then(() => {});

/** A backup of this page's in place of everything here: its files written all or none (savefs.ts writeTogether: a write
 *  that fails, the browser's storage full, leaves the saves here as they were), then what it does not have goes. */
export function restoreSaves(saveDir: string, r: Restore): Promise<void> {
  return oneWriter(async () => {
    await writeTogether(saveDir, r.entries);
    const theirs = new Set(r.entries.map(e => e.path));
    await pruneSaveDir(saveDir, path => theirs.has(path));
  });
}

/**
 * A keyphrase's saves (vault.ts kept(): a zip of the save folder but the screenshots, the covers, the graphics options,
 * the LAN server's file and the device's own settings) as a backup of this page's, whatever is at their top:
 * restoreVault puts them in place of everything here.  The same checks as a backup's; what a keyphrase does not keep is
 * left out (in one sent before 2026-10-25: covers, black ones as they were then, and the other browser's graphics
 * options and device settings; before 2026-10-28: the other browser's gamefile.bin).
 */
export async function readVault(buf: Uint8Array): Promise<Restore> {
  const refused: string[] = [];
  let other = 0;
  const all = await unzip(buf, (name, size) => {
    const p = entryPath(name, true);
    return !!p && !isScreenshot(p) && size <= MAX_ENTRY && ((other += size) <= MAX_OTHER || ((other -= size), false));
  }, refused);
  const files = all.map(e => ({ path: entryPath(e.name, true)!, data: e.data, mtime: e.mtime }));
  return finalPaths(backupIn(kept(files), refused));
}

/**
 * The profiles of a keyphrase's version (its files as vault.ts kept() has them) that readVault would leave out (another
 * version's, a path it cannot write): a version taken by itself (VaultSync.pull) must not lose one, so the player is
 * asked instead.  Their folders.
 */
export function leftOut(files: FileIn[]): string[] {
  const r = restoredOf(files);
  const came = new Set([...r.profiles, ...r.repaired.map(x => x.name)]);
  return files.map(f => f.path.split('/'))
    .filter(s => s.length === 3 && s[0] === PROFILES_DIR && lower(s[1]) === lower(s[2]) && !coverOf(s[1]) && !came.has(s[1])).map(s => s[1]);
}

/** what a restore makes of a version's files (readVault's paths, as restoreVault writes them) */
function restoredOf(files: FileIn[]) {
  const loose = files.flatMap(f => { const p = entryPath(f.path, true); return p ? [{ ...f, path: p }] : []; });
  return finalPaths(backupIn(loose, []));
}
/** The profiles a restore of a version's files brings, by the names they come in under (a broken name mended). */
export function profilesComing(files: FileIn[]): string[] { return restoredOf(files).profiles; }

/**
 * readVault's saves in place of the ones here: everything goes but what a keyphrase does not keep (vault.ts kept()):
 * the screenshots (and a test's input script), the DVD and magazine covers of the profiles they bring (those of a
 * profile whose name the restore mends under its new name), the game's graphics options, the LAN server's file, and
 * the device's own settings, which settings.ini keeps from the one here (settings.ts withDevice).  Their files are
 * written first, then what they replace goes: a write that fails (the browser's storage full) leaves the saves here.
 */
export function restoreVault(saveDir: string, r: Restore, still?: () => Promise<boolean>, beside: BesideFile[] = []): Promise<boolean> {
  return oneWriter(async () => {
    if (still && !await still()) return false;             // (the saves here changed since it was decided: VaultSync.take)
    const names = new Set(r.profiles.map(lower)), dec = new TextDecoder(), mended = new Map(r.repaired.map(x => [x.name, x.to]));
    const ini = r.entries.find(e => e.path === 'settings.ini');
    const settings = withDevice(ini ? dec.decode(ini.data) : null, await readText(saveDir, 'settings.ini'));
    const here = await listFiles(saveDir), files: FileIn[] = [];
    // every folder here and of the version: a cover moved under a mended profile's name takes the next free number
    const folders = new Set([...here.map(f => f.path), ...r.entries.map(e => e.path)].map(p => p.split('/'))
      .filter(s => s.length >= 3 && s[0] === PROFILES_DIR).map(s => s[1]));
    for (const f of here) {                                  // the covers here of a profile it mends, renamed
      const s = f.path.split('/'), c = s.length === 3 && s[0] === PROFILES_DIR ? coverOf(s[1]) : null, to = c && mended.get(c.profile);
      if (!c || !to) continue;
      const [kind, num] = [c.cover.replace(/ \d+$/, ''), +c.cover.replace(/^\D+/, '')];
      let n = num, folder = `${to} ${kind} ${n}`;
      while (folders.has(folder)) folder = `${to} ${kind} ${++n}`;
      folders.add(folder);
      files.push({ path: `${PROFILES_DIR}/${folder}/${lower(s[2]) === lower(s[1]) ? folder : s[2]}`, data: await readBytes(saveDir, f.path) });
    }
    files.push(...r.entries.filter(e => e !== ini));
    if (settings !== null) files.push({ path: 'settings.ini', data: new TextEncoder().encode(settings) });
    await writeTogether(saveDir, files, beside);
    // what it replaces goes; a profile it leaves out (one this game cannot load: 'drops') stays here, its covers too
    const written = new Set(files.map(f => f.path)), stays = new Set(r.refused.map(x => x.name));
    for (const p of r.skipped) { const s = p.split('/'); if (s.length === 3 && s[0] === PROFILES_DIR && lower(s[1]) === lower(s[2])) stays.add(s[1]); }
    await pruneSaveDir(saveDir, path => {
      const s = path.split('/'), of = s.length >= 3 && s[0] === PROFILES_DIR ? coverOf(s[1])?.profile ?? s[1] : null;
      return written.has(path) || isScreenshot(path) || path === 'autotest-script.txt' || path === 'registry.txt' || path === GAMEFILE ||
        (of !== null && stays.has(of)) || (isCover(path) && names.has(lower(coverOf(s[1])!.profile)));
    });
    return true;
  });
}

/** The save folder as a game's start has it, for refillSaves: all but the screenshots, the covers (1 MB each) and a
 *  test's input script; a player's is some 100 KB. */
export async function snapshotSaves(saveDir: string): Promise<FileIn[]> {
  const out: FileIn[] = [];
  for (const f of await listFiles(saveDir))
    if (snapshotted(f.path) && f.size <= MAX_ENTRY) out.push({ path: f.path, data: await readBytes(saveDir, f.path), mtime: f.mtime });
  return out;
}
/** what a snapshotSaves keeps: not the screenshots, the covers, a test's input script, nor a file written to be renamed
 *  (registry.txt.tmp: the runtime's registry, renamed to registry.txt once written) */
const snapshotted = (path: string) => !isScreenshot(path) && !isCover(path) && path !== 'autotest-script.txt' && !/\.tmp$/i.test(path);

/** A file the game (or the page) wrote during the game into a snapshotSaves of its start, so the start's saves that
 *  come back after the browser deleted the folder are as the game last saved them (shell.ts onSaved). */
export async function snapshotFile(start: FileIn[], saveDir: string, path: string, data?: Uint8Array) {
  if (!snapshotted(path)) return;
  const d = data ?? await readBytes(saveDir, path).catch(() => null);
  if (!d || d.length > MAX_ENTRY) return;
  const i = start.findIndex(f => f.path === path), f = { path, data: d, mtime: Date.now() };
  if (i >= 0) start[i] = f; else start.push(f);
}

/**
 * The saves back in a save folder the browser deleted during a game (shell.ts saveGone, io-worker.ts recover): cloud,
 * a keyphrase's version (readVault's entries), with every file of start (the folder as the game's start had it,
 * snapshotSaves) it lacks, this device's settings in settings.ini among them; without cloud saves, start.  A file the
 * folder has already stays (the game wrote it since: the newest), looked for again just before each write.  A write
 * that fails is tried once more after the others; failed: those that failed again (none left half there).
 */
export async function refillSaves(fs: { writeIfAbsent(path: string, data: Uint8Array): Promise<boolean> }, cloud: FileIn[] | null,
    start: FileIn[]): Promise<{ written: number; failed: { path: string; error: unknown }[] }> {
  const files: FileIn[] = [];
  if (cloud) {
    const dec = new TextDecoder(), ini = cloud.find(e => e.path === 'settings.ini'), mine = start.find(e => e.path === 'settings.ini');
    const settings = withDevice(ini ? dec.decode(ini.data) : null, mine ? dec.decode(mine.data) : null);
    files.push(...cloud.filter(e => e !== ini));
    if (settings !== null) files.push({ path: 'settings.ini', data: new TextEncoder().encode(settings) });
    const have = new Set(files.map(f => f.path));
    files.push(...start.filter(f => !have.has(f.path)));
  } else files.push(...start);
  let written = 0, again: FileIn[] = [];
  const failed: { path: string; error: unknown }[] = [];
  for (const round of [1, 2]) {
    const todo = round === 1 ? files : again;
    again = [];
    for (const f of todo)
      try { if (await fs.writeIfAbsent(f.path, f.data)) written++; }
      catch (error) { if (round === 1) again.push(f); else failed.push({ path: f.path, error }); }
  }
  return { written, failed };
}

/** What a restore says of the profiles whose name inside it mends (Restore.repaired), one line each. */
export function repairNotes(r: Pick<Restore, 'repaired'>): string[] {
  return r.repaired.map(x => t('{from}: its name inside was broken, so it comes in as {to}.', { from: x.name, to: x.to }));
}

/** What a restore's question says of its profiles' state: a name mended, changed by a save editor, mod cars, left out, skipped. */
function restoreNotes(r: Restore): string[] {
  const items: string[] = [...repairNotes(r)];
  const refused = r.refused.map(x => `${x.name}: ${x.why}`).join('; ');
  if (r.edited.length) items.push(tn(r.edited.length, '{names} was edited with a save editor: values the game wouldn\'t make can crash it or break the career.',
    '{names} were edited with a save editor: values the game wouldn\'t make can crash it or break the career.', { names: listOf(r.edited) }));
  if (r.modded.length) items.push(tn(r.modded.length, '{names} has modded cars this game lacks: it may crash or show them wrong.',
    '{names} have modded cars this game lacks: it may crash or show them wrong.', { names: listOf(r.modded) }));
  if (refused) items.push(tn(r.refused.length, 'Left out, as the game would not load it: {list}.', 'Left out, as the game would not load them: {list}.', { list: refused }));
  const skipped = r.skipped.slice(0, 3).join(', ') + (r.skipped.length > 3 ? ' …' : '');
  if (r.skipped.length) items.push(r.kind === 'profiles' ? tn(r.skipped.length, '{n} other file is left out: {list}', '{n} other files are left out: {list}', { list: skipped })
    : tn(r.skipped.length, '{n} entry is skipped (an unsafe name, or over the size limits): {list}', '{n} entries are skipped (unsafe names, or over the size limits): {list}', { list: skipped }));
  return items;
}

/** What a backup's restore brings (or a keyphrase's, vault-ui.ts: the game's options stay), its profiles, the game's
 *  options, settings.ini, restoreNotes. */
export function backupItems(r: Restore, vault = false): string[] {
  const ini = r.entries.find(e => e.path === 'settings.ini');
  const warn = ini ? settingsWarnings(new TextDecoder().decode(ini.data)) : [];
  return [
    r.profiles.length ? t('Profiles: {names}', { names: r.profiles.join(', ') }) : t('No profiles'),
    ...vault ? [] : [r.entries.some(e => e.path === 'registry.txt') ? t('The game\'s options (registry.txt)') : t('No registry.txt: the next launch starts with fresh game options')],
    ini ? (warn.length ? `settings.ini. ${warn.join(' ')}` : 'settings.ini') : t('No settings.ini: the defaults apply'),
    ...restoreNotes(r),
  ];
}

/**
 * Add the profiles of a Restore of kind 'profiles'.  A profile of the same name here (in
 * any case: the I/O worker finds names so) goes, with its cover shoots; nothing else
 * changes.  The new folders are written before the old ones go.  Their names leave
 * AUTOSAVE_SEEN, so the page's Autosave comes to them as to new profiles.
 */
export function addProfiles(fs: RenameFiles, r: Restore): Promise<void> {
  return oneWriter(() => addProfilesNow(fs, r));
}
async function addProfilesNow(fs: RenameFiles, r: Restore) {
  const names = new Set(r.profiles.map(lower));
  const written = new Set(r.entries.map(e => e.path.split('/')[1]));
  const paths = new Set(r.entries.map(e => e.path));
  const old = new Set<string>(), stale: string[] = [];
  for (const { path } of await fs.listFiles()) {
    const s = path.split('/');
    if (s.length < 3 || s[0] !== PROFILES_DIR || !names.has(lower(coverOf(s[1])?.profile ?? s[1]))) continue;
    if (!written.has(s[1])) old.add(s[1]);
    // a folder of the same name, written over: the old profile's MPWINS.DAT goes too, if the new one brings none (the
    // old career's wins waiting for its garage would go to the new one's; a GARAGE.DAT has its fingerprint for this)
    else if (!paths.has(path) && lower(s[2]) === 'mpwins.dat') stale.push(path);
  }
  for (const e of r.entries) await fs.writeFile(e.path, e.data);
  for (const f of old) await fs.remove(`${PROFILES_DIR}/${f}`);
  for (const p of stale) await fs.remove(p);
  const seen = await fs.readText(AUTOSAVE_SEEN);
  const kept = seen?.split('\n').filter(l => !names.has(lower(l.trim()))).join('\n');
  if (seen != null && kept !== seen) await fs.writeFile(AUTOSAVE_SEEN, kept!);
}

/** "about 3 min left" */
function eta(s: number): string {
  if (!isFinite(s)) return '';
  if (s < 60) return t('less than a minute left');
  if (s < 3600) return t('about {n} min left', { n: Math.round(s / 60) });
  return t('about {n} h left', { n: (s / 3600).toFixed(1) });
}
const QUIT_TO_CHANGE = N('Quit the game (or close the other tab that runs it) to change the game data.');
const QUIT_TO_RENAME = N('Quit the game (or close the other tab that runs it) to rename a profile or restore a backup.');

export class SavesPanel {
  /** root: the Saves tab; dataPanel: Game data on this device */
  constructor(private root: HTMLElement, private dataPanel: HTMLElement, private deps: SavesDeps) {
    root.querySelector<HTMLButtonElement>('#export')!.onclick = () => void this.doExport();
    const inp = root.querySelector<HTMLInputElement>('#import')!;
    inp.onchange = () => { const f = inp.files?.[0]; inp.value = ''; if (f) void this.doImport(f); };
    dataPanel.querySelector<HTMLButtonElement>('#clear-cache')!.onclick = () => void this.doClearCache();
    dataPanel.querySelector<HTMLButtonElement>('#fetch-all')!.onclick = () => void this.doDownload();
    this.shots = new ScreenshotsPart(root, { saveDir: deps.saveDir, confirm: deps.confirm, changed: () => void this.refresh() });
  }

  private shots: ScreenshotsPart;

  /** the section's messages, drawn again when the page's language changes (a Msg); said: which English text each says
   *  (the "quit the game first" ones, which go once the game ended) */
  private said = { saves: '', data: '' };
  private msg(s: string | Msg, en = '') { this.said.saves = en; live(this.root.querySelector('#saves-msg')!, typeof s === 'string' ? () => s : s); }
  private dataMsg(s: string | Msg, warn = false, en = '') {
    const m = this.dataPanel.querySelector('#data-msg')!;
    this.said.data = en; live(m, typeof s === 'string' ? () => s : s); m.classList.toggle('warn', warn);
  }

  /** the game data in this browser: of the server's files (null: their list did not load), and all that is kept */
  private data: { have: number; total: number } | null = null;
  private kept = { bytes: 0, files: 0 };
  private counted = 0;                     // the latest count wins (one may start while the data is deleted)
  private shownState: DownloadState = 'idle';
  private profiles: Profile[] = [];
  /** the profile whose name is being edited; focusName: whose Rename button gets the focus when the list is shown */
  private renaming: string | null = null;
  private focusName: string | null = null;
  /** what the section is writing into the saves (a rename, a restore): settled() waits for it */
  private writing: Promise<unknown> = Promise.resolve();

  /** Resolves when the section writes nothing into the saves: a game starts after that. */
  settled(): Promise<void> { return Promise.all([this.writing.catch(() => {}), writesDone()]).then(() => {}); }
  private write<T>(job: Promise<T>): Promise<T> { this.writing = job; return job; }

  /** Show the profiles, the screenshots and the downloaded game data as they are now. */
  async refresh() {
    const files = await listFiles(this.deps.saveDir);
    this.profiles = await listProfiles(this.deps.saveDir, files);
    this.shots.show(files.filter(f => isScreenshot(f.path)));
    const busy = this.deps.busy();
    // a name being typed stays as it is (the list is shown again when the edit ends)
    if (!this.profiles.some(p => p.name === this.renaming)) { this.renaming = null; this.showProfiles(); }
    for (const b of this.root.querySelectorAll<HTMLButtonElement>('.profiles .rename-btn, .profiles .renaming [type="submit"]')) b.disabled = busy;
    this.root.querySelector<HTMLLabelElement>('.file-btn')!.toggleAttribute('aria-disabled', busy);
    this.root.querySelector<HTMLInputElement>('#import')!.disabled = busy;
    this.dataPanel.querySelector<HTMLButtonElement>('#clear-cache')!.disabled = busy;
    if (busy) { this.msg(() => t(QUIT_TO_RENAME), QUIT_TO_RENAME); this.dataMsg(() => t(QUIT_TO_CHANGE), false, QUIT_TO_CHANGE); }
    else {
      if (this.said.saves === QUIT_TO_RENAME) this.msg('');
      if (this.said.data === QUIT_TO_CHANGE) this.dataMsg('');
    }
    this.root.dataset.files = String(files.length);
    if (this.deps.download.running) { this.showData(); return; }     // it counts as it goes
    const n = ++this.counted;
    const [kept, data] = await Promise.all([cacheBytes(), gameDataKept(this.deps.download.language(), this.deps.download.packs()).catch(() => null)]);
    if (n !== this.counted) return;
    this.kept = kept; this.data = data;
    this.showData();
  }

  /** The download's progress (Predownload's onChange), or what is kept when none runs. */
  showData(st: DownloadStatus = this.deps.download.status) {
    const active = st.state === 'running' || st.state === 'paused';
    if (active && st.total) this.data = { have: st.have, total: st.total };
    if (st.state !== this.shownState) {
      this.shownState = st.state;
      if (st.state === 'done') { this.data = { have: st.have, total: st.total }; this.dataMsg(() => t('The game data is on this device now.')); }
      if (st.state === 'error') { const e = st.error; this.dataMsg(() => t('Preloading stopped: {error}. What\'s done is kept.', { error: e ?? '' }), true); }
      if (!active) void this.refresh();                         // counted again; shown now with what is known
    }
    const info = this.dataPanel.querySelector('#cache-info')!, bar = this.dataPanel.querySelector<HTMLElement>('#data-bar')!;
    const fetchBtn = this.dataPanel.querySelector<HTMLButtonElement>('#fetch-all')!;
    const d = this.data, rest = d ? d.total - d.have : 0;
    bar.hidden = !active;
    fetchBtn.disabled = this.deps.busy() && !active;
    if (active) {
      const f = d?.total ? d.have / d.total : 0;
      (bar.firstElementChild as HTMLElement).style.width = `${(f * 100).toFixed(1)}%`;
      bar.setAttribute('aria-valuenow', String(Math.floor(f * 100)));
      fetchBtn.hidden = false;
      fetchBtn.textContent = t('Stop preloading');
      fetchBtn.disabled = st.state === 'paused';
      info.textContent = st.state === 'paused' ? (d ? t('Preloading paused while you play: {have} of {total}.', { have: fmtBytes(d.have), total: fmtBytes(d.total) })
          : t('Preloading paused while you play.'))
        : !d ? t('Preloading: checking what\'s here already.')
        : t('Preloading: {have} of {total} ({pct} %){speed}. You can play meanwhile: it pauses while you do.',
          { have: fmtBytes(d.have), total: fmtBytes(d.total), pct: Math.floor(f * 100), speed: st.rate ? `, ${fmtBytes(st.rate)}/s, ${eta(rest / st.rate)}` : '' });
      return;
    }
    fetchBtn.textContent = d && d.have ? t('Preload the rest ({size})', { size: fmtBytes(rest) }) : d ? t('Preload the game data ({size})', { size: fmtBytes(d.total) }) : t('Preload the game data');
    fetchBtn.hidden = !!d && rest <= 0;
    if (d && rest <= 0)
      info.textContent = 'serviceWorker' in navigator
        ? t('All game data ({size}) is on this device; only the movies stream from the server. The game starts offline too, without movies.', { size: fmtBytes(d.total) })
        : t('All game data ({size}) is on this device; only the movies stream from the server.', { size: fmtBytes(d.total) });
    else if (d ? d.have : this.kept.bytes)
      info.textContent = (d ? t('{have} of {total} is on this device (movies not included).', { have: fmtBytes(d.have), total: fmtBytes(d.total) })
        : t('{have} is on this device.', { have: fmtBytes(this.kept.bytes) })) + ' ' +
        t('Preload the rest so loading never waits on the network.');
    else
      info.textContent = t('Nothing here yet: the game downloads data as it needs it. Preload it now so loading never waits on the network.');
  }

  /** The profiles, each with its Rename button, or the name being edited. */
  private showProfiles() {
    const ul = this.root.querySelector('#profiles')!, busy = this.deps.busy();
    let focus: HTMLElement | null = null;
    ul.replaceChildren();
    if (!this.profiles.length) {
      const li = document.createElement('li'); li.className = 'empty';
      li.textContent = t('No profiles yet. Create one in the game.');
      ul.append(li);
    }
    for (const p of this.profiles) {
      const li = document.createElement('li');
      if (p.name === this.renaming) {
        const { form, input } = this.renameForm(p.name, busy);
        const help = document.createElement('p'); help.className = 'hint'; help.id = 'rename-help';
        help.textContent = t('Letters and digits, up to {n}, as in the game.', { n: NAME_MAX });
        const m = document.createElement('p'); m.className = 'msg warn'; m.id = 'rename-msg'; m.setAttribute('role', 'alert');
        li.className = 'renaming'; li.append(form, help, m); ul.append(li);
        focus = input;
        continue;
      }
      const n = document.createElement('span'); n.className = 'name'; n.textContent = p.name;
      const m = document.createElement('span'); m.className = 'meta';
      m.textContent = t('saved {when}, {size}', { when: new Date(p.mtime).toLocaleString(dateLocale()), size: fmtBytes(p.size) });
      const b = document.createElement('button'); b.type = 'button'; b.className = 'text-btn rename-btn'; b.textContent = t('Rename');
      b.setAttribute('aria-label', t('Rename {name}', { name: p.name })); b.disabled = busy;
      b.onclick = () => { this.renaming = p.name; this.showProfiles(); };
      if (p.name === this.focusName) focus = b;
      li.append(n, m, b); ul.append(li);
    }
    this.focusName = null;
    if (focus) { focus.focus(); if (focus instanceof HTMLInputElement) focus.select(); }
  }

  private renameForm(name: string, busy: boolean) {
    const form = document.createElement('form'); form.className = 'row';
    const input = document.createElement('input');
    input.type = 'text'; input.value = name; input.maxLength = NAME_MAX; input.spellcheck = false; input.autocomplete = 'off';
    input.setAttribute('aria-label', t('New name for {name}', { name })); input.setAttribute('aria-describedby', 'rename-help rename-msg');
    // what the game's field keeps: other characters never appear
    input.oninput = () => {
      const v = input.value, keep = (t: string) => t.replace(/[^A-Za-z0-9]/g, '');
      if (keep(v) === v) return;
      const at = keep(v.slice(0, input.selectionStart ?? v.length)).length;
      input.value = keep(v); input.setSelectionRange(at, at);
    };
    const ok = document.createElement('button'); ok.type = 'submit'; ok.className = 'btn'; ok.textContent = t('Rename'); ok.disabled = busy;
    const cancel = document.createElement('button'); cancel.type = 'button'; cancel.className = 'text-btn'; cancel.textContent = t('Cancel');
    cancel.onclick = () => this.endRename(name);
    form.append(input, ok, cancel);
    form.onsubmit = e => { e.preventDefault(); void this.doRename(name, input.value); };
    form.onkeydown = e => { if (e.key === 'Escape') { e.preventDefault(); this.endRename(name); } };
    return { form, input };
  }

  private endRename(name: string) { this.renaming = null; this.focusName = name; this.showProfiles(); }

  private async doRename(from: string, to: string) {
    const say = (s: string) => { this.root.querySelector('#rename-msg')!.textContent = s; };
    if (to === from) { this.endRename(from); return; }
    if (this.deps.busy()) { say(t('Quit the game (or close the other tab that runs it) to rename a profile.')); return; }
    const ok = this.root.querySelector<HTMLButtonElement>('.profiles .renaming [type="submit"]')!;
    if (ok.disabled) return;
    ok.disabled = true;
    let r: Awaited<ReturnType<typeof renameProfile>>;
    try { r = await this.write(oneWriter(() => renameProfile(filesOf(this.deps.saveDir), PROFILES_DIR, from, to))); }
    catch (err) { this.renaming = null; this.msg(() => t('Renaming {name} stopped: {error}', { name: from, error: String(err) })); await this.refresh(); return; }
    if ('refused' in r) { ok.disabled = this.deps.busy(); say(r.refused); return; }
    this.renaming = null; this.focusName = to;
    this.msg(() => t('Renamed {from} to {to}.', { from, to }));
    this.deps.onChanged?.();
    await this.refresh();
  }

  private async doDownload() {
    const dl = this.deps.download;
    if (dl.running) { await dl.stop(); this.dataMsg(() => t('Preloading stopped. What\'s done is kept.')); return; }
    if (this.deps.busy()) return;
    this.dataMsg('');
    dl.start();
  }

  private async doExport() {
    try {
      const e = await exportSaves(this.deps.saveDir);
      downloadBlob(e.zip as BlobPart, e.name, 'application/zip');
      this.msg(() => tn(e.files, 'Downloaded {name}: {n} file, {size}.', 'Downloaded {name}: {n} files, {size}.', { name: e.name, size: fmtBytes(e.zip.length) }) + (e.shotBytes > MAX_SHOTS
        ? ' ' + t('It holds {shots} of screenshots: a restore keeps only the first {max} of them.', { shots: fmtBytes(e.shotBytes), max: fmtBytes(MAX_SHOTS) }) : ''));
    } catch (err) { this.msg(() => t('The backup failed: {error}', { error: String(err) })); }
  }

  private async doImport(file: File) {
    if (this.deps.busy()) { this.msg(() => t('Quit the game first: restoring replaces the saves it is using.')); return; }
    let r: Restore;
    try { r = await readBackup(new Uint8Array(await file.arrayBuffer()), file.name, this.profiles.map(p => p.name)); }
    catch (err) {
      const name = file.name;
      this.msg(() => err instanceof ZipError ? t('{file} cannot be restored: {why}.', { file: name, why: err.message === 'not a zip file' ? t('it is neither a backup (a zip) nor a profile of the game\'s') : err.message })
        : t('{file} cannot be read: {error}', { file: name, error: String(err) }));
      return;
    }
    const refused = r.refused.map(x => `${x.name}: ${x.why}`).join('; ');
    if (!r.profiles.length && (r.kind === 'profiles' || !r.entries.length)) {
      const one = r.refused.length === 1 ? r.refused[0] : null;
      const covers = r.skipped.length && r.skipped.every(s => coverOf(s.split('/').pop()!));
      const name = file.name;
      this.msg(() => one ? (one.name === name ? t('{file} cannot be restored: {why}.', { file: name, why: one.why }) : t('The profile {name} in {file} cannot be restored: {why}.', { name: one.name, file: name, why: one.why }))
        : refused ? t('{file} has no profile the game can load: {list}.', { file: name, list: refused })
        : covers ? t('{file} holds only covers: restore them with their profile, in a zip of the NFS Underground 2 folder.', { file: name })
        : t('{file} has no save files.', { file: name }));
      return;
    }
    const list = listOf;
    const body = document.createElement('div');
    const p = document.createElement('p');
    const items: string[] = [];
    const here = new Map(this.profiles.map(x => [x.name.toLowerCase(), x.name]));
    const replaced = r.profiles.map(n => here.get(n.toLowerCase())).filter((x): x is string => !!x);
    if (r.kind === 'profiles') {
      p.textContent = tn(r.profiles.length, 'Adds the profile in {file}. Other profiles, the game\'s options and settings.ini stay as they are.',
        'Adds the profiles in {file}. Other profiles, the game\'s options and settings.ini stay as they are.', { file: file.name });
      items.push(t('Profiles: {names}', { names: r.profiles.map(n => {
        const covers = r.entries.filter(e => coverOf(e.path.split('/')[1])?.profile === n).length;
        return covers ? tn(covers, '{name} (with its cover shoot)', '{name} (with its {n} cover shoots)', { name: n }) : n;
      }).join(', ') }));
      if (replaced.length) items.push(tn(replaced.length, 'Replaces {names} here, with its career and cover shoots',
        'Replaces {names} here, with their careers and cover shoots', { names: list(replaced) }));
      items.push(...restoreNotes(r));
    } else {
      p.textContent = tn(r.entries.length, 'Replaces every profile, the game\'s options and settings.ini here with the {n} file in {file}.',
        'Replaces every profile, the game\'s options and settings.ini here with the {n} files in {file}.', { file: file.name });
      items.push(...backupItems(r));
    }
    const ul = document.createElement('ul');
    for (const t of items) { const li = document.createElement('li'); li.textContent = t; ul.append(li); }
    body.append(p, ul);
    const adding = r.kind === 'profiles';
    const many = r.profiles.length > 1, name = file.name;
    if (!await this.deps.confirm(adding ? (many ? t('Add these profiles?') : t('Add this profile?')) : t('Restore this backup?'), body,
      adding ? (many ? t('Add profiles') : t('Add profile')) : t('Restore backup'), !adding || replaced.length > 0)) { this.msg(() => t('Nothing changed.')); return; }
    try {
      if (adding) {
        await this.write(addProfiles(filesOf(this.deps.saveDir), r));
        this.msg(() => t('Added {names} from {file}.', { names: list(r.profiles), file: name }));
      } else {
        await this.write(restoreSaves(this.deps.saveDir, r));
        this.msg(() => tn(r.entries.length, 'Restored {n} file from {file}.', 'Restored {n} files from {file}.', { file: name }));
        this.deps.onRestored();
      }
    } catch (err) {
      this.msg(() => err instanceof UndoPending ? t('An earlier restore stopped halfway and is not undone yet, so nothing was changed. Reload the page, then try again.')
        : adding ? t('Adding stopped halfway: {error}. Add the profiles again before playing.', { error: String(err) })
        : t('Restoring stopped halfway: {error}. Restore the backup again before playing.', { error: String(err) }));
    }
    this.deps.onChanged?.();
    await this.refresh();
  }

  private async doClearCache() {
    if (this.deps.busy()) return;
    const dl = this.deps.download;
    if (!await this.deps.confirm(t('Delete the game data on this device?'), (dl.running ? t('This stops preloading.') + ' ' : '') +
      t('The game downloads it again as needed. Your saves stay.'), t('Delete game data'), true)) return;
    await dl.stop();
    await clearCache();
    await this.refresh();
    this.dataMsg(() => t('Game data on this device deleted.'));
  }
}
