// The names in the browser's storage that both games' pages use alike (PLAN D-36), listed here once: every other name
// is a game's own (src/games/<id>/storage.ts, profile.ts StorageNames).  The two games' sites are two origins, each
// with its own storage, so these are shared only where one origin serves both pages (a test server's).  No imports.
export const SHARED_STORAGE = {
  /** a wheel's set-up and its shifter's, the wheel's USB ids after it (input/wheel.ts): the same wheel in both
   *  games (assumed, D-36) */
  WHEEL: 'nfsu2.wheel.',
  SHIFTER: 'nfsu2.shifter.',
  /** the start page's language before settings.ini is read (i18n.ts): the same player (assumed, D-36) */
  PAGE_LANGUAGE: 'nfsu2-page-language',
  /** not shared, but every game's: the names at the top of the origin private file system that are a game's (each
   *  game's SAVE_DIR, CACHE_DIR and PAGE_ID_FILE), which no ?save=<name> and no test page's folder may be
   *  (app/save-dir.ts; web/test/storage-names.test.mjs checks the list against the games') */
  OPFS_TOP: ['save', 'cache', 'nfsu2.page-id', 'mw-save', 'mw-cache', 'nfsmw.page-id'],
} as const;
