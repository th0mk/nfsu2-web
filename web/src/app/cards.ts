// The page's cards over the game in one look (style.css .ov-*: the race lobby's, quick chat's picker, a message to a
// friend): blocks a line apart (a title, what it is about, the keys, one hint), a key cap and what it does.

export const el = <K extends keyof HTMLElementTagNameMap>(tag: K, cls?: string, text?: string) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
};

/** a block of a card (a line above all but the first) */
export const block = (...kids: HTMLElement[]) => { const b = el('div', 'ov-block'); b.append(...kids); return b; };

/** a key cap (null: none) and what it does; with f a button too (a click or a tap does it, not the game's key) */
export function act(key: string | null, what: string | Node, f?: () => void): HTMLElement {
  const d = el('div', f ? 'ov-act tap' : 'ov-act');
  if (key) d.append(el('kbd', '', key));
  d.append(typeof what === 'string' ? el('span', '', what) : what);
  if (f) {
    d.onpointerdown = e => e.preventDefault();   // the canvas keeps the focus (and with it the keyboard)
    d.onclick = () => f();
  }
  return d;
}

/** a line's text with a player's name in it bold (lime: the names in the log and the notes) */
export function named(s: string, name: string): HTMLSpanElement {
  const span = el('span'), at = name ? s.indexOf(name) : -1;
  if (at < 0) { span.textContent = s; return span; }
  span.append(s.slice(0, at), el('b', '', name), s.slice(at + name.length));
  return span;
}

