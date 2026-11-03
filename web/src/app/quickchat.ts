// Quick chat in free roam and in races between players: set phrases the players say to each other.  Nobody types
// anything, so there is nothing to moderate, and each player reads a phrase in their own language.  Each player picks
// sixteen of the library's (phrases.ts) on the start page, in four groups of four (settings.ini quick_chat_phrases,
// quick_chat_groups).
//   pick   keys 1-4 open a group and 1-4 again says its phrase; on a controller the right stick pressed in shows the
//          four groups, then a flick (up, right, down or left) opens one and another says its phrase (the stick
//          itself looks round the car while driving, runtime/look.c: the game gets it at rest while the picker has
//          it); on a touch screen the chat button shows all sixteen and a tap says one.  Open while driving in free
//          roam with free roam on, and in a race between players (Online, LAN, a free-roam lobby's) while driving and
//          on its results (the game's status "netrace"; the results' menus take none of 1-4 either), free roam on or
//          off; never in a race of the career, nor in the career's free roam with free roam off (1-4, Esc and the
//          right stick's click stay the game's there: the click is a car's hydraulics).  Esc, the stick's click
//          again, PICK_MS without a pick, or leaving those closes it.  At most BURST phrases in WINDOW_MS (the game
//          keeps the same limit): then the picker asks to wait.
//   wire   a phrase is its number in the library.  In free roam the game sends it to the players around
//          (runtime/roam_game.c say_*: within 250 m, or in our race lobby); in a race to the race's players
//          (runtime/racehorn.c qc_*: the page puts it in the LAN mailbox).  Either way back to this page as said by the
//          player.  A number this page has no phrase for (a newer page's) is not shown, nor what a player this one
//          blocks (settings.ini roam_block) said.
//   show   in free roam, a line over the car of each player who said something, where the game draws the name over it:
//          the game tells where in each frame it draws (marks(), the renderer's GwMarks), so the line moves with the
//          car in the same picture; the player's own over their car.  In free roam and in races the chat log on the
//          left, in the notices' place, and in free roam beside the player's name in a race lobby's card.
//   count  the countdown (a start for the players lined up, 2026-11-03): a phrase of the library (phrases.ts
//          COUNTDOWN, marked as special), put in a slot as any.  Said in free roam (in a race it does nothing), then
//          COUNT_S seconds and GO!: big in the picture's middle for the one who said it and the players within NEAR_M
//          of them (the game's "d"), over their car for the others.  While one is on and GAP_MS after it, another
//          player's is not shown, and the player's own waits (one over a car far away holds back only far ones).  At a car meet only a player who has stayed there starts
//          one (roam_game.c say_stay, SAY_MEET_N): the game drops the others' and refuses the player's own while its
//          status "nocount" says so; the picker dims it then and, picked, says only that it is not allowed here.
import type { GameSession } from './game';
import { t } from './i18n';
import { catName, COUNTDOWN, DEFAULT_GROUPS, DEFAULT_PHRASES, PHRASES, type CatId } from './phrases';

/** the picker closes this long after it opened or the last pick in it (ms) */
const PICK_MS = 4000;
/** at most BURST phrases in WINDOW_MS (roam_game.c SAY_BURST, SAY_WINDOW_US) */
const BURST = 3, WINDOW_MS = 6000;
/** a line over a car: this long (ms; roam_game.c SAY_SHOW_US), the last FADE_MS of it fading */
const SHOW_MS = 5000, FADE_MS = 600;
/** the name over a friend's car is about this tall (HUD units at scale 1): the line goes above it */
const NAME_H = 13;
/** a line shrinks with the distance as the name under it does (to 0.5), but not below this: it stays readable */
const LINE_MIN = 0.7;
/** the right stick: a flick is past FLICK of its travel, and counts again once back within REST */
const FLICK = 0.8, REST = 0.35;
/** the stick's flicks and the keys' places, in the order of a group's phrases: up, right, down, left */
const ARROWS = ['↑', '→', '↓', '←'];
/** a speech balloon: what marks a line as chat */
export const ICON = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M3 2h10a2 2 0 0 1 2 2v6a2 2 0 0 1-2 2H8l-3.5 2.6V12H3a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2z"/></svg>';
/** a stopwatch: what marks the countdown, a special phrase (in the picker, and its line over the car) */
const WATCH = '<svg class="qc-watch" viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="9.2" r="5.3"/><path d="M6.2 1.6h3.6M8 1.6v2.3M8 9.2V6.4M12.3 4.4l1-1"/></svg>';

/** the countdown: COUNT_S seconds, then GO! for GO_MS; big for the players within NEAR_M (m) of the one who said it;
 *  after it, GAP_MS before another (10 s until 2026-11-03: players started one after another at a car meet) */
const COUNT_S = 5, GO_MS = 1000, NEAR_M = 50, GAP_MS = 30000;

/** what the game sends (roam_game.c say_page; racehorn.c qc_page, with race); d: a friend's distance (m); no: the
 *  player's own countdown not started (not theirs to start here) */
interface Said { id: number; name: string; p: number; me?: number; lobby?: number; race?: number; d?: number; no?: number }

export interface QuickChatDeps {
  /** a line in the chat log (the notices' place): the name, what was said, whether it is the player's own */
  line(name: string, text: string, me: boolean): void;
  /** the race lobby's card: what a player said, beside their name */
  lobby(name: string, text: string): void;
  /** the picker's place (the notices', placed again for the picture's size now: place()) */
  place(): void;
  /** messages between friends (messages.ts): whether the touch sheet offers "Message a friend", and opening it */
  message?: { on(): boolean; open(): void };
  log(s: string): void;
}

type Mode = 'keys' | 'pad' | 'touch';
/** the picker's open: all sixteen (touch), the four groups (the controller's click) */
const ALL = 4, GROUPS = 5;
const KEYS: Record<string, number> = { Digit1: 0, Digit2: 1, Digit3: 2, Digit4: 3, Numpad1: 0, Numpad2: 1, Numpad3: 2, Numpad4: 3 };
/** a player's name as a block compares it: trimmed, capitals not counting (the server's: roam hub.rs) */
const nameKey = (n: string) => n.trim().toLowerCase();

export class QuickChat {
  private session: GameSession | null = null;
  private on = false;
  /** free roam with others on (settings.ini roam): else free roam is the career's alone, with nobody to say it to */
  private roam = false;
  private layer: HTMLElement;
  private picker: HTMLElement;
  /** the lines over cars by the player's id (until: performance.now() it goes; out: fading) */
  private lines = new Map<number, { el: HTMLElement; until: number; me: boolean; shown: boolean; out: boolean }>();
  /** the countdown on now: who said it, since when (performance.now()), its big number (near) or the text of its
   *  line over the car, the step shown (0..COUNT_S-1 the numbers, COUNT_S GO!) */
  private count: { id: number; at: number; big: HTMLElement | null; text: HTMLElement | null; step: number } | null = null;
  private countTimer = 0;
  /** no countdown of another player shown, nor the player's own said, before this (performance.now()): after one
   *  big (the player's own, or one within NEAR_M) */
  private countFree = 0;
  /** no countdown of a player past NEAR_M shown before this: after one over a car (one far away holds back no near
   *  one, nor the player's own: a player just outside a car meet could keep its start waiting) */
  private farFree = 0;
  /** the picker: closed (-1), a group open (0-3), all of them (ALL, touch), the groups to flick to (GROUPS, the
   *  controller) */
  private open = -1;
  private mode: Mode = 'keys';
  private closeTimer = 0;
  private sent: number[] = [];
  private drive = 0;
  /** the game's status "netrace" (runtime/racehorn.c qc_status): 1 driving in a race between players, 2 on its
   *  results, 0 not (also a game without it: no picker in races) */
  private netrace = 0;
  /** the game's status "nocount" (runtime/web/status_web.c): 1 while the player's countdown would not start here */
  private nocount = 0;
  private driveAt = 0;
  private armed = true;
  /** the player's sixteen: the phrase in each slot (group * 4 + place), and each group's name */
  private layout: readonly number[] = DEFAULT_PHRASES;
  private groups: readonly CatId[] = DEFAULT_GROUPS;
  /** the names of the players this one blocks (settings.ini roam_block), as the server compares them: their phrases
   *  are not shown (the server keeps them out of free roam; a race's go round it) */
  private blocked = new Set<string>();

  constructor(stage: HTMLElement, private deps: QuickChatDeps) {
    this.layer = document.createElement('div');
    this.layer.className = 'qc-lines';
    this.picker = document.createElement('div');
    this.picker.className = 'qc-picker';
    this.picker.hidden = true;
    this.picker.dataset.nfsu2Ui = '';
    this.picker.addEventListener('click', e => {
      const b = (e.target as HTMLElement).closest<HTMLElement>('[data-slot]');
      if (b) this.pick(+b.dataset.slot!);
    });
    this.picker.addEventListener('pointerdown', e => e.preventDefault());    // the canvas keeps the focus (and the keys)
    stage.append(this.layer, this.picker);
    // caught before the game's input (attached later, at each Play): the keys the picker uses go to it alone
    window.addEventListener('keydown', e => this.key(e), { capture: true });
    stage.addEventListener('pointerdown', e => {
      if (this.open === ALL && !this.picker.contains(e.target as Node) && !(e.target as Element).closest?.('[data-ctl="chat"]')) this.close();
    }, { capture: true });
  }

  /** a game started: quick chat on (the setting) or off, free roam with others on or off, with the player's sixteen
   *  (phrases.ts phrasesOf, groupsOf), and the names the player blocks (settings.ts blockList) */
  start(s: GameSession, on: boolean, roam: boolean, layout: readonly number[], groups: readonly CatId[], blocked: readonly string[] = []) {
    this.session = s; this.on = on; this.roam = roam; this.drive = 0; this.netrace = 0; this.nocount = 0; this.driveAt = 0;
    this.layout = layout; this.groups = groups;
    this.blocked = new Set(blocked.map(nameKey));
  }
  stop() {
    this.session = null; this.on = false;
    this.close();
    this.endCount();
    this.countFree = 0; this.farFree = 0;
    for (const l of this.lines.values()) l.el.remove();
    this.lines.clear();
  }

  /** the picker's place: the notices' (shell.ts placeNotices), u: CSS px per HUD unit */
  place(left: number, top: number, u: number) {
    const s = this.picker.style;
    s.left = `${left}px`; s.top = `${top}px`;
    s.setProperty('--u', `${u}px`);
    this.layer.style.setProperty('--u', `${u}px`);
  }

  /** driving in free roam with others (the game's status: runtime/web/status_web.c "drive" 2, not in a cutscene; it
   *  says 2 in the career's free roam with free roam off too); messages.ts asks too */
  driving(): boolean {
    if (!this.session || !this.on || !this.roam) return false;
    this.readStatus();
    return (this.drive & 3) === 2 && (this.drive & 8) === 0;
  }
  /** in a race between players, driving or on its results (racehorn.c's "netrace"): the picker opens there too */
  private racing(): boolean {
    if (!this.session || !this.on) return false;
    this.readStatus();
    return this.netrace > 0;
  }
  /** the picker may open: driving in free roam with others, or in a race between players */
  private can(): boolean { return this.driving() || this.racing(); }
  private readStatus() {
    const s = this.session, now = performance.now();
    if (!s || now - this.driveAt <= 100) return;
    this.driveAt = now;
    const st = s.status?.read();
    if (st) { this.drive = st.get('drive') ?? 0; this.netrace = st.get('netrace') ?? 0; this.nocount = st.get('nocount') ?? 0; }
  }
  /** a countdown of the player's would start now: in free roam (not in a race), and not at a car meet where the
   *  player has not stayed */
  private countOk(): boolean { return this.driving() && !this.nocount; }

  private key(e: KeyboardEvent) {
    if (!this.session || e.metaKey || e.ctrlKey || e.altKey) return;
    const el = e.target as HTMLElement | null;
    // (the page's own things over the game take their keys: messages.ts's card picks a friend with 1-9)
    if (el?.closest?.('input, textarea, select, dialog[open], [data-nfsu2-ui]') || document.querySelector('dialog[open]')) return;
    const k = KEYS[e.code];
    if (k === undefined && !(e.code === 'Escape' && this.open >= 0)) return;
    if (!this.can()) { if (this.open >= 0) this.close(); return; }
    // the game binds none of 1-4 (its driving key maps: dinput_kbd.c DIMAP=1; nor do its menus, the results': the
    // front end's key table), and gets them all the same, so a control the player put on one still works; Esc closes
    // the picker instead of pausing the game (or leaving the results)
    if (e.code === 'Escape') { e.preventDefault(); e.stopImmediatePropagation(); }
    if (e.repeat) return;
    if (e.code === 'Escape') this.close();
    else if (this.open < 0 || this.open >= ALL || this.mode !== 'keys') this.group(k, 'keys');
    else this.pick(this.open * 4 + k);
  }

  /** the right stick, every time the controller is read (gamepad.ts PadPage.stick); true: the picker has it (opened
   *  by the stick's click), else the game looks round with it */
  stick(x: number, y: number): boolean {
    const r = Math.hypot(x, y), mine = this.open >= 0 && this.mode === 'pad';
    if (r < REST) { this.armed = true; return mine; }
    if (!mine) return false;
    if (r < FLICK || !this.armed) return true;
    this.armed = false;
    if (!this.can()) { this.close(); return false; }
    const dir = Math.abs(x) > Math.abs(y) ? (x > 0 ? 1 : 3) : (y < 0 ? 0 : 2);
    if (this.open === GROUPS) this.group(dir, 'pad');
    else this.pick(this.open * 4 + dir);
    return true;
  }

  /** the right stick pressed in (gamepad.ts PadPage.click): the four groups, or the picker closed again; true: the
   *  picker took it (where it can open), else it is the game's */
  click(): boolean {
    if (this.open >= 0) { this.close(); return true; }
    if (!this.can()) return false;
    this.armed = false;                              // (a flick counts once the stick was back at rest)
    this.group(GROUPS, 'pad');
    return true;
  }

  /** the touch controls' chat button: all sixteen, or closed again */
  toggle() {
    if (this.open === ALL) { this.close(); return; }
    if (!this.can()) return;
    this.mode = 'touch'; this.open = ALL;
    this.draw();
  }

  private group(g: number, mode: Mode) {
    this.mode = mode; this.open = g;
    this.draw();
  }

  private close() {
    clearTimeout(this.closeTimer);
    this.open = -1;
    this.picker.hidden = true;
    this.picker.replaceChildren();
  }

  /** at most BURST in WINDOW_MS: the time until the next may go (0: now) */
  private wait(now: number): number {
    this.sent = this.sent.filter(at => now - at < WINDOW_MS);
    return this.sent.length < BURST ? 0 : this.sent[0] + WINDOW_MS - now;
  }

  /** the slot picked (group * 4 + place): says the phrase the player put there */
  private pick(slot: number) {
    const s = this.session, now = performance.now(), p = this.layout[slot];
    if (!s || p === undefined || !PHRASES[p]) return;
    // (in free roam the free-roam mailbox's game says it, in a race the LAN mailbox's; the countdown: free roam's only,
    // and not while one is on near)
    const race = !this.driving() && this.racing();
    if (p === COUNTDOWN && race) { this.close(); return; }
    if (p === COUNTDOWN && !this.countOk()) { this.refuse(); return; }
    const hold = Math.max(this.wait(now), p === COUNTDOWN ? this.countFree - now : 0);
    if (hold > 0) { this.draw(hold); return; }
    if (!s.net?.say?.(p, race)) { this.close(); return; }
    this.sent.push(now);
    this.deps.log(`[chat] said ${p} (${PHRASES[p].text})${race ? ' in the race' : ''}`);
    this.close();
  }

  /** a slot's phrase in the page's language */
  private text(slot: number): string { return t(PHRASES[this.layout[slot]]?.text ?? ''); }
  /** a slot's phrase into el: its text, the countdown with its stopwatch (dimmed where it would not start: in a race,
   *  where it does nothing, and at a car meet where the player has not stayed) */
  private slotText(el: HTMLElement, slot: number) {
    el.textContent = this.text(slot);
    if (this.layout[slot] !== COUNTDOWN) return;
    el.insertAdjacentHTML('afterbegin', WATCH);
    if (!this.countOk()) el.classList.add('qc-off');
  }

  /** the player's countdown not started here: the picker says so (not why: who stayed how long stays the game's) */
  private refuse() {
    this.deps.log('[chat] the countdown: not allowed here');
    this.draw(0, t('You are not allowed to start a countdown here'));
  }

  private draw(hold = 0, no = '') {
    clearTimeout(this.closeTimer);
    const p = this.picker, wait = no ? 0 : Math.max(hold, this.wait(performance.now()));
    this.deps.place();
    p.dataset.mode = this.mode;
    p.hidden = false;
    const kids: HTMLElement[] = [];
    const el = (tag: string, cls: string, text?: string) => {
      const e = document.createElement(tag); e.className = cls; if (text !== undefined) e.textContent = text; return e;
    };
    if (no) {
      kids.push(el('div', 'qc-title', t('Quick chat')), el('div', 'qc-wait qc-no', no));
      this.closeTimer = window.setTimeout(() => this.close(), PICK_MS);
    } else if (wait > 0) {
      kids.push(el('div', 'qc-title', t('Quick chat')), el('div', 'qc-wait', t('Wait a moment')));
      this.closeTimer = window.setTimeout(() => this.close(), Math.min(wait, PICK_MS));
    } else if (this.open === GROUPS) {
      kids.push(el('div', 'qc-title', t('Quick chat')));
      for (let g = 0; g < 4; g++) {
        const row = el('div', 'qc-row');
        row.append(el('kbd', '', ARROWS[g]), el('span', 'qc-text', t(catName(this.groups[g]))));
        kids.push(row);
      }
      this.closeTimer = window.setTimeout(() => this.close(), PICK_MS);
    } else if (this.open === ALL) {
      for (let g = 0; g < 4; g++) {
        const col = el('div', 'qc-group');
        col.append(el('div', 'qc-title', t(catName(this.groups[g]))));
        for (let i = 0; i < 4; i++) {
          const b = el('button', 'qc-say') as HTMLButtonElement;
          this.slotText(b, g * 4 + i);
          b.type = 'button'; b.dataset.slot = String(g * 4 + i);
          col.append(b);
        }
        kids.push(col);
      }
      const m = this.deps.message;
      if (m?.on() && this.driving()) {               // (messages go in free roam: messages.ts)
        const b = el('button', 'qc-say qc-msg', t('Message a friend')) as HTMLButtonElement;
        b.type = 'button';
        b.addEventListener('click', () => { this.close(); m.open(); });
        kids.push(b);
      }
    } else {
      kids.push(el('div', 'qc-title', t(catName(this.groups[this.open]))));
      for (let i = 0; i < 4; i++) {
        const row = el('div', 'qc-row');
        row.dataset.slot = String(this.open * 4 + i);
        const tx = el('span', 'qc-text');
        this.slotText(tx, this.open * 4 + i);
        row.append(el('kbd', '', this.mode === 'pad' ? ARROWS[i] : String(i + 1)), tx);
        kids.push(row);
      }
      if (this.mode === 'keys') kids.push(el('div', 'qc-foot', t('Esc closes')));
      this.closeTimer = window.setTimeout(() => this.close(), PICK_MS);
    }
    p.replaceChildren(...kids);
  }

  /** a phrase said (roam_game.c say_page, racehorn.c qc_page: NET_KIND_SAY): its line over the car (free roam), the
   *  log, the lobby's card (free roam) */
  said(json: string) {
    let s: Said;
    try { s = JSON.parse(json) as Said; } catch { this.deps.log(`[chat] not JSON: ${json.slice(0, 80)}`); return; }
    if (!this.on || typeof s.id !== 'number' || typeof s.p !== 'number') return;
    if (s.p === COUNTDOWN && s.me && s.no) { this.refuse(); return; }      // (the game's: picked as the status changed)
    if (s.p === COUNTDOWN) { if (!s.race) this.counted(s); return; }       // (said in free roam only)
    if (!(s.p >= 0 && s.p < PHRASES.length)) return;
    const text = t(PHRASES[s.p].text), me = !!s.me, name = String(s.name ?? ''), where = s.race ? ', in the race' : s.lobby ? ', in our lobby' : '';
    if (!me && this.blocked.has(nameKey(name))) { this.deps.log(`[chat] ${name} said ${s.p}${where}: not shown (blocked)`); return; }
    this.deps.log(`[chat] ${me ? 'you' : name} said ${s.p} (${PHRASES[s.p].text})${where}`);
    // a race: the log only (the game tells nobody where its racers' names are drawn)
    if (s.race) { this.deps.line(name, text, me); return; }
    this.overCar(s.id, text, me, SHOW_MS);
    this.deps.line(name, text, me);
    this.deps.lobby(name, text);
  }

  /** a line over the car of player id for ms (in place of the one there), the countdown's with its stopwatch: its
   *  text */
  private overCar(id: number, text: string, me: boolean, ms: number, icon = ICON): HTMLElement {
    this.lines.get(id)?.el.remove();
    // the place on .qc-line (every frame), the look and its pop on .qc-b
    const el = document.createElement('div'), b = document.createElement('div');
    const span = Object.assign(document.createElement('span'), { textContent: text });
    el.className = 'qc-line' + (me ? ' me' : '');
    b.className = 'qc-b';
    b.innerHTML = icon;
    b.append(span);
    el.append(b);
    el.hidden = true;
    this.layer.append(el);
    this.lines.set(id, { el, until: performance.now() + ms, me, shown: false, out: false });
    return span;
  }

  /** a countdown said (the player's own, or a friend's): big for the one who said it and the players within NEAR_M,
   *  else over the car; not while another is on, nor GAP_MS after it (a far one: after a far one only) */
  private counted(s: Said) {
    const me = !!s.me, name = String(s.name ?? ''), now = performance.now();
    if (!me && this.blocked.has(nameKey(name))) { this.deps.log(`[chat] ${name}'s countdown: not shown (blocked)`); return; }
    const near = me || (typeof s.d === 'number' && s.d <= NEAR_M);
    if (!me && now < (near ? this.countFree : Math.max(this.countFree, this.farFree))) {
      this.deps.log(`[chat] ${name}'s countdown: not shown (one was on)`); return; }
    const was = this.count;                                // (a far one's number over its car: gone with it)
    if (was?.text) { this.lines.get(was.id)?.el.remove(); this.lines.delete(was.id); }
    this.endCount();
    if (near) this.countFree = now + COUNT_S * 1000 + GO_MS + GAP_MS;
    else this.farFree = now + COUNT_S * 1000 + GO_MS + GAP_MS;
    this.deps.log(`[chat] ${me ? 'you' : name} started the countdown${me ? '' : ` (${s.d ?? '?'} m${near ? '' : ': over the car'})`}`);
    let big: HTMLElement | null = null, text: HTMLElement | null = null;
    if (near) {
      big = document.createElement('div');
      big.className = 'qc-count';
      this.layer.append(big);
    } else text = this.overCar(s.id, String(COUNT_S), me, COUNT_S * 1000 + GO_MS, WATCH);
    this.count = { id: s.id, at: now, big, text, step: -1 };
    this.countTick();
    this.countTimer = window.setInterval(() => this.countTick(), 100);
    const said = t('Countdown');
    this.deps.line(name, said, me);
    this.deps.lobby(name, said);
  }

  /** the countdown's number now (its pop at each), GO!, and its end */
  private countTick() {
    const c = this.count;
    if (!c) return;
    const ms = performance.now() - c.at, step = Math.min(Math.floor(ms / 1000), COUNT_S);
    if (ms >= COUNT_S * 1000 + GO_MS) { this.endCount(); return; }
    if (c.big) {
      if (step !== c.step) {
        const n = document.createElement('span');
        n.textContent = step < COUNT_S ? String(COUNT_S - step) : t('GO!');
        c.big.replaceChildren(n);
      }
      if (!c.big.classList.contains('out') && ms >= COUNT_S * 1000 + GO_MS - FADE_MS) c.big.classList.add('out');
      this.placeCount();
    } else if (c.text && step !== c.step) c.text.textContent = step < COUNT_S ? String(COUNT_S - step) : t('GO!');
    c.step = step;
  }

  /** the big number: in the picture's middle across, a quarter down (over the road ahead, above the cars) */
  private placeCount() {
    const b = this.count?.big, v = this.session?.viewRect();
    if (!b || !v || !(v.w > 0 && v.h > 0)) return;
    b.style.left = `${(v.x + v.w / 2).toFixed(1)}px`;
    b.style.top = `${(v.y + v.h * 0.24).toFixed(1)}px`;
  }

  private endCount() {
    clearInterval(this.countTimer);
    this.count?.big?.remove();
    this.count = null;
  }

  /** every frame drawn (the renderer's onMarks): the lines over the cars where this frame has their names */
  marks(n: number, m: Float32Array, ids: Uint32Array) {
    if (this.count?.big) this.placeCount();
    if (!this.lines.size) return;
    const s = this.session, now = performance.now();
    const v = s?.viewRect();
    for (const [id, l] of this.lines) {
      if (now >= l.until) { l.el.remove(); this.lines.delete(id); continue; }
      let i = 0;
      while (i < n && ids[4 * i] !== id) i++;
      const show = i < n && !!v && v.w > 0 && v.h > 0;
      if (show) {
        // its bottom middle on the name's top (the game's point is the name's middle), scaled with the name
        const u = Math.min(v.w / 640, v.h / 480), sc = m[4 * i + 3], ls = Math.max(sc, LINE_MIN);
        const x = v.x + m[4 * i + 1] * v.w, y = v.y + m[4 * i + 2] * v.h - (l.me ? 0 : NAME_H / 2 * u * sc);
        l.el.style.transform = `translate(${x.toFixed(1)}px, ${y.toFixed(1)}px) scale(${ls.toFixed(3)}) translate(-50%, -100%)`;
      }
      if (show !== l.shown) { l.el.hidden = !show; l.shown = show; }
      if (!l.out && now >= l.until - FADE_MS) { l.out = true; l.el.classList.add('out'); }
    }
  }

  /** quick chat is on in this game (its setting): the touch controls' chat button shows (touch.ts) */
  get enabled() { return this.on; }

  /** for the page's test hooks */
  state() {
    const c = this.count;
    return { on: this.on, open: this.open, mode: this.mode, lines: [...this.lines.keys()], sent: this.sent.length, netrace: this.netrace,
      count: c ? { id: c.id, big: !!c.big, text: (c.big ?? c.text)?.textContent ?? '' } : null };
  }
}
