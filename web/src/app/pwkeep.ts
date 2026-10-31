// The cloud saves passphrase in the browser's own passwords, so that it outlives this site's data: cleared browsing
// data, or a disk running low, takes the saves and the link (vault-ui.ts, localStorage) with them, but not the
// passwords, which the browser may also sync to the player's other devices.  One entry per site, user name ID.
//
// Chrome, Edge and the other Chromium browsers have the Credential Management API's PasswordCredential (Chromium's
// components/password_manager CredentialManagerImpl and CredentialManagerPendingRequestTask; checked in Chrome 156
// over CDP with chrome://password-manager-internals, 2026-10-31):
// - store() of a new entry shows the browser's "Save password?" (nothing is kept until the player says Save); one with
//   the same user name is updated without a question, so a new passphrase replaces the old one.  No user gesture
//   needed; nothing in incognito, with "Offer to save passwords" off, or under --enable-automation (so no test
//   browser of ours ever shows it).
// - get() with mediation 'silent' hands the entry back without a question only when it is the site's only one, the
//   browser signs in by itself ("Sign in automatically", on by default) AND its one-time auto sign-in introduction was
//   shown before; on most profiles it was not, and the answer is empty.  With 'optional' the browser then shows its
//   own chooser ("Cloud saves"), which turns automatic sign-in on once picked; with no entry for the site it shows
//   nothing and answers empty at once.
// - Either way, a page whose box (index.html #vault-form: a hidden user name field and a password field) has a saved
//   entry is filled in by the browser as it loads.
// Firefox and Safari have no such API: their password managers, and extensions, fill that box, and may offer to save
// what the player typed or pasted there for Load.  A passphrase the page made is never typed, so it stays with Copy
// and Download there.

/** the entry's user name, in every language (a translated one would be a second entry) */
export const ID = 'Cloud saves';

const api = () => (window as unknown as { PasswordCredential?: new (o: { id: string; password: string }) => Credential }).PasswordCredential;

/** this browser can be asked to keep it */
export function canKeep(): boolean { return isSecureContext && typeof api() === 'function' && !!navigator.credentials; }

/** The browser asked to keep the phrase (its "Save password?"); false: no such API here, or it failed. */
export async function keepPhrase(phrase: string): Promise<boolean> {
  const P = api();
  if (!canKeep() || !P) return false;
  try { await navigator.credentials.store(new P({ id: ID, password: phrase })); return true; } catch { return false; }
}

/** The phrase the browser keeps for this site (null: none, the player closed the browser's chooser, or no such API).
 *  ask: the browser may show its chooser ('optional'); without, only what it gives without a question ('silent'). */
export async function keptPhrase(ask: boolean): Promise<string | null> {
  if (!canKeep()) return null;
  try {
    const c = await navigator.credentials.get({ password: true, mediation: ask ? 'optional' : 'silent' } as CredentialRequestOptions) as { type?: string; password?: unknown } | null;
    return c?.type === 'password' && typeof c.password === 'string' ? c.password : null;
  } catch { return null; }
}

/** a short mark of a phrase, kept to know it was offered to the browser (FNV-1a, not the phrase itself) */
export function markOf(phrase: string): string {
  let h = 0x811c9dc5;
  for (let i = 0; i < phrase.length; i++) { h ^= phrase.charCodeAt(i); h = Math.imul(h, 0x01000193); }
  return (h >>> 0).toString(16);
}
