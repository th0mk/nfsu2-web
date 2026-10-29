// Cloud saves, the last part of the Saves section (index.html #vault): keyphrase saves (vault.ts) made, loaded from
// another browser, synced, settled when two browsers changed them, gone back a day, stopped or deleted.  Shown only
// where the server keeps them (vaultAvailable).  One box holds the keyphrase in every state: the one this browser
// syncs with (read only, with its download), or one typed in to load.  It shows the first 3 characters and dots for
// the rest until its eye shows it all.  Only the tab that owns the saves syncs (instance.ts); the link (the keyphrase
// and the version it has) is kept in this browser's localStorage, beside the saves it is for.
//
// When it syncs: the page opens (or this tab takes the saves over), Play (newer saves from another browser come in
// first, 4 s at most), 10 s after the game's last save of a burst, when the game ends, a few seconds after the Saves
// or Settings section changed the files, and Sync now.
//
// A conflict (both changed) shows both sides, their profiles and when they were saved, the newer one marked; each way
// out asks first, saying what it replaces.  Nothing syncs while one waits, so Play asks for it before the game starts
// (askAtPlay).  Before a cloud version replaces the saves here by the player's choice (Use the cloud's, Load, Go
// back), the saves here are kept in this browser for 30 days (replacedDir), and Put back brings them back (2026-10-27,
// issue 33: a player who took the cloud's lost 7 hours).

import { listFiles, readBytes, readText, removeSaveDir, writeFile } from './savefs';
import { backupItems, readVault, restoreVault } from './saves-ui';
import { downloadBlob } from './logs';
import { zip } from './zip';
import { kept, keeps, keptBy, profilesIn, readPhrase, sideOf, vaultAvailable, VaultError, VaultSync, type Link, type Remote, type SaveFile, type Side } from './vault';
import { codeOf, dateLocale, live, N, t, tn, tvars, type Msg } from './i18n';

export interface VaultPanelDeps {
  saveDir: string;
  /** this tab owns the saves (another tab of this browser only looks) */
  owner(): boolean;
  /** a game runs in this tab */
  playing(): boolean;
  /** a game runs or starts here, or another tab owns the saves: nothing writes into them from this section */
  busy(): boolean;
  confirm(title: string, body: Node | string, ok: string, danger?: boolean): Promise<boolean>;
  /** a question with two answers: 'ok', 'cancel', or '' (closed: Esc) */
  choose(title: string, body: Node | string, ok: string, cancel: string): Promise<string>;
  /** the saves here were replaced: the Saves list and the settings again */
  onRestored(): void;
  /** the passphrase linked here changed (made, loaded, stopped, deleted): what the claimed names say of this player */
  onLink?(): void;
  log(s: string): void;
}

/** how long `token()` waits for the section to know whether this server keeps cloud saves (`start`) */
const READY_WAIT_MS = 5000;

/** where a save folder's link is kept (localStorage) */
const linkKey = (saveDir: string) => `nfsu2-keyphrase:${saveDir}`;
/** where its games are counted until Play offers cloud saves, 'asked' after (localStorage) */
const offerKey = (saveDir: string) => `nfsu2-cloud-offer:${saveDir}`;
/** the games played in a save folder before its next Play offers cloud saves */
const OFFER_AFTER = 4;

/** where the saves a cloud version replaced are kept (OPFS, beside the save folder: a save folder's name has no '~'):
 *  saves.zip (as a keyphrase keeps them: readVault, restoreVault) and about.json (Replaced) */
const replacedDir = (saveDir: string) => `${saveDir}~replaced`;
/** how long they are kept */
const REPLACED_DAYS = 30;
interface Replaced { at: number; side: Side }

/** A save folder emptied (a test page's &fresh): its link goes too, or the page would sync an empty folder. */
export function forgetLink(saveDir: string) {
  try { localStorage.removeItem(linkKey(saveDir)); localStorage.removeItem(offerKey(saveDir)); } catch { /* storage off */ }
  void removeSaveDir(replacedDir(saveDir));
}

const today = (d: Date) => d.toDateString() === new Date().toDateString();
const clock = (d: Date) => d.toLocaleTimeString(dateLocale(), { hour: '2-digit', minute: '2-digit' });
/** "14:05" today, "21 Oct" before */
function short(at: number): string {
  const d = new Date(at);
  return today(d) ? clock(d) : d.toLocaleDateString(dateLocale(), { day: 'numeric', month: 'short' });
}
/** "today at 14:05", "21 Oct 2026, 14:05" */
function when(at: number): string {
  const d = new Date(at);
  return today(d) ? t('today at {time}', { time: clock(d) }) : `${d.toLocaleDateString(dateLocale(), { day: 'numeric', month: 'short', year: 'numeric' })}, ${clock(d)}`;
}
/** the box's dots: the first 3 characters, then one dot each */
export const masked = (s: string) => s.slice(0, 3) + '•'.repeat(Math.max(0, s.length - 3));

/** which side of a conflict is newer: both times known, more than a minute apart (the two clocks differ a little) */
function newer(here: Side, cloud: Side | null): 'here' | 'cloud' | null {
  if (here.saved === null || cloud?.saved == null || Math.abs(here.saved - cloud.saved) <= 60_000) return null;
  return here.saved > cloud.saved ? 'here' : 'cloud';
}

/** a conflict's two sides: where, its profiles, when they were saved, the newer one marked (cloud undefined: this
 *  browser's only) */
function sidesOf(here: Side, cloud?: Side | null): HTMLElement {
  const ul = document.createElement('ul'), n = cloud === undefined ? null : newer(here, cloud);
  ul.className = 'vault-sides';
  const row = (where: string, s: Side | null, isNewer: boolean) => {
    const li = document.createElement('li'), name = document.createElement('b'), what = document.createElement('span');
    name.textContent = where;
    const names = !s?.profiles ? '' : !s.profiles.length ? t('no profiles')
      : s.profiles.map(p => p.money === null ? p.name : `${p.name} $${p.money.toLocaleString(dateLocale())}`).join(', ');
    const at = s?.saved != null ? t('saved {when}', { when: when(s.saved) }) : '';
    what.textContent = [names, at].filter(Boolean).join(' · ');
    li.append(name, what);
    if (isNewer) li.append(Object.assign(document.createElement('em'), { textContent: t('Newer') }));
    li.classList.toggle('newer', isNewer);
    ul.append(li);
  };
  row(t('This browser'), here, n === 'here');
  if (cloud !== undefined) row(t('Cloud'), cloud, n === 'cloud');
  return ul;
}

const para = (text: string) => Object.assign(document.createElement('p'), { textContent: text });

const QUIT_FIRST = N('Quit the game first.');

export class VaultPanel {
  private sync: VaultSync | null = null;
  /** this tab owns the saves, and its settings are loaded: it may sync */
  private owned = false;
  /** the box shows dots after the first 3 characters */
  private hidden = true;
  /** the box holds the linked keyphrase (not one typed in) */
  private holdsLink = false;
  private timer: ReturnType<typeof setTimeout> | undefined;
  /** when check() last asked the server (a tab shown again asks at most once a minute) */
  private checked = 0;
  /** Play stopped waiting for the server: no version replaces the saves until the game ends (check()) */
  private held = false;
  /** the saves a cloud version replaced, kept here (null: none) */
  private replaced: Replaced | null = null;
  private readonly key: string;
  private readonly input: HTMLInputElement;

  /** `start` has found whether this server keeps cloud saves (and set up the sync, with the link kept here) */
  private ready: Promise<void>;
  private readyNow: () => void = () => {};
  /** the phrase linked when the section last showed (undefined: not shown yet): `onLink` when it changes */
  private shownLink: string | null | undefined = undefined;

  constructor(private root: HTMLElement, private d: VaultPanelDeps) {
    this.ready = new Promise<void>(r => { this.readyNow = r; });
    this.key = linkKey(d.saveDir);
    this.input = this.$<HTMLInputElement>('#vault-input');
    const on = (id: string, f: () => unknown) => { this.$<HTMLButtonElement>(id).onclick = () => void f(); };
    on('#vault-make', () => this.make());
    // shown, it is selected, and the linked keyphrase copied
    on('#vault-eye', async () => {
      this.hidden = !this.hidden;
      this.mask();
      if (this.hidden || !this.input.value) return;
      this.input.focus(); this.input.select();
      if (!this.sync?.link) return;
      try { await navigator.clipboard.writeText(this.input.value); this.msg(() => t('Copied.')); } catch { /* the selection is there to copy */ }
    });
    on('#vault-file', () => this.download());
    on('#vault-sync', () => this.syncNow());
    on('#vault-earlier', () => this.goBack());
    on('#vault-stop', () => this.stop());
    on('#vault-delete', () => this.remove());
    on('#vault-mine', () => this.keepMine());
    on('#vault-theirs', () => this.takeTheirs());
    on('#vault-putback', () => this.putBack());
    this.$<HTMLFormElement>('#vault-form').onsubmit = e => { e.preventDefault(); if (!this.sync?.link) void this.use(this.input.value); };
    // the dots follow the field's text and its scrolling
    for (const ev of ['input', 'scroll', 'keyup', 'select', 'pointerup', 'focus']) this.input.addEventListener(ev, () => this.mask());
  }

  private $<T extends HTMLElement = HTMLElement>(sel: string) { return this.root.querySelector<T>(sel)!; }
  /** the line under the buttons, drawn again when the page's language changes (a Msg); code: a failure's name */
  private msg(s: string | Msg, warn = false, code?: string) {
    const m = this.$('#vault-msg');
    live(m, typeof s === 'string' ? () => s : s, code); m.classList.toggle('warn', warn);
  }

  /** the box's dots (or not), and its eye */
  private mask() {
    const m = this.$('#vault-mask'), eye = this.$('#vault-eye');
    this.$('#vault-box').classList.toggle('masked', this.hidden);
    m.textContent = masked(this.input.value);
    m.scrollLeft = this.input.scrollLeft;
    eye.setAttribute('aria-pressed', String(!this.hidden));
    eye.setAttribute('aria-label', this.hidden ? t('Show passphrase') : t('Hide passphrase'));
  }

  private load(): Link | null {
    try { const t = localStorage.getItem(this.key); return t ? JSON.parse(t) as Link : null; } catch { return null; }
  }
  private save(l: Link | null) {
    try { if (l) localStorage.setItem(this.key, JSON.stringify(l)); else localStorage.removeItem(this.key); } catch { /* storage off: this page's link only */ }
  }

  /** a VaultSync on the link kept now (another tab may have changed it while this one only looked) */
  private makeSync() {
    const s = new VaultSync({ files: o => this.files(o), restore: zip => this.restore(zip), load: () => this.load(), save: l => this.save(l),
      log: line => this.d.log(line) });
    s.onChange = () => this.show();
    s.onTaken = r => {
      this.msg(() => t('Saves from your other browser came in ({when}).', { when: short(r.saved) }));
      this.d.log('[app] keyphrase: the saves another browser sent came in');
    };
    this.sync = s;
  }

  /** Shows the section when this server keeps keyphrase saves. */
  async start() {
    if (!await vaultAvailable().finally(() => setTimeout(() => this.readyNow(), 0))) return;
    this.makeSync();
    this.readyNow();
    this.root.hidden = false;
    await this.loadReplaced();
    this.show();
    if (this.owned) this.check();
  }

  /** This tab owns the saves now and has loaded its settings: it syncs from here on. */
  async ownerNow() {
    this.owned = true;
    if (!this.sync) return;
    this.makeSync();
    await this.loadReplaced();
    this.show();
    this.check();
  }

  /** newer saves from another browser in, then these out when they changed */
  private check() {
    const s = this.sync;
    if (!s?.link || !this.d.owner() || this.d.busy()) return;
    this.checked = Date.now();
    this.held = false;
    void s.pull().then(() => s.push()).catch(() => {});
  }

  /** The tab is shown again: newer saves from another browser, at most once a minute. */
  shown() { if (Date.now() - this.checked > 60_000) this.check(); }

  /** the files of the save folder as a keyphrase keeps them (was: as a link of that version counted them) */
  private async files(o: { was?: number } = {}): Promise<SaveFile[]> {
    const out: SaveFile[] = [], want = o.was ? keptBy(o.was) : keeps;
    for (const f of await listFiles(this.d.saveDir))
      if (want(f.path)) out.push({ path: f.path, data: await readBytes(this.d.saveDir, f.path), mtime: f.mtime });
    return kept(out, o.was);
  }

  /** a version of the keyphrase's (its zip) in place of the saves here, when no game uses them */
  private async restore(zip: Uint8Array) {
    if (this.d.playing() || this.held || !this.d.owner()) throw new VaultError(N('a game is running here: the other browser\'s saves come in after it'), 'VAULT-PLAYING');
    await restoreVault(this.d.saveDir, await readVault(zip));
    this.d.onRestored();
  }

  // ---- the saves a cloud version replaced ----

  /** what is kept, if anything; after REPLACED_DAYS it goes (by the tab that owns the saves) */
  private async loadReplaced() {
    const dir = replacedDir(this.d.saveDir);
    let r: Replaced | null = null;
    try { r = JSON.parse(await readText(dir, 'about.json') ?? 'null') as Replaced | null; } catch { /* unreadable: none */ }
    if (r && Date.now() - r.at > REPLACED_DAYS * 86400_000) {
      if (this.d.owner()) await removeSaveDir(dir);
      r = null;
    }
    this.replaced = r;
  }

  /** the saves here kept before a cloud version replaces them (none when they have no profile: nothing to lose) */
  private async keepReplaced(files?: SaveFile[]) {
    files ??= await this.files();
    if (!profilesIn(files).length) return;
    const dir = replacedDir(this.d.saveDir), r: Replaced = { at: Date.now(), side: sideOf(files) };
    await writeFile(dir, 'saves.zip', zip(files.map(f => ({ name: f.path, data: f.data, mtime: f.mtime }))));
    await writeFile(dir, 'about.json', JSON.stringify(r));
    this.replaced = r;
    this.d.log(`[app] keyphrase: the saves here kept before a cloud version replaces them (${r.side.profiles!.length} profiles)`);
  }

  /** the kept saves back in place of the ones here, which are kept in their place (Put back again swaps back) */
  private async putBack() {
    const r = this.replaced;
    if (!r) return;
    if (this.d.busy()) { this.msg(() => t(QUIT_FIRST), true); return; }
    const body = document.createElement('div');
    body.append(para(t('The saves this browser had before cloud saves replaced them ({when}) come back in place of the ones here. The saves here now are kept instead, so you can switch back.', { when: when(r.at) })),
      sidesOf(r.side));
    if (!await this.d.confirm(t('Put back the replaced saves?'), body, t('Put back'), true)) return;
    if (this.d.busy()) { this.msg(() => t(QUIT_FIRST), true); return; }
    try {
      const dir = replacedDir(this.d.saveDir), back = await readVault(await readBytes(dir, 'saves.zip'));
      await this.keepReplaced();
      await restoreVault(this.d.saveDir, back);
    } catch (e) {
      this.msg(() => t('Putting back stopped: {error}.', { error: e instanceof Error ? e.message : String(e) }), true, 'VAULT-PUT-BACK');
      return;
    } finally { this.d.onRestored(); }
    this.msg(this.sync?.link ? () => t('Put back. They go to the cloud now.') : () => t('Put back.'));
    this.d.log('[app] keyphrase: put back the saves a cloud version had replaced');
    // what the sync judged before is old now: synced again (a conflict asked again, with these saves)
    this.sync?.reopen();
    this.show();
    this.check();
  }

  // ---- when it syncs ----

  /** The game saved a profile: sent 10 s after the last save of a burst. */
  saved() { this.later(10_000); }
  /** The Saves or Settings section changed the files: sent in a few seconds. */
  changed() { this.later(3000); }
  private later(ms: number) {
    clearTimeout(this.timer);
    this.timer = setTimeout(() => { if (this.d.owner()) void this.sync?.push({ playing: this.d.playing() }).catch(() => {}); }, ms);
  }
  /** The game ended: newer saves from another browser in (when none came here), then what it saved goes. */
  gameEnded() {
    clearTimeout(this.timer);
    this.show();
    this.check();
  }
  /**
   * Before a game: newer saves from another browser come in first (the server asked for 4 s at most, 5 s in all with
   * a job of the section's before it).  True when they did.  Given up on, what comes later waits for the game's end.
   */
  async beforePlay(): Promise<boolean> {
    const s = this.sync;
    this.held = false;
    if (!s?.link || !this.d.owner() || this.d.playing()) return false;
    const before = s.link.rev;
    const done = await Promise.race([s.pull({ timeoutMs: 4000 }).then(() => true, () => true), new Promise<boolean>(r => setTimeout(r, 5000, false))]);
    if (!done) this.held = true;
    return !!s.link && s.link.rev !== before;
  }
  /**
   * Play while a conflict waits (nothing syncs until it is settled): the player picks the saves to keep before the
   * game starts, each way asked as in the section, or goes back to the page (Esc).  'taken': the cloud's came in (the
   * settings again); 'stop': no game.
   */
  async askAtPlay(): Promise<'play' | 'taken' | 'stop'> {
    const s = this.sync, st = s?.state;
    if (!s?.link || st?.s !== 'conflict' || !this.d.owner()) return 'play';
    const body = document.createElement('div');
    body.append(para(this.conflictText()), sidesOf(st.here, st.cloud), para(t('Cloud saves stay paused until you pick. Esc goes back without playing.')));
    const pick = await this.d.choose(t('Which saves do you want to keep?'), body, t('Keep this browser\'s'), t('Use the cloud\'s'));
    this.d.log(`[app] keyphrase: a conflict (${st.why}) at Play: ${pick === 'ok' ? 'keep this browser\'s' : pick === 'cancel' ? 'use the cloud\'s' : 'back to the page'}`);
    if (pick === 'ok') return await this.keepMine() ? 'play' : 'stop';
    if (pick === 'cancel') return await this.takeTheirs(true) ? 'taken' : 'stop';
    return 'stop';
  }
  /** A game started or ended, or this tab's ownership changed: the buttons again. */
  refresh() { this.show(); }

  // ---- offered at Play (shell.ts askCloud) ----

  /** A game started here: counted until Play offers cloud saves; with a keyphrase here, they are never offered. */
  played() {
    try {
      const k = offerKey(this.d.saveDir), v = localStorage.getItem(k);
      if (v !== 'asked') localStorage.setItem(k, this.sync?.link ? 'asked' : String((Number(v) || 0) + 1));
    } catch { /* storage off: no link is kept here either */ }
  }
  /** Play may offer cloud saves: this server keeps them, this tab owns the saves and has no keyphrase, OFFER_AFTER
   *  games were played here, and they were not offered before. */
  offerable(): boolean {
    const s = this.sync;
    if (!s || s.link || s.state.s === 'working' || !this.d.owner()) return false;
    try { const v = localStorage.getItem(offerKey(this.d.saveDir)); return v !== 'asked' && (Number(v) || 0) >= OFFER_AFTER; } catch { return false; }
  }
  /** Play offered them: not again in this browser. */
  offered() { try { localStorage.setItem(offerKey(this.d.saveDir), 'asked'); } catch { /* storage off */ } }
  /** Cloud saves turned on from Play's question: the keyphrase made, or why not (the section says it too). */
  async enable(): Promise<{ phrase: string } | { why: string }> {
    const l = await this.make() ? this.sync?.link : null, st = this.sync?.state;
    return l ? { phrase: l.phrase } : { why: st?.s === 'error' ? tvars(st.why.t, st.why.v) : '' };
  }
  /** the linked keyphrase's token (claim.ts: the claimed names), null without one */
  // (it waits for `start`: asked before the section knew its link, it answered none, and the line under Your name
  // said the player's own claimed name was another player's: a player's report, 2026-10-29)
  async token(): Promise<string | null> {
    await Promise.race([this.ready, new Promise(r => setTimeout(r, READY_WAIT_MS))]);
    return this.sync ? this.sync.token() : null;
  }
  /** a keyphrase is linked here */
  linked(): boolean { return !!this.sync?.link; }
  /** the line about the name this keyphrase claims (null: none) */
  showClaim(name: string | null) {
    const p = this.$<HTMLElement>('#vault-claim');
    p.hidden = !name || !this.sync?.link;
    p.textContent = name ? t('This passphrase also holds your claim on {name}.', { name }) : '';
  }
  /** the linked keyphrase as a file */
  download() { if (this.sync?.link) downloadBlob(`${this.sync.link.phrase}\n`, 'nfsu2-passphrase.txt', 'text/plain'); }

  // ---- the buttons ----

  /** true: made */
  private async make(): Promise<boolean> {
    if (!this.sync || !this.d.owner()) return false;
    try {
      await this.sync.create();
      this.hidden = false;
      this.show();
      this.msg(() => t('Passphrase made. Keep it safe: it is the only way back to these saves.'));
      this.d.log('[app] keyphrase: made one, the saves went to the server');
      return true;
    } catch { return false; /* the state says why */ }
  }

  private async use(text: string) {
    const s = this.sync;
    if (!s) return;
    const p = readPhrase(text);
    if ('bad' in p) { const w = p.why; this.msg(() => tvars(w.t, w.v), true); return; }
    if (this.d.busy()) { this.msg(() => t(QUIT_FIRST), true); return; }
    let got: Awaited<ReturnType<VaultSync['peek']>>;
    try { got = await s.peek(p.phrase); } catch { return; }
    if (!got) { this.msg(() => t('No cloud saves for this passphrase. Check it for typos.'), true); return; }
    if (await this.replace(got, t('Load cloud saves?'),
      t('They replace the profiles and settings.ini here with the ones saved {when}. Screenshots stay, and so do this device\'s graphics, frame rate, sound output and controller settings.', { when: when(got.remote.saved) }), t('Load'))) {
      await s.adopt(p.phrase, got.remote).catch(() => {});
      this.msg(() => t('Loaded. This browser syncs from now on.'));
      this.d.log('[app] keyphrase: loaded its saves, this browser syncs with it');
    }
  }

  /** what a version brings, asked; then it replaces the saves here.  False: nothing changed. */
  private async replace(got: { zip: Uint8Array; remote: Remote }, title: string, text: string, ok: string): Promise<boolean> {
    let r: Awaited<ReturnType<typeof readVault>>;
    try { r = await readVault(got.zip); } catch (e) { this.msg(() => t('These saves cannot be read: {error}.', { error: e instanceof Error ? e.message : String(e) }), true, 'VAULT-UNREADABLE'); return false; }
    const body = document.createElement('div'), p = document.createElement('p'), ul = document.createElement('ul');
    p.textContent = text;
    for (const t of backupItems(r, true)) { const li = document.createElement('li'); li.textContent = t; ul.append(li); }
    body.append(p, ul);
    if (!await this.d.confirm(title, body, ok, (await listFiles(this.d.saveDir)).some(f => keeps(f.path)))) { this.msg(''); return false; }
    if (this.d.busy()) { this.msg(() => t(QUIT_FIRST), true); return false; }
    try { await this.keepReplaced(); }
    catch (e) { this.msg(() => t('The saves here could not be kept, so nothing was replaced: {error}.', { error: e instanceof Error ? e.message : String(e) }), true, 'VAULT-KEEP-COPY'); return false; }
    try { await restoreVault(this.d.saveDir, r); }
    catch (e) { this.msg(() => t('Loading stopped halfway: {error}. Load again before playing.', { error: e instanceof Error ? e.message : String(e) }), true, 'VAULT-LOAD-HALFWAY'); return false; }
    finally { this.d.onRestored(); }
    return true;
  }

  private async syncNow() {
    const s = this.sync;
    if (!s?.link || this.d.busy()) return;
    this.msg('');
    try { await s.pull(); await s.push(); } catch { return; /* the state says why */ }
    // the server has no saves for the keyphrase: the ones here go there again, if the player says so
    if (!s.link?.gone || !await this.d.confirm(t('Send these saves to the cloud again?'),
      t('The cloud has no saves for this passphrase now. The saves here go there under the same passphrase, and your other browsers with it sync again.'), t('Send'))) return;
    try { await s.resend(); this.msg(() => t('Sent. This browser syncs again.')); this.d.log('[app] keyphrase: its saves were gone from the server, sent again'); }
    catch { /* the state says why */ }
  }

  private async goBack() {
    const s = this.sync, l = s?.link;
    if (!s || !l?.earlier) return;
    if (this.d.busy()) { this.msg(() => t(QUIT_FIRST), true); return; }
    let got: Awaited<ReturnType<VaultSync['peek']>>;
    try { got = await s.peek(l.phrase, true); } catch { return; }
    if (!got) { this.msg(() => t('The cloud has no earlier copy now.'), true); return; }
    const at = got.remote.saved;
    if (await this.replace(got, t('Go back to {when}?', { when: short(at) }),
      t('The cloud copy from {when} replaces the saves here and becomes the latest. Progress since then is lost. Screenshots stay.', { when: when(at) }), t('Go back'))) {
      this.msg(() => t('Back to {when}.', { when: short(at) }));
      this.d.log('[app] keyphrase: went back to the copy of an earlier day');
      await s.push().catch(() => {});
    }
  }

  private async stop() {
    const s = this.sync;
    if (!s?.link) return;
    if (!await this.d.confirm(t('Stop syncing here?'), t('Your saves stay here and in the cloud. To sync again, you need the passphrase.'), t('Stop syncing'))) return;
    const phrase = s.link.phrase;
    await s.forget().catch(() => {});
    // the keyphrase stays in the box, to keep or to load again (the job's end showed the box empty)
    this.holdsLink = false;
    this.input.value = phrase;
    this.show();
    this.msg(() => t('Stopped syncing. Your saves stay here and in the cloud.'));
  }

  private async remove() {
    const s = this.sync;
    if (!s?.link) return;
    if (!await this.d.confirm(t('Delete your cloud saves?'), t('They are deleted from the server now, with the earlier copy. This browser keeps its saves; other browsers stop syncing.'),
      t('Delete'), true)) return;
    try {
      await s.remove();
      this.msg(() => t('Deleted from the cloud. Your saves stay here.'));
      this.d.log('[app] keyphrase: its saves were deleted from the server');
    } catch { /* the state says why */ }
  }

  /** a conflict settled for the saves here, once the player says so, told what it replaces (with no profile here,
   *  every browser that syncs with the keyphrase loses its profiles).  True: settled. */
  private async keepMine(): Promise<boolean> {
    const s = this.sync, st = s?.state;
    if (!s?.link || st?.s !== 'conflict') return false;
    const n = s.link.profiles, body = document.createElement('div'), cloudAt = st.cloud?.saved;
    if (st.why === 'empty') body.append(para(tn(n, 'This browser has no profiles. The cloud\'s profile is deleted, here and in every browser that syncs with this passphrase.',
      'This browser has no profiles. The cloud\'s {n} profiles are deleted, here and in every browser that syncs with this passphrase.')));
    else body.append(para(cloudAt != null ? t('They replace the cloud\'s saves from {when}, here and in every browser that syncs with this passphrase.', { when: when(cloudAt) })
      : t('They replace the cloud\'s saves, here and in every browser that syncs with this passphrase.')));
    body.append(sidesOf(st.here, st.cloud));
    const cloudNewer = newer(st.here, st.cloud) === 'cloud';
    if (cloudNewer) body.append(Object.assign(para(t('The cloud\'s saves are newer: what was played on them since is lost.')), { className: 'warn' }));
    if (!await this.d.confirm(t('Keep this browser\'s saves?'), body, t('Keep this browser\'s'), st.why === 'empty' || cloudNewer)) return false;
    await s.keepMine().catch(() => {});
    return s.state.s === 'synced';
  }

  /** a conflict settled for the keyphrase's version, once the player says so, told what it replaces; the saves here
   *  are kept first (Put back).  True: settled.  atPlay: Play asks (askAtPlay), its game not started yet. */
  private async takeTheirs(atPlay = false): Promise<boolean> {
    const s = this.sync, st = s?.state, busy = () => !atPlay && this.d.busy();
    if (!s?.link || st?.s !== 'conflict') return false;
    if (busy()) { this.msg(() => t(QUIT_FIRST), true); return false; }
    const body = document.createElement('div'), cloudAt = st.cloud?.saved;
    body.append(para(cloudAt != null ? t('The cloud\'s saves from {when} replace the ones here.', { when: when(cloudAt) }) : t('The cloud\'s saves replace the ones here.')),
      sidesOf(st.here, st.cloud));
    if (cloudAt != null && newer(st.here, st.cloud) === 'here')
      body.append(Object.assign(para(t('This browser\'s saves are newer: the progress since {when} is replaced.', { when: when(cloudAt) })), { className: 'warn' }));
    if (st.why === 'bare') body.append(Object.assign(para(t('They have no profiles: the profiles here are deleted.')), { className: 'warn' }));
    if (st.here.profiles?.length) body.append(para(tn(REPLACED_DAYS, 'A copy of the saves here stays in this browser for {n} day: Put back under Cloud saves brings it back.',
      'A copy of the saves here stays in this browser for {n} days: Put back under Cloud saves brings it back.')));
    if (!await this.d.confirm(t('Use the cloud\'s saves?'), body, t('Use the cloud\'s'), true)) return false;
    if (busy()) { this.msg(() => t(QUIT_FIRST), true); return false; }
    try { await this.keepReplaced(); }
    catch (e) { this.msg(() => t('The saves here could not be kept, so nothing was replaced: {error}.', { error: e instanceof Error ? e.message : String(e) }), true, 'VAULT-KEEP-COPY'); return false; }
    await s.takeTheirs().catch(() => {});
    this.show();
    return s.state.s === 'synced';
  }

  // ---- what it shows ----

  private show() {
    const s = this.sync;
    if (!s) return;
    const l = s.link, st = s.state, owner = this.d.owner(), busy = this.d.busy();
    const phrase = l?.phrase ?? null;
    if (phrase !== this.shownLink) {
      const first = this.shownLink === undefined;
      this.shownLink = phrase;
      if (!first) this.d.onLink?.();
    }
    const working = st.s === 'working';
    // beside the heading: Enable cloud saving, or what it is doing, or when it last synced
    this.$('#vault-make').hidden = !!l || working;
    const note = this.$('#vault-when');
    note.hidden = !l && !working;
    note.textContent = st.s === 'working' ? `${t(st.what)}…` : l ? t('Synced {when}', { when: short(l.saved) }) : '';
    // the box: the linked keyphrase (read only, with its download), or one to type in and load
    if (l) { this.input.value = l.phrase; this.holdsLink = true; }
    else if (this.holdsLink) { this.input.value = ''; this.holdsLink = false; }
    this.input.readOnly = !!l;
    this.$('#vault-file').hidden = !l;
    this.$('#vault-load').hidden = !!l;
    this.$('#vault-about').hidden = !!l;
    this.mask();
    this.$('#vault-on').hidden = !l;
    const earlier = this.$<HTMLButtonElement>('#vault-earlier');
    earlier.hidden = !l?.earlier;
    if (l?.earlier) earlier.textContent = t('Go back to {when}', { when: short(l.earlier) });
    const conflict = st.s === 'conflict' ? st : null;
    this.$('#vault-conflict').hidden = !conflict || !l;
    this.$('#vault-sides').replaceChildren(...conflict && l ? [sidesOf(conflict.here, conflict.cloud)] : []);
    // the saves a cloud version replaced, kept here
    const kept = this.replaced;
    this.$('#vault-replaced').hidden = !kept;
    if (kept) this.$('#vault-replaced-text').textContent = t('Kept in this browser: the saves replaced {when}.', { when: when(kept.at) });
    for (const id of ['#vault-make', '#vault-mine', '#vault-delete', '#vault-stop']) this.$<HTMLButtonElement>(id).disabled = !owner || working;
    for (const id of ['#vault-sync', '#vault-earlier', '#vault-theirs', '#vault-load', '#vault-putback']) this.$<HTMLButtonElement>(id).disabled = busy || working;
    // under it, only what needs the player: another tab syncs, a failure, a conflict, the keyphrase gone
    const failed = st.s === 'error' ? tvars(st.why.t, st.why.v) : '';
    const status = this.$('#vault-status');
    status.textContent = !owner && l ? t('Another tab is syncing these saves.')
      : st.s === 'error' ? (l ? t('Sync failed: {error}. It tries again at the next save.', { error: failed }) : t('{Error}.', { Error: `${failed[0]?.toUpperCase() ?? ''}${failed.slice(1)}` }))
      : st.s === 'gone' ? t('The cloud has no saves for this passphrase now: deleted in another browser, unused for 2 years, or lost on the server. Nothing is sent; Sync now looks again and can send these saves there.')
      : conflict && l ? `${this.conflictText()} ${t('Cloud saves are paused until you pick.')}`
      : '';
    if (st.s === 'error' && st.code && (owner || !l)) status.append(codeOf(st.code));
  }

  /** a conflict in a line (the section's, Play's question) */
  private conflictText(): string {
    const l = this.sync?.link, st = this.sync?.state;
    if (st?.s !== 'conflict' || !l) return '';
    const n = l.profiles, theirs = st.theirs ? short(st.theirs.saved) : '';
    return st.why === 'empty' ? tn(n, 'No profiles here, but the cloud has one. Nothing was sent.', 'No profiles here, but the cloud has {n}. Nothing was sent.')
      : st.why === 'bare' ? (theirs ? tn(n, 'The cloud\'s saves from {when} have no profiles, but this browser has one. Keep which?', 'The cloud\'s saves from {when} have no profiles, but this browser has {n}. Keep which?', { when: theirs })
        : tn(n, 'The cloud\'s saves have no profiles, but this browser has one. Keep which?', 'The cloud\'s saves have no profiles, but this browser has {n}. Keep which?'))
      : st.why === 'older' ? (theirs ? t('The cloud has older saves ({when}) than this browser\'s ({mine}): the server lost the newer ones. Keep which?', { when: theirs, mine: short(l.saved) })
        : t('The cloud has older saves than this browser\'s ({mine}): the server lost the newer ones. Keep which?', { mine: short(l.saved) }))
      : theirs ? t('Changed here and in another browser ({when}). Keep which?', { when: theirs }) : t('Changed here and in another browser. Keep which?');
  }
}
