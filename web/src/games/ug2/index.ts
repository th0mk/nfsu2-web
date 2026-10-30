// Underground 2's page profile (web/src/games/profile.ts): the values the page uses today, from where it reads them.
import type * as self from './index';
import type { Profile } from '../profile';

export const GAME = 'ug2';
/** index.html and manifest.json still carry it as text */
export const SITE_NAME = 'Underground 2 Revived';
export { EXE, PROFILES_DIR, LOG_MARK, FILE_PREFIX } from './run';
export { REGISTRY_DEFAULTS } from './registry';
export { TEMPLATE as SETTINGS_TEMPLATE } from './template';
export { APP_DEFAULTS } from '../../app/settings';
export { LANGUAGES } from '../../app/i18n';
export { PREFIX as VAULT_PREFIX } from '../../app/vault';
export { STORAGE } from './storage';
export { MOVIE_SIZE } from './movies';
export { LAUNCHED, KEYPHRASE_WORD } from './launch';
export { DERIVE_INFO as VAULT_DERIVE_INFO, SEAL_AAD as VAULT_AAD } from './keyphrase';

export type Checked = Profile<typeof self>;
