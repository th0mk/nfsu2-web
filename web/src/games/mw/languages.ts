// Most Wanted's languages: settings.ini language's values, one setting for the page and the game (PLAN U-05;
// runtime/mw/registry_mw.c gives the game the ones it has text in).  Underground 2's, and Finnish, which only this
// game has: its page's Finnish file is its own (web/src/games/mw/lang/finnish.json, web/tools/i18n-keys.mjs).
// No imports but a type: i18n.ts takes this alone.
import type { Language } from '../../app/i18n';

/** english first, the default */
export const LANGUAGES: readonly Language[] = ['english', 'french', 'german', 'italian', 'spanish', 'dutch', 'swedish', 'danish',
  'finnish', 'korean', 'chinese', 'japanese', 'russian', 'thai', 'portuguese', 'polish', 'turkish', 'hungarian'];
