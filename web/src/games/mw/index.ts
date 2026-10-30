// Most Wanted's page profile (web/src/games/profile.ts): what is its own so far, and Underground 2's values for the
// rest until the Most Wanted page work gives it its own.  Its page's head (head.html: title, description, social and
// search tags, favicon, JSON-LD) and its app files (static/: manifest.json and the icons, from web/tools/icons.mjs
// --accent '#b87c28', the colour of the game's menu highlight) are its own too (web/build.mjs).  Their English waits
// for the user's approval, the icons' colour for the user's pick.
import type * as self from './index';
import type { Profile } from '../profile';

export const GAME = 'mw';
/** the site's name, the top bar's tab text (PLAN D-31) */
export const SITE_NAME = 'Most Wanted Revived';
// Underground 2's, for now
export { PROFILES_DIR, REGISTRY_DEFAULTS, SETTINGS_TEMPLATE, APP_DEFAULTS, LANGUAGES, VAULT_PREFIX } from '../ug2';
// its own
export { STORAGE } from './storage';
export { LAUNCHED, KEYPHRASE_WORD } from './launch';
export { DERIVE_INFO as VAULT_DERIVE_INFO, SEAL_AAD as VAULT_AAD } from './keyphrase';

export type Checked = Profile<typeof self>;
