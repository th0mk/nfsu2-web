// The Controls sheet's steering wheel (controls.ts, the Controller tab): while a wheel is connected, what its controls
// do in the game (runtime/dinput_pad.c g_prof_wheel, the buttons as web/src/input/wheel.ts puts them), and its
// set-up, for a wheel the page reads wrong: let go, turn the wheel to full lock, press each pedal (WheelLearn).  The
// set-up is kept in this browser for that model, and the game's controller follows it at once (gamepad.ts).  While a
// game runs, the sheet is the wheel's only while the wheel drives (a controller takes over with a button pressed on it,
// gamepad.ts), and a line says so while one is connected too.  For a wheel with a shifter, what the shifter does
// (wheel.ts Shifter), kept in this browser for that model too.  An RC radio drives as a wheel: its sticks are the
// wheel and the pedals, and its set-up goes on with a switch for nitrous, the handbrake and a respawn (Skip passes
// one).

import { drivingPad, familyOf, mayDrive, PAD_DRIVER_EVENT } from '../input/gamepad';
import {
  guessMap, loadMap, loadShifter, saveMap, saveShifter, SHIFTER_MODES, shifterOf, WheelLearn, wheelOf, type LearnStep,
  type ShifterMode, type SwitchStep,
} from '../input/wheel';
import { N, t } from './i18n';

const SHIFTER_NAMES: Record<ShifterMode, string> = {
  'push-up': N('Sequential: push to shift up'),
  'pull-up': N('Sequential: pull to shift up'),
  handbrake: N('Handbrake'),
  off: N('Off'),
};
let sections = 0;

const STEPS: Record<Exclude<LearnStep, SwitchStep>, string> = {
  rest: N('Centre the wheel, then let go of it and of the pedals.'),
  steer: N('Turn the wheel right to where the car should steer all the way, and hold it there. A quarter turn suits this game.'),
  gas: N('Let go of the wheel. Press the gas pedal all the way down and hold it.'),
  brake: N('Let go of the gas. Press the brake pedal all the way down and hold it.'),
  done: N('Done: the wheel is set up in this browser.'),
};
const RADIO_STEPS: Record<LearnStep, string> = {
  rest: N('Centre the sticks, pull the throttle all the way down, and put the switches where they sit while you drive.'),
  steer: N('Push the stick that steers all the way right, and hold it there.'),
  gas: N('Let go of it. Push the throttle stick all the way up (or another stick, for the gas) and hold it.'),
  brake: N('Pull the throttle back down. Pull the right stick all the way back (or another stick, for the brake) and hold it.'),
  nitrous: N('Flip the switch or hold the button for nitrous. None? Skip.'),
  handbrake: N('Flip the switch or hold the button for the handbrake. None? Skip.'),
  respawn: N('Flip the switch or hold the button to respawn the car. None? Skip.'),
  done: N('Done: the radio is set up in this browser. Put its switches back where they sit while you drive.'),
};
const isRadio = (g: Gamepad) => wheelOf(g.id)?.layout === 'radio';

/** the wheel the sheet is about, one the browser has no standard mapping for: the one driving the game's controller
 *  while a game runs, else the first connected one; or null */
function connectedWheel(): Gamepad | null {
  const wheel = (g: Gamepad | null) => !!g?.connected && g.mapping !== 'standard' && !!wheelOf(g.id);
  try {
    const d = drivingPad();
    if (d) return wheel(d) ? d : null;
    return navigator.getGamepads?.().find(wheel) ?? null;
  } catch { return null; }
}

/** whether a controller besides the wheel at index is connected that could drive instead (gamepad.ts) */
function otherPad(index: number): boolean {
  try { return navigator.getGamepads?.().some(g => !!g?.connected && g.index !== index && mayDrive(g)) ?? false; }
  catch { return false; }
}

const esc = (s: string) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

/**
 * The wheel's part of the Controller tab, after the drawing (hidden with no wheel; the drawing hides while one is
 * there): box, which the sheet puts after the drawing each time it draws it; relabel draws it again in the page's
 * language.  cap: a key cap with a button's Xbox and PlayStation names (the sheet shows its family's).
 */
export function wheelSection(pad: HTMLElement, cap: (xb: string, ps: string) => string) {
  const box = document.createElement('div');
  box.className = 'wheel';
  box.hidden = true;
  const $ = <T extends HTMLElement>(s: string) => box.querySelector<T>(s)!;
  const uid = `wheel${++sections}`;
  let learnBox: HTMLElement, step: HTMLElement, fill: HTMLElement, learnBtn: HTMLButtonElement, guessBtn: HTMLButtonElement;
  let skipBtn: HTMLButtonElement, shifterRow: HTMLElement, shifterSel: HTMLSelectElement;
  /** what a set-up going on shows now (its step), and the line under the list ('' for the usual one) */
  let stepText = '', stateText = '';

  function draw() {
    const row = (dt: string, dd: string) => `<div><dt>${dt}</dt><dd>${esc(t(dd))}</dd></div>`;
    box.innerHTML = `<p class="wheel-name"></p>
    <dl class="keys wheel-keys">
      ${row(esc(t('Wheel and pedals')), N('Steer, throttle, brake and reverse'))}
      ${row(esc(t('Left and right paddles')), N('Shift down, shift up'))}
      ${row(`${cap('A', 'cross')} ${cap('X', 'square')}`, N('Nitrous, handbrake'))}
      ${row(`${cap('B', 'circle')} ${cap('Y', 'triangle')}`, N('Look back, camera'))}
      ${row(`${cap('view', 'Share')} ${cap('menu', 'Options')}`, N('Respawn, pause'))}
      ${row(`${cap('up', 'up')} ${cap('left', 'left')} ${cap('right', 'right')}`, N('Start event, map, messages'))}
    </dl>
    <dl class="keys radio-keys" hidden>
      ${row(esc(t('Sticks')), N('Steer, throttle, brake and reverse'))}
      ${row(esc(t('Switches you set up')), N('Nitrous, handbrake and respawn'))}
      ${row(esc(t('Keyboard')), N('Everything else'))}
    </dl>
    <div class="set wheel-shifter" hidden>
      <label class="set-label" for="${uid}-shifter">${esc(t('Shifter'))}</label>
      <p class="set-help" id="${uid}-shifter-help">${esc(t('The game shifts one gear at a time. Sequential: each move of the lever into a gear is one shift, and reverse holds the handbrake. Handbrake: any gear holds it.'))}</p>
      <div class="set-control"><select id="${uid}-shifter" aria-describedby="${uid}-shifter-help">${SHIFTER_MODES.map(m =>
        `<option value="${m}">${esc(t(SHIFTER_NAMES[m]))}</option>`).join('')}</select></div>
    </div>
    <p class="hint wheel-other" hidden>${esc(t('With a controller connected too, the game uses whichever you last pressed a button on.'))}</p>
    <p class="hint radio-other" hidden>${esc(t('With a controller connected too, the game uses whichever you last used: a button, or the radio\'s sticks or switches.'))}</p>
    <div class="pad-foot">
      <p class="hint wheel-state"></p>
      <div class="row">
        <button type="button" class="text-btn" data-wheel="guess" hidden>${esc(t('Use the guess'))}</button>
        <button type="button" class="btn" data-wheel="learn">${esc(t('Set up the wheel'))}</button>
      </div>
    </div>
    <div class="wheel-learn"${learn ? '' : ' hidden'}>
      <p class="wheel-step" aria-live="polite"></p>
      <div class="bar"><div class="bar-fill"></div></div>
      <button type="button" class="text-btn" data-wheel="skip" hidden>${esc(t('Skip'))}</button>
      <button type="button" class="text-btn" data-wheel="cancel">${esc(t('Cancel'))}</button>
    </div>`;
    learnBox = $('.wheel-learn'); step = $('.wheel-step'); fill = $('.bar-fill');
    learnBtn = $<HTMLButtonElement>('[data-wheel="learn"]'); guessBtn = $<HTMLButtonElement>('[data-wheel="guess"]');
    skipBtn = $<HTMLButtonElement>('[data-wheel="skip"]');
    shifterRow = $('.wheel-shifter'); shifterSel = $<HTMLSelectElement>('.wheel-shifter select');
    shifterSel.onchange = () => { if (id) saveShifter(id, shifterSel.value as ShifterMode); };
    if (learn) { showStep(); learnBtn.disabled = true; }
    learnBtn.onclick = start;
    $<HTMLButtonElement>('[data-wheel="cancel"]').onclick = () => stop();
    skipBtn.onclick = () => { if (learn?.skip()) showStep(); };
    guessBtn.onclick = () => { saveMap(id, null); stateText = ''; refresh(); };
  }

  let id = '', radio = false, learn: WheelLearn | null = null, raf = 0;

  /** the set-up's step as it is now, Skip only for a switch's */
  function showStep() {
    if (!learn) return;
    stepText = (radio ? RADIO_STEPS : STEPS as Record<LearnStep, string>)[learn.step];
    step.textContent = t(stepText);
    skipBtn.hidden = !(learn.step === 'nitrous' || learn.step === 'handbrake' || learn.step === 'respawn');
  }

  /** show the wheel connected now (or none), unless a set-up is going on */
  function refresh() {
    if (learn) return;
    const g = connectedWheel();
    id = g?.id ?? '';
    box.hidden = !g;
    for (const el of pad.children) if (el !== box) (el as HTMLElement | SVGElement).style.display = g ? 'none' : '';
    if (!g) return;
    radio = isRadio(g);
    $('.wheel-name').textContent = familyOf(g.id).name;
    $('.wheel-keys').hidden = radio;
    $('.radio-keys').hidden = !radio;
    $('.wheel-other').hidden = radio || !otherPad(g.index);
    $('.radio-other').hidden = !radio || !otherPad(g.index);
    shifterRow.hidden = !shifterOf(wheelOf(g.id)!, g.buttons.length);
    shifterSel.value = loadShifter(g.id);
    const kept = !!loadMap(g.id);
    $('.wheel-state').textContent = stateText ? t(stateText) : kept ? t('Set up in this browser.')
      : radio ? t('Steering, gas and brake are a guess, and the switches do nothing until you set it up.')
      : t('How it steers and brakes is a guess from how the browser reports it. Wrong? Set it up.');
    guessBtn.hidden = !kept;
    learnBtn.textContent = kept ? t('Set up again') : radio ? t('Set up the radio') : t('Set up the wheel');
  }

  /** the set-up ends; msg: what the line under the list says then */
  function stop(msg?: string) {
    cancelAnimationFrame(raf);
    learn = null;
    learnBox.hidden = true;
    learnBtn.disabled = false;
    stateText = msg ?? '';
    refresh();
  }

  function tick() {
    // the tab or the dialog closed: no set-up behind the player's back
    if (!learn || !(box.checkVisibility?.() ?? box.offsetParent !== null)) { stop(); return; }
    const g = navigator.getGamepads?.().find(p => !!p?.connected && p.id === id);
    if (!g) { stop(radio ? N('The radio went away before its set-up was done.') : N('The wheel went away before its set-up was done.')); return; }
    const pressed = radio ? g.buttons.map(b => !!b && (b.pressed || b.value > 0.5)) : undefined;
    if (learn.feed(Array.from(g.axes), performance.now(), pressed)) showStep();
    fill.style.width = `${Math.round(learn.progress * 100)}%`;
    if (learn.step === 'done') {
      saveMap(id, learn.map());
      stop(radio ? RADIO_STEPS.done : STEPS.done);
      return;
    }
    raf = requestAnimationFrame(tick);
  }

  function start() {
    const g = connectedWheel();
    if (!g) { refresh(); return; }
    id = g.id;
    radio = isRadio(g);
    const info = wheelOf(g.id)!;
    learn = new WheelLearn(loadMap(g.id) ?? guessMap(info, Array.from(g.axes), g.buttons.length), radio);
    stateText = '';
    showStep();
    fill.style.width = '0%';
    learnBox.hidden = false;
    learnBtn.disabled = true;
    raf = requestAnimationFrame(tick);
  }

  draw();
  addEventListener('gamepadconnected', () => { stateText = ''; refresh(); });
  addEventListener('gamepaddisconnected', () => { stateText = ''; refresh(); });
  addEventListener(PAD_DRIVER_EVENT, () => refresh());
  refresh();
  return { box, refresh, relabel() { draw(); refresh(); } };
}
