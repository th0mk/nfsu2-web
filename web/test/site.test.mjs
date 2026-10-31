// The two games' sites (the site plan of 2026-10-31; web/src/games/site.mjs, web/src/site, web/build.mjs):
//   node --test web/test/site.test.mjs
// The pages built from this checkout's sources into a temporary folder, then:
// 1. The top bar (TRACKER M6-53): Most Wanted's page has it, its tab current and Underground 2's a link to
//    https://nfsu2.xyz/; Underground 2's page has it once its launch switch is on, its tab current and Most Wanted's a
//    link to https://nfsrevived.com/mw/, and otherwise is byte for byte as it was (the bar out, the language picker
//    and Discord back where they were); with the switch off its index.html is web/index.html.
// 2. The site's page (https://nfsrevived.com/, which Most Wanted's server answers at its root): both games' links,
//    its canonical address, no inline script or style (the server's CSP allows none), every file it names beside it.
// 3. No page names Most Wanted's old address (nfsrevived.xyz) or another address for it than https://nfsrevived.com/mw/.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { GAMES, SITE, hasBar, withSiteBar } from '../src/games/site.mjs';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'site-'));
test.after(() => fs.rmSync(root, { recursive: true, force: true }));
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
const launchTs = g => fs.readFileSync(path.join(WEB, 'src/games', g, 'launch.ts'), 'utf8');
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
  assert.match(b, /^<nav class="site-bar" id="site-bar" aria-label="Games">/);
  // the language picker (the one #lang-pick) and the Discord button in the bar, no longer in the header
  for (const part of ['<label class="lang-pick">', '<a class="btn" href="https://discord.gg/25zXmQ2Ua9"', 'id="lang-pick"']) {
    assert.equal(html.split(part).length, 2, part);
    assert.ok(b.includes(part), `${part} in the bar`);
  }
  const header = html.slice(html.indexOf('<header class="top">'), html.indexOf('</header>'));
  assert.match(header, /id="donate-btn"/);
  assert.doesNotMatch(header, /lang-pick|discord\.gg/);
});

test('Underground 2\'s page: as it was until its launch switch is on, then the bar and nothing else new', () => {
  assert.equal(hasBar('ug2', launchTs('ug2')), /export const LAUNCHED = true;/.test(launchTs('ug2')));
  assert.equal(hasBar('mw', null), true);
  assert.equal(hasBar('ug2', null), false);
  assert.throws(() => hasBar('ug2', 'export const LAUNCHED = maybe;'), /no "export const LAUNCHED/);
  if (!hasBar('ug2', launchTs('ug2'))) assert.equal(read('ug2', 'index.html'), INDEX, 'the switch off: index.html as it is');
  // the switch on (the page build.mjs makes then)
  const on = withSiteBar(INDEX, 'ug2');
  assert.deepEqual(tabs(on), [['Underground 2 Revived', null, true], ['Most Wanted Revived', 'https://nfsrevived.com/mw/', false]]);
  // the bar out and its two parts back where they were: index.html byte for byte
  const b = bar(on);
  const lang = /<label class="lang-pick">[^]*?<\/label>/.exec(b)[0], discord = /<a class="btn" href="https:\/\/discord\.gg\/[^]*?<\/a>/.exec(b)[0];
  const back = on.replace(b + '\n\n', '').replace('<div class="top-end">', `<div class="top-end">\n        ${lang}\n        ${discord}`);
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
  assert.doesNotMatch(html, /<script|<style|\sstyle=|\son[a-z]+=/i, 'the server\'s CSP: no inline script or style');
  for (const m of html.matchAll(/(?:href|src)="(\/mw\/site\/[^"]+)"/g)) {
    assert.ok(fs.existsSync(path.join(dist('mw'), m[1].slice('/mw/'.length))), m[1]);
  }
  assert.ok(read('mw', 'site/u2.png').length > 0 && read('mw', 'site/mw.png').length > 0);
  // only Most Wanted's server answers it: Underground 2's page folder has none
  assert.ok(!fs.existsSync(path.join(dist('ug2'), 'site')));
});

test('no page names an address of Most Wanted\'s but https://nfsrevived.com/mw/', () => {
  for (const g of ['ug2', 'mw']) {
    for (const f of fs.readdirSync(dist(g), { recursive: true }).map(String).filter(f => /\.(html|json|js|css)$/.test(f))) {
      const text = read(g, f);
      assert.doesNotMatch(text, /nfsrevived\.xyz/, `${g} ${f}`);
      for (const m of text.matchAll(/https:\/\/nfsrevived\.com[^"'\s)<]*/g)) assert.ok(['https://nfsrevived.com/', 'https://nfsrevived.com/mw/'].includes(m[0]), `${g} ${f}: ${m[0]}`);
    }
  }
});
