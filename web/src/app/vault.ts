// Passphrase saves: this browser's saves kept on the server too, under a keyphrase only the player has, so the career
// goes on in any browser the keyphrase is typed into (docs/web/PLAYING.md, Saves).  The server keeps what it is sent
// and gives it back to whoever shows the token (web/server-rs/crates/vault: the caps, the copy of an earlier day, two
// years unused); all that knows a save is here, so the server never sees one:
// - the keyphrase: "nfsu2-xyz-" and 8 of vault-words.ts's 1296 words (about 2^83), from crypto.getRandomValues; read
//   back in any case, with any separators, "nfsu2-xyz" optional;
// - PBKDF2-SHA-256 of it (100,000 rounds, a fixed salt: the keyphrase is all there is) gives 64 bytes: an AES-GCM key
//   and a token.  The server keeps the keyphrase under the token's SHA-256 and never sees the key;
// - what is sent (kept()): the save folder but the F12 screenshots, the DVD and magazine covers, the game's graphics
//   options (registry.txt), the LAN server's file (gamefile.bin), a test's input script and the device's own lines of
//   settings.ini, as a stored zip (zip.ts), deflated, then sealed: a version byte (1), a 12-byte IV, the AES-256-GCM
//   ciphertext and its tag.  A player's folder is 6-14 KB so; the server takes 256 KB.  A cover is a picture of 1 MB
//   that packs into about 500 KB (2026-10-25: since the covers are read back from the GPU, no longer all zeros), so the
//   covers stay in the browser that took them: taking another browser's version keeps the ones of its profiles here.
//   The graphics options and settings.ini's resolution scale, frame rate, controller and the like (settings.ts
//   KeyDef.device) are the device's, not the player's: a phone's would slow a PC down and a PC's a phone, so each
//   browser keeps its own of those too, and of the record of the LAN games and free-roam races it hosted (2026-10-28:
//   on the server, 151 keyphrases had versions of 33-235 KB, where a profile packs into 2-4 KB);
// - VaultSync: this browser's link (the keyphrase, the version it has, a hash of the files it sent or took) and the
//   two ways of syncing.  Sending: after the game saves and when it closes, if the files changed since; taking: when
//   the page opens and before Play, if another browser sent a newer version and the files here did not change.  Both
//   changed (to something else): a conflict, the player picks.  But a version this browser sent itself and never
//   heard back about (a timeout, a frozen or closed page: Link.sent) is no other browser's: the link follows it and
//   the files here go out over it (2026-10-27, issue 33: one lost answer stopped a player's syncing for 7 hours, and
//   "Use the cloud's" then took that old version).  A browser with no profile while its keyphrase has
//   some (its storage cleared, an empty backup restored) never sends by itself either, a version with no profile
//   never replaces the profiles here by itself, and neither does a version older than the one here (the server went
//   back: a backup restored, a write lost).  A keyphrase the server does not have (any more) stays linked: the next
//   sync looks again, and the player may send the saves here under it again.

import { WORDS } from './vault-words';
import { crc32, unzip, zip, zipComment } from './zip';
import { coverOf, profileMoney, profileWhole } from './profile';
import { isScreenshot, PROFILES_DIR, UndoPending } from './savefs';
import { withoutDevice } from './settings';
import { fill, N, type Vars } from './i18n';

export const PREFIX = 'nfsu2-xyz';
export const PHRASE_WORDS = 8;
export const KDF_ROUNDS = 100_000;
/** what the server takes (crates/vault VaultOptions::sized) */
export const MAX_BOX = 256 << 10;
/** the most a keyphrase's zip inflates to: a backup's saves and settings (saves-ui.ts MAX_OTHER) */
const MAX_PLAIN = 256 << 20;
const SALT = 'nfsu2-xyz keyphrase saves v1', AAD = 'nfsu2-xyz keyphrase saves v1';
const WORD_SET = new Set(WORDS);
const enc = new TextEncoder(), dec = new TextDecoder();

/** A failure of the cloud saves: its message in English (the log, the tests); why and v: the same as written, for the
 *  page's language; code: its fixed name, the same in every language. */
export class VaultError extends Error {
  constructor(public why: string, public code = 'VAULT', public v?: Vars) { super(fill(why, v)); }
}

/** A new keyphrase: the prefix and PHRASE_WORDS random words. */
export function newPhrase(): string {
  const words: string[] = [];
  const limit = Math.floor(65536 / WORDS.length) * WORDS.length;     // past it, a value would favour the first words
  while (words.length < PHRASE_WORDS)
    for (const v of crypto.getRandomValues(new Uint16Array(PHRASE_WORDS)))
      if (v < limit && words.length < PHRASE_WORDS) words.push(WORDS[v % WORDS.length]);
  return [PREFIX, ...words].join('-');
}

/** A keyphrase as typed or pasted (any case, any separators, the prefix optional), or what is wrong with it. */
export function readPhrase(text: string): { phrase: string } | { bad: string; why: { t: string; v?: Vars } } {
  const bad = (t: string, v?: Vars) => ({ bad: fill(t, v), why: { t, v } });
  let w = text.toLowerCase().split(/[^a-z0-9]+/).filter(Boolean);
  if (w[0] === 'nfsu2' && w[1] === 'xyz') w = w.slice(2);
  if (!w.length) return bad(N('Type or paste your passphrase.'));
  const unknown = w.find(x => !WORD_SET.has(x));
  if (unknown) return bad(N('"{word}" is not a passphrase word. Check the spelling.'), { word: unknown });
  if (w.length !== PHRASE_WORDS) return bad(N('A passphrase has {n} words after {prefix}; this has {got}.'), { n: PHRASE_WORDS, prefix: PREFIX, got: w.length });
  return { phrase: [PREFIX, ...w].join('-') };
}

export interface Keys { key: CryptoKey; token: string }

const base64url = (b: Uint8Array) => btoa(String.fromCharCode(...b)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
const hex = (b: Uint8Array) => Array.from(b, x => x.toString(16).padStart(2, '0')).join('');

/** The key that seals a keyphrase's saves and the token the server keeps them under. */
export async function keysOf(phrase: string): Promise<Keys> {
  const base = await crypto.subtle.importKey('raw', enc.encode(phrase), 'PBKDF2', false, ['deriveBits']);
  const bits = new Uint8Array(await crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt: enc.encode(SALT), iterations: KDF_ROUNDS }, base, 512));
  const key = await crypto.subtle.importKey('raw', bits.slice(0, 32), 'AES-GCM', false, ['encrypt', 'decrypt']);
  return { key, token: base64url(bits.subarray(32)) };
}

export async function seal(key: CryptoKey, plain: Uint8Array): Promise<Uint8Array> {
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const ct = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv, additionalData: enc.encode(AAD) }, key, plain as BufferSource));
  const out = new Uint8Array(13 + ct.length);
  out[0] = 1; out.set(iv, 1); out.set(ct, 13);
  return out;
}

export async function unseal(key: CryptoKey, box: Uint8Array): Promise<Uint8Array> {
  if (box.length < 13 + 16 || box[0] !== 1) throw new VaultError(N('what the server holds for this passphrase is not a passphrase\'s saves'), 'VAULT-NOT-SAVES');
  try {
    return new Uint8Array(await crypto.subtle.decrypt({ name: 'AES-GCM', iv: box.slice(1, 13), additionalData: enc.encode(AAD) }, key, box.slice(13)));
  } catch { throw new VaultError(N('these saves do not open with this passphrase'), 'VAULT-WRONG-KEY'); }
}

export interface SaveFile { path: string; data: Uint8Array; mtime?: number }

/** A DVD or magazine cover's file ("NFS Underground 2/<NAME> DVD 1/<NAME> DVD 1"). */
export function isCover(path: string): boolean {
  const p = path.split('/');
  return p.length >= 3 && p[0] === PROFILES_DIR && !!coverOf(p[1]);
}
/** The LAN server's file (server.dll, when this browser hosts a LAN game or a free-roam race): its ranking lists and
 *  the races it held, read back at its next start and written whole at its end. */
export const GAMEFILE = 'gamefile.bin';
/** the link's version now: its hash counts kept() (Link.v) */
const LINK_V = 4;
/** A file a keyphrase keeps, settings.ini whole, under a link of version v (Link.v; none: 1): 1, all but the
 *  screenshots and a test's input script; 2 (2026-10-25), the covers left out too; 3 (2026-10-25), the game's graphics
 *  options (registry.txt) too, and settings.ini without the device's own settings (kept()); 4 (2026-10-28), the LAN
 *  server's file too (GAMEFILE: the host's, not the career's). */
export const keptBy = (v: number) => (path: string) => !isScreenshot(path) && path !== 'autotest-script.txt' && !path.endsWith('.incoming') &&
  (v < 2 || !isCover(path)) && (v < 3 || path !== 'registry.txt') && (v < 4 || path !== GAMEFILE);
/** A file of the save folder a keyphrase keeps: all but the F12 screenshots, the covers, the game's graphics options,
 *  the LAN server's file and a test's input script. */
export const keeps = keptBy(LINK_V);

/** The files of the save folder as a keyphrase keeps them (as a link of version v counted them): keptBy(v)'s, from
 *  version 3 settings.ini without the device's own settings (settings.ts withoutDevice). */
export function kept<F extends { path: string; data: Uint8Array }>(files: F[], v = LINK_V): F[] {
  return files.filter(f => keptBy(v)(f.path)).map(f => {
    if (v < 3 || f.path !== 'settings.ini') return f;
    const text = dec.decode(f.data), own = withoutDevice(text);
    return own === text ? f : { ...f, data: enc.encode(own) };
  });
}

/** a stream's bytes, at most max of them */
async function bytesOf(s: ReadableStream<Uint8Array>, max = Infinity): Promise<Uint8Array> {
  const rd = s.getReader(), parts: Uint8Array[] = [];
  let n = 0;
  try {
    for (;;) {
      const { done, value } = await rd.read();
      if (done) break;
      if ((n += value.length) > max) throw new VaultError(N('these saves are larger than a passphrase keeps'), 'VAULT-TOO-LARGE');
      parts.push(value);
    }
  } catch (e) { void rd.cancel().catch(() => {}); throw e; }
  const out = new Uint8Array(n);
  let p = 0;
  for (const x of parts) { out.set(x, p); p += x.length; }
  return out;
}

/**
 * What each version this page sends says of itself (the zip's comment, which every page's unzip skips): its page never
 * sends a folder thinner than the one it synced by itself (a deleted folder, a damaged profile: VaultSync.push).  A
 * version without it, from an older page (one cached on another device), that would drop profiles here is not taken
 * by itself (VaultSync.pull, 'thin').
 */
export const GUARDED = 'nfsu2 cloud saves: sent by a page that asks before it sends fewer profiles';

/** The files as a zip, deflated (comment: the zip's, GUARDED). */
export async function pack(files: SaveFile[], comment = ''): Promise<Uint8Array> {
  const z = zip(files.map(f => ({ name: f.path, data: f.data, mtime: f.mtime })), comment);
  return bytesOf(new Blob([z as BlobPart]).stream().pipeThrough(new CompressionStream('deflate-raw')));
}

/** pack()'s zip again (at most MAX_PLAIN: what restores as a backup would) */
export async function unpack(packed: Uint8Array): Promise<Uint8Array> {
  try { return await bytesOf(new Blob([packed as BlobPart]).stream().pipeThrough(new DecompressionStream('deflate-raw')), MAX_PLAIN); }
  catch (e) { throw e instanceof VaultError ? e : new VaultError(N('these saves are damaged ({error})'), 'VAULT-DAMAGED', { error: e instanceof Error ? e.message : String(e) }); }
}

/** What the files hold, whatever their times: SHA-256 over their paths and bytes, in path order. */
export async function contentHash(files: SaveFile[]): Promise<string> {
  const sorted = [...files].sort((a, b) => a.path < b.path ? -1 : a.path > b.path ? 1 : 0);
  const parts = sorted.flatMap(f => {
    const name = enc.encode(f.path), head = new Uint8Array(8), v = new DataView(head.buffer);
    v.setUint32(0, name.length); v.setUint32(4, f.data.length);
    return [head, name, f.data];
  });
  return hex(new Uint8Array(await crypto.subtle.digest('SHA-256', await new Blob(parts as BlobPart[]).arrayBuffer())));
}

/** The profiles' files among the files ("NFS Underground 2/<NAME>/<NAME>", not a cover shoot's). */
function profileFiles(files: SaveFile[]): SaveFile[] {
  return files.filter(f => { const p = f.path.split('/');
    return p.length === 3 && p[0] === PROFILES_DIR && p[1].toLowerCase() === p[2].toLowerCase() && !coverOf(p[1]); });
}
/** The profiles among the files, by name. */
export const profilesIn = (files: SaveFile[]): string[] => profileFiles(files).map(f => f.path.split('/')[1]);

/** how many profiles are not whole, as the game wrote them (their checksums fail: read halfway through a save, or
 *  emptied by a write that failed; a name inside the game's field does not make is no half save) */
const tornCount = (files: SaveFile[]) => profileFiles(files).filter(f => !profileWhole(f.data)).length;
/** how many profiles are whole */
const wholeCount = (files: SaveFile[]) => profileFiles(files).length - tornCount(files);

// ------------------------------------------------------------- the server --

/** A version on the server: its rev (the ETag), when it was saved, when the copy of an earlier day was (null: none). */
export interface Remote { rev: string; saved: number; earlier: number | null }
export type Fetch = (url: string, init?: RequestInit) => Promise<Response>;

function remoteOf(r: Response): Remote | null {
  const rev = r.headers.get('etag'), saved = Number(r.headers.get('x-vault-saved')), e = r.headers.get('x-vault-earlier');
  return rev && saved ? { rev, saved, earlier: e ? Number(e) : null } : null;
}

function failure(r: Response): VaultError {
  return new VaultError(r.status === 429 ? N('the server asks this address to wait a minute (too many requests)')
    : r.status === 507 ? N('the server has no room for more saves now')
    : r.status === 503 ? N('the server takes no more new cloud saves for now: try again in an hour')
    : r.status === 413 ? N('these saves are larger than the server takes')
    : N('the server answered {status}'), `VAULT-HTTP-${r.status}`, { status: r.status });
}

const incomplete = () => new VaultError(N('the server\'s answer is incomplete'), 'VAULT-INCOMPLETE');

/** /vault for one keyphrase's token. */
export class VaultApi {
  /** timeoutMs: each request's answer, at most */
  constructor(private token: string, private f: Fetch = (u, i) => fetch(u, i), private timeoutMs = 30_000) {}

  private req(method: string, headers: Record<string, string> = {}, body?: Uint8Array, query = '') {
    return this.f(`/vault${query}`, {
      method, headers: { authorization: `Bearer ${this.token}`, ...headers }, body: body as BodyInit | undefined, cache: 'no-store',
      signal: AbortSignal.timeout(this.timeoutMs),
    });
  }

  /** The current version (or the copy of an earlier day), null when there is none; { same } when it is still `rev`. */
  async get(rev?: string | null, earlier = false): Promise<{ box: Uint8Array; remote: Remote } | { same: Remote } | null> {
    const r = await this.req('GET', rev && !earlier ? { 'if-none-match': rev } : {}, undefined, earlier ? '?earlier' : '');
    if (r.status === 404) return null;
    const remote = remoteOf(r);
    if (r.status === 304) { if (!remote) throw incomplete(); return { same: remote }; }
    if (!r.ok) throw failure(r);
    if (!remote) throw incomplete();
    return { box: new Uint8Array(await r.arrayBuffer()), remote };
  }

  /** Saved over `rev` (null: as a new keyphrase), or what is there instead (null: nothing). */
  async put(box: Uint8Array, rev: string | null): Promise<{ saved: Remote } | { conflict: Remote | null }> {
    const r = await this.req('PUT', rev ? { 'if-match': rev } : { 'if-none-match': '*' }, box);
    if (r.status === 412) return { conflict: remoteOf(r) };
    if (!r.ok) throw failure(r);
    const remote = remoteOf(r);
    if (!remote) throw incomplete();
    return { saved: remote };
  }

  /** The keyphrase and its versions gone from the server: false when there was none. */
  async remove(): Promise<boolean> {
    const r = await this.req('DELETE');
    if (r.status === 404) return false;
    if (!r.ok) throw failure(r);
    return true;
  }
}

/** Whether this server keeps keyphrase saves: /vault without a token answers 401 there (404 on one without). */
export async function vaultAvailable(f: Fetch = (u, i) => fetch(u, i)): Promise<boolean> {
  try { return (await f('/vault', { cache: 'no-store' })).status === 401; } catch { return false; }
}

// ------------------------------------------------------------- syncing --

/** This browser's keyphrase and the version of its saves it has. */
export interface Link {
  phrase: string;
  /** the version this browser sent or took (its ETag) */
  rev: string;
  /** contentHash of the files here as they were then */
  hash: string;
  /** when that version was saved (the server's clock), and the copy of an earlier day (null: none) */
  saved: number;
  earlier: number | null;
  /** its profiles */
  profiles: number;
  /** the server did not have the keyphrase at the last sync: nothing is sent by itself */
  gone?: boolean;
  /** what hash counts: LINK_V, kept(); an older one, kept(files, v) (none: 1; VaultSync.upgraded) */
  v?: number;
  /** the hashes of files sent under this link whose answer never came (a timeout, a frozen or closed page): a version
   *  of the server's with one of them is this browser's own, not another's (none: every send was answered) */
  sent?: string[];
  /** the save folder this link synced with (VaultDeps.folder's mark; none: a link from before 2026-11-02, or none
   *  made yet): another there, or none, and the folder was deleted since (the browser cleared the site's data) */
  folder?: string;
  /** sends held back (VaultSync.hold) by a page whose save folder the browser deleted during a game, until the saves
   *  are back in it: kept here, so a page after one whose refill did not end sends nothing by itself either */
  held?: boolean;
  /** the profiles that were not whole then (a save torn by a crash, kept as the player chose): a push asks when more are */
  torn?: number;
  /** the whole profiles then (none: a link of before 2026-11-02, profiles): a push asks when fewer are, but for those
   *  deleted on purpose (dropped) */
  whole?: number;
  /** the profiles deleted on purpose since (VaultSync.removed: the game's own Delete; intended: the player changed the
   *  saves here, a restore, a rename, Go back, so all of them): kept here, so a send that failed and a reload ask
   *  nothing */
  dropped?: number;
  /** the folder's mark (folder) could not be written (the browser's storage full): a deleted folder is noticed by its
   *  profiles only (fewer, empty), and asks no 'wiped' at every send, until a mark is written */
  unmarked?: boolean;
}

/** one side of a conflict: its profiles and their banks (null: not known) and when they were saved (here: the newest
 *  profile's file's time, this device's clock; the cloud: the server's; null: not known) */
export interface Side {
  saved: number | null; profiles: { name: string; money: number | null; whole?: boolean }[] | null;
  /** each profile's CRC-32, by its name in lower case: which of two sides' profiles differ */
  sums?: Record<string, number>;
}

export interface VaultDeps {
  /** the files of the save folder as a keyphrase keeps them (kept()); was: as a link of that version counted them
   *  (kept(files, was)) */
  files(o?: { was?: number }): Promise<SaveFile[]>;
  /** a keyphrase's version in place of the saves here (its zip): throws when it may not be written now.  still: asked
   *  once no other restore writes (saves-ui.ts oneWriter), just before it writes; false: nothing written, false back */
  restore(zip: Uint8Array, still?: () => Promise<boolean>): Promise<boolean | void>;
  /** the link kept in this browser */
  load(): Link | null;
  save(l: Link | null): void;
  fetch?: Fetch;
  /** a line for the game's log */
  log?(s: string): void;
  /** the profiles a restore of a version's files (kept()) brings, by the names they come in under (a broken one
   *  mended: saves-ui.ts profilesComing) */
  named?(files: SaveFile[]): string[];
  /** the profiles (their folders) of a version's files (kept()) that restore() would leave out (saves-ui.ts leftOut) */
  leftOut?(files: SaveFile[]): string[] | Promise<string[]>;
  /** the save folder's mark (a file beside it: no restore or cloud save touches it), null: none; mark: made so */
  folder?(): Promise<string | null>;
  mark?(id: string): Promise<void>;
}

export type SyncState =
  | { s: 'off' }
  | { s: 'synced' }
  | { s: 'working'; what: string }
  /**
   * the player picks: both changed ('both'); no profile here while the keyphrase keeps some ('empty'); the keyphrase's
   * version has no profile while here are some ('bare'); the keyphrase's version is older than the one here ('older');
   * another browser's version would leave out a profile of its own, one restore() cannot take in ('drops'); the save
   * folder here was deleted since it synced (the browser cleared the site's data) and is not whole again ('wiped');
   * more profiles here not whole than when it synced (an empty or torn file), or fewer whole ones but for those deleted
   * on purpose ('fewer'); another browser's version, from an older page, would drop profiles here ('thin')
   */
  | { s: 'conflict'; why: 'both' | 'empty' | 'bare' | 'older' | 'drops' | 'wiped' | 'fewer' | 'thin'; theirs: Remote | null; here: Side; cloud: Side | null;
      /** ('drops') the profiles here the cloud's version cannot bring, which Use the cloud's keeps here */
      stays?: string[] }
  /** the last try failed: the next save, Play or Sync now tries again (msg: why, in English; why: the same for t(); code:
   *  its fixed name) */
  | { s: 'error'; msg: string; why: { t: string; v?: Vars }; code: string }
  /** the server does not have the keyphrase (deleted in another browser, two years unused, lost): nothing is sent */
  | { s: 'gone' };

/** a failure's words for the player */
export function failureText(e: unknown): string { return failureOf(e).msg; }
/** the same as written (why, v: for t()), and its fixed name */
export function failureOf(e: unknown): { msg: string; why: { t: string; v?: Vars }; code: string } {
  const of = (t: string, code: string, v?: Vars) => ({ msg: fill(t, v), why: { t, v }, code });
  return e instanceof VaultError || e instanceof UndoPending ? of(e.why, e.code, e instanceof VaultError ? e.v : undefined)
    : e instanceof TypeError ? of(N('the server does not answer'), 'VAULT-NO-ANSWER')
    : e instanceof DOMException && (e.name === 'TimeoutError' || e.name === 'AbortError') ? of(N('the server did not answer in time'), 'VAULT-TIMEOUT')
    : of(String(e instanceof Error ? e.message : e), 'VAULT-ERROR');
}

/** the unanswered sends a link remembers */
const SENT_MAX = 8;

/** a side of a conflict from its files (saved: the server's time of them; none: the newest profile's file's) */
export function sideOf(files: SaveFile[], saved?: number): Side {
  const profiles = profileFiles(files), times = profiles.filter(f => f.mtime).map(f => f.mtime!);
  return { saved: saved ?? (times.length ? Math.max(...times) : null),
    profiles: profiles.map(f => ({ name: f.path.split('/')[1], money: profileMoney(f.data), ...profileWhole(f.data) ? {} : { whole: false } })),
    sums: Object.fromEntries(profiles.map(f => [f.path.split('/')[1].toLowerCase(), crc32(f.data)])) };
}

export class VaultSync {
  link: Link | null;
  state: SyncState;
  onChange: () => void = () => {};
  /** another browser's version came in (pull, takeTheirs) */
  onTaken: (r: Remote) => void = () => {};
  private keys: { phrase: string; k: Keys } | null = null;
  private queue: Promise<unknown> = Promise.resolve();

  constructor(private d: VaultDeps) {
    this.link = d.load();
    this.state = this.rest();
  }

  private async keysFor(phrase: string): Promise<Keys> {
    if (this.keys?.phrase !== phrase) this.keys = { phrase, k: await keysOf(phrase) };
    return this.keys.k;
  }
  private api(k: Keys, timeoutMs?: number) { return new VaultApi(k.token, this.d.fetch, timeoutMs); }
  /** the linked keyphrase's token (what claims a name: claim.ts), null without one */
  async token(): Promise<string | null> { return this.link ? (await this.keysFor(this.link.phrase)).token : null; }
  private set(state: SyncState) { this.state = state; this.onChange(); }
  /** the state when no job runs and nothing waits for the player */
  private rest(): SyncState { return !this.link ? { s: 'off' } : this.link.gone ? { s: 'gone' } : { s: 'synced' }; }
  private setLink(l: Link | null) { this.link = l; this.d.save(l); }
  /** sends held back (hold): the save folder is not whole (the browser deleted it during a game, and the saves are
   *  not back yet): a folder of the one profile the game saved since would replace the cloud's profiles */
  private holding = false;
  /**
   * Sends held back, or let go (shell.ts saveGone): the save folder is not whole (the browser deleted it during a
   * game, and the saves are not back yet): a folder of the one profile saved since must never replace the cloud's.
   * Kept in the link too (Link.held), so a page after one whose refill did not end asks before it sends.
   */
  hold(on: boolean) {
    this.holding = on;
    if (this.link && !!this.link.held !== on) this.setLink({ ...this.link, held: on || undefined });
  }
  /** A profile deleted on purpose (the game's own Delete: shell.ts onRemoved): one fewer at the next push is no loss. */
  removed(n = 1) {
    if (this.link) this.setLink({ ...this.link, dropped: (this.link.dropped ?? 0) + n });
  }
  /** The player changed the saves here (a restore, a rename, Go back): what the next push finds is what they want. */
  intended() {
    const l = this.link;
    if (l) this.setLink({ ...l, dropped: Math.max(l.dropped ?? 0, l.whole ?? l.profiles) });
  }
  /** The link kept again where the page keeps it, and the folder's mark (the browser deletes both with the site's data). */
  async keepLink() {
    if (!this.link) return;
    this.d.save(this.link);
    await this.markFolder();
  }
  private marking: Promise<void> = Promise.resolve();
  /** The save folder marked as the one the link synced with (deps.folder): its mark taken, or the link's put back (a
   *  new one without), one at a time. */
  markFolder(): Promise<void> {
    return this.marking = this.marking.then(async () => {
      if (!this.link || !this.d.folder || !this.d.mark) return;
      let id = await this.d.folder().catch(() => null);
      if (!id) {
        id = this.link.folder ?? Array.from(crypto.getRandomValues(new Uint8Array(8)), b => b.toString(16).padStart(2, '0')).join('');
        try { await this.d.mark(id); }
        catch (e) {
          // the player's choice stands: no 'wiped' at every send for a mark that cannot be written
          if (this.link && !this.link.unmarked) {
            this.d.log?.(`[app] keyphrase: the save folder's mark could not be written (${e instanceof Error ? e.message : e}): a deleted folder is noticed by its profiles only, until it can`);
            this.setLink({ ...this.link, folder: id, unmarked: true });
          }
          return;
        }
      }
      if (this.link && (this.link.folder !== id || this.link.unmarked)) this.setLink({ ...this.link, folder: id, unmarked: undefined });
    });
  }

  /** one job at a time, in order: its state while it runs, a failure's words after */
  private run<T>(what: string, job: () => Promise<T>): Promise<T> {
    const p = this.queue.then(async () => {
      this.set({ s: 'working', what });
      try { return await job(); }
      catch (e) { this.set({ s: 'error', ...failureOf(e) }); throw e; }
      finally { if (this.state.s === 'working') this.set(this.rest()); }
    });
    this.queue = p.catch(() => {});
    return p;
  }

  private async boxOf(k: Keys, files: SaveFile[]): Promise<Uint8Array> {
    const box = await seal(k.key, await pack(files, GUARDED));
    if (box.length > MAX_BOX) {
      // what takes the room, for the log: each file packed on its own, the largest first
      const sized = await Promise.all(files.map(async f => ({ path: f.path, kb: (await pack([f])).length / 1024 })));
      sized.sort((a, b) => b.kb - a.kb);
      this.d.log?.(`[app] keyphrase: the saves pack into ${Math.ceil(box.length / 1024)} KB (${files.length} files, ` +
        `${profilesIn(files).length} profiles); the largest packed: ${sized.slice(0, 5).map(s => `${s.path} ${s.kb.toFixed(1)} KB`).join(', ')}`);
      throw new VaultError(N('these saves pack into {kb} KB, more than the server takes ({max} KB)'), 'VAULT-TOO-LARGE', { kb: Math.ceil(box.length / 1024), max: MAX_BOX >> 10 });
    }
    return box;
  }

  /** the link at a version; mark: the save folder is marked as its own (a first link, a take, an adopt, the player's
   *  keepMine): never by a send alone, whose answer may come after the browser deleted the folder */
  private linked(phrase: string, r: Remote, hash: string, files: SaveFile[], mark = false): Promise<void> {
    const first = !this.link?.folder, torn = tornCount(files);
    this.setLink({ phrase, rev: r.rev, hash, saved: r.saved, earlier: r.earlier, profiles: profilesIn(files).length,
      v: LINK_V, folder: this.link?.folder, unmarked: this.link?.unmarked, held: this.holding || undefined, torn: torn || undefined,
      whole: profileFiles(files).length - torn });
    // (a mark that could not be written is tried again after each send)
    return mark || first || this.link?.unmarked ? this.markFolder() : Promise.resolve();
  }

  /** The link, its hash of kept().  An older one counted more (2026-10-25: the covers, the graphics options and the
   *  device's settings stayed out; 2026-10-28: the LAN server's file): when the files here are still the ones it counted, it takes their hash as kept now
   *  (hash), so they still count as unchanged since; when not, they changed. */
  private async upgraded(hash: string): Promise<Link> {
    const l = this.link!;
    if (l.v === LINK_V) return l;
    const same = await contentHash(await this.d.files({ was: l.v ?? 1 })) === l.hash;
    this.setLink({ ...l, hash: same ? hash : '', v: LINK_V });
    return this.link!;
  }

  /** The server has no version of the keyphrase: one that keeps no keyphrase saves at all now says so; else the link
   *  stays (the phrase in the box, the next sync looks again), marked gone, and sends nothing by itself. */
  private async gone() {
    if (!await vaultAvailable(this.d.fetch)) throw new VaultError(N('the server keeps no cloud saves now'), 'VAULT-OFF');
    if (this.link) this.setLink({ ...this.link, gone: true, earlier: null });
    this.set({ s: 'gone' });
  }

  /** a version of the keyphrase's that differs from the one here, which changed since too (both) or not; here: the
   *  files here, cloud: theirs (none: not fetched) */
  private clash(l: Link, theirs: Remote | null, both: boolean, here: SaveFile[], cloud?: SaveFile[], why?: 'drops' | 'thin', stays?: string[]): void {
    this.set({ s: 'conflict', why: why ?? (theirs && theirs.saved < l.saved ? 'older' : both ? 'both' : 'bare'), theirs, here: sideOf(here),
      cloud: cloud ? sideOf(cloud, theirs?.saved) : theirs ? { saved: theirs.saved, profiles: null } : null, ...stays?.length ? { stays } : {} });
  }

  /** the files here (hash) about to be sent under the link: remembered until an answer comes (Link.sent) */
  private sending(l: Link, hash: string): Link {
    this.setLink({ ...l, sent: [...(l.sent ?? []).filter(h => h !== hash), hash].slice(-SENT_MAX) });
    return this.link!;
  }

  /** the keyphrase's version now, its files and their hash; null: none */
  private async current(k: Keys, api: VaultApi): Promise<{ remote: Remote; files: SaveFile[]; hash: string } | null> {
    const r = await api.get();
    if (!r) return null;
    if (!('box' in r)) throw incomplete();
    const files = await this.filesOf(k, r.box);
    return { remote: r.remote, files, hash: await contentHash(files) };
  }

  /**
   * The files here sent over rev (null: as the keyphrase's first version again).  When the server has another version:
   * the same files are linked; this browser's own, sent before without an answer (Link.sent), is linked and sent over,
   * once; another's is a conflict.
   */
  private async send(l: Link, k: Keys, files: SaveFile[], hash: string, rev: string | null): Promise<void> {
    const api = this.api(k), box = await this.boxOf(k, files);
    for (let again = false; ; again = true) {
      l = this.sending(l, hash);
      const r = await api.put(box, rev);
      if ('saved' in r) return this.linked(l.phrase, r.saved, hash, files);
      const c = r.conflict && await this.current(k, api);
      if (!c) return this.gone();
      if (c.hash === hash) return this.linked(l.phrase, c.remote, hash, files);
      if (again || !l.sent?.includes(c.hash)) {
        if (l.gone) this.setLink(l = { ...l, gone: false });         // another browser sent it again first
        return this.clash(l, c.remote, true, files, c.files);
      }
      l = this.relinked(l, c);
      rev = c.remote.rev;
    }
  }

  /** the link at a version of this browser's own whose answer never came */
  private relinked(l: Link, c: { remote: Remote; files: SaveFile[]; hash: string }): Link {
    this.setLink({ ...l, rev: c.remote.rev, hash: c.hash, saved: c.remote.saved, earlier: c.remote.earlier,
      profiles: profilesIn(c.files).length, v: LINK_V, sent: undefined, gone: undefined, torn: tornCount(c.files) || undefined,
      whole: wholeCount(c.files) });
    return this.link!;
  }

  /** A new keyphrase for the saves here, their first version sent: the keyphrase. */
  create(): Promise<string> {
    return this.run(N('Making passphrase'), async () => {
      for (;;) {
        const phrase = newPhrase(), k = await this.keysFor(phrase), files = await this.d.files();
        const r = await this.api(k).put(await this.boxOf(k, files), null);
        if ('conflict' in r) continue;                  // one of 2^83 taken already: never, but not this one
        await this.linked(phrase, r.saved, await contentHash(files), files, true);
        return phrase;
      }
    });
  }

  /** What a keyphrase holds (its zip, for the player to look at before it replaces the saves here), or null: none. */
  peek(phrase: string, earlier = false): Promise<{ zip: Uint8Array; remote: Remote } | null> {
    return this.run(N('Loading'), async () => {
      const k = await this.keysFor(phrase), r = await this.api(k).get(null, earlier);
      if (!r || 'same' in r) return null;
      return { zip: await unpack(await unseal(k.key, r.box)), remote: r.remote };
    });
  }

  /** The saves here are a keyphrase's version now (the player restored what peek() gave): this browser follows it. */
  adopt(phrase: string, remote: Remote, theirs?: SaveFile[]): Promise<void> {
    return this.run(N('Linking'), async () => {
      // theirs: the version's own files (kept()): what was written of them, not what came after (a game's save)
      const files = theirs ?? await this.d.files();
      await this.linked(phrase, remote, await contentHash(files), files, true);
    });
  }

  /** a version's files as a keyphrase keeps them (one sent before 2026-10-25 may have covers, black ones as they were,
   *  the other browser's graphics options and its device's settings) */
  private async filesOf(k: Keys, box: Uint8Array): Promise<SaveFile[]> {
    return this.keptOf(await unzip(await unpack(await unseal(k.key, box))));
  }
  private keptOf(entries: { name: string; data: Uint8Array; mtime?: number }[]): SaveFile[] {
    return kept(entries.map(e => ({ path: e.name, data: e.data, mtime: e.mtime })));
  }
  /** a question for the player about the saves here (why), the cloud's version beside them as it is now */
  private async ask(why: 'empty' | 'wiped' | 'fewer', l: Link, files: SaveFile[]): Promise<void> {
    let c: { remote: Remote; files: SaveFile[] } | null = null;
    try { const k = await this.keysFor(l.phrase); c = await this.current(k, this.api(k)); } catch { /* not known */ }
    this.set({ s: 'conflict', why, theirs: c?.remote ?? null, here: sideOf(files), cloud: c ? sideOf(c.files, c.remote.saved) : null });
  }

  /** a version in place of the saves here, and the link to it; expect: only if the saves here are still the ones of
   *  that hash when it writes (an automatic take, decided before a restore of the Saves tab's wrote), or nothing is
   *  written: the next push asks */
  private async take(phrase: string, k: Keys, box: Uint8Array, remote: Remote, expect?: string) {
    const still = expect === undefined ? undefined : async () => await contentHash(await this.d.files()) === expect;
    if (await this.d.restore(await unpack(await unseal(k.key, box)), still) === false) {
      this.d.log?.('[app] keyphrase: another browser\'s saves came, but the saves here changed before they were written: not taken');
      return;
    }
    const files = await this.d.files();
    await this.linked(phrase, remote, await contentHash(files), files, true);
    this.onTaken(remote);
  }

  /** Another browser's newer version, taken when the saves here did not change since they synced (timeoutMs: the server's answer, at most). */
  pull(o: { timeoutMs?: number } = {}): Promise<void> {
    if (!this.link || this.state.s === 'conflict') return Promise.resolve();
    return this.run(N('Checking'), async () => {
      let l = this.link!;
      const k = await this.keysFor(l.phrase), r = await this.api(k, o.timeoutMs).get(l.rev);
      if (!r) return this.gone();
      if (l.gone) this.setLink(l = { ...l, gone: false });               // there again (a server's folder back)
      if ('same' in r) { if (r.same.earlier !== l.earlier) this.setLink({ ...l, earlier: r.same.earlier }); return; }
      const plain = await unpack(await unseal(k.key, r.box)), guarded = zipComment(plain) === GUARDED;
      const files = await this.d.files(), hash = await contentHash(files), theirs = this.keptOf(await unzip(plain));
      l = await this.upgraded(hash);
      const theirHash = await contentHash(theirs);
      // the same as here (a settings.ini both pages brought up to date)
      if (theirHash === hash) return this.linked(l.phrase, r.remote, hash, files);
      // this browser's own, sent before without an answer: the link follows it, and push() sends what changed since
      if (l.sent?.includes(theirHash)) { this.relinked(l, { remote: r.remote, files: theirs, hash: theirHash }); return; }
      // taken only when the saves here did not change since, it is not older than theirs, and it keeps their profiles
      const older = r.remote.saved < l.saved, bare = !profilesIn(theirs).length && profilesIn(files).length > 0;
      if (hash === l.hash && !older && !bare) {
        // never a profile lost by itself: one of theirs the restore cannot take in, or (from an older page, which may
        // have sent a folder the browser had deleted) one here theirs lacks; the player picks
        // (by their folders and by the names a restore gives them: a broken one this browser holds mended)
        const out = await this.d.leftOut?.(theirs) ?? [];
        const names = new Set([...profilesIn(theirs), ...guarded ? [] : this.d.named?.(theirs) ?? []].map(n => n.toLowerCase()));
        const lacks = guarded ? [] : profilesIn(files).filter(n => !names.has(n.toLowerCase()));
        if (!out.length && !lacks.length) return this.take(l.phrase, k, r.box, r.remote, hash);
        this.d.log?.(out.length ? `[app] keyphrase: another browser's saves came, but ${out.length} profile(s) of theirs would not come in with them: asked`
          : `[app] keyphrase: another browser's saves came from an older page, without ${lacks.length} profile(s) here: asked`);
        return this.clash(l, r.remote, false, files, theirs, out.length ? 'drops' : 'thin', out);
      }
      this.clash(l, r.remote, hash !== l.hash, files, theirs);
    });
  }

  /**
   * The saves here sent, when they changed since they synced.  playing: a game runs, so a profile read halfway
   * through a save (its checksums do not hold) waits for the next save or the game's end.
   */
  push(o: { playing?: boolean } = {}): Promise<void> {
    if (!this.link || this.link.gone || this.state.s === 'conflict' || this.holding) return Promise.resolve();
    return this.run(N('Syncing'), async () => {
      if (this.holding) return;                              // held since this was asked for
      const files = await this.d.files(), torn = tornCount(files);
      // more profiles torn than it synced, during a game: one being saved now, sent once whole (its next save, the game's end)
      if (o.playing && torn > (this.link?.torn ?? 0)) return;
      const hash = await contentHash(files), l = await this.upgraded(hash);
      // unchanged since, unless a send was not answered: the server may have that one (the files here went back since)
      if (hash === l.hash && !l.sent?.length) return;
      if (!profilesIn(files).length && l.profiles > 0) return this.ask('empty', l, files);
      // the folder deleted since it synced (its mark gone) and not whole again: what is here may be one profile the
      // game saved since, never sent over the cloud's by itself (a profile deleted on purpose leaves the mark)
      await this.marking;
      if (l.held || l.folder && !l.unmarked && this.d.folder && await this.d.folder().catch(() => l.folder) !== l.folder) {
        this.d.log?.(`[app] keyphrase: this browser's save folder was deleted since it synced${l.held ? ', and its saves were not all put back' : ''}: nothing sent, asked`);
        return this.ask('wiped', l, files);
      }
      // a profile not whole that was when it synced (an empty file a failed write left, a save torn), or fewer whole ones
      // than it synced but for those deleted on purpose (Link.dropped): never sent by itself
      const whole = profileFiles(files).length - torn, was = l.whole ?? l.profiles;
      if (torn > (l.torn ?? 0) || whole < was - (l.dropped ?? 0)) {
        this.d.log?.(`[app] keyphrase: ${whole} whole profile(s) here and ${torn} not, ${was} and ${l.torn ?? 0} when it synced` +
          `${l.dropped ? `, ${l.dropped} deleted on purpose` : ''}: nothing sent, asked`);
        return this.ask('fewer', l, files);
      }
      if (this.holding) return;
      await this.send(l, await this.keysFor(l.phrase), files, hash, l.rev);
    });
  }

  /** A conflict settled for the saves here: sent over whatever the keyphrase has. */
  keepMine(): Promise<void> {
    const st = this.state;
    if (!this.link || st.s !== 'conflict') return Promise.resolve();
    return this.run(N('Syncing'), async () => {
      const l = this.link!, files = await this.d.files(), hash = await contentHash(files), k = await this.keysFor(l.phrase);
      const box = await this.boxOf(k, files), api = this.api(k);
      this.sending(l, hash);
      let r = await api.put(box, st.theirs?.rev ?? l.rev);
      if ('conflict' in r && r.conflict) r = await api.put(box, r.conflict.rev);
      if ('saved' in r) return this.linked(l.phrase, r.saved, hash, files, true);
      if (!r.conflict) return this.gone();
      this.clash(l, r.conflict, true, files);
    });
  }

  /** The keyphrase gone from the server: the saves here sent under it again (a new keyphrase there). */
  resend(): Promise<void> {
    if (!this.link?.gone) return Promise.resolve();
    return this.run(N('Syncing'), async () => {
      const l = this.link!, files = await this.d.files(), hash = await contentHash(files);
      await this.send(l, await this.keysFor(l.phrase), files, hash, null);
    });
  }

  /** A conflict settled for the keyphrase's version: it replaces the saves here. */
  takeTheirs(): Promise<void> {
    if (!this.link || this.state.s !== 'conflict') return Promise.resolve();
    return this.run(N('Loading'), async () => {
      const l = this.link!, k = await this.keysFor(l.phrase), r = await this.api(k).get();
      if (!r) return this.gone();
      if ('box' in r) await this.take(l.phrase, k, r.box, r.remote);
    });
  }

  /** The saves here replaced while a conflict waited (vault-ui.ts Put back): the next pull and push judge it again. */
  reopen() { if (this.state.s === 'conflict') this.set(this.rest()); }

  /** This browser stops syncing (after what it is doing); the saves stay here and on the server. */
  forget(): Promise<void> {
    return this.run(N('Stopping'), async () => this.setLink(null));
  }

  /** The keyphrase and its versions deleted from the server, and this browser unlinked. */
  remove(): Promise<void> {
    if (!this.link) return Promise.resolve();
    return this.run(N('Deleting'), async () => {
      const k = await this.keysFor(this.link!.phrase);
      await this.api(k).remove();
      this.setLink(null);
    });
  }
}
