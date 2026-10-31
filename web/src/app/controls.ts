// The Controls sheet: the start page's (#keys), and its copy in the menu's Controls dialog (shell.ts openHelp).
// Three tabs: the keyboard (on a touch screen: touch), the controller and this site.  The controller's tab is a
// drawing of a pad with what each button the game uses does beside it, the buttons named as Xbox or PlayStation
// names them (the switch under it; a PlayStation controller connecting, or taking over in the game, picks those
// names).  While a steering wheel is connected (in the game: drives), the tab is the wheel's instead (wheel-setup.ts).
// The touch tab has the steering's list, the controls' position (which thumb steers, by the sides or moved in), the
// switches for Nitro on the steering's side and the level horizon, the controls' size and the game's menu buttons'
// (touch.ts), the same in both copies and in step with the touch controls' own steering button (held, it opens the
// menu's copy at this tab).
//
// The drawing: a front view in its own units (260 x 180, x 130 the middle, the triggers above y 0) at SCALE in the
// SVG's 336 px, the labels in rows of px left and right of it and over its middle, each joined to its control by a
// line.  The D-pad's three rows and the face buttons' four share one line, from a bracket beside them.

import { drivingPad, familyOf, PAD_DRIVER_EVENT } from '../input/gamepad';
import { PADF_PLAYSTATION } from '../input/proto';
import { wheelSection } from './wheel-setup';
import { askMotion, levelHorizon, menuSize, nitroSteerSide, onSteering, setSteering, steering, STEERING, steeringName, touchPosition,
  touchSize, type ListSetting, type Steering, type SwitchSetting } from './touch';
import { language, N, onLanguage, t } from './i18n';

export type Tab = 'keys' | 'pad' | 'site';
export type Family = 'xb' | 'ps';

const SCALE = 0.56, MID = 168, TOP = 31 + 14 * SCALE;
const at = ([x, y]: readonly [number, number]) => [MID + SCALE * (x - 130), TOP + SCALE * y].map(v => +v.toFixed(1));

const BODY = 'M70 16C100 9 160 9 190 16C216 21 236 30 244 54C253 82 258 124 254 150C251 170 232 178 218 168C206 159 198 136 186 126' +
  'C176 119 158 117 130 117C102 117 84 119 74 126C62 136 54 159 42 168C28 178 9 170 6 150C2 124 7 82 16 54C24 30 44 21 70 16Z';
const BUMPERS = ['M15 42C22 22 40 12 76 7', 'M245 42C238 22 220 12 184 7'];
const DPAD = 'M93 81H103V89H111V99H103V107H93V99H85V89H93Z';

/** a button's name drawn in a 14 x 14 box, where a word does not do */
const SHAPE: Record<string, string> = {
  up: '<path d="M7 10.5V3.8M4.3 6.4 7 3.7l2.7 2.7"/>',
  left: '<path d="M10.5 7H3.8M6.4 4.3 3.7 7l2.7 2.7"/>',
  right: '<path d="M3.5 7h6.7M7.6 4.3 10.3 7l-2.7 2.7"/>',
  view: '<path d="M3.5 5.5h4.5v4h-4.5zM6 3.8h4.5v4"/>',
  menu: '<path d="M4 4.8h6M4 7h6M4 9.2h6"/>',
  cross: '<path d="M4.6 4.6l4.8 4.8M9.4 4.6l-4.8 4.8"/>',
  circle: '<circle cx="7" cy="7" r="3.1"/>',
  square: '<path d="M4.2 4.2h5.6v5.6H4.2z"/>',
  triangle: '<path d="M7 3.7l3.2 5.7H3.8z"/>',
};
/** the names a screen reader says for the shapes (English, translated where they are drawn) */
const SPOKEN: Record<string, string> = {
  up: N('D-pad up'), left: N('D-pad left'), right: N('D-pad right'), cross: N('Cross'), circle: N('Circle'), square: N('Square'), triangle: N('Triangle'),
  // Xbox's own names, as printed on the controller
  view: 'View', menu: 'Menu',
};
/** text for the drawing's markup */
const esc = (s: string) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

/** a row: its middle (px), its button as Xbox and PlayStation name it (a word or a SHAPE), what it does (and a
 *  second line; English, translated where drawn), the point of the drawing its line goes to (none: its group's),
 *  and spoken names where a shape stands for a word */
interface Row { y: number; xb: string; ps: string; does: string; sub?: string; to?: readonly [number, number]; say?: [string, string] }
const LEFT: Row[] = [
  { y: 36, xb: 'LT', ps: 'L2', does: N('Brake'), sub: N('and reverse'), to: [30, -5] },
  { y: 64, xb: 'LB', ps: 'L1', does: N('Nitrous'), to: [20.6, 31.2] },
  { y: 84, xb: 'LS', ps: 'L3', does: N('Steer'), sub: N('press in: horn'), to: [47, 54] },
  { y: 116, xb: 'up', ps: 'up', does: N('Start event') },
  { y: 130, xb: 'left', ps: 'left', does: N('Map') },
  { y: 144, xb: 'right', ps: 'right', does: N('Messages') },
];
const RIGHT: Row[] = [
  { y: 36, xb: 'RT', ps: 'R2', does: N('Throttle'), to: [230, -5] },
  { y: 54, xb: 'RB', ps: 'R1', does: N('Handbrake'), to: [239.4, 31.2] },
  { y: 76, xb: 'A', ps: 'cross', does: N('Shift up') },
  { y: 90, xb: 'X', ps: 'square', does: N('Shift down') },
  { y: 104, xb: 'Y', ps: 'triangle', does: N('Camera') },
  { y: 118, xb: 'B', ps: 'circle', does: N('Look back') },
  { y: 140, xb: 'RS', ps: 'R3', does: N('Quick chat'), to: [162, 94] },
];
/** over the middle: View and Menu, their lines straight down */
const MIDDLE: (Row & { x: number })[] = [
  { x: 150, y: 12, xb: 'view', ps: 'view', does: N('Respawn'), to: [112, 48.5], say: ['View', 'Create'] },
  { x: 172, y: 12, xb: 'menu', ps: 'menu', does: N('Pause'), to: [148, 48.5], say: ['Menu', 'Options'] },
];
/** the brackets: x, from y to y, the line's start on it, and the control it goes to */
const BRACKETS = [{ x: 90.5, y0: 110, y1: 150, from: 123, to: [85, 94] }, { x: 244.5, y0: 70, y1: 124, from: 80, to: [217, 54] }] as const;

const word = (v: string) => v in SHAPE ? '' : v;
const chipW = (r: Row) => word(r.xb).length > 1 || word(r.ps).length > 1 ? 20 : 14;
function name(v: string, w: number): string {
  return v in SHAPE ? `<g transform="translate(${(w - 14) / 2} 0)">${SHAPE[v]}</g>` : `<text x="${w / 2}" y="10.2">${v}</text>`;
}
function chip(x: number, r: Row): string {
  const w = chipW(r);
  const inner = r.xb === r.ps ? name(r.xb, w) : `<g class="xb">${name(r.xb, w)}</g><g class="ps">${name(r.ps, w)}</g>`;
  return `<g class="pad-chip" transform="translate(${x} ${r.y - 7})"><rect x="0.5" y="0.5" width="${w - 1}" height="13" rx="3"/>${inner}</g>`;
}
function line(x: number, y: number, to: readonly [number, number]): string {
  const [x2, y2] = at(to);
  return `<path class="pad-line" d="M${x} ${y}L${x2} ${y2}"/><circle class="pad-dot" cx="${x2}" cy="${y2}" r="1.7"/>`;
}
const used = (el: string) => el.replace('/>', ' class="on"/>');

function drawing(): string {
  const n = 'vector-effect="non-scaling-stroke"';
  const ring = (cx: number, cy: number, on: boolean) =>
    `<circle cx="${cx}" cy="${cy}" r="17" ${n}${on ? ' class="on"' : ''}/><circle cx="${cx}" cy="${cy}" r="10" ${n}${on ? ' class="on"' : ''}/>`;
  const button = (cx: number, cy: number, r: number) => used(`<circle cx="${cx}" cy="${cy}" r="${r}" ${n}/>`);
  return `<g class="pad-ctl" transform="translate(${MID - 130 * SCALE} ${TOP}) scale(${SCALE})">` +
    used(`<rect x="30" y="-14" width="40" height="18" rx="7" ${n}/>`) + used(`<rect x="190" y="-14" width="40" height="18" rx="7" ${n}/>`) +
    BUMPERS.map(d => `<path class="pad-bumper on" d="${d}"/><path class="pad-bumper-in" d="${d}"/>`).join('') +
    `<path class="pad-body" d="${BODY}" ${n}/>` +
    ring(64, 54, true) + ring(162, 94, true) + used(`<path d="${DPAD}" ${n}/>`) +
    button(112, 54, 5.5) + button(148, 54, 5.5) + `<circle cx="130" cy="31" r="8" ${n}/>` +
    button(196, 40, 7) + button(210, 54, 7) + button(196, 68, 7) + button(182, 54, 7) +
    '</g>';
}

/** what the drawing says, for a screen reader */
function spoken(f: Family): string {
  const say = (r: Row) => r.say?.[f === 'xb' ? 0 : 1] ?? (SPOKEN[r[f]] ? t(SPOKEN[r[f]]) : r[f]);
  const low = (s: string) => language() === 'english' ? s.toLowerCase() : s;
  return t('Controller: {list}', { list: [...LEFT, ...MIDDLE, ...RIGHT].map(r => `${say(r)}: ${low(t(r.does))}${r.sub ? `, ${t(r.sub)}` : ''}`).join('; ') });
}

function padSvg(): string {
  let s = drawing();
  const does = (r: Row) => esc(t(r.does));
  for (const r of LEFT) {
    const x = 86 - chipW(r);
    s += chip(x, r) + `<text class="pad-label" x="${x - 5}" y="${r.y + 4}" text-anchor="end">${does(r)}</text>`;
    if (r.sub) s += `<text class="pad-sub" x="${x - 5}" y="${r.y + 18}" text-anchor="end">${esc(t(r.sub))}</text>`;
    if (r.to) s += line(89, r.y, r.to);
  }
  for (const r of RIGHT) {
    s += chip(248, r) + `<text class="pad-label" x="${248 + chipW(r) + 5}" y="${r.y + 4}">${does(r)}</text>`;
    if (r.to) s += line(245, r.y, r.to);
  }
  for (const [i, r] of MIDDLE.entries()) {
    s += chip(r.x, r) + (i === 0 ? `<text class="pad-label" x="${r.x - 5}" y="${r.y + 4}" text-anchor="end">${does(r)}</text>`
      : `<text class="pad-label" x="${r.x + 19}" y="${r.y + 4}">${does(r)}</text>`);
    s += line(r.x + 7, r.y + 7.5, r.to!);
  }
  for (const b of BRACKETS) s += `<path class="pad-line" d="M${b.x} ${b.y0}V${b.y1}"/>` + line(b.x, b.from, b.to);
  return `<svg class="pad" viewBox="0 0 336 154" role="img" aria-label="${esc(spoken('xb'))}">${s}</svg>`;
}

/** a key cap with the button's Xbox and PlayStation names */
function cap(xb: string, ps: string): string {
  const one = (v: string) => v in SHAPE ? `<svg class="glyph" viewBox="0 0 14 14" role="img" aria-label="${esc(t(SPOKEN[v]))}">${SHAPE[v]}</svg>` : v;
  return `<kbd><span class="xb">${one(xb)}</span><span class="ps">${one(ps)}</span></kbd>`;
}

/** relabel: drawn again in the page's language (i18n.ts onLanguage) */
export interface ControlsSheet { show(tab: Tab, focus?: boolean): void; family(f: Family): void; relabel(): void; readonly tab: Tab; readonly fam: Family }

/**
 * Wires the sheet in root (its tabs and their panels in the page's markup; the controller's tab is drawn here):
 * id prefixes the ids the tabs and panels get, so the page's sheet and its copy in the dialog differ.  follow: a
 * controller connecting shows its tab, unless the player picked one, and its family's names.
 */
export function controlsSheet(root: HTMLElement, id: string, opts: { tab?: Tab; fam?: Family; follow?: boolean } = {}): ControlsSheet {
  const tabs = [...root.querySelectorAll<HTMLButtonElement>('[role="tab"]')];
  const panels = [...root.querySelectorAll<HTMLElement>('[role="tabpanel"]')];
  for (const p of panels) {
    p.id = `${id}-${p.dataset.tab}`;
    p.setAttribute('aria-labelledby', `${id}-tab-${p.dataset.tab}`);
  }
  for (const t of tabs) { t.id = `${id}-tab-${t.dataset.tab}`; t.setAttribute('aria-controls', `${id}-${t.dataset.tab}`); }
  const pad = panels.find(p => p.dataset.tab === 'pad')!;
  const wheel = wheelSection(pad, cap);
  let svg: SVGElement, radios: HTMLInputElement[] = [];
  /** the drawing and its foot, in the page's language; the wheel's part after them */
  const draw = () => {
    const menus = esc(t('In menus: D-pad and {a}, {b} goes back.')).replace('{a}', cap('A', 'cross')).replace('{b}', cap('B', 'circle'));
    pad.innerHTML = padSvg() + `<div class="pad-foot">
    <p class="hint">${menus}</p>
    <div class="seg" role="radiogroup" aria-label="${esc(t('Button names'))}">
      <label class="opt"><input type="radio" name="${id}-fam" value="xb">Xbox</label>
      <label class="opt"><input type="radio" name="${id}-fam" value="ps">PlayStation</label>
    </div></div>`;
    pad.append(wheel.box);
    svg = pad.querySelector('svg')!;
    radios = [...pad.querySelectorAll<HTMLInputElement>('input[type="radio"]')];
    for (const r of radios) r.onchange = () => { if (r.checked) sheet.family(r.value as Family); };
    wheel.relabel();
    requestAnimationFrame(fit);
  };
  /** a label too long for the drawing (another language's words) a little smaller, so it stays inside the 336 px;
   *  measured once the tab shows (a hidden one has no size).  Measured again after each change: a renderer that draws
   *  at a rounded size keeps the width for a while as the size goes down (Chromium on Linux draws a Japanese label
   *  63 units wide at 10.5 px and at 10 px), so one step in proportion can leave it past the drawing's edge; while it
   *  is (the first step aims 1 unit inside), the next steps take at least 3 % off.  Where the width follows the size,
   *  the first step fits and nothing changes. */
  const fit = () => {
    if (pad.hidden) return;
    for (const el of svg.querySelectorAll<SVGTextElement>('.pad-label, .pad-sub')) {
      el.style.fontSize = '';
      for (let step = 0; step < 8; step++) {
        const b = el.getBBox(), over = Math.max(1 - b.x, b.x + b.width - 335);
        if (!b.width || over <= (step ? 1 : 0)) break;
        el.style.fontSize = `${parseFloat(getComputedStyle(el).fontSize) * Math.min((b.width - over) / b.width, step ? 0.97 : 1)}px`;
      }
    }
  };

  let tab: Tab = opts.tab ?? 'keys', fam: Family = opts.fam ?? 'xb', picked = false;
  const sheet: ControlsSheet = {
    get tab() { return tab; },
    get fam() { return fam; },
    show(t, focus) {
      tab = t;
      for (const b of tabs) { const on = b.dataset.tab === t; b.setAttribute('aria-selected', String(on)); b.tabIndex = on ? 0 : -1; if (on && focus) b.focus(); }
      for (const p of panels) p.hidden = p.dataset.tab !== t;
      if (t === 'pad') requestAnimationFrame(fit);
    },
    family(f) {
      fam = f;
      root.dataset.fam = f;
      svg.setAttribute('aria-label', spoken(f));
      for (const r of radios) r.checked = r.value === f;
    },
    relabel() { draw(); sheet.family(fam); },
  };
  draw();
  for (const b of tabs) b.onclick = () => { picked = true; sheet.show(b.dataset.tab as Tab); };
  // the arrow keys between the tabs, Home and End to the first and last (a tab list's keys)
  root.querySelector<HTMLElement>('[role="tablist"]')!.onkeydown = e => {
    const i = tabs.findIndex(b => b.dataset.tab === tab);
    const to = e.key === 'ArrowRight' ? (i + 1) % tabs.length : e.key === 'ArrowLeft' ? (i + tabs.length - 1) % tabs.length
      : e.key === 'Home' ? 0 : e.key === 'End' ? tabs.length - 1 : -1;
    if (to < 0) return;
    e.preventDefault();
    picked = true;
    sheet.show(tabs[to].dataset.tab as Tab, true);
  };
  const steer = root.querySelector<HTMLElement>('[data-steer]');
  if (steer) {
    const sel = steer.querySelector('select')!;
    sel.id = `${id}-steering`; steer.querySelector('label')!.htmlFor = sel.id;
    const help = steer.querySelector('.set-help')!; help.id = `${id}-steering-help`; sel.setAttribute('aria-describedby', help.id);
    const fill = () => {
      sel.replaceChildren(...STEERING.map(s => { const o = document.createElement('option'); o.value = s; o.textContent = steeringName(s); return o; }));
      sel.value = steering();
    };
    sel.onchange = () => {
      const to = sel.value as Steering;
      if (to === 'tilt') void askMotion();          // iOS asks from a choice of the player's
      setSteering(to);
    };
    onSteering(() => { sel.value = steering(); });
    onLanguage(fill);
    fill();
  }
  // the switches: Nitro on the steering's side, the level horizon
  const toggle = (set: HTMLElement | null, name: string, setting: SwitchSetting) => {
    if (!set) return;
    const sw = set.querySelector<HTMLInputElement>('.switch')!, val = set.querySelector('.val')!;
    sw.id = `${id}-${name}`; set.querySelector('label')!.htmlFor = sw.id;
    const help = set.querySelector('.set-help')!; help.id = `${id}-${name}-help`; sw.setAttribute('aria-describedby', help.id);
    const show = () => { sw.checked = setting.get(); val.textContent = sw.checked ? t('On') : t('Off'); };
    sw.onchange = () => setting.set(sw.checked);
    setting.on(show);
    onLanguage(show);
    show();
  };
  toggle(root.querySelector<HTMLElement>('[data-nitro]'), 'nitro', nitroSteerSide);
  toggle(root.querySelector<HTMLElement>('[data-level]'), 'level', levelHorizon);
  // the touch controls' position's list and the sizes': the touch controls' and the game's menu buttons'
  const list = <T extends string | number>(set: HTMLElement | null, name: string, setting: ListSetting<T>) => {
    if (!set) return;
    const sel = set.querySelector('select')!;
    sel.id = `${id}-${name}`; set.querySelector('label')!.htmlFor = sel.id;
    const help = set.querySelector('.set-help')!; help.id = `${id}-${name}-help`; sel.setAttribute('aria-describedby', help.id);
    const fill = () => {
      sel.replaceChildren(...setting.values.map(v => { const o = document.createElement('option'); o.value = String(v); o.textContent = setting.name(v); return o; }));
      sel.value = String(setting.get());
    };
    sel.onchange = () => { const to = setting.values.find(v => String(v) === sel.value); if (to !== undefined) setting.set(to); };
    setting.on(fill);
    onLanguage(fill);
    fill();
  };
  list(root.querySelector<HTMLElement>('[data-position]'), 'position', touchPosition);
  list(root.querySelector<HTMLElement>('[data-size]'), 'size', touchSize);
  list(root.querySelector<HTMLElement>('[data-menu-size]'), 'menu-size', menuSize);
  if (opts.follow) {
    const seen = (g: Gamepad) => {
      sheet.family(familyOf(g.id).family === PADF_PLAYSTATION ? 'ps' : 'xb');
      if (!picked) sheet.show('pad');
    };
    window.addEventListener('gamepadconnected', e => seen(e.gamepad));
    window.addEventListener(PAD_DRIVER_EVENT, () => { const g = drivingPad(); if (g) seen(g); });
    try { const g = navigator.getGamepads?.().find(p => p?.connected); if (g) seen(g); } catch { /* no Gamepad API */ }
  }
  sheet.show(tab);
  sheet.family(fam);
  return sheet;
}
