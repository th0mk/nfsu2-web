// The page's own keys beside the game's: Message a friend's I (messages.ts), Add friend's F (friends.ts), quick chat's
// 1 to 4 and Numpad 1 to 4 (quickchat.ts).  None is one of the game's in its driving map, but a player can put any of
// the game's controls on any key (Options > Controls > Keyboard Configuration).  Then the page leaves that key to the
// game: the game's status "keys" (runtime/dinput_kbd.c page_keys) has a bit for each of them, set while the map the
// game's keyboard reads has one of its controls on that key (not the key's own default: Numpad 1 to 4 are its debug
// camera's, which it never reads).  The runtime's own keys (X drift mode, O the show-off, N the horn) and the
// controller's buttons (D-pad down held, the stick clicks) step aside in the runtime the same way.
import type { GameSession } from './game';

/** the status "keys"' bits, in the order of runtime/dinput_kbd.c's PAGE_KEYS */
export const KEY_MESSAGE = 0, KEY_FRIEND = 1;
const CHAT: Record<string, number> = { Digit1: 2, Digit2: 3, Digit3: 4, Digit4: 5, Numpad1: 6, Numpad2: 7, Numpad3: 8, Numpad4: 9 };
/** quick chat's key code's bit (-1: not one of quick chat's keys) */
export const chatBit = (code: string): number => CHAT[code] ?? -1;

/** whether the game has the page's key with that bit now (one of the game's controls is on it): the page leaves it */
export function gameHas(s: GameSession | null | undefined, bit: number): boolean {
  if (!s || bit < 0) return false;
  const v = s.status?.read()?.get('keys') ?? 0;
  return ((v >>> bit) & 1) === 1;
}
