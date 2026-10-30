// The game the page is built for.  One page (web/src) serves each game; what differs between them comes from the
// game's profile, web/src/games/<id>/index.ts, which `node build.mjs --game <id>` puts behind the import path '@game'
// (ug2 when no --game is given; tsconfig.json points '@game' at ug2 too, for tsc and the tests' bundles), and
// '@game/<file>' at a file of that folder: '@game/storage' is the profile's STORAGE alone, for the service worker,
// which must not take the rest of the page with it.  Each
// profile exports every field below, and tsc checks that (its Checked type).  A game's own page texts go in
// web/src/games/<id>/lang/<language>.json, over the shared web/src/app/lang/<language>.json (build.mjs merges them,
// web/tools/i18n-keys.mjs checks them).
//
// The values still sit where the page reads them (savefs.ts, settings.ts, ...): ug2/index.ts re-exports them, and no
// part of the page imports '@game' yet.  What else differs per game (the exe path, the save format, the controls, the
// storage names, index.html's head ...) moves in here with the work that gives Most Wanted its own.

/** What a game's profile exports. */
export interface GameProfile {
  /** the game's id: its folder under web/src/games and build.mjs --game */
  readonly GAME: string;
  /** the site's name */
  readonly SITE_NAME: string;
  /** the folder the game keeps its profiles in, at the top of the save folder */
  readonly PROFILES_DIR: string;
  /** registry.txt as a first launch writes it */
  readonly REGISTRY_DEFAULTS: string;
  /** settings.ini as a first launch writes it */
  readonly SETTINGS_TEMPLATE: string;
  /** the runtime variables the page sets when neither settings.ini nor the address does */
  readonly APP_DEFAULTS: Readonly<Record<string, string>>;
  /** the game's text languages (settings.ini language), english first */
  readonly LANGUAGES: readonly string[];
  /** a cloud-save keyphrase's first word */
  readonly VAULT_PREFIX: string;
  /** the game's names in the browser's storage (src/games/<id>/storage.ts) */
  readonly STORAGE: StorageNames;
}

/** A game's names in the storage the browser keeps per origin (Cache Storage, OPFS, local and session storage, Web
 *  Locks).  Where both games are served from one origin (/u2/ and /mw/), their pages share that storage, so each
 *  game's names are its own: Underground 2 keeps the names it always had, Most Wanted has others. */
export interface StorageNames {
  /** offline mode's copies (sw.ts): the Cache Storage it keeps them in, and the start of the names its older
   *  versions used, which it deletes once it runs (never another game's: the games' starts differ) */
  readonly OFFLINE_CACHE: string;
  readonly OFFLINE_PREFIX: string;
}

/** T, which tsc checks is a whole profile: each profile's `export type Checked = Profile<typeof self>` */
export type Profile<T extends GameProfile> = T;
