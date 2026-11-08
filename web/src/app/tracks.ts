// Tracks (the page menu's, F9): career free roam on the race tracks' own maps, the drag strips, drift arenas, Street X
// tracks and Speedways (runtime/travel.c), and back to the city.  The game goes there through a Loading screen, starts
// the car on a race's start line, and meets there only the players at the same spot: free roam's room with the spot's
// key mixed in (roam_game.c room_at), so /players?rooms= counts each spot's players as a world's.  The game tells where
// the car is and why a trip cannot go now in its status's "travel".  Pure (no DOM), so its test runs under node.
import { N, t } from './i18n';
import { roomHash } from './players';

/** the spots, in travel.c's SPOTS order (the game's number for one: its index + 1), each its room's key (a layout's
 *  number of its track) and its name: the race type and the place as the game's career names its races there
 *  (TRACK_NAME_<key>: Airport Circuit Track 1, Bayview Speedway Track 1, Parkade Drift 1 ...) */
export const TRACKS: readonly { key: number; name: string }[] = [
  { key: 4711, name: N('URL: Airport Circuit') },
  { key: 4701, name: N('URL: Bayview Speedway') },
  { key: 4311, name: N('Drift: Stadium') },
  { key: 4301, name: N('Drift: Parkade') },
  { key: 4201, name: N('Drag: Bayview Bridge') },
  { key: 4211, name: N('Drag: Central Station') },
  { key: 4221, name: N('Drag: South Runway') },
  { key: 4601, name: N('Street X: Industrial Park') },
  { key: 4605, name: N('Street X: Parkade') },
];

/** the room of the spot with this key in the world whose room's hash is base (0 the main world): roam_game.c room_at */
export function trackRoom(base: number, key: number): number {
  return ((base ^ Math.imul(key, 0x9e3779b1)) >>> 0) || 1;
}
/** the race track's spot a friend in free roam is at (the server's whereabouts: `room` its world's name, `at` its room's
 *  hash, the world's or a spot's there): 1.. TRACKS', 0 the city (or not known) */
export function friendSpot(room: string | undefined, at: number | undefined): number {
  if (typeof at !== 'number') return 0;
  const bases = room !== undefined ? [roomHash(room)] : [0];
  for (const base of bases) {
    const k = TRACKS.findIndex(s => trackRoom(base, s.key) === at);
    if (k >= 0) return k + 1;
  }
  return 0;
}
/** the rooms /players?spots= is asked for, in TRACKS' order, in the world whose room's hash is base */
export const trackRooms = (base: number): number[] => TRACKS.map(s => trackRoom(base, s.key));
/** A friend's world as the server's whereabouts name it, held against its room's hash (`at`: its world's, or a race
 *  track's spot's there): '' the main world (at 0, or one of its spots, whatever name came with it); the name when
 *  its hash leads to at; none when it does not (a name its page told before it moved on, a server's stand-in for a
 *  name it would not keep), so no Join takes the player to a world the friend is not in.  Without at (a server of
 *  before), the name as it came. */
export function friendRoom(room: string | undefined, at: number | undefined): string | undefined {
  if (typeof at !== 'number') return room;
  if (at === 0 || trackRooms(0).includes(at)) return '';
  if (room === undefined) return undefined;
  const base = roomHash(room);
  return base === at || trackRooms(base).includes(at) ? room : undefined;
}

/** why a trip cannot go now (travel.c WHY_*): 0 it can */
export const WHY_GOING = 6, WHY_COOP = 7;
const WHY: readonly string[] = [
  '',
  N('Only while you drive in free roam.'),
  N('Buy a car of your own first.'),
  N('Not during an outrun, a race or a race lobby.'),
  N('Not during a timed career task.'),
  N('Drive off the marker first.'),
  N('On the way…'),
  N('Not with two players on one screen.'),
];

/** the game's "travel" (travel.c publish: 65536 + why * 256 + where): where the car is (0 the city, k TRACKS[k - 1])
 *  and why a trip cannot go now; null: a game without trips (or none running) */
export function travelState(v: number | undefined | null): { at: number; why: number } | null {
  if (typeof v !== 'number' || !Number.isInteger(v) || v < 65536) return null;
  const w = v - 65536, at = w & 255, why = w >> 8;
  return at <= TRACKS.length && why < WHY.length ? { at, why } : null;
}
/** why as the player reads it ('' a trip can go) */
export const whyText = (why: number): string => (WHY[why] ? t(WHY[why]) : '');
/** the game's word on the page's trip to a friend (travel.c said: "travel_said" = count * 256 + why * 16 + what), null
 *  none or no new one since `last` (the count it had) */
export const SAID_THERE = 1, SAID_LOCKED = 2, SAID_NOT_FOUND = 3, SAID_NOT_NOW = 4;
export function travelSaid(v: number | undefined | null, last: number): { n: number; what: number; why: number } | null {
  if (typeof v !== 'number' || !Number.isInteger(v) || v < 256) return null;
  const n = Math.floor(v / 256), what = v & 15, why = (v >> 4) & 15;
  return n !== last ? { n, what, why } : null;
}
/** where the car is as a line names it: a spot's name, '' the city */
export const atName = (at: number): string => (at >= 1 && at <= TRACKS.length ? t(TRACKS[at - 1].name) : '');
