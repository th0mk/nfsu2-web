// How many play on this server now, for the start page's Multiplayer settings: the server's /players (three
// counts, whether it is full, how many wait in line for a place and whether free roam is busy, nothing else; with a Room setting, how many are in
// free roam in that room too, and with crews how many in each).  Asked every 15 s, and only while the start page is visible and no game runs; a
// server without the route shows nothing.  Whether the server answered at all goes to reached() (offline mode).

/** roam: driving in free roam or paused there (null, or missing from an older server: not shown);
 *  roamBusy: free roam gives fewer cars a player now (the server's hub cut its links), so the first Play does not ask about it;
 *  room: of roam, those in the room asked for (/players?room=, missing without one or from an older server);
 *  crews: of roam, those in each crew asked for, in the order asked (/players?crews=, the same) */
export interface PlayerCounts {
  players: number; online: number | null; roam?: number | null; full?: boolean; roamBusy?: boolean; room?: number; crews?: number[];
  /** pages waiting in line for a place (missing from an older server) */
  queue?: number;
}

import { t, tn, type Msg } from './i18n';
import { at } from '../base';

const EVERY_MS = 15_000;

/** One character of a Room's or a crew's name as its hash takes it, as roam_core.c room_fold does (the two must agree
 *  on every character: web/test/players.test.mjs and tests/roam_unit.c check the same sum): 0 for a blank or an
 *  invisible one, capitals small in Latin, Greek and Cyrillic, Turkish i as i, final sigma as sigma, full-width ASCII
 *  as ASCII; any other character is itself. */
export function roomFold(c: number): number {
  if (c === 0x20 || (c >= 0x09 && c <= 0x0d) || c === 0xa0 || c === 0xad || c === 0x1680 || (c >= 0x2000 && c <= 0x200d) ||
      c === 0x2028 || c === 0x2029 || c === 0x202f || c === 0x205f || c === 0x2060 || c === 0x3000 || c === 0xfeff) return 0;
  if (c >= 0xff01 && c <= 0xff5e) c -= 0xfee0;
  if (c >= 0x41 && c <= 0x5a) return c + 32;
  if (c < 0xc0) return c;
  if (c <= 0xde) return c === 0xd7 ? c : c + 32;
  if (c >= 0x100 && c <= 0x17f) {
    if (c === 0x130 || c === 0x131) return 0x69;
    if (c === 0x178) return 0xff;
    if (c <= 0x137 || (c >= 0x14a && c <= 0x177)) return c | 1;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) return c & 1 ? c + 1 : c;
    return c;
  }
  if (c >= 0x218 && c <= 0x21f) return c | 1;
  if (c >= 0x391 && c <= 0x3ab) return c === 0x3a2 ? c : c + 32;
  if (c === 0x386) return 0x3ac;
  if (c >= 0x388 && c <= 0x38a) return c + 37;
  if (c === 0x38c) return 0x3cc;
  if (c === 0x38e || c === 0x38f) return c + 63;
  if (c === 0x3c2) return 0x3c3;
  if (c >= 0x400 && c <= 0x40f) return c + 80;
  if (c >= 0x410 && c <= 0x42f) return c + 32;
  if ((c >= 0x460 && c <= 0x481) || (c >= 0x48a && c <= 0x4bf) || (c >= 0x4d0 && c <= 0x52f)) return c | 1;
  if (c === 0x4c0) return 0x4cf;
  if (c >= 0x4c1 && c <= 0x4ce) return c & 1 ? c + 1 : c;
  return c;
}

/** A Room's or a crew's name as the game is given it: in Unicode's composed form (NFC), so an "é" typed as e and an
 *  accent is the "é" of a keyboard */
export const roomText = (name: string) => name.normalize('NFC');

const utf8 = new TextEncoder();
/** The Room setting's hash, as roam_core.c rc_room_hash makes it of roomText(name) (FNV-1a of its UTF-8 with each
 *  character as roomFold has it: "Blue Room" is "blueroom"; 0: no room), for /players?room= (a crew is the hash of its
 *  name too) */
export function roomHash(name: string): number {
  let h = 0x811c9dc5, kept = 0;
  for (const ch of roomText(name)) {
    const c = roomFold(ch.codePointAt(0)!);
    if (!c) continue;
    for (const b of utf8.encode(String.fromCodePoint(c))) h = Math.imul(h ^ b, 0x01000193) >>> 0;
    kept++;
  }
  return !kept ? 0 : h || 1;
}

/** The line the page shows ('' for an answer it cannot read); room: the player's Room setting, when free roam is on
 *  ('' for none): free roam shows only that room's players, which the line says, with how many are there. */
export function playersText(c: PlayerCounts, room = ''): string {
  if (!Number.isInteger(c.players) || c.players < 0) return '';
  if (c.players === 0) return t('On this server now: nobody is playing.');
  const known = (k: number | null | undefined): k is number => Number.isInteger(k) && k! >= 0;
  const list = [tn(c.players, '{n} player', '{n} players')];
  if (known(c.online)) list.push(t('{n} in Online mode', { n: c.online }));
  if (known(c.roam)) list.push(t('{n} in free roam', { n: c.roam }));
  room = room.trim();
  const inRoom = !room ? '' : ' ' + (!known(c.room) ? t('You are in room "{room}".', { room })
    : c.room === 0 ? t('You are in room "{room}": nobody in free roam there.', { room }) : t('You are in room "{room}": {n} in free roam there.', { room, n: c.room }));
  const waiting = Number.isInteger(c.queue) && c.queue! > 0 ? c.queue! : 0;
  const full = c.full !== true ? ''
    : ' ' + (waiting ? tn(waiting, 'The server is full and {n} is waiting in line for a place. At Play you can wait in line, playing alone until it is your turn, or play offline.',
      'The server is full and {n} are waiting in line for a place. At Play you can wait in line, playing alone until it is your turn, or play offline.')
      : t('The server is full: playing with others waits until a place frees up.'));
  return t('On this server now: {list}.', { list: list.join(', ') }) + inRoom + full;
}

/** 1st, 2nd, 3rd, 4th, ..., 11th, 12th, 13th, ..., 21st */
function ordinal(k: number): string {
  const t = k % 100, u = k % 10;
  return `${k}${t >= 11 && t <= 13 ? 'th' : u === 1 ? 'st' : u === 2 ? 'nd' : u === 3 ? 'rd' : 'th'}`;
}

/** The line over the game while it waits in line for a place on a full server (net's NetQueue): where it is, and
 *  about how long that is (once the server can tell). */
export function queueText(q: { pos: number; of: number; eta?: number }): string {
  const e = q.eta;
  const wait = e === undefined ? '' : e < 60 ? t('less than a minute') : e < 90 * 60 ? t('about {n} min', { n: Math.round(e / 60) }) : t('about {n} h', { n: Math.round(e / 3600) });
  const v = { pos: ordinal(q.pos), n: q.pos, of: q.of, wait };
  if (q.pos <= 1) return wait ? t('The server is full. In line for a place: next ({wait}).', v) : t('The server is full. In line for a place: next.');
  return wait ? t('The server is full. In line for a place: {pos} of {of} ({wait}).', v) : t('The server is full. In line for a place: {pos} of {of}.', v);
}

/** Whether the server answers (any answer but a 5xx from a proxy in front of it), within ms. */
export async function serverAnswers(ms: number): Promise<boolean> {
  try { return (await fetch(at('players'), { method: 'HEAD', cache: 'no-store', signal: AbortSignal.timeout(ms) })).status < 500; }
  catch { return false; }
}

/** How many are in free roam in each crew asked for (their hashes, in the order asked), by hash; none from an older
 *  server (no crews in its answer) or for a count it does not give. */
export function crewCounts(c: PlayerCounts, asked: readonly number[]): Map<number, number> {
  const m = new Map<number, number>();
  if (Array.isArray(c.crews)) asked.forEach((h, i) => { const n = c.crews![i]; if (Number.isInteger(n) && n >= 0) m.set(h, n); });
  return m;
}

/** How many are in free roam in each of these crews now (crewCounts), asked once, whatever the page does, and
 *  without watchPlayers' line or its say on whether the server answers: the page menu's Crews while a game runs.  The
 *  player counts there too, once its game is in free roam in that crew (the server's hub heard its HELLO).  null: no
 *  answer within ms. */
export async function askCrews(hashes: readonly number[], ms: number): Promise<Map<number, number> | null> {
  const mine = hashes.slice(0, 4);
  if (!mine.length) return new Map();
  try {
    const r = await fetch(at(`players?crews=${mine.join(',')}`), { cache: 'no-store', signal: AbortSignal.timeout(ms) });
    return r.ok ? crewCounts(await r.json() as PlayerCounts, mine) : null;
  } catch { return null; }
}

/** show(line, crews) at once and every 15 s while idle() and the page is visible (crews: crewCounts); reached(whether
 *  the server answered); room(): the Room setting while free roam is on ('' for none); crews(): the Crews setting's
 *  hashes ([] for none, at most 4); last(): the newest answer (null before the first, or from a server without
 *  the route); fresh(ms): asked now, whatever the page does (Play: whether the server is full), within ms, then the
 *  newest answer.  An answer that comes after a later question's is not shown: it was asked with the settings of
 *  before (the page's first question goes before settings.ini is read, the next one at once after it). */
export function watchPlayers(show: (line: Msg, crews: Map<number, number>) => void, idle: () => boolean, reached?: (ok: boolean) => void,
                             room: () => string = () => '', crews: () => number[] = () => []) {
  let gone = false, last: PlayerCounts | null = null, asked = 0;
  const get = async (ms: number) => {
    if (gone) return;
    const n = ++asked;
    try {
      const name = room(), hash = roomHash(name), mine = crews().slice(0, 4);
      const q = [hash ? `room=${hash}` : '', mine.length ? `crews=${mine.join(',')}` : ''].filter(Boolean).join('&');
      const r = await fetch(at(q ? `players?${q}` : 'players'), { cache: 'no-store', signal: AbortSignal.timeout(ms) });
      reached?.(r.status < 500);
      if (r.status === 404) { gone = true; last = null; show(() => '', new Map()); return; }
      if (!r.ok) return;
      const c = await r.json() as PlayerCounts, inRoom = hash ? name : '';
      if (n !== asked) return;
      last = c; show(() => playersText(c, inRoom), crewCounts(c, mine));
    } catch { reached?.(false); }
  };
  const ask = async () => { if (document.visibilityState === 'visible' && idle()) await get(5000); };
  window.setInterval(() => void ask(), EVERY_MS);
  document.addEventListener('visibilitychange', () => void ask());
  void ask();
  return { now: () => void ask(), last: () => last, fresh: async (ms: number) => { await get(ms); return last; } };
}
