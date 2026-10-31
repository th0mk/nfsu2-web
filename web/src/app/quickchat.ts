// Quick chat in free roam: set phrases the players say to each other.  Nobody types anything, so there is nothing to
// moderate, and each player reads a phrase in their own language.  Each player picks sixteen of the library's
// (phrases.ts) on the start page, in four groups of four (settings.ini quick_chat_phrases, quick_chat_groups).
//   pick   keys 1-4 open a group and 1-4 again says its phrase; a controller's right stick the same (a flick up, right,
//          down or left: the game leaves that stick unused); on a touch screen the chat button shows all sixteen and a
//          tap says one.  Esc, PICK_MS without a pick, or no longer driving in free roam closes it.  At most BURST
//          phrases in WINDOW_MS (runtime/roam_game.c keeps the same limit): then the picker asks to wait.
//   wire   a phrase is its number in the library: the game sends it to the players around (roam_game.c say_*: within
//          250 m, or in our race lobby) and back to this page as said by the player.  A number this page has no phrase
//          for (a newer page's) is not shown.
//   show   a line over the car of each player who said something, where the game draws the name over it: the game
//          tells where in each frame it draws (marks(), the renderer's GwMarks), so the line moves with the car in the
//          same picture; the player's own over their car.  Also the chat log on the left, in the notices' place, and
//          beside the player's name in a race lobby's card.
import type { GameSession } from './game';
import { t } from './i18n';
import { catName, DEFAULT_GROUPS, DEFAULT_PHRASES, PHRASES, type CatId } from './phrases';

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

/** what the game sends (roam_game.c say_page) */
interface Said { id: number; name: string; p: number; me?: number; lobby?: number }

export interface QuickChatDeps {
  /** a line in the chat log (the notices' place): the name, what was said, whether it is the player's own */
  line(name: string, text: string, me: boolean): void;
  /** the race lobby's card: what a player said, beside their name */
  lobby(name: string, text: string): void;
  /** the picker's place (the notices', placed again for the picture's size now: place()) */
  place(): void;
  log(s: string): void;
}

type Mode = 'keys' | 'pad' | 'touch';
const KEYS: Record<string, number> = { Digit1: 0, Digit2: 1, Digit3: 2, Digit4: 3, Numpad1: 0, Numpad2: 1, Numpad3: 2, Numpad4: 3 };

export class QuickChat {
  private session: GameSession | null = null;
  private on = false;
  private layer: HTMLElement;
  private picker: HTMLElement;
  /** the lines over cars by the player's id (until: performance.now() it goes; out: fading) */
  private lines = new Map<number, { el: HTMLElement; until: number; me: boolean; shown: boolean; out: boolean }>();
  /** the picker: closed (-1), a group open (0-3), all of them (4, touch) */
  private open = -1;
  private mode: Mode = 'keys';
  private closeTimer = 0;
  private sent: number[] = [];
  private drive = 0;
  private driveAt = 0;
  private armed = true;
  /** the player's sixteen: the phrase in each slot (group * 4 + place), and each group's name */
  private layout: readonly number[] = DEFAULT_PHRASES;
  private groups: readonly CatId[] = DEFAULT_GROUPS;

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
      if (this.open === 4 && !this.picker.contains(e.target as Node) && !(e.target as Element).closest?.('[data-ctl="chat"]')) this.close();
    }, { capture: true });
  }

  /** a game started: quick chat on (the setting) or off, with the player's sixteen (phrases.ts phrasesOf, groupsOf) */
  start(s: GameSession, on: boolean, layout: readonly number[], groups: readonly CatId[]) {
    this.session = s; this.on = on; this.drive = 0; this.driveAt = 0;
    this.layout = layout; this.groups = groups;
  }
  stop() {
    this.session = null; this.on = false;
    this.close();
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

  /** driving in free roam (the game's status: runtime/web/status_web.c "drive" 2, not in a cutscene) */
  private driving(): boolean {
    const s = this.session, now = performance.now();
    if (!s || !this.on) return false;
    if (now - this.driveAt > 100) {
      this.driveAt = now;
      const st = s.status?.read();
      if (st) this.drive = st.get('drive') ?? 0;
    }
    return (this.drive & 3) === 2 && (this.drive & 8) === 0;
  }

  private key(e: KeyboardEvent) {
    if (!this.session || e.metaKey || e.ctrlKey || e.altKey) return;
    const el = e.target as HTMLElement | null;
    if (el?.closest?.('input, textarea, select, dialog[open]') || document.querySelector('dialog[open]')) return;
    const k = KEYS[e.code];
    if (k === undefined && !(e.code === 'Escape' && this.open >= 0)) return;
    if (!this.driving()) { if (this.open >= 0) this.close(); return; }
    // the game binds none of 1-4 (its key maps: dinput_kbd.c DIMAP=1), and gets them all the same, so a control the
    // player put on one still works; Esc closes the picker instead of pausing the game
    if (e.code === 'Escape') { e.preventDefault(); e.stopImmediatePropagation(); }
    if (e.repeat) return;
    if (e.code === 'Escape') this.close();
    else if (this.open < 0 || this.open === 4 || this.mode !== 'keys') this.group(k, 'keys');
    else this.pick(this.open * 4 + k);
  }

  /** the right stick, every time the controller is read (gamepad.ts PadPage.stick); true: the picker has it */
  stick(x: number, y: number): boolean {
    const r = Math.hypot(x, y), mine = this.open >= 0 && this.mode === 'pad';
    if (r < REST) { this.armed = true; return mine; }
    if (r < FLICK || !this.armed) return mine;
    this.armed = false;
    if (!this.driving()) return false;
    const dir = Math.abs(x) > Math.abs(y) ? (x > 0 ? 1 : 3) : (y < 0 ? 0 : 2);
    if (!mine) this.group(dir, 'pad');
    else this.pick(this.open * 4 + dir);
    return true;
  }

  /** the touch controls' chat button: all sixteen, or closed again */
  toggle() {
    if (this.open === 4) { this.close(); return; }
    if (!this.driving()) return;
    this.mode = 'touch'; this.open = 4;
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
    if (this.wait(now) > 0) { this.draw(); return; }
    if (!s.net?.say?.(p)) { this.close(); return; }
    this.sent.push(now);
    this.deps.log(`[chat] said ${p} (${PHRASES[p].text})`);
    this.close();
  }

  /** a slot's phrase in the page's language */
  private text(slot: number): string { return t(PHRASES[this.layout[slot]]?.text ?? ''); }

  private draw() {
    clearTimeout(this.closeTimer);
    const p = this.picker, wait = this.wait(performance.now());
    this.deps.place();
    p.dataset.mode = this.mode;
    p.hidden = false;
    const kids: HTMLElement[] = [];
    const el = (tag: string, cls: string, text?: string) => {
      const e = document.createElement(tag); e.className = cls; if (text !== undefined) e.textContent = text; return e;
    };
    if (wait > 0) {
      kids.push(el('div', 'qc-title', t('Quick chat')), el('div', 'qc-wait', t('Wait a moment')));
      this.closeTimer = window.setTimeout(() => this.close(), Math.min(wait, PICK_MS));
    } else if (this.open === 4) {
      for (let g = 0; g < 4; g++) {
        const col = el('div', 'qc-group');
        col.append(el('div', 'qc-title', t(catName(this.groups[g]))));
        for (let i = 0; i < 4; i++) {
          const b = el('button', 'qc-say', this.text(g * 4 + i)) as HTMLButtonElement;
          b.type = 'button'; b.dataset.slot = String(g * 4 + i);
          col.append(b);
        }
        kids.push(col);
      }
    } else {
      kids.push(el('div', 'qc-title', t(catName(this.groups[this.open]))));
      for (let i = 0; i < 4; i++) {
        const row = el('div', 'qc-row');
        row.dataset.slot = String(this.open * 4 + i);
        row.append(el('kbd', '', this.mode === 'pad' ? ARROWS[i] : String(i + 1)), el('span', 'qc-text', this.text(this.open * 4 + i)));
        kids.push(row);
      }
      if (this.mode === 'keys') kids.push(el('div', 'qc-foot', t('Esc closes')));
      this.closeTimer = window.setTimeout(() => this.close(), PICK_MS);
    }
    p.replaceChildren(...kids);
  }

  /** a phrase said (roam_game.c say_page, NET_KIND_SAY): its line over the car, the log, the lobby's card */
  said(json: string) {
    let s: Said;
    try { s = JSON.parse(json) as Said; } catch { this.deps.log(`[chat] not JSON: ${json.slice(0, 80)}`); return; }
    if (!this.on || typeof s.id !== 'number' || typeof s.p !== 'number' || !(s.p >= 0 && s.p < PHRASES.length)) return;
    const text = t(PHRASES[s.p].text), me = !!s.me, name = String(s.name ?? '');
    this.deps.log(`[chat] ${me ? 'you' : name} said ${s.p} (${PHRASES[s.p].text})${s.lobby ? ', in our lobby' : ''}`);
    this.lines.get(s.id)?.el.remove();
    // the place on .qc-line (every frame), the look and its pop on .qc-b
    const el = document.createElement('div'), b = document.createElement('div');
    el.className = 'qc-line' + (me ? ' me' : '');
    b.className = 'qc-b';
    b.innerHTML = ICON;
    b.append(Object.assign(document.createElement('span'), { textContent: text }));
    el.append(b);
    el.hidden = true;
    this.layer.append(el);
    this.lines.set(s.id, { el, until: performance.now() + SHOW_MS, me, shown: false, out: false });
    this.deps.line(name, text, me);
    this.deps.lobby(name, text);
  }

  /** every frame drawn (the renderer's onMarks): the lines over the cars where this frame has their names */
  marks(n: number, m: Float32Array, ids: Uint32Array) {
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
  state() { return { on: this.on, open: this.open, mode: this.mode, lines: [...this.lines.keys()], sent: this.sent.length }; }
}
