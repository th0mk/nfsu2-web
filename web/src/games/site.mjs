// The two games' sites (the site plan of 2026-10-31): Underground 2 at https://nfsu2.xyz/, Most Wanted at
// https://nfsrevived.com/mw/, and https://nfsrevived.com/ a page that links both (src/site, which Most Wanted's
// server answers at its root).  web/build.mjs gives each game's page the top bar (TRACKER M6-53, design M6-52 option
// A): the two games as tabs, its own the current one and the other a link to that game's site, and the language
// picker and the Discord button, moved up from the header unchanged.  No imports: build.mjs and the tests take it as
// it is.

/** the site's page, which links both games */
export const SITE = 'https://nfsrevived.com/';
/** the game whose page folder holds the site's page (build.mjs writes it there; that game's server answers it at its
 *  root: web/server-rs src/game.rs landing) */
export const SITE_GAME = 'mw';
/** each page language's tags on the site's page: its lang attribute and Content-Language (src/app/i18n.ts LOCALE;
 *  the server's src/game.rs LANDING_LANGUAGES) and its og:locale */
export const SITE_LOCALES = {
  english: ['en', 'en_US'], french: ['fr', 'fr_FR'], german: ['de', 'de_DE'], italian: ['it', 'it_IT'], spanish: ['es', 'es_ES'],
  dutch: ['nl', 'nl_NL'], swedish: ['sv', 'sv_SE'], danish: ['da', 'da_DK'], finnish: ['fi', 'fi_FI'], korean: ['ko', 'ko_KR'],
  chinese: ['zh-Hant', 'zh_TW'], japanese: ['ja', 'ja_JP'], russian: ['ru', 'ru_RU'], thai: ['th', 'th_TH'], portuguese: ['pt-BR', 'pt_BR'],
  polish: ['pl', 'pl_PL'], turkish: ['tr', 'tr_TR'], hungarian: ['hu', 'hu_HU'],
};
/** the site's page's address in a language: English's is the site's own (where the server picks by the browser's
 *  languages), another's ?lang=<language> */
export const siteUrl = lang => lang === 'english' ? SITE : `${SITE}?lang=${lang}`;

/** The site's page (src/site/index.html) in a page language.  It has no script, so build.mjs writes it once per
 *  language (site/index.html, site/index.<language>.html) and the server picks one (?lang=, Accept-Language).  Each
 *  marked text (data-i18n: an element's text; data-i18n-attr="content": a meta's) comes from texts (the language's
 *  file as build.mjs merges it; a text it lacks stays English); the lang attribute and og:locale are the language's,
 *  the canonical address and og:url its own (siteUrl), and the page names every language's (langs) for search engines
 *  (hreflang, x-default the site's address).  The marks go.  Throws on a language SITE_LOCALES lacks, or a page
 *  without one of the parts it changes. */
export function sitePage(html, lang, texts = {}, langs = [lang]) {
  for (const l of [lang, ...langs]) if (!SITE_LOCALES[l]) throw new Error(`site.mjs: no language ${l}`);
  const esc = s => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  const unesc = s => s.replace(/&quot;/g, '"').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&');
  const t = en => typeof texts[en] === 'string' && texts[en].trim() ? texts[en] : en;
  const swap = (a, b) => {
    if (html.split(a).length !== 2) throw new Error(`site.mjs: the site's page has ${html.includes(a) ? 'more than one' : 'no'} ${a}`);
    html = html.replace(a, () => b);
  };
  html = html.replace(/<(\w+)([^>]*?) data-i18n>([^<]*)<\/\1>/g, (m, tag, attrs, text) => `<${tag}${attrs}>${esc(t(unesc(text).replace(/\s+/g, ' ').trim()))}</${tag}>`)
    .replace(/ content="([^"]*)"([^>]*?) data-i18n-attr="content"/g, (m, text, rest) => ` content="${esc(t(unesc(text)))}"${rest}`);
  const [tag, og] = SITE_LOCALES[lang], url = siteUrl(lang);
  swap('<html lang="en">', `<html lang="${tag}">`);
  swap('<meta property="og:locale" content="en_US">', `<meta property="og:locale" content="${og}">`);
  swap(`<meta property="og:url" content="${SITE}">`, `<meta property="og:url" content="${url}">`);
  const alternates = [...langs.map(l => `<link rel="alternate" hreflang="${SITE_LOCALES[l][0]}" href="${siteUrl(l)}">`),
    `<link rel="alternate" hreflang="x-default" href="${SITE}">`];
  swap(`<link rel="canonical" href="${SITE}">`, [`<link rel="canonical" href="${url}">`, ...alternates].join('\n'));
  return html;
}
/** each game's page: its tab's name, its address and `bar`, whether its page has the bar.  Underground 2's is a switch
 *  of its own, off until the release that opens Most Wanted's site (its page stays as it is until then).  It is not
 *  the launch switch (src/games/ug2/launch.ts LAUNCHED), which also changes the word keyphrases are shown with:
 *  either one can go on without the other. */
export const GAMES = [
  { id: 'ug2', name: 'Underground 2 Revived', url: 'https://nfsu2.xyz/', bar: false },
  { id: 'mw', name: 'Most Wanted Revived', url: 'https://nfsrevived.com/mw/', bar: true },
];

/** the header's parts that move up into the bar (index.html has each once, in the header's .top-end) */
const LANG_PICK = /\n[ \t]*(<label class="lang-pick">[^]*?<\/label>)/;
const DISCORD = /\n[ \t]*(<a class="btn" href="https:\/\/discord\.gg\/[^"]+"[^>]*>[^]*?<\/a>)/;
const APP = '<main class="app" id="app">';

/** whether a game's page has the bar (its `bar` in GAMES; a game not in GAMES has none) */
export function hasBar(game) {
  return GAMES.find(x => x.id === game)?.bar === true;
}

/** The page (index.html after the game's own edits) with the bar above main#app, the language picker and Discord
 *  moved into it.  Throws when the page lacks one of the parts, or has one twice. */
export function withSiteBar(html, game) {
  const me = GAMES.find(x => x.id === game);
  if (!me) throw new Error(`site.mjs: no game ${game}`);
  if (html.includes('<nav class="site-bar"')) throw new Error('site.mjs: the page has the bar already');
  const take = (re, what) => {
    const m = re.exec(html);
    if (!m || re.exec(html.slice(m.index + m[0].length))) throw new Error(`site.mjs: the page has ${m ? 'more than one' : 'no'} ${what}`);
    html = html.slice(0, m.index) + html.slice(m.index + m[0].length);
    return m[1];
  };
  const lang = take(LANG_PICK, 'language picker'), discord = take(DISCORD, 'Discord button');
  const at = html.indexOf(APP);
  if (at < 0 || html.indexOf(APP, at + 1) >= 0) throw new Error(`site.mjs: the page has ${at < 0 ? 'no' : 'more than one'} ${APP}`);
  const tab = g => g === me ? `<a class="site-tab" aria-current="page">${g.name}</a>` : `<a class="site-tab" href="${g.url}">${g.name}</a>`;
  const bar = `<nav class="site-bar" id="site-bar" aria-label="Games" data-i18n-attr="aria-label">
  <div class="site-bar-in">
    <div class="site-tabs">
      ${GAMES.map(tab).join('\n      ')}
    </div>
    <div class="site-end">
      ${lang}
      ${discord}
    </div>
  </div>
</nav>

`;
  return html.slice(0, at) + bar + html.slice(at);
}
