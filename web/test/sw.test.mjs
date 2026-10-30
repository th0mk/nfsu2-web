// Offline mode's service worker (web/src/sw.ts) with a fake cache and network (no Chrome): one start uses one set of
// files.  A player's start on 2026-10-25 had the page's script from a copy (yesterday's) and /game.json, so the module,
// from the server (today's): its renderer refused the game's graphics ("GPU control block mismatch").
// Under a base (/u2/, /mw/: both games on one address) the worker keeps that base's files only, and leaves every
// request outside it to the network; each game's worker keeps and cleans up its own Cache Storage only.
//   node --test web/test/sw.test.mjs
// The service worker in a real Chrome, with a server that goes and comes back: web/tools/offline-test.mjs.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const ORIGIN = 'https://nfsu2.test';

/** sw.js of a game's page (build.mjs --game: '@game' is its profile's folder) */
const bundles = {};
async function bundle(game) {
  bundles[game] ??= (await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/sw.ts')], bundle: true, format: 'esm', write: false,
    alias: { '@game': path.join(ROOT, 'web/src/games', game) } })).outputFiles[0].text;
  return bundles[game];
}

// ---- the service worker's world: self, caches, fetch ------------------------
const keyOf = k => { const u = new URL(typeof k === 'string' ? k : k.url, ORIGIN); return u.pathname + u.search; };
let loads = 0;
/** A service worker of `game` at `base` (its sw.js at <base>sw.js), in a world of its own: its event handlers (on),
 *  its copies, the server (path -> [status, body, ms before the answer]; anything else 404 at once), the Cache Storage
 *  names there are (names) and what it opened and deleted, and every path it fetched. */
async function worker({ game = 'ug2', base = '/', names = [] } = {}) {
  const w = { on: {}, copies: new Map(), server: {}, names: [...names], opened: [], deleted: [], fetched: [] };
  const cache = {
    match: async k => w.copies.get(keyOf(k))?.clone(),
    put: async (k, r) => { w.copies.set(keyOf(k), r); },
    keys: async () => [...w.copies.keys()].map(k => ({ url: ORIGIN + k })),
    delete: async k => w.copies.delete(keyOf(k)),
  };
  globalThis.self = {
    location: { origin: ORIGIN, href: `${ORIGIN}${base}sw.js` }, addEventListener: (t, f) => { w.on[t] = f; },
    skipWaiting: async () => {}, clients: { claim: async () => {} },
  };
  globalThis.caches = {
    open: async n => { w.opened.push(n); return cache; },
    keys: async () => w.names,
    delete: async n => { w.deleted.push(n); w.names = w.names.filter(x => x !== n); return true; },
  };
  globalThis.fetch = async q => {
    const k = keyOf(q);
    w.fetched.push(k);
    const [status, body, ms] = w.server[k] ?? [404, 'Not found', 0];
    if (ms) await new Promise(r => setTimeout(r, ms));
    const r = new Response(body, { status, headers: { etag: `"${body.length}"` } });
    Object.defineProperty(r, 'type', { value: 'basic' });
    return r;
  };
  // (a module of its own each time: a data: URL loads once)
  await import('data:text/javascript;base64,' + Buffer.from(`${await bundle(game)}\n// ${loads++}`).toString('base64'));
  /** one request of the page, as the service worker answers it: its answer, or null (not the worker's: the network's) */
  w.answer = (p, mode = 'cors') => {
    let answer = null;
    w.on.fetch({ request: { method: 'GET', headers: new Headers(), url: ORIGIN + p, mode }, respondWith: r => { answer = r; }, waitUntil: () => {} });
    return answer;
  };
  /** its body (the worker's own, or the test fails) */
  w.get = async (p, mode = 'cors') => {
    const answer = w.answer(p, mode);
    assert.ok(answer, `${p} is the service worker's`);
    return (await answer).text();
  };
  w.online = () => new Promise(res => w.on.message({ data: 'online', ports: [{ postMessage: res }] }));
  w.event = async t => { const done = []; w.on[t]({ waitUntil: p => done.push(p) }); await Promise.all(done); };
  return w;
}

/** a start whose script's request fails once: the files after it come from the copies too, until the page says online */
async function oneStartOneSet(base) {
  const w = await worker({ base });
  const at = p => base + p;
  for (const [k, v] of Object.entries({ [at('')]: 'page 1', [at('main.js')]: 'script 1', [at('main.css')]: 'style 1', [at('game.json')]: '{"rev":"1"}' }))
    w.copies.set(k, new Response(v, { headers: { etag: `"old-${k}"` } }));
  // a new version on the server, whose script fails once (a 5xx from the proxy in front) while the stylesheet,
  // asked at the same time, answers a little later
  w.server = { [at('')]: [200, 'page 2', 0], [at('main.js')]: [502, 'Bad gateway', 0], [at('main.css')]: [200, 'style 2', 20], [at('game.json')]: [200, '{"rev":"2"}', 0] };
  assert.equal(await w.get(at(''), 'navigate'), 'page 2');
  const [script, style] = await Promise.all([w.get(at('main.js')), w.get(at('main.css'))]);
  assert.equal(script, 'script 1', 'the script from its copy');
  assert.equal(style, 'style 2', 'the stylesheet was on its way already');
  assert.equal(await w.get(at('game.json')), '{"rev":"1"}', 'the list of the module the copied script goes with, not the server\'s');
  // a reload for the server's version (shell.ts reload): from the server again
  await w.online();
  assert.equal(await w.get(at('game.json')), '{"rev":"2"}');
}

test('a copy answered for one file: the files after it in that start are copies too, until the page says online', () => oneStartOneSet('/'));
test('the same under a base (/u2/: both games on one address)', () => oneStartOneSet('/u2/'));

test('under a base: the base\'s files are the worker\'s, every request outside it the network\'s', async () => {
  const w = await worker({ base: '/u2/' });
  for (const [p, mode] of [['/u2/', 'navigate'], ['/u2/index.html', 'navigate'], ['/u2/main.js'], ['/u2/data-manifest'], ['/u2/audio/worklet.js'],
    ['/u2/lang/french.json'], ['/u2/game.wasm?v=abc-1']])
    assert.ok(w.answer(p, mode), `${p}: the worker's`);
  for (const [p, mode] of [['/', 'navigate'], ['/index.html', 'navigate'], ['/mw/', 'navigate'], ['/main.js'], ['/mw/main.js'], ['/game.json'],
    ['/lang/french.json'], ['/mw/lang/french.json'], ['/game.wasm?v=abc-1'], ['/mw/game.wasm?v=abc-1'], ['/u2/lan'], ['/u2/players'],
    ['/u2/data/GLOBAL/a.bin'], ['/u2/vault'], ['/u2/main.js?x=1'], ['/u2/lang/french.json?x'], ['/vault'], ['/robots.txt']])
    assert.equal(w.answer(p, mode), null, `${p}: the network's`);
  assert.deepEqual(w.fetched, [], 'nothing fetched for them');
  // at the root: the whole address is its base, but only its own files
  const r = await worker({ base: '/' });
  assert.ok(r.answer('/', 'navigate') && r.answer('/main.js'));
  for (const p of ['/u2/main.js', '/mw/main.js', '/lan', '/players']) assert.equal(r.answer(p), null, p);
});

test('install under a base: the base\'s files, then the module its game.json names', async () => {
  const w = await worker({ base: '/mw/', game: 'mw' });
  w.server = { '/mw/game.json': [200, '{"module":"m-1"}', 0], '/mw/game.wasm?v=m-1': [200, 'module', 0] };
  await w.event('install');
  assert.deepEqual(w.fetched.slice(0, 10), ['/mw/', '/mw/main.js', '/mw/main.css', '/mw/game.json', '/mw/data-manifest', '/mw/host/game-worker.js',
    '/mw/host/io-worker.js', '/mw/host/predownload-worker.js', '/mw/gpu/png-worker.js', '/mw/audio/worklet.js']);
  assert.deepEqual(w.fetched.slice(10), ['/mw/game.wasm?v=m-1']);
  assert.ok(w.copies.has('/mw/game.wasm?v=m-1') && w.copies.has('/mw/game.json'));
});

test('each game keeps its own Cache Storage and cleans up only its own older ones', async () => {
  const names = ['nfsu2-offline-1', 'nfsu2-offline-2', 'nfsmw-offline-0', 'nfsmw-offline-1', 'other'];
  const u = await worker({ base: '/u2/', names });
  await u.event('activate');
  assert.deepEqual(u.deleted, ['nfsu2-offline-1']);
  await u.event('install');
  assert.deepEqual([...new Set(u.opened)], ['nfsu2-offline-2'], 'Underground 2\'s name, as before');
  const m = await worker({ base: '/mw/', game: 'mw', names });
  await m.event('activate');
  assert.deepEqual(m.deleted, ['nfsmw-offline-0']);
  await m.event('install');
  assert.deepEqual([...new Set(m.opened)], ['nfsmw-offline-1']);
});
