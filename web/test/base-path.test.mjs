// The page under a base path (web/src/base.ts): one build served at the root ('/') or under one (/u2/, /mw/) asks for
// everything under that base, and for nothing outside it.
//   node --test web/test/base-path.test.mjs
// 1. baseOf: the base of main.js and of a test page's script, at the root and under a base; '/' under node.
// 2. blockRuns: the manifest's blocks.url, resolved against the manifest's address, is taken only on the same server
//    and under the manifest's folder (the base).
// 3. The page as web/build.mjs builds it (into a scratch folder, for each game): no script of it holds an address of
//    the server's root ('/players', '/game.json', '/?x', '/' + x, `/${x}`, ...), only the game's own file paths (/save,
//    /data, which never leave the browser, and never with more after them), and no markup or style it builds names
//    one (src="/x", url(/x)); its stylesheets neither; index.html and the test pages name their files relatively;
//    manifest.json too, with no id.
// 4. build.mjs refuses a bundle base.ts would not place.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { GAMES } from '../tools/i18n-keys.mjs';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const entry = `export * from './src/base.ts'; export { blockRuns, BLOCK } from './src/host/datacache.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: WEB, loader: 'ts' }, bundle: true, format: 'esm', write: false,
  platform: 'neutral' })).outputFiles[0].text;
const B = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

test('baseOf: main.js sits at the base, a test page\'s script one folder below it', () => {
  const cases = [
    ['https://nfsu2.xyz/main.js', '/', 'https://nfsu2.xyz'],
    ['http://127.0.0.1:8615/main.js', '/', 'http://127.0.0.1:8615'],
    ['https://both.example/u2/main.js', '/u2/', 'https://both.example'],
    ['https://both.example/mw/main.js', '/mw/', 'https://both.example'],
    ['http://localhost:8615/test/play.js', '/', 'http://localhost:8615'],
    ['https://both.example/u2/test/play.js', '/u2/', 'https://both.example'],
    ['https://both.example/mw/test/hostcheck.js', '/mw/', 'https://both.example'],
    // a worker that bundles it (through i18n.ts): one folder below too
    ['https://both.example/u2/host/predownload-worker.js', '/u2/', 'https://both.example'],
    // a base named like a bundle's folder
    ['https://example.test/test/main.js', '/test/', 'https://example.test'],
    ['https://example.test/test/test/play.js', '/test/', 'https://example.test'],
    // node: the tests' bundles
    ['data:text/javascript;base64,AAAA', '/', ''],
    ['file:///home/x/web/main.js', '/', ''],
    ['not a url', '/', ''],
  ];
  for (const [u, base, origin] of cases) assert.deepEqual(B.baseOf(u), { base, origin }, u);
  // under node: paths as today's, at the root
  assert.equal(B.BASE, '/');
  assert.equal(B.at('players'), '/players');
  assert.equal(B.abs('data-manifest'), '/data-manifest');
});

test('blockRuns: the same server, under the manifest\'s folder, resolved against the manifest', () => {
  const runs = (url, manifest) => B.blockRuns({ files: [], blocks: { url, size: B.BLOCK, max: 8 } }, manifest);
  const ROOT = 'https://nfsu2.xyz/data-manifest', U2 = 'https://both.example/u2/data-manifest';
  // today's server at the root: as before
  assert.deepEqual(runs('/data-blocks/v2/', ROOT), { url: 'https://nfsu2.xyz/data-blocks/v2/', max: 8 });
  // a server under a base: its own path, or one relative to the manifest
  assert.deepEqual(runs('/u2/data-blocks/v2/', U2), { url: 'https://both.example/u2/data-blocks/v2/', max: 8 });
  assert.deepEqual(runs('data-blocks/v2/', U2), { url: 'https://both.example/u2/data-blocks/v2/', max: 8 });
  // outside the base (the root's, the other game's), another origin, a query the file's path would follow: none,
  // so byte ranges of data/ under the base
  for (const u of ['/data-blocks/v2/', '/mw/data-blocks/v2/', '../data-blocks/v2/', '/u2x/data-blocks/', 'https://evil.example/u2/data-blocks/v2/',
    '//evil.example/u2/', 'http://both.example/u2/data-blocks/v2/', '/u2/data-blocks/v2/?x=1', '/u2/data-blocks/v2/#x', 'http://[', 7])
    assert.equal(runs(u, U2), null, String(u));
  // not this cache's block size, no runs at all
  assert.equal(B.blockRuns({ files: [], blocks: { url: '/u2/data-blocks/v2/', size: 65536, max: 8 } }, U2), null);
  assert.equal(B.blockRuns({ files: [], blocks: { url: '/u2/data-blocks/v2/', size: B.BLOCK, max: 0 } }, U2), null);
  assert.equal(B.blockRuns({ files: [] }, U2), null);
  // the manifest's own address must be absolute: a worker resolves nothing against its own
  assert.throws(() => runs('data-blocks/v2/', '/u2/data-manifest'));
});

// ---- the built page ------------------------------------------------------------------------------------------------
// Strings that start like an address of the server's root but are none: the game's own file system (the wasm
// program's paths, runtime/web), which the page hands to the game, and a texture format's cache key (gpu/encoder.ts: '/o' opaque, '/ms' multisampled).
const NOT_ADDRESSES = new Set(['/save', '/data', '/dev', '/save/registry.txt', '/save/autotest-script.txt', '/data/SPEED2.EXE', '/o', '/ms']);
// Offline mode's service worker still knows the root only: shell.ts registers it only where the base is '/'.
const ROOT_ONLY = new Set(['sw.js']);

// A path at the server's root: '/' and a name's first character, or the root's query or fragment ('/?x', '/#x')
const ROOT_PATH = /^\/[A-Za-z0-9_.~%?#-]/;
// an address at the root in markup or style: an attribute's value, a CSS url() or @import (escapes kept: the text of a
// script's literal); never '//' (another server's)
const ROOT_ATTR = /(?<![\w-])(?:src|href|action|formaction|poster|srcset)\s*=\s*(\\?["']?)\/(?!\/)[^"'\s>\\]*\1/gi;
const ROOT_URL = /(?:url\(\s*|@import\s+)(\\?["']?)\/(?!\/)[^"')\s;\\]*\1\)?/gi;

const BEFORE_EXPR = new Set(['return', 'typeof', 'instanceof', 'in', 'of', 'new', 'delete', 'void', 'throw', 'case', 'do', 'else', 'yield', 'await']);
/** The text of a script's string and template literals, as written (escapes kept), each part with
 *  first: it starts its literal (a string, a template's head: a template's middle and tail parts follow an
 *  expression, so they never start an address), and
 *  then: the start of a value whose rest an expression gives (a + after the string, the head's ${), where no + or +=
 *  comes before the literal (that is the middle of a join: a + '/' + b).
 *  A scanner, not a parser (the TypeScript here has no JavaScript API): a '/' starts a regular expression where no
 *  value comes before it, as in esbuild's output; it stops at a string, template, comment or regular expression that
 *  does not end, which is where that rule would have lost its way. */
function literals(code, name) {
  const out = [];
  const fail = (what, at) => { throw new Error(`${name}: ${what} at ${at} (${JSON.stringify(code.slice(Math.max(0, at - 40), at + 40))})`); };
  // the code's first character after `at` that is not white space; whether a + or += comes right before `at`
  const after = at => { while (at < code.length && /\s/.test(code[at])) at++; return code[at]; };
  const joins = at => { at--; while (at >= 0 && /\s/.test(code[at])) at--; return code[at] === '+' || (code[at] === '=' && code[at - 1] === '+'); };
  let i = 0, regexOk = true, depth = 0;
  const subs = [];                                  // the brace depth each open template substitution started at
  const template = (first, joined) => {
    let s = '';
    for (;;) {
      if (i >= code.length) fail('a template that does not end', i);
      const c = code[i];
      if (c === '\\') { s += code.slice(i, i + 2); i += 2; continue; }
      if (c === '`') { i++; out.push({ s, first, then: false }); regexOk = false; return; }
      if (c === '$' && code[i + 1] === '{') { i += 2; out.push({ s, first, then: first && !joined }); subs.push(depth); regexOk = true; return; }
      s += c; i++;
    }
  };
  while (i < code.length) {
    const c = code[i];
    if (c === '"' || c === "'") {
      let s = '', j = i + 1;
      for (; j < code.length && code[j] !== c; j++) {
        if (code[j] === '\\') { s += code.slice(j, j + 2); j++; } else if (code[j] === '\n') fail('a string that does not end', i); else s += code[j];
      }
      if (j >= code.length) fail('a string that does not end', i);
      out.push({ s, first: true, then: after(j + 1) === '+' && !joins(i) }); i = j + 1; regexOk = false; continue;
    }
    if (c === '`') { const joined = joins(i); i++; template(true, joined); continue; }
    if (c === '/' && code[i + 1] === '/') { const e = code.indexOf('\n', i); i = e < 0 ? code.length : e; continue; }
    if (c === '/' && code[i + 1] === '*') { const e = code.indexOf('*/', i + 2); if (e < 0) fail('a comment that does not end', i); i = e + 2; continue; }
    if (c === '/' && regexOk) {
      let j = i + 1, cls = false;
      for (; j < code.length; j++) {
        const d = code[j];
        if (d === '\\') { j++; continue; }
        if (d === '\n') fail('a regular expression that does not end', i);
        if (cls) { if (d === ']') cls = false; } else if (d === '[') cls = true; else if (d === '/') break;
      }
      if (j >= code.length) fail('a regular expression that does not end', i);
      i = j + 1; while (i < code.length && /[a-z]/.test(code[i])) i++;
      regexOk = false; continue;
    }
    if (/[A-Za-z0-9_$\u0080-￿]/.test(c)) {
      let j = i; while (j < code.length && /[A-Za-z0-9_$\u0080-￿]/.test(code[j])) j++;
      regexOk = BEFORE_EXPR.has(code.slice(i, j)); i = j; continue;
    }
    if (/\s/.test(c)) { i++; continue; }
    if (c === '{') { depth++; regexOk = true; i++; continue; }
    if (c === '}') {
      if (subs.length && subs[subs.length - 1] === depth) { subs.pop(); i++; template(false, false); continue; }
      depth--; regexOk = false; i++; continue;
    }
    regexOk = c !== ')' && c !== ']'; i++;
  }
  if (subs.length) fail('a template substitution that does not end', i);
  return out;
}

/** What of a built page (dist, its dev folder) names an address at the server's root. */
function audit(dist, dev) {
  const bad = [];
  const files = (d, ext) => fs.readdirSync(d, { recursive: true }).map(String).filter(f => f.endsWith(ext));
  for (const [dir, f] of [...files(dist, '.js').map(f => [dist, f]), ...files(path.join(dev, 'test'), '.js').map(f => [dev, path.join('test', f)])]) {
    if (ROOT_ONLY.has(f)) continue;
    for (const { s, first, then } of literals(fs.readFileSync(path.join(dir, f), 'utf8'), f)) {
      if (first && ROOT_PATH.test(s) && !NOT_ADDRESSES.has(s)) bad.push(`${f}: ${JSON.stringify(s)}`);
      // a root path whose rest an expression gives ('/' + x, `/${x}`), the game's own paths too: '/data' + x is the
      // server's /data/ as much as the game's folder
      else if (first && then && (s === '/' || NOT_ADDRESSES.has(s))) bad.push(`${f}: ${JSON.stringify(s)} + …`);
      // markup or style the script builds
      else for (const m of [...s.matchAll(ROOT_ATTR), ...s.matchAll(ROOT_URL)]) bad.push(`${f}: ${m[0]}`);
    }
  }
  for (const f of files(dist, '.css')) for (const m of fs.readFileSync(path.join(dist, f), 'utf8').matchAll(ROOT_URL)) bad.push(`${f}: ${m[0]}`);
  const pages = [[dist, 'index.html'], ...fs.readdirSync(path.join(dev, 'test')).filter(f => f.endsWith('.html')).map(f => [dev, path.join('test', f)])];
  for (const [dir, f] of pages) {
    const html = fs.readFileSync(path.join(dir, f), 'utf8');
    for (const m of html.matchAll(/\s(?:src|href|content|action|formaction|poster|srcset)\s*=\s*(["']?)([^"'\s>]*)\1/gi))
      if (/^\/(?!\/)/.test(m[2])) bad.push(`${f}: ${m[0].trim()}`);
    for (const m of html.matchAll(ROOT_URL)) bad.push(`${f}: ${m[0]}`);
  }
  const man = JSON.parse(fs.readFileSync(path.join(dist, 'manifest.json'), 'utf8'));
  const strings = [];
  const each = v => { if (typeof v === 'string') strings.push(v); else if (v && typeof v === 'object') Object.values(v).forEach(each); };
  each(man);
  for (const s of strings) if (s.startsWith('/')) bad.push(`manifest.json: ${JSON.stringify(s)}`);
  if ('id' in man) bad.push(`manifest.json: an id (${JSON.stringify(man.id)}): each base's comes from its start_url`);
  if (man.start_url !== './' || man.scope !== './') bad.push(`manifest.json: start_url ${man.start_url}, scope ${man.scope}: not ./`);
  return bad;
}

test('the built page names no address at the server\'s root: scripts, stylesheets, pages, manifest.json (each game)', { timeout: 120_000 }, t => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'base-path-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  // build.mjs builds the folder it is in: this page's sources, linked
  fs.copyFileSync(path.join(WEB, 'build.mjs'), path.join(root, 'build.mjs'));
  fs.copyFileSync(path.join(WEB, 'index.html'), path.join(root, 'index.html'));
  for (const d of ['src', 'node_modules']) fs.symlinkSync(path.join(WEB, d), path.join(root, d));
  for (const g of GAMES) {
    const r = spawnSync(process.execPath, [path.join(root, 'build.mjs'), '--game', g], { cwd: os.tmpdir(), encoding: 'utf8' });
    assert.equal(r.status, 0, `${g}: ${r.stderr}`);
    const dist = path.join(root, g === 'ug2' ? 'dist' : `dist-${g}`), dev = path.join(root, g === 'ug2' ? 'dist-dev' : `dist-${g}-dev`);
    const bad = audit(dist, dev);
    console.log(`${g}: ${bad.length ? bad.join('\n  ') : 'no address at the root'}`);
    assert.deepEqual(bad, [], g);
  }
  // the audit finds what it looks for: the literals of the page before it had a base; the root's query and fragment; a
  // root path or a game's path that an expression goes on with; markup and style with a root address, built in a
  // script, in a stylesheet, in a page.  And not a path's joins, another server's address, or one from an expression.
  fs.writeFileSync(path.join(root, 'dist', 'probe.js'), 'fetch("/players");fetch(`/lang/${x}.json`);new Worker(new URL("/host/io-worker.js",import.meta.url));' +
    'a.split("/");b=c+"/"+d;b+="/"+c;d=`${e}/f`;g="/save";h=/"\\/x"/.test(i)?1/j:k;l=`${m?`/n`:"o"}`;q=r?"/":"s";t=u+`/${v}`;' +
    'fetch("/?a");location.href="/#b";c="/"+d;e=`/${f}`;g="/data"+h;i=`/save${j}`;' +
    'k.innerHTML=\'<img src="/l.png">\';m.innerHTML=`<a href="${n}">`;o.innerHTML=`<i>${p}</i><img src="/q.png">`;' +
    'r.style.background="url(/s.png)";t.innerHTML="<img src=\\"//cdn.example/u.png\\">";');
  fs.writeFileSync(path.join(root, 'dist', 'probe.css'), 'a{background:url(/bg.png)}b{background:url(data:x)}@import "/x.css";' +
    'c{background:url( "//cdn.example/x.png")}');
  fs.writeFileSync(path.join(root, 'dist', 'index.html'), '<link rel="manifest" href="/manifest.json"><script type="module" src="/main.js"></script><a href="https://x.example/">' +
    '<div style="background:url(\'/d.png\')"></div>');
  assert.deepEqual(audit(path.join(root, 'dist'), path.join(root, 'dist-dev')), [
    'probe.js: "/players"', 'probe.js: "/lang/"', 'probe.js: "/host/io-worker.js"', 'probe.js: "/n"',
    'probe.js: "/?a"', 'probe.js: "/#b"', 'probe.js: "/" + …', 'probe.js: "/" + …', 'probe.js: "/data" + …', 'probe.js: "/save" + …',
    'probe.js: src="/l.png"', 'probe.js: src="/q.png"', 'probe.js: url(/s.png)',
    'probe.css: url(/bg.png)', 'probe.css: @import "/x.css"',
    'index.html: href="/manifest.json"', 'index.html: src="/main.js"', 'index.html: url(\'/d.png\')',
  ]);
  assert.throws(() => literals('a="b', 'x.js'), /a string that does not end/);
});

// baseOf knows the page's main.js by its name alone: a main.js in a folder would take that folder for the base, so
// build.mjs refuses one, and any bundle neither at the base nor one folder below it (before it builds anything)
test('build.mjs refuses a bundle base.ts would not place', t => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'base-path-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  for (const d of ['src', 'node_modules']) fs.symlinkSync(path.join(WEB, d), path.join(root, d));
  const build = fs.readFileSync(path.join(WEB, 'build.mjs'), 'utf8');
  const main = "'main': 'src/main.ts',";
  assert.ok(build.includes(main));
  for (const name of ['host/main', 'test/main', 'x', 'a/b/c', 'Host/x']) {
    fs.writeFileSync(path.join(root, 'build.mjs'), build.replace(main, `${main} '${name}': 'src/main.ts',`));
    const r = spawnSync(process.execPath, [path.join(root, 'build.mjs')], { cwd: os.tmpdir(), encoding: 'utf8' });
    assert.equal(r.status, 2, `${name}: ${r.stderr}`);
    assert.match(r.stderr, new RegExp(`the bundle ${name} would not find the page's base`), name);
    assert.ok(!fs.existsSync(path.join(root, 'dist')), `${name}: nothing built`);
  }
});
