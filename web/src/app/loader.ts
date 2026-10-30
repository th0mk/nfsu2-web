// Loads the game module from the local server: /game.json (what was built)
// and /game.wasm, compiled while it streams in.  The progress comes from a
// clone of the response, so the original response still goes to
// compileStreaming and Chrome can reuse its compiled-code cache for it.
// The server sends the module brotli-compressed, at an address that names its
// content (/game.wasm?v=<hash>, the hash from /game.json, which no cache
// keeps): the browser and Cloudflare keep it for good, a reload reads it from
// Chrome's HTTP cache without asking, and a new build is a new address.

import { at } from '../base';

export interface GameInfo { pages: number; maxPages: number; guestEnd: number; rev: string; size: number; opt: string; module?: string }
export interface Loaded {
  module: WebAssembly.Module; info: GameInfo;
  ms: number;              // fetch + compile, wall time
  transferred: number;     // bytes over the network (0 = from the HTTP cache), -1 unknown
}

/** /game.json and the module it names (a server without module: /game.wasm, revalidated) */
async function fetchModule(signal?: AbortSignal) {
  for (let tries = 1; ; tries++) {
    const ir = await fetch(at('game.json'), { cache: 'no-store', signal });
    if (!ir.ok) throw new Error(`${at('game.json')}: HTTP ${ir.status} (the server has no game module)`);
    const info = await ir.json() as GameInfo;
    const url = new URL(at(info.module ? `game.wasm?v=${encodeURIComponent(info.module)}` : 'game.wasm'), location.href).href;
    const r = await fetch(url, { cache: info.module ? 'default' : 'no-cache', signal });
    // 409: a new build arrived between the two answers: its /game.json
    if (r.status === 409 && tries < 3) { await r.body?.cancel(); continue; }
    if (!r.ok) throw new Error(`${at('game.wasm')}: HTTP ${r.status}`);
    return { info, url, r };
  }
}

export async function loadGame(onProgress: (got: number, total: number) => void, signal?: AbortSignal): Promise<Loaded> {
  const t0 = performance.now();
  const { info, url, r } = await fetchModule(signal);
  const total = info.size || +(r.headers.get('content-length') ?? 0);
  const counting = (async () => {
    const rd = r.clone().body!.getReader();
    let got = 0;
    for (;;) {
      const { done, value } = await rd.read();
      if (done) break;
      got += value.length;
      onProgress(got, total);
    }
  })();
  const [module] = await Promise.all([WebAssembly.compileStreaming(r), counting]);
  const ms = performance.now() - t0;
  const pe = performance.getEntriesByName(url).pop() as PerformanceResourceTiming | undefined;
  return { module, info, ms, transferred: pe ? pe.transferSize : -1 };
}

/** The revision the server has now, or null (no answer within ms).  The query keeps the request past offline
 *  mode (sw.ts keeps bare paths only), so asking never makes its copy of /game.json one its module is not. */
export async function serverRev(ms: number): Promise<string | null> {
  try {
    const r = await fetch(at('game.json?latest'), { cache: 'no-store', signal: AbortSignal.timeout(ms) });
    return r.ok ? ((await r.json()) as GameInfo).rev || null : null;
  } catch { return null; }
}
