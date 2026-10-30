// Underground 2's launch switch (web/src/games/profile.ts): off until the release that opens its new address, the one
// it shares with Most Wanted.  No imports: the page's leaf modules (vault.ts) take these alone.

/** on in the launch release, never before */
export const LAUNCHED = false;
/** the word a cloud-save keyphrase is shown with: the one both games share once launched, until then the one it
 *  always had (vault.ts reads either, and derives from PREFIX whatever is shown) */
export const KEYPHRASE_WORD: string = LAUNCHED ? 'nfsrevived' : 'nfsu2-xyz';
