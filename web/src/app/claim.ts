// Claimed names (docs/web/PLAYING.md, Your name): a player claims the name it plays under with its keyphrase (the
// one that keeps its cloud saves), and while the claim holds nobody else plays under that name.  The server keeps
// the claims and checks them where names are used (web/server-rs/crates/claims; /names, the Online lobby, free
// roam's hub); this is the page's side of it:
// - onlineName: the Online form of a name (runtime/online.c clean(), nfsu2-claims clean()), which both the Online
//   menu and free roam use, and by which names are compared, in any case;
// - digitsFor: the name with four digits in place of its end, the same ones for this browser and name: what the page
//   plays under when another player claimed the name, or for NAME, the game's default that nobody claims;
// - NameClaims: /names, asked with the keyphrase's token (never the keyphrase): what a name is to this player, and
//   the claim itself, with Cloudflare Turnstile's answer when the server asks for its check (turnstile below).

import { blockedName } from './names.mjs';

/** a claim holds this long after the player last played under it (nfsu2-claims HOLD_DAYS) */
export const HOLD_DAYS = 180;
/** the name before a change stays the player's this long (GRACE_DAYS) */
export const GRACE_DAYS = 14;
/** the game's names for nobody in particular, which nobody claims (lower case) */
export const UNCLAIMABLE = ['name', 'player'];

/** The Online form of a name: the letters, digits, - and _, any other run of characters one _ (none at either end),
 *  cut to 15; one or two characters padded with _.  null: nothing of a name in it. */
export function onlineName(s: string): string | null {
  let out = '', gap = false;
  for (const ch of s) {
    if (out.length >= 15) break;
    if (!/^[A-Za-z0-9_-]$/.test(ch)) { gap = true; continue; }
    if (gap && out && out.length < 14) out += '_';
    gap = false;
    out += ch;
  }
  if (!out) return null;
  while (out.length < 3) out += '_';
  return out;
}

/** the name as typed or its Online form reads as a slur (the server's nfsu2-claims reads_as_slur) */
export function readsAsSlur(typed: string): boolean {
  const o = onlineName(typed);
  return blockedName(typed) || (o !== null && blockedName(o));
}

/** The name with four digits in place of its end, the same ones for one seed (this browser's id) and name;
 *  "Player" with the digits where the shortened name reads as a slur (as nfsu2-online free_variant). */
export async function digitsFor(name: string, seed: string): Promise<string> {
  const base = onlineName(name) ?? 'Player';
  const h = new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(`${seed}\n${base.toLowerCase()}`)));
  const d = String(((h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3]) >>> 0) % 9000 + 1000);
  const cut = base.slice(0, 11) + d;
  return blockedName(cut) ? `Player${d}` : cut;
}

export type NameState = 'free' | 'yours' | 'old' | 'taken' | 'default' | 'blocked' | 'invalid';
export interface Mine { name: string; until: number; next: number }
/** what a name is to this player (GET /names) */
export interface NameInfo {
  name: string | null;
  state: NameState;
  /** yours, old: until when */
  until?: number;
  /** the second name asked about (the page's digits) */
  alt?: NameInfo;
  /** the keyphrase's own name */
  mine?: Mine;
  /** Cloudflare Turnstile's site key: a claim needs its check */
  captcha?: string;
}
export type ClaimError = 'captcha' | 'captcha-down' | 'keyphrase' | 'invalid' | 'default' | 'blocked' | 'taken' | 'soon' | 'full' | 'busy' | 'net';
export type Claimed = { ok: true; mine: Mine } | { ok: false; error: ClaimError; next?: number };

type Fetch = (url: string, init?: RequestInit) => Promise<Response>;

/** /names for this player: `token()` is its keyphrase's (null: it has none) */
export class NameClaims {
  /** the server keeps claims: unknown until it answered once */
  here: boolean | null = null;
  constructor(private token: () => Promise<string | null>, private f: Fetch = (u, i) => fetch(u, i)) {}

  private async headers(): Promise<Record<string, string>> {
    const t = await this.token().catch(() => null);
    return t ? { authorization: `Bearer ${t}` } : {};
  }

  /** what `name` (and `alt`) is to this player; null: no answer, or a server without claims */
  async check(name: string, alt?: string): Promise<NameInfo | null> {
    const u = new URLSearchParams({ name });
    if (alt) u.set('alt', alt);
    try {
      const r = await this.f(`/names?${u}`, { headers: await this.headers(), cache: 'no-store', signal: AbortSignal.timeout(8000) });
      if (r.status === 404) { this.here = false; return null; }
      if (!r.ok) return null;
      this.here = true;
      return await r.json() as NameInfo;
    } catch { return null; }
  }

  /** this player's keyphrase claims `name` (a change when it had another), with Turnstile's answer when asked for */
  async claim(name: string, captcha: string | null): Promise<Claimed> {
    try {
      const r = await this.f('/names', {
        method: 'POST', headers: { ...await this.headers(), 'content-type': 'application/json' }, cache: 'no-store',
        body: JSON.stringify({ name, ...(captcha ? { captcha } : {}) }), signal: AbortSignal.timeout(20_000),
      });
      const j = await r.json().catch(() => ({})) as { mine?: Mine; error?: ClaimError; next?: number };
      if (r.ok && j.mine) return { ok: true, mine: j.mine };
      if (r.status === 429 && !j.error) return { ok: false, error: 'busy' };
      return { ok: false, error: j.error ?? 'net', ...(j.next ? { next: j.next } : {}) };
    } catch { return { ok: false, error: 'net' }; }
  }
}

// ---- Cloudflare Turnstile ----

interface TurnstileApi {
  render(el: HTMLElement, o: Record<string, unknown>): string;
  remove(id: string): void;
}
let script: Promise<TurnstileApi> | null = null;

function load(): Promise<TurnstileApi> {
  script ??= new Promise<TurnstileApi>((ok, no) => {
    const s = document.createElement('script');
    s.src = 'https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit';
    s.onload = () => { const ts = (window as unknown as { turnstile?: TurnstileApi }).turnstile; if (ts) ok(ts); else no(new Error('no turnstile')); };
    s.onerror = () => { s.remove(); no(new Error('the script did not load')); };
    document.head.append(s);
  }).catch(e => { script = null; throw e; });
  return script;
}

/**
 * Turnstile's widget for `siteKey` in `box`: its answer once it passed, null when it failed, timed out (`ms`) or was
 * stopped (`stop`).  `quiet`: the widget shows only when it wants a click (`asks` then says so); else always.
 */
export async function turnstile(siteKey: string, box: HTMLElement, o: { quiet?: boolean; asks?: () => void; stop?: AbortSignal; ms?: number } = {}): Promise<string | null> {
  let ts: TurnstileApi;
  try { ts = await load(); } catch { return null; }
  return new Promise<string | null>(done => {
    let id = '';
    const end = (v: string | null) => { clearTimeout(timer); try { if (id) ts.remove(id); } catch { /* gone */ } done(v); };
    const timer = setTimeout(() => end(null), o.ms ?? 120_000);
    o.stop?.addEventListener('abort', () => end(null), { once: true });
    try {
      id = ts.render(box, {
        sitekey: siteKey, action: 'claim', theme: 'dark', size: 'flexible',
        appearance: o.quiet ? 'interaction-only' : 'always',
        callback: (token: string) => end(token),
        'error-callback': () => end(null),
        'expired-callback': () => end(null),
        'before-interactive-callback': () => o.asks?.(),
      });
    } catch { end(null); }
  });
}
