// Bundles the web app (TypeScript) with esbuild:
//   web/dist      the player's page: what the server serves to everyone (and web/deploy/pack.sh ships)
//   web/dist-dev  the test pages (test/*) and every source map: what the server serves only with --dev
//                 (it never looks in this folder without it, so no spelling of a path reaches them)
// One build; the outputs are sorted into the two folders as they are written.
// --game <id>: the page for that game (src/games/<id>/index.ts behind the import path '@game', src/games/profile.ts),
// into web/dist-<id> and web/dist-<id>-dev; ug2, the default, keeps web/dist and web/dist-dev.
import * as esbuild from 'esbuild';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
// the paths below are web/'s, also when this runs from elsewhere (node web/build.mjs)
process.chdir(path.dirname(fileURLToPath(import.meta.url)));
const watch = process.argv.includes('--watch');
const at = process.argv.indexOf('--game');
const GAME = at < 0 ? 'ug2' : process.argv[at + 1] ?? '';
if (!/^[a-z0-9]+$/.test(GAME) || !fs.existsSync(path.join('src/games', GAME, 'index.ts'))) {
  console.error(`build.mjs: no game "${GAME}" (src/games/<id>/index.ts)`);
  process.exit(2);
}
const DIST = GAME === 'ug2' ? 'dist' : `dist-${GAME}`, DEV = GAME === 'ug2' ? 'dist-dev' : `dist-${GAME}-dev`;
// the start page's languages: the shared texts (src/app/lang/<language>.json) with the game's own over them
// (src/games/<id>/lang/<language>.json); web/tools/i18n-keys.mjs checks both
const LANG = 'src/app/lang', OWN_LANG = path.join('src/games', GAME, 'lang');
for (const f of fs.existsSync(OWN_LANG) ? fs.readdirSync(OWN_LANG) : []) if (f.endsWith('.json') && !fs.existsSync(path.join(LANG, f))) {
  console.error(`build.mjs: ${OWN_LANG}/${f} has no shared ${LANG}/${f}`);
  process.exit(2);
}
const opts = {
  entryPoints: {
    'main': 'src/main.ts',
    'host/game-worker': 'src/host/game-worker.ts',
    'host/io-worker': 'src/host/io-worker.ts',
    'host/predownload-worker': 'src/host/predownload-worker.ts',
    'gpu/png-worker': 'src/gpu/png-worker.ts',
    'sw': 'src/sw.ts',
    'test/hostcheck': 'src/test/hostcheck.ts',
    'test/play': 'src/test/play.ts',
    'test/shadercheck': 'src/test/shadercheck.ts',
    'test/aicheck': 'src/test/aicheck.ts',
    'audio/worklet': 'src/audio/worklet.ts',
  },
  outdir: DIST, bundle: true, format: 'esm', target: ['chrome140', 'firefox145'], sourcemap: true, minify: !watch && !process.env.NOMINIFY,
  logLevel: 'info', write: false, absWorkingDir: process.cwd(),
  alias: { '@game': `./src/games/${GAME}/index.ts` },
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
// the start page's languages (LANG and OWN_LANG, i18n.ts): /lang/<language>.json, fetched once a player picks one
fs.rmSync(path.join(DIST, 'lang'), { recursive: true, force: true });
fs.mkdirSync(path.join(DIST, 'lang'));
const texts = f => fs.existsSync(f) ? JSON.parse(fs.readFileSync(f, 'utf8')) : {};
for (const f of fs.readdirSync(LANG)) if (f.endsWith('.json'))
  fs.writeFileSync(path.join(DIST, 'lang', f), JSON.stringify({ ...texts(path.join(LANG, f)), ...texts(path.join(OWN_LANG, f)) }));
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
