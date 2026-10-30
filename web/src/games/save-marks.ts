// What tells one game's saves from another's in a zip given to the Saves section (web/src/app/saves-ui.ts readBackup).
// Both games' pages back up in the same shape (registry.txt, settings.ini, the profiles' folder), and a backup's name
// (nfsu2-saves-…, nfsmw-saves-…) is the player's to change: without these, a backup of one game's page restored on the
// other's would replace that game's saves with files it does not read.  Every game's marks are here, as each page tells
// its own from the others' (web/test/games.test.mjs checks them against each game's own files).  No imports but the
// games' run.ts, which have none.
import { PROFILES_DIR as UG2_PROFILES } from './ug2/run';
import { PROFILES_DIR as MW_PROFILES } from './mw/run';

export interface SaveMarks {
  /** the game's id: its folder under web/src/games */
  readonly game: string;
  /** its name in the Saves section's messages */
  readonly title: string;
  /** the folder it keeps its profiles in (its run.ts PROFILES_DIR): a folder of that name anywhere in the zip, which
   *  finds the PC game's folder zipped too */
  readonly profilesDir: string;
  /** how its settings.ini starts: its template's first line, as far as the game's name */
  readonly settings: string;
  /** a line of its registry.txt ("d <name>=<value>"): the value the game writes at its first start, which its
   *  registry-defaults.txt seeds */
  readonly registry: RegExp;
}

export const SAVE_MARKS: readonly SaveMarks[] = [
  // NotFirstTime: runtime/win32_ext.c; the Mac app's settings.ini starts as the page's does
  { game: 'ug2', title: 'Need for Speed Underground 2', profilesDir: UG2_PROFILES, settings: '# NFSU2 settings',
    registry: /^(?:\S+ )?NotFirstTime=/m },
  // FirstTime: runtime/mw/registry_mw.c
  { game: 'mw', title: 'Need for Speed Most Wanted', profilesDir: MW_PROFILES, settings: '# NFS Most Wanted settings',
    registry: /^(?:\S+ )?FirstTime=/m },
];
