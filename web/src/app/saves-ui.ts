// The saves section of the start page: the profiles in this browser (each can
// be renamed: profile.ts renameProfile), a backup of the whole save
// overlay as a zip (profiles, registry.txt, settings.ini, screenshots) and
// restoring one, or adding the PC game's profiles beside the ones here
// (readBackup tells them apart), and the F12 screenshots (shots.ts); a keyphrase's saves come in as a
// backup does (readVault, restoreVault: vault-ui.ts).  And the section at the bottom of the page, the game data on
// this PC: how much of it, all of it downloaded ahead (predownload.ts), or
// deleted.  The game data is never part of a backup and never written back:
// it stays on the server.

import { cacheBytes, clearCache, clearSaveDir, filesOf, isScreenshot, listFiles, listProfiles, PROFILES_DIR, readBytes, readText, writeFile, type Profile } from './savefs';
import { AUTOSAVE_SEEN, coverOf, isProfileFile, modCars, NAME_MAX, profileName, renameProfile, sealed, type RenameFiles } from './profile';
import { listZip, unzip, zip, ZipError, type ZipEntry } from './zip';
import { downloadBlob, stamp } from './logs';
import { fmtBytes } from './checks';
import { resolveEnv, withDevice } from './settings';
import { gameDataKept, type DownloadState, type DownloadStatus, type Predownload } from './predownload';
import { ScreenshotsPart } from './shots';
import { kept } from './vault';
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
export function safeEntryPath(name: string): string | null {
  const n = name.replace(/\\/g, '/');
  if (n.startsWith('/') || /^[A-Za-z]:/.test(n)) return null;
  const parts = n.split('/').filter(p => p !== '' && p !== '.');
  if (!parts.length || parts.some(p => p === '..' || p.length > 255 || /[\x00-\x1f]/.test(p))) return null;
  return parts.join('/');
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
}

type FileIn = { path: string; data: Uint8Array; mtime?: number };
const lower = (s: string) => s.toLowerCase();

/** A zip of this page's backup or of the PC game's profiles, or a profile's own file (fileName: its name, for the messages). */
export async function readBackup(buf: Uint8Array, fileName = 'the file'): Promise<Restore> {
  if (isProfileFile(buf)) return profilesIn([{ path: fileName, data: buf }], []);
  // a backup zipped from a folder ("saves/NFS Underground 2/...") still restores: strip one common top folder
  const listed = await listZip(buf);
  const names = listed.map(n => safeEntryPath(n)).filter((x): x is string => !!x);
  const top = names.length && names.every(n => n.includes('/') && n.split('/')[0] === names[0].split('/')[0]) &&
    !names.some(n => n.startsWith(PROFILES_DIR + '/')) ? names[0].split('/')[0] + '/' : '';
  const local = (name: string) => { let p = safeEntryPath(name); if (p && top && p.startsWith(top)) p = p.slice(top.length); return p; };
  let shots = 0, other = 0;
  const refused: string[] = [];
  const all = await unzip(buf, (name, size) => {
    const p = local(name);
    if (!p || size > MAX_ENTRY) return false;
    if (isScreenshot(p)) return (shots += size) <= MAX_SHOTS || ((shots -= size), false);
    return (other += size) <= MAX_OTHER || ((other -= size), false);
  }, refused);
  const files = all.map(e => ({ path: local(e.name)!, data: e.data, mtime: e.mtime }));
  return files.some(e => e.path === 'registry.txt' || e.path === 'settings.ini') ? backupIn(files, refused) : profilesIn(files, refused);
}

/** A backup of this page's: every entry, but a profile the game would not load and its cover shoots. */
function backupIn(files: FileIn[], skipped: string[]): Restore {
  const out: Restore = { kind: 'backup', entries: [], profiles: [], skipped, refused: [], modded: [], edited: [] };
  const gone = new Set<string>();
  for (const e of files) {
    const s = e.path.split('/');
    if (s.length !== 3 || s[0] !== PROFILES_DIR || lower(s[2]) !== lower(s[1]) || coverOf(s[1])) continue;
    let r = profileName(e.data);
    if ('name' in r && lower(r.name) !== lower(s[1])) r = { bad: t('the name inside is "{name}"', { name: r.name }) };
    if ('bad' in r) { out.refused.push({ name: s[1], why: r.bad }); gone.add(lower(s[1])); continue; }
    out.profiles.push(s[1]);
    if (r.edited) { out.edited.push(s[1]); e.data = sealed(e.data); }
    if (modCars(e.data)) out.modded.push(s[1]);
  }
  out.entries = files.filter(e => {
    const s = e.path.split('/');
    return !(s.length >= 3 && s[0] === PROFILES_DIR && gone.has(lower(coverOf(s[1])?.profile ?? s[1])));
  });
  return out;
}

/**
 * Profiles found by what they hold, wherever they are in the zip: each one the game
 * would load goes to "NFS Underground 2/<the name inside>/", and its cover shoots
 * ("<NAME> DVD 1/<NAME> DVD 1" anywhere) beside it.  Everything else is left out.
 */
function profilesIn(files: FileIn[], skipped: string[]): Restore {
  const out: Restore = { kind: 'profiles', entries: [], profiles: [], skipped, refused: [], modded: [], edited: [] };
  const found = new Map<string, FileIn & { name: string; edited: boolean }>();
  const covers: (FileIn & { profile: string; cover: string })[] = [];
  for (const e of files) {
    const s = e.path.split('/'), file = s[s.length - 1], folder = s.length > 1 ? s[s.length - 2] : '';
    const c = coverOf(file);                            // named as its folder; "20CM" too, but no profile
    if (c) { covers.push({ ...e, ...c }); continue; }
    if (!isProfileFile(e.data)) { out.skipped.push(e.path); continue; }
    const r = profileName(e.data);
    if ('bad' in r) out.refused.push({ name: folder || file, why: r.bad });
    else if (found.has(lower(r.name))) out.skipped.push(e.path);          // a second copy of one
    else found.set(lower(r.name), { ...e, name: r.name, edited: r.edited });
  }
  for (const p of found.values()) {
    out.entries.push({ path: `${PROFILES_DIR}/${p.name}/${p.name}`, data: p.edited ? sealed(p.data) : p.data, mtime: p.mtime });
    out.profiles.push(p.name);
    if (p.edited) out.edited.push(p.name);
    if (modCars(p.data)) out.modded.push(p.name);
  }
  for (const c of covers) {
    const p = found.get(lower(c.profile));
    if (!p) { out.skipped.push(c.path); continue; }
    const folder = `${p.name} ${c.cover}`;
    out.entries.push({ path: `${PROFILES_DIR}/${folder}/${folder}`, data: c.data, mtime: c.mtime });
  }
  return out;
}

export async function restoreSaves(saveDir: string, r: Restore) {
  await clearSaveDir(saveDir);
  for (const e of r.entries) await writeFile(saveDir, e.path, e.data);
}

/**
 * A keyphrase's saves (vault.ts kept(): a zip of the save folder but the screenshots, the covers, the graphics options
 * and the device's own settings) as a backup of this page's, whatever is at their top: restoreVault puts them in place
 * of everything here.  The same checks as a backup's; what a keyphrase does not keep is left out (in one sent before
 * 2026-10-25: covers, black ones as they were then, and the other browser's graphics options and device settings).
 */
export async function readVault(buf: Uint8Array): Promise<Restore> {
  const refused: string[] = [];
  let other = 0;
  const all = await unzip(buf, (name, size) => {
    const p = safeEntryPath(name);
    return !!p && !isScreenshot(p) && size <= MAX_ENTRY && ((other += size) <= MAX_OTHER || ((other -= size), false));
  }, refused);
  const files = all.map(e => ({ path: safeEntryPath(e.name)!, data: e.data, mtime: e.mtime }));
  return backupIn(kept(files), refused);
}

/** readVault's saves in place of the ones here: everything goes but what a keyphrase does not keep (vault.ts kept()):
 *  the screenshots (and a test's input script), the DVD and magazine covers of the profiles they bring, the game's
 *  graphics options, and the device's own settings, which settings.ini keeps from the one here (settings.ts withDevice). */
export async function restoreVault(saveDir: string, r: Restore) {
  const names = new Set(r.profiles.map(lower)), dec = new TextDecoder();
  const ini = r.entries.find(e => e.path === 'settings.ini');
  const settings = withDevice(ini ? dec.decode(ini.data) : null, await readText(saveDir, 'settings.ini'));
  await clearSaveDir(saveDir, name => isScreenshot(name) || name === 'autotest-script.txt' || name === 'registry.txt' || name === PROFILES_DIR);
  await clearSaveDir(saveDir, name => names.has(lower(coverOf(name)?.profile ?? '')), PROFILES_DIR);
  for (const e of r.entries) if (e !== ini) await writeFile(saveDir, e.path, e.data);
  if (settings !== null) await writeFile(saveDir, 'settings.ini', settings);
}

/** What a restore's question says of its profiles' state: changed by a save editor, mod cars, left out, skipped. */
function restoreNotes(r: Restore): string[] {
  const items: string[] = [];
  const refused = r.refused.map(x => `${x.name}: ${x.why}`).join('; ');
  if (r.edited.length) items.push(tn(r.edited.length, '{names} was changed outside the game, by a save editor (the checksums no longer matched): a value the game would not make itself may crash it or break the career.',
    '{names} were changed outside the game, by a save editor (the checksums no longer matched): a value the game would not make itself may crash it or break the career.', { names: listOf(r.edited) }));
  if (r.modded.length) items.push(tn(r.modded.length, '{names} has cars a mod added, which this game does not have: the game may crash or show them wrong when it loads them.',
    '{names} have cars a mod added, which this game does not have: the game may crash or show them wrong when it loads them.', { names: listOf(r.modded) }));
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
export async function addProfiles(fs: RenameFiles, r: Restore) {
  const names = new Set(r.profiles.map(lower));
  const written = new Set(r.entries.map(e => e.path.split('/')[1]));
  const old = new Set<string>();
  for (const { path } of await fs.listFiles()) {
    const s = path.split('/');
    if (s.length >= 3 && s[0] === PROFILES_DIR && names.has(lower(coverOf(s[1])?.profile ?? s[1])) && !written.has(s[1])) old.add(s[1]);
  }
  for (const e of r.entries) await fs.writeFile(e.path, e.data);
  for (const f of old) await fs.remove(`${PROFILES_DIR}/${f}`);
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
  /** root: the Saves section; dataPanel: Game data on this PC */
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
  settled(): Promise<void> { return this.writing.then(() => {}, () => {}); }
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
    const [kept, data] = await Promise.all([cacheBytes(), gameDataKept(this.deps.download.language(), this.deps.download.texpack(), this.deps.download.voices()).catch(() => null)]);
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
      if (st.state === 'done') { this.data = { have: st.have, total: st.total }; this.dataMsg(() => t('The game data is on this PC now.')); }
      if (st.state === 'error') { const e = st.error; this.dataMsg(() => t('Preloading stopped: {error}. What it preloaded is kept.', { error: e ?? '' }), true); }
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
      info.textContent = st.state === 'paused' ? (d ? t('Preloading paused while the game runs: {have} of {total}. It goes on when the game ends.', { have: fmtBytes(d.have), total: fmtBytes(d.total) })
          : t('Preloading paused while the game runs: counting. It goes on when the game ends.'))
        : !d ? t('Preloading the game data: counting what is here already.')
        : t('Preloading the game data: {have} of {total} ({pct} %){speed}. Play whenever you like: preloading pauses while the game runs and goes on after it.',
          { have: fmtBytes(d.have), total: fmtBytes(d.total), pct: Math.floor(f * 100), speed: st.rate ? `, ${fmtBytes(st.rate)}/s, ${eta(rest / st.rate)}` : '' });
      return;
    }
    fetchBtn.textContent = d && d.have ? t('Preload the rest ({size})', { size: fmtBytes(rest) }) : d ? t('Preload the game data ({size})', { size: fmtBytes(d.total) }) : t('Preload the game data');
    fetchBtn.hidden = !!d && rest <= 0;
    if (d && rest <= 0)
      info.textContent = 'serviceWorker' in navigator
        ? t('The game data ({size}) is kept in this browser: the game loads it from this PC, not the network. Only the movies stream from the server, as they play, and the game starts without the server too (offline, with the movies off). Deleting it only means loading it from the server again.', { size: fmtBytes(d.total) })
        : t('The game data ({size}) is kept in this browser: the game loads it from this PC, not the network. Only the movies stream from the server, as they play. Deleting it only means loading it from the server again.', { size: fmtBytes(d.total) });
    else if (d ? d.have : this.kept.bytes)
      info.textContent = (d ? t('{have} of the game\'s {total} (without the movies) is kept in this browser, so the game loads that from this PC instead of the network.', { have: fmtBytes(d.have), total: fmtBytes(d.total) })
        : t('{have} is kept in this browser, so the game loads that from this PC instead of the network.', { have: fmtBytes(this.kept.bytes) })) + ' ' +
        t('Preload the rest too, and no loading screen waits on the network. Deleting it only means loading it from the server again.');
    else
      info.textContent = t('Nothing here yet. The game streams its data from the server as it needs it and keeps it here. Preload it now (all but the movies, compressed on the way) and no loading screen waits on the network.');
  }

  /** The profiles, each with its Rename button, or the name being edited. */
  private showProfiles() {
    const ul = this.root.querySelector('#profiles')!, busy = this.deps.busy();
    let focus: HTMLElement | null = null;
    ul.replaceChildren();
    if (!this.profiles.length) {
      const li = document.createElement('li'); li.className = 'empty';
      li.textContent = t('No profiles yet. Create one in the game: it is saved in this browser.');
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
    try { r = await this.write(renameProfile(filesOf(this.deps.saveDir), PROFILES_DIR, from, to)); }
    catch (err) { this.renaming = null; this.msg(() => t('Renaming {name} stopped: {error}', { name: from, error: String(err) })); await this.refresh(); return; }
    if ('refused' in r) { ok.disabled = this.deps.busy(); say(r.refused); return; }
    this.renaming = null; this.focusName = to;
    this.msg(() => t('Renamed {from} to {to}.', { from, to }));
    this.deps.onChanged?.();
    await this.refresh();
  }

  private async doDownload() {
    const dl = this.deps.download;
    if (dl.running) { await dl.stop(); this.dataMsg(() => t('Preloading stopped. What it preloaded is kept: Preload the rest goes on from there.')); return; }
    if (this.deps.busy()) return;
    this.dataMsg('');
    dl.start();
  }

  private async doExport() {
    try {
      const e = await exportSaves(this.deps.saveDir);
      downloadBlob(e.zip as BlobPart, e.name, 'application/zip');
      this.msg(() => tn(e.files, 'Downloaded {name}: {n} file, {size}.', 'Downloaded {name}: {n} files, {size}.', { name: e.name, size: fmtBytes(e.zip.length) }) + (e.shotBytes > MAX_SHOTS
        ? ' ' + t('It holds {shots} of screenshots: a restore keeps the saves and settings and only the first {max} of screenshots.', { shots: fmtBytes(e.shotBytes), max: fmtBytes(MAX_SHOTS) }) : ''));
    } catch (err) { this.msg(() => t('The backup failed: {error}', { error: String(err) })); }
  }

  private async doImport(file: File) {
    if (this.deps.busy()) { this.msg(() => t('Quit the game first: restoring replaces the saves it is using.')); return; }
    let r: Restore;
    try { r = await readBackup(new Uint8Array(await file.arrayBuffer()), file.name); }
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
        : covers ? t('{file} holds DVD or magazine covers only: they come with their profile, in a zip of the NFS Underground 2 folder.', { file: name })
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
      p.textContent = tn(r.profiles.length, 'Adds the profile in {file} to this browser. The other profiles here, the game\'s options and settings.ini stay as they are.',
        'Adds the profiles in {file} to this browser. The other profiles here, the game\'s options and settings.ini stay as they are.', { file: file.name });
      items.push(t('Profiles: {names}', { names: r.profiles.map(n => {
        const covers = r.entries.filter(e => coverOf(e.path.split('/')[1])?.profile === n).length;
        return covers ? tn(covers, '{name} (with its cover shoot)', '{name} (with its {n} cover shoots)', { name: n }) : n;
      }).join(', ') }));
      if (replaced.length) items.push(tn(replaced.length, 'Replaces the profile {names} in this browser, its career and cover shoots too',
        'Replaces the profiles {names} in this browser, their careers and cover shoots too', { names: list(replaced) }));
      items.push(...restoreNotes(r));
    } else {
      p.textContent = tn(r.entries.length, 'Restoring replaces every profile, the game\'s options and settings.ini in this browser with the {n} file in {file}.',
        'Restoring replaces every profile, the game\'s options and settings.ini in this browser with the {n} files in {file}.', { file: file.name });
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
      this.msg(() => adding ? t('Adding stopped halfway: {error}. Add the profiles again before playing.', { error: String(err) })
        : t('Restoring stopped halfway: {error}. Restore the backup again before playing.', { error: String(err) }));
    }
    this.deps.onChanged?.();
    await this.refresh();
  }

  private async doClearCache() {
    if (this.deps.busy()) return;
    const dl = this.deps.download;
    if (!await this.deps.confirm(t('Delete the game data on this PC?'), (dl.running ? t('This stops preloading.') + ' ' : '') +
      t('The game will stream it from the server again the next time, which is slower once. Your saves are not touched.'), t('Delete game data'), true)) return;
    await dl.stop();
    await clearCache();
    await this.refresh();
    this.dataMsg(() => t('Game data on this PC deleted.'));
  }
}
