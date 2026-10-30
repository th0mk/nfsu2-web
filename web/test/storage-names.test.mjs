// The names each game's page uses in the storage the browser keeps per origin (PLAN D-36, web/src/games/profile.ts
// StorageNames), which both games share once they are served from one origin (/u2/ and /mw/):
//   node --test web/test/storage-names.test.mjs
// 1. The table: Underground 2's names as they always were; Most Wanted's its own (OPFS mw-save and mw-cache, keys that
//    start nfsmw. or nfsmw-); no name of one game is the other's, starts one of the other's families (a name that ends
//    in ':' or '.') or is one of the shared names (src/games/shared-storage.ts), which are listed once, there.
// 2. No part of the page names a place in that storage but through the table: outside src/games, the only string
//    literals that start nfsu2, nfsmw or nfsrevived are the uses listed here, none of them storage.
// 3. The built pages (web/dist: node web/build.mjs; web/dist-mw: node web/build.mjs --game mw; a page not built is
//    skipped): every origin-wide name in each, from its game's table, and none of the other game's own names in it.
// 4. The save folder a page takes (src/app/save-dir.ts): a development server's ?save=<name> and the test pages (play,
//    hostcheck, aicheck, which empty their folder first) never take a game's save folder, cache or page-id file but the
//    page's own save folder, and the test pages' folders are each game's own (save-test, mw-save-test).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const GAMES = ['ug2', 'mw'];

/** a module of src (with '@game' as the game's profile), loaded */
async function load(entry, game = 'ug2') {
  const js = (await esbuild.build({ entryPoints: [path.join(WEB, 'src', entry)], bundle: true, write: false, format: 'esm', platform: 'neutral',
    alias: { '@game': path.join(WEB, 'src/games', game) } })).outputFiles[0].text;
  return import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
}
const T = {};
for (const g of GAMES) T[g] = (await load(`games/${g}/storage.ts`)).STORAGE;
const { SHARED_STORAGE } = await load('games/shared-storage.ts');

/** every name of a table: [where, name] */
const names = (o, at = '') => Object.entries(o).flatMap(([k, v]) => typeof v === 'string' ? [[at + k, v]] : Array.isArray(v) ? v.map((x, i) => [`${at}${k}[${i}]`, x]) : names(v, `${at}${k}.`));
const family = n => /[:.]$/.test(n);
/** a and b name the same place, or one is a family that takes the other */
const clash = (a, b) => a === b || (family(a) && b.startsWith(a)) || (family(b) && a.startsWith(b));

test('Underground 2\'s names are the ones it always had', () => {
  assert.deepEqual(T.ug2, {
    OFFLINE_CACHE: 'nfsu2-offline-2', OFFLINE_PREFIX: 'nfsu2-offline-',
    SAVE_DIR: 'save', CACHE_DIR: 'cache', PAGE_ID_FILE: 'nfsu2.page-id',
    SAVES_LOCK: 'nfsu2-saves:', INVITE_CHANNEL: 'nfsu2-invite:', RADIO_DB: 'nfsu2.radio',
    LOCAL: {
      GPU_PIPELINES: 'nfsu2.gpu.pipelines.v1', LOG_SALT: 'nfsu2.logsalt', RENDER_CHECK: 'nfsu2.device.render',
      LAST_GAME: 'nfsu2.lastgame.', PAGE_ID: 'nfsu2.net.page-id', PLAYED_MS: 'nfsu2.play.ms', SENT_MS: 'nfsu2.play.sms',
      RADIO_MODES: 'nfsu2.radio.modes', RADIO_ON: 'nfsu2.radio.on', RADIO_NEXT: 'nfsu2.radio.next', RADIO_SHUFFLE: 'nfsu2.radio.shuffle',
      TOUCH_STEERING: 'nfsu2-touch-steering', TOUCH_LEVEL: 'nfsu2-touch-level', TOUCH_SIZE: 'nfsu2-touch-size', MENU_SIZE: 'nfsu2-menu-size',
      KEYPHRASE_LINK: 'nfsu2-keyphrase:', CLOUD_OFFER: 'nfsu2-cloud-offer:',
      NAME_ASKED: 'nfsu2-name-asked:', NAME_CLAIMED: 'nfsu2-name-claimed:', CLAIM_TRIED: 'nfsu2-claim-tried:',
    },
    SESSION: { UPDATE: 'nfsu2-update', OTHER_BUILD: 'nfsu2-other-build' },
  });
});

test('Most Wanted\'s names are its own (D-36), the shared ones listed once', () => {
  // the same places, each its own name
  assert.deepEqual(names(T.mw).map(([k]) => k), names(T.ug2).map(([k]) => k));
  for (const [k, n] of names(T.mw)) assert.match(n, /^(nfsmw[.-]|mw-(save|cache)$)/, k);
  assert.deepEqual([T.mw.SAVE_DIR, T.mw.CACHE_DIR, T.mw.SAVES_LOCK, T.mw.OFFLINE_CACHE], ['mw-save', 'mw-cache', 'nfsmw-saves:', 'nfsmw-offline-1']);
  assert.deepEqual([T.mw.SESSION.UPDATE, T.mw.SESSION.OTHER_BUILD], ['nfsmw-update', 'nfsmw-other-build']);
  // within a game, no two places are one
  for (const g of GAMES) {
    const all = names(T[g]).filter(([k]) => k !== 'OFFLINE_PREFIX' && k !== 'OFFLINE_CACHE');
    for (const [i, [k, a]] of all.entries()) for (const [l, b] of all.slice(i + 1)) assert.ok(!clash(a, b), `${g}: ${k} ${a} and ${l} ${b}`);
  }
  // across the games and the shared names: nothing in common (the offline caches: sw.ts deletes its own prefix only)
  const shared = names(SHARED_STORAGE).filter(([k]) => !k.startsWith('OPFS_TOP'));
  for (const [k, a] of names(T.ug2)) for (const [l, b] of [...names(T.mw), ...shared]) assert.ok(!clash(a, b), `ug2 ${k} ${a} and ${l} ${b}`);
  for (const [k, a] of names(T.mw)) for (const [l, b] of shared) assert.ok(!clash(a, b), `mw ${k} ${a} and ${l} ${b}`);
  assert.deepEqual(shared.map(([, n]) => n), ['nfsrevived.keyphrase', 'nfsrevived.lastgame', 'nfsu2.wheel.', 'nfsu2.shifter.', 'nfsu2-page-language']);
  // the top of the private file system: every game's folders and page-id file, no more
  assert.deepEqual([...SHARED_STORAGE.OPFS_TOP].sort(), GAMES.flatMap(g => [T[g].SAVE_DIR, T[g].CACHE_DIR, T[g].PAGE_ID_FILE]).sort());
});

/** The literals that start nfsu2, nfsmw or nfsrevived outside src/games, none a name in the browser's storage:
 *  file -> [the literal's start, why] */
const NOT_STORAGE = {
  'host/process.ts': [['"nfsu2.mem"', 'a custom section of the module'], ['\'nfsu2.mem\'', 'the same']],
  'host/game-worker.ts': [['"nfsu2"', 'the game\'s argv[0]']],
  'app/game.ts': [['\'nfsu2\'', 'the game\'s argv[0]']],
  'test/play.ts': [['\'nfsu2\'', 'the game\'s argv[0]']],
  'test/hostcheck.ts': [['\'nfsu2\'', 'the game\'s argv[0]']],
  'test/aicheck.ts': [['\'nfsu2\'', 'the game\'s argv[0]']],
  'gpu/index.ts': [['`nfsu2_g', 'a frame dump\'s file name']],
  'input/wheel.ts': [['\'nfsu2-wheel-map\'', 'an event of the page\'s window']],
  'input/gamepad.ts': [['\'nfsu2-pad-driver\'', 'an event of the page\'s window']],
  'input/index.ts': [['\'nfsu2-gesture\'', 'an event of the page\'s window']],
  'audio/index.ts': [['\'nfsu2-out\'', 'the audio worklet\'s processor'], ['\'nfsu2-gesture\'', 'an event of the page\'s window']],
  'audio/worklet.ts': [['\'nfsu2-out\'', 'the audio worklet\'s processor']],
  'app/vault-ui.ts': [['\'nfsu2-passphrase.txt\'', 'a download\'s file name']],
  'app/shell.ts': [['`nfsu2-${stamp()}.log`', 'a download\'s file name']],
  'app/shots.ts': [['`nfsu2-${stamp(d)}`', 'a screenshot\'s file name']],
  'app/saves-ui.ts': [['`nfsu2-saves-${stamp()}.zip`', 'a download\'s file name']],
  'app/vault.ts': [['"nfsu2-xyz', 'the keyphrase\'s words (comments)'], ['\'nfsu2-xyz', 'the keyphrase\'s words'], ['"nfsrevived"', 'the keyphrase\'s word (a comment)'],
    ['\'nfsrevived\'', 'the keyphrase\'s word'], ['\'nfsu2\'', 'the keyphrase\'s words']],
};

test('no part of the page names a place in the storage but through the table', () => {
  const src = path.join(WEB, 'src'), found = [];
  for (const f of fs.readdirSync(src, { recursive: true }).map(String)) {
    if (!f.endsWith('.ts') || f.split(path.sep)[0] === 'games') continue;
    const rel = f.split(path.sep).join('/'), s = fs.readFileSync(path.join(src, f), 'utf8');
    for (const m of s.matchAll(/['"`]nfs(u2|mw|revived)/g)) {
      const at = s.slice(m.index);
      if (!(NOT_STORAGE[rel] ?? []).some(([start]) => at.startsWith(start))) found.push(`${rel}: ${at.slice(0, 40)}`);
    }
  }
  assert.deepEqual(found, []);
  // and the calls that name a place there take no literal of their own
  const calls = /\b(localStorage|sessionStorage)\.(getItem|setItem|removeItem)\(\s*['"`]|locks\.request\(\s*['"`]|caches\.(open|delete)\(\s*['"`]|new BroadcastChannel\(\s*['"`]|indexedDB\.open\(\s*['"`]/;
  for (const f of fs.readdirSync(src, { recursive: true }).map(String)) {
    if (!f.endsWith('.ts')) continue;
    const s = fs.readFileSync(path.join(src, f), 'utf8');
    assert.ok(!calls.test(s), `${f}: ${calls.exec(s)?.[0]}`);
  }
});

for (const g of GAMES) {
  const dist = path.join(WEB, g === 'ug2' ? 'dist' : `dist-${g}`), other = GAMES.find(x => x !== g);
  test(`the built page of ${g}: its own names, none of ${other}'s`, { skip: !fs.existsSync(path.join(dist, 'main.js')) && `no ${path.basename(dist)} (node web/build.mjs${g === 'ug2' ? '' : ` --game ${g}`})` }, t => {
    const js = fs.readdirSync(dist, { recursive: true }).map(String).filter(f => f.endsWith('.js')).map(f => fs.readFileSync(path.join(dist, f), 'utf8')).join('\n');
    // the top of the private file system's names are every page's (save-dir.ts refuses them all)
    const top = new Set(SHARED_STORAGE.OPFS_TOP);
    const own = names(T[g]).filter(([, n]) => !top.has(n)), theirs = names(T[other]).filter(([, n]) => !top.has(n));
    const missing = own.filter(([, n]) => !js.includes(n)).map(([k, n]) => `${k} ${n}`);
    assert.deepEqual(missing, [], 'names of its table the page does not use');
    assert.deepEqual(theirs.filter(([, n]) => js.includes(n)).map(([k, n]) => `${k} ${n}`), [], `${other}'s names in ${g}'s page`);
    t.diagnostic(`${path.basename(dist)}: ${[...own.map(([, n]) => n), ...[T[g].SAVE_DIR, T[g].CACHE_DIR, T[g].PAGE_ID_FILE], ...names(SHARED_STORAGE).filter(([k]) => !k.startsWith('OPFS_TOP')).map(([, n]) => n)].join(' ')}`);
  });
}

test('the save folder a page takes: never a game\'s folder, cache or page-id file but its own', async () => {
  for (const g of GAMES) {
    const D = await load('app/save-dir.ts', g), own = T[g].SAVE_DIR;
    // the start page: ?save=<name> or the game's own folder; a refused name gives <own>-test (shell.ts takeParams)
    const start = asked => D.saveDirName(asked || own, own) ?? `${own}-test`;
    assert.equal(start(null), own, g);
    assert.equal(start(own), own, g);
    for (const n of SHARED_STORAGE.OPFS_TOP.filter(n => n !== own)) assert.equal(start(n), `${own}-test`, `${g} ?save=${n}`);
    assert.equal(start('..'), `${own}-test`, g);
    assert.equal(start('a/b c'), 'a_b_c', g);
    assert.equal(start('save-x'), 'save-x', g);
    // the test pages: the game's own folders by default, and never a player's
    assert.equal(D.testSaveDir(null, 'test'), `${own}-test`, g);
    assert.equal(D.testSaveDir(null, 'play'), `${own}-play`, g);
    assert.equal(D.testSaveDir('save-x', 'test'), 'save-x', g);
    for (const n of ['cache', 'save', 'mw-save', 'mw-cache', 'nfsu2.page-id', 'nfsmw.page-id', '.', '..', 'a/b'])
      assert.throws(() => D.testSaveDir(n, 'test'), /a test page empties its save folder first/, `${g} ${n}`);
  }
  assert.equal(T.ug2.SAVE_DIR + '-test', 'save-test');
});
