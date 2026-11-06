// The Mods tab (index.html #mods): the catalogue of mods.ts in its sections, one row per mod (two that share a setting:
// one row), as the settings' rows look (.set); then our changes (mods.ts ADDED), one row per group.  A row with a
// setting is the settings panel's own (settings-ui.ts: its control, Reset, saving and read-only state as anywhere in the
// panel), drawn by the panel; the others are drawn here, a name and a help line only.  Every row's name has an (i) right
// after it, which opens the row's popup (#mod-info, as the Changelog's): a mod's makers, linked, what it does (English,
// as the changelog), short facts (where it is set among them) and its pages; a group's changes, a line each.  The fixed
// words follow the page's language.

import { ADDED, KINDS, LABELS, MODS, ONLINE, OURS, PLACES, USES, type Author, type Change, type Mod } from './mods';
import { listOf, onLanguage, t, tn } from './i18n';

const BY_ID = new Map(MODS.map(m => [m.id, m]));
const GROUPS = new Map(ADDED.map(g => [g.id, g]));

/** the entries of id's row: the entry, then those that share its row (mods.ts row) */
export function rowMods(id: string): Mod[] {
  const first = BY_ID.get(id);
  return first ? [first, ...MODS.filter(m => m.row === id)] : [];
}

/** a row's name: its mods' names ("Upscaled HUD and Detailed Map") */
export function rowName(id: string): string {
  return listOf(rowMods(id).map(m => m.name));
}

/** a row's help line in the page's language: "by <authors> · <its line>" */
export function rowHelp(id: string): string {
  const ms = rowMods(id), names = [...new Set(ms.flatMap(m => m.authors.map(a => a.name)))];
  const line = t(ms[0]?.line ?? '');
  return names.length ? `${t(LABELS.rowBy, { names: listOf(names) })} · ${line}` : line;
}

/** a group's names in its row's help: the first 2 or 3 (a third where they stay this short), then how many more */
const NAMES_LONG = 44;
/** where the English names go in the help's translated text */
const MARK = '\u0001';

/** a group of our changes' help line: its first names (English, as the popup's), and "and {n} more" */
function groupHelp(items: readonly Change[]): (Node | string)[] {
  const names = items.map(i => i.name);
  let n = Math.min(2, names.length);
  if (n < names.length && names.slice(0, n + 1).join(', ').length <= NAMES_LONG) n++;
  const rest = names.length - n;
  const shown = rest ? names.slice(0, n).join(', ') : listOf(names);
  const text = rest ? tn(rest, '{names} and {n} more', '{names} and {n} more', { names: MARK }) : MARK;
  const at = text.indexOf(MARK);
  if (at < 0) return [text];
  return [text.slice(0, at), Object.assign(document.createElement('span'), { lang: 'en', textContent: shown }), text.slice(at + 1)];
}

const SVG = 'http://www.w3.org/2000/svg';
/** the (i): a circle with an i, as the header's icons (24x24, a 2 px stroke, round caps) */
function infoIcon(): SVGSVGElement {
  const svg = document.createElementNS(SVG, 'svg');
  svg.setAttribute('viewBox', '0 0 24 24'); svg.setAttribute('aria-hidden', 'true');
  const c = document.createElementNS(SVG, 'circle'); c.setAttribute('cx', '12'); c.setAttribute('cy', '12'); c.setAttribute('r', '9');
  const p = document.createElementNS(SVG, 'path'); p.setAttribute('d', 'M12 11v5M12 8h.01');
  svg.append(c, p);
  return svg;
}

/** the (i) of row id (a mod's, or a group of our changes'), named name: it opens the row's popup */
function infoButton(id: string, name: string): HTMLButtonElement {
  const b = document.createElement('button'); b.type = 'button'; b.className = 'icon-btn mod-about'; b.dataset.mod = id;
  b.setAttribute('aria-haspopup', 'dialog');
  b.setAttribute('aria-label', name); b.title = name;
  b.append(infoIcon());
  b.onclick = () => openInfo(id, b);
  return b;
}

/**
 * A row's name (its .set-label, a span) with the (i) right after it: the name's last word and the (i) in one piece that
 * does not wrap (.set-name-end), so where the name takes two lines the (i) follows its last word, never alone.  As the
 * (i) is in it, the label names nothing by its text: the row's control (settings-ui.ts) and group take the name as
 * their aria-label.
 */
function withInfo(label: HTMLElement, info: HTMLButtonElement): HTMLElement {
  const text = label.textContent ?? '', at = text.lastIndexOf(' ') + 1;
  const end = document.createElement('span'); end.className = 'set-name-end';
  end.append(text.slice(at), info);
  label.replaceChildren(text.slice(0, at), end);
  return label;
}

/** a mod's row's name: its label (settings-ui.ts's, for a row with a setting) with the mod's (i) in it */
export function modName(label: HTMLElement, id: string): HTMLElement {
  return withInfo(label, infoButton(id, t(LABELS.about, { name: rowName(id) })));
}

/** a section of the tab: its heading and its rows */
function section(id: string, title: string, rows: HTMLElement[]): HTMLElement {
  const box = document.createElement('div'); box.className = 'group mods-group';
  const h = document.createElement('h3'); h.id = `mods-${id}`; h.textContent = t(title);
  box.setAttribute('role', 'group'); box.setAttribute('aria-labelledby', h.id);
  box.append(h, ...rows);
  return box;
}

/** a section's first row, a setting of no mod's: how other players' cars show the mods' parts (settings-ui.ts; out for
 *  now with the rims: { shared: 'others_mod_parts' }) */
const LEAD: Partial<Record<string, string>> = {};

/**
 * The tab's sections, in KINDS' order (a section without entries is left out), a row per mod, then our changes, a row
 * per group.  settingRow(key): the settings panel's row for that setting (its name with the (i): modName).
 */
export function modSections(settingRow: (key: string) => HTMLElement): HTMLElement[] {
  const mods = KINDS.flatMap(k => {
    const ms = MODS.filter(m => m.kind === k.kind && !m.row);
    const lead = LEAD[k.kind] ? [settingRow(LEAD[k.kind]!)] : [];
    return ms.length ? [section(k.kind, k.title, [...lead, ...ms.map(m => 'setting' in m.control ? settingRow(m.control.setting) : modRow(m))])] : [];
  });
  return [...mods, section('ours', OURS.title, ADDED.map(g =>
    plainRow(g.id, t(g.title), t(LABELS.ours, { name: t(g.title) }), groupHelp(g.items))))];
}

/** a row with no control: its name with its (i) (named about), and its help line */
function plainRow(id: string, name: string, about: string, help: (Node | string)[]): HTMLElement {
  const row = document.createElement('div'); row.className = 'set set-plain'; row.dataset.mod = id;
  const label = document.createElement('span'); label.className = 'set-label'; label.textContent = name;
  const p = document.createElement('p'); p.className = 'set-help'; p.append(...help);
  row.append(withInfo(label, infoButton(id, about)), p);
  row.setAttribute('role', 'group'); row.setAttribute('aria-label', name);
  return row;
}

/** a mod's row without a control here (where it is set: its popup's facts) */
const modRow = (m: Mod) => plainRow(m.id, rowName(m.id), t(LABELS.about, { name: rowName(m.id) }), [rowHelp(m.id)]);

/** where a mod without a setting is set (a popup's fact): "Always on", or the page's or the game's place */
function placeOf(m: Mod): string {
  const c = m.control;
  return 'always' in c ? t(LABELS.always) : 'elsewhere' in c ? t(PLACES[c.elsewhere]) : 'menu' in c ? t(PLACES[c.menu]) : '';
}

// ------------------------------------------------------------------------------------------------- the popup --

/** the row whose popup is open, and the button that opened it (the focus goes back to it) */
let open: { id: string; from: HTMLButtonElement } | null = null;

/** text with {names} as the author links (the page's "a, b and c"): an author's own page, or the mod's for its one
 *  author of its own; a port's authors and a mod's several without a page of their own as plain text */
function byLine(label: string, authors: readonly Author[], m: Mod): Node[] {
  const marks = authors.map((_, i) => `\u0001${i}\u0001`);
  const text = t(label, { names: listOf(marks) });
  const sole = m.authors.filter(a => !a.port).length === 1;
  return text.split(/\u0001(\d+)\u0001/).map((part, i) => {
    if (i % 2 === 0) return document.createTextNode(part);
    const a = authors[+part], href = a.url ?? (sole && !a.port ? m.links.page ?? m.links.source : undefined);
    if (!href) return document.createTextNode(a.name);
    return Object.assign(document.createElement('a'), { href, target: '_blank', rel: 'noopener noreferrer', textContent: a.name });
  });
}

/** a mod's makers in one line: from its mod, by its authors, ported by */
function makers(m: Mod): HTMLElement | null {
  const own = m.authors.filter(a => !a.port), port = m.authors.filter(a => a.port);
  const parts: Node[][] = [];
  if (m.mod) parts.push([document.createTextNode(t(LABELS.partOf, { mod: m.mod }))]);
  if (own.length) parts.push(byLine(LABELS.by, own, m));
  if (port.length) parts.push(byLine(LABELS.port, port, m));
  if (!parts.length) return null;
  const p = document.createElement('p'); p.className = 'hint mod-by';
  parts.forEach((x, i) => { if (i) p.append(' · '); p.append(...x); });
  return p;
}

/** a mod's short facts: where it is set (or "With it off: as in the original game"), who sees it, its download, what
 *  of it is here, its licence */
function facts(m: Mod): HTMLElement {
  const ul = document.createElement('ul'); ul.className = 'mod-facts';
  const add = (s: string) => ul.append(Object.assign(document.createElement('li'), { textContent: s }));
  if ('setting' in m.control) { if (m.off !== false) add(t(LABELS.off)); } else add(placeOf(m));
  if (!m.parts) add(t(ONLINE[m.online]));
  if (m.sizeMB) add(t(LABELS.size, { n: m.sizeMB }));
  if (m.uses) add(t(USES[m.uses]));
  if (m.licence) add(t(LABELS.licence, { licence: m.licence }));
  return ul;
}

/** one mod in the popup: (a shared row's: its name and its makers) what it does, its parts, its facts and its pages */
function modInfo(m: Mod, named: boolean): Node[] {
  const out: Node[] = [];
  if (named) {
    out.push(Object.assign(document.createElement('h3'), { textContent: m.name }));
    const by = makers(m);
    if (by) out.push(by);
  }
  out.push(Object.assign(document.createElement('p'), { lang: 'en', textContent: m.details }));
  if (m.parts) {
    const ul = document.createElement('ul'); ul.className = 'mod-parts';
    for (const p of m.parts) {
      const li = ul.appendChild(document.createElement('li'));
      const text = document.createElement('span'); text.lang = 'en'; text.textContent = p.text;
      li.append(Object.assign(document.createElement('strong'), { lang: 'en', textContent: `${p.name}:` }), ' ', text, ` (${t(ONLINE[p.online])})`);
    }
    out.push(ul);
  }
  out.push(facts(m));
  const links = ([[m.links.page, LABELS.page], [m.links.source, LABELS.source]] as const).filter(([href]) => href);
  if (links.length) {
    const row = document.createElement('p'); row.className = 'mod-links';
    for (const [href, label] of links) row.append(Object.assign(document.createElement('a'), { href: href!, target: '_blank', rel: 'noopener noreferrer', textContent: t(label) }));
    out.push(row);
  }
  return out;
}

/** a group of our changes in the popup: a line each, "<name> (<key>): <line>" (English, as the changelog's) */
function changes(items: readonly Change[]): HTMLElement {
  const ul = document.createElement('ul'); ul.className = 'mod-parts'; ul.lang = 'en';
  for (const c of items) {
    const li = ul.appendChild(document.createElement('li'));
    li.append(Object.assign(document.createElement('strong'), { textContent: c.key ? `${c.name} (${c.key}):` : `${c.name}:` }), ' ', c.line);
  }
  return ul;
}

/** the popup's title (the row's name) and, for one mod, its makers under it; then each mod's own (a group of our
 *  changes': its title, and its changes) */
function draw(d: HTMLDialogElement, id: string) {
  const g = GROUPS.get(id), ms = g ? [] : rowMods(id), one = ms.length === 1;
  d.querySelector('#mod-info-title')!.textContent = g ? t(g.title) : rowName(id);
  const by = d.querySelector<HTMLElement>('#mod-info-by')!, made = one ? makers(ms[0]) : null;
  by.replaceChildren(...made ? made.childNodes : []); by.hidden = !made;
  d.querySelector('#mod-info-body')!.replaceChildren(...g ? [changes(g.items)] : ms.flatMap(m => modInfo(m, !one)));
}

/** the popup of row id, from its (i) button: Close or Esc closes it, and the keyboard goes back to the button (the row's
 *  new one where the row was drawn again meanwhile) */
function openInfo(id: string, from: HTMLButtonElement) {
  const d = document.querySelector<HTMLDialogElement>('#mod-info');
  if (!d) return;
  draw(d, id);
  open = { id, from };
  if (!d.dataset.wired) {
    d.dataset.wired = '1';
    d.addEventListener('close', () => {
      const was = open; open = null;
      if (!was) return;
      const b = was.from.isConnected ? was.from : document.querySelector<HTMLButtonElement>(`.mod-about[data-mod="${CSS.escape(was.id)}"]`);
      b?.focus();
    });
    onLanguage(() => { if (open && d.open) draw(d, open.id); });
  }
  d.showModal();
  d.querySelector<HTMLElement>('#mod-info-body')!.scrollTop = 0;
}
