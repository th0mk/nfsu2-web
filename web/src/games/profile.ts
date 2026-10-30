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
  /** the launch switch (src/games/<id>/launch.ts): Underground 2's, off until the release that opens its new
   *  address */
  readonly LAUNCHED: boolean;
  /** the word a keyphrase is shown with (the derivation always takes VAULT_PREFIX's) */
  readonly KEYPHRASE_WORD: string;
  /** how a keyphrase's key and token derive (src/games/<id>/keyphrase.ts): HKDF-SHA-256's info over its PBKDF2
   *  bytes (null: those bytes themselves, Underground 2's), and the additional data a box is sealed with */
  readonly VAULT_DERIVE_INFO: string | null;
  readonly VAULT_AAD: string;
  /** the movies' picture size (src/games/<id>/movies.ts), which the page asks the browser's AV1 decoder about */
  readonly MOVIE_SIZE: { readonly w: number; readonly h: number };
}

/** A game's names in the storage the browser keeps per origin (Cache Storage, OPFS, local and session storage, Web
 *  Locks, broadcast channels, IndexedDB).  Where both games are served from one origin (/u2/ and /mw/), their pages
 *  share that storage, so each game's names are its own: Underground 2 keeps the names it always had, Most Wanted has
 *  others (PLAN D-36).  The few names both games share are src/games/shared-storage.ts's, listed there once.  No part
 *  of the page names a place in that storage but through these (web/test/storage-names.test.mjs).  A name that ends in
 *  ':' or '.' is the start of a family: the rest is a save folder's name, a player's name or a wheel's ids. */
export interface StorageNames {
  /** offline mode's copies (sw.ts): the Cache Storage it keeps them in, and the start of the names its older
   *  versions used, which it deletes once it runs (never another game's: the games' starts differ) */
  readonly OFFLINE_CACHE: string;
  readonly OFFLINE_PREFIX: string;
  /** the origin private file system's top: the player's save folder (a development server's ?save=<name> picks
   *  another), the game data's cache (host/datacache.ts), and the file the page's LAN id is kept in (page-id.ts) */
  readonly SAVE_DIR: string;
  readonly CACHE_DIR: string;
  readonly PAGE_ID_FILE: string;
  /** the Web Lock that says which tab owns a save folder's saves (instance.ts), with the folder's name after it */
  readonly SAVES_LOCK: string;
  /** the broadcast channel invite links go to another tab of the same save folder on (shell.ts), the folder after it */
  readonly INVITE_CHANNEL: string;
  /** the IndexedDB database the radio's folder is kept in (radio-folder.ts) */
  readonly RADIO_DB: string;
  /** localStorage, each key's use beside it in the game's storage.ts */
  readonly LOCAL: LocalKeys;
  /** sessionStorage: the version a reload was for (shell.ts afterUpdate), the other build a reload was tried for */
  readonly SESSION: { readonly UPDATE: string; readonly OTHER_BUILD: string };
}

/** A game's own localStorage keys (StorageNames.LOCAL) */
export interface LocalKeys {
  readonly GPU_PIPELINES: string;
  readonly LOG_SALT: string;
  readonly RENDER_CHECK: string;
  readonly LAST_GAME: string;
  readonly PAGE_ID: string;
  readonly PLAYED_MS: string;
  readonly SENT_MS: string;
  readonly RADIO_MODES: string;
  readonly RADIO_ON: string;
  readonly RADIO_NEXT: string;
  readonly RADIO_SHUFFLE: string;
  readonly TOUCH_STEERING: string;
  readonly TOUCH_LEVEL: string;
  readonly TOUCH_SIZE: string;
  readonly MENU_SIZE: string;
  readonly KEYPHRASE_LINK: string;
  readonly CLOUD_OFFER: string;
  readonly NAME_ASKED: string;
  readonly NAME_CLAIMED: string;
  readonly CLAIM_TRIED: string;
}

/** T, which tsc checks is a whole profile: each profile's `export type Checked = Profile<typeof self>` */
export type Profile<T extends GameProfile> = T;
