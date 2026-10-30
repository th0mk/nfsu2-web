// What a run of Underground 2 takes from its game (web/src/games/profile.ts): the program, the folder of its profiles,
// and the names its log and backups go by.  No imports: the page's leaf modules (game.ts, logs.ts, privacy.ts) take
// these alone.

/** SPEED2.EXE in the game's folder (the server's /data) */
export const EXE = '/data/SPEED2.EXE';
/** what the game creates under the save root */
export const PROFILES_DIR = 'NFS Underground 2';
/** each game's first line in the log */
export const LOG_MARK = '=== NFSU2 web';
/** the start of the files the page gives the player: the log (<it>-<time>.log), a backup (<it>-saves-<time>.zip), the
 *  keyphrase (<it>-passphrase.txt), an F12 photo in the save folder (<it>-<time>.png) */
export const FILE_PREFIX = 'nfsu2';
