// Last.fm: the songs a player hears in the game, EA Trax's and the Radio's, scrobbled to their Last.fm profile.  The server signs
// and forwards the calls (server-rs src/app/lastfm.rs: the API account's secret lives there); the page keeps the session key, on
// this device only.  Connecting: Last.fm's "allow access" page in a popup, which comes back to /lastfm.html (lastfm-callback.ts)
// with a token; that page trades it for the session key and closes.
//
// What counts as heard is the runtime's (radio_proto.h song_*): the time a song was mixed and not paused, and whether it played to
// its end.  Last.fm's rules: a track longer than 30 s is scrobbled once it has been heard half its length or 4 minutes.  An EA Trax
// song's length is not known: it is scrobbled after 4 minutes, or at its end after 30 s.  "Now playing" goes once a song is heard.
import type { Lines, RadioLink, Song } from './radio-link';

export interface Session { name: string; sk: string }
interface Scrobble { artist: string; track: string; album?: string; duration?: number; timestamp: number }
/** the Radio's track served last (radio.ts lastTrack) */
export interface FolderTrack { tags: Promise<Lines>; plaque: Promise<Lines>; seconds: number }

const SESSION_KEY = 'nfsu2.lastfm', QUEUE_KEY = 'nfsu2.lastfm.queue';
/** what the callback page did last ({result: 'ok' | an error, at}): the row says it */
export const RESULT_KEY = 'nfsu2.lastfm.result';
export const AUTH_PAGE = 'https://www.last.fm/api/auth/';
const MIN_S = 30, ENOUGH_S = 240, QUEUE_MAX = 50, RETRY_MS = 120_000;

const read = <T>(key: string): T | null => { try { return JSON.parse(localStorage.getItem(key) ?? 'null') as T | null; } catch { return null; } };
const write = (key: string, v: unknown) => { try { if (v === null) localStorage.removeItem(key); else localStorage.setItem(key, JSON.stringify(v)); } catch { /* a private window keeps nothing */ } };

export function readSession(): Session | null {
  const s = read<Session>(SESSION_KEY);
  return s && typeof s.name === 'string' && typeof s.sk === 'string' && s.sk ? s : null;
}
export const keepSession = (s: Session | null) => write(SESSION_KEY, s);

/** /lastfm on this server: its API key (null: the server has no Last.fm), and the calls it signs */
export class LastfmApi {
  async key(): Promise<string | null> {
    const r = await fetch('/lastfm', { cache: 'no-store' });
    if (r.status === 404) return null;
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const j = await r.json() as { key?: unknown };
    return typeof j.key === 'string' && j.key ? j.key : null;
  }
  /** the server's answer: status and body ({error} when refused); throws when it could not be asked */
  async call(body: Record<string, unknown>): Promise<{ status: number; json: Record<string, unknown> }> {
    const r = await fetch('/lastfm', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) });
    let json: Record<string, unknown> = {};
    try { json = await r.json() as Record<string, unknown>; } catch { /* not JSON: the status says it */ }
    return { status: r.status, json };
  }
}

/** Last.fm's "allow access" page, which sends the player back to /lastfm.html with a token */
export const authLink = (key: string, origin = location.origin) => `${AUTH_PAGE}?api_key=${encodeURIComponent(key)}&cb=${encodeURIComponent(`${origin}/lastfm.html`)}`;

/** a title without its artist in front ("Artist - Title"), as the game's plaque shows it (radio-tags.ts plaqueTags) */
const ownTitle = (t: Lines) => t.artist && t.title.toLowerCase().startsWith(`${t.artist} - `.toLowerCase()) ? t.title.slice(t.artist.length + 3) : t.title;

interface Current {
  seq: number; kind: Song['kind']; lines: Lines | null; duration?: number;
  start: number;                         // unix seconds when it began (now less the time heard at first sight)
  ms: number; ended: boolean; told: boolean; done: boolean;
}

export interface ScrobblerDeps {
  api: Pick<LastfmApi, 'call'>;
  /** the Radio's track served last, null without the Radio */
  folder(): FolderTrack | null;
  /** the session key stopped working (the player took the access back on Last.fm): forgotten here */
  dropped(): void;
  log(line: string): void;
  now?(): number;
}

/** Follows the song playing (tick, about once a second while a game runs) and tells Last.fm. */
export class Scrobbler {
  private cur: Current | null = null;
  private queue: Scrobble[] = read<Scrobble[]>(QUEUE_KEY)?.filter(s => s && typeof s.artist === 'string') ?? [];
  private retryAt = 0;
  private sending = false;
  constructor(private deps: ScrobblerDeps) {}
  private now() { return this.deps.now?.() ?? Date.now(); }

  /** song: the runtime's (null while it writes it); none: no game, or no session */
  tick(song: Song | null, session: Session | null) {
    if (!session) { this.cur = null; return; }
    if (this.queue.length && this.now() >= this.retryAt) void this.flush(session);
    if (!song) return;
    if (!this.cur || song.seq !== this.cur.seq) this.begin(song);
    const c = this.cur!;
    c.ms = song.ms; c.ended = song.ended;
    if (c.kind === 'none' || !c.lines || !c.lines.artist || !c.lines.title) return;
    if (!c.told && c.ms >= 1000) { c.told = true; void this.send(session, 'playing', this.track(c)); }
    const enough = Math.min(ENOUGH_S, c.duration ? c.duration / 2 : ENOUGH_S) * 1000;
    if (!c.done && (c.duration ?? MIN_S + 1) > MIN_S && (c.ms >= enough || (c.ended && c.ms >= MIN_S * 1000))) {
      c.done = true;
      void this.send(session, 'scrobble', { ...this.track(c), timestamp: c.start });
    }
  }

  /** a new song: its lines (the Radio's track: its own tags in place of the plaque's ASCII, when it is the one the game shows) */
  private begin(song: Song) {
    const c: Current = { seq: song.seq, kind: song.kind, lines: song.kind === 'game' ? song.lines : null, start: Math.round((this.now() - song.ms) / 1000),
      ms: song.ms, ended: song.ended, told: false, done: false };
    this.cur = c;
    const t = song.kind === 'folder' ? this.deps.folder() : null;
    if (!t) { if (song.kind === 'folder') c.lines = song.lines; return; }
    void Promise.all([t.plaque, t.tags]).then(([plaque, tags]) => {
      const same = plaque.artist === song.lines.artist && plaque.title === song.lines.title;
      // (a track without an artist shows "Unknown artist" on the plaque: nothing to scrobble)
      c.lines = same ? { artist: tags.artist.trim(), title: ownTitle(tags).trim(), album: tags.album.trim() } : song.lines;
      if (same) c.duration = Math.round(t.seconds);
    }, () => { c.lines = song.lines; });
  }

  private track(c: Current): Omit<Scrobble, 'timestamp'> {
    const l = c.lines!;
    return { artist: l.artist, track: l.title, ...(l.album ? { album: l.album } : {}), ...(c.duration ? { duration: c.duration } : {}) };
  }

  /** a call, and what came of it: 'later' when Last.fm could not be asked or asks to wait */
  private async post(session: Session, op: 'playing' | 'scrobble', s: Omit<Scrobble, 'timestamp'> & { timestamp?: number }): Promise<'ok' | 'later' | 'refused'> {
    let r: { status: number; json: Record<string, unknown> } | null = null;
    try { r = await this.deps.api.call({ op, sk: session.sk, ...s }); } catch (e) { this.deps.log(`[lastfm] ${op} not sent: ${e}`); }
    const error = r ? String(r.json.error ?? '') : 'down';
    if (r?.status === 200) {
      this.deps.log(`[lastfm] ${op === 'playing' ? 'now playing' : r.json.ignored ? 'scrobble ignored by Last.fm' : 'scrobbled'}`);
      return 'ok';
    }
    if (error === 'session') { this.deps.log('[lastfm] the session key no longer works: disconnected'); this.cur = null; this.deps.dropped(); return 'refused'; }
    this.deps.log(`[lastfm] ${op} refused: ${error || r?.status}`);
    return error === 'down' || error === 'rate' ? 'later' : 'refused';
  }

  /** a scrobble that cannot go now is kept (on this device) and sent later; a sent one sends the kept ones */
  private async send(session: Session, op: 'playing' | 'scrobble', s: Omit<Scrobble, 'timestamp'> & { timestamp?: number }) {
    const got = await this.post(session, op, s);
    if (op !== 'scrobble') return;
    if (got === 'later' && s.timestamp) {
      this.queue = [...this.queue, s as Scrobble].slice(-QUEUE_MAX);
      this.retryAt = this.now() + RETRY_MS;
      write(QUEUE_KEY, this.queue);
      this.deps.log(`[lastfm] ${this.queue.length} scrobbles kept for later`);
    } else if (got === 'ok' && this.queue.length) void this.flush(session);
  }

  /** the scrobbles kept, oldest first, until one cannot go yet */
  private async flush(session: Session) {
    if (this.sending) return;
    this.sending = true;
    this.retryAt = this.now() + RETRY_MS;
    try {
      while (this.queue.length) {
        const s = this.queue[0];
        if (s.timestamp > this.now() / 1000 - 14 * 86_400 + 60 && await this.post(session, 'scrobble', s) === 'later') break;   // (Last.fm takes two weeks)
        this.queue = this.queue.filter(q => q !== s);
        write(QUEUE_KEY, this.queue.length ? this.queue : null);
      }
    } finally { this.sending = false; }
  }
}

/** Runs the scrobbler while a game runs (port: its radio's words), about once a second; the runtime keeps the song's words up only
 *  while a session is there. */
export function scrobbleWhilePlaying(port: () => RadioLink | null, s: Scrobbler, session: () => Session | null): () => void {
  let link: RadioLink | null = null;
  const timer = setInterval(() => {
    const l = port(), have = session();
    if (l !== link) { link = l; s.tick(null, null); }
    l?.listen(!!have);
    s.tick(l && have ? l.song() : null, l ? have : null);
  }, 1000);
  return () => clearInterval(timer);
}
