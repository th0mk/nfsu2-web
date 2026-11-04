// This browser's own id on the server's LAN: random, made once and kept in two of the browser's stores, localStorage
// and a file at the top of the origin private file system (beside the save folder, not in it: no backup, restore or
// cloud sync takes it), sent in the switch's hello (net/index.ts).  The operator's bans name a page by it besides its
// address and its passphrase (web/server-rs/crates/bans), so that a banned player is not back by changing networks.
// Each load puts the id back in a store that lost it (keepPageId: one of them cleared alone), and when the two held
// different ids the hello names the other one too ("was"), so that a ban of either holds.  The server counts the
// browsers that come each day by it too (seen(): web/server-rs src/app/days.rs).  It says nothing about the player,
// and the server keeps only a keyed hash of it, in a ban, and in the day's counts with a key of that day.  Without
// storage (a private window, storage blocked) each load has a new one.

const KEY = 'nfsu2.net.page-id';
/** the file at the top of the origin private file system (shell.ts keeps the save folder off its name) */
export const PAGE_ID_FILE = 'nfsu2.page-id';

/** what the hello names: the id, and the one the other store held when it was another (net/index.ts id, was) */
export interface PageIds { id: string; was: string[] }

const valid = (s: string | null | undefined): s is string => !!s && /^[A-Za-z0-9_-]{22}$/.test(s);
/** this load's id when localStorage keeps none */
let loose: string | null = null;
let kept: Promise<PageIds> | null = null;

/** The id now: localStorage's (made and kept there when it has none).  keepPageId, early in a load, may change it
 *  once, to the file's. */
export function pageId(): string {
  try {
    const have = localStorage.getItem(KEY);
    if (valid(have)) return have;
    const id = make();
    localStorage.setItem(KEY, id);
    return id;
  } catch { return loose ??= make(); }
}

/** The id from both stores, put back in a store that lost it or held another: the file's first (the harder one to
 *  clear alone), else localStorage's, else a new one.  Once a load. */
export function keepPageId(): Promise<PageIds> {
  return kept ??= (async () => {
    let local: string | null = null;
    try { local = localStorage.getItem(KEY); } catch { /* no localStorage */ }
    if (!valid(local)) local = null;
    const file = await readFile();
    const id = file ?? local ?? loose ?? make();
    if (file !== id) await writeFile(id);
    if (local !== id) {
      try { localStorage.setItem(KEY, id); } catch { loose = id; }
    }
    return { id, was: local && local !== id ? [local] : [] };
  })();
}

/** what a day's first Play tells the server besides: the kind of device, and the settings changed from its defaults
 *  (settings.ts changedSettings: "key=value", the player's own words only as "set") */
export interface SettingsReport { device: 'pc' | 'phone' | 'tablet'; changed: string[] }
/** the UTC day this browser last told the server its settings */
const TOLD_KEY = 'nfsu2.seen.settings';

/** This browser came today, for the server's daily counts (web/server-rs src/app/days.rs): at a load ('page') and at
 *  Play ('play').  At the first Play of a UTC day the settings go along (web/server-rs src/app/settings_days.rs keeps
 *  only the day's totals): once a day, so the server needs nothing per browser to count each one once.  A day whose
 *  request failed is not marked, and a browser without storage tells them at each Play.  Nothing comes back, and a
 *  server that does not answer changes nothing. */
export function seen(at: 'page' | 'play', settings?: SettingsReport): void {
  const day = new Date().toISOString().slice(0, 10);
  let told: string | null = null;
  try { told = localStorage.getItem(TOLD_KEY); } catch { /* no localStorage */ }
  const report = settings && told !== day ? `&k=${settings.device}&s=${encodeURIComponent(settings.changed.join(','))}` : '';
  void keepPageId()
    .then(({ id }) => fetch(`/seen?at=${at}&id=${encodeURIComponent(id)}${report}`, { cache: 'no-store', keepalive: true }))
    .then(r => { if (report && r.ok) try { localStorage.setItem(TOLD_KEY, day); } catch { /* no localStorage */ } })
    .catch(() => {});
}

async function readFile(): Promise<string | null> {
  try {
    const f = await (await (await navigator.storage.getDirectory()).getFileHandle(PAGE_ID_FILE)).getFile();
    const s = (await f.text()).trim();
    return valid(s) ? s : null;
  } catch { return null; }
}

async function writeFile(id: string): Promise<void> {
  try {
    const h = await (await navigator.storage.getDirectory()).getFileHandle(PAGE_ID_FILE, { create: true });
    const w = await h.createWritable();
    await w.write(id); await w.close();
  } catch { /* no origin private file system here, or no room: localStorage alone */ }
}

/** 128 random bits, base64url (22 characters) */
function make(): string {
  const b = crypto.getRandomValues(new Uint8Array(16));
  return btoa(String.fromCharCode(...b)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}
