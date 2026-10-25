// The start page in the player's language.  It is the game's own (settings.ini language, settings.ts LANGUAGES), so
// one choice sets both, in Settings and in the list at the top; English until the player picks another.  English is
// the source: each text is written in English where it is used, and t() looks that English up in the language's file
// (web/src/app/lang/<language>.json, served as /lang/<language>.json, fetched once the language is picked), {name}
// for the parts that change.  A text the file does not have stays English.  The start page's own markup is marked in
// index.html: data-i18n (its text), data-i18n-html (a text with elements in it, which the file's text names <0>…</0>
// in the order they open, so a language may move them), data-i18n-attr (attributes, by name).
//
// Texts shown for longer (the status lines, the panels) are drawn again when the language changes (onLanguage, live).
// Problems and errors carry a fixed code in every language (checks.ts, shell.ts), so a screenshot in any language
// says which one it is; the log stays English.  web/test/i18n.test.mjs checks every file against the texts in use.
//
// This module touches the DOM only when called with it, and imports nothing, so the pure modules (settings.ts) can
// use it under node.

/** the game's languages (runtime/language.c), settings.ini language's values; english is the default */
export const LANGUAGES = ['english', 'french', 'german', 'italian', 'spanish', 'dutch', 'swedish', 'danish', 'korean',
  'chinese', 'japanese', 'russian', 'thai', 'portuguese', 'polish', 'turkish'] as const;
export type Language = typeof LANGUAGES[number];
type Forms = Partial<Record<Intl.LDMLPluralRule, string>>;
export type Vars = Record<string, string | number>;

/** each language's BCP 47 tag: the page's lang attribute, its plurals, dates and lists (the game's chinese is
 *  Traditional Chinese) */
export const LOCALE: Record<Language, string> = {
  english: 'en', french: 'fr', german: 'de', italian: 'it', spanish: 'es', dutch: 'nl', swedish: 'sv', danish: 'da',
  korean: 'ko', chinese: 'zh-Hant', japanese: 'ja', russian: 'ru', thai: 'th', portuguese: 'pt-BR', polish: 'pl', turkish: 'tr',
};
/** each language in its own words (the pickers) */
export const NATIVE: Record<Language, string> = {
  english: 'English', french: 'Français', german: 'Deutsch', italian: 'Italiano', spanish: 'Español', dutch: 'Nederlands',
  swedish: 'Svenska', danish: 'Dansk', korean: '한국어', chinese: '繁體中文', japanese: '日本語', russian: 'Русский', thai: 'ไทย',
  portuguese: 'Português (Brasil)', polish: 'Polski', turkish: 'Türkçe',
};

let current: Language = 'english';
let dict: Record<string, string | Forms> = {};
let plural: Intl.PluralRules | null = null;
const listeners: (() => void)[] = [];
/** the language's file asked for last (the newest pick wins) */
let asked: Language = 'english';

export const isLanguage = (v: string): v is Language => (LANGUAGES as readonly string[]).includes(v);
/** the page's language now */
export const language = (): Language => current;

/** s with its {name}s from v (English, as written: the log's) */
export function fill(s: string, v?: Vars): string {
  return v ? s.replace(/\{(\w+)\}/g, (m, k: string) => k in v ? String(v[k]) : m) : s;
}

/** An English text for t() later: it stays as it is here (a table's labels, a check's detail), and is translated
 *  where it is shown.  web/test/i18n.test.mjs finds the texts in N(), t() and tn(). */
export const N = (en: string) => en;

/** en in the page's language, its {name}s filled from v */
export function t(en: string, v?: Vars): string {
  const x = dict[en];
  return fill(typeof x === 'string' ? x : en, v);
}

/** A text with a count, {n}: English has one and other (other is the key); a language's file gives the forms its
 *  plural rules have (one, few, many, other: Intl.PluralRules), or one text for every count. */
export function tn(n: number, one: string, other: string, v?: Vars): string {
  const x = dict[other], vars = { n, ...v };
  if (typeof x === 'string') return fill(x, vars);
  if (x) {
    plural ??= new Intl.PluralRules(LOCALE[current]);
    const f = x[plural.select(n)] ?? x.other;
    if (f) return fill(f, vars);
  }
  return fill(n === 1 ? one : other, vars);
}

/** t(), with v's texts translated as well (a check's "the browser") */
export function tvars(en: string, v?: Vars): string {
  return t(en, v && Object.fromEntries(Object.entries(v).map(([k, x]) => [k, typeof x === 'string' ? t(x) : x])));
}

/** a text made in the page's language whenever it is shown: what live() and the status lines take */
export type Msg = () => string;
/** t(en, v) as a Msg */
export const T = (en: string, v?: Vars): Msg => () => t(en, v);

/** the locales for dates and times: in English the browser's own, as before; else the page's language */
export const dateLocale = (): string[] | string => current === 'english' ? [] : LOCALE[current];
/** "a, b and c" in the page's language */
export function listOf(items: string[]): string {
  if (items.length < 2) return items[0] ?? '';
  return new Intl.ListFormat(current === 'english' ? 'en-GB' : LOCALE[current], { type: 'conjunction' }).format(items);
}

/** x as a percentage in the page's language (1.25: 125 %) */
export function percent(x: number): string {
  return new Intl.NumberFormat(current === 'english' ? 'en-GB' : LOCALE[current], { style: 'percent' }).format(x);
}

/** f again whenever the page's language changes */
export function onLanguage(f: () => void) { listeners.push(f); }

/** elements whose text live() keeps in the page's language */
const lives = new Map<Element, { f: Msg; code?: string }>();
/** el's text from f, now and again whenever the language changes, until live() gives it another; code: a problem's
 *  fixed name after it (the same in every language) */
export function live(el: Element, f: Msg, code?: string) {
  lives.set(el, { f, code });
  draw(el, f, code);
}
function draw(el: Element, f: Msg, code?: string) {
  el.textContent = f();
  if (code) el.append(codeOf(code));
}
/** a problem's code as the page shows it (beside its text, in any language) */
export function codeOf(code: string): HTMLElement {
  const c = document.createElement('code');
  c.className = 'err-code'; c.textContent = code;
  return c;
}

/**
 * The page in language l: its file fetched (English has none), then the markup, the live texts and every onLanguage
 * listener.  False: the file did not load (the page stays as it was; the log says why).
 */
export async function setLanguage(l: string, log?: (s: string) => void): Promise<boolean> {
  const want: Language = isLanguage(l) ? l : 'english';
  asked = want;
  if (want === current) return true;
  let next: Record<string, string | Forms> = {};
  if (want !== 'english') {
    try {
      const r = await fetch(`/lang/${want}.json`);
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      next = await r.json() as Record<string, string | Forms>;
    } catch (e) {
      log?.(`[app] the page's ${want} texts did not load (${e}): it stays ${current}`);
      return false;
    }
  }
  if (asked !== want) return false;            // a newer pick came first
  current = want; dict = next; plural = null;
  try { localStorage.setItem(STORE, want); } catch { /* not kept: the next start shows English until settings.ini is read */ }
  if (typeof document !== 'undefined') {
    document.documentElement.lang = LOCALE[want];
    translatePage(document);
    for (const [el, x] of lives) { if (el.isConnected) draw(el, x.f, x.code); else lives.delete(el); }
  }
  for (const f of listeners) { try { f(); } catch (e) { log?.(`[app] the page in ${want}: a part was not drawn again (${e})`); } }
  log?.(`[app] the page is in ${want} now`);
  return true;
}

/** the language this browser showed last (the page asks for its file at once, before settings.ini is read) */
const STORE = 'nfsu2-page-language';
export function lastLanguage(): Language {
  try { const v = localStorage.getItem(STORE) ?? ''; return isLanguage(v) ? v : 'english'; } catch { return 'english'; }
}

// ------------------------------------------------------------------------------------------------ the markup --

const norm = (s: string) => s.replace(/\s+/g, ' ').trim();
const esc = (s: string) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const unesc = (s: string) => s.replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&');

/** A data-i18n-html element's English: its text, each element in it <k>…</k> in the order they open (and numbered
 *  with data-i18n-n, so a copy of the markup finds them as well).  web/test/i18n.test.mjs makes the same of index.html. */
function htmlKey(el: Element): string {
  let k = 0;
  const walk = (n: Node): string => {
    let s = '';
    for (const c of n.childNodes) {
      if (c.nodeType === 3) s += esc(c.textContent ?? '');
      else if (c instanceof Element) { const i = k++; c.setAttribute('data-i18n-n', String(i)); s += `<${i}>${walk(c)}</${i}>`; }
    }
    return s;
  };
  return norm(walk(el));
}

/** The element's content from text, a translation of its key: the text between the tags, and its own elements
 *  (the same nodes, so what listens on them stays) where the tags put them.  A tag it does not have is left out. */
function build(el: Element, text: string) {
  const nodes = new Map<number, Element>();
  for (const e of el.querySelectorAll('[data-i18n-n]')) if (e.parentElement?.closest('[data-i18n-html]') === el) nodes.set(+e.getAttribute('data-i18n-n')!, e);
  const stack: Element[] = [el];
  el.replaceChildren();
  for (const part of text.split(/(<\/?\d+>)/)) {
    const m = /^<(\/?)(\d+)>$/.exec(part);
    if (!m) { if (part) stack[stack.length - 1].append(unesc(part)); continue; }
    const e = nodes.get(+m[2]);
    if (!e) continue;
    if (m[1]) { if (stack.length > 1 && stack[stack.length - 1] === e) stack.pop(); continue; }
    e.replaceChildren();
    stack[stack.length - 1].append(e);
    stack.push(e);
  }
}

/** The marked markup in root in the page's language (the English kept on each element the first time). */
export function translatePage(root: ParentNode) {
  for (const el of root.querySelectorAll<HTMLElement>('[data-i18n]')) {
    const src = el.getAttribute('data-i18n-src') ?? norm(el.textContent ?? '');
    el.setAttribute('data-i18n-src', src);
    el.textContent = t(src);
  }
  for (const el of root.querySelectorAll<HTMLElement>('[data-i18n-html]')) {
    const src = el.getAttribute('data-i18n-src') ?? htmlKey(el);
    el.setAttribute('data-i18n-src', src);
    build(el, t(src));
  }
  for (const el of root.querySelectorAll<HTMLElement>('[data-i18n-attr]')) {
    for (const a of el.getAttribute('data-i18n-attr')!.split(/\s+/).filter(Boolean)) {
      const src = el.getAttribute(`data-i18n-${a}`) ?? el.getAttribute(a) ?? '';
      el.setAttribute(`data-i18n-${a}`, src);
      el.setAttribute(a, t(src));
    }
  }
}
