// Underground 2's controls on the page (src/app/controls.ts draws the sheet, src/app/touch.ts makes the touch
// controls; Most Wanted's are src/games/mw/controls.ts): what each button of a controller does in the game, and the keys
// and controller bits the touch controls press.
import type { PadLayout } from '../../app/controls';
import type { TouchSet } from '../../app/touch';
import { PAD_A, PAD_LB, PAD_RB, PAD_X } from '../../input/proto';
import { N } from '../../app/i18n';

/** the controller drawing's rows (src/app/controls.ts Row): runtime/pad_ug2.c's layout */
export const PAD: PadLayout = {
  left: [
    { y: 36, xb: 'LT', ps: 'L2', does: N('Brake'), sub: N('and reverse'), to: [30, -5] },
    { y: 64, xb: 'LB', ps: 'L1', does: N('Nitrous'), to: [20.6, 31.2] },
    { y: 84, xb: 'LS', ps: 'L3', does: N('Steer'), sub: N('press in: horn'), to: [47, 54] },
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
    { y: 140, xb: 'RS', ps: 'R3', does: N('Quick chat'), to: [162, 94] },
  ],
  middle: [
    { x: 150, y: 12, xb: 'view', ps: 'view', does: N('Respawn'), to: [112, 48.5], say: ['View', 'Create'] },
    { x: 172, y: 12, xb: 'menu', ps: 'menu', does: N('Pause'), to: [148, 48.5], say: ['Menu', 'Options'] },
  ],
  brackets: [{ x: 90.5, y0: 110, y1: 150, from: 123, to: [85, 94] }, { x: 244.5, y0: 70, y1: 124, from: 80, to: [217, 54] }],
  unused: [],
};

/** the touch controls' keys and controller bits (src/app/touch.ts) */
export const TOUCH: TouchSet = {
  camera: { keys: ['KeyC'], hold: ['KeyB'] },
  nitro: { keys: ['ArrowUp'], bit: 1 << PAD_LB },
  handbrake: { bit: 1 << PAD_RB },
  gearDown: { bit: 1 << PAD_X },
  gearUp: { bit: 1 << PAD_A },
  roam: ['map', 'msgs', 'drift', 'status', 'horn', 'chat', 'start'],
};
