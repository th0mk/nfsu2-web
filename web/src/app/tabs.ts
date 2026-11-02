// A panel's own tabs on the start page (index.html .tabs.big): the big panel's Saves, Online, Friends, Game, Controls
// and Other, and About's About, Credits and Translations.  The tabs are the panel's heading: words on a divider, the
// open one lime and underlined (the Controls sheet's tabs, controls.ts, at a heading's size).  Where they do not fit
// on one line (a narrow column, another language's longer words) they are the sheet's size (data-fit="small"), and
// where that does not fit either, or on a phone, in rows of three (data-fit="rows").  Every panel's tabs take the same
// step, so both panels' tabs look alike.
import { onLanguage } from './i18n';

/** the page's tab lists, sized together (fitAll) */
const lists: { list: HTMLElement; tabs: HTMLButtonElement[] }[] = [];
const STEPS = ['', 'small', 'rows'] as const;
const PHONE = '(max-width: 559px)';

/** wider than the line: the tabs' own widths and the gaps between them */
function over(l: typeof lists[number]): boolean {
  const room = l.list.clientWidth;
  if (!room) return false;
  const gap = parseFloat(getComputedStyle(l.list).columnGap) || 0;
  return l.tabs.reduce((w, b) => w + b.scrollWidth, 0) + gap * (l.tabs.length - 1) > room;
}

/** each list at the first step where every list fits on its line (rows on a phone) */
function fitAll() {
  const at = (i: number) => { for (const l of lists) { if (STEPS[i]) l.list.dataset.fit = STEPS[i]; else delete l.list.dataset.fit; } };
  let i = 0;
  if (matchMedia(PHONE).matches) i = STEPS.length - 1;
  else for (; i < STEPS.length - 1; i++) { at(i); if (!lists.some(over)) break; }
  at(i);
}
let fitting = false;

/** what panelTabs() gives: show(the tab, whether it takes the focus) and the open one */
export interface PanelTabs { show(tab: string, focus?: boolean): void; readonly tab: string }

/**
 * Wires the tabs of panel (its tab list and the panes after it, each with the data-tab of its tab).  store: where the
 * open tab is kept between visits (localStorage; none: the first tab each time).  hash: a page address ending in
 * #<a tab> (#friends) opens that tab, also later (an address with = in it is an invite link: invite.ts).
 */
export function panelTabs(panel: HTMLElement, opts: { store?: string; hash?: boolean } = {}): PanelTabs {
  const list = panel.querySelector<HTMLElement>(':scope > .tabs')!;
  const tabs = [...list.querySelectorAll<HTMLButtonElement>(':scope > [role="tab"]')];
  const panes = [...panel.querySelectorAll<HTMLElement>(':scope > [role="tabpanel"]')];
  const names = tabs.map(b => b.dataset.tab!);
  const prefix = panel.id || 'tabs';
  // (a pane keeps the id the page's code knows it by: #saves, #online, #friends)
  for (const p of panes) { p.id ||= `${prefix}-pane-${p.dataset.tab}`; p.setAttribute('aria-labelledby', `${prefix}-tab-${p.dataset.tab}`); }
  for (const b of tabs) { b.id = `${prefix}-tab-${b.dataset.tab}`; b.setAttribute('aria-controls', panes.find(p => p.dataset.tab === b.dataset.tab)!.id); }

  let tab = names[0];
  const ui: PanelTabs = {
    get tab() { return tab; },
    show(t, focus) {
      if (!names.includes(t)) return;
      tab = t;
      for (const b of tabs) { const on = b.dataset.tab === t; b.setAttribute('aria-selected', String(on)); b.tabIndex = on ? 0 : -1; if (on && focus) b.focus(); }
      for (const p of panes) p.hidden = p.dataset.tab !== t;
    },
  };
  const pick = (t: string, focus = false) => {
    ui.show(t, focus);
    if (opts.store) try { localStorage.setItem(opts.store, t); } catch { /* storage off: the first tab next time */ }
  };
  for (const b of tabs) b.onclick = () => pick(b.dataset.tab!);
  // the arrow keys between the tabs, Home and End to the first and last (a tab list's keys)
  list.onkeydown = e => {
    const i = names.indexOf(tab);
    const to = e.key === 'ArrowRight' ? (i + 1) % names.length : e.key === 'ArrowLeft' ? (i + names.length - 1) % names.length
      : e.key === 'Home' ? 0 : e.key === 'End' ? names.length - 1 : -1;
    if (to < 0) return;
    e.preventDefault();
    pick(names[to], true);
  };

  let first = names[0];
  if (opts.store) try { const v = localStorage.getItem(opts.store); if (v && names.includes(v)) first = v; } catch { /* storage off */ }
  const fromHash = () => { const h = location.hash.slice(1).toLowerCase(); return names.includes(h) ? h : null; };
  if (opts.hash) {
    first = fromHash() ?? first;
    window.addEventListener('hashchange', () => { const h = fromHash(); if (h) pick(h); });
  }
  ui.show(first);

  lists.push({ list, tabs });
  fitAll();
  if (!fitting) {
    fitting = true;
    window.addEventListener('resize', fitAll);
    onLanguage(fitAll);
    void document.fonts?.ready.then(fitAll);
  }
  return ui;
}
