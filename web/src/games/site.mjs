// The two games' sites (the site plan of 2026-10-31): Underground 2 at https://nfsu2.xyz/, Most Wanted at
// https://nfsrevived.com/mw/, and https://nfsrevived.com/ a page that links both (src/site, which Most Wanted's
// server answers at its root).  web/build.mjs gives each game's page the top bar (TRACKER M6-53, design M6-52 option
// A): the two games as tabs, its own the current one and the other a link to that game's site, and the language
// picker and the Discord button, moved up from the header unchanged.  No imports: build.mjs and the tests take it as
// it is.

/** the site's page, which links both games */
export const SITE = 'https://nfsrevived.com/';
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
  const bar = `<nav class="site-bar" id="site-bar" aria-label="Games">
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
