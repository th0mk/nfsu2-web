// The worlds of free roam (players found the one of everyone too busy): the main world, everyone's (no Room set), World
// 1 to 4, which anyone can pick, and a world of the player's own by its name.  A world is a room as before (the Room
// setting, roam_room, and #room= links): the server's hub links only the players of one, its /players?rooms= counts
// each.  World 1 to 4 are rooms by a fixed English name whatever the page's language, so everyone who picks World 2
// meets there; the page shows their names translated.  World 4 has Ghost mode off for everyone in it (NO_GHOST): the
// page keeps its players' cars solid there, whatever Ghost mode they picked.  Pure (no DOM), so its test runs under node.
import { t } from './i18n';
import { roomHash } from './players';

/** World 1 to 4: the names they have in settings.ini and links, and the game */
export const WORLDS = ['World 1', 'World 2', 'World 3', 'World 4'] as const;
const HASHES = WORLDS.map(roomHash);

/** the world of World 1 to 4 where nobody drives through others: Ghost mode off for everyone in it */
export const NO_GHOST = 4;
/** which of World 1 to 4 room is (1 to 4), 0 none: the main world or a world of the player's own */
export function worldOf(room: string): number {
  const h = roomHash(room);
  return h ? HASHES.indexOf(h) + 1 : 0;
}
/** whether room is the main world: no name (or only blanks, which the game's hash leaves out too) */
export const isMain = (room: string) => !roomHash(room);
/** whether room is a world of the player's own: a name, not World 1 to 4 */
export const isOwn = (room: string) => !isMain(room) && !worldOf(room);

/** whether room is the world with Ghost mode off for everyone (NO_GHOST) */
export const noGhost = (room: string) => worldOf(room) === NO_GHOST;

/** room as the player reads it in a list: Main world, World 2 (in the page's language), World 4 with its rule, or its
 *  own name */
export function worldName(room: string): string {
  const n = worldOf(room);
  return n === NO_GHOST ? t('World {n} (Ghost mode off)', { n }) : isOwn(room) ? room.trim() : worldNamed(room);
}
/** room as a sentence names it: Main world, World 2, a world of the player's own in quotes */
export function worldNamed(room: string): string {
  if (isMain(room)) return t('Main world');
  const n = worldOf(room);
  return n ? t('World {n}', { n }) : t('"{name}"', { name: room.trim() });
}

/** the rooms /players?rooms= is asked for, in this order: the main world (0), World 1 to 4, then own if it is a world
 *  of the player's own */
export const worldRooms = (own: string): number[] => [0, ...HASHES, ...(isOwn(own) ? [roomHash(own)] : [])];
