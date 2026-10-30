// This browser's own id on the server's LAN: random, made once and kept in two of the browser's stores, localStorage
// and a file at the top of the origin private file system (beside the save folder, not in it: no backup, restore or
// cloud sync takes it), sent in the switch's hello (net/index.ts).  The operator's bans name a page by it besides its
// address and its passphrase (web/server-rs/crates/bans), so that a banned player is not back by changing networks.
// Each load puts the id back in a store that lost it (keepPageId: one of them cleared alone), and when the two held
// different ids the hello names the other one too ("was"), so that a ban of either holds.  It says nothing about the
// player, and the server keeps only a keyed hash of it, in a ban.  Without storage (a private window, storage
// blocked) each load has a new one.

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
