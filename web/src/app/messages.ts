// Messages between friends (docs/web/PLAYING.md, Friends): typed, at most MSG_MAX characters, to a friend only.  The
// server checks and delivers (web/server-rs/src/app/friends.rs, nfsu2-online buddy.rs page_send): to the friend's EA
// Messenger when it is in the game's Online mode, else to its open pages, else the message waits 7 days and the
// friend's page takes it with its friends list.  The server keeps a message only until it is delivered; the
// conversations are kept here, in this browser (MessageStore).
//   in the game  free roam's log on the left (quick chat's place: shell.ts msgLine), an envelope, the name and the
//                text, wrapping; "I Write back" under the newest.  I opens a card in quick chat's picker place and
//                look: the answer to the newest message of this game, or (none, or older than REPLY_MS) a friend to
//                pick first, 1-9.  Enter sends, Esc closes; the game has no keys while the card is open.  On a touch
//                screen a tap on a message answers it, and quick chat's sheet has "Message a friend".  A message that
//                comes while the player is not driving in free roam (a race, a shop, a menu) shows once they are.
//   wire         {"t":"friend","op":"msg","name","body","id"} -> {"op":"msg","id","done"};
//                {"op":"mail","from","body","at"} and the list's "mail" bring the friends' (friends.rs).
//   start page   friends.ts FriendsPanel: Message on each friend, the unread ones counted on it; ChatDialog.
import type { GameSession } from './game';
import { t } from './i18n';
import { touchFirst } from './device';

/** an envelope: what marks a line in the log as a friend's message (quick chat's have a speech balloon) */
export const MSG_ICON = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M1.5 3.5h13v9h-13z" fill="none" stroke="currentColor" stroke-width="1.6"/><path d="M1.8 4l6.2 5 6.2-5" fill="none" stroke="currentColor" stroke-width="1.6"/></svg>';
/** the key that opens the card while driving in free roam, and its label.  Not T: the game's Career Status (T and F4,
 *  its driving key map, dinput_kbd.c DIMAP=1, action 0x12; issue #52).  The game's driving map leaves E I O V Z free;
 *  I is away from W A S D and the arrows, so an accidental press (the card takes the keys) is unlikely */
export const MSG_KEY = 'KeyI', MSG_KEY_LABEL = 'I';
/** a message at most (characters; the server's PAGE_MSG_LEN): three lines of the log */
export const MSG_MAX = 120;
/** I answers the newest message of this game when it came less than this long ago (ms); after that it asks whom */
const REPLY_MS = 120_000;
/** a message's line in the log: this long, and longer for a long one (ms; quick chat's are 8 s) */
const LINE_MS = 8000, LINE_MS_PER_CHAR = 60, LINE_MS_MAX = 15_000;
/** a message the server has not answered for this long did not go (ms) */
const SEND_WAIT_MS = 10_000;
/** what the store keeps: messages per friend, friends */
const KEEP_MSGS = 40, KEEP_FRIENDS = 60;
/** the friend list's rows at most (keys 1-9) */
const PICK_MAX = 9;

/** one message of a conversation: from the friend ('in') or the player ('out'), when (ms since 1970), and what became
 *  of the player's: on its way, got by the friend's game or page, waiting for the friend, not sent */
export interface Msg { dir: 'in' | 'out'; body: string; t: number; st?: 'wait' | 'sent' | 'kept' | 'failed' }
interface Talk { name: string; msgs: Msg[]; unread: number }

const keyOf = (name: string) => name.toLowerCase();

/** The conversations of the player's claimed name, in this browser's storage (none: kept while the page is open). */
export class MessageStore {
  private me = '';
  private talks: Record<string, Talk> = {};
  private listeners = new Set<() => void>();

  /** the player's claimed name (the friends list's "me"): its conversations (null: none) */
  setMe(me: string | null) {
    const k = me ? keyOf(me) : '';
    if (k === this.me) return;
    // what came before the name was known (a message ahead of the friends list) joins the name's
    const early = this.me ? {} : this.talks;
    this.me = k;
    this.talks = {};
    if (k) {
      try {
        const v = JSON.parse(localStorage.getItem(this.storeKey()) ?? '{}');
        if (v && typeof v === 'object') this.talks = v;
      } catch { /* none kept */ }
    }
    for (const [f, tk] of Object.entries(early)) {
      const to = this.talks[f] ??= { name: tk.name, msgs: [], unread: 0 };
      to.msgs.push(...tk.msgs);
      to.unread += tk.unread;
    }
    this.changed(Object.keys(early).length > 0);
  }

  private storeKey() { return `nfsu2.msgs.${this.me}`; }

  private changed(save = true) {
    if (save && this.me) {
      // the friends talked to last stay
      const ks = Object.keys(this.talks);
      if (ks.length > KEEP_FRIENDS) {
        const last = (k: string) => this.talks[k].msgs.at(-1)?.t ?? 0;
        for (const k of ks.sort((a, b) => last(a) - last(b)).slice(0, ks.length - KEEP_FRIENDS)) delete this.talks[k];
      }
      try { localStorage.setItem(this.storeKey(), JSON.stringify(this.talks)); } catch { /* not kept */ }
    }
    for (const f of this.listeners) f();
  }

  onChange(f: () => void) { this.listeners.add(f); }

  /** a message to or from friend `name`; unread: one of theirs not seen yet */
  add(name: string, m: Msg, unread: boolean) {
    const k = keyOf(name), tk = this.talks[k] ??= { name, msgs: [], unread: 0 };
    tk.name = name;
    tk.msgs.push(m);
    if (tk.msgs.length > KEEP_MSGS) tk.msgs.splice(0, tk.msgs.length - KEEP_MSGS);
    if (unread) tk.unread++;
    this.changed();
  }

  /** what became of the player's message to `name` sent at `at` */
  mark(name: string, at: number, st: Msg['st']) {
    const m = this.talks[keyOf(name)]?.msgs.find(x => x.dir === 'out' && x.t === at);
    if (m) { m.st = st; this.changed(); }
  }

  messages(name: string): readonly Msg[] { return this.talks[keyOf(name)]?.msgs ?? []; }
  unread(name: string): number { return this.talks[keyOf(name)]?.unread ?? 0; }
  read(name: string) {
    const tk = this.talks[keyOf(name)];
    if (tk?.unread) { tk.unread = 0; this.changed(); }
  }
  /** the newest message from `name` (none: undefined) */
  lastIn(name: string): Msg | undefined { return this.messages(name).findLast(m => m.dir === 'in'); }
}

/** the messages that came with a list or one by one (friends.rs "mail"), as the store takes them */
export function mailOf(v: unknown): { from: string; body: string; t: number }[] {
  return Array.isArray(v)
    ? v.filter((m: any) => typeof m?.from === 'string' && typeof m?.body === 'string')
      .map((m: any) => ({ from: m.from, body: m.body.slice(0, MSG_MAX * 2), t: Number(m.at) || Date.now() }))
    : [];
}

/** what a "done" other than sent or kept says to the player (null: nothing) */
export function sendTrouble(done: string, name: string): string | null {
  switch (done) {
    case 'sent': case 'kept': case 'empty': return null;
    case 'none': return t('{name} is not on your friends list', { name });
    case 'off': return t('You can\'t send messages for a few minutes');
    case 'rate': return t('Wait a moment');
    case 'claim': return t('Claim your name on the start page to send messages');
    default: return t('The message was not sent: try again in a moment');
  }
}

export interface FriendMessagesDeps {
  /** a friend message to the server on the game's link (net/index.ts friend); false: not connected now */
  send(m: Record<string, unknown>): boolean;
  /** driving in free roam now (quickchat.ts driving) */
  driving(): boolean;
  /** a message's line in the log (shell.ts msgLine): its name, text, whether it is the player's, a note after it, how
   *  long it stays, and what a tap on it does (none: nothing); returns the line */
  line(name: string, text: string, out: boolean, note: string, ms: number, tap?: () => void): HTMLElement;
  /** "I Write back" under the newest message, for ms (shell.ts msgHint); null: gone */
  hint(ms: number | null): void;
  /** the card's place: the log's (shell.ts placeNotices calls place()) */
  placeNow(): void;
  /** the keyboard back to the game */
  focusGame(): void;
  toast(text: string): void;
  log(line: string): void;
}

type Friend = { name: string; at: string };

/** Messages between friends in the game (this module's doc). */
export class FriendMessages {
  private session: GameSession | null = null;
  private on = false;
  private card: HTMLElement;
  /** the card: closed, a friend to pick, or a message to `to` */
  private mode: null | 'pick' | 'write' = null;
  private to = '';
  private touch = false;
  /** the friends as the server last listed them, and where each plays */
  private friends: Friend[] = [];
  private listed = false;
  /** the newest message of this game that was shown, for MSG_KEY */
  private newest: { name: string; at: number } | null = null;
  /** the friends' messages not shown yet (the player was not driving) */
  private queue: { from: string; body: string }[] = [];
  /** the player's messages on their way: page id -> the friend, its time (the store's key), its text, when it went */
  private waiting = new Map<number, { name: string; t: number; body: string; at: number }>();
  private nextId = 1;
  private timer = 0;

  constructor(stage: HTMLElement, private store: MessageStore, private deps: FriendMessagesDeps) {
    const c = this.card = document.createElement('div');
    c.className = 'qc-picker msg-card'; c.hidden = true; c.tabIndex = -1; c.dataset.nfsu2Ui = '';
    c.addEventListener('click', e => {
      const row = (e.target as HTMLElement).closest<HTMLElement>('[data-friend]');
      if (row) this.write(row.dataset.friend!);
    });
    c.addEventListener('keydown', e => this.cardKey(e));
    stage.append(c);
    // MSG_KEY, caught before the game's input and quick chat's; taken only while driving in free roam with others
    window.addEventListener('keydown', e => this.key(e), { capture: true });
    stage.addEventListener('pointerdown', e => {
      if (this.mode && this.touch && !c.contains(e.target as Node) && !(e.target as Element).closest?.('.notices .msg')) this.close();
    }, { capture: true });
  }

  /** a game started: messages on (free roam with others) or off */
  start(s: GameSession, on: boolean) {
    this.session = s; this.on = on; this.friends = []; this.listed = false; this.newest = null; this.queue = []; this.waiting.clear();
    clearInterval(this.timer);
    if (on) this.timer = window.setInterval(() => this.tick(), 250);
  }
  stop() {
    clearInterval(this.timer);
    this.close();
    this.session = null; this.on = false; this.queue = []; this.deps.hint(null);
  }

  /** the card's place: the log's (shell.ts placeNotices), u: CSS px per HUD unit */
  place(left: number, top: number, u: number) {
    const s = this.card.style;
    s.left = `${left}px`; s.top = `${top}px`;
    s.setProperty('--u', `${u}px`);
  }

  /** a friend message from the server (friends.rs): the list (with the messages that waited), a message, an answer */
  server(m: any) {
    if (typeof m?.op !== 'string') return;
    if (m.op === 'list') {
      this.store.setMe(typeof m.me === 'string' ? m.me : null);
      const names: string[] = Array.isArray(m.friends) ? m.friends.filter((f: unknown) => typeof f === 'string') : [];
      const at = new Map(this.friends.map(f => [f.name, f.at]));
      this.friends = names.map(name => ({ name, at: at.get(name) ?? '' }));
      this.listed = true;
      for (const x of mailOf(m.mail)) this.incoming(x.from, x.body, x.t);
      if (this.mode === 'pick') this.draw();
    } else if (m.op === 'where') {
      // where each friend plays (friends.ts asks every minute; the card asks when it opens)
      if (!Array.isArray(m.friends)) return;
      this.friends = m.friends.filter((f: any) => typeof f?.name === 'string')
        .map((f: any) => ({ name: f.name, at: f.where === 'roam' || f.where === 'online' ? f.where : '' }));
      this.listed = true;
      if (this.mode === 'pick') this.draw();
    } else if (m.op === 'mail') {
      for (const x of mailOf([m])) this.incoming(x.from, x.body, x.t);
    } else if (m.op === 'msg') {
      const w = this.waiting.get(Number(m.id));
      if (!w) return;
      this.waiting.delete(Number(m.id));
      this.answered(w, String(m.done ?? ''));
    }
  }

  /** a friend's message: kept, and on the log now or once the player drives in free roam */
  private incoming(from: string, body: string, at: number) {
    this.deps.log(`[msg] from ${from}`);
    this.store.add(from, { dir: 'in', body, t: at }, true);
    if (this.on) this.queue.push({ from, body });
    this.tick();
  }

  /** the server's answer to the player's message: its line in the log, or why it did not go */
  private answered(w: { name: string; t: number; body: string }, done: string) {
    this.deps.log(`[msg] to ${w.name}: ${done}`);
    if (done === 'sent' || done === 'kept') {
      this.store.mark(w.name, w.t, done);
      const note = done === 'kept' ? t('(gets it when back)') : '';
      this.deps.line(t('To {name}', { name: w.name }), w.body, true, note, lineMs(w.body));
      return;
    }
    this.store.mark(w.name, w.t, 'failed');
    const why = sendTrouble(done, w.name);
    if (why) this.deps.toast(why);
  }

  private tick() {
    const now = performance.now();
    for (const [id, w] of this.waiting) {
      if (now - w.at > SEND_WAIT_MS) { this.waiting.delete(id); this.answered(w, 'timeout'); }
    }
    if (this.mode && !this.deps.driving()) this.close();
    if (!this.queue.length || !this.deps.driving()) return;
    for (const { from, body } of this.queue.splice(0)) {
      const ms = lineMs(body);
      this.deps.line(from, body, false, '', ms, () => this.write(from, true));
      this.store.read(from);
      this.newest = { name: from, at: now };
      this.deps.hint(touchFirst() ? null : ms);
    }
  }

  private key(e: KeyboardEvent) {
    if (!this.on || !this.session || e.metaKey || e.ctrlKey || e.altKey || e.code !== MSG_KEY) return;
    const el = e.target as HTMLElement | null;
    if (el?.closest?.('input, textarea, select, dialog[open]') || document.querySelector('dialog[open]')) return;
    if (this.mode || !this.deps.driving()) return;
    e.preventDefault(); e.stopImmediatePropagation();
    if (e.repeat) return;
    const n = this.newest;
    if (n && performance.now() - n.at < REPLY_MS) this.write(n.name, false);
    else this.pick(false);
  }

  /** quick chat's touch sheet: "Message a friend" */
  pickByTouch() { if (this.deps.driving()) this.pick(true); }

  /** whether the touch sheet offers it (quick chat's sheet asks) */
  get enabled() { return this.on; }

  private pick(touch: boolean) {
    this.touch = touch; this.mode = 'pick'; this.to = '';
    this.deps.hint(null);
    // where each friend plays now
    this.deps.send({ op: 'where' });
    this.draw();
    if (!touch) this.card.focus({ preventScroll: true });
  }

  private write(name: string, touch = this.touch) {
    if (!this.deps.driving()) return;
    this.touch = touch; this.mode = 'write'; this.to = name;
    this.deps.hint(null);
    this.draw();
    this.card.querySelector<HTMLInputElement>('input')?.focus({ preventScroll: true });
  }

  private close() {
    if (!this.mode) return;
    const had = this.card.contains(document.activeElement);
    this.mode = null; this.to = '';
    this.card.hidden = true;
    this.card.replaceChildren();
    if (had) this.deps.focusGame();
  }

  private cardKey(e: KeyboardEvent) {
    if (e.code === 'Escape') { e.preventDefault(); e.stopPropagation(); this.close(); return; }
    if (this.mode === 'pick' && !e.repeat) {
      const d = /^(?:Digit|Numpad)([1-9])$/.exec(e.code);
      const f = d ? this.sorted()[+d[1] - 1] : undefined;
      if (f) { e.preventDefault(); this.write(f.name, false); }
    } else if (this.mode === 'write' && e.code === 'Enter' && !e.isComposing) {
      e.preventDefault();
      this.sendNow();
    }
  }

  /** the friends in the order the card lists them: in free roam, in the Online lobby, then the others */
  private sorted(): Friend[] {
    const rank = (a: string) => a === 'roam' ? 0 : a === 'online' ? 1 : 2;
    return [...this.friends].sort((a, b) => rank(a.at) - rank(b.at) || a.name.localeCompare(b.name)).slice(0, PICK_MAX);
  }

  private sendNow() {
    const input = this.card.querySelector<HTMLInputElement>('input');
    const body = (input?.value ?? '').replace(/\s+/g, ' ').trim().slice(0, MSG_MAX);
    if (!body || !this.to) { this.close(); return; }
    const id = this.nextId++, at = Date.now();
    if (!this.deps.send({ op: 'msg', name: this.to, body, id })) { this.deps.toast(t('The message was not sent: try again in a moment')); return; }
    this.store.add(this.to, { dir: 'out', body, t: at, st: 'wait' }, false);
    this.store.read(this.to);
    this.waiting.set(id, { name: this.to, t: at, body, at: performance.now() });
    this.deps.log(`[msg] sending to ${this.to} (${body.length} characters)`);
    this.close();
  }

  private draw() {
    const c = this.card;
    this.deps.placeNow();
    c.dataset.mode = this.touch ? 'touch' : 'keys';
    const el = (tag: string, cls: string, text?: string) => {
      const e = document.createElement(tag); e.className = cls; if (text !== undefined) e.textContent = text; return e;
    };
    const kids: HTMLElement[] = [];
    if (this.mode === 'pick') {
      kids.push(el('div', 'qc-title', t('Message a friend')));
      const list = this.sorted();
      for (const [i, f] of list.entries()) {
        const row = el(this.touch ? 'button' : 'div', 'qc-row msg-friend');
        if (row instanceof HTMLButtonElement) row.type = 'button';
        row.dataset.friend = f.name;
        if (!this.touch) row.append(el('kbd', '', String(i + 1)));
        const where = f.at === 'roam' ? t('free roam') : f.at === 'online' ? t('Online lobby') : t('offline');
        row.append(el('span', 'qc-text', f.name), el('span', 'msg-where' + (f.at ? ' ok' : ''), where));
        kids.push(row);
      }
      if (!list.length) kids.push(el('div', 'qc-wait', this.listed ? t('Touch front bumpers in free roam for 3 s to add a player.') : t('Wait a moment')));
      if (!this.touch) kids.push(el('div', 'qc-foot', t('Esc closes')));
    } else if (this.mode === 'write') {
      kids.push(el('div', 'qc-title', t('Message {name}', { name: this.to })));
      // what is answered: the friend's message, when it is the conversation's newest
      const last = this.store.messages(this.to).at(-1);
      if (last?.dir === 'in') {
        const q = el('div', 'msg-quote'), b = el('b', '', this.to);
        q.append(b, document.createTextNode(` ${last.body}`));
        kids.push(q);
      }
      const input = document.createElement('input');
      input.type = 'text'; input.className = 'msg-input'; input.maxLength = MSG_MAX; input.autocomplete = 'off';
      input.enterKeyHint = 'send'; input.spellcheck = true;
      input.setAttribute('aria-label', t('Message {name}', { name: this.to }));
      const left = el('span', 'msg-left', String(MSG_MAX));
      input.addEventListener('input', () => { left.textContent = String(MSG_MAX - input.value.length); });
      kids.push(input);
      const foot = el('div', 'qc-foot msg-foot');
      if (this.touch) {
        const send = el('button', 'qc-say msg-send', t('Send')) as HTMLButtonElement;
        send.type = 'button';
        send.addEventListener('pointerdown', e => e.preventDefault());    // the field keeps the focus (and the keyboard)
        send.addEventListener('click', () => this.sendNow());
        foot.append(send, left);
      } else {
        foot.append(el('span', '', t('Enter sends · Esc closes')), left);
      }
      kids.push(foot);
    }
    c.replaceChildren(...kids);
    c.hidden = !this.mode;
  }

  /** for the page's test hooks */
  state() {
    return { on: this.on, mode: this.mode, to: this.to, friends: this.friends.map(f => `${f.name}:${f.at}`), queue: this.queue.length, waiting: this.waiting.size };
  }
}

/** how long a message's line stays in the log (ms) */
function lineMs(body: string): number { return Math.min(LINE_MS + body.length * LINE_MS_PER_CHAR, LINE_MS_MAX); }

export interface ChatDialogDeps {
  /** a message to friend `name` (FriendsApi.send): the server's "done" (null: no answer) */
  send(name: string, body: string): Promise<string | null>;
  log(line: string): void;
}

/** The start page's conversation with one friend (index.html #chat-pop): the messages kept here, a field, Send. */
export class ChatDialog {
  private name = '';
  private d: HTMLDialogElement;

  constructor(private store: MessageStore, private deps: ChatDialogDeps) {
    this.d = document.querySelector<HTMLDialogElement>('#chat-pop')!;
    const input = this.input();
    input.maxLength = MSG_MAX;
    input.addEventListener('input', () => this.count());
    input.addEventListener('keydown', e => { if (e.key === 'Enter' && !e.isComposing) { e.preventDefault(); void this.send(); } });
    this.d.querySelector<HTMLButtonElement>('#chat-send')!.addEventListener('click', () => void this.send());
    store.onChange(() => { if (this.d.open) { this.store.read(this.name); this.render(); } });
  }

  private input() { return this.d.querySelector<HTMLInputElement>('#chat-text')!; }

  open(name: string) {
    this.name = name;
    this.d.querySelector('#chat-pop-title')!.textContent = name;
    this.d.querySelector('#chat-note')!.textContent = '';
    this.input().value = '';
    this.count();
    this.store.read(name);
    this.render();
    this.d.showModal();
    this.input().focus();
  }

  private count() {
    this.d.querySelector('#chat-left')!.textContent = String(MSG_MAX - this.input().value.length);
  }

  private render() {
    const box = this.d.querySelector<HTMLElement>('#chat-log')!, msgs = this.store.messages(this.name);
    const fmt = new Intl.DateTimeFormat(undefined, { weekday: 'short', hour: '2-digit', minute: '2-digit' });
    const rows: HTMLElement[] = msgs.map(m => {
      const r = document.createElement('div');
      r.className = `chat-msg ${m.dir}`;
      const head = document.createElement('div');
      head.className = 'chat-head';
      const who = document.createElement('b');
      who.textContent = m.dir === 'in' ? this.name : t('You');
      const when = document.createElement('span');
      const st = m.st === 'wait' ? t('sending') : m.st === 'kept' ? t('gets it when back') : m.st === 'failed' ? t('not sent') : '';
      when.textContent = st ? `${fmt.format(m.t)} · ${st}` : fmt.format(m.t);
      head.append(who, when);
      const body = document.createElement('p');
      body.textContent = m.body;
      r.append(head, body);
      return r;
    });
    if (!rows.length) {
      const p = document.createElement('p');
      p.className = 'hint';
      p.textContent = t('No messages yet. Messages wait up to 7 days for a friend who is away.');
      rows.push(p);
    }
    box.replaceChildren(...rows);
    box.scrollTop = box.scrollHeight;
  }

  private async send() {
    const input = this.input(), name = this.name;
    const body = input.value.replace(/\s+/g, ' ').trim().slice(0, MSG_MAX);
    if (!body) return;
    const at = Date.now();
    input.value = '';
    this.count();
    this.store.add(name, { dir: 'out', body, t: at, st: 'wait' }, false);
    let done: string | null = null;
    try { done = await this.deps.send(name, body); } catch (e) { this.deps.log(`[msg] to ${name} failed: ${e}`); }
    this.deps.log(`[msg] to ${name} from the start page: ${done ?? 'no answer'}`);
    const ok = done === 'sent' || done === 'kept';
    this.store.mark(name, at, ok ? done as 'sent' | 'kept' : 'failed');
    const why = done === null ? t('The message was not sent: try again in a moment') : sendTrouble(done, name);
    this.d.querySelector('#chat-note')!.textContent = why ?? '';
  }
}
