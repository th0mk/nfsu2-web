// The game the page is built for (web/src/games/profile.ts, web/build.mjs --game):
//   node --test web/test/games.test.mjs
// 1. Each game's profile (web/src/games/<id>/index.ts) exports the same fields, its GAME is its folder's name, and
//    Most Wanted's site name is still the neutral stand-in.  (tsc checks the fields' types: npm run check.)
// 2. build.mjs on a made-up page, run from another folder: without --game the import path '@game' is ug2's profile
//    and the page goes to dist and dist-dev; with --game <id> it is that game's, into dist-<id> and dist-<id>-dev,
//    with the game's own texts over the shared ones in /lang/<language>.json; an unknown game, or a game's language
//    file without a shared one, stops the build.
import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { GAMES } from '../tools/i18n-keys.mjs';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

test('every game\'s profile has the same fields and its own id', async () => {
  const entry = GAMES.map(g => `export * as ${g} from './src/games/${g}/index.ts';`).join('\n');
  const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: WEB, loader: 'ts' }, bundle: true, format: 'esm', write: false,
    platform: 'neutral', loader: { '.txt': 'text', '.png': 'dataurl' } })).outputFiles[0].text;
  const P = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
  const fields = Object.keys(P.ug2).sort();
  assert.deepEqual(fields, ['APP_DEFAULTS', 'GAME', 'LANGUAGES', 'PROFILES_DIR', 'REGISTRY_DEFAULTS', 'SETTINGS_TEMPLATE', 'SITE_NAME', 'VAULT_PREFIX']);
  for (const g of GAMES) {
    assert.deepEqual(Object.keys(P[g]).sort(), fields, g);
    assert.equal(P[g].GAME, g);
    for (const f of fields) assert.ok(P[g][f] && (typeof P[g][f] === 'string' ? P[g][f].trim() : typeof P[g][f] === 'object'), `${g} ${f}`);
    assert.equal(P[g].LANGUAGES[0], 'english', g);
  }
  assert.equal(P.ug2.SITE_NAME, 'Underground 2 Revived');
  // the site's name is not chosen yet
  assert.equal(P.mw.SITE_NAME, 'Most Wanted');
});

test('build.mjs --game: the profile behind @game, the game\'s own folders and texts', t => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'build-games-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const w = (f, s) => { fs.mkdirSync(path.dirname(path.join(root, f)), { recursive: true }); fs.writeFileSync(path.join(root, f), typeof s === 'string' ? s : JSON.stringify(s)); };
  const read = f => fs.readFileSync(path.join(root, f), 'utf8');
  // build.mjs builds the folder it is in, wherever it is run from
  fs.copyFileSync(path.join(WEB, 'build.mjs'), path.join(root, 'build.mjs'));
  fs.symlinkSync(path.join(WEB, 'node_modules'), path.join(root, 'node_modules'));
  const build = (...args) => spawnSync(process.execPath, [path.join(root, 'build.mjs'), ...args], { cwd: os.tmpdir(), encoding: 'utf8', env: { ...process.env, NOMINIFY: '' } });
  w('index.html', '<!doctype html><title>made up</title>');
  w('src/static/manifest.json', '{}');
  w('src/main.ts', 'import { GAME } from \'@game\';\nconsole.log(`the page of ${GAME}`);\n');
  w('src/games/ug2/index.ts', 'export const GAME = \'ug2\';\n');
  w('src/games/one/index.ts', 'export const GAME = \'one\';\n');
  w('src/app/lang/french.json', { A: 'a partagé', B: 'b partagé' });
  w('src/games/one/lang/french.json', { B: 'b à la une', C: 'c à la une' });

  let r = build();
  assert.equal(r.status, 0, r.stderr);
  assert.match(read('dist/main.js'), /\bug2\b/);
  assert.doesNotMatch(read('dist/main.js'), /\bone\b/);
  assert.equal(read('dist/lang/french.json'), JSON.stringify({ A: 'a partagé', B: 'b partagé' }));
  assert.equal(read('dist/index.html'), '<!doctype html><title>made up</title>');
  assert.ok(fs.existsSync(path.join(root, 'dist-dev/test/dev.json')));
  assert.ok(fs.existsSync(path.join(root, 'dist-dev/main.js.map')));

  r = build('--game', 'one');
  assert.equal(r.status, 0, r.stderr);
  assert.match(read('dist-one/main.js'), /\bone\b/);
  assert.equal(read('dist-one/lang/french.json'), JSON.stringify({ A: 'a partagé', B: 'b à la une', C: 'c à la une' }));
  assert.ok(fs.existsSync(path.join(root, 'dist-one/manifest.json')));
  assert.ok(fs.existsSync(path.join(root, 'dist-one-dev/test/dev.json')));
  assert.ok(!fs.existsSync(path.join(root, 'dist-one/main.js.map')));
  assert.match(read('dist/main.js'), /\bug2\b/, 'the ug2 page stays as it was');

  r = build('--game', 'two');
  assert.equal(r.status, 2);
  assert.match(r.stderr, /no game "two"/);
  w('src/games/one/lang/english.json', { A: 'a' });
  r = build('--game', 'one');
  assert.equal(r.status, 2);
  assert.match(r.stderr, /src\/games\/one\/lang\/english\.json has no shared src\/app\/lang\/english\.json/);
});
