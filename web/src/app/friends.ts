// Free roam's friends (docs/web/PLAYING.md, Friends): two players whose cars touch front bumpers for 3 seconds may
// add each other as friends, and see each other on their start pages from then on.  The server keeps the friends
// lists (EA Messenger's, for claimed names only) and checks everything (web/server-rs/src/app/friends.rs); this is the
// page's side:
// - FriendPrompt, over the game: the game says when the player's car and another's touch nose to nose
//   (runtime/roam_game.c bump_*, NET_KIND_FRIEND), the page asks the server what that player is to it ("can"), and a
//   line in quick chat's look, just above the game's outrun line, offers it: "Add Bravo as a friend" (F, a click or a tap
//   on the line, or the page's menu: a controller's every button is the game's), "Bravo wants to be friends: accept", or says why not;
//   gone once the cars part.  A request from afar (by name, from a start page or another game's Friends) gets the same
//   line for a while once the player drives, and the newest one that waited does at the game's start: its answer goes
//   on /friends, as the start page's.  It also tells the server the room's name once the player has friends (their
//   Join), asks where the friends play every minute ("where"), says in the notices when one comes into free roam, and
//   fills the page menu's Friends, there in every game in free roam with others: the requests to answer, the
//   friends in free roam (Join a friend's room, Invite to the player's race, Invite to a crew), how to add one, a field
//   to ask by name.  A friend's invite to its crew (from its start page or its game's Friends) gets a card at the top
//   as a race invite's (the race invite's key, a click or a tap joins, as an invite link would: the crew, in the
//   friend's room), then waits in the page menu's Crews until the game ends.
// - CrewInvites: a friend in free roam's crew icon (the start page's Friends, the page menu's Friends): one crew, a
//   click invites to it; more, they open under the friend's line, a click on one invites to that.
// - FriendsPanel, on the start page (its Friends & crews tab): one line a friend, where it plays after its name and a
//   Join in free roam, the requests to answer, the ones sent; icons for what each line does, a friend's Message among
//   them (lime with a dot while messages wait to be read: messages.ts), and a friend's Discord icon, which copies its
//   Discord username (copy.ts: a toast at it says so); under them a field to ask a player by name, and the player's own Discord username ("Your
//   Discord": only its friends both ways see it, as Discord allows one: discordName).
// - FriendsApi: /friends, asked with the keyphrase's token (claim.ts NameClaims does the same for /names).
import type { GameSession } from './game';
import { touchFirst } from './device';
import { gameHas, KEY_FRIEND, KEY_LOBBY_INVITE } from './gamekeys';
import { raceKindText, type Hosted } from './lobby';
import { t, tn } from './i18n';
import { keyName, siteKey } from './sitekeys';
import { pageDialog } from './padnav';
import { onlineName } from './claim';
import { roomHash } from './players';
import { atName, friendRoom, friendSpot, TRACKS, trackRoom } from './tracks';
import { worldName, worldNamed } from './worlds';
import { iconButton, TRASH } from './settings-ui';
import { named } from './cards';
import { copyText, copyToast, READ_MS } from './copy';
import { mailOf, type MessageStore } from './messages';
import { CREW_MAX, type Crew } from './crews';
import { checkInvite, sameName as sameCrew, tidyInvite } from './invite';

/** two names of one player: the same Online form, in any case (the name over a car, a claimed name) */
function sameName(a: string, b: string): boolean {
  return (onlineName(a) ?? a).toLowerCase() === (onlineName(b) ?? b).toLowerCase();
}

/** a friend as the start page shows it: where it plays ('roam' free roam, 'online' the Online lobby, '' neither),
 *  its room's name in free roam ('' the room of everyone; none: its page did not say), its room's hash there (its
 *  world's, or a race track's spot's: tracks.ts friendSpot; none from a server of before), and its line for its
 *  friends */
export interface Friend { name: string; where: 'roam' | 'online' | ''; room?: string; at?: number; contact?: string }
/** the lists: the player's claimed name (null: none, so no friends), its friends, the requests that wait for its
 *  answer and its own that wait, the friends' messages that waited for it (gone from the server now); `done` what the
 *  last change did (friends.rs) */
export interface FriendLists {
  me: string | null; friends: Friend[]; asks: string[]; sent: string[]; mail: { from: string; body: string; t: number }[]; done?: string;
  /** the player's own Discord username (null: none) */
  contact?: string | null;
}

/** A Discord username as Discord allows one (2 to 32 of a-z, 0-9, _ and ., never two . together), from what the
 *  player typed: the spaces around it, a leading @ and capitals let go (as the server's buddy.rs discord_name).
 *  null: not one. */
export function discordName(typed: string): string | null {
  let s = typed.trim();
  if (s.startsWith('@')) s = s.slice(1);
  s = s.replace(/[A-Z]/g, c => c.toLowerCase());
  return /^[a-z0-9_.]{2,32}$/.test(s) && !s.includes('..') ? s : null;
}

/** the friends of an answer of the server's (GET /friends, "where"), read leniently: a world's name only when its room's
 *  hash leads to the friend (tracks.ts friendRoom) */
function friendsOf(v: unknown): Friend[] {
  return Array.isArray(v) ? v.filter((f: any) => typeof f?.name === 'string').map((f: any) => {
    const at: number | undefined = Number.isInteger(f.at) && f.at >= 0 && f.at <= 0xffffffff ? f.at : undefined;
    const room = friendRoom(typeof f.room === 'string' ? f.room : undefined, at);
    return {
      name: f.name, where: f.where === 'roam' || f.where === 'online' ? f.where : '',
      ...(room !== undefined ? { room } : {}), ...(at !== undefined ? { at } : {}),
      ...(typeof f.contact === 'string' ? { contact: f.contact } : {}),
    };
  }) : [];
}

/** /friends on this server: null when the page has no keyphrase or the server none (404) */
export class FriendsApi {
  constructor(private token: () => Promise<string | null>) {}

  private async ask(init?: RequestInit): Promise<FriendLists | null> {
    const tk = await this.token().catch(() => null);
    if (!tk) return null;
    const r = await fetch('/friends', { ...init, headers: { ...init?.headers, authorization: `Bearer ${tk}` }, cache: 'no-store' });
    if (!r.ok) return null;
    const j = await r.json();
    const names = (v: unknown) => Array.isArray(v) ? v.filter((x): x is string => typeof x === 'string') : [];
    return {
      me: typeof j.me === 'string' ? j.me : null, friends: friendsOf(j.friends), asks: names(j.asks), sent: names(j.sent), mail: mailOf(j.mail),
      done: typeof j.done === 'string' ? j.done : undefined, contact: typeof j.contact === 'string' ? j.contact : null,
    };
  }

  list(): Promise<FriendLists | null> { return this.ask(); }
  act(op: 'accept' | 'decline' | 'remove' | 'block' | 'ask', name: string): Promise<FriendLists | null> {
    return this.ask({ method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ op, name }) });
  }
  /** the player's Discord username from now on ('' none): the lists after it, `done` "ok", "name" (refused), "rate" */
  discord(name: string): Promise<FriendLists | null> {
    return this.ask({ method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ op: 'discord', name }) });
  }
  /** a message to friend `name` (messages.ts): the lists after it, `done` what became of it */
  send(name: string, body: string): Promise<FriendLists | null> {
    return this.ask({ method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ op: 'msg', name, body }) });
  }
  /** an invite to friend `name` (in free roam) to the player's crew `crew`, in room `room` ('' the room of everyone):
   *  the lists after it, `done` "sent", "away" (not in free roam now), "rate", ... (friends.rs crew_invite) */
  crew(name: string, crew: string, room: string): Promise<FriendLists | null> {
    return this.ask({ method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ op: 'crew', name, crew, room }) });
  }
}

/** the room f plays in is `mine` (the game's room: '' none), in free roam */
function inRoom(f: Friend, mine: string): boolean {
  return f.where === 'roam' && f.room !== undefined && roomHash(f.room) === roomHash(mine);
}

/** where a friend plays, after its name: free roam (and its world, but the main world; `mine`: the game's own room, in
 *  the game: your world; and the race track it is at, if one), the Online lobby, offline */
export function whereText(f: Friend, mine?: string): string {
  if (f.where === 'roam') {
    const w = mine !== undefined && inRoom(f, mine) ? t('free roam · your world')
      : f.room ? t('free roam · {world}', { world: worldName(f.room) }) : t('free roam');
    const spot = atName(friendSpot(f.room, f.at));
    return spot ? `${w} · ${spot}` : w;
  }
  return f.where === 'online' ? t('Online lobby') : t('offline');
}

/** whether friend f drives where the player does (`mine` its room, `spot` the race track it is at, 0 the city): the
 *  same room on the server's hub, so a race invite reaches it (without the friend's hash, a server of before: its
 *  world) */
function atSpot(f: Friend, mine: string, spot: number): boolean {
  if (!inRoom(f, mine)) return false;
  if (f.at === undefined) return true;
  const base = roomHash(mine);
  return f.at === (spot >= 1 && spot <= TRACKS.length ? trackRoom(base, TRACKS[spot - 1].key) : base);
}

/** where a friend in free roam plays, in the page menu's Friends (all of them in free roam there): your world (`mine`,
 *  the game's room), its world (worlds.ts), or free roam (its page did not say); and the race track it is at, if one */
function roomText(f: Friend, mine: string): string {
  const w = inRoom(f, mine) ? t('your world') : f.room !== undefined ? worldName(f.room) : t('free roam');
  const spot = atName(friendSpot(f.room, f.at));
  return spot ? `${w} · ${spot}` : w;
}

/** the signs of the friends' icons (24-unit boxes, as settings-ui.ts's) */
const CHECK = 'M5 12.5l4.5 4.5L19 7.5', CROSS = 'M6 6l12 12M18 6L6 18';
const JOIN = 'M10 17l5-5-5-5M15 12H3M14 4h6v16h-6', ENVELOPE = 'M3 6h18v12H3zM3 7l9 6 9-6', PLUS = 'M12 5v14M5 12h14';
/** a race's flag on its pole: Invite to the player's race lobby */
const FLAG = 'M5 21V4M5 4h13l-3 4.5 3 4.5H5';
/** a page back and on (the start page's Friends in pages), as the menu's ‹ */
const PREV = 'M15 6l-6 6 6 6', NEXT = 'M9 6l6 6-6 6';
/** a magnifier (Filter, while empty) */
const FIND = 'M10.5 17.5a7 7 0 1 0 0-14 7 7 0 0 0 0 14zM20 20l-4.5-4.5';
/** two people, one behind the other: Invite to a crew */
const GROUP = 'M9 11a3.5 3.5 0 1 0 0-7 3.5 3.5 0 0 0 0 7zM2 20c0-3.6 3.1-6 7-6s7 2.4 7 6M15.5 4.3a3.5 3.5 0 0 1 0 6.4M17.5 14.4c2.6.6 4.5 2.7 4.5 5.6';
/** Discord's mark (as the start page's Discord button has it): a filled shape, the icon's class says so (style.css) */
const DISCORD = 'M20.317 4.3698a19.7913 19.7913 0 00-4.8851-1.5152.0741.0741 0 00-.0785.0371c-.211.3753-.4447.8648-.6083 1.2495-1.8447-.2762-3.68-.2762-5.4868 0-.1636-.3933-.4058-.8742-.6177-1.2495a.077.077 0 00-.0785-.037 19.7363 19.7363 0 00-4.8852 1.515.0699.0699 0 00-.0321.0277C.5334 9.0458-.319 13.5799.0992 18.0578a.0824.0824 0 00.0312.0561c2.0528 1.5076 4.0413 2.4228 5.9929 3.0294a.0777.0777 0 00.0842-.0276c.4616-.6304.8731-1.2952 1.226-1.9942a.076.076 0 00-.0416-.1057c-.6528-.2476-1.2743-.5495-1.8722-.8923a.077.077 0 01-.0076-.1277c.1258-.0943.2517-.1923.3718-.2914a.0743.0743 0 01.0776-.0105c3.9278 1.7933 8.18 1.7933 12.0614 0a.0739.0739 0 01.0785.0095c.1202.099.246.1981.3728.2924a.077.077 0 01-.0066.1276 12.2986 12.2986 0 01-1.873.8914.0766.0766 0 00-.0407.1067c.3604.698.7719 1.3628 1.225 1.9932a.076.076 0 00.0842.0286c1.961-.6067 3.9495-1.5219 6.0023-3.0294a.077.077 0 00.0313-.0552c.5004-5.177-.8382-9.6739-3.5485-13.6604a.061.061 0 00-.0312-.0286zM8.02 15.3312c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9555-2.4189 2.157-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.9555 2.4189-2.1569 2.4189zm7.9748 0c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9554-2.4189 2.1569-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.946 2.4189-2.1568 2.4189z';

/** a line of a friends list: the name, where it plays (or the request's state) after it in small text, lime while it
 *  plays; dimmed while it does not; the icons at its end; under: a line under it (the player's crews to invite it to) */
interface Line { name: string; state: string; ok?: boolean; off?: boolean; acts: HTMLButtonElement[]; under?: HTMLElement | null }

function icon(cls: string, name: string, path: string, f: () => void): HTMLButtonElement {
  const b = iconButton(cls, name, path);
  b.onclick = () => f();
  return b;
}

function friendList(lines: Line[]): HTMLUListElement {
  const ul = document.createElement('ul');
  ul.className = 'fl';
  for (const l of lines) {
    const li = document.createElement('li');
    if (l.off) li.className = 'off';
    const name = document.createElement('span');
    name.className = 'fl-name';
    const who = document.createElement('span');
    who.className = 'fl-who';
    who.textContent = l.name;
    const st = document.createElement('span');
    st.className = l.ok ? 'fl-st ok' : 'fl-st';
    st.textContent = l.state;
    name.append(who, st);
    const acts = document.createElement('span');
    acts.className = 'fl-act';
    acts.append(...l.acts);
    li.append(name, acts);
    ul.append(li);
    if (l.under) ul.append(l.under);
  }
  return ul;
}

/** the player's crews and the room it plays in ('' the room of everyone), for a friend's crew invite: the running
 *  game's in its menu's Friends, settings.ini's on the start page */
export interface CrewsNow { crews: readonly Crew[]; room: string }

/** A friend's crew invite (its friends both ways in free roam only, as the server tells only those): an icon on the
 *  friend's line; with one crew a click invites to it, with more it opens the player's crews under the line (a chip
 *  each, as the Crews row's: its dot and name), a click on one invites to that.  What the server did shows in a toast
 *  at the icon (copy.ts: the copy buttons' look).  The crew's name goes to the server and that friend only, and in
 *  the log never. */
export class CrewInvites {
  /** the friend whose line has the crews open under it ('' none), and its icon as last drawn (the toast's place) */
  private open = '';
  private icons = new Map<string, HTMLButtonElement>();

  /** now: the crews and room (null: none known); redraw: the list drawn again (a line opened or closed) */
  constructor(private api: FriendsApi, private now: () => CrewsNow | null, private redraw: () => void, private log: (line: string) => void) {}

  /** the icon for friend `name`'s line (none while the player has no crew) */
  acts(name: string): HTMLButtonElement[] {
    const o = this.now(), key = name.toLowerCase();
    if (!o?.crews.length) { this.icons.delete(key); return []; }
    const b = icon('fl-crew', t('Invite {name} to a crew', { name }), GROUP, () => {
      const c = this.now();
      if (!c?.crews.length) return;
      if (c.crews.length === 1) { void this.send(name, c.crews[0].name, c.room); return; }
      this.open = sameName(this.open, name) ? '' : name;
      this.redraw();
      if (this.open) this.icons.get(key)?.closest('li')?.nextElementSibling?.querySelector<HTMLElement>('.crew-pick')?.focus();
    });
    if (o.crews.length > 1) b.setAttribute('aria-expanded', String(!!this.open && sameName(this.open, name)));
    this.icons.set(key, b);
    return [b];
  }

  /** the line under friend `name`'s while its crews are open: "Invite {name} to" and the crews (null: closed) */
  under(name: string): HTMLLIElement | null {
    const o = this.now();
    if (!this.open || !sameName(this.open, name) || !o || o.crews.length < 2) return null;
    const li = document.createElement('li');
    li.className = 'fl-pick';
    const lead = document.createElement('span'); lead.className = 'fl-st'; lead.textContent = t('Invite {name} to', { name });
    li.append(lead);
    for (const c of o.crews) {
      const b = document.createElement('button');
      b.type = 'button'; b.className = 'crew crew-pick';
      b.style.setProperty('--crew', `#${c.rgb}`);
      b.setAttribute('aria-label', t('Invite {name} to crew {crew}', { name, crew: c.name }));
      const dot = document.createElement('span'); dot.className = 'crew-dot';
      const text = document.createElement('span'); text.className = 'crew-text';
      const nm = document.createElement('span'); nm.className = 'crew-name'; nm.textContent = c.name;
      text.append(nm);
      b.append(dot, text);
      b.onclick = () => {
        this.open = '';
        this.redraw();
        this.icons.get(name.toLowerCase())?.focus();
        void this.send(name, c.name, o.room);
      };
      li.append(b);
    }
    return li;
  }

  /** the invite on /friends, and what became of it in a toast at the friend's icon */
  private async send(name: string, crew: string, room: string) {
    let r: FriendLists | null = null;
    try { r = await this.api.crew(name, crew, room); } catch (e) { this.log(`[friends] the crew invite failed: ${e}`); }
    const done = r?.done ?? '';
    this.log(`[friends] a crew invite to ${name}: ${done || 'no answer'}`);
    const text = done === 'sent' ? t('Invited {name} to {crew}', { name, crew })
      : done === 'away' ? t('{name} isn\'t in free roam now', { name })
      : done === 'rate' ? t('Wait a moment')
      : t('Not sent: try again in a moment');
    const at = this.icons.get(name.toLowerCase());
    if (at?.isConnected) copyToast(at, text, READ_MS);
  }
}

/** a friend's Discord icon: a click copies its username, and a toast at it says so with the name (for long enough to
 *  read it; the name alone where the browser would not copy, so it can still be typed) */
function discordIcon(name: string): HTMLButtonElement {
  const b = icon('fl-discord', t('Copy Discord name {name}', { name }), DISCORD, async () => {
    copyToast(b, await copyText(name, b) ? t('Copied {name}', { name }) : t('Discord: {name}', { name }), READ_MS);
  });
  return b;
}

/** what a request by name did (friends.rs "ask": `r` the lists after it, null no answer), for the line under its
 *  field; claim: what to say when the player has no claimed name (where it claims one differs) */
function askNote(r: FriendLists | null, name: string, claim: string): { text: string; warn: boolean } {
  const done = r?.done ?? '', shown = [...(r?.sent ?? []), ...(r?.friends.map(f => f.name) ?? [])].find(n => sameName(n, name)) ?? name;
  switch (done) {
    case 'asked': return { text: t('Friend request sent to {name}', { name: shown }), warn: false };
    case 'friends': return { text: t('You and {name} are friends', { name: shown }), warn: false };
    // (the server says "claim" for this player's own name too: its claim gone since the list was drawn)
    case 'claim': return { text: r?.me ? t('{name} has no claimed name, so can\'t add friends yet', { name }) : claim, warn: true };
    case 'self': return { text: t('That\'s your own name'), warn: true };
    case 'name': return { text: t('Not a player\'s name'), warn: true };
    case 'full': return { text: t('A friends list is full (250 at most)'), warn: true };
    case 'rate': return { text: t('Wait a moment'), warn: true };
    default: return { text: t('Not sent: try again in a moment'), warn: true };
  }
}

/** The field to ask a player by name (the start page's Friends, the page menu's Friends): the Crews row's box
 *  with a + in it, what the last request did under it.  One element over redraws, so a player typing in it keeps the
 *  keyboard and the caret (nodes() puts them back). */
class AskField {
  private readonly box = document.createElement('div');
  private readonly input = document.createElement('input');
  private readonly note = document.createElement('p');

  /** took: the lists after a request (null: no answer); claim: the note when the player has no claimed name */
  constructor(private api: FriendsApi, private claim: () => string, private took: (r: FriendLists | null) => void, private log: (line: string) => void) {
    const { box, input: inp } = this;
    box.className = 'crew-add fl-add';
    inp.type = 'text'; inp.autocomplete = 'off'; inp.spellcheck = false; inp.enterKeyHint = 'send'; inp.maxLength = 32;
    const go = icon('fl-ask', t('Send friend request'), PLUS, () => void this.ask());
    go.disabled = true;
    inp.oninput = () => { go.disabled = !inp.value.trim(); };
    inp.onkeydown = e => { if (e.key === 'Enter' && !e.isComposing) { e.preventDefault(); void this.ask(); } };
    box.append(inp, go);
    this.note.className = 'set-note fl-note'; this.note.setAttribute('aria-live', 'polite');
  }

  /** the field and its note, for a redraw (in the language now); keep(): after the redraw, the focus back in it */
  nodes(): [HTMLElement, HTMLElement] {
    this.input.placeholder = t('Add a player'); this.input.setAttribute('aria-label', t('Add a player'));
    this.box.querySelector('.fl-ask')?.setAttribute('aria-label', t('Send friend request'));
    return [this.box, this.note];
  }
  /** around a redraw that moves the field: its focus and caret kept */
  keep(redraw: () => void) {
    const inp = this.input, typing = document.activeElement === inp, at = [inp.selectionStart, inp.selectionEnd];
    redraw();
    if (typing) { inp.focus({ preventScroll: true }); inp.setSelectionRange(at[0], at[1]); }
  }

  /** a request to the player whose name is in the field (friends.rs "ask": any claimed name, wherever they are) */
  private async ask() {
    const inp = this.input, name = inp.value.trim();
    if (!name) return;
    let r: FriendLists | null = null;
    try { r = await this.api.act('ask', name); } catch (e) { this.log(`[friends] ask failed: ${e}`); }
    this.log(`[friends] ask by name: ${r?.done ?? 'no answer'}`);
    const n = askNote(r, name, this.claim());
    this.note.textContent = n.text;
    this.note.classList.toggle('warn', n.warn);
    if (r?.done === 'asked' || r?.done === 'friends') { inp.value = ''; inp.dispatchEvent(new Event('input')); }
    this.took(r);
  }
}

/** the start page's Filter: the Add field's box over a long list (FriendsPanel), a magnifier in it while empty
 *  (as the Add field's +, dimmed), then a ✕ that empties it; kept over renders as that one is (the player may be typing) */
class FindField {
  private readonly box = document.createElement('div');
  private readonly input = document.createElement('input');
  private readonly clear: HTMLButtonElement;

  /** changed: the text is another now */
  constructor(changed: () => void) {
    const { box, input: inp } = this;
    box.className = 'crew-add fl-find';
    inp.type = 'text'; inp.autocomplete = 'off'; inp.spellcheck = false; inp.enterKeyHint = 'search'; inp.maxLength = 32;
    this.clear = icon('fl-clear', t('Clear'), FIND, () => { inp.value = ''; inp.focus(); inp.dispatchEvent(new Event('input')); });
    this.clear.disabled = true;
    inp.oninput = () => { this.mark(); changed(); };
    inp.onkeydown = e => { if (e.key === 'Escape' && inp.value) { e.preventDefault(); this.clear.click(); } };
    box.append(inp, this.clear);
  }

  /** the box's icon: a magnifier while the field is empty, else its Clear */
  private mark() {
    const empty = !this.input.value;
    this.clear.disabled = empty;
    this.clear.querySelector('path')?.setAttribute('d', empty ? FIND : CROSS);
  }

  /** the text to find, in lower case ('' none) */
  text(): string { return this.input.value.trim().toLowerCase(); }
  /** the field, for a redraw (in the language now) */
  node(): HTMLElement {
    this.input.placeholder = t('Filter'); this.input.setAttribute('aria-label', t('Filter'));
    this.clear.setAttribute('aria-label', t('Clear')); this.clear.title = t('Clear');
    return this.box;
  }
  /** around a redraw that moves the field: its focus and caret kept */
  keep(redraw: () => void) {
    const inp = this.input, typing = document.activeElement === inp, at = [inp.selectionStart, inp.selectionEnd];
    redraw();
    if (typing) { inp.focus({ preventScroll: true }); inp.setSelectionRange(at[0], at[1]); }
  }
}

/** how often the list is tried until the game's link to the server is up (ms) */
const LIST_TRY_MS = 2000;
/** how often the game asks where its friends play, while it has some (ms) */
const WHERE_MS = 60_000;

/** what touching the other car may lead to: the server's answer to "can", or what the page did since */
type St = 'wait' | 'new' | 'asks' | 'asked' | 'friends' | 'claim' | 'theirs';

export interface FriendPromptDeps {
  /** a friend message to the server on the game's link (net/index.ts friend); false: not connected now */
  send(m: Record<string, unknown>): boolean;
  /** the game's room now ('' none) */
  room(): string;
  /** the race track the player's car is at now (tracks.ts travelState: 0 the city); null: a game without trips */
  spot(): number | null;
  /** a line over the game for a moment (shell.ts toast): name in it bold, hot with the border of something to answer */
  toast(text: string, o?: { name?: string; hot?: boolean }): void;
  /** the column at the top the lines go in (shell.ts placeNotices: its place, the HUD's size) */
  place(): void;
  /** a friend's line in the notices (shell.ts friendNotice): text names it; here: it is in the player's room */
  notice(name: string, text: string, here: boolean): void;
  /** "the menu (F9)", with what opens it here (shell.ts menuName) */
  menu(): string;
  /** where the friends play changed (the page menu's Friends is drawn again) */
  placed(): void;
  /** the race lobby the player hosts now, with room in it (lobby.ts: null none): its friends in free roam in its room
   *  get an Invite in the page menu's Friends */
  hosted(): Hosted | null;
  /** a friend's invite taken: its lobby joined from wherever the player is (the game: input LB_JOIN) */
  joinLobby(host: number, lobby: number, race: number): boolean;
  /** /friends: answers to requests and requests by name, which reach a player from afar */
  api: FriendsApi;
  /** driving in free roam now (quickchat.ts driving): a request's line waits for it */
  driving(): boolean;
  /** the running game's crews (shell.ts roamCrews) */
  crews(): readonly Crew[];
  /** a friend's crew invite taken: the crew and room joined, as an invite link's Join (shell.ts) */
  joinCrew(crew: string, room: string): void;
  /** the crew invites that wait changed (the page menu's Crews is drawn again) */
  crewsChanged(): void;
  log(line: string): void;
}

/** a friend's invite to its crew, in its room ('' the room of everyone) */
export interface CrewAsk { name: string; crew: string; room: string }
/** the most crew invites that wait in the page menu's Crews (the newest kept) */
const CREW_ASKS_MAX = 4;

/** how long a friend shows "invited" in the menu's Friends, and an invite's line shows, after it was sent (ms) */
const INVITED_MS = 60_000, INVITE_LINE_MS = 20_000;
/** the page menu's Friends: the names it lists in the Online lobby and offline, at most (then "and 212 more") */
const NAMES_MAX = 10;
/** how long a request from afar shows on the line, once the player drives (ms): then it waits in the menu's Friends */
const ASK_LINE_MS = 15_000;
/** an invite from a friend that waits for the player's answer: who, its lobby (the host's id, the lobby's, its race's
 *  hash), what race, until when its line shows */
interface Invite { name: string; host: number; lobby: number; race: number; kind: string; n: number; max: number; until: number }

export class FriendPrompt {
  private root: HTMLButtonElement;
  private session: GameSession | null = null;
  private on = false;
  private raf = 0;
  /** the player the cars touch (the name over its car, then its claimed name), and what that may lead to */
  private who: { name: string; st: St } | null = null;
  /** the friends the server last listed, and whether the room's name went to it since the last change of room */
  private friends: string[] = [];
  private roomSent: string | null = null;
  /** the list asked of the server on this link (once it is up: tried every LIST_TRY_MS until then) */
  private listed = false;
  private triedAt = 0;
  /** where the friends play, as the server last said ("where", asked every WHERE_MS; null: not yet), and what each
   *  was then (by its name in lower case: a notice when one comes into free roam) */
  private where: Friend[] | null = null;
  private was = new Map<string, Friend['where']>();
  private whereAt = 0;
  /** the friends invited lately (by name in lower case: when), and a friend's invite that waits for an answer, its line */
  private invited = new Map<string, number>();
  private invite: Invite | null = null;
  private inviteEl: HTMLButtonElement;
  private inviteTimer = 0;
  /** the player's claimed name as the server last listed it (null: none, so no friends; undefined: not listed yet),
   *  the requests that wait for its answer, and whether this game's first list came (its newest request: the line) */
  private me: string | null | undefined = undefined;
  private asks: string[] = [];
  private firstList = false;
  /** a request to answer on the line while no cars touch: whose, and until when it shows (0: once the player drives) */
  private asking: { name: string; until: number } | null = null;
  /** the page menu's Friends's field to ask a player by name */
  private askField: AskField;
  /** the page menu's Friends's crew icons */
  private crewInvites: CrewInvites;
  /** friends' crew invites that wait for the player's answer (the page menu's Crews), the one whose card shows at the
   *  top, and the card */
  private crewAsks: CrewAsk[] = [];
  private crewCard: CrewAsk | null = null;
  private crewTimer = 0;
  private crewEl: HTMLButtonElement;

  /** stage: the game's box; the line goes over the picture while the cars touch (place) */
  constructor(stage: HTMLElement, private deps: FriendPromptDeps) {
    this.askField = new AskField(deps.api, () => t('Claim your name on the start page to add friends'), r => this.tookLists(r), deps.log);
    const b = this.root = document.createElement('button');
    b.type = 'button'; b.className = 'friend-prompt'; b.tabIndex = -1; b.hidden = true; b.dataset.nfsu2Ui = '';
    b.onpointerdown = e => e.preventDefault();     // the canvas keeps the focus (and with it the keyboard)
    b.onclick = () => this.act();
    // in the column of the notes over the game, at its top (style.css .toasts: an invite first, then this line)
    const col = stage.querySelector<HTMLElement>('.toasts') ?? stage;
    col.prepend(b);
    // a friend's invite to its crew: a line of the same look as a race invite's, under it
    const ce = this.crewEl = document.createElement('button');
    ce.type = 'button'; ce.className = 'friend-prompt offer invite crew-invite'; ce.tabIndex = -1; ce.hidden = true; ce.dataset.nfsu2Ui = '';
    ce.onpointerdown = e => e.preventDefault();
    ce.onclick = () => { if (this.crewCard) this.answerCrew(this.crewCard, true); };
    col.prepend(ce);
    // a friend's invite to its race: a line of the same look, above the friend line (a click or a tap joins)
    const iv = this.inviteEl = document.createElement('button');
    iv.type = 'button'; iv.className = 'friend-prompt offer invite'; iv.tabIndex = -1; iv.hidden = true; iv.dataset.nfsu2Ui = '';
    iv.onpointerdown = e => e.preventDefault();
    iv.onclick = () => this.takeInvite();
    col.prepend(iv);
    this.crewInvites = new CrewInvites(deps.api, () => this.on ? { crews: deps.crews(), room: deps.room() } : null, () => deps.placed(), deps.log);
    // caught before the game's input: F is the game's for nothing (its key maps: dinput_kbd.c), and only while the
    // line offers something
    window.addEventListener('keydown', e => this.key(e), { capture: true });
  }

  /** a game started: friends on (free roam with others) or off */
  start(s: GameSession, on: boolean) {
    this.session = s; this.on = on; this.who = null; this.friends = []; this.roomSent = null; this.listed = false; this.triedAt = -Infinity;
    this.where = null; this.was.clear(); this.whereAt = -Infinity;
    this.me = undefined; this.asks = []; this.firstList = false; this.asking = null;
    this.crewAsks = []; this.dropCrewCard();
    const tick = () => {
      const now = performance.now();
      if (!this.listed && now - this.triedAt > LIST_TRY_MS) { this.triedAt = now; this.listed = this.deps.send({ op: 'list' }); }
      if (this.listed && this.friends.length && now - this.whereAt > WHERE_MS) this.askWhere();
      this.askingTick(now);
      this.raf = requestAnimationFrame(tick);
    };
    if (on) this.raf = requestAnimationFrame(tick);
  }
  stop() {
    cancelAnimationFrame(this.raf);
    this.session = null; this.on = false; this.who = null; this.asking = null;
    this.crewAsks = []; this.dropCrewCard();
    this.dropInvite();
    this.draw();
  }

  /** a request's line: up once the player drives (for ASK_LINE_MS), gone when it stops (the request waits in the
   *  menu's Friends) */
  private askingTick(now: number) {
    const a = this.asking;
    if (!a || this.who) return;
    const driving = this.deps.driving();
    if (!a.until && driving) a.until = now + ASK_LINE_MS;
    else if (a.until && (now > a.until || !driving)) this.asking = null;
    else return;
    this.draw();
  }

  /** the link to the server is up again (a restart): the friends asked again, the room's name told again */
  online() {
    this.listed = false; this.roomSent = null; this.triedAt = -Infinity;
  }

  /** the room changed while the game runs (an invite link): its name to the server again, when there are friends */
  roomChanged() {
    this.roomSent = null;
    this.sendRoom();
  }

  private sendRoom() {
    const room = this.deps.room();
    if (!this.on || !this.friends.length || this.roomSent === room) return;
    if (this.deps.send({ t: 'room', name: room })) this.roomSent = room;
  }

  /** where the friends play, asked now (and again in WHERE_MS): the page menu's Friends opened */
  askWhere() {
    if (!this.on || !this.friends.length) return;
    this.whereAt = performance.now();
    this.deps.send({ op: 'where' });
  }

  /** free roam with friends */
  hasFriends(): boolean {
    return this.on && this.friends.length > 0;
  }
  /** the page menu has its Friends button: free roam with others, friends or not yet (which says how to add one) */
  available(): boolean {
    return this.on;
  }
  /** the requests that wait for the player's answer (the page menu's Friends shows a dot) */
  waiting(): number {
    return this.on ? this.asks.length : 0;
  }

  /** the game: the cars touch nose to nose for 3 s (on 1), or parted (NET_KIND_FRIEND: {"id","name","on"}) */
  bump(json: string) {
    let m: any;
    try { m = JSON.parse(json); } catch { return; }
    if (!this.on || typeof m?.name !== 'string') return;
    if (m.on === 1) {
      this.who = { name: m.name, st: 'wait' };
      this.deps.log(`[friends] front bumpers touching ${m.name}'s: asking the server`);
      if (!this.deps.send({ op: 'can', name: m.name })) this.who = null;
    } else if (this.who && sameName(this.who.name, m.name)) {
      this.who = null;
    }
    this.draw();
  }

  /** a friend message from the server (friends.rs) */
  server(m: any) {
    if (!this.on || typeof m?.op !== 'string') return;
    const name = typeof m.name === 'string' ? m.name : '';
    const mine = !!this.who && !!name && sameName(this.who.name, name);
    switch (m.op) {
      case 'list': {
        const names = (v: unknown): string[] => Array.isArray(v) ? v.filter((f: unknown): f is string => typeof f === 'string') : [];
        this.friends = names(m.friends);
        this.me = typeof m.me === 'string' ? m.me : null;
        this.asks = names(m.asks);
        // the game's first list: the newest request that waited, on the line once the player drives
        if (!this.firstList) {
          this.firstList = true;
          const last = this.asks.at(-1);
          if (last && !this.who) this.asking = { name: last, until: 0 };
        } else if (this.asking && !this.asks.some(n => sameName(n, this.asking!.name))) this.asking = null;
        this.sendRoom();
        this.whereAt = -Infinity;                    // (where they are, with the next frame)
        this.deps.placed();
        break;
      }
      case 'where':
        this.placedNow(friendsOf(m.friends));
        return;
      case 'can':
        if (!mine || !this.who) return;
        if (m.st === 'far') { this.who = null; break; }
        if (['new', 'asks', 'asked', 'friends', 'claim', 'theirs'].includes(m.st)) this.who = { name, st: m.st };
        break;
      case 'add':
        // (the line says it while the cars touch; the note says it once they parted)
        if (m.done === 'friends') { this.was.set(name.toLowerCase(), 'roam'); this.answered(name); }
        if (m.done === 'friends' && !mine) this.deps.toast(t('You and {name} are friends now', { name }), { name });
        else if (m.done === 'full') this.deps.toast(t('A friends list is full (250 at most)'));
        if (mine && this.who && ['asked', 'friends', 'claim', 'theirs'].includes(m.done)) this.who = { name, st: m.done };
        else if (mine && m.done === 'full') this.who = null;
        break;
      case 'asks':
        // the other player pressed first, or asked from afar: answer here while the cars touch, else on the line for a
        // while once the player drives, and in the menu's Friends
        if (!name) return;
        if (!this.asks.some(n => sameName(n, name))) this.asks.push(name);
        if (mine && this.who) this.who = { name, st: 'asks' };
        else this.asking = { name, until: 0 };
        this.deps.placed();
        break;
      case 'friends':
        // (just met here: no notice that it is in free roam)
        this.was.set(name.toLowerCase(), 'roam');
        this.answered(name);
        if (mine && this.who) this.who = { name, st: 'friends' };
        else this.deps.toast(t('You and {name} are friends now', { name }), { name });
        break;
      case 'invite':
        if (typeof m.done === 'string') return this.inviteAnswer(name, m.done);
        this.invitedBy(m);
        return;
      case 'crew':
        this.crewInvitedBy(m);
        return;
      default:
        return;
    }
    this.draw();
  }

  /** an invite of ours to friend `name`: the server's answer (friends.rs "invite") */
  private inviteAnswer(name: string, done: string) {
    this.deps.log(`[friends] the invite to ${name}: ${done}`);
    switch (done) {
      case 'sent': this.invited.set(name.toLowerCase(), performance.now()); this.deps.toast(t('Invite sent to {name}', { name }), { name }); break;
      case 'away': this.deps.toast(t('{name} isn\'t in free roam in your world now', { name }), { name }); break;
      case 'nolobby': this.deps.toast(t('Open a race lobby first, then invite')); break;
      case 'full': this.deps.toast(t('Your race lobby is full')); break;
      case 'claim': this.deps.toast(t('Claim your name on the start page to invite friends')); break;
      case 'rate': this.deps.toast(t('Too many invites at once: try again in a moment')); break;
      default: this.deps.toast(t('{name} can\'t be invited now', { name }), { name });
    }
    this.deps.placed();
  }

  /** a friend's invite to its race (friends.rs: it hosts the lobby, we are both in free roam in one room): its line for
   *  INVITE_LINE_MS, the player's key, a click or a tap joins from wherever the player is */
  private invitedBy(m: any) {
    const num = (v: unknown) => typeof v === 'number' && Number.isFinite(v) ? v >>> 0 : 0;
    const name = typeof m.name === 'string' ? m.name : '', host = num(m.host), lobby = num(m.lobby), race = num(m.race);
    if (!name || !host || !lobby || !race) return;
    const kind = typeof m.kind === 'string' ? m.kind : 'race', n = num(m.n), max = num(m.max);
    this.deps.log(`[friends] ${name} invites us to a ${kind} (${n}/${max}), lobby ${lobby.toString(16)}`);
    this.invite = { name, host, lobby, race, kind, n, max, until: performance.now() + INVITE_LINE_MS };
    clearTimeout(this.inviteTimer);
    this.inviteTimer = window.setTimeout(() => this.dropInvite(), INVITE_LINE_MS);
    this.draw();
  }
  private dropInvite() {
    clearTimeout(this.inviteTimer);
    this.invite = null;
    this.inviteEl.hidden = true;
  }
  /** A friend's invite to its crew (friends.rs crew_invite: we are in free roam): its card for INVITE_LINE_MS (the
   *  race invite's key, a click or a tap joins), then it waits in the page menu's Crews until the game ends; the
   *  newest from a friend to a crew the player is not in, checked as an invite link's names are (a page of the
   *  friend's could send anything) */
  private crewInvitedBy(m: any) {
    const name = typeof m.name === 'string' ? m.name : '', inv = tidyInvite({ crew: typeof m.crew === 'string' ? m.crew : '', room: typeof m.room === 'string' ? m.room : '' });
    if (!this.on || !name || !inv.crew || !checkInvite(inv).ok) return;
    if (this.deps.crews().some(c => sameCrew(c.name, inv.crew!))) { this.deps.log(`[friends] ${name} invites us to a crew we are in`); return; }
    this.deps.log(`[friends] ${name} invites us to a crew`);
    const ask: CrewAsk = { name, crew: inv.crew, room: inv.room ?? '' };
    this.crewAsks = [...this.crewAsks.filter(a => !sameCrew(a.crew, ask.crew)), ask].slice(-CREW_ASKS_MAX);
    this.crewCard = ask;
    clearTimeout(this.crewTimer);
    this.crewTimer = window.setTimeout(() => { this.dropCrewCard(); }, INVITE_LINE_MS);
    this.draw();
    this.deps.crewsChanged();
  }
  private dropCrewCard() {
    clearTimeout(this.crewTimer);
    this.crewCard = null;
    this.crewEl.hidden = true;
  }
  /** the crew invites that wait, but for crews the player joined since */
  crewAsksNow(): readonly CrewAsk[] {
    const mine = this.deps.crews();
    return this.crewAsks.filter(a => !mine.some(c => sameCrew(c.name, a.crew)));
  }
  /** a crew invite answered (its card, its key, or the page menu's Crews): yes joins its crew in its room; with 4
   *  crews already, a note says so and it waits (the page menu's Crews has the Leave beside it) */
  answerCrew(a: CrewAsk, yes: boolean) {
    if (this.crewCard === a) this.dropCrewCard();
    if (yes && this.deps.crews().length >= CREW_MAX) {
      this.deps.toast(t('Up to 4 crews: leave one to join another.'));
      this.deps.crewsChanged();
      return;
    }
    this.crewAsks = this.crewAsks.filter(x => x !== a);
    this.deps.log(`[friends] ${yes ? 'joining' : 'declining'} ${a.name}'s crew invite`);
    if (yes) this.deps.joinCrew(a.crew, a.room);
    this.deps.crewsChanged();
  }
  /** The page menu's Crews's crew invites, above its crews: a line each, the friend and the crew, Join and
   *  Decline (null: none waits) */
  drawCrewAsks(): HTMLUListElement | null {
    const asks = this.crewAsksNow();
    if (!asks.length) return null;
    return friendList(asks.map(a => ({ name: a.name, state: t('invites you to crew "{crew}"', { crew: a.crew }), ok: true, acts: [
      icon('fl-yes', t('Join'), CHECK, () => this.answerCrew(a, true)),
      icon('fl-no', t('Decline'), CROSS, () => this.answerCrew(a, false)),
    ] })));
  }

  /** the invite taken: its lobby joined (the game leaves one it is in for it) */
  private takeInvite() {
    const iv = this.invite;
    if (!iv) return;
    this.deps.log(`[friends] taking ${iv.name}'s invite`);
    if (!this.deps.joinLobby(iv.host, iv.lobby, iv.race)) this.deps.toast(t('{name} can\'t be invited now', { name: iv.name }), { name: iv.name });
    this.dropInvite();
  }

  /** `name`'s request is answered (friends now, or declined): off the list, off the line */
  private answered(name: string) {
    this.asks = this.asks.filter(n => !sameName(n, name));
    if (this.asking && sameName(this.asking.name, name)) this.asking = null;
  }

  /** the lists after a change on /friends (an answer, a request by name): the requests that wait now; the friends
   *  come on the link (the server sends this page its list again on a friendship: friends.rs told) */
  private tookLists(r: FriendLists | null) {
    if (r?.me !== undefined) this.me = r.me;
    if (r) this.asks = r.asks;
    if (this.asking && !this.asks.some(n => sameName(n, this.asking!.name))) this.asking = null;
    this.draw();
    this.deps.placed();
  }

  /** an answer to `name`'s request, from the line or the menu's Friends: on /friends, as the start page's, since the
   *  request may be from afar (the game's "add" takes only a player its car touched) */
  private async answer(name: string, yes: boolean) {
    this.deps.log(`[friends] ${yes ? 'accepting' : 'declining'} ${name}'s request`);
    this.answered(name);
    this.draw();
    this.deps.placed();
    let r: FriendLists | null = null;
    try { r = await this.deps.api.act(yes ? 'accept' : 'decline', name); } catch (e) { this.deps.log(`[friends] the answer failed: ${e}`); }
    const done = r?.done ?? '';
    this.deps.log(`[friends] ${yes ? 'accept' : 'decline'} ${name}: ${done || 'no answer'}`);
    if (done === 'friends') this.deps.toast(t('You and {name} are friends now', { name }), { name });
    else if (done === 'full') this.deps.toast(t('A friends list is full (250 at most)'));
    else if (!r) {
      this.deps.toast(t('Not sent: try again in a moment'));
      if (!this.asks.some(n => sameName(n, name))) this.asks.push(name);   // (still waits: in the menu again)
    }
    this.tookLists(r);
  }

  /** invite friend `name` to the player's race lobby (the page menu's Friends) */
  inviteFriend(name: string) {
    const h = this.deps.hosted();
    if (!h) { this.deps.toast(t('Open a race lobby first, then invite')); return; }
    this.deps.log(`[friends] inviting ${name} to our ${h.kind}`);
    if (!this.deps.send({ op: 'invite', name, kind: h.kind, stage: h.stage })) this.deps.toast(t('{name} can\'t be invited now', { name }), { name });
  }

  /** the server's "where": a notice for each friend that came into free roam since the last one (or is there at the
   *  first), the menu's Friends drawn again */
  private placedNow(list: Friend[]) {
    const room = this.deps.room(), was = this.was;
    this.was = new Map(list.map(f => [f.name.toLowerCase(), f.where]));
    this.where = list;
    for (const f of list) {
      if (f.where !== 'roam' || was.get(f.name.toLowerCase()) === 'roam') continue;
      const name = f.name, menu = this.deps.menu(), here = f.room === undefined || inRoom(f, room);
      // (at a race track: the menu's Join goes there, in a game with trips)
      const track = this.deps.spot() !== null ? atName(friendSpot(f.room, f.at)) : '', world = worldNamed(f.room ?? '');
      this.deps.notice(name, track
        ? here ? t('Your friend {name} is at {track}. Join in {menu}', { name, track, menu })
          : t('Your friend {name} is at {track} in {world}. Join in {menu}', { name, track, world, menu })
        : here ? t('Your friend {name} is in free roam', { name })
          : t('Your friend {name} is in free roam in {world}: Join in {menu}', { name, world, menu }), here);
    }
    this.deps.placed();
  }

  /** The page menu's Friends, in box: the requests to answer (Accept, Decline), the friends in free roam, a Join
   *  for each one (join: the game goes to it, its world, its race track and its car; `goes` false, a game without
   *  that: for each one in another room only, its world), a Discord icon for one that gave its Discord username; the
   *  others in a line; with no friends yet, how to add one; then the field to ask a player by name.  Without a claimed
   *  name, only how to get one.  Returns whether a friend has a Join (the menu's line on Join shows then). */
  drawPlaced(box: HTMLElement, join: (f: Friend) => void, goes = false): boolean {
    const p = (text: string) => Object.assign(document.createElement('p'), { className: 'hint', textContent: text });
    if (this.me === null) { box.replaceChildren(p(t('Claim your name on the start page to add friends'))); return false; }
    if (this.me === undefined) { box.replaceChildren(p(t('Checking.'))); return false; }
    const lines: Line[] = this.asks.map(name => ({ name, state: t('wants to be friends'), ok: true, acts: [
      icon('fl-yes', t('Accept'), CHECK, () => void this.answer(name, true)),
      icon('fl-no', t('Decline'), CROSS, () => void this.answer(name, false)),
    ] }));
    const after: Node[] = [];
    let joins = false;
    const all = this.where, room = this.deps.room();
    if (!this.friends.length) after.push(p(t('Touch front bumpers in free roam for 3 s to add a player.')));
    else if (!all) after.push(p(t('Checking.')));
    else {
      const hosted = this.deps.hosted(), now = performance.now(), spot = this.deps.spot() ?? 0;
      for (const f of all.filter(f => f.where === 'roam')) {
        const at = this.invited.get(f.name.toLowerCase()), sent = at !== undefined && now - at < INVITED_MS;
        const there = goes ? f.room !== undefined || f.at !== undefined : f.room !== undefined && !inRoom(f, room);
        joins ||= there;
        lines.push({
          name: f.name, state: sent ? `${roomText(f, room)} · ${t('invited')}` : roomText(f, room), ok: true,
          acts: [
            ...(f.contact ? [discordIcon(f.contact)] : []),
            ...(there ? [icon('fl-join', t('Join'), JOIN, () => join(f))] : []),
            // (a friend where we drive, while we host a lobby with room in it: the invite joins it from where it is)
            ...(hosted && atSpot(f, room, spot) && !sent ? [icon('fl-invite', t('Invite {name} to your race', { name: f.name }), FLAG, () => this.inviteFriend(f.name))] : []),
            ...this.crewInvites.acts(f.name),
          ],
          under: this.crewInvites.under(f.name),
        });
      }
      if (!all.some(f => f.where === 'roam')) after.push(p(t('None of your friends is in free roam now.')));
      // (the first NAMES_MAX of each, then how many more: a list of 250 would fill the menu)
      const names = (w: Friend['where']) => {
        const l = all.filter(f => f.where === w).map(f => f.name), more = l.length - NAMES_MAX;
        return more > 0 ? tn(more, '{names} and {n} more', '{names} and {n} more', { names: l.slice(0, NAMES_MAX).join(', ') }) : l.join(', ');
      };
      const rest = [names('online') && t('In the Online lobby: {names}.', { names: names('online') }), names('') && t('Offline: {names}.', { names: names('') })];
      if (rest.some(Boolean)) after.push(p(rest.filter(Boolean).join(' ')));
    }
    this.askField.keep(() => box.replaceChildren(...(lines.length ? [friendList(lines)] : []), ...after, ...this.askField.nodes()));
    return joins;
  }

  /** the request from afar the line shows now (no cars touch), or null */
  private shownAsk(): string | null {
    return !this.who && this.asking?.until ? this.asking.name : null;
  }

  /** what the line offers: a request, or the answer to theirs (the player the car touches, or one from afar) */
  private actionable(): boolean {
    return !!this.who ? this.who.st === 'new' || this.who.st === 'asks' : this.shownAsk() !== null;
  }

  private act() {
    const far = this.shownAsk();
    if (far) { void this.answer(far, true); return; }
    if (!this.actionable() || !this.who) return;
    this.deps.log(`[friends] ${this.who.st === 'asks' ? 'accepting' : 'asking'} ${this.who.name}`);
    if (this.deps.send({ op: 'add', name: this.who.name })) { this.who = { ...this.who, st: 'wait' }; this.draw(); }
  }

  /** the page's menu (shell.ts): its button for the line while the line offers something, else null */
  menuItem(): { label: string; act: () => void } | null {
    if (!this.actionable()) return null;
    const far = this.shownAsk(), name = far ?? this.who!.name;
    return { label: far || this.who?.st === 'asks' ? t('Accept {name} as a friend', { name }) : t('Add {name} as a friend', { name }), act: () => this.act() };
  }

  private key(e: KeyboardEvent) {
    if ((this.invite || this.crewCard) && e.code === siteKey('lobbyInvite') && !e.metaKey && !e.ctrlKey && !e.altKey && !gameHas(this.session, KEY_LOBBY_INVITE)) {
      const el = e.target as HTMLElement | null;
      if (!el?.closest?.('input, textarea, select') && !pageDialog()) {
        e.preventDefault(); e.stopImmediatePropagation();
        // (a race's first: it lasts only while its lobby is open; the crew's waits in the page menu's Crews)
        if (!e.repeat) { if (this.invite) this.takeInvite(); else if (this.crewCard) this.answerCrew(this.crewCard, true); }
        return;
      }
    }
    if (e.code !== siteKey('friend') || e.metaKey || e.ctrlKey || e.altKey || !this.actionable()) return;
    const el = e.target as HTMLElement | null;
    // (a control of the game's the player put on F: the game's alone, gamekeys.ts; a click on the line still adds)
    if (el?.closest?.('input, textarea, select') || pageDialog() || gameHas(this.session, KEY_FRIEND)) return;
    e.preventDefault(); e.stopImmediatePropagation();
    if (!e.repeat) this.act();
  }

  /** the column's place and the HUD's size, as the line shows (shell.ts placeNotices) */
  private place() { this.deps.place(); }


  private drawInvite() {
    const iv = this.invite, el = this.inviteEl;
    if (!iv || !this.on) { el.hidden = true; return; }
    const kbd = (k: string) => { const e = document.createElement('kbd'); e.textContent = k; return e; };
    const ik = siteKey('lobbyInvite'), key = ik && !touchFirst() && !gameHas(this.session, KEY_LOBBY_INVITE) ? [kbd(keyName(ik))] : [];
    const text = (s: string) => Object.assign(document.createElement('span'), { textContent: s });
    const kind = raceKindText(iv.kind), count = iv.max ? ` (${iv.n}/${iv.max})` : '';
    el.replaceChildren(named(t('{name} invites you to a {kind}', { name: iv.name, kind }) + count, iv.name), ...key, text(t('Join')));
    this.place();
    el.hidden = false;
  }

  /** a friend's crew invite's card: "Ann invites you to crew "Touge"", the race invite's key, Join */
  private drawCrew() {
    const c = this.crewCard, el = this.crewEl;
    if (!c || !this.on) { el.hidden = true; return; }
    const kbd = (k: string) => { const e = document.createElement('kbd'); e.textContent = k; return e; };
    const ik = siteKey('lobbyInvite'), key = ik && !touchFirst() && !gameHas(this.session, KEY_LOBBY_INVITE) ? [kbd(keyName(ik))] : [];
    const text = (s: string) => Object.assign(document.createElement('span'), { textContent: s });
    el.replaceChildren(named(t('{name} invites you to crew "{crew}"', { name: c.name, crew: c.crew }), c.name), ...key, text(t('Join')));
    this.place();
    el.hidden = false;
  }

  private draw() {
    this.drawInvite();
    this.drawCrew();
    const far = this.shownAsk(), w = this.who ?? (far ? { name: far, st: 'asks' as St } : null), b = this.root;
    if (!w || w.st === 'wait') { b.hidden = true; return; }
    const kbd = (k: string) => { const e = document.createElement('kbd'); e.textContent = k; return e; };
    const fk = siteKey('friend');                   // (the player's key for it: sitekeys.ts, F by default)
    const offer = this.actionable(), key = offer && fk && !touchFirst() && !gameHas(this.session, KEY_FRIEND) ? [kbd(keyName(fk))] : [];
    const line = (s: string) => b.replaceChildren(...key, named(s, w.name));
    switch (w.st) {
      case 'new': line(t('Add {name} as a friend', { name: w.name })); break;
      case 'asks': line(t('{name} wants to be friends: accept', { name: w.name })); break;
      case 'asked': line(t('Friend request sent to {name}', { name: w.name })); break;
      case 'friends': line(t('You and {name} are friends', { name: w.name })); break;
      case 'claim': line(t('Claim your name on the start page to add friends')); break;
      case 'theirs': line(t('{name} has no claimed name, so can\'t add friends yet', { name: w.name })); break;
    }
    b.classList.toggle('offer', offer);
    this.place();
    b.hidden = false;
  }

}

export interface FriendsPanelDeps {
  api: FriendsApi;
  /** the conversations (messages.ts), and one opened (ChatDialog) */
  store: MessageStore;
  chat(name: string): void;
  /** join free roam's room of this name ('' the room of everyone; shell.ts offerInvite) */
  join(room: string): void;
  /** a question with Yes/No (shell.ts confirmBox) */
  confirm(title: string, body: string, ok: string): Promise<boolean>;
  /** whether the start page is in front and no game runs (the list is asked for then, every POLL_MS) */
  idle(): boolean;
  /** what waits for the player: the requests to answer and the friends' messages not read yet (the tab's count) */
  waiting?(n: number): void;
  /** the player's crews and room as settings.ini has them, for a friend's crew invite (none: no crew icons) */
  crews?(): CrewsNow | null;
  log(line: string): void;
}

/** how often the start page asks for the list while it is in front */
const POLL_MS = 60_000;

/** the friends in the order the list shows them: in free roam, in the Online lobby, offline */
const ORDER: Record<Friend['where'], number> = { roam: 0, online: 1, '': 2 };
/** the start page's list in pages of this many lines (requests, friends, requests sent), Filter on the row past one page */
const PAGE_LINES = 20;

export class FriendsPanel {
  private lists: FriendLists | null = null;
  private timer = 0;
  private busy = false;
  private again = false;

  constructor(private root: HTMLElement, private deps: FriendsPanelDeps) {
    this.crewInvites = new CrewInvites(deps.api, () => deps.crews?.() ?? null, () => this.render(), deps.log);
    this.askField = new AskField(deps.api, () => t('Claim your name (Online, Your name) to add friends.'), r => { this.took(r); this.render(); }, deps.log);
    this.findField = new FindField(() => { this.page = 0; this.render(); });
    document.addEventListener('visibilitychange', () => { if (!document.hidden) void this.refresh(); });
    this.timer = window.setInterval(() => void this.refresh(), POLL_MS);
    deps.store.onChange(() => this.render());
  }

  /** new lists from the server: the player's conversations, and the messages that came with them */
  private took(l: FriendLists | null) {
    if (!l) return;
    this.lists = l;
    this.deps.store.setMe(l.me);
    for (const m of l.mail ?? []) {
      this.deps.log(`[msg] from ${m.from}, on the start page`);
      this.deps.store.add(m.from, { dir: 'in', body: m.body, t: m.t }, true);
    }
  }

  /** a message from the start page (ChatDialog): the server's "done" (null: no answer) */
  async send(name: string, body: string): Promise<string | null> {
    const r = await this.deps.api.send(name, body);
    if (r) this.took(r);
    this.render();
    return r?.done ?? null;
  }

  /** the list now (the keyphrase or its name changed, a game ended) */
  async refresh(force = false) {
    // (one asked for while a list is on its way runs after it: a name claimed or a passphrase entered meanwhile)
    if (this.busy) { if (force) this.again = true; return; }
    if (!force && (!this.deps.idle() || document.hidden)) return;
    this.busy = true;
    try {
      const l = await this.deps.api.list();
      if (l) this.took(l); else this.lists = null;
    } catch (e) {
      this.deps.log(`[friends] the list could not be read: ${e}`);
    } finally {
      this.busy = false;
    }
    this.render();
    if (this.again) { this.again = false; void this.refresh(true); }
  }

  /** the field to ask a player by name (kept over renders: the player may be typing), with what the last ask did */
  private askField: AskField;
  /** Filter over a list of more than a page, and the page shown (from 0) */
  private findField: FindField;
  private page = 0;
  /** the crew icons of the friends in free roam */
  private crewInvites: CrewInvites;
  /** the Your Discord row (kept over renders, as the field above), and what the last change of it did ('' fine) */
  private discordInput: HTMLInputElement | null = null;
  private discordRow: HTMLElement | null = null;
  private discordState = { text: '', bad: false };

  private discordField(): HTMLElement {
    const inp = this.discordInput ??= Object.assign(document.createElement('input'), {
      type: 'text', id: 'set-discord', placeholder: t('Discord username'), maxLength: 40, autocomplete: 'off', spellcheck: false,
    });
    if (!this.discordRow) {
      inp.enterKeyHint = 'done';
      inp.setAttribute('aria-describedby', 'set-discord-help set-discord-state');
      inp.onchange = () => void this.setDiscord();   // (Enter, or leaving the field: as the Room setting's)
    }
    const r = this.discordRow ??= document.createElement('div');
    r.className = 'set';
    const label = Object.assign(document.createElement('label'), { className: 'set-label', htmlFor: 'set-discord', textContent: t('Your Discord') });
    const help = Object.assign(document.createElement('p'), { className: 'set-help', id: 'set-discord-help', textContent: t('Only your friends see it, so they can add you on Discord.') });
    const ctl = Object.assign(document.createElement('div'), { className: 'set-control' });
    ctl.append(inp);
    const st = Object.assign(document.createElement('p'), { className: 'set-state bad', id: 'set-discord-state', textContent: this.discordState.text });
    st.setAttribute('aria-live', 'polite');
    st.hidden = !this.discordState.text;
    inp.placeholder = t('Discord username');
    r.replaceChildren(label, help, ctl, st);
    return r;
  }

  /** the field's username to the server (friends.rs "discord"): as Discord allows one, or none */
  private async setDiscord() {
    const inp = this.discordInput;
    if (!inp) return;
    const typed = inp.value.trim(), name = typed ? discordName(typed) : '';
    const say = (text: string) => { this.discordState = { text, bad: !!text }; };
    if (name === (this.lists?.contact ?? '')) { inp.value = name; say(''); this.render(); return; }
    if (name === null) { say(t('Not a Discord username: 2 to 32 small letters, digits, _ or .')); this.render(); return; }
    let r: FriendLists | null = null;
    try { r = await this.deps.api.discord(name); } catch (e) { this.deps.log(`[friends] Discord name failed: ${e}`); }
    this.deps.log(`[friends] Discord name ${name ? 'set' : 'taken away'}: ${r?.done ?? 'no answer'}`);
    this.took(r);
    const done = r?.done ?? '';
    if (done === 'ok') { inp.value = r?.contact ?? ''; say(''); }
    else if (done === 'name') say(t('{name} isn\'t allowed here.', { name }));
    else if (done === 'rate') say(t('Wait a moment'));
    else say(t('Not saved: try again in a moment'));
    this.render();
  }

  private async act(op: 'accept' | 'decline' | 'remove' | 'block', name: string) {
    try {
      const r = await this.deps.api.act(op, name);
      this.took(r);
      this.deps.log(`[friends] ${op} ${name}: ${r?.done ?? 'no answer'}`);
    } catch (e) {
      this.deps.log(`[friends] ${op} ${name} failed: ${e}`);
    }
    this.render();
  }

  /** the Friends & crews tab's Friends (no lists: no passphrase, or a server without friends; no name: none claimed) */
  render() {
    const l = this.lists, root = this.root;
    // a row as the Crews setting's above it: the name, a line of help, the list; past a page Filter on its right
    const row = (help: string, find: HTMLElement | null, ...ctl: HTMLElement[]) => {
      const r = document.createElement('div'); r.className = find ? 'set set-list fl-paged' : 'set set-list';
      const label = document.createElement('div'); label.className = 'set-label'; label.textContent = t('Friends');
      const p = document.createElement('p'); p.className = 'set-help'; p.textContent = help;
      r.append(label, p);
      if (find) r.append(find);
      if (ctl.length) { const c = document.createElement('div'); c.className = 'set-control'; c.append(...ctl); r.append(c); }
      return r;
    };
    this.deps.waiting?.(l?.me ? l.asks.length + l.friends.reduce((n, f) => n + this.deps.store.unread(f.name), 0) : 0);
    if (!l?.me) {
      root.replaceChildren(row(t('Claim your name (Online, Your name) to add friends.'), null));
      return;
    }
    const lines: Line[] = [];
    for (const name of l.asks) {
      lines.push({ name, state: t('wants to be friends'), ok: true, acts: [
        icon('fl-yes', t('Accept'), CHECK, () => this.act('accept', name)),
        icon('fl-no', t('Decline'), CROSS, () => this.act('decline', name)),
      ] });
    }
    for (const f of [...l.friends].sort((a, b) => ORDER[a.where] - ORDER[b.where])) {
      const acts: HTMLButtonElement[] = [];
      if (f.contact) acts.push(discordIcon(f.contact));
      if (f.where === 'roam' && f.room !== undefined) acts.push(icon('fl-join', t('Join'), JOIN, () => this.deps.join(f.room!)));
      if (f.where === 'roam') acts.push(...this.crewInvites.acts(f.name));
      const unread = this.deps.store.unread(f.name);
      acts.push(icon(unread ? 'fl-msg unread' : 'fl-msg', unread ? t('Message ({n})', { n: unread }) : t('Message'), ENVELOPE, () => this.deps.chat(f.name)));
      acts.push(icon('fl-del', t('Remove'), TRASH, async () => {
        if (await this.deps.confirm(t('Remove {name}?', { name: f.name }), t('You are friends no more, on both lists.'), t('Remove'))) void this.act('remove', f.name);
      }));
      lines.push({ name: f.name, state: whereText(f), ok: !!f.where, off: !f.where, acts, under: f.where === 'roam' ? this.crewInvites.under(f.name) : null });
    }
    for (const name of l.sent) {
      lines.push({ name, state: t('request sent'), off: true, acts: [icon('fl-del', t('Take back'), CROSS, () => this.act('remove', name))] });
    }
    // (in pages past PAGE_LINES: Filter on the row's right, which looks in all of them, ‹ 1 of 13 › on Add's line)
    const many = lines.length > PAGE_LINES, find = many ? this.findField.text() : '';
    const found = find ? lines.filter(l => l.name.toLowerCase().includes(find)) : lines;
    const pages = Math.max(1, Math.ceil(found.length / PAGE_LINES));
    this.page = Math.min(this.page, pages - 1);
    const shown = found.slice(this.page * PAGE_LINES, (this.page + 1) * PAGE_LINES);
    const none = Object.assign(document.createElement('p'), { className: 'set-note', textContent: t('No friend has that name') });
    const [add, addNote] = this.askField.nodes();
    const foot = pages > 1 ? Object.assign(document.createElement('div'), { className: 'fl-foot' }) : null;
    foot?.append(add, this.pager(pages));
    const ctl = [...(shown.length ? [friendList(shown)] : find ? [none] : []), foot ?? add, addNote];
    // (the Your Discord field shows the server's name unless the player is changing it)
    const dIn = this.discordInput, dTyping = !!dIn && document.activeElement === dIn;
    const dRow = this.discordField();
    if (!dTyping && !this.discordState.text) this.discordInput!.value = l.contact ?? '';
    // the field stays the same element: a player typing in it keeps the keyboard and the caret
    this.findField.keep(() => this.askField.keep(() => root.replaceChildren(row(t('Touch front bumpers in free roam for 3 s to add a player.'), many ? this.findField.node() : null, ...ctl), dRow)));
    if (dTyping) this.discordInput!.focus({ preventScroll: true });
  }

  /** ‹ 1 of 13 › on the Add field's line: a page back and on (the one pressed keeps the focus, the other one at an end) */
  private pager(pages: number): HTMLElement {
    const box = document.createElement('div');
    box.className = 'fl-pager';
    const go = (by: number, cls: string, other: string) => () => {
      this.page += by;
      this.render();
      const b = this.root.querySelector<HTMLButtonElement>(`.fl-pager .${cls}:enabled`) ?? this.root.querySelector<HTMLButtonElement>(`.fl-pager .${other}`);
      b?.focus({ preventScroll: true });
    };
    const prev = icon('fl-prev', t('Previous page'), PREV, go(-1, 'fl-prev', 'fl-next'));
    const next = icon('fl-next', t('Next page'), NEXT, go(1, 'fl-next', 'fl-prev'));
    prev.disabled = this.page === 0;
    next.disabled = this.page >= pages - 1;
    const at = Object.assign(document.createElement('span'), { className: 'fl-page', textContent: t('{n} of {total}', { n: this.page + 1, total: pages }) });
    at.setAttribute('aria-live', 'polite');
    box.append(prev, at, next);
    return box;
  }

  dispose() { clearInterval(this.timer); }
}
