// Most Wanted's page profile (web/src/games/profile.ts): what is its own so far, and Underground 2's values for the
// rest until the Most Wanted page work gives it its own.  Its page's head (head.html: title, description, social and
// search tags, favicon, JSON-LD) and its app files (static/: manifest.json and the icons, from web/tools/icons.mjs
// --accent '#b87c28', the colour of the game's menu highlight) are its own too (web/build.mjs).
import type * as self from './index';
import type { Profile } from '../profile';

export const GAME = 'mw';
/** the site's name, the top bar's tab text (PLAN D-31) */
export const SITE_NAME = 'Most Wanted Revived';
// its own
export { EXE, PROFILES_DIR, LOG_MARK, FILE_PREFIX } from './run';
export { REGISTRY_DEFAULTS } from './registry';
export { TEMPLATE as SETTINGS_TEMPLATE } from './template';
export { STORAGE } from './storage';
export { MOVIE_SIZE } from './movies';
export { LAUNCHED, KEYPHRASE_WORD } from './launch';
export { DERIVE_INFO as VAULT_DERIVE_INFO, SEAL_AAD as VAULT_AAD } from './keyphrase';
// the same as Underground 2's
// APP_DEFAULTS: each variable's reader is in the runtime both games share (runtime/core.list, runtime/web: com.c,
// autotest.c, audio_web.c, gpu_web.c, main_web.c) or the page; LANGUAGES: one setting for the page and the game
// (PLAN U-05), registry_mw.c maps it; VAULT_PREFIX: the one keyphrase (D-37)
export { APP_DEFAULTS, LANGUAGES, VAULT_PREFIX } from '../ug2';

export type Checked = Profile<typeof self>;
