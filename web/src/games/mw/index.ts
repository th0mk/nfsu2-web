// Most Wanted's page profile (web/src/games/profile.ts), a placeholder: the site's name is not chosen yet, and every
// other value is still Underground 2's until the Most Wanted page work gives it its own.
import type * as self from './index';
import type { Profile } from '../profile';

export const GAME = 'mw';
/** a neutral stand-in until the site's name is chosen */
export const SITE_NAME = 'Most Wanted';
// Underground 2's, for now
export { PROFILES_DIR, REGISTRY_DEFAULTS, SETTINGS_TEMPLATE, APP_DEFAULTS, LANGUAGES, VAULT_PREFIX } from '../ug2';
// its own
export { STORAGE } from './storage';

export type Checked = Profile<typeof self>;
