// Underground 2's page profile (web/src/games/profile.ts): the values the page uses today, from where it reads them.
import type * as self from './index';
import type { Profile } from '../profile';

export const GAME = 'ug2';
/** index.html and manifest.json still carry it as text */
export const SITE_NAME = 'Underground 2 Revived';
export { PROFILES_DIR } from '../../app/savefs';
export { default as REGISTRY_DEFAULTS } from '../../../../app/registry-defaults.txt';
export { TEMPLATE as SETTINGS_TEMPLATE, APP_DEFAULTS } from '../../app/settings';
export { LANGUAGES } from '../../app/i18n';
export { PREFIX as VAULT_PREFIX } from '../../app/vault';
export { STORAGE } from './storage';
export { LAUNCHED, KEYPHRASE_WORD } from './launch';

export type Checked = Profile<typeof self>;
