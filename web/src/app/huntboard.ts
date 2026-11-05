// The daily treasure hunt's board (web/server-rs/src/app/hunts.rs): a hunt's time once all 15 diamonds are found
// (treasure.ts), and a day's board, how many finished it and the 10 fastest by their claimed names.  The server knows
// a player by its keyphrase's token (as /names and /friends: shell.ts's vault.token()), else by the page's id
// (page-id.ts); a time without a claimed name is counted, not listed.  One time a player a UTC day, the first.
import { pageId } from './page-id';
import { count, t } from './i18n';

/** a day's board: `day` as achievements.ts utcDay counts them, `finished` players with a time, `best` the fastest */
export interface HuntBoard { day: number; finished: number; best: { name: string; ms: number }[] }

/** the board of an answer of the server's, read leniently (null: none) */
function boardOf(j: any): HuntBoard | null {
  if (typeof j?.day !== 'number' || typeof j.finished !== 'number') return null;
  const best = Array.isArray(j.best) ? j.best.filter((b: any) => typeof b?.name === 'string' && typeof b.ms === 'number')
    .map((b: any) => ({ name: b.name, ms: b.ms })) : [];
  return { day: j.day, finished: j.finished, best };
}

/** Day `day`'s hunt took `ms` (active free roam, the 1st diamond to the 15th): true once the server has a time of
 *  this player's for the day (this one, or its first: "had").  `token()`: the keyphrase's (null: none).  false: refused
 *  (not today or yesterday, under 2 minutes), too many, no server board, or no answer. */
export async function reportHunt(day: number, ms: number, token: () => Promise<string | null> = async () => null): Promise<boolean> {
  try {
    const tk = await token().catch(() => null);
    const r = await fetch('/hunt', {
      method: 'POST', cache: 'no-store', signal: AbortSignal.timeout(8000),
      headers: { 'content-type': 'application/json', ...(tk ? { authorization: `Bearer ${tk}` } : {}) },
      body: JSON.stringify({ day, ms: Math.round(ms), id: pageId() }),
    });
    if (!r.ok) return false;
    const j = await r.json();
    return j?.done === 'new' || j?.done === 'had';
  } catch { return false; }
}

/** day `day`'s board; null: no answer, or a server without one (404) */
export async function huntBoard(day: number): Promise<HuntBoard | null> {
  try {
    const r = await fetch(`/hunt?day=${encodeURIComponent(day)}`, { cache: 'no-store', signal: AbortSignal.timeout(8000) });
    return r.ok ? boardOf(await r.json()) : null;
  } catch { return null; }
}

/** the board under the achievements tab's hunt line: how many finished the day's hunt, and the fastest (up to 5) */
export function showBoard(el: HTMLElement, b: HuntBoard | null, clock: (ms: number) => string) {
  el.hidden = !b;
  if (!b) return;
  const head = document.createElement('p');
  head.className = 'hint';
  head.textContent = b.finished ? t('Finished today: {n}.', { n: count(b.finished) }) : t('No one has found all 15 today yet.');
  const list = document.createElement('ol');
  list.className = 'hunt-best';
  for (const x of b.best.slice(0, 5)) {
    const li = document.createElement('li'), name = document.createElement('span'), time = document.createElement('b');
    name.textContent = x.name; time.textContent = clock(x.ms);
    li.append(name, time); list.append(li);
  }
  el.replaceChildren(head, ...(b.best.length ? [list] : []));
}
