// How many play on this server now, for the start page's Multiplayer settings: the server's /players (two
// counts and whether it is full, nothing else).  Asked every 15 s, and only while the start page is visible and no game runs; a
// server without the route shows nothing.  Whether the server answered at all goes to reached() (offline mode).

export interface PlayerCounts { players: number; online: number | null; full?: boolean }

const EVERY_MS = 15_000;
const n = (k: number, one: string, many: string) => `${k} ${k === 1 ? one : many}`;

/** The line the page shows ('' for an answer it cannot read). */
export function playersText(c: PlayerCounts): string {
  if (!Number.isInteger(c.players) || c.players < 0) return '';
  if (c.players === 0) return 'On this server now: nobody is playing.';
  const on = Number.isInteger(c.online) && c.online! >= 0 ? `, ${c.online} in Online mode` : '';
  const full = c.full === true ? ' The server is full: playing with others waits until a place frees up.' : '';
  return `On this server now: ${n(c.players, 'player', 'players')}${on}.${full}`;
}

/** The same, short, for the start page's status header: "12 playing · 3 in Online" ('' for an answer it cannot read). */
export function playersShort(c: PlayerCounts): string {
  if (!Number.isInteger(c.players) || c.players < 0) return '';
  const on = Number.isInteger(c.online) && c.online! > 0 ? ` · ${c.online} in Online` : '';
  return c.full === true ? `Server full · ${c.players} playing` : `${c.players} playing${on}`;
}

/** Whether the server answers (any answer but a 5xx from a proxy in front of it), within ms. */
export async function serverAnswers(ms: number): Promise<boolean> {
  try { return (await fetch('/players', { method: 'HEAD', cache: 'no-store', signal: AbortSignal.timeout(ms) })).status < 500; }
  catch { return false; }
}

/** show(line, short) at once and every 15 s while idle() and the page is visible; reached(whether the server answered) */
export function watchPlayers(show: (line: string, short: string) => void, idle: () => boolean, reached?: (ok: boolean) => void) {
  let gone = false;
  const ask = async () => {
    if (gone || document.visibilityState !== 'visible' || !idle()) return;
    try {
      const r = await fetch('/players', { cache: 'no-store', signal: AbortSignal.timeout(5000) });
      reached?.(r.status < 500);
      if (r.status === 404) { gone = true; show('', ''); return; }
      if (r.ok) { const c = await r.json() as PlayerCounts; show(playersText(c), playersShort(c)); }
    } catch { reached?.(false); }
  };
  window.setInterval(() => void ask(), EVERY_MS);
  document.addEventListener('visibilitychange', () => void ask());
  void ask();
  return { now: () => void ask() };
}
