// Cloud saves, the last part of the Saves section (index.html #vault): keyphrase saves (vault.ts) made, loaded from
// another browser, synced, settled when two browsers changed them, gone back a day, stopped or deleted.  Shown only
// where the server keeps them (vaultAvailable).  One box holds the keyphrase in every state: the one this browser
// syncs with (read only, with its download), or one typed in to load.  It shows the first 3 characters and dots for
// the rest until its eye shows it all, for 30 s.  Only the tab that owns the saves syncs (instance.ts); the link (the keyphrase
// and the version it has) is kept in this browser's localStorage, beside the saves it is for.  Where the browser has
// passwords for sites, it is asked to keep the keyphrase too, which outlives the site's data, and a browser with no
// link and no profile is asked for the one it keeps, whose saves the page offers (pwkeep.ts, `passwords`).
//
// When it syncs: the page opens (or this tab takes the saves over), Play (newer saves from another browser come in
// first, 4 s at most), 10 s after the game's last save of a burst, when the game ends, a few seconds after the Saves
// tab or a setting changed the files, and Sync now.
//
// A conflict (both changed) shows both sides, their profiles and when they were saved, the newer one marked; each way
// out asks first, saying what it replaces.  Nothing syncs while one waits, so Play asks for it before the game starts
// (askAtPlay).  Before a cloud version replaces the saves here by the player's choice (Use the cloud's, Load, Go
// back), the saves here are kept in this browser for 30 days (replacedDir), and Put back brings them back (2026-10-27,
// issue 33: a player who took the cloud's lost 7 hours).

import { exists, filesOf, listFiles, listProfiles, readBeside, readBytes, readText, removeSaveDir, UndoPending, writeBeside, writeFile, type BesideFile } from './savefs';
import { backupItems, leftOut, profilesComing, readVault, refillSaves, repairNotes, restoreVault, type Restore } from './saves-ui';
import { downloadBlob } from './logs';
import { unzip, zip } from './zip';
import { contentHash, kept, keeps, keptBy, keysOf, profilesIn, readPhrase, sideOf, vaultAvailable, VaultError, VaultSync, type Link, type Remote, type SaveFile, type Side, type SyncState } from './vault';
import { codeOf, dateLocale, listOf, live, N, t, tn, tvars, type Msg } from './i18n';
import { canKeep, keepPhrase, keptPhrase, markOf } from './pwkeep';

export interface VaultPanelDeps {
  saveDir: string;
  /** this tab owns the saves (another tab of this browser only looks) */
  owner(): boolean;
  /** a game runs in this tab */
  playing(): boolean;
  /** a game runs or starts here, or another tab owns the saves: nothing writes into them from this section */
  busy(): boolean;
  confirm(title: string, body: Node | string, ok: string, danger?: boolean): Promise<boolean>;
  /** a question with two answers: 'ok', 'cancel', or '' (closed: Esc); ok and cancel '': none of the dialog's own
   *  buttons (the body's buttons answer, by their value) and nothing focused */
  /** danger: ok red (what it does cannot be undone); cancelDanger: cancel red (the safe one focused) */
  choose(title: string, body: Node | string, ok: string, cancel: string, danger?: boolean, cancelDanger?: boolean): Promise<string>;
  /** the saves here were replaced: the Saves list and the settings again */
  onRestored(): void;
  /** the passphrase linked here changed (made, loaded, stopped, deleted): what the claimed names say of this player */
  onLink?(): void;
  log(s: string): void;
}

/** where a save folder's link is kept (localStorage) */
const linkKey = (saveDir: string) => `nfsu2-keyphrase:${saveDir}`;
/** how long Play waits for the section to know whether this server keeps cloud saves (`started`) */
const READY_WAIT_MS = 5000;
/** how often Play offered cloud saves for it, and when last (localStorage, Offer as JSON) */
const offerKey = (saveDir: string) => `nfsu2-cloud-offer:${saveDir}`;
/** which phrase the browser was asked to keep in its passwords (pwkeep.ts markOf); 'asked' once the browser was asked
 *  for one with its chooser and gave none, 'no:<mark>' when the player turned down the saves of the one it gave
 *  (localStorage) */
const keptKey = (saveDir: string) => `nfsu2-keyphrase-kept:${saveDir}`;
/** Play offers cloud saves again this long after a Not now, and this often at most (2026-10-31: it was once, at the
 *  fifth Play) */
const OFFER_AGAIN_MS = 7 * 86400_000;
export const OFFER_TIMES = 3;
/** and once more, whatever the count and the week, once more than LATE_GAMES games were started here and more than
 *  LATE_MS played (2026-11-03): a player who keeps playing on saves kept only here */
const LATE_GAMES = 12, LATE_MS = 3 * 3600_000;
/** asked: the offers counted against OFFER_TIMES; at: the last offer; late: the once-more one was made (or an offer
 *  made past its numbers stood for it) */
interface Offer { asked: number; at: number; late?: boolean }
const lateDue = (o: Offer, games: number, played: number) => !o.late && games > LATE_GAMES && played > LATE_MS;
const regularDue = (o: Offer) => o.asked < OFFER_TIMES && Date.now() - o.at >= OFFER_AGAIN_MS;
/** the offers so far (before 2026-10-31 the key held a count of games, then 'asked' after the one offer) */
function readOffer(v: string | null): Offer {
  if (v === 'asked') return { asked: 1, at: 0 };
  try {
    const o = JSON.parse(v ?? '') as Partial<Offer> | null;
    if (typeof o?.asked === 'number') return { asked: o.asked, at: Number(o.at) || 0, late: o.late === true };
  }
  catch { /* a count, or nothing */ }
  return { asked: 0, at: 0 };
}

/** where the saves a cloud version replaced are kept (OPFS, beside the save folder: a save folder's name has no '~'):
 *  saves.zip (as a keyphrase keeps them: readVault, restoreVault) and about.json (Replaced) */
const replacedDir = (saveDir: string) => `${saveDir}~replaced`;
/** the save folder's mark (VaultDeps.folder): a file beside it, which the browser deletes with the folder */
const markFile = (saveDir: string) => `${saveDir}~folder`;
/** how long they are kept */
const REPLACED_DAYS = 30;
/** how long the server may take to give the cloud's saves back into a save folder the browser deleted during a game
 *  (refill; the game waits 12 s at most for them: io-worker.ts recover) */
const REFILL_WAIT_MS = 8000;
interface Replaced { at: number; side: Side }

/** A save folder emptied (a test page's &fresh): its link goes too, or the page would sync an empty folder. */
export function forgetLink(saveDir: string) {
  try { for (const k of [linkKey, offerKey, keptKey]) localStorage.removeItem(k(saveDir)); } catch { /* storage off */ }
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

/** a side's profiles and their money ("ALEX $123,450, BOB $500"), a damaged one said so */
const namesOf = (s: Side | null) => !s?.profiles ? '' : !s.profiles.length ? t('no profiles')
  : s.profiles.map(p => p.whole === false ? t('{name} (damaged)', { name: p.name })
    : p.money === null ? p.name : `${p.name} $${p.money.toLocaleString(dateLocale())}`).join(', ');
/**
 * What one side of a conflict loses when the other is kept: its whole profiles the kept side does not have whole
 * (gone), and, when it is the newer side, those the kept side has otherwise (changed: what was played on them since).
 * stays: profiles that stay anyway ('drops': Use the cloud's keeps the ones the cloud's version cannot bring).  A side
 * whose profiles are not known loses nothing known.
 */
function loses(side: Side | null, kept: Side | null, newerSide: boolean, stays: string[] = []): { gone: string[]; changed: string[] } {
  if (!side?.profiles || !kept?.profiles) return { gone: [], changed: [] };
  const low = (n: string) => n.toLowerCase(), stay = new Set(stays.map(low));
  const keptWhole = new Set(kept.profiles.filter(p => p.whole !== false).map(p => low(p.name)));
  const whole = side.profiles.filter(p => p.whole !== false && !stay.has(low(p.name)));
  return { gone: whole.filter(p => !keptWhole.has(low(p.name))).map(p => p.name),
    changed: !newerSide ? [] : whole.filter(p => keptWhole.has(low(p.name)) && side.sums?.[low(p.name)] !== kept.sums?.[low(p.name)]).map(p => p.name) };
}
type Conflict = Extract<SyncState, { s: 'conflict' }>;
/** what Keep this browser's loses of the cloud's (on 'drops' also the profiles of the cloud's this browser cannot take
 *  in: Keep sends the saves here without them) */
function keepLoses(st: Conflict) {
  const l = loses(st.cloud, st.here, newer(st.here, st.cloud) === 'cloud');
  const here = new Set((st.here.profiles ?? []).filter(p => p.whole !== false).map(p => p.name.toLowerCase()));
  for (const n of st.stays ?? []) if (!here.has(n.toLowerCase()) && !l.gone.includes(n)) l.gone.push(n);
  return l;
}
/** what Use the cloud's loses of this browser's */
const useLoses = (st: Conflict) => loses(st.here, st.cloud, newer(st.here, st.cloud) === 'here', st.stays);
/** Keep this browser's red: it deletes or replaces something of the cloud's (or, the cloud's not known, the saves here
 *  may be broken) */
function keepDanger(st: Conflict): boolean {
  const l = keepLoses(st);
  return l.gone.length > 0 || l.changed.length > 0 || !st.cloud?.profiles && (st.why === 'empty' || st.why === 'wiped' || st.why === 'fewer');
}
/** Use the cloud's red: it deletes or replaces something of this browser's */
function useDanger(st: Conflict): boolean {
  const l = useLoses(st);
  return st.why === 'bare' || l.gone.length > 0 || l.changed.length > 0;
}
/** what Keep this browser's deletes of the cloud's, said (null: nothing) */
function lostNote(st: Conflict): HTMLElement | null {
  const lost = keepLoses(st).gone;
  return !lost.length ? null : Object.assign(para(tn(lost.length, 'The cloud\'s {names} is deleted, here and in every browser that syncs with this passphrase.',
    'The cloud\'s {names} are deleted, here and in every browser that syncs with this passphrase.', { names: listOf(lost) })), { className: 'warn' });
}

/** a conflict's two sides: where, its profiles, when they were saved, the newer one marked (cloud undefined: this
 *  browser's only) */
function sidesOf(here: Side, cloud?: Side | null): HTMLElement {
  const ul = document.createElement('ul');
  ul.className = 'vault-sides';
  ul.append(...cardsOf(here, cloud));
  return ul;
}
/** a conflict's cards: this browser's, and the cloud's (cloud undefined: none) */
function cardsOf(here: Side, cloud?: Side | null): HTMLElement[] {
  const n = cloud === undefined ? null : newer(here, cloud);
  const card = (where: string, s: Side | null, isNewer: boolean) => {
    const li = document.createElement('li'), name = document.createElement('b'), what = document.createElement('span');
    name.textContent = where;
    const at = s?.saved != null ? t('saved {when}', { when: when(s.saved) }) : '';
    what.textContent = [namesOf(s), at].filter(Boolean).join(' · ');
    li.append(name, what);
    if (isNewer) li.append(Object.assign(document.createElement('em'), { textContent: t('Newer') }));
    li.classList.toggle('newer', isNewer);
    return li;
  };
  return [card(t('This browser'), here, n === 'here'), ...cloud !== undefined ? [card(t('Cloud'), cloud, n === 'cloud')] : []];
}

/**
 * A conflict's two sides as the choice: each card with its own button under it, This browser's with Keep this
 * browser's, the cloud's with Use the cloud's (stacked on a narrow screen, each under its card).  The two look alike,
 * plain: neither is the one to press, as either replaces saves (its question after says what goes).
 */
function choiceOf(here: Side, cloud: Side | null, keep: HTMLElement, use: HTMLElement): HTMLElement {
  const div = document.createElement('div'), cards = cardsOf(here, cloud);
  div.className = 'vault-choice';
  for (const [card, btn] of [[cards[0], keep], [cards[1], use]] as const) {
    if (!card) continue;
    const col = document.createElement('div'), ul = document.createElement('ul');
    col.className = 'vault-pick'; ul.className = 'vault-sides';
    btn.className = 'btn';
    ul.append(card); col.append(ul, btn); div.append(col);
  }
  return div;
}

/** the saves here as Play's cloud saves question shows them, in a conflict side's box: the profiles, their money, and
 *  how long was played in this browser (playtime.ts; under a minute: not said) */
function stakeOf(here: Side, playedMs: number): HTMLElement {
  const ul = document.createElement('ul'), li = document.createElement('li'), name = document.createElement('b'), what = document.createElement('span');
  ul.className = 'vault-sides';
  name.textContent = t('This browser');
  const h = Math.floor(playedMs / 3600_000), m = Math.floor(playedMs / 60_000);
  const played = h ? tn(h, '{n} hour played', '{n} hours played') : m ? tn(m, '{n} minute played', '{n} minutes played') : '';
  what.textContent = [namesOf(here), played].filter(Boolean).join(' · ');
  li.append(name, what);
  ul.append(li);
  return ul;
}

const para = (text: string) => Object.assign(document.createElement('p'), { textContent: text });

const QUIT_FIRST = N('Quit the game first.');
/** how long the box shows the whole passphrase (the eye, or Make) before its dots come back; typing in it starts the
 *  wait again.  (2026-11-03: screenshots of the Saves tab, sent for help, had the phrase left shown in them.) */
const SHOWN_MS = 30_000;

export class VaultPanel {
  private sync: VaultSync | null = null;
  /** this tab owns the saves, and its settings are loaded: it may sync */
  private owned = false;
  /** the box shows dots after the first 3 characters */
  private hidden = true;
  /** shown, the dots come back at this (SHOWN_MS) */
  private hideTimer: ReturnType<typeof setTimeout> | undefined;
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

  /** start() at work (asking the server, then setting up the sync), null: not now */
  private starting: Promise<void> | null = null;
  /** `token` before `start`: the kept link's phrase and its token */
  private early: { phrase: string; token: Promise<string> } | null = null;
  /** the name this keyphrase claims (showClaim) */
  private claimed: string | null = null;
  /** the phrase linked when the section last showed (undefined: not shown yet): `onLink` when it changes */
  private shownLink: string | null | undefined = undefined;
  /** this page looked at the browser's passwords (`passwords`) */
  private pwLooked = false;

  constructor(private root: HTMLElement, private d: VaultPanelDeps) {
    this.key = linkKey(d.saveDir);
    this.input = this.$<HTMLInputElement>('#vault-input');
    const on = (id: string, f: () => unknown) => { this.$<HTMLButtonElement>(id).onclick = () => void f(); };
    on('#vault-make', () => this.make());
    // shown, it is selected, and the linked keyphrase copied
    on('#vault-eye', async () => {
      this.reveal(this.hidden);
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
    this.input.addEventListener('input', () => { if (!this.hidden) this.reveal(true); });
  }

  /** the whole passphrase shown (for SHOWN_MS), or its dots */
  private reveal(on: boolean) {
    this.hidden = !on;
    clearTimeout(this.hideTimer);
    if (on) this.hideTimer = setTimeout(() => this.reveal(false), SHOWN_MS);
    this.mask();
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
    // a password field while hidden, for the browser's passwords (pwkeep.ts); the mask still draws it
    const type = this.hidden ? 'password' : 'text';
    if (this.input.type !== type) this.input.type = type;
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
    const s = new VaultSync({ files: o => this.files(o), restore: (zip, still) => this.restore(zip, still), named: files => profilesComing(files), load: () => this.load(), save: l => this.save(l),
      log: line => this.d.log(line), leftOut: files => leftOut(files),
      folder: () => readBeside(markFile(this.d.saveDir)), mark: id => writeBeside(markFile(this.d.saveDir), id) });
    s.onChange = () => this.show();
    s.onTaken = r => {
      const mended = this.mended;
      this.mended = { repaired: [] };
      this.msg(() => [t('Saves from your other browser came in ({when}).', { when: short(r.saved) }), ...repairNotes(mended)].join(' '));
      this.d.log(`[app] keyphrase: the saves another browser sent came in${mended.repaired.length ? `, ${mended.repaired.length} profile(s) with the name inside mended` : ''}`);
    };
    this.sync = s;
  }

  /** Shows the section when this server keeps keyphrase saves (asked again when the server answers again, after it
   *  did not as the page started: once shown, nothing more).  Called while it runs: that same run. */
  start(): Promise<void> {
    if (this.sync) return Promise.resolve();
    return this.starting ??= this.begin().finally(() => { this.starting = null; });
  }
  private async begin() {
    const on = await vaultAvailable();
    if (!on || this.sync) return;
    this.makeSync();
    this.root.hidden = false;
    await this.loadReplaced();
    this.show();
    if (this.owned) { this.check(); void this.passwords(); }
  }
  /** Play: a `start` still asking the server is waited for (READY_WAIT_MS at most), so Play's questions and its pull
   *  of newer saves (askAtPlay, offerable, beforePlay) have the sync.  (Until 2026-11-02 the claimed names' `token`
   *  waited for it, and Play with it.) */
  async started(): Promise<void> {
    const s = this.starting;
    if (s) await Promise.race([s.catch(() => {}), new Promise(r => setTimeout(r, READY_WAIT_MS))]);
  }

  /** This tab owns the saves now and has loaded its settings: it syncs from here on. */
  async ownerNow() {
    this.owned = true;
    if (!this.sync) return;
    this.makeSync();
    await this.loadReplaced();
    this.show();
    this.check();
    void this.passwords();
  }

  /**
   * Once a page, in the tab that owns the saves, where the browser has passwords for sites (pwkeep.ts): a linked
   * passphrase it was never asked to keep is offered to it (links made before 2026-10-31); with none linked and no
   * profile here (this browser's data was cleared, or it is another device of the player's), the browser is asked for
   * the one it keeps, its own chooser allowed the first time (it shows nothing where it keeps none; once it gave none,
   * only what it gives without a question), and that one's saves are offered, asked first.  Turned down, or without
   * saves on the server, that phrase is not offered again in this browser.
   */
  private async passwords() {
    const s = this.sync;
    if (this.pwLooked || !s || !canKeep()) return;
    this.pwLooked = true;
    const k = keptKey(this.d.saveDir);
    const mark = (v: string) => { try { localStorage.setItem(k, v); } catch { /* storage off: asked again at the next page */ } };
    let was: string | null = null;
    try { was = localStorage.getItem(k); } catch { /* storage off */ }
    if (s.link) { if (was !== markOf(s.link.phrase)) this.keep(s.link.phrase); return; }
    if ((await listProfiles(this.d.saveDir)).length) return;
    const ask = was === null, phrase = await keptPhrase(ask), p = phrase ? readPhrase(phrase) : null;
    if (!p || 'bad' in p) {
      if (ask) { mark('asked'); this.d.log(`[app] keyphrase: the browser gave ${phrase ? 'no passphrase' : 'none'} when asked: only taken without a question from now on`); }
      return;
    }
    if (was === `no:${markOf(p.phrase)}` || s.link || this.d.busy()) return;
    let got: Awaited<ReturnType<VaultSync['peek']>>;
    try { got = await s.peek(p.phrase); } catch { return; }
    if (s.link || this.d.busy()) return;
    if (!got) { mark(`no:${markOf(p.phrase)}`); this.d.log('[app] keyphrase: the browser keeps one, but the server has no saves for it'); return; }
    this.d.log('[app] keyphrase: the browser keeps one, and no profile is here: its saves offered');
    if (await this.replace(got, t('Load cloud saves?'), `${t('Your browser\'s passwords have the passphrase of your cloud saves.')} ${
      t('They replace the profiles and settings.ini here with the ones saved {when}. Screenshots and this device\'s graphics, sound and controller settings stay.', { when: when(got.remote.saved) })}`, t('Load'),
      false)) {                           // no profile here to lose (the page's own settings.ini at most)
      await s.adopt(p.phrase, got.remote).catch(() => {});
      mark(markOf(p.phrase));             // the browser has it: not asked to keep it again
      this.msg(() => t('Loaded. This browser syncs from now on.'));
      this.d.log('[app] keyphrase: loaded the saves of the one the browser keeps, this browser syncs with it');
    } else {
      mark(`no:${markOf(p.phrase)}`);
      this.d.log('[app] keyphrase: the saves of the one the browser keeps were not loaded');
    }
  }

  /** the browser asked to keep the phrase in its passwords (its own "Save password?"), once per phrase */
  private keep(phrase: string) {
    void keepPhrase(phrase).then(ok => {
      if (!ok) return;
      try { localStorage.setItem(keptKey(this.d.saveDir), markOf(phrase)); } catch { /* asked again at the next page */ }
      this.d.log('[app] keyphrase: the browser was asked to keep it in its passwords');
    });
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

  /** the profiles whose name inside the last version taken by itself mended (said with onTaken's line) */
  private mended: Pick<Restore, 'repaired'> = { repaired: [] };
  /** the copy of the saves here Use the cloud's has its take write (replacedCopy) */
  private copying: { files: BesideFile[]; r: Replaced } | null = null;

  /** a version of the keyphrase's (its zip) in place of the saves here, when no game uses them */
  private async restore(zip: Uint8Array, still?: () => Promise<boolean>): Promise<boolean> {
    if (this.d.playing() || this.held || !this.d.owner()) throw new VaultError(N('a game is running here: the other browser\'s saves come in after it'), 'VAULT-PLAYING');
    const r = await readVault(zip);
    // looked at again once no other restore writes, just before it writes: the game may be about to start by then
    const now = async () => {
      if (this.d.playing() || this.held) throw new VaultError(N('a game is running here: the other browser\'s saves come in after it'), 'VAULT-PLAYING');
      return !still || await still();
    };
    const copy = this.copying;                         // (Use the cloud's: the saves here kept with the take)
    try { if (!await restoreVault(this.d.saveDir, r, now, copy?.files)) return false; }
    catch (e) { if (copy) await this.loadReplaced().catch(() => {}); throw e; }
    this.keptNow(copy);
    this.mended = r;
    this.d.onRestored();
    return true;
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

  /** copies an earlier page wrote beside the kept one before 2026-11-02 (saves.next.zip): gone (the copy is written
   *  with its restore since, all or none); in the writer queue after the page start's settleStaged (shell.ts) */
  async settleKept() {
    if (!this.d.owner()) return;
    const dir = replacedDir(this.d.saveDir);
    for (const n of ['saves.next.zip', 'about.next.json']) if (await exists(dir, n)) await filesOf(dir).remove(n).catch(() => {});
  }

  /**
   * The saves here as the copy kept of what something replaces (none when they have no profile: nothing to lose):
   * files the restore writes with its own, all or none (savefs.ts writeTogether), so the copy changes exactly when
   * the saves do: a Put back or a take that fails, or is cut by a closed tab, leaves the copy before.
   */
  private async replacedCopy(files?: SaveFile[]): Promise<{ files: BesideFile[]; r: Replaced } | null> {
    files ??= await this.files();
    if (!profilesIn(files).length) return null;
    const dir = replacedDir(this.d.saveDir), r: Replaced = { at: Date.now(), side: sideOf(files) };
    return { r, files: [{ dir, path: 'saves.zip', data: zip(files.map(f => ({ name: f.path, data: f.data, mtime: f.mtime }))) },
      { dir, path: 'about.json', data: JSON.stringify(r) }] };
  }
  /** the copy written with a restore that came to its end */
  private keptNow(c: { r: Replaced } | null) {
    if (!c) return;
    this.replaced = c.r;
    this.d.log(`[app] keyphrase: the saves here kept before a cloud version replaced them (${c.r.side.profiles!.length} profiles)`);
  }

  /** the kept saves back in place of the ones here, which are kept in their place (Put back again swaps back) */
  private async putBack() {
    const r = this.replaced;
    if (!r) return;
    if (this.d.busy()) { this.msg(() => t(QUIT_FIRST), true); return; }
    const body = document.createElement('div');
    body.append(para(t('This browser\'s saves from before cloud saves replaced them ({when}) come back. The current ones are kept, so you can switch back.', { when: when(r.at) })),
      sidesOf(r.side));
    if (!await this.d.confirm(t('Put back the replaced saves?'), body, t('Put back'), true)) return;
    if (this.d.busy()) { this.msg(() => t(QUIT_FIRST), true); return; }
    try {
      // the saves here become the copy in the same write that puts the kept ones back: either both or neither
      const dir = replacedDir(this.d.saveDir), back = await readVault(await readBytes(dir, 'saves.zip')), copy = await this.replacedCopy();
      await restoreVault(this.d.saveDir, back, undefined, copy?.files);
      this.keptNow(copy);
    } catch (e) {
      // (the copy as it is now: a write that went through before what failed after it swapped it)
      await this.loadReplaced().catch(() => {});
      this.msg(() => e instanceof UndoPending ? t('An earlier restore stopped halfway and is not undone yet, so nothing was changed. Reload the page, then try again.') : t('Putting back stopped: {error}.', { error: e instanceof Error ? e.message : String(e) }), true, 'VAULT-PUT-BACK');
      this.show();
      return;
    } finally { this.d.onRestored(); }
    this.msg(this.sync?.link ? () => t('Put back. They go to the cloud now.') : () => t('Put back.'));
    this.d.log('[app] keyphrase: put back the saves a cloud version had replaced');
    // what the sync judged before is old now: synced again (a conflict asked again, with these saves), as the player chose
    this.sync?.intended();
    this.sync?.reopen();
    this.show();
    this.check();
  }

  /**
   * The browser deleted the site's data during the game (shell.ts saveGone): nothing is sent until the saves are back
   * in the new, empty save folder (VaultSync.hold, kept in the link).  The cloud's version comes back only where pull()
   * would take it by itself: the saves as the game's start had them (start: snapshotSaves, kept up by the game's saves
   * since) are the ones the link synced, no conflict waits, and the version (the server REFILL_WAIT_MS at most) is no
   * older than the link's, has profiles where those have some, and leaves none out; it is linked to as written.  Else
   * start comes back, and the sync goes on from there as for any change here (a conflict when the cloud has another).
   * Every file of start the version lacks comes too.  A write that fails again: thrown after all the others, the sends
   * still held (the next page asks before it sends anything).
   */
  async refill(start: SaveFile[]): Promise<{ from: 'cloud' | 'start'; files: number }> {
    const s = this.sync, l = s?.link;
    s?.hold(true);
    let cloud: { files: SaveFile[]; theirs: SaveFile[]; remote: Remote } | null = null;
    if (s && l && s.state.s !== 'conflict' && !l.gone && !l.held && await contentHash(kept(start, l.v ?? 1)) === l.hash) {
      const got = await Promise.race([s.peek(l.phrase).catch(() => null), new Promise<null>(r => setTimeout(r, REFILL_WAIT_MS, null))]);
      if (got) {
        const theirs = kept((await unzip(got.zip)).map(e => ({ path: e.name, data: e.data, mtime: e.mtime })));
        const older = got.remote.saved < l.saved, bare = !profilesIn(theirs).length && profilesIn(kept(start)).length > 0;
        if (!older && !bare && !leftOut(theirs).length) cloud = { files: (await readVault(got.zip)).entries, theirs, remote: got.remote };
        else this.d.log(`[app] keyphrase: the cloud's version is ${older ? 'older than this browser\'s' : bare ? 'without profiles' : 'one with a profile this browser cannot take in'}: the saves of the game's start come back`);
      }
    } else if (s && l) this.d.log('[app] keyphrase: the saves here had changed since they synced (or a conflict waits): the saves of the game\'s start come back, not the cloud\'s');
    const r = await refillSaves(filesOf(this.d.saveDir), cloud?.files ?? null, start);
    if (cloud && !r.failed.length) await s!.adopt(l!.phrase, cloud.remote, cloud.theirs);
    await s?.keepLink();                     // the link and the folder's mark, deleted with the site's data
    if (r.failed.length) {
      this.d.log(`[app] the saves could not all be put back: ${r.failed.map(f => `${f.path.split('/').pop()}: ${(f.error as Error)?.name ?? f.error}`).join(', ')}`);
      throw r.failed[0].error;
    }
    s?.hold(false);
    return { from: cloud ? 'cloud' : 'start', files: r.written };
  }

  /** A profile the game deleted (its own Delete): the next send may have one fewer. */
  removed() { this.sync?.removed(); }
  /** The player changed the saves on the Saves tab (a rename, a restore, profiles added): sent in a few seconds, as they are. */
  changedSaves() { this.sync?.intended(); this.later(3000); }

  // ---- when it syncs ----

  /** The game saved a profile: sent 10 s after the last save of a burst. */
  saved() { this.later(10_000); }
  /** The Saves tab or a setting changed the files: sent in a few seconds. */
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
    // each card's own button under it, the two alike (the dialog's own buttons none, nothing focused: Enter picks
    // neither); the question after each says what goes
    const keep = Object.assign(document.createElement('button'), { value: 'ok', textContent: t('Keep this browser\'s') });
    const use = Object.assign(document.createElement('button'), { value: 'cancel', textContent: t('Use the cloud\'s') });
    body.append(para(this.conflictText()), choiceOf(st.here, st.cloud, keep, use));
    const lost = lostNote(st);
    if (lost) body.append(lost);
    body.append(para(t('Cloud saves stay paused until you pick. Esc goes back without playing.')));
    const pick = await this.d.choose(t('Which saves do you want to keep?'), body, '', '');
    this.d.log(`[app] keyphrase: a conflict (${st.why}) at Play: ${pick === 'ok' ? 'keep this browser\'s' : pick === 'cancel' ? 'use the cloud\'s' : 'back to the page'}`);
    if (pick === 'ok') return await this.keepMine() ? 'play' : 'stop';
    if (pick === 'cancel') return await this.takeTheirs(true) ? 'taken' : 'stop';
    return 'stop';
  }
  /** Play ended without a game (Esc at a question, the game did not start): a pull given up on at Play waits no more. */
  noGame() { this.held = false; }
  /** A game started or ended, or this tab's ownership changed: the buttons again. */
  refresh() { this.show(); }

  // ---- offered at Play (shell.ts askCloud) ----

  /** Play may offer cloud saves: this server keeps them, this tab owns the saves and has no keyphrase, and they were
   *  offered fewer than OFFER_TIMES times, the last one OFFER_AGAIN_MS ago or more, or the once-more offer is due
   *  (games started and play here: playtime.ts; with storage off: never, as no link would be kept either). */
  offerable(games: number, played: number): boolean {
    const s = this.sync;
    if (!s || s.link || s.state.s === 'working' || !this.d.owner()) return false;
    try { const o = readOffer(localStorage.getItem(offerKey(this.d.saveDir))); return regularDue(o) || lateDue(o, games, played); }
    catch { return false; }
  }
  /** Play offered them now: which offer, for the log ("2 of 3", or the once-more one) */
  offered(games: number, played: number): string {
    try {
      const k = offerKey(this.d.saveDir), o = readOffer(localStorage.getItem(k)), regular = regularDue(o);
      // (an offer past the once-more numbers stands for that one too: not asked again at the next Play)
      const late = o.late || games > LATE_GAMES && played > LATE_MS;
      localStorage.setItem(k, JSON.stringify({ asked: regular ? o.asked + 1 : o.asked, at: Date.now(), late } satisfies Offer));
      return regular ? `${o.asked + 1} of ${OFFER_TIMES}` : `once more, ${games} games and ${Math.round(played / 3600_000)} h played here`;
    } catch { return 'storage off'; }
  }
  /** what the saves here hold, for Play's question (stakeOf): playedMs, the play counted in this browser */
  async stake(playedMs: number): Promise<HTMLElement> { return stakeOf(sideOf(await this.files()), playedMs); }
  /** Cloud saves turned on from Play's question: the keyphrase made, or why not (the section says it too). */
  async enable(): Promise<{ phrase: string } | { why: string }> {
    const l = await this.make() ? this.sync?.link : null, st = this.sync?.state;
    return l ? { phrase: l.phrase } : { why: st?.s === 'error' ? tvars(st.why.t, st.why.v) : '' };
  }
  /** the linked keyphrase's token (claim.ts: the claimed names; the friends; the switch's hello), null without one.
   *  Before `start` has set up the sync: the link kept here.  (2026-10-29, a player's report: asked before the section
   *  knew its link, it answered none, and the line under Your name said the player's own claimed name was another
   *  player's.  Until 2026-11-02 it waited up to 5 s for `start` instead, so a slower first /vault check, or one that
   *  failed and answered later, still left the page without the passphrase, the line never asking again.) */
  async token(): Promise<string | null> {
    if (this.sync) return this.sync.token();
    const l = this.load();
    if (typeof l?.phrase !== 'string') return null;
    if (this.early?.phrase !== l.phrase) {
      const token = keysOf(l.phrase).then(k => k.token);
      token.catch(() => { if (this.early?.token === token) this.early = null; });
      this.early = { phrase: l.phrase, token };
    }
    return this.early.token;
  }
  /** a keyphrase is linked here (before `start`: one is kept here) */
  linked(): boolean { return this.sync ? !!this.sync.link : typeof this.load()?.phrase === 'string'; }
  /** the line about the name this keyphrase claims (null: none); drawn again as the section shows (the name can be
   *  known before `start` set up the sync) */
  showClaim(name: string | null) {
    this.claimed = name;
    this.claimLine();
  }
  private claimLine() {
    const p = this.$<HTMLElement>('#vault-claim'), name = this.claimed;
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
      this.reveal(true);
      this.show();
      this.msg(() => t('Enter it on another device to sync your saves. Store it somewhere safe: it can\'t be recovered.'));
      this.d.log('[app] keyphrase: made one, the saves went to the server');
      if (this.sync.link) this.keep(this.sync.link.phrase);
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
      t('They replace the profiles and settings.ini here with the ones saved {when}. Screenshots and this device\'s graphics, sound and controller settings stay.', { when: when(got.remote.saved) }), t('Load'))) {
      await s.adopt(p.phrase, got.remote).catch(() => {});
      this.msg(() => t('Loaded. This browser syncs from now on.'));
      this.d.log('[app] keyphrase: loaded its saves, this browser syncs with it');
      if (s.link) this.keep(s.link.phrase);
    }
  }

  /** what a version brings, asked; then it replaces the saves here.  False: nothing changed.  danger: its button red
   *  (undefined: when anything a keyphrase keeps is here, settings.ini too) */
  private async replace(got: { zip: Uint8Array; remote: Remote }, title: string, text: string, ok: string, danger?: boolean): Promise<boolean> {
    let r: Awaited<ReturnType<typeof readVault>>;
    try { r = await readVault(got.zip); } catch (e) { this.msg(() => t('These saves cannot be read: {error}.', { error: e instanceof Error ? e.message : String(e) }), true, 'VAULT-UNREADABLE'); return false; }
    const body = document.createElement('div'), p = document.createElement('p'), ul = document.createElement('ul');
    p.textContent = text;
    for (const t of backupItems(r, true)) { const li = document.createElement('li'); li.textContent = t; ul.append(li); }
    body.append(p, ul);
    if (!await this.d.confirm(title, body, ok, danger ?? (await listFiles(this.d.saveDir)).some(f => keeps(f.path)))) { this.msg(''); return false; }
    if (this.d.busy()) { this.msg(() => t(QUIT_FIRST), true); return false; }
    let copy: Awaited<ReturnType<VaultPanel['replacedCopy']>>;
    try { copy = await this.replacedCopy(); }
    catch (e) { this.msg(() => t('The saves here could not be kept, so nothing was replaced: {error}.', { error: e instanceof Error ? e.message : String(e) }), true, 'VAULT-KEEP-COPY'); return false; }
    try { await restoreVault(this.d.saveDir, r, undefined, copy?.files); this.keptNow(copy); }
    catch (e) {
      if (copy) await this.loadReplaced().catch(() => {});
      this.msg(() => e instanceof UndoPending ? t('An earlier restore stopped halfway and is not undone yet, so nothing was changed. Reload the page, then try again.') : t('Loading stopped halfway: {error}. Load again before playing.', { error: e instanceof Error ? e.message : String(e) }), true, 'VAULT-LOAD-HALFWAY');
      return false;
    }
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
      t('The cloud has no saves for this passphrase. The saves here go there under the same passphrase, so your other browsers sync again.'), t('Send'))) return;
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
      s.intended();                                       // its fewer profiles are the player's choice: sent, not asked
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
    if (keepLoses(st).changed.length) body.append(Object.assign(para(t('The cloud\'s saves are newer: what was played on them since is lost.')), { className: 'warn' }));
    const lost = lostNote(st);
    if (lost) body.append(lost);
    if (!await this.d.confirm(t('Keep this browser\'s saves?'), body, t('Keep this browser\'s'), keepDanger(st))) return false;
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
    const lose = useLoses(st);
    if (cloudAt != null && lose.changed.length)
      body.append(Object.assign(para(t('This browser\'s saves are newer: the progress since {when} is replaced.', { when: when(cloudAt) })), { className: 'warn' }));
    if (st.why === 'bare') body.append(Object.assign(para(t('They have no profiles: the profiles here are deleted.')), { className: 'warn' }));
    else if (lose.gone.length) body.append(Object.assign(para(tn(lose.gone.length, 'This browser\'s {names} is deleted here: the cloud\'s saves do not have it.',
      'This browser\'s {names} are deleted here: the cloud\'s saves do not have them.', { names: listOf(lose.gone) })), { className: 'warn' }));
    if (st.here.profiles?.length) body.append(para(tn(REPLACED_DAYS, 'A copy of the saves here is kept for {n} day (Put back, under Cloud saves).',
      'A copy of the saves here is kept for {n} days (Put back, under Cloud saves).')));
    if (!await this.d.confirm(t('Use the cloud\'s saves?'), body, t('Use the cloud\'s'), useDanger(st))) return false;
    if (busy()) { this.msg(() => t(QUIT_FIRST), true); return false; }
    try { this.copying = await this.replacedCopy(); }
    catch (e) { this.msg(() => t('The saves here could not be kept, so nothing was replaced: {error}.', { error: e instanceof Error ? e.message : String(e) }), true, 'VAULT-KEEP-COPY'); return false; }
    // (the take's restore writes the copy with it: VaultPanel.restore)
    try { await s.takeTheirs().catch(() => {}); } finally { this.copying = null; }
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
      // (the claimed name was the last one's: asked again with this one)
      if (!first) { this.claimed = null; this.d.onLink?.(); }
    }
    this.claimLine();
    const working = st.s === 'working';
    // beside the heading: Turn on, or what it is doing, or when it last synced
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
    // the two sides, each with its own button under it (choiceOf); else the buttons wait in their hidden row
    const mine = this.$('#vault-mine'), theirs = this.$('#vault-theirs'), row = this.$('#vault-conflict');
    if (conflict && l) this.$('#vault-sides').replaceChildren(choiceOf(conflict.here, conflict.cloud, mine, theirs));
    else { row.append(mine, theirs); this.$('#vault-sides').replaceChildren(); }
    row.hidden = true;
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
      : st.s === 'gone' ? t('The cloud has no saves for this passphrase (deleted in another browser, unused for 2 years, or lost). Nothing was sent; Sync now checks again and can send these.')
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
      : st.why === 'wiped' ? t('The browser deleted this site\'s saves since they last synced, so nothing was sent. Keep which?')
      : st.why === 'fewer' ? t('This browser has fewer whole profiles than when they last synced, so nothing was sent. Keep which?')
      : st.why === 'thin' ? t('The cloud\'s saves from {when} lack profiles this browser has and come from an older version of this page, so they did not come in by themselves. Keep which?', { when: theirs })
      : st.why === 'drops' ? t('The cloud\'s saves from {when} have a profile this browser cannot take in, so they did not come in by themselves. Keep which?', { when: theirs })
      : st.why === 'bare' ? (theirs ? tn(n, 'The cloud\'s saves from {when} have no profiles, but this browser has one. Keep which?', 'The cloud\'s saves from {when} have no profiles, but this browser has {n}. Keep which?', { when: theirs })
        : tn(n, 'The cloud\'s saves have no profiles, but this browser has one. Keep which?', 'The cloud\'s saves have no profiles, but this browser has {n}. Keep which?'))
      : st.why === 'older' ? (theirs ? t('The cloud has older saves ({when}) than this browser\'s ({mine}): the server lost the newer ones. Keep which?', { when: theirs, mine: short(l.saved) })
        : t('The cloud has older saves than this browser\'s ({mine}): the server lost the newer ones. Keep which?', { mine: short(l.saved) }))
      : theirs ? t('Changed here and in another browser ({when}). Keep which?', { when: theirs }) : t('Changed here and in another browser. Keep which?');
  }
}
