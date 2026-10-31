// The two games' sites (the site plan of 2026-10-31; web/src/games/site.mjs, web/src/site, web/build.mjs):
//   node --test web/test/site.test.mjs
// The pages built from this checkout's sources into a temporary folder, then:
// 1. The top bar (TRACKER M6-53): Most Wanted's page has it, its tab current and Underground 2's a link to
//    https://nfsu2.xyz/; Underground 2's page has it once its bar's own switch is on (site.mjs GAMES, not launch.ts
//    LAUNCHED), its tab current and Most Wanted's a link to https://nfsrevived.com/mw/, and otherwise is byte for byte
//    as it was (the bar out, the language picker and Discord back where they were); with the switch off its
//    index.html is web/index.html.
// 2. The site's page (https://nfsrevived.com/, which Most Wanted's server answers at its root): both games' links,
//    its canonical address, no inline script or style (the server's CSP allows none), every file it names beside it.
// 3. The site's page in each page language (site/index.<language>.html, which the server answers by ?lang= or the
//    browser's languages): its texts from the language's file, its lang, og:locale and address; every language named
//    for search engines; the server's table of the languages the same as the page's.
// 4. No page names an address of Most Wanted's but https://nfsrevived.com/mw/ (and the site's page's own).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { GAMES, SITE, SITE_LOCALES, hasBar, withSiteBar, sitePage, siteUrl } from '../src/games/site.mjs';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
// web/ in a copy of the repository: the page's changelog is the repository's CHANGELOG.md, beside web/
const top = fs.mkdtempSync(path.join(os.tmpdir(), 'site-')), root = path.join(top, 'web');
test.after(() => fs.rmSync(top, { recursive: true, force: true }));
fs.mkdirSync(root);
fs.copyFileSync(path.join(WEB, '..', 'CHANGELOG.md'), path.join(top, 'CHANGELOG.md'));
fs.copyFileSync(path.join(WEB, 'build.mjs'), path.join(root, 'build.mjs'));
fs.copyFileSync(path.join(WEB, 'index.html'), path.join(root, 'index.html'));
for (const d of ['src', 'node_modules']) fs.symlinkSync(path.join(WEB, d), path.join(root, d));
const built = {};
for (const g of ['ug2', 'mw']) {
  const r = spawnSync(process.execPath, [path.join(root, 'build.mjs'), '--game', g], { cwd: os.tmpdir(), encoding: 'utf8' });
  built[g] = r.status === 0 ? path.join(root, g === 'ug2' ? 'dist' : `dist-${g}`) : new Error(`${g}: ${r.stderr}`);
}
const dist = g => { if (built[g] instanceof Error) throw built[g]; return built[g]; };
const read = (g, f) => fs.readFileSync(path.join(dist(g), f), 'utf8');
const INDEX = fs.readFileSync(path.join(WEB, 'index.html'), 'utf8');
/** the bar's tabs: [name, href or null, current] */
const tabs = html => [...html.matchAll(/<a class="site-tab"( aria-current="page")?(?: href="([^"]+)")?>([^<]+)<\/a>/g)].map(m => [m[3], m[2] ?? null, !!m[1]]);
const bar = html => { const a = html.indexOf('<nav class="site-bar"'), b = html.indexOf('</nav>', a); return a < 0 ? null : html.slice(a, b + 6); };

test('the games and their addresses', () => {
  assert.deepEqual(GAMES.map(g => [g.id, g.url]), [['ug2', 'https://nfsu2.xyz/'], ['mw', 'https://nfsrevived.com/mw/']]);
  assert.equal(SITE, 'https://nfsrevived.com/');
  assert.ok(GAMES[1].url.startsWith(SITE));
});

test('Most Wanted\'s page: the bar, its tab current, Underground 2\'s a link to its site', () => {
  const html = read('mw', 'index.html');
  assert.deepEqual(tabs(html), [['Underground 2 Revived', 'https://nfsu2.xyz/', false], ['Most Wanted Revived', null, true]]);
  const b = bar(html);
  assert.ok(html.indexOf(b) < html.indexOf('<main class="app" id="app">'), 'above main#app, outside it');
  assert.match(b, /^<nav class="site-bar" id="site-bar" aria-label="Games" data-i18n-attr="aria-label">/);
  // the language picker (the one #lang-pick) and the Discord button in the bar, no longer in the header
  for (const part of ['<label class="lang-pick">', '<a class="btn" href="https://discord.gg/25zXmQ2Ua9"', 'id="lang-pick"']) {
    assert.equal(html.split(part).length, 2, part);
    assert.ok(b.includes(part), `${part} in the bar`);
  }
  const header = html.slice(html.indexOf('<header class="top">'), html.indexOf('</header>'));
  assert.match(header, /id="donate-btn"/);
  assert.doesNotMatch(header, /lang-pick|discord\.gg/);
});

test('Underground 2\'s page: as it was until its bar is on, then the bar and nothing else new', () => {
  // the bar's own switch (GAMES' bar); the launch switch (launch.ts LAUNCHED) has no part in it (games.test.mjs builds
  // a page with LAUNCHED on and the bar off)
  assert.deepEqual(GAMES.map(g => typeof g.bar), ['boolean', 'boolean']);
  assert.equal(hasBar('ug2'), GAMES[0].bar);
  assert.equal(hasBar('mw'), true);
  assert.equal(hasBar('nfs3'), false);
  if (!hasBar('ug2')) assert.equal(read('ug2', 'index.html'), INDEX, 'the switch off: index.html as it is');
  // the switch on (the page build.mjs makes then)
  const on = withSiteBar(INDEX, 'ug2');
  assert.deepEqual(tabs(on), [['Underground 2 Revived', null, true], ['Most Wanted Revived', 'https://nfsrevived.com/mw/', false]]);
  // the bar out and its two parts back where they were: index.html byte for byte
  const b = bar(on);
  const lang = /<label class="lang-pick">[^]*?<\/label>/.exec(b)[0], discord = /<a class="btn" href="https:\/\/discord\.gg\/[^]*?<\/a>/.exec(b)[0];
  // (main's Changelog button, between the two, stays in the top bar's end)
  const back = on.replace(b + '\n\n', '').replace('<div class="top-end">', `<div class="top-end">\n        ${lang}`)
    .replace(/(<button type="button" class="btn" id="changelog-btn"[^]*?<\/button>)/, `$1\n        ${discord}`);
  assert.equal(back, INDEX);
  assert.throws(() => withSiteBar(on, 'ug2'), /the bar already/);
  assert.throws(() => withSiteBar(INDEX.replace('<label class="lang-pick">', '<label>'), 'ug2'), /no language picker/);
  assert.throws(() => withSiteBar(INDEX, 'nfs3'), /no game nfs3/);
});

test('the site\'s page: both games, its address, no inline code, its files beside it', () => {
  const html = read('mw', 'site/index.html');
  const links = [...html.matchAll(/<a class="play" href="([^"]+)">/g)].map(m => m[1]);
  assert.deepEqual(links, ['https://nfsu2.xyz/', '/mw/']);
  assert.match(html, /<link rel="canonical" href="https:\/\/nfsrevived\.com\/">/);
  assert.match(html, /<meta property="og:url" content="https:\/\/nfsrevived\.com\/">/);
  assert.match(html, /<html lang="en">/);
  assert.match(html, /<title>NFS Revived: Underground 2 and Most Wanted in the browser<\/title>/);
  assert.doesNotMatch(html, /data-i18n/, 'the marks go');
  assert.doesNotMatch(html, /<script|<style|\sstyle=|\son[a-z]+=/i, 'the server\'s CSP: no inline script or style');
  for (const m of html.matchAll(/(?:href|src)="(\/mw\/site\/[^"]+)"/g)) {
    assert.ok(fs.existsSync(path.join(dist('mw'), m[1].slice('/mw/'.length))), m[1]);
  }
  assert.ok(read('mw', 'site/u2.png').length > 0 && read('mw', 'site/mw.png').length > 0);
  // only Most Wanted's server answers it: Underground 2's page folder has none
  assert.ok(!fs.existsSync(path.join(dist('ug2'), 'site')));
});

test('the site\'s page in each page language', () => {
  const langs = fs.readdirSync(path.join(dist('mw'), 'lang')).map(f => f.replace(/\.json$/, '')).sort();
  assert.ok(langs.length >= 16 && langs.includes('finnish'), langs.join());
  const english = read('mw', 'site/index.html');
  for (const l of langs) {
    const html = read('mw', `site/index.${l}.html`), texts = JSON.parse(read('mw', `lang/${l}.json`));
    assert.match(html, new RegExp(`<html lang="${SITE_LOCALES[l][0]}">`), l);
    assert.match(html, new RegExp(`<meta property="og:locale" content="${SITE_LOCALES[l][1]}">`), l);
    assert.ok(html.includes(`<link rel="canonical" href="${siteUrl(l)}">`) && html.includes(`<meta property="og:url" content="${siteUrl(l)}">`), l);
    // its texts, each the language's own (none left in English), and the rest of the page as the English one
    const esc = s => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
    for (const en of ['NFS Revived: Underground 2 and Most Wanted in the browser', 'Play Underground 2', 'Play Most Wanted',
      'Fan project. Not affiliated with or endorsed by Electronic Arts.',
      'Need for Speed: Underground 2 (2004) in the browser: the PC game\'s own code with WebGPU graphics, up to 120 fps widescreen, free roam with friends and Online.',
      'Need for Speed: Most Wanted (2005) in the browser: the PC game\'s own code (version 1.3) with WebGPU graphics and Web Audio sound.']) {
      assert.ok(typeof texts[en] === 'string' && texts[en] !== en, `${l}: ${en}`);
      assert.ok(html.includes(`>${esc(texts[en])}<`), `${l}: ${texts[en]}`);
      assert.ok(!html.includes(`>${esc(en)}<`), `${l}: ${en} in English`);
    }
    const description = 'Play Need for Speed: Underground 2 (2004) and Need for Speed: Most Wanted (2005) in Chrome and Firefox. Fan project, not affiliated with Electronic Arts.';
    assert.equal(html.split(`content="${esc(texts[description])}"`).length, 3, `${l}: the description and og:description`);
    const links = [...html.matchAll(/<a class="play" href="([^"]+)"/g)].map(m => m[1]);
    assert.deepEqual(links, ['https://nfsu2.xyz/', '/mw/'], l);
    assert.doesNotMatch(html, /<script|<style|\sstyle=|\son[a-z]+=|data-i18n/i, l);
    const skeleton = s => s.replace(/>[^<]*</g, '><').replace(/ (content|href|lang)="[^"]*"/g, '');
    assert.equal(skeleton(html), skeleton(english), `${l}: the same page`);
  }
  // every page names each language's address, and x-default the site's
  for (const html of [english, ...langs.map(l => read('mw', `site/index.${l}.html`))]) {
    const alt = [...html.matchAll(/<link rel="alternate" hreflang="([^"]+)" href="([^"]+)">/g)].map(m => [m[1], m[2]]);
    assert.deepEqual(alt, [...['english', ...langs].map(l => [SITE_LOCALES[l][0], siteUrl(l)]), ['x-default', SITE]]);
  }
  // the server's table (src/game.rs LANDING_LANGUAGES): the same languages and tags
  const rs = fs.readFileSync(path.join(WEB, 'server-rs/src/game.rs'), 'utf8');
  const table = /LANDING_LANGUAGES: \[\(&str, &str\); \d+\] = \[([^\]]+)\]/.exec(rs)[1];
  const pairs = Object.fromEntries([...table.matchAll(/\("(\w+)", "([\w-]+)"\)/g)].map(m => [m[1], m[2]]));
  assert.deepEqual(pairs, Object.fromEntries(Object.entries(SITE_LOCALES).filter(([l]) => l !== 'english').map(([l, [tag]]) => [l, tag])));
  assert.deepEqual(Object.keys(pairs).sort(), langs, 'a page in each language the server may pick');
  // sitePage: a text the language's file lacks stays English; a page without a part it changes is refused
  assert.match(sitePage(fs.readFileSync(path.join(WEB, 'src/site/index.html'), 'utf8'), 'german', {}), /<title>NFS Revived: Underground 2 and Most Wanted in the browser<\/title>/);
  assert.throws(() => sitePage('<html lang="en">', 'german'), /no <meta property="og:locale"/);
  assert.throws(() => sitePage(english, 'klingon'), /no language klingon/);
});

test('no page names an address of Most Wanted\'s but https://nfsrevived.com/mw/', () => {
  for (const g of ['ug2', 'mw']) {
    for (const f of fs.readdirSync(dist(g), { recursive: true }).map(String).filter(f => /\.(html|json|js|css)$/.test(f))) {
      const text = read(g, f);
      assert.doesNotMatch(text, /nfsrevived\.xyz/, `${g} ${f}`);
      const site = Object.keys(SITE_LOCALES).map(siteUrl);
      for (const m of text.matchAll(/https:\/\/nfsrevived\.com[^"'\s)<]*/g))
        assert.ok(['https://nfsrevived.com/mw/', ...site].includes(m[0]) && (m[0] === SITE || f.startsWith('site/') || m[0].endsWith('/mw/')), `${g} ${f}: ${m[0]}`);
    }
  }
});
