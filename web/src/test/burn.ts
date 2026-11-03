// ?mainburn=N[:P]: the test page's main thread busy for N ms every P ms (default 16), as a slow computer's frame work
// keeps it (the renderer encodes each frame there): what that does to the network's datagrams, its link on the page
// (networker=0) or in its worker (web/src/net/remote.ts)
export function mainBurn(q: URLSearchParams, log: (s: string) => void) {
  const m = /^(\d+(?:\.\d+)?)(?::(\d+))?$/.exec(q.get('mainburn') ?? '');
  if (!m) return;
  const ms = +m[1], every = +(m[2] ?? 16);
  log(`[burn] the main thread busy ${ms} ms every ${every} ms`);
  setInterval(() => { const t = performance.now(); while (performance.now() - t < ms) { /* busy */ } }, every);
}
