// Each game's page head, manifest and icons (web/build.mjs; Most Wanted's own: web/src/games/mw/head.html and
// web/src/games/mw/static):
//   node --test web/test/head.test.mjs
// The page built from this checkout's sources into a temporary folder (build.mjs builds the folder it is in) for
// each game, then:
// 1. Underground 2's index.html and app files are web/index.html and web/src/static byte for byte.
// 2. Most Wanted's index.html is Underground 2's with its own head from <title> to main.js's script: the body the same
//    but for its own edits (src/games/mw/body.json, as web/tools/i18n-keys.mjs pageHtml makes them), and in the head no value of Underground 2's (its name, texts, keywords, favicon) but those both pages share (the
//    AdSense account, PLAN F-216; the charset, viewport, colours, robots, the links' file names).
// 3. Most Wanted's head has its title, description, Open Graph and Twitter tags, canonical and og:url
//    https://nfsrevived.xyz/mw/, a favicon, the manifest and icon links, and JSON-LD that parses.
// 4. Its manifest: its name, start_url and scope './', no id (each base's id is its start_url: D-38), icons that are its
//    own PNGs of the sizes it says.
// 5. In Chrome (web/tools/browser.mjs), the page served under /mw/: the manifest's computed id is the base, as it is
//    https://nfsrevived.xyz/mw/ there.  Skipped without Chrome or Playwright's Chromium.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { pageHtml } from '../tools/i18n-keys.mjs';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'head-'));
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
const read = (g, f) => fs.readFileSync(path.join(dist(g), f));
const split = html => { const a = html.indexOf('<title>'), b = html.indexOf('<script type="module" src="main.js"></script>'); return [html.slice(0, a), html.slice(a, b), html.slice(b)]; };

/** the head's values: title, each meta's content, each link's href, the JSON-LD's strings */
function values(head) {
  const out = [];
  for (const m of head.matchAll(/<title>([^<]*)<\/title>/g)) out.push(['title', m[1]]);
  for (const m of head.matchAll(/<meta\s+(?:name|property)="([^"]+)"\s+content="([^"]*)"/g)) out.push([m[1], m[2]]);
  for (const m of head.matchAll(/<link\s+rel="([^"]+)"\s+href="([^"]*)"/g)) out.push([`link ${m[1]}`, m[2]]);
  const ld = /<script type="application\/ld\+json">([\s\S]*?)<\/script>/.exec(head);
  if (ld) (function walk(v, at) {
    if (typeof v === 'string') out.push([at, v]);
    else if (Array.isArray(v)) v.forEach((x, i) => walk(x, `${at}[${i}]`));
    else if (v && typeof v === 'object') for (const [k, x] of Object.entries(v)) walk(x, `${at}.${k}`);
  })(JSON.parse(ld[1]), 'ld');
  return out;
}
/** the values both pages may share: the same account, markup and file names, not Underground 2's identity */
const SHARED = new Set(['ca-pub-5817610257612647', 'index, follow', 'website', 'en_US', 'summary', 'manifest.json', 'icon-180.png', 'main.css',
  'https://schema.org', 'VideoGame', 'WebApplication', 'GameApplication', 'Racing', 'Web browser', 'Windows, macOS', 'SinglePlayer']);

test('Underground 2\'s page: index.html and its app files as they are', () => {
  assert.ok(read('ug2', 'index.html').equals(fs.readFileSync(path.join(WEB, 'index.html'))));
  for (const f of fs.readdirSync(path.join(WEB, 'src/static'))) assert.ok(read('ug2', f).equals(fs.readFileSync(path.join(WEB, 'src/static', f))), f);
});

test('Most Wanted\'s head: its own, no value of Underground 2\'s; the rest of the page the same', () => {
  const [ua, uh, ub] = split(read('ug2', 'index.html').toString()), [ma, mh, mb] = split(read('mw', 'index.html').toString());
  assert.equal(ma, ua, 'before <title>');
  assert.equal(mb, split(pageHtml('mw', root))[2], 'from main.js on: its body edits made');
  assert.notEqual(mb, ub, 'the body has edits of its own');
  assert.doesNotMatch(mh, /underground|nfsu2|ug2|\bu2\b/i);
  const theirs = values(uh).filter(([, v]) => !SHARED.has(v));
  assert.ok(theirs.length >= 12, `${theirs.length} values of Underground 2's head`);
  for (const [k, v] of theirs) assert.ok(!mh.includes(v), `Underground 2's ${k}: ${v}`);
  const mine = Object.fromEntries(values(mh));
  assert.equal(mine.title, 'Most Wanted Revived');
  for (const k of ['application-name', 'og:site_name', 'og:title', 'twitter:title', 'ld.name']) assert.equal(mine[k], 'Most Wanted Revived', k);
  for (const k of ['link canonical', 'og:url', 'ld.url']) assert.equal(mine[k], 'https://nfsrevived.xyz/mw/', k);
  for (const k of ['description', 'og:description', 'twitter:description', 'ld.description']) assert.match(mine[k], /^.*Need for Speed: Most Wanted \(2005\)/, k);
  for (const k of ['description', 'og:description', 'twitter:description']) assert.match(mine[k], /Fan project, not affiliated with Electronic Arts\.$/, k);
  assert.equal(mine['google-adsense-account'], 'ca-pub-5817610257612647');
  assert.match(mine['link icon'], /^data:image\/svg\+xml,/);
  assert.notEqual(mine['link icon'], Object.fromEntries(values(uh))['link icon']);
  assert.deepEqual([mine['link manifest'], mine['link apple-touch-icon'], mine['link stylesheet']], ['manifest.json', 'icon-180.png', 'main.css']);
});

/** a PNG's width and height */
const pngSize = b => { assert.equal(b.subarray(1, 4).toString(), 'PNG'); return [b.readUInt32BE(16), b.readUInt32BE(20)]; };

test('Most Wanted\'s manifest and icons: its own, start_url and scope ./, no id', () => {
  const man = JSON.parse(read('mw', 'manifest.json'));
  assert.equal(man.name, 'Most Wanted Revived');
  assert.equal(man.short_name, 'Most Wanted');
  assert.equal(man.start_url, './');
  assert.equal(man.scope, './');
  assert.ok(!('id' in man));
  assert.doesNotMatch(JSON.stringify(man), /underground|nfsu2|ug2/i);
  const icons = [...man.icons.map(i => i.src), 'icon-180.png'];
  assert.ok(man.icons.some(i => i.purpose === 'maskable'));
  for (const i of [...man.icons, { src: 'icon-180.png', sizes: '180x180' }]) {
    const b = read('mw', i.src);
    assert.equal(pngSize(b).join('x'), i.sizes, i.src);
    assert.ok(!b.equals(read('ug2', i.src)), `${i.src} is Underground 2's`);
  }
  // nothing of src/static's that Most Wanted lacks
  assert.deepEqual(fs.readdirSync(path.join(WEB, 'src/games/mw/static')).sort(), ['manifest.json', ...icons].sort().filter((x, i, a) => a.indexOf(x) === i));
});

test('Chrome: the manifest\'s id is the page\'s base', { timeout: 60_000 }, async t => {
  let chromium, pickBrowser;
  try { ({ chromium } = await import('playwright')); ({ pickBrowser } = await import('../tools/browser.mjs')); } catch { t.skip('no Playwright'); return; }
  const types = { '.html': 'text/html', '.json': 'application/manifest+json', '.png': 'image/png', '.css': 'text/css', '.js': 'text/javascript' };
  const srv = http.createServer((q, r) => {
    const p = decodeURIComponent(new URL(q.url, 'http://x').pathname);
    if (!p.startsWith('/mw/')) { r.writeHead(404).end(); return; }
    const f = path.join(dist('mw'), p.slice(4) || 'index.html');
    if (!f.startsWith(dist('mw')) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { r.writeHead(404).end(); return; }
    r.writeHead(200, { 'content-type': types[path.extname(f)] ?? 'application/octet-stream' }).end(fs.readFileSync(f));
  });
  await new Promise(res => srv.listen(+(process.env.NFSU2_TEST_HEAD_PORT || 0), '127.0.0.1', res));
  const port = srv.address().port;
  let browser;
  try {
    try { browser = await chromium.launch({ ...pickBrowser(), headless: true }); } catch (e) { t.skip(`no browser: ${e.message.split('\n')[0]}`); return; }
    const page = await browser.newPage();
    // the page's own scripts are not what this checks (they would ask a game server)
    await page.route(/\.js$/, r => r.fulfill({ status: 200, contentType: 'text/javascript', body: '' }));
    await page.goto(`http://127.0.0.1:${port}/mw/`);
    const cdp = await page.context().newCDPSession(page);
    const { appId } = await cdp.send('Page.getAppId');
    assert.equal(appId, `http://127.0.0.1:${port}/mw/`);
  } finally {
    await browser?.close();
    await new Promise(res => srv.close(res));
  }
});
