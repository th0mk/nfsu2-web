// Most Wanted's controls on the page (src/app/controls.ts draws the sheet, src/app/touch.ts makes the touch controls;
// Underground 2's are src/games/ug2/controls.ts).
//   The controller: runtime/mw/pad_mw.c's layout (PLAN D-65, modern like Underground 2's), which it writes over the
// game's own for any controller the page gives the game (the game's own, for a pad it does not know by name, put
// pause on RT and a wheel's gas pedal; tests/mw/scripts/pad_binds.txt, web/tools/mw-binds.py): RT gas, LT brake and
// reverse, the left stick steers, LB nitrous, RB handbrake, A shift up, X shift down, Y camera, B look back, View
// respawn, Menu pause, the right stick pressed Speedbreaker; the D-pad as the game has it: up starts an event, left
// the World Map, right the SMS messages (tests/mw/scripts/pad_dpad.txt); its down, the game's "View Blacklist",
// brought up nothing in free roam there: not drawn.  A accepts and B goes back in the menus.  Four actions have no
// controller binding in either layout (pad_binds.txt): skipping a cutscene (Enter on the keyboard), the next song
// (T), the pulled-back camera (P) and the front end's FE_ACTION_B2 (T).
// pad_race.txt drives a race so.  A steering wheel shifts with its paddles, A (Cross) nitrous, X (Square) the
// handbrake, the rest as the pad's (WHEEL).
//   The touch controls press the game's own keys (runtime/mw: the keyboard is read as raw key states, so Shift, Ctrl
// and Alt are keys like the others): X nitrous (the action's second key; the first is Left Alt), Space the handbrake,
// Left Ctrl and Left Shift the gears, C the camera, L look back (held), G the Speedbreaker (the action's second key;
// Right Ctrl is the first), Enter Start in free roam (an event's marker); the stick and tilt steer with the
// controller's left stick, which the game binds to steering.
import type { PadLayout } from '../../app/controls';
import type { TouchSet } from '../../app/touch';
import type { WheelRow } from '../../app/wheel-setup';
import { N } from '../../app/i18n';

/** the controller drawing's rows (src/app/controls.ts Row): runtime/mw/pad_mw.c's layout */
export const PAD: PadLayout = {
  left: [
    { y: 36, xb: 'LT', ps: 'L2', does: N('Brake'), sub: N('and reverse'), to: [30, -5] },
    { y: 64, xb: 'LB', ps: 'L1', does: N('Nitrous'), to: [20.6, 31.2] },
    { y: 84, xb: 'LS', ps: 'L3', does: N('Steer'), to: [47, 54] },
    { y: 116, xb: 'up', ps: 'up', does: N('Start event') },
    { y: 130, xb: 'left', ps: 'left', does: N('Map') },
    { y: 144, xb: 'right', ps: 'right', does: N('Messages') },
  ],
  right: [
    { y: 36, xb: 'RT', ps: 'R2', does: N('Throttle'), to: [230, -5] },
    { y: 54, xb: 'RB', ps: 'R1', does: N('Handbrake'), to: [239.4, 31.2] },
    { y: 76, xb: 'A', ps: 'cross', does: N('Shift up') },
    { y: 90, xb: 'X', ps: 'square', does: N('Shift down') },
    { y: 104, xb: 'Y', ps: 'triangle', does: N('Camera') },
    { y: 118, xb: 'B', ps: 'circle', does: N('Look back') },
    { y: 140, xb: 'RS', ps: 'R3', does: N('Speedbreaker'), to: [162, 94] },
  ],
  middle: [
    { x: 150, y: 12, xb: 'view', ps: 'view', does: N('Respawn'), to: [112, 48.5], say: ['View', 'Create'] },
    { x: 172, y: 12, xb: 'menu', ps: 'menu', does: N('Pause'), to: [148, 48.5], say: ['Menu', 'Options'] },
  ],
  brackets: [{ x: 90.5, y0: 110, y1: 150, from: 123, to: [85, 94] }, { x: 244.5, y0: 70, y1: 124, from: 80, to: [217, 54] }],
  unused: [],
};

/** a steering wheel's list in the sheet (src/app/wheel-setup.ts): pad_mw.c's wheel layout */
export const WHEEL: readonly WheelRow[] = [
  { keys: N('Wheel and pedals'), does: N('Steer, throttle, brake and reverse') },
  { keys: N('Left and right paddles'), does: N('Shift down, shift up') },
  { keys: [['A', 'cross'], ['X', 'square']], does: N('Nitrous, handbrake') },
  { keys: [['B', 'circle'], ['Y', 'triangle']], does: N('Look back, camera') },
  { keys: [['view', 'Share'], ['menu', 'Options']], does: N('Respawn, pause') },
  { keys: [['RS', 'R3']], does: N('Speedbreaker') },
  { keys: [['up', 'up'], ['left', 'left'], ['right', 'right']], does: N('Start event, map, messages') },
];

/** the touch controls' keys (src/app/touch.ts): the game's own keys, no controller bits */
export const TOUCH: TouchSet = {
  camera: { keys: ['KeyC'] },
  lookBack: ['KeyL'],
  breaker: { keys: ['KeyG'], label: N('Speedbreaker') },
  nitro: { keys: ['ArrowUp', 'KeyX'] },
  handbrake: { keys: ['Space'] },
  gearDown: { keys: ['ControlLeft'] },
  gearUp: { keys: ['ShiftLeft'] },
  roam: ['start'],
};
