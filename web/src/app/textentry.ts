// Typing on a phone or tablet: the game's text entry (a new profile's name, the LAN name) takes characters from a
// keyboard, and a phone's keyboard only comes up for a text field of the page.  So while the game's text entry is up
// (runtime/web/input_web.c) and a finger has touched the game (at once on a touch screen), a field of the page's own
// shows at the top of the game with the game's text in it, selected, and takes the focus: the phone's keyboard comes
// up (where the browser wants a tap for that, a tap on the field).  Each change of the field goes to the game as
// characters (WM_CHAR, input/index.ts text()): backspaces back to where the two texts part, then the rest.  The field
// keeps only what the game's takes (its kind's characters, its length), so the two stay the same; Enter (the
// keyboard's Done) accepts, as on a keyboard.  At the top, the keyboard coming up from below leaves it in sight.
// It goes when the game's text entry does, and when a key goes to the game instead (a keyboard), until the next touch.
import type { GameSession } from './game';
import type { TextEntry } from '../input/index';
import { cp1252 } from '../input/keymap';
import { touchFirst } from './device';
import { t } from './i18n';
import { TEXT_PUNCT as PUNCT } from '@game/run';

/** whether the game's text entry of this kind takes the character (UG2's 0x4FF310, Most Wanted's 0x56C820) */
export function takes(kind: number, ch: string): boolean {
  const c = ch.length === 1 ? cp1252(ch.charCodeAt(0)) : -1;
  if (c < 0x20 || c === 0x7F) return false;
  if (/^[A-Za-z0-9]$/.test(ch)) return true;
  if (kind === 4 && '@._'.includes(ch)) return true;
  if (kind === 5 && ' ,@.'.includes(ch)) return true;
  return (kind === 0 || kind === 3) && ((c >= 0xC0 && c <= 0xFF) || PUNCT.includes(ch));
}

/** what the game's text entry of this kind keeps of s: the characters it takes, max of them */
export function fit(s: string, kind: number, max: number): string {
  return [...s].filter(ch => takes(kind, ch)).slice(0, max).join('');
}

/** the characters (cp1252) that turn the game's text from into to: a backspace (8) for each character after the
 *  part they share, then the rest of to */
export function edits(from: string, to: string): number[] {
  let p = 0;
  while (p < from.length && p < to.length && from[p] === to[p]) p++;
  const out: number[] = Array(from.length - p).fill(8);
  for (const ch of to.slice(p)) out.push(cp1252(ch.charCodeAt(0)));
  return out;
}

export class TextField {
  private root: HTMLFormElement;
  private field: HTMLInputElement;
  private session: GameSession | null = null;
  private raf = 0;
  private entry: TextEntry | null = null;
  /** the game's text: as the field sent it, or as the game had it when the field took it */
  private sent = '';
  /** a finger touched the game; a key went to the game after the last touch */
  private seen = touchFirst();
  private away = false;
  private shown = false;

  constructor(stage: HTMLElement, private deps: { log(s: string): void }) {
    const root = this.root = document.createElement('form');
    root.className = 'type'; root.hidden = true; root.dataset.nfsu2Ui = '';
    const f = this.field = document.createElement('input');
    f.type = 'text'; f.autocomplete = 'off'; f.spellcheck = false; f.enterKeyHint = 'done';
    f.setAttribute('autocapitalize', 'off'); f.setAttribute('autocorrect', 'off');
    f.setAttribute('aria-label', t('Text for the game'));
    root.append(f);
    stage.append(root);
    f.addEventListener('input', () => this.onInput());
    f.addEventListener('focus', () => this.take());
    f.addEventListener('keydown', e => { if (e.key === 'Enter' && !e.isComposing) { e.preventDefault(); this.accept(); } });
    root.addEventListener('submit', e => { e.preventDefault(); this.accept(); });
    stage.addEventListener('pointerdown', e => {
      if (e.pointerType !== 'touch') return;
      this.seen = true; this.away = false; this.update();
    }, { capture: true });
    addEventListener('keydown', e => { if (e.isTrusted && this.session && e.target !== f) { this.away = true; this.update(); } }, { capture: true });
  }

  /** a game started: the field follows its text entry until stop() */
  start(s: GameSession) {
    this.session = s;
    const tick = () => { this.update(); this.raf = requestAnimationFrame(tick); };
    this.raf = requestAnimationFrame(tick);
  }

  /** the game ended */
  stop() {
    cancelAnimationFrame(this.raf);
    this.close();
    this.session = null; this.entry = null;
  }

  /** for tests and logs */
  state() { return { shown: this.shown, entry: this.entry, field: this.field.value, focused: document.activeElement === this.field }; }

  private update() {
    this.entry = this.session?.input?.textEntry?.() ?? null;
    const show = !!this.entry && this.seen && !this.away;
    if (show && !this.shown) this.open();
    else if (!show && this.shown) this.close();
    else if (show && document.activeElement !== this.field) this.take();     // the game's text, as a keyboard left it
  }

  private open() {
    this.shown = true; this.root.hidden = false;
    this.take();
    this.field.focus({ preventScroll: true });
    this.field.select();
    this.deps.log(`[app] the game asks for text (up to ${this.entry?.max} characters): the page's field for the phone's keyboard is up`);
  }

  private close() {
    if (document.activeElement === this.field) this.field.blur();
    this.shown = false; this.root.hidden = true;
  }

  /** the field starts from the game's text */
  private take() {
    const e = this.entry;
    if (!e) return;
    this.sent = e.text;
    if (this.field.value !== e.text) this.field.value = e.text;
    this.field.maxLength = e.max;
  }

  private onInput() {
    const e = this.entry;
    if (!e) return;
    const v = fit(this.field.value, e.kind, e.max);
    if (v !== this.field.value) this.field.value = v;
    const c = edits(this.sent, v);
    if (c.length) this.session?.input?.text?.(c);
    this.sent = v;
  }

  private accept() {
    this.onInput();
    this.session?.input?.text?.([13]);
  }
}
