// What a run of Most Wanted takes from its game (web/src/games/profile.ts; Underground 2's beside it in
// src/games/ug2/run.ts).  No imports: the page's leaf modules (game.ts, logs.ts, privacy.ts) take these alone.

/** speed.exe in the game's folder (the server's /data; lower case, as the game's own folder has it) */
export const EXE = '/data/speed.exe';
/** what the game creates under the save root: "NFS Most Wanted/<ALIAS>/<ALIAS>" (GAME.md 10.1, docs/mw/save-format.md) */
export const PROFILES_DIR = 'NFS Most Wanted';
/** each game's first line in the log */
export const LOG_MARK = '=== NFSMW web';
/** the start of the files the page gives the player: the log (<it>-<time>.log), a backup (<it>-saves-<time>.zip), the
 *  keyphrase (<it>-passphrase.txt), an F12 photo in the save folder (<it>-<time>.png) */
export const FILE_PREFIX = 'nfsmw';
