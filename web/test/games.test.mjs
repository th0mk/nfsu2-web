// The game the page is built for (web/src/games/profile.ts, web/build.mjs --game):
//   node --test web/test/games.test.mjs
// 1. Each game's profile (web/src/games/<id>/index.ts) exports the same fields, its GAME is its folder's name, and
//    Most Wanted's site name is PLAN D-31's.  (tsc checks the fields' types: npm run check.)
// 2. build.mjs on a made-up page, run from another folder: without --game the import path '@game' is ug2's profile
//    and the page goes to dist and dist-dev; with --game <id> it is that game's, into dist-<id> and dist-<id>-dev,
//    with the game's own texts over the shared ones in /lang/<language>.json; an unknown game, or a game's language
//    file without a shared one, stops the build; a game's body edits (src/games/<id>/body.json) are made in its
//    index.html, as i18n-keys.mjs reads it, and one that does not fit stops the build.
// 3. Most Wanted's body edits fit today's index.html: its name, its keys, no Underground 2 panel.
// 4. Each game's marks in a backup (src/games/save-marks.ts: its profiles' folder, how its settings.ini starts, its
//    registry.txt's first-start value) are its own files' and no other game's.
import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { catalog, GAMES, pageHtml } from '../tools/i18n-keys.mjs';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

test('every game\'s profile has the same fields and its own id', async () => {
  const entry = GAMES.map(g => `export * as ${g} from './src/games/${g}/index.ts';`).join('\n');
  const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: WEB, loader: 'ts' }, bundle: true, format: 'esm', write: false,
    platform: 'neutral', loader: { '.txt': 'text', '.png': 'dataurl' } })).outputFiles[0].text;
  const P = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
  const fields = Object.keys(P.ug2).sort();
  assert.deepEqual(fields, ['APP_DEFAULTS', 'EXE', 'FILE_PREFIX', 'GAME', 'KEYPHRASE_WORD', 'LANGUAGES', 'LAUNCHED', 'LOG_MARK', 'MOVIE_SIZE',
    'PROFILES_DIR', 'REGISTRY_DEFAULTS', 'SETTINGS_TEMPLATE', 'SITE_NAME', 'STORAGE', 'VAULT_AAD', 'VAULT_DERIVE_INFO', 'VAULT_PREFIX']);
  for (const g of GAMES) {
    assert.deepEqual(Object.keys(P[g]).sort(), fields, g);
    assert.equal(P[g].GAME, g);
    for (const f of fields)
      assert.ok(typeof P[g][f] === 'boolean' || (f === 'VAULT_DERIVE_INFO' && P[g][f] === null) ||
        P[g][f] && (typeof P[g][f] === 'string' ? P[g][f].trim() : typeof P[g][f] === 'object'), `${g} ${f}`);
    assert.equal(P[g].LANGUAGES[0], 'english', g);
  }
  assert.equal(P.ug2.SITE_NAME, 'Underground 2 Revived');
  // the movies' picture size the page asks the browser's AV1 decoder about (host/movie.ts movieCodec)
  assert.deepEqual([P.ug2.MOVIE_SIZE, P.mw.MOVIE_SIZE], [{ w: 768, h: 512 }, { w: 1024, h: 512 }]);
  // the sites' names, the top bar's tabs (PLAN D-31)
  assert.equal(P.mw.SITE_NAME, 'Most Wanted Revived');
  // what a run takes from its game (src/games/<id>/run.ts): Underground 2's as they always were (the log's mark and the
  // files' names too: logs.test.mjs, app-test.mjs), Most Wanted's its own; the registry seeds start the game without
  // its hardware detection (FirstTime = 1 for Most Wanted, NotFirstTime for Underground 2)
  assert.deepEqual([P.ug2.EXE, P.ug2.PROFILES_DIR, P.ug2.LOG_MARK, P.ug2.FILE_PREFIX],
    ['/data/SPEED2.EXE', 'NFS Underground 2', '=== NFSU2 web', 'nfsu2']);
  assert.deepEqual([P.mw.EXE, P.mw.PROFILES_DIR, P.mw.LOG_MARK, P.mw.FILE_PREFIX],
    ['/data/speed.exe', 'NFS Most Wanted', '=== NFSMW web', 'nfsmw']);
  assert.match(P.ug2.REGISTRY_DEFAULTS, /^d NotFirstTime=1$/m);
  assert.match(P.mw.REGISTRY_DEFAULTS, /^d FirstTime=1$/m);
  assert.doesNotMatch(P.mw.REGISTRY_DEFAULTS, /NotFirstTime/);
  for (const g of GAMES) for (const f of ['LOG_MARK', 'FILE_PREFIX'])
    for (const o of GAMES.filter(x => x !== g)) assert.ok(!P[g][f].startsWith(P[o][f]), `${g} ${f} and ${o}'s`);
  // Underground 2's launch switch is off until the launch release; until then its keyphrases show its own word, and
  // every keyphrase derives from VAULT_PREFIX's (vault.test.mjs turns the switch on)
  assert.equal(P.ug2.LAUNCHED, false);
  assert.deepEqual([P.ug2.KEYPHRASE_WORD, P.mw.KEYPHRASE_WORD], ['nfsu2-xyz', 'nfsrevived']);
  for (const g of GAMES) assert.equal(P[g].VAULT_PREFIX, 'nfsu2-xyz', g);
  // one keyphrase, each game's own key, token and sealing (vault.test.mjs): Underground 2's as it always was
  assert.deepEqual([P.ug2.VAULT_DERIVE_INFO, P.ug2.VAULT_AAD], [null, 'nfsu2-xyz keyphrase saves v1']);
  assert.ok(P.mw.VAULT_DERIVE_INFO && P.mw.VAULT_AAD !== P.ug2.VAULT_AAD);
  // the browser's storage: Underground 2's names as they always were, Most Wanted's its own; one game's offline copies
  // are never another's older ones (sw.ts deletes the names that start with its prefix)
  // (every other name: storage-names.test.mjs)
  assert.deepEqual([P.ug2.STORAGE.OFFLINE_CACHE, P.ug2.STORAGE.OFFLINE_PREFIX], ['nfsu2-offline-2', 'nfsu2-offline-']);
  for (const g of GAMES) {
    const s = P[g].STORAGE;
    assert.deepEqual(Object.keys(s).sort(), Object.keys(P.ug2.STORAGE).sort(), g);
    assert.ok(s.OFFLINE_CACHE.startsWith(s.OFFLINE_PREFIX), g);
    for (const o of GAMES.filter(x => x !== g)) {
      const t = P[o].STORAGE;
      assert.ok(!t.OFFLINE_CACHE.startsWith(s.OFFLINE_PREFIX) && !s.OFFLINE_PREFIX.startsWith(t.OFFLINE_PREFIX), `${g} and ${o}`);
    }
  }
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
  w('src/main.ts', 'import { GAME } from \'@game\';\nimport { NAME } from \'@game/names\';\nconsole.log(`the page of ${GAME}, ${NAME}`);\n');
  w('src/games/ug2/index.ts', 'export const GAME = \'ug2\';\n');
  w('src/games/ug2/names.ts', 'export const NAME = \'name-of-ug2\';\n');
  w('src/games/one/index.ts', 'export const GAME = \'one\';\n');
  w('src/games/one/names.ts', 'export const NAME = \'name-of-one\';\n');
  w('src/app/lang/french.json', { A: 'a partagé', B: 'b partagé' });
  w('src/games/one/lang/french.json', { B: 'b à la une', C: 'c à la une' });

  let r = build();
  assert.equal(r.status, 0, r.stderr);
  assert.match(read('dist/main.js'), /\bug2\b/);
  assert.match(read('dist/main.js'), /name-of-ug2/, '@game/<file>: a file of the game\'s folder');
  assert.doesNotMatch(read('dist/main.js'), /\bone\b/);
  assert.equal(read('dist/lang/french.json'), JSON.stringify({ A: 'a partagé', B: 'b partagé' }));
  assert.equal(read('dist/index.html'), '<!doctype html><title>made up</title>');
  assert.ok(fs.existsSync(path.join(root, 'dist-dev/test/dev.json')));
  assert.ok(fs.existsSync(path.join(root, 'dist-dev/main.js.map')));

  r = build('--game', 'one');
  assert.equal(r.status, 0, r.stderr);
  assert.match(read('dist-one/main.js'), /\bone\b/);
  assert.match(read('dist-one/main.js'), /name-of-one/);
  assert.doesNotMatch(read('dist-one/main.js'), /ug2/);
  assert.equal(read('dist-one/lang/french.json'), JSON.stringify({ A: 'a partagé', B: 'b à la une', C: 'c à la une' }));
  assert.ok(fs.existsSync(path.join(root, 'dist-one/manifest.json')));
  assert.ok(fs.existsSync(path.join(root, 'dist-one-dev/test/dev.json')));
  assert.ok(!fs.existsSync(path.join(root, 'dist-one/main.js.map')));
  assert.match(read('dist/main.js'), /\bug2\b/, 'the ug2 page stays as it was');

  // a game's body edits (src/games/<id>/body.json): a text in place of one, a cut from one text to the end of another;
  // i18n-keys.mjs reads the same page; one that does not fit index.html stops the build
  w('index.html', '<!doctype html><title>made up</title><h1>Game One</h1><p data-i18n>Kept</p><p data-i18n>Cut A</p><p data-i18n>Cut B</p>');
  w('src/games/one/body.json', [{ at: '<h1>Game One</h1>', put: '<h1>Game Two</h1>' }, { at: '<p data-i18n>Cut A', until: 'Cut B</p>', put: '' }]);
  r = build('--game', 'one');
  assert.equal(r.status, 0, r.stderr);
  assert.equal(read('dist-one/index.html'), '<!doctype html><title>made up</title><h1>Game Two</h1><p data-i18n>Kept</p>');
  assert.equal(pageHtml('one', root), read('dist-one/index.html'));
  assert.deepEqual([...catalog('one', root).keys.keys()].filter(k => /^(Kept|Cut)/.test(k)), ['Kept']);
  r = build();
  assert.equal(read('dist/index.html'), read('index.html'), 'no edits for a game without them');
  w('src/games/one/body.json', [{ at: '<h1>Game Three</h1>', put: '' }]);
  r = build('--game', 'one');
  assert.equal(r.status, 2);
  assert.match(r.stderr, /body\.json: index\.html has no "<h1>Game Three<\/h1>"/);
  assert.throws(() => pageHtml('one', root), /does not fit/);
  fs.rmSync(path.join(root, 'src/games/one/body.json'));

  r = build('--game', 'two');
  assert.equal(r.status, 2);
  assert.match(r.stderr, /no game "two"/);
  w('src/games/one/lang/english.json', { A: 'a' });
  r = build('--game', 'one');
  assert.equal(r.status, 2);
  assert.match(r.stderr, /src\/games\/one\/lang\/english\.json has no shared src\/app\/lang\/english\.json/);
});

test('Most Wanted\'s page body: its name and keys, no Underground 2 panel (src/games/mw/body.json)', () => {
  assert.equal(pageHtml('ug2'), fs.readFileSync(path.join(WEB, 'index.html'), 'utf8'), 'Underground 2\'s page is index.html');
  const mw = pageHtml('mw'), body = mw.slice(mw.indexOf('<body'));
  assert.match(body, /<h1>Most Wanted Revived<\/h1>/);
  assert.doesNotMatch(body, /Underground 2|Hydraulics|Drift mode|Quick chat|outrun|Photo mode|Garage &gt; Save|id="about"|id="changes"/);
  assert.match(body, /<section class="panel" id="online" aria-labelledby="online-title" hidden>/);
  assert.match(body, /<kbd>G<\/kbd><\/dt><dd data-i18n>Speedbreaker<\/dd>/);
});

test('each game\'s marks in a backup (src/games/save-marks.ts) are its own files\' and no other game\'s', async () => {
  const entry = `export { SAVE_MARKS } from './src/games/save-marks.ts';\n` +
    GAMES.map(g => `export * as ${g} from './src/games/${g}/index.ts';`).join('\n');
  const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: WEB, loader: 'ts' }, bundle: true, format: 'esm', write: false,
    platform: 'neutral', loader: { '.txt': 'text', '.png': 'dataurl' } })).outputFiles[0].text;
  const P = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
  assert.deepEqual(P.SAVE_MARKS.map(m => m.game).sort(), [...GAMES].sort(), 'every game has its marks');
  for (const m of P.SAVE_MARKS) {
    const g = P[m.game];
    assert.ok(m.title.trim(), m.game);
    assert.equal(m.profilesDir, g.PROFILES_DIR, m.game);
    assert.ok(g.SETTINGS_TEMPLATE.startsWith(m.settings), `${m.game}: its settings.ini starts with ${m.settings}`);
    assert.match(g.REGISTRY_DEFAULTS, m.registry, `${m.game}: its registry.txt seed`);
    for (const o of P.SAVE_MARKS.filter(x => x !== m)) {
      assert.notEqual(o.profilesDir.toLowerCase(), m.profilesDir.toLowerCase(), `${m.game} and ${o.game}`);
      assert.ok(!g.SETTINGS_TEMPLATE.startsWith(o.settings), `${m.game}'s settings.ini is not ${o.game}'s`);
      assert.doesNotMatch(g.REGISTRY_DEFAULTS, o.registry, `${m.game}'s registry.txt is not ${o.game}'s`);
    }
  }
});
