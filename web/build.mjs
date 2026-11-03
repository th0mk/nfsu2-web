// Bundles the web app (TypeScript) with esbuild:
//   web/dist      the player's page: what the server serves to everyone (and web/deploy/pack.sh ships)
//   web/dist-dev  the test pages (test/*) and every source map: what the server serves only with --dev
//                 (it never looks in this folder without it, so no spelling of a path reaches them)
// One build; the outputs are sorted into the two folders as they are written.
import * as esbuild from 'esbuild';
import fs from 'node:fs';
import path from 'node:path';
import { bootTexts } from './tools/i18n-keys.mjs';
const watch = process.argv.includes('--watch');
const DIST = 'dist', DEV = 'dist-dev';
const opts = {
  entryPoints: {
    'main': 'src/main.ts',
    'host/game-worker': 'src/host/game-worker.ts',
    'host/io-worker': 'src/host/io-worker.ts',
    'host/predownload-worker': 'src/host/predownload-worker.ts',
    'gpu/png-worker': 'src/gpu/png-worker.ts',
    'net/worker': 'src/net/worker.ts',
    'sw': 'src/sw.ts',
    'test/hostcheck': 'src/test/hostcheck.ts',
    'test/play': 'src/test/play.ts',
    'test/shadercheck': 'src/test/shadercheck.ts',
    'test/aicheck': 'src/test/aicheck.ts',
    'audio/worklet': 'src/audio/worklet.ts',
  },
  outdir: DIST, bundle: true, format: 'esm', target: ['chrome140', 'firefox145'], sourcemap: true, minify: !watch && !process.env.NOMINIFY,
  logLevel: 'info', write: false,
  // registry-defaults.txt and similar seeds go into the page as strings, the stylesheet's pictures as data: URLs
  loader: { '.txt': 'text', '.png': 'dataurl' },
};
for (const [k, v] of Object.entries(opts.entryPoints)) if (!fs.existsSync(v)) delete opts.entryPoints[k];
// boot.js: what the start page says when main.js never starts (src/boot.ts), a classic script that runs before it,
// with its few texts in every language (it cannot fetch a language file)
const bootOpts = { ...opts, entryPoints: { boot: 'src/boot.ts' }, format: 'iife', define: { BOOT_TEXTS: JSON.stringify(bootTexts()) } };

// test pages and source maps go to dist-dev (same relative path: /test/x.js, /main.js.map); the rest to dist
const devOnly = rel => rel.startsWith('test/') || rel.endsWith('.map');
function emit(files) {
  for (const f of files) {
    const rel = path.relative(path.resolve(DIST), f.path).split(path.sep).join('/');
    const out = path.join(devOnly(rel) ? DEV : DIST, rel);
    fs.mkdirSync(path.dirname(out), { recursive: true });
    fs.writeFileSync(out, f.contents);
  }
}
// an older build wrote test pages and maps into dist: never leave them there
fs.rmSync(path.join(DIST, 'test'), { recursive: true, force: true });
for (const f of fs.existsSync(DIST) ? fs.readdirSync(DIST, { recursive: true }) : []) if (String(f).endsWith('.map')) fs.rmSync(path.join(DIST, String(f)));
fs.mkdirSync(path.join(DEV, 'test'), { recursive: true });
fs.mkdirSync(DIST, { recursive: true });
// the start page (its script and stylesheet are main.js / main.css), and its files as they are (src/static: the
// app's manifest and icons, for installing it on a phone's home screen or as a desktop app)
fs.copyFileSync('index.html', path.join(DIST, 'index.html'));
for (const f of fs.readdirSync('src/static')) fs.copyFileSync(path.join('src/static', f), path.join(DIST, f));
// the start page's languages (src/app/lang, i18n.ts): /lang/<language>.json, fetched once a player picks one
fs.rmSync(path.join(DIST, 'lang'), { recursive: true, force: true });
fs.mkdirSync(path.join(DIST, 'lang'));
for (const f of fs.readdirSync('src/app/lang')) if (f.endsWith('.json'))
  fs.writeFileSync(path.join(DIST, 'lang', f), JSON.stringify(JSON.parse(fs.readFileSync(path.join('src/app/lang', f), 'utf8'))));
// the changelog (CHANGELOG.md at the top of the repo): /changelog.json, fetched when a player opens it (changelog.ts):
// [{ date, commit, changes: [[area, text]] }], newest first, one release a day.  A line that looks like a release or a
// change but is neither stops the build, so a typo never reaches the page as a missing release; so does a day twice
fs.writeFileSync(path.join(DIST, 'changelog.json'), JSON.stringify(changelog(fs.readFileSync('../CHANGELOG.md', 'utf8'))));
function changelog(md) {
  const out = [];
  md.split(/\r?\n/).forEach((line, i) => {
    const bad = what => { throw new Error(`CHANGELOG.md:${i + 1}: ${what}: ${line}`); };
    let m;
    if ((m = /^## (\d{4}-\d{2}-\d{2}) \(([0-9a-f]{7,40})\)$/.exec(line))) {
      if (out.some(r => r.date === m[1])) bad("a day's releases share one section");
      out.push({ date: m[1], commit: m[2], changes: [] });
    }
    else if (line.startsWith('#') && i > 0) bad('a release is "## <yyyy-mm-dd> (<commit>)"');
    else if ((m = /^- (?:\*\*([^*]+):\*\* )?(\S.*)$/.exec(line))) (out.at(-1) ?? bad('a change before the first release')).changes.push([m[1] ?? '', m[2]]);
    else if (/^\s+\S/.test(line) && out.at(-1)?.changes.length) out.at(-1).changes.at(-1)[1] += ' ' + line.trim();
    else if (/^\s*[-*+]/.test(line)) bad('a change is "- **Area:** what changed"');
  });
  return out;
}
const page =name => `<!doctype html><meta charset=utf-8><title>${name}</title><script type=module src="/test/${name}.js"></script>`;
for (const name of ['play', 'hostcheck', 'shadercheck', 'aicheck']) fs.writeFileSync(path.join(DEV, 'test', `${name}.html`), page(name));
// the player's page takes its address's test parameters only from a server that serves this (--dev: shell.ts)
fs.writeFileSync(path.join(DEV, 'test', 'dev.json'), '{"dev":true}\n');
if (watch) {
  for (const o of [opts, bootOpts]) {
    const ctx = await esbuild.context({ ...o, plugins: [{ name: 'emit', setup(b) { b.onEnd(r => { if (r.outputFiles) emit(r.outputFiles); }); } }] });
    await ctx.watch();
  }
} else {
  for (const o of [opts, bootOpts]) emit((await esbuild.build(o)).outputFiles);
}
