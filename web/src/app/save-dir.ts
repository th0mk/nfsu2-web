// Which folder at the top of the origin private file system a page keeps its saves in.  The start page's is the
// game's own (src/games/<id>/storage.ts SAVE_DIR) unless a development server's ?save=<name> names another; a test page
// (play, hostcheck, aicheck) empties its folder first, so its folder is never a player's.  Both games share the origin
// (PLAN D-36): no name may be another game's folder, or any game's game data cache or page-id file
// (src/games/shared-storage.ts OPFS_TOP).
import { STORAGE } from '@game/storage';
import { SHARED_STORAGE } from '../games/shared-storage';

const TOP: readonly string[] = SHARED_STORAGE.OPFS_TOP;

/** A save folder's name as an address gives it, made one path segment of letters, digits, '.', '_' and '-'; null when
 *  it may not be one: dots alone, or a name at the top of the origin private file system that is a game's (its save
 *  folder, game data cache or page-id file), unless it is `own`, the page's own save folder where it may use it. */
export function saveDirName(asked: string, own: string | null = null): string | null {
  const s = asked.replace(/[^A-Za-z0-9._-]/g, '_');
  return /^\.+$/.test(s) || (s !== own && TOP.includes(s)) ? null : s;
}

/** A test page's save folder: ?save=<name>, or the game's save folder's name and the page's (save-test, mw-save-play);
 *  never a player's folder or another name saveDirName refuses or changes: an error then. */
export function testSaveDir(asked: string | null, page: string): string {
  if (asked === null) return `${STORAGE.SAVE_DIR}-${page}`;
  if (saveDirName(asked) !== asked)
    throw new Error(`save=${asked}: a test page empties its save folder first, so it is never ${TOP.join(', ')} or dots, ` +
      'and its name is letters, digits, ".", "_" and "-"');
  return asked;
}
