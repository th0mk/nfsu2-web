// Where the page is served: its base path, '/' where a server serves its game at the root (Underground 2 at
// https://nfsu2.xyz/), '/mw/' under a base (Most Wanted at https://nfsrevived.com/mw/, whose origin's root is the
// site's page).  Every address the page asks for is built from it: a request outside the base is no route of the
// game's.
//
// The base comes from where the running script came from (import.meta.url), so one build serves any base; the page
// has no <base> element (the server's Content-Security-Policy has base-uri 'none').  The scripts that use this file
// are main.js, which sits at the base, and the test pages' test/<name>.js, one folder below it; build.mjs keeps every
// bundle but main.js and sw.js one folder below the base (the workers and the worklet take the addresses they need
// from the page, absolute, and sw.js takes its own from its location).  Under node (the unit tests' bundles, from a
// data: or file: address) the base is '/' and addresses stay paths.

/** The base path of a script at this address ('/' or '/<folders>/'), and its origin ('' for one that is not a
 *  web server's). */
export function baseOf(script: string): { base: string; origin: string } {
  let u: URL;
  try { u = new URL(script); } catch { return { base: '/', origin: '' }; }
  if (u.protocol !== 'http:' && u.protocol !== 'https:') return { base: '/', origin: '' };
  return { base: new URL(/\/main\.js$/.test(u.pathname) ? './' : '../', u).pathname, origin: u.origin };
}

const here = baseOf(import.meta.url);
/** the page's base path: '/', '/mw/' */
export const BASE = here.base;
/** A path under the base, as the page asks for it: at('players') is '/players' at the root, '/mw/players' under /mw/. */
export const at = (rel: string) => BASE + rel;
/** The same address, absolute: for a worker, whose own address is not the page's, and for what it resolves
 *  against it (datacache.ts blockRuns). */
export const abs = (rel: string) => here.origin + at(rel);
