// Bundles the web app (TypeScript) with esbuild:
//   web/dist      the player's page: what the server serves to everyone (and web/deploy/pack.sh ships)
//   web/dist-dev  the test pages (test/*) and every source map: what the server serves only with --dev
//                 (it never looks in this folder without it, so no spelling of a path reaches them)
// One build; the outputs are sorted into the two folders as they are written.
import * as esbuild from 'esbuild';
import fs from 'node:fs';
import path from 'node:path';
const watch = process.argv.includes('--watch');
const DIST = 'dist', DEV = 'dist-dev';
const opts = {
  entryPoints: {
    'main': 'src/main.ts',
    'host/game-worker': 'src/host/game-worker.ts',
    'host/io-worker': 'src/host/io-worker.ts',
    'host/predownload-worker': 'src/host/predownload-worker.ts',
    'gpu/png-worker': 'src/gpu/png-worker.ts',
    'net/net-worker': 'src/net/worker.ts',
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
const page = name => `<!doctype html><meta charset=utf-8><title>${name}</title><script type=module src="/test/${name}.js"></script>`;
for (const name of ['play', 'hostcheck', 'shadercheck', 'aicheck']) fs.writeFileSync(path.join(DEV, 'test', `${name}.html`), page(name));
// the player's page takes its address's test parameters only from a server that serves this (--dev: shell.ts)
fs.writeFileSync(path.join(DEV, 'test', 'dev.json'), '{"dev":true}\n');
if (watch) {
  const ctx = await esbuild.context({ ...opts, plugins: [{ name: 'emit', setup(b) { b.onEnd(r => { if (r.outputFiles) emit(r.outputFiles); }); } }] });
  await ctx.watch();
} else {
  const r = await esbuild.build(opts);
  emit(r.outputFiles);
}
