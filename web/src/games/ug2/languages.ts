// Underground 2's languages: settings.ini language's values (runtime/language.c), one setting for the page and the game
// (PLAN U-05).  The page has a language file for each but english (web/src/app/lang, web/tools/i18n-keys.mjs).
// No imports but a type: i18n.ts takes this alone.
import type { Language } from '../../app/i18n';

/** english first, the default */
export const LANGUAGES: readonly Language[] = ['english', 'french', 'german', 'italian', 'spanish', 'dutch', 'swedish', 'danish',
  'korean', 'chinese', 'japanese', 'russian', 'thai', 'portuguese', 'polish', 'turkish', 'hungarian'];
