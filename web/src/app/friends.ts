// Free roam's friends (docs/web/PLAYING.md, Friends): two players whose cars touch front bumpers for 3 seconds may
// add each other as friends, and see each other on their start pages from then on.  The server keeps the friends
// lists (EA Messenger's, for claimed names only) and checks everything (web/server-rs/src/app/friends.rs); this is the
// page's side:
// - FriendPrompt, over the game: the game says when the player's car and another's touch nose to nose
//   (runtime/roam_game.c bump_*, NET_KIND_FRIEND), the page asks the server what that player is to it ("can"), and a
//   line under the middle of the picture offers it: "Add Bravo as a friend" (F, a click or a tap on the line, or the
//   page's menu: a controller's every button is the game's), "Bravo wants to be friends: accept", or says why not;
//   gone once the cars part.  It also tells the server the room's name once the player has friends (their Join).
// - FriendsPanel, on the start page (the Online section): the friends with where each one plays and a Join, the
//   requests to answer, the ones sent.
// - FriendsApi: /friends, asked with the keyphrase's token (claim.ts NameClaims does the same for /names).
import type { GameSession } from './game';
import { touchFirst } from './device';
import { t } from './i18n';
import { pageDialog } from './padnav';
import { onlineName } from './claim';

/** two names of one player: the same Online form, in any case (the name over a car, a claimed name) */
function sameName(a: string, b: string): boolean {
  return (onlineName(a) ?? a).toLowerCase() === (onlineName(b) ?? b).toLowerCase();
}

/** a friend as the start page shows it: where it plays ('roam' free roam, 'online' the Online lobby, '' neither),
 *  its room's name in free roam ('' the room of everyone; none: its page did not say), and its line for its friends */
export interface Friend { name: string; where: 'roam' | 'online' | ''; room?: string; contact?: string }
/** the lists: the player's claimed name (null: none, so no friends), its friends, the requests that wait for its
 *  answer and its own that wait; `done` what the last change did (friends.rs) */
export interface FriendLists { me: string | null; friends: Friend[]; asks: string[]; sent: string[]; done?: string }

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
    const friends: Friend[] = Array.isArray(j.friends) ? j.friends.filter((f: any) => typeof f?.name === 'string').map((f: any) => ({
      name: f.name, where: f.where === 'roam' || f.where === 'online' ? f.where : '',
      ...(typeof f.room === 'string' ? { room: f.room } : {}), ...(typeof f.contact === 'string' ? { contact: f.contact } : {}),
    })) : [];
    return { me: typeof j.me === 'string' ? j.me : null, friends, asks: names(j.asks), sent: names(j.sent), done: typeof j.done === 'string' ? j.done : undefined };
  }

  list(): Promise<FriendLists | null> { return this.ask(); }
  act(op: 'accept' | 'decline' | 'remove' | 'block', name: string): Promise<FriendLists | null> {
    return this.ask({ method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ op, name }) });
  }
}

/** where the line goes, in HUD units (640x480 in the picture): its top under the middle (above the game's own
 *  prompts at the bottom, under its outrun line) */
const TOP_U = 128;

/** how often the list is tried until the game's link to the server is up (ms) */
const LIST_TRY_MS = 2000;

/** what touching the other car may lead to: the server's answer to "can", or what the page did since */
type St = 'wait' | 'new' | 'asks' | 'asked' | 'friends' | 'claim' | 'theirs';

export interface FriendPromptDeps {
  /** a friend message to the server on the game's link (net/index.ts friend); false: not connected now */
  send(m: Record<string, unknown>): boolean;
  /** the game's room now ('' none) */
  room(): string;
  /** a line over the game for a moment (shell.ts toast) */
  toast(text: string): void;
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

  constructor(stage: HTMLElement, private deps: FriendPromptDeps) {
    const b = this.root = document.createElement('button');
    b.type = 'button'; b.className = 'pause-hint friend-prompt'; b.tabIndex = -1; b.hidden = true; b.dataset.nfsu2Ui = '';
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
    const tick = () => {
      const now = performance.now();
      if (!this.listed && now - this.triedAt > LIST_TRY_MS) { this.triedAt = now; this.listed = this.deps.send({ op: 'list' }); }
      this.place();
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
        return;
      case 'can':
        if (!mine || !this.who) return;
        if (m.st === 'far') { this.who = null; break; }
        if (['new', 'asks', 'asked', 'friends', 'claim', 'theirs'].includes(m.st)) this.who = { name, st: m.st };
        break;
      case 'add':
        // (the line says it while the cars touch; the note says it once they parted)
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
        if (mine && this.who) this.who = { name, st: 'friends' };
        else this.deps.toast(t('You and {name} are friends now', { name }));
        break;
      default:
        return;
    }
    this.draw();
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

  private draw() {
    const w = this.who, b = this.root;
    if (!w || w.st === 'wait') { b.hidden = true; return; }
    const kbd = (k: string) => { const e = document.createElement('kbd'); e.textContent = k; return e; };
    const key = touchFirst() ? [] : [kbd('F')];
    const text = (s: string) => document.createTextNode(s);
    switch (w.st) {
      case 'new': b.replaceChildren(...key, text(t('Add {name} as a friend', { name: w.name }))); break;
      case 'asks': b.replaceChildren(...key, text(t('{name} wants to be friends: accept', { name: w.name }))); break;
      case 'asked': b.replaceChildren(text(t('Friend request sent to {name}', { name: w.name }))); break;
      case 'friends': b.replaceChildren(text(t('You and {name} are friends', { name: w.name }))); break;
      case 'claim': b.replaceChildren(text(t('Claim your name on the start page to add friends'))); break;
      case 'theirs': b.replaceChildren(text(t('{name} has no claimed name, so can\'t add friends yet', { name: w.name }))); break;
    }
    b.classList.toggle('offer', this.actionable());
    b.hidden = false;
  }

  private place() {
    const s = this.session;
    if (!s || this.root.hidden) return;
    const v = s.viewRect();
    if (!v.w || !v.h) return;
    const u = Math.min(v.w / 640, v.h / 480), st = this.root.style;
    st.left = `${v.x + v.w / 2}px`;
    st.top = `${v.y + v.h / 2 + TOP_U * u}px`;
  }
}

export interface FriendsPanelDeps {
  api: FriendsApi;
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

export class FriendsPanel {
  private lists: FriendLists | null = null;
  private timer = 0;
  private busy = false;

  constructor(private root: HTMLElement, private deps: FriendsPanelDeps) {
    document.addEventListener('visibilitychange', () => { if (!document.hidden) void this.refresh(); });
    this.timer = window.setInterval(() => void this.refresh(), POLL_MS);
  }

  /** the list now (the keyphrase or its name changed, a game ended) */
  async refresh(force = false) {
    if (this.busy || (!force && (!this.deps.idle() || document.hidden))) return;
    this.busy = true;
    try {
      this.lists = await this.deps.api.list();
    } catch (e) {
      this.deps.log(`[friends] the list could not be read: ${e}`);
    } finally {
      this.busy = false;
    }
    this.render();
  }

  private async act(op: 'accept' | 'decline' | 'remove' | 'block', name: string) {
    try {
      const r = await this.deps.api.act(op, name);
      if (r) this.lists = r;
      this.deps.log(`[friends] ${op} ${name}: ${r?.done ?? 'no answer'}`);
    } catch (e) {
      this.deps.log(`[friends] ${op} ${name} failed: ${e}`);
    }
    this.render();
  }

  render() {
    const l = this.lists, root = this.root;
    root.hidden = !l;
    if (!l) { root.replaceChildren(); return; }
    const h = document.createElement('h3');
    h.textContent = t('Friends');
    const nodes: Node[] = [h];
    const help = (s: string) => { const p = document.createElement('p'); p.className = 'hint'; p.textContent = s; return p; };
    if (!l.me) {
      nodes.push(help(t('Claim your name (Your name, above) to add friends.')));
      root.replaceChildren(...nodes);
      return;
    }
    nodes.push(help(t('In free roam, touch front bumpers with another player for 3 seconds to add them.')));
    for (const name of l.asks) {
      nodes.push(this.row(name, t('Wants to be friends'), 'ok', [
        this.button(t('Accept'), 'btn', () => this.act('accept', name)),
        this.button(t('Decline'), 'text-btn', () => this.act('decline', name)),
      ]));
    }
    for (const f of l.friends) {
      const where = f.where === 'roam'
        ? (f.room === '' ? t('In free roam') : f.room ? t('In free roam, room {room}', { room: f.room }) : t('In free roam, another room'))
        : f.where === 'online' ? t('In the Online lobby') : t('Offline');
      const ctl: HTMLElement[] = [];
      if (f.where === 'roam' && f.room !== undefined) ctl.push(this.button(t('Join'), 'btn', () => this.deps.join(f.room!)));
      ctl.push(this.button(t('Remove'), 'text-btn', async () => {
        if (await this.deps.confirm(t('Remove {name}?', { name: f.name }), t('You are friends no more, on both lists.'), t('Remove'))) void this.act('remove', f.name);
      }));
      nodes.push(this.row(f.name, where, f.where ? 'ok' : 'free', ctl, f.contact));
    }
    for (const name of l.sent) {
      nodes.push(this.row(name, t('Request sent'), 'free', [this.button(t('Cancel'), 'text-btn', () => this.act('remove', name))]));
    }
    root.replaceChildren(...nodes);
  }

  private row(name: string, state: string, cls: string, ctl: HTMLElement[], contact?: string): HTMLElement {
    const r = document.createElement('div');
    r.className = 'set friend';
    const label = document.createElement('div');
    label.className = 'set-label';
    label.textContent = name;
    const help = document.createElement('p');
    help.className = `set-help friend-state ${cls}`;
    help.textContent = state;
    const c = document.createElement('div');
    c.className = 'set-control';
    c.append(...ctl);
    r.append(label, help, c);
    if (contact) {
      const p = document.createElement('p');
      p.className = 'set-state friend-contact';
      p.textContent = contact;
      r.append(p);
    }
    return r;
  }

  private button(text: string, cls: string, f: () => void): HTMLButtonElement {
    const b = document.createElement('button');
    b.type = 'button'; b.className = cls; b.textContent = text;
    b.onclick = () => f();
    return b;
  }

  dispose() { clearInterval(this.timer); }
}
