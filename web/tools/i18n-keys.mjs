// The start page's English texts, as the page looks them up in a language's file (web/src/app/i18n.ts):
//   node web/tools/i18n-keys.mjs              every text, with where it is and its plural's one form (JSON)
//   node web/tools/i18n-keys.mjs <language>   the texts web/src/app/lang/<language>.json does not have yet
//   node web/tools/i18n-keys.mjs --check      what is wrong in each game's language files (--check <language>: one)
//   --game <id>                               one game's page (web/build.mjs --game): ug2 by default, with --check
//                                             every game
// From the sources: the first argument of t(), T(), N() and tvars() and the last two of tn() when they are string
// literals, and the markup index.html marks (data-i18n: its text, data-i18n-html: its text with <0>…</0> for the
// elements in it, data-i18n-attr: the attributes named).  A game's page has the texts of the shared sources and of its
// own folder, web/src/games/<id>, and its language files are the shared web/src/app/lang/<language>.json with the
// game's own web/src/games/<id>/lang/<language>.json over it (web/build.mjs merges them the same way).  So the shared
// file holds the texts every game uses, and a game's file the texts only it uses and those it words differently.
// web/test/i18n.test.mjs checks every game's language files with it.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

/** a JS string literal at s[i] (a quote): [its value, the index after it] */
function literal(s, i) {
  const q = s[i];
  let out = '', j = i + 1;
  for (; j < s.length && s[j] !== q; j++) {
    if (s[j] !== '\\') { out += s[j]; continue; }
    const c = s[++j];
    out += c === 'n' ? '\n' : c === 't' ? '\t' : c === 'u' ? String.fromCharCode(parseInt(s.slice(j + 1, (j += 4) + 1), 16)) : c;
  }
  return [out, j + 1];
}
const space = (s, i) => { while (/\s/.test(s[i])) i++; return i; };

/** the texts of one TypeScript source: [{ key, one?, plural }]; bad: calls whose text is not a plain literal */
export function sourceKeys(src) {
  const out = [], bad = [];
  const re = /\b(t|T|N|tvars|tn)\(/g;
  for (let m; (m = re.exec(src));) {
    if (/[\w.$]/.test(src[m.index - 1] ?? '')) continue;          // x.t(, myT(
    let i = space(src, m.index + m[0].length);
    if (m[1] === 'tn') {
      // past the count, to the first comma at its own depth
      let depth = 0;
      for (; i < src.length; i++) {
        const c = src[i];
        if (c === '\'' || c === '"' || c === '`') { i = literal(src, i)[1] - 1; continue; }
        if ('([{'.includes(c)) depth++;
        else if (')]}'.includes(c)) depth--;
        else if (c === ',' && depth === 0) break;
      }
      i = space(src, i + 1);
      if (src[i] !== '\'' && src[i] !== '"') { bad.push(src.slice(m.index, m.index + 60)); continue; }
      const [one, j] = literal(src, i);
      i = space(src, j);
      if (src[i] !== ',') { bad.push(src.slice(m.index, m.index + 60)); continue; }
      i = space(src, i + 1);
      if (src[i] !== '\'' && src[i] !== '"') { bad.push(src.slice(m.index, m.index + 60)); continue; }
      out.push({ key: literal(src, i)[0], one, plural: true });
      continue;
    }
    // t(x), T(x) and tvars(x) of a text made elsewhere (in N()) are fine; N() of anything but a literal is not
    if (src[i] !== '\'' && src[i] !== '"') { if (m[1] === 'N') bad.push(src.slice(m.index, m.index + 60)); continue; }
    out.push({ key: literal(src, i)[0], plural: false });
  }
  return { keys: out, bad };
}

// ------------------------------------------------------------------------------------------------ index.html --

const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'source', 'track', 'wbr']);
const decode = s => s.replace(/&(#x[0-9a-f]+|#\d+|[a-z]+);/gi, (m, e) => e[0] === '#' ? String.fromCodePoint(e[1] === 'x' || e[1] === 'X' ? parseInt(e.slice(2), 16) : +e.slice(1))
  : ({ amp: '&', lt: '<', gt: '>', quot: '"', apos: '\'', nbsp: ' ' })[e.toLowerCase()] ?? m);
const esc = s => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const norm = s => s.replace(/\s+/g, ' ').trim();

/** index.html as a tree: { tag, attrs, kids } and text strings */
function parseHtml(html) {
  const root = { tag: '#root', attrs: {}, kids: [] }, stack = [root];
  const re = /<!--[\s\S]*?-->|<!doctype[^>]*>|<\/([a-z0-9-]+)\s*>|<([a-z0-9-]+)((?:\s+[^\s=>/]+(?:\s*=\s*(?:"[^"]*"|'[^']*'|[^\s>]+))?)*)\s*\/?>|([^<]+)/gi;
  for (let m; (m = re.exec(html));) {
    const top = stack[stack.length - 1];
    if (m[4] !== undefined) { top.kids.push(decode(m[4])); continue; }
    if (m[1]) { const t = m[1].toLowerCase(); while (stack.length > 1 && stack.pop().tag !== t); continue; }
    if (!m[2]) continue;
    const attrs = {};
    for (const a of m[3].matchAll(/([^\s=>/]+)(?:\s*=\s*("[^"]*"|'[^']*'|[^\s>]+))?/g)) attrs[a[1].toLowerCase()] = a[2] === undefined ? '' : decode(a[2].replace(/^["']|["']$/g, ''));
    const el = { tag: m[2].toLowerCase(), attrs, kids: [] };
    top.kids.push(el);
    if (!VOID.has(el.tag) && el.tag !== 'script' && el.tag !== 'style') stack.push(el);
    else if (el.tag === 'script' || el.tag === 'style') { const end = html.indexOf(`</${el.tag}`, re.lastIndex); re.lastIndex = end < 0 ? html.length : end; }
  }
  return root;
}
const textOf = n => typeof n === 'string' ? n : n.kids.map(textOf).join('');
/** i18n.ts htmlKey: the text, each element <k>…</k> in the order they open */
function htmlKey(el) {
  let k = 0;
  const walk = n => n.kids.map(c => typeof c === 'string' ? esc(c) : (i => `<${i}>${walk(c)}</${i}>`)(k++)).join('');
  return norm(walk(el));
}

/** the texts index.html marks: [{ key, where }] */
export function htmlKeys(html) {
  const out = [];
  const visit = n => {
    if (typeof n === 'string') return;
    const where = `index.html ${n.tag}${n.attrs.id ? '#' + n.attrs.id : ''}`;
    if ('data-i18n' in n.attrs) out.push({ key: norm(textOf(n)), where });
    if ('data-i18n-html' in n.attrs) out.push({ key: htmlKey(n), where, html: true });
    if ('data-i18n-attr' in n.attrs) for (const a of n.attrs['data-i18n-attr'].split(/\s+/).filter(Boolean)) out.push({ key: n.attrs[a] ?? '', where: `${where} [${a}]` });
    n.kids.forEach(visit);
  };
  visit(parseHtml(html));
  return out;
}

// --------------------------------------------------------------------------------------------- the catalog --

/** the games the page is built for (web/build.mjs --game <id>): the folders of web/src/games with an index.ts, ug2
 *  (the default) first.  root: the web folder (another one in the tests), here and below. */
export function gamesIn(root = WEB) {
  const dir = path.join(root, 'src/games');
  const all = fs.existsSync(dir) ? fs.readdirSync(dir).filter(g => fs.existsSync(path.join(dir, g, 'index.ts'))).sort() : [];
  return [...all.filter(g => g === 'ug2'), ...all.filter(g => g !== 'ug2')];
}
export const GAMES = gamesIn();

function sources(dir, game, games) {
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap(e => {
    const p = path.join(dir, e.name);
    // the test pages are not the player's, nor is another game's folder; i18n.ts itself only defines the functions
    return e.isDirectory() ? (e.name === 'test' || (dir === games && e.name !== game) ? [] : sources(p, game, games))
      : e.name.endsWith('.ts') && e.name !== 'i18n.ts' ? [p] : [];
  });
}

/** every text of a game's page: Map key -> { where: string[], plural, one?, html? }; bad: calls the extraction cannot
 *  read */
export function catalog(game = 'ug2', root = WEB) {
  const src = path.join(root, 'src');
  const all = new Map(), bad = [];
  const add = (k, where, extra = {}) => {
    if (!k) return;
    const e = all.get(k) ?? { where: [], plural: false };
    if (!e.where.includes(where)) e.where.push(where);
    Object.assign(e, extra.plural ? { plural: true, one: extra.one } : {}, extra.html ? { html: true } : {});
    all.set(k, e);
  };
  for (const f of sources(src, game, path.join(src, 'games'))) {
    const rel = path.relative(src, f).split(path.sep).join('/');
    const r = sourceKeys(fs.readFileSync(f, 'utf8'));
    for (const k of r.keys) add(k.key, rel, k);
    bad.push(...r.bad.map(b => `${rel}: ${b}`));
  }
  for (const k of htmlKeys(pageHtml(game, root))) add(k.key, k.where, k);
  return { keys: all, bad };
}

/** index.html as a game's page has it: with the edits of its src/games/<id>/body.json made, as web/build.mjs makes
 *  them (each "at" once in the page, "until" the end of the cut after it); throws where one does not fit */
export function pageHtml(game = 'ug2', root = WEB) {
  let html = fs.readFileSync(path.join(root, 'index.html'), 'utf8');
  const body = path.join(root, 'src/games', game, 'body.json');
  if (fs.existsSync(body)) for (const e of JSON.parse(fs.readFileSync(body, 'utf8'))) {
    const a = html.indexOf(e.at), u = e.until === undefined ? a + e.at.length : html.indexOf(e.until, a + e.at.length);
    if (a < 0 || html.indexOf(e.at, a + 1) >= 0 || u < 0) throw new Error(`games/${game}/body.json: "${e.at.trim().slice(0, 80)}" does not fit index.html`);
    html = html.slice(0, a) + e.put + html.slice(e.until === undefined ? u : u + e.until.length);
  }
  return html;
}

/** the {name}s of a text */
export const holes = s => new Set([...s.matchAll(/\{(\w+)\}/g)].map(m => m[1]));
/** the <k> tags of a text, in order, opening and closing */
export const tags = s => [...s.matchAll(/<(\/?)(\d+)>/g)].map(m => m[0]);

/** CLDR's plural forms (Intl.PluralRules) of each language the files are for */
const LOCALE = { french: 'fr', german: 'de', italian: 'it', spanish: 'es', dutch: 'nl', swedish: 'sv', danish: 'da', korean: 'ko',
  chinese: 'zh-Hant', japanese: 'ja', russian: 'ru', thai: 'th', portuguese: 'pt-BR', polish: 'pl', turkish: 'tr' };
export const LANGUAGE_FILES = Object.keys(LOCALE);
export const langFile = (l, root = WEB) => path.join(root, 'src/app/lang', `${l}.json`);
/** a game's own texts in a language, over the shared file's */
export const ownLangFile = (game, l, root = WEB) => path.join(root, 'src/games', game, 'lang', `${l}.json`);

/** a language's texts on a game's page: the shared file, the game's own file (if any) over it, as web/build.mjs
 *  writes /lang/<language>.json; throws when a file does not read */
export function langTexts(game, lang, root = WEB) {
  const { shared, own } = langFiles(game, lang, root);
  return { ...shared, ...own };
}
function langFiles(game, lang, root) {
  const read = (f, name) => { try { return JSON.parse(fs.readFileSync(f, 'utf8')); } catch (e) { throw new Error(`${name}: ${e.message}`); } };
  const own = ownLangFile(game, lang, root);
  return { shared: read(langFile(lang, root), `${lang}.json`), own: fs.existsSync(own) ? read(own, `games/${game}/lang/${lang}.json`) : {} };
}

/** What is wrong with a game's texts in a language against the texts its page uses: [] when nothing. */
export function check(lang, keys, game = 'ug2', root = WEB) {
  keys ??= catalog(game, root).keys;
  const out = [];
  let shared, own;
  try { ({ shared, own } = langFiles(game, lang, root)); } catch (e) { return [e.message]; }
  const have = { ...shared, ...own }, ownName = `games/${game}/lang/${lang}.json`;
  for (const [k, v] of Object.entries(own)) if (k in shared && JSON.stringify(v) === JSON.stringify(shared[k])) out.push(`${ownName} says it as the shared file does: ${JSON.stringify(k)}`);
  const forms = new Intl.PluralRules(LOCALE[lang]).resolvedOptions().pluralCategories;
  const sameTags = (en, x) => JSON.stringify(tags(en).sort()) === JSON.stringify(tags(x).sort());
  /** x's {name}s are the English's, and it keeps all of them but {n}, which one form may say in words; {pos} (an
   *  English ordinal, "3rd") comes with {n}, its number, which a language says its own way in place of it */
  const holesOk = (en, x, mayDropN) => {
    const a = holes(en), b = holes(x);
    if (a.has('pos')) a.add('n');
    return [...b].every(h => a.has(h)) && [...a].every(h => b.has(h) || (h === 'n' && (mayDropN || a.has('pos'))) || (h === 'pos' && b.has('n')));
  };
  for (const [k, e] of keys) {
    // a text only the game's own folder uses goes in the game's file
    if (!(k in have)) { out.push(`missing${e.where.every(w => w.startsWith(`games/${game}/`)) ? ` (${ownName})` : ''}: ${JSON.stringify(k)}`); continue; }
    const v = have[k];
    const en = e.plural ? [e.one, k].join(' ') : k;
    const texts = typeof v === 'string' ? [[v, false]] : v && typeof v === 'object' && e.plural ? Object.entries(v).map(([f, x]) => [x, f !== 'other', f]) : null;
    if (!texts) { out.push(`not a text${e.plural ? ' or plural forms' : ''}: ${JSON.stringify(k)}`); continue; }
    if (typeof v === 'object') {
      for (const f of Object.keys(v)) if (!forms.includes(f)) out.push(`plural form "${f}" is not ${lang}'s (${forms.join(', ')}): ${JSON.stringify(k)}`);
      if (!('other' in v)) out.push(`plural forms without "other": ${JSON.stringify(k)}`);
    }
    for (const [x, single] of texts) {
      if (typeof x !== 'string' || !x.trim()) { out.push(`empty: ${JSON.stringify(k)}`); continue; }
      if (!holesOk(e.plural ? en : k, x, e.plural && single)) out.push(`{names} differ: ${JSON.stringify(k)} -> ${JSON.stringify(x)}`);
      if (!sameTags(k, x)) out.push(`<n> tags differ: ${JSON.stringify(k)} -> ${JSON.stringify(x)}`);
    }
  }
  // the shared file's texts are every game's: one this game does not use belongs in the files of the games that do
  for (const k of Object.keys(have)) if (!keys.has(k)) out.push(`not in use${k in own ? ` (${ownName})` : ''}: ${JSON.stringify(k)}`);
  return out;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2), at = args.indexOf('--game');
  const only = at < 0 ? null : args.splice(at, 2)[1] ?? '';
  if (only !== null && !GAMES.includes(only)) { console.error(`no game "${only}" (web/src/games: ${GAMES.join(', ')})`); process.exit(2); }
  const [a, b] = args;
  if (a === '--check') {
    let n = 0;
    for (const g of only ? [only] : GAMES) {
      const { keys, bad } = catalog(g);
      for (const x of bad) console.error(`${g}: not a literal: ${x}`);
      for (const l of b ? [b] : LANGUAGE_FILES) { const p = check(l, keys, g); n += p.length; console.log(`${g} ${l}: ${p.length ? p.length + ' problems' : 'ok'}`); for (const x of p) console.log(`  ${x}`); }
    }
    process.exit(n ? 1 : 0);
  }
  const { keys, bad } = catalog(only ?? 'ug2');
  for (const x of bad) console.error(`not a literal: ${x}`);
  const have = a ? (() => { try { return langTexts(only ?? 'ug2', a); } catch { return {}; } })() : {};
  const out = {};
  for (const [k, e] of keys) if (!(k in have)) out[k] = e;
  console.log(JSON.stringify(out, null, 1));
}
