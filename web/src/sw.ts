// The service worker: offline mode.  It keeps the start page, its scripts,
// the game module and the list of game files in the browser, so the game
// starts without the server.  The game data itself is in OPFS (datacache.ts):
// with all of it downloaded (Game data on this PC) and the movies off, a game
// needs nothing from the network.
//
// Network first: while the server answers, every one of these comes from it
// as before (a new version arrives at once), and the copy here is replaced
// when its ETag changes.  When the server cannot be reached (no network, the
// server down: a failed request, a 5xx from a proxy in front of it, or no
// answer to the page within NAV_WAIT_MS), the copies answer, and for
// OFFLINE_MS after that they answer first, so one start uses one set of them.
// An answer from the server in that time does not end it: the page's script
// from a copy beside /game.json from the server would be two versions, whose
// renderer and module do not fit (a start that failed so, 2026-10-25).
// A page that finds the server again and reloads for its newer version (a
// deploy restarts the server, so its players' pages may start from the copies:
// shell.ts) says so first ('online'): that start is the server's.  After every start page that came from the server, the files it loads only
// later (the workers, the list of game files) are brought up to date too, so
// the copies are one version.  Nothing else is touched: the game data, /lan,
// /players, the test pages go to the network as they always did.
//
// The module is kept at its own address (/game.wasm?v=<hash>, loader.ts), one
// at a time: offline, a start gets the module its /game.json names, or none.
// The page registers this once the module has loaded (shell.ts), so the first
// visit downloads the module once, and the copy comes from the HTTP cache.
//
// Every address here is under the page's base (src/base.ts), where this file
// is: '/' where a server serves one game at its root, '/u2/' or '/mw/' where
// both games share one address.  The worker's scope is that base, its copies
// are the game's own (STORAGE: each game's Cache Storage name, and the start of
// its older ones, which it deletes), and a request outside the base is never
// answered here: it goes to the network as if there were no worker.

import { STORAGE } from '@game/storage';

declare const self: ServiceWorkerGlobalScope;
export {};

/** the page's base path: this file's folder ('/', '/u2/', '/mw/') */
const BASE = new URL('./', self.location.href).pathname;
const CACHE = STORAGE.OFFLINE_CACHE;
const PAGE = BASE, MODULE = BASE + 'game.wasm', GAME_JSON = BASE + 'game.json';
/** what a start needs, and the module; the page loads the first five itself before Play */
const KEPT = ['', 'main.js', 'main.css', 'game.json', 'data-manifest',
  'host/game-worker.js', 'host/io-worker.js', 'host/predownload-worker.js', 'gpu/png-worker.js', 'audio/worklet.js'].map(p => BASE + p);
const LATER = KEPT.slice(4);
/** the start page's language files (i18n.ts): one is kept once the page fetched it, and kept up to date with the rest */
const LANG = /^lang\/[a-z]+\.json$/;
const isLang = (path: string) => path.startsWith(BASE) && LANG.test(path.slice(BASE.length));
const NAV_WAIT_MS = 4000, OFFLINE_MS = 30_000;
let offlineUntil = 0;

/** the copy's key for a request, or null: not ours */
function keyOf(r: Request): string | null {
  if (r.method !== 'GET' || r.headers.has('range')) return null;
  const u = new URL(r.url);
  if (u.origin !== self.location.origin) return null;
  if (r.mode === 'navigate') return u.pathname === PAGE || u.pathname === BASE + 'index.html' ? PAGE : null;
  if (u.pathname === MODULE) return /^\?v=[\w-]+$/.test(u.search) ? MODULE + u.search : null;
  return !u.search && (KEPT.includes(u.pathname) || isLang(u.pathname)) ? u.pathname : null;
}

/** keep a 200 answer, unless the copy has its ETag already; a module replaces the one kept before */
async function keep(cache: Cache, key: string, res: Response, kept?: Response) {
  if (res.status !== 200 || res.type !== 'basic') return;
  const tag = res.headers.get('etag');
  if (kept && tag && kept.headers.get('etag') === tag) { await res.body?.cancel(); return; }
  await cache.put(key, res);
  if (key.startsWith(MODULE + '?'))
    for (const r of await cache.keys()) { const u = new URL(r.url); if (u.pathname === MODULE && MODULE + u.search !== key) await cache.delete(r); }
}

/** the module the kept /game.json names (from the HTTP cache once the page has loaded it) */
async function keepModule(cache: Cache) {
  const info = await (await cache.match(GAME_JSON, { ignoreVary: true }))?.json() as { module?: string } | undefined;
  if (!info?.module) return;
  const key = `${MODULE}?v=${encodeURIComponent(info.module)}`;
  const kept = await cache.match(key, { ignoreVary: true });
  if (!kept) await keep(cache, key, await fetch(key));
}

async function refresh(cache: Cache, paths: string[]) {
  await Promise.allSettled(paths.map(async p => {
    const kept = await cache.match(p, { ignoreVary: true });
    const res = await fetch(p, { cache: 'no-cache' });
    await keep(cache, p, res, kept);
  }));
}

async function answer(e: FetchEvent, key: string): Promise<Response> {
  const cache = await caches.open(CACHE);
  const kept = await cache.match(key, { ignoreVary: true });
  if (kept && Date.now() < offlineUntil) return kept;
  const net = fetch(e.request);
  const late = new Promise<null>(res => setTimeout(() => res(null), NAV_WAIT_MS));
  try {
    const res = kept && key === PAGE ? await Promise.race([net, late]) : await net;
    if (!res || (res.status >= 500 && kept)) {
      offlineUntil = Date.now() + OFFLINE_MS;
      // a late answer still replaces the copy when it comes
      if (!res) e.waitUntil(net.then(r => keep(cache, key, r, kept)).catch(() => {}));
      return kept!;
    }
    e.waitUntil((async () => {
      await keep(cache, key, res.clone(), kept);
      if (key === PAGE) await refresh(cache, [...LATER, ...(await cache.keys()).map(r => new URL(r.url).pathname).filter(isLang)]);
    })().catch(() => {}));
    return res;
  } catch (err) {
    if (!kept) throw err;
    offlineUntil = Date.now() + OFFLINE_MS;
    return kept;
  }
}

self.addEventListener('install', e => {
  e.waitUntil((async () => {
    const cache = await caches.open(CACHE);
    await refresh(cache, KEPT);
    await keepModule(cache).catch(() => {});
    await self.skipWaiting();
  })());
});
self.addEventListener('activate', e => {
  e.waitUntil((async () => {
    for (const k of await caches.keys()) if (k.startsWith(STORAGE.OFFLINE_PREFIX) && k !== CACHE) await caches.delete(k);
    await self.clients.claim();
  })());
});
self.addEventListener('message', e => {
  if (e.data !== 'online') return;
  offlineUntil = 0;
  e.ports[0]?.postMessage('online');
});
self.addEventListener('fetch', e => {
  const key = keyOf(e.request);
  if (key) e.respondWith(answer(e, key));
});
