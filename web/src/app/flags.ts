// The flag a player shows before their name over their car in free roam (settings.ini roam_flag; the start page's
// Your flag row): none until they pick one, and the list puts the country their connection comes from first (the
// server's /country, Cloudflare's guess, which nobody else sees).  On the wire and in settings.ini a flag is its code
// here, lower case; the game draws it from runtime/flags.dat (web/tools/mkflags.mjs, the same list in the same order)
// and shows none for a code it does not have.  Pure (no DOM): settings.ts checks the setting with it.
import { N, t, LOCALE, language } from './i18n';

/** every flag: each country of ISO 3166-1, Kosovo (xk, as Cloudflare names it), the United Kingdom's nations and
 *  Quebec (ca-qc) */
export const FLAG_CODES = [
  'ad', 'ae', 'af', 'ag', 'ai', 'al', 'am', 'ao', 'aq', 'ar', 'as', 'at', 'au', 'aw', 'ax', 'az', 'ba', 'bb', 'bd',
  'be', 'bf', 'bg', 'bh', 'bi', 'bj', 'bl', 'bm', 'bn', 'bo', 'bq', 'br', 'bs', 'bt', 'bv', 'bw', 'by', 'bz', 'ca',
  'ca-qc', 'cc', 'cd', 'cf', 'cg', 'ch', 'ci', 'ck', 'cl', 'cm', 'cn', 'co', 'cr', 'cu', 'cv', 'cw', 'cx', 'cy', 'cz', 'de',
  'dj', 'dk', 'dm', 'do', 'dz', 'ec', 'ee', 'eg', 'eh', 'er', 'es', 'et', 'fi', 'fj', 'fk', 'fm', 'fo', 'fr', 'ga',
  'gb', 'gb-eng', 'gb-nir', 'gb-sct', 'gb-wls', 'gd', 'ge', 'gf', 'gg', 'gh', 'gi', 'gl', 'gm', 'gn', 'gp', 'gq',
  'gr', 'gs', 'gt', 'gu', 'gw', 'gy', 'hk', 'hm', 'hn', 'hr', 'ht', 'hu', 'id', 'ie', 'il', 'im', 'in', 'io', 'iq',
  'ir', 'is', 'it', 'je', 'jm', 'jo', 'jp', 'ke', 'kg', 'kh', 'ki', 'km', 'kn', 'kp', 'kr', 'kw', 'ky', 'kz', 'la',
  'lb', 'lc', 'li', 'lk', 'lr', 'ls', 'lt', 'lu', 'lv', 'ly', 'ma', 'mc', 'md', 'me', 'mf', 'mg', 'mh', 'mk', 'ml',
  'mm', 'mn', 'mo', 'mp', 'mq', 'mr', 'ms', 'mt', 'mu', 'mv', 'mw', 'mx', 'my', 'mz', 'na', 'nc', 'ne', 'nf', 'ng',
  'ni', 'nl', 'no', 'np', 'nr', 'nu', 'nz', 'om', 'pa', 'pe', 'pf', 'pg', 'ph', 'pk', 'pl', 'pm', 'pn', 'pr', 'ps',
  'pt', 'pw', 'py', 'qa', 're', 'ro', 'rs', 'ru', 'rw', 'sa', 'sb', 'sc', 'sd', 'se', 'sg', 'sh', 'si', 'sj', 'sk',
  'sl', 'sm', 'sn', 'so', 'sr', 'ss', 'st', 'sv', 'sx', 'sy', 'sz', 'tc', 'td', 'tf', 'tg', 'th', 'tj', 'tk', 'tl',
  'tm', 'tn', 'to', 'tr', 'tt', 'tv', 'tw', 'tz', 'ua', 'ug', 'um', 'us', 'uy', 'uz', 'va', 'vc', 've', 'vg', 'vi',
  'vn', 'vu', 'wf', 'ws', 'xk', 'ye', 'yt', 'za', 'zm', 'zw'
] as const;
export type FlagCode = typeof FLAG_CODES[number];
const CODES = new Set<string>(FLAG_CODES);
/** /flags.png (web/build.mjs, from runtime/flags.dat): every flag at 48x36 in FLAG_CODES' order, this many a row */
export const FLAG_SHEET_COLS = 16;
export const FLAG_W = 48, FLAG_H = 36;
/** where a flag is in /flags.png, in its pixels ([x, y] of its top left), or null for none */
export function flagAt(code: string): [number, number] | null {
  const i = FLAG_CODES.indexOf(code as FlagCode);
  return i < 0 ? null : [(i % FLAG_SHEET_COLS) * FLAG_W, Math.floor(i / FLAG_SHEET_COLS) * FLAG_H];
}

/** a setting's text as a flag's code (any case, spaces round it), '' for none or anything else */
export function flagCode(v: string): FlagCode | '' {
  const c = v.trim().toLowerCase();
  return CODES.has(c) ? c as FlagCode : '';
}

/** the nations of the United Kingdom and Quebec, which have no region name of their own in the browser */
const NATIONS: Record<string, string> = {
  'gb-eng': N('England'), 'gb-sct': N('Scotland'), 'gb-wls': N('Wales'), 'gb-nir': N('Northern Ireland'),
  'ca-qc': N('Quebec'),
};
let names: Intl.DisplayNames | null = null, namesFor = '';
/** the flag's name in the page's language */
export function flagName(code: FlagCode): string {
  if (code in NATIONS) return t(NATIONS[code]);
  const loc = LOCALE[language()];
  if (namesFor !== loc) {
    namesFor = loc;
    try { names = new Intl.DisplayNames([loc, 'en'], { type: 'region' }); } catch { names = null; }
  }
  try { return names?.of(code.toUpperCase()) ?? code.toUpperCase(); } catch { return code.toUpperCase(); }
}

/** every flag by its name in the page's language */
export function flagsByName(): { code: FlagCode; name: string }[] {
  const loc = LOCALE[language()];
  return FLAG_CODES.map(code => ({ code, name: flagName(code) })).sort((a, b) => a.name.localeCompare(b.name, loc));
}
