// The game the page is built for.  One page (web/src) serves each game; what differs between them comes from the
// game's profile, web/src/games/<id>/index.ts, which `node build.mjs --game <id>` puts behind the import path '@game'
// (ug2 when no --game is given; tsconfig.json points '@game' at ug2 too, for tsc and the tests' bundles), and
// '@game/<file>' at a file of that folder: '@game/storage' is the profile's STORAGE alone, for the service worker,
// which must not take the rest of the page with it.  Each
// profile exports every field below, and tsc checks that (its Checked type).  A game's own page texts go in
// web/src/games/<id>/lang/<language>.json, over the shared web/src/app/lang/<language>.json (build.mjs merges them,
// web/tools/i18n-keys.mjs checks them).
//
// The page reads what differs through '@game/<file>', each a file of the game's folder that takes no more than it
// needs: '@game/storage' (the browser's storage names), '@game/launch' and '@game/keyphrase' (cloud saves),
// '@game/run' (the program, the profiles' folder, the movies' size, the log's and the backup's names: no imports),
// '@game/registry' (registry.txt's seed) and '@game/profile' (the game's save files: SaveFormat below).  Other values
// still sit where the page reads them (settings.ts, i18n.ts, ...), and ug2/index.ts re-exports them; what else differs
// per game (the settings, the controls ...) moves in here with the work that gives Most Wanted its own.

import type { RenameFiles, SaveFiles } from '../app/profile';

/** What a game's profile exports. */
export interface GameProfile {
  /** the game's id: its folder under web/src/games and build.mjs --game */
  readonly GAME: string;
  /** the site's name */
  readonly SITE_NAME: string;
  /** the program the page starts, in the game's folder as the server serves it (/data) */
  readonly EXE: string;
  /** the folder the game keeps its profiles in, at the top of the save folder */
  readonly PROFILES_DIR: string;
  /** the start of the line each game's session opens the log with (logs.ts keeps the newest one's lines) */
  readonly LOG_MARK: string;
  /** the start of the files the page gives the player: the log, a backup, the keyphrase, an F12 photo (run.ts) */
  readonly FILE_PREFIX: string;
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

/** A game's save files as far as the page reads or changes them ('@game/profile': Underground 2's src/app/profile.ts,
 *  Most Wanted's src/games/mw/profile.ts): the profiles are "<PROFILES_DIR>/<NAME>/<NAME>", each the game's own
 *  "20CM" container.  What saves-ui.ts, savefs.ts, vault.ts and shell.ts use of it. */
export interface SaveFormat {
  readonly PROFILES_DIR: string;
  /** the longest name the game's own name field makes, and whether a name is one it makes */
  readonly NAME_MAX: number;
  isProfileName(s: string): boolean;
  /** a file that starts as the game's save files do ("20CM"): a profile, or one of another version or game */
  isProfileFile(file: Uint8Array): boolean;
  /** the name inside a profile from elsewhere, or why the page does not take it (in the page's language); edited: its
   *  checks do not hold, as a save editor leaves them, and it comes in as sealed() makes it */
  profileName(file: Uint8Array): { name: string; edited: boolean } | { bad: string };
  /** a profile's bank; null: not a profile of this game and version */
  profileMoney(file: Uint8Array): number | null;
  /** how many of a profile's cars are of a type the game does not have (a mod's) */
  modCars(file: Uint8Array): number;
  /** a copy of a profile profileName took as edited, its checks made again */
  sealed(file: Uint8Array): Uint8Array;
  /** a folder beside a profile's that belongs to it (Underground 2's DVD and magazine covers), or null */
  coverOf(folder: string): { profile: string; cover: string } | null;
  /** a profile renamed as if the game had made it under the new name, or why not (nothing written then) */
  renameProfile(fs: RenameFiles, profilesDir: string, from: string, to: string): Promise<{ files: number } | { refused: string }>;
  /** before a Play: the game's Save option at Autosave, once per profile (settings.ini autosave) */
  turnOnAutosave(fs: SaveFiles, profilesDir: string): Promise<{ turnedOn: string[]; notes: string[] }>;
  /** the save folder's file of the profiles turnOnAutosave has looked at */
  readonly AUTOSAVE_SEEN: string;
}

/** T, which tsc checks is a whole SaveFormat: each '@game/profile''s `export type Checked = Saves<typeof self>` */
export type Saves<T extends SaveFormat> = T;
