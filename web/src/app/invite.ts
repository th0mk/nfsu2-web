// Invite links (a player's request: "why can't we just invite them to rooms using links?"): this server's start page
// with a free-roam room or crew after its '#', nfsu2.xyz/#room=Blue%20Room or nfsu2.xyz/#crew=Night%20Owls (with the
// sender's room after it when the sender is in one: crew-mates meet only within one room).  The page that opens one
// asks the player whether to join (shell.ts); nothing changes before Join, so a link cannot change a player's game by
// itself (the audit's PAGE-1).  The names stay in the address's fragment, which a browser never sends: the server and
// its logs never see them.  Pure (no DOM), so its test runs under node.
import { N, type Vars } from './i18n';
import { checkCrew } from './crews';
import { checkSetting } from './settings';
import { roomHash, roomText } from './players';

/** what a link joins: a room, a crew, or a crew in a room ('' with a crew: in no room, everyone's) */
export interface Invite { room?: string; crew?: string }

/** Unicode's blanks (each one a blank to roomFold), and the invisible characters it leaves out too */
const BLANK = '\\s\\u00a0\\u1680\\u2000-\\u200a\\u2028\\u2029\\u202f\\u205f\\u3000';
const HIDDEN = '\\u00ad\\u200b-\\u200d\\u2060\\ufeff';
const BLANKS = new RegExp(`[${BLANK}]+`, 'gu'), INVISIBLE = new RegExp(`[${HIDDEN}]`, 'gu'), UNSEEN = new RegExp(`[${BLANK}${HIDDEN}]`, 'u');
/** a typed or linked name as the settings keep it: composed (NFC), its blanks single spaces, the invisible characters
 *  out, trimmed */
export const tidyName = (s: string) => roomText(s).replace(INVISIBLE, '').replace(BLANKS, ' ').trim();

/** A typed Room: ok, the name to keep; or why not, an English text for t() with its names in v (null: nothing typed).
 *  settings.ini reads a # or ; after a blank as a comment (the free-roam keys' "value ; comment"), which would cut the
 *  name short there, so a name has none; else the setting's own check (at most 63 bytes, nothing that reads as a slur). */
export type RoomCheck = { ok: true; name: string } | { ok: false; why: string | null; v?: Vars };
export function checkRoom(typed: string): RoomCheck {
  const name = tidyName(typed);
  if (!roomHash(name)) return { ok: false, why: null };
  if (/(^|[ \t])[#;]/.test(name)) return { ok: false, why: N('no # or ; after a space in a room name') };
  const c = checkSetting('roam_room', name);
  if (c && !c.ok) return { ok: false, why: c.why, v: c.v };
  return { ok: true, name };
}

/** a name in a link: letters, digits, - _ ~ and the visible characters past ASCII as they are (a chat shows them so),
 *  every other as %XX of its UTF-8: blanks, the URL's own signs, and the . , ) ! that a chat would cut off a link's end */
function enc(s: string): string {
  let out = '';
  for (const ch of s) {
    const c = ch.codePointAt(0)!;
    if (/[A-Za-z0-9_~-]/.test(ch) || (c >= 0xa0 && !(c >= 0xd800 && c <= 0xdfff) && !UNSEEN.test(ch))) { out += ch; continue; }
    let e: string;
    try { e = encodeURIComponent(ch); } catch { e = '%EF%BF%BD'; }           // (half a surrogate pair: U+FFFD, as UTF-8 has it)
    out += e.replace(/[!'()*.]/g, c => `%${c.charCodeAt(0).toString(16).toUpperCase()}`);
  }
  return out;
}

/** The link to this server (origin: 'https://nfsu2.xyz') that joins inv: the crew first, then the room (a crew's
 *  with an empty room too: its friend leaves a room of their own to meet the sender in everyone's). */
export function inviteLink(origin: string, inv: Invite): string {
  const p: string[] = [];
  if (inv.crew) p.push(`crew=${enc(inv.crew)}`);
  if (inv.room || (inv.crew && inv.room !== undefined)) p.push(`room=${enc(inv.room)}`);
  return `${origin}/#${p.join('&')}`;
}

/** The invite in a page address's fragment ('#room=...'), its names tidied; null: none (another fragment, a name
 *  that does not decode, or a room of blanks alone). */
export function readInvite(hash: string): Invite | null {
  const inv: Invite = {};
  for (const part of hash.replace(/^#/, '').split('&')) {
    const eq = part.indexOf('='), key = eq < 0 ? part : part.slice(0, eq);
    if (eq < 0 || (key !== 'room' && key !== 'crew')) continue;
    let v: string;
    try { v = decodeURIComponent(part.slice(eq + 1).replace(/\+/g, ' ')); } catch { return null; }
    inv[key] = tidyName(v);
  }
  if (!inv.crew) delete inv.crew;
  if (inv.room === '' && !inv.crew) delete inv.room;
  return inv.room || inv.crew ? inv : null;
}

/** Whether a link's names can be joined: each as the Room row and the Crews box check a typed one.  why: an English
 *  text for t(), with the name it is about as {name} and the check's own names. */
export type InviteCheck = { ok: true } | { ok: false; why: string; v: Vars };
export function checkInvite(inv: Invite): InviteCheck {
  if (inv.room) {
    const r = checkRoom(inv.room);
    if (!r.ok && r.why) return { ok: false, why: r.why, v: { name: inv.room, ...r.v } };
  }
  if (inv.crew) {
    const c = checkCrew(inv.crew, []);
    if (!c.ok && c.why) return { ok: false, why: c.why, v: { name: inv.crew } };
  }
  return { ok: true };
}

/** whether two names are one room or one crew (the hash's rules: capitals, blanks and invisible characters do not count) */
export const sameName = (a: string, b: string) => roomHash(a) === roomHash(b);
