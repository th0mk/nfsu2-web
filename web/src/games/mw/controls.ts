// Most Wanted's controls on the page (src/app/controls.ts draws the sheet, src/app/touch.ts makes the touch controls;
// Underground 2's are src/games/ug2/controls.ts).
//   The controller: the game's own bindings for a pad it does not know by name, as its input set-up (0x63F850 ->
// 0x63CD80) writes them into the table 0x91F418 with a pad there from the start (tests/mw/scripts/pad_binds.txt,
// web/tools/mw-binds.py), in the button order the runtime gives Most Wanted's pad (runtime/mw/game_mw.c pad_buttons
// "X A B Y LB RB LT RT BACK START LS RS"): X gas, B brake, A handbrake, Y Speedbreaker, LT nitrous, LB look back, RB
// respawn, RT pause, the left stick steers, the D-pad's up starts an event (its down, left and right are HUD actions
// whose use is not known: not drawn); no gear shifts and no camera on the pad; A accepts, B goes back and Start is the
// menus' Start.  pad_race.txt drove a race with X, paused it with RT, and saw Start not pause it.
//   The touch controls press the game's own keys (runtime/mw: the keyboard is read as raw key states, so Shift, Ctrl
// and Alt are keys like the others): X nitrous (the action's second key; the first is Left Alt), Space the handbrake,
// Left Ctrl and Left Shift the gears, C the camera, L look back (held), G the Speedbreaker (the action's second key;
// Right Ctrl is the first), Enter Start in free roam (an event's marker); the stick and tilt steer with the
// controller's left stick, which the game binds to steering.
import type { PadLayout } from '../../app/controls';
import type { TouchSet } from '../../app/touch';
import { N } from '../../app/i18n';

/** the controller drawing's rows (src/app/controls.ts Row) */
export const PAD: PadLayout = {
  left: [
    { y: 36, xb: 'LT', ps: 'L2', does: N('Nitrous'), to: [30, -5] },
    { y: 64, xb: 'LB', ps: 'L1', does: N('Look back'), to: [20.6, 31.2] },
    { y: 84, xb: 'LS', ps: 'L3', does: N('Steer'), to: [47, 54] },
    { y: 116, xb: 'up', ps: 'up', does: N('Start event'), to: [98, 84] },
  ],
  right: [
    { y: 36, xb: 'RT', ps: 'R2', does: N('Pause'), to: [230, -5] },
    { y: 54, xb: 'RB', ps: 'R1', does: N('Respawn'), to: [239.4, 31.2] },
    { y: 76, xb: 'A', ps: 'cross', does: N('Handbrake') },
    { y: 90, xb: 'X', ps: 'square', does: N('Throttle') },
    { y: 104, xb: 'Y', ps: 'triangle', does: N('Speedbreaker') },
    { y: 118, xb: 'B', ps: 'circle', does: N('Brake'), sub: N('and reverse') },
  ],
  middle: [],
  brackets: [{ x: 244.5, y0: 70, y1: 124, from: 80, to: [217, 54] }],
  unused: ['rs', 'view', 'menu'],
};

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
