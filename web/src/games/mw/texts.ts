// Most Wanted's words for what the page says where the games differ ('@game/texts'; Underground 2's:
// src/games/ug2/texts.ts).

import { N } from '../../app/i18n';

/** checks.ts: the page is not cross-origin isolated */
export const NOT_ISOLATED = N('The page is not cross-origin isolated (no SharedArrayBuffer). Open it from the Most Wanted Revived server, not another server.');
/** vault-ui.ts: a passphrase typed in here keeps Underground 2's cloud saves but none of Most Wanted's (the question
 *  'Use this passphrase here too?') */
export const OTHER_GAME_SAVES = N('This passphrase keeps your Underground 2 cloud saves but none of Most Wanted\'s yet. Send the saves here under it too? Each game keeps its own saves.');
