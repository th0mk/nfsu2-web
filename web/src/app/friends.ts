// Free roam's friends (docs/web/PLAYING.md, Friends): two players whose cars touch front bumpers for 3 seconds may
// add each other as friends, and see each other on their start pages from then on.  The server keeps the friends
// lists (EA Messenger's, for claimed names only) and checks everything (web/server-rs/src/app/friends.rs); this is the
// page's side:
// - FriendPrompt, over the game: the game says when the player's car and another's touch nose to nose
//   (runtime/roam_game.c bump_*, NET_KIND_FRIEND), the page asks the server what that player is to it ("can"), and a
//   line in quick chat's look, just above the game's outrun line, offers it: "Add Bravo as a friend" (F, a click or a tap
//   on the line, or the page's menu: a controller's every button is the game's), "Bravo wants to be friends: accept", or says why not;
//   gone once the cars part.  It also tells the server the room's name once the player has friends (their Join), asks
//   where the friends play every minute ("where"), says in the notices when one comes into free roam, and fills the
//   page menu's Friends popup (Join a friend's room).
// - FriendsPanel, on the start page (its Friends & crews tab): one line a friend, where it plays after its name and a
//   Join in free roam, the requests to answer, the ones sent; icons for what each line does, a friend's Message among
//   them (lime with a dot while messages wait to be read: messages.ts); under them a field to ask a player by name.
// - FriendsApi: /friends, asked with the keyphrase's token (claim.ts NameClaims does the same for /names).
import type { GameSession } from './game';
import { touchFirst } from './device';
import { t } from './i18n';
import { pageDialog } from './padnav';
import { onlineName } from './claim';
import { roomHash } from './players';
import { iconButton } from './settings-ui';
import { mailOf, type MessageStore } from './messages';

/** two names of one player: the same Online form, in any case (the name over a car, a claimed name) */
function sameName(a: string, b: string): boolean {
  return (onlineName(a) ?? a).toLowerCase() === (onlineName(b) ?? b).toLowerCase();
}

/** a friend as the start page shows it: where it plays ('roam' free roam, 'online' the Online lobby, '' neither),
 *  its room's name in free roam ('' the room of everyone; none: its page did not say), and its line for its friends */
export interface Friend { name: string; where: 'roam' | 'online' | ''; room?: string; contact?: string }
/** the lists: the player's claimed name (null: none, so no friends), its friends, the requests that wait for its
 *  answer and its own that wait, the friends' messages that waited for it (gone from the server now); `done` what the
 *  last change did (friends.rs) */
export interface FriendLists {
  me: string | null; friends: Friend[]; asks: string[]; sent: string[]; mail: { from: string; body: string; t: number }[]; done?: string;
}

/** the friends of an answer of the server's (GET /friends, "where"), read leniently */
function friendsOf(v: unknown): Friend[] {
  return Array.isArray(v) ? v.filter((f: any) => typeof f?.name === 'string').map((f: any) => ({
    name: f.name, where: f.where === 'roam' || f.where === 'online' ? f.where : '',
    ...(typeof f.room === 'string' ? { room: f.room } : {}), ...(typeof f.contact === 'string' ? { contact: f.contact } : {}),
  })) : [];
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
      done: typeof j.done === 'string' ? j.done : undefined,
    };
  }

  list(): Promise<FriendLists | null> { return this.ask(); }
  act(op: 'accept' | 'decline' | 'remove' | 'block' | 'ask', name: string): Promise<FriendLists | null> {
    return this.ask({ method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ op, name }) });
  }
  /** a message to friend `name` (messages.ts): the lists after it, `done` what became of it */
  send(name: string, body: string): Promise<FriendLists | null> {
    return this.ask({ method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ op: 'msg', name, body }) });
  }
}

/** the room f plays in is `mine` (the game's room: '' none), in free roam */
function inRoom(f: Friend, mine: string): boolean {
  return f.where === 'roam' && f.room !== undefined && roomHash(f.room) === roomHash(mine);
}

/** where a friend plays, after its name: free roam (and its room; `mine`: the game's own room, in the game), the
 *  Online lobby, offline */
export function whereText(f: Friend, mine?: string): string {
  if (f.where === 'roam') {
    if (mine !== undefined && inRoom(f, mine)) return t('free roam · your room');
    return f.room ? t('free roam · {room}', { room: f.room }) : t('free roam');
  }
  return f.where === 'online' ? t('Online lobby') : t('offline');
}

/** the signs of the friends' icons (24-unit boxes, as settings-ui.ts's) */
const CHECK = 'M5 12.5l4.5 4.5L19 7.5', CROSS = 'M6 6l12 12M18 6L6 18', TRASH = 'M4 7h16M10 11v6M14 11v6M9 7V4h6v3M6 7l1 13h10l1-13';
const JOIN = 'M10 17l5-5-5-5M15 12H3M14 4h6v16h-6', ENVELOPE = 'M3 6h18v12H3zM3 7l9 6 9-6', PLUS = 'M12 5v14M5 12h14';

/** a line of a friends list: the name, where it plays (or the request's state) after it in small text, lime while it
 *  plays; dimmed while it does not; the icons at its end; the friend's own line under it, when it has one */
interface Line { name: string; state: string; ok?: boolean; off?: boolean; acts: HTMLButtonElement[]; contact?: string }

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
    const st = document.createElement('span');
    st.className = l.ok ? 'fl-st ok' : 'fl-st';
    st.textContent = l.state;
    name.append(l.name, st);
    const acts = document.createElement('span');
    acts.className = 'fl-act';
    acts.append(...l.acts);
    li.append(name, acts);
    if (l.contact) {
      const c = document.createElement('span');
      c.className = 'fl-contact';
      c.textContent = l.contact;
      li.append(c);
    }
    ul.append(li);
  }
  return ul;
}

/** how often the list is tried until the game's link to the server is up (ms) */
const LIST_TRY_MS = 2000;
/** how often the game asks where its friends play, while it has some (ms) */
const WHERE_MS = 60_000;

/** what touching the other car may lead to: the server's answer to "can", or what the page did since */
type St = 'wait' | 'new' | 'asks' | 'asked' | 'friends' | 'claim' | 'theirs';

/** the line's top edge, in the game's HUD units from the middle of the picture: just above the game's outrun line
 *  (runtime/roam_game.c du_hud: its middle at -165), which shows too, as the cars are near each other */
const LINE_TOP_U = -195;
/** the line's icon (18x16 units): a person with a plus while it offers something, else a person */
const PERSON_PLUS = 'M6 8a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM0 15c0-3.3 2.7-5.5 6-5.5s6 2.2 6 5.5zM14.5 3.25h1.5v2h2v1.5h-2v2h-1.5v-2h-2v-1.5h2z';
const PERSON = 'M9 8a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM3 15c0-3.3 2.7-5.5 6-5.5s6 2.2 6 5.5z';
function personIcon(d: string): SVGSVGElement {
  const ns = 'http://www.w3.org/2000/svg', s = document.createElementNS(ns, 'svg'), p = document.createElementNS(ns, 'path');
  s.setAttribute('viewBox', '0 0 18 16'); s.setAttribute('aria-hidden', 'true');
  p.setAttribute('d', d);
  s.append(p);
  return s;
}

export interface FriendPromptDeps {
  /** a friend message to the server on the game's link (net/index.ts friend); false: not connected now */
  send(m: Record<string, unknown>): boolean;
  /** the game's room now ('' none) */
  room(): string;
  /** a line over the game for a moment (shell.ts toast) */
  toast(text: string): void;
  /** a friend's line in the notices (shell.ts friendNotice): text names it */
  notice(name: string, text: string): void;
  /** "the menu (F9)", with what opens it here (shell.ts menuName) */
  menu(): string;
  /** where the friends play changed (the page menu's Friends popup is drawn again) */
  placed(): void;
  log(line: string): void;
}

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
  /** where the line was put last (place) */
  private spot = '';

  /** stage: the game's box; the line goes over the picture while the cars touch (place) */
  constructor(stage: HTMLElement, private deps: FriendPromptDeps) {
    const b = this.root = document.createElement('button');
    b.type = 'button'; b.className = 'friend-prompt'; b.tabIndex = -1; b.hidden = true; b.dataset.nfsu2Ui = '';
    b.onpointerdown = e => e.preventDefault();     // the canvas keeps the focus (and with it the keyboard)
    b.onclick = () => this.act();
    stage.append(b);
    // caught before the game's input: F is the game's for nothing (its key maps: dinput_kbd.c), and only while the
    // line offers something
    window.addEventListener('keydown', e => this.key(e), { capture: true });
  }

  /** a game started: friends on (free roam with others) or off */
  start(s: GameSession, on: boolean) {
    this.session = s; this.on = on; this.who = null; this.friends = []; this.roomSent = null; this.listed = false; this.triedAt = -Infinity;
    this.where = null; this.was.clear(); this.whereAt = -Infinity;
    const tick = () => {
      const now = performance.now();
      if (!this.listed && now - this.triedAt > LIST_TRY_MS) { this.triedAt = now; this.listed = this.deps.send({ op: 'list' }); }
      if (this.listed && this.friends.length && now - this.whereAt > WHERE_MS) this.askWhere();
      if (!this.root.hidden) this.place();         // (the window may change size while it shows)
      this.raf = requestAnimationFrame(tick);
    };
    if (on) this.raf = requestAnimationFrame(tick);
  }
  stop() {
    cancelAnimationFrame(this.raf);
    this.session = null; this.on = false; this.who = null;
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

  /** where the friends play, asked now (and again in WHERE_MS): the page menu's Friends popup opened */
  askWhere() {
    if (!this.on || !this.friends.length) return;
    this.whereAt = performance.now();
    this.deps.send({ op: 'where' });
  }

  /** the page menu has its Friends button: free roam with friends */
  hasFriends(): boolean {
    return this.on && this.friends.length > 0;
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
      case 'list':
        this.friends = Array.isArray(m.friends) ? m.friends.filter((f: unknown) => typeof f === 'string') : [];
        this.sendRoom();
        this.whereAt = -Infinity;                    // (where they are, with the next frame)
        return;
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
        if (m.done === 'friends') this.was.set(name.toLowerCase(), 'roam');
        if (m.done === 'friends' && !mine) this.deps.toast(t('You and {name} are friends now', { name }));
        else if (m.done === 'full') this.deps.toast(t('A friends list is full (50 at most)'));
        if (mine && this.who && ['asked', 'friends', 'claim', 'theirs'].includes(m.done)) this.who = { name, st: m.done };
        else if (mine && m.done === 'full') this.who = null;
        break;
      case 'asks':
        // the other player pressed first: answer here while the cars touch, or on the start page
        if (mine && this.who) this.who = { name, st: 'asks' };
        else this.deps.toast(t('{name} wants to be friends: answer under Friends on the start page', { name }));
        break;
      case 'friends':
        // (just met here: no notice that it is in free roam)
        this.was.set(name.toLowerCase(), 'roam');
        if (mine && this.who) this.who = { name, st: 'friends' };
        else this.deps.toast(t('You and {name} are friends now', { name }));
        break;
      default:
        return;
    }
    this.draw();
  }

  /** the server's "where": a notice for each friend that came into free roam since the last one (or is there at the
   *  first), the popup drawn again */
  private placedNow(list: Friend[]) {
    const room = this.deps.room(), was = this.was;
    this.was = new Map(list.map(f => [f.name.toLowerCase(), f.where]));
    this.where = list;
    for (const f of list) {
      if (f.where !== 'roam' || was.get(f.name.toLowerCase()) === 'roam') continue;
      const name = f.name, menu = this.deps.menu();
      this.deps.notice(name, f.room === undefined || inRoom(f, room) ? t('Your friend {name} is in free roam', { name })
        : f.room ? t('Your friend {name} is in free roam, room {room}: Join in {menu}', { name, room: f.room, menu })
        : t('Your friend {name} is in free roam, in no room: Join in {menu}', { name, menu }));
    }
    this.deps.placed();
  }

  /** The page menu's Friends popup, in box: the friends in free roam, a Join for each one in another room (join: the
   *  room's name, '' the room of everyone); the others in a line */
  drawPlaced(box: HTMLElement, join: (room: string) => void) {
    const p = (text: string) => Object.assign(document.createElement('p'), { className: 'hint', textContent: text });
    const all = this.where, room = this.deps.room();
    if (!all) { box.replaceChildren(p(t('Checking.'))); return; }
    const roam = all.filter(f => f.where === 'roam');
    const nodes: Node[] = [];
    if (roam.length) {
      nodes.push(friendList(roam.map(f => ({
        name: f.name, state: whereText(f, room), ok: true,
        acts: f.room !== undefined && !inRoom(f, room) ? [icon('fl-join', t('Join'), JOIN, () => join(f.room!))] : [],
      }))));
    } else {
      nodes.push(p(t('None of your friends is in free roam now.')));
    }
    const names = (w: Friend['where']) => all.filter(f => f.where === w).map(f => f.name).join(', ');
    const rest = [names('online') && t('In the Online lobby: {names}.', { names: names('online') }), names('') && t('Offline: {names}.', { names: names('') })];
    if (rest.some(Boolean)) nodes.push(p(rest.filter(Boolean).join(' ')));
    box.replaceChildren(...nodes);
  }

  /** what the line offers: a request, or the answer to theirs */
  private actionable(): boolean {
    return !!this.who && (this.who.st === 'new' || this.who.st === 'asks');
  }

  private act() {
    if (!this.actionable() || !this.who) return;
    this.deps.log(`[friends] ${this.who.st === 'asks' ? 'accepting' : 'asking'} ${this.who.name}`);
    if (this.deps.send({ op: 'add', name: this.who.name })) { this.who = { ...this.who, st: 'wait' }; this.draw(); }
  }

  /** the page's menu (shell.ts): its button for the line while the line offers something, else null */
  menuItem(): { label: string; act: () => void } | null {
    if (!this.actionable() || !this.who) return null;
    const name = this.who.name;
    return { label: this.who.st === 'asks' ? t('Accept {name} as a friend', { name }) : t('Add {name} as a friend', { name }), act: () => this.act() };
  }

  private key(e: KeyboardEvent) {
    if (e.code !== 'KeyF' || e.metaKey || e.ctrlKey || e.altKey || !this.actionable()) return;
    const el = e.target as HTMLElement | null;
    if (el?.closest?.('input, textarea, select') || pageDialog()) return;
    e.preventDefault(); e.stopImmediatePropagation();
    if (!e.repeat) this.act();
  }

  /** the line in the middle of the picture, its top LINE_TOP_U HUD units above the middle (--u: one unit, its size) */
  private place() {
    const v = this.session?.viewRect();
    if (!v || !v.w || !v.h) return;
    const u = Math.min(v.w / 640, v.h / 480), x = v.x + v.w / 2, y = v.y + v.h / 2 + LINE_TOP_U * u;
    const spot = `${x} ${y} ${u}`;
    if (spot === this.spot) return;
    this.spot = spot;
    const s = this.root.style;
    s.left = `${x}px`;
    s.setProperty('--y', `${y}px`);
    s.setProperty('--u', `${u}px`);
  }

  private draw() {
    const w = this.who, b = this.root;
    if (!w || w.st === 'wait') { b.hidden = true; return; }
    const kbd = (k: string) => { const e = document.createElement('kbd'); e.textContent = k; return e; };
    const offer = this.actionable(), key = offer && !touchFirst() ? [kbd('F')] : [];
    const text = (s: string) => Object.assign(document.createElement('span'), { textContent: s });
    const line = (s: string) => b.replaceChildren(personIcon(offer ? PERSON_PLUS : PERSON), ...key, text(s));
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
  log(line: string): void;
}

/** how often the start page asks for the list while it is in front */
const POLL_MS = 60_000;

/** the friends in the order the list shows them: in free roam, in the Online lobby, offline */
const ORDER: Record<Friend['where'], number> = { roam: 0, online: 1, '': 2 };

export class FriendsPanel {
  private lists: FriendLists | null = null;
  private timer = 0;
  private busy = false;
  private again = false;

  constructor(private root: HTMLElement, private deps: FriendsPanelDeps) {
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

  /** the field to ask a player by name (kept over renders: the player may be typing), and what the last ask did */
  private addInput: HTMLInputElement | null = null;
  private addBox: HTMLElement | null = null;
  private addNote = { text: '', warn: false };

  private addField(): HTMLElement {
    if (this.addBox) return this.addBox;
    const box = document.createElement('div');
    box.className = 'crew-add fl-add';
    const inp = this.addInput = document.createElement('input');
    inp.type = 'text'; inp.placeholder = t('Add a player by name'); inp.setAttribute('aria-label', t('Add a player by name'));
    inp.autocomplete = 'off'; inp.spellcheck = false; inp.enterKeyHint = 'send'; inp.maxLength = 32;
    const go = icon('fl-ask', t('Send friend request'), PLUS, () => void this.ask());
    go.disabled = true;
    inp.oninput = () => { go.disabled = !inp.value.trim(); };
    inp.onkeydown = e => { if (e.key === 'Enter' && !e.isComposing) { e.preventDefault(); void this.ask(); } };
    box.append(inp, go);
    return this.addBox = box;
  }

  /** a request to the player whose name is in the field (friends.rs "ask": any claimed name, wherever they are) */
  private async ask() {
    const inp = this.addInput, name = inp?.value.trim() ?? '';
    if (!inp || !name) return;
    let r: FriendLists | null = null;
    try { r = await this.deps.api.act('ask', name); } catch (e) { this.deps.log(`[friends] ask failed: ${e}`); }
    this.deps.log(`[friends] ask by name: ${r?.done ?? 'no answer'}`);
    this.took(r);
    const done = r?.done ?? '', shown = [...(r?.sent ?? []), ...(r?.friends.map(f => f.name) ?? [])].find(n => sameName(n, name)) ?? name;
    const say = (text: string, warn: boolean) => { this.addNote = { text, warn }; };
    if (done === 'asked') say(t('Friend request sent to {name}', { name: shown }), false);
    else if (done === 'friends') say(t('You and {name} are friends', { name: shown }), false);
    else if (done === 'claim') say(t('{name} has no claimed name, so can\'t add friends yet', { name }), true);
    else if (done === 'self') say(t('That\'s your own name'), true);
    else if (done === 'name') say(t('Not a player\'s name'), true);
    else if (done === 'full') say(t('A friends list is full (50 at most)'), true);
    else if (done === 'rate') say(t('Wait a moment'), true);
    else say(t('Not sent: try again in a moment'), true);
    if (done === 'asked' || done === 'friends') { inp.value = ''; inp.dispatchEvent(new Event('input')); }
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
    // a row as the Crews setting's above it: the name, a line of help, the list
    const row = (help: string, ...ctl: HTMLElement[]) => {
      const r = document.createElement('div'); r.className = 'set set-list';
      const label = document.createElement('div'); label.className = 'set-label'; label.textContent = t('Friends');
      const p = document.createElement('p'); p.className = 'set-help'; p.textContent = help;
      r.append(label, p);
      if (ctl.length) { const c = document.createElement('div'); c.className = 'set-control'; c.append(...ctl); r.append(c); }
      return r;
    };
    if (!l?.me) {
      root.replaceChildren(row(t('Claim your name (Online, Your name) to add friends.')));
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
      if (f.where === 'roam' && f.room !== undefined) acts.push(icon('fl-join', t('Join'), JOIN, () => this.deps.join(f.room!)));
      const unread = this.deps.store.unread(f.name);
      acts.push(icon(unread ? 'fl-msg unread' : 'fl-msg', unread ? t('Message ({n})', { n: unread }) : t('Message'), ENVELOPE, () => this.deps.chat(f.name)));
      acts.push(icon('fl-del', t('Remove'), TRASH, async () => {
        if (await this.deps.confirm(t('Remove {name}?', { name: f.name }), t('You are friends no more, on both lists.'), t('Remove'))) void this.act('remove', f.name);
      }));
      lines.push({ name: f.name, state: whereText(f), ok: !!f.where, off: !f.where, acts, contact: f.contact });
    }
    for (const name of l.sent) {
      lines.push({ name, state: t('request sent'), off: true, acts: [icon('fl-del', t('Take back'), CROSS, () => this.act('remove', name))] });
    }
    const note = document.createElement('p');
    note.className = 'set-note fl-note' + (this.addNote.warn ? ' warn' : ''); note.setAttribute('aria-live', 'polite');
    note.textContent = this.addNote.text;
    const ctl = [...(lines.length ? [friendList(lines)] : []), this.addField(), note];
    // the field stays the same element: a player typing in it keeps the keyboard and the caret
    const inp = this.addInput!, typing = document.activeElement === inp, at = [inp.selectionStart, inp.selectionEnd];
    root.replaceChildren(row(t('Touch front bumpers in free roam for 3 s to add a player.'), ...ctl));
    if (typing) { inp.focus({ preventScroll: true }); inp.setSelectionRange(at[0], at[1]); }
  }

  dispose() { clearInterval(this.timer); }
}
