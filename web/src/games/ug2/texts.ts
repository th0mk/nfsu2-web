// Underground 2's words for what the page says where the games differ ('@game/texts'; Most Wanted's:
// src/games/mw/texts.ts).

import { N } from '../../app/i18n';

/** checks.ts: the page is not cross-origin isolated */
export const NOT_ISOLATED = N('The page is not cross-origin isolated (no SharedArrayBuffer). Open it from the NFSU2 web server.');
/** vault-ui.ts: a passphrase typed in here keeps Most Wanted's cloud saves but none of Underground 2's (the question
 *  'Use this passphrase here too?') */
export const OTHER_GAME_SAVES = N('This passphrase keeps your Most Wanted cloud saves but none of Underground 2\'s yet. Send the saves here under it too? Each game keeps its own saves.');
