// The settings on the start page: a form over settings.ini, in Settings and,
// for the Online group, in a section of its own under Saves.  Every change
// rewrites the one line of that key (settingsPut, bundle.m's settings_put), so
// the file stays the source of truth and keeps the player's own comments;
// "Reset" puts the template's commented-out line back, also on the lines the
// panel has no row for, and "Reset all settings" the whole template but the
// name, crews and language.  Native controls (switches are checkboxes, choices
// are radio groups, the crews a list with buttons), each described by its help
// line, so a screen reader and the keyboard get them as they are.

import { checkSetting, DEFAULTS, keyDef, resolveEnv, settingsPut, TEMPLATE, type Resolved } from './settings';
import { readText, writeFile } from './savefs';
import { playHorn, stopHorn } from './horn-preview';
import { checkCrew, CREW_BIG, CREW_COLOUR_NAMES, CREW_MAX, CREW_PALETTE, crewsOf, crewsValue, freeColour, rgbsValue, type Crew } from './crews';
import { roomHash } from './players';
import { CATS, DEFAULT_GROUPS, DEFAULT_PHRASES, groupsOf, PHRASES, phrasesOf, SLOTS } from './phrases';
import { LANGUAGES, live, N, NATIVE, t, tn, tvars, type Language, type Msg } from './i18n';

type Control =
  /** inv: on when the key is off (Ghost mode is roam_collide = off) */
  | { t: 'switch'; inv?: boolean }
  | { t: 'seg'; opts: [value: string, label: string][] }
  /** a choice of more than a row of buttons holds: a drop-down list; preview: a Listen button beside it plays the
   *  value chosen (the promise ends when it has played), stop ends that early */
  | { t: 'select'; opts: [value: string, label: string][]; preview?: { play(value: string): Promise<void>; stop(): void } }
  | { t: 'num'; min?: number; max?: number; step?: number; unit?: string }
  | { t: 'text'; placeholder?: string }
  /** the crews (crews.ts): a line per crew with Leave, and a box with Join for one more */
  | { t: 'crews'; placeholder: string }
  /** quick chat's sixteen (phrases.ts): under "Choose phrases", four columns of a name and four phrases, each a
   *  drop-down list */
  | { t: 'phrases' };

/** link: the words of help that link to a page (a mod's, say), opened in a new tab */
interface Item { key: string; label: string; help: string; link?: { text: string; href: string }; c: Control; more?: boolean }
/** status: the group shows a line of the page's own under its title (Online: who plays on this server now);
 *  own: the group is a section of the page of its own (#online, under Saves), named by that section's heading;
 *  more: the title of its "more" part.  The texts are English (N), translated where they are drawn. */
interface Group { title: string; more: string; items: Item[]; status?: boolean; own?: boolean }

/** each language's name in English (translated where it is shown), beside its own name */
const LANGUAGE_NAMES: Record<Language, string> = {
  english: N('English'), french: N('French'), german: N('German'), italian: N('Italian'), spanish: N('Spanish'), dutch: N('Dutch'),
  swedish: N('Swedish'), danish: N('Danish'), korean: N('Korean'), chinese: N('Chinese'), japanese: N('Japanese'), russian: N('Russian'),
  thai: N('Thai'), portuguese: N('Portuguese (Brazil)'), polish: N('Polish'), turkish: N('Turkish'),
};
const LANGUAGE_OPTS = LANGUAGES.map(l => [l, LANGUAGE_NAMES[l]] as [string, string]);

const GROUPS: Group[] = [
  { title: N('Online'), more: N('More online settings'), status: true, own: true, items: [
    { key: 'roam', label: N('Free roam with others'), help: N('Everyone on this server who turns this on sees your car in career free roam, and you see theirs, with their names over them.'), c: { t: 'switch' } },
    { key: 'roam_name', label: N('Your name'), help: N('What other players see over your car in free roam, and your name in the Online menu (there only letters, digits, - and _, at most 15). Empty uses your profile name.'), c: { t: 'text', placeholder: N('profile name') } },
    { key: 'roam_crew', label: N('Crews'), help: N('Friends who type the same crew name always see each other in free roam, however far apart, with names and map arrows in the crew\'s colour. Up to 4 crews. Anyone who types the name joins.'), c: { t: 'crews', placeholder: N('Crew name') } },
    { key: 'horn', label: N('Your horn'), help: N('N or the controller\'s left stick click, in free roam and in races with other players.'), c: { t: 'select', opts: [
      ['dual', N('Dual tone')], ['single', N('Single tone')], ['truck', N('Air horn')], ['train', N('Train horn')], ['klaxon', N('Klaxon')],
      ['cucaracha', N('La Cucaracha')], ['clown', N('Clown horn')], ['trombone', N('Sad trombone')]], preview: { play: playHorn, stop: stopHorn } } },
    { key: 'roam_room', label: N('Room'), help: N('Leave empty to drive with everyone. With a name, free roam shows only the players who set the same name. Not private: everyone on this server still sees your name.'), c: { t: 'text', placeholder: N('everyone') } },
    { key: 'roam_collide', label: N('Ghost mode'), help: N('You drive through the other players\' cars, and they through yours. Off: you collide with them, and a car that appears on yours lets you through until you are apart.'), c: { t: 'switch', inv: true } },
    { key: 'quick_chat', label: N('Quick chat'), help: N('Say one of your sixteen phrases to the players near you in free roam: 1 to 4, then 1 to 4 (on a controller, the right stick twice; on a touch screen, the chat button). Theirs show over their cars.'), c: { t: 'switch' } },
    { key: 'quick_chat_phrases', label: N('Your phrases'), help: N('Which sixteen quick chat has, in four groups of four. Everyone reads what you say in their own language.'), c: { t: 'phrases' } },
    { key: 'online', label: N('Online mode'), help: N('The game\'s Online menu takes you straight to the Online Main Menu under your name, without the EA Login screens: lobby, chat and races with everyone playing from this server.'), c: { t: 'switch' }, more: true },
    { key: 'roam_max_cars', label: N('Players\' cars shown'), help: N('The nearest. Phones start at 16 and tablets at 24: many cars loading at once can take more memory than they have.'), c: { t: 'seg', opts: [['16', '16'], ['20', '20'], ['24', '24'], ['28', '28'], ['32', '32']] }, more: true },
  ] },
  { title: N('Game'), more: N('More game settings'), items: [
    { key: 'language', label: N('Language'), help: N('The game\'s menus and subtitles, the voices in languages with a dub, and this page.'), c: { t: 'select', opts: LANGUAGE_OPTS } },
    { key: 'voices', label: N('Voices and movies'), help: N('Dubbed: voices and cutscene movies in languages with a dub (including Brazilian Portuguese PT-BR by ZANIT). Original: original English voices and movies with the chosen language.'), c: { t: 'seg', opts: [['dubbed', N('Dubbed')], ['original', N('Original')]] } },
    { key: 'movies', label: N('Movies'), help: N('Intro, story and FMV movies.'), c: { t: 'switch' } },
    { key: 'subtitles', label: N('Subtitles'), help: N('The movies\' lines as text. English\'s are VileStuff\'s English Subtitles Fixed, with who says each line.'), link: { text: N('English Subtitles Fixed'), href: 'https://nfsmods.xyz/mod/5286' }, c: { t: 'switch' } },
    { key: 'autosave', label: N('Autosave'), help: N('Sets each profile\'s Save option in the game to Autosave once, so races, purchases and changes are saved without asking. Prompt or Off chosen afterwards in the game\'s Options, Gameplay stays.'), c: { t: 'switch' } },
    { key: 'intro', label: N('Startup movies'), help: N('The EA and THX logos, the PSA and the intro. Skippable: Enter, Esc, Space, a left click, or A, B or Start ends each one. Original: as in the game, where only the intro ends early.'), c: { t: 'seg', opts: [['skippable', N('Skippable')], ['original', N('Original')], ['off', N('Off')]] }, more: true },
    { key: 'sound', label: N('Sound'), help: N('All sound output.'), c: { t: 'switch' }, more: true },
    { key: 'background', label: N('When the tab is in the background'), help: N('Pause freezes the game and its sound, as alt-tab does on Windows. In LAN and Online play it keeps running, without sound.'), c: { t: 'seg', opts: [['pause', N('Pause')], ['run', N('Keep running')]] }, more: true },
    { key: 'audio_latency', label: N('Output latency'), help: N('Lowest reacts fastest; Robust survives a busy PC best.'), c: { t: 'seg', opts: [['interactive', N('Lowest')], ['balanced', N('Balanced')], ['playback', N('Robust')]] }, more: true },
    { key: 'seed_registry', label: N('Highest graphics for a new save'), help: N('The game\'s own graphics options at their highest, at this screen\'s resolution. Off lets the game run its own hardware check.'), c: { t: 'switch' }, more: true },
  ] },
  { title: N('Display'), more: N('More display settings'), items: [
    { key: 'fullscreen', label: N('Full screen on Play'), help: N('Off plays in this browser window; the menu (F9) has full screen while you play. Hold Esc to leave it. On by default on phones.'), c: { t: 'switch' } },
    { key: 'render', label: N('Resolution scale'), help: N('Scales the game\'s screen-size resolution (its default), per side: 50% renders a quarter of the pixels. Lower is faster and softer. Phones start at 75%.'), c: { t: 'seg', opts: [['scale:0.5', '50%'], ['scale:0.75', '75%'], ['backing', '100%'], ['scale:1.5', '150%'], ['scale:2', '200%']] } },
    { key: 'fps', label: N('Frame rate'), help: N('Auto follows the display (120 on a 120 Hz screen, 240 on a 240 Hz one). 30 is steadier where 60 stutters, as on a slow phone, and easier on the battery. Online and LAN races run at up to 120, as on a PC; cutscenes stay at 30.'), c: { t: 'seg', opts: [['auto', N('Auto')], ['30', '30'], ['60', '60'], ['120', '120'], ['240', '240']] } },
    { key: 'ui_textures', label: N('Menus and HUD'), help: N('Original: the game\'s own. Upscaled: LunicAura106\'s Upscaled HUD mod, the menus, race HUD, fonts and logos redrawn at four times their size, sharp at high resolutions. About 5 MB to download, and on most phones four times the memory.'), link: { text: N('Upscaled HUD'), href: 'https://nfsmods.xyz/mod/4319' }, c: { t: 'seg', opts: [['original', N('Original')], ['uhud', N('Upscaled')]] }, more: true },
    { key: 'hud', label: N('Race HUD'), help: N('Wide anchors it to the screen edges; 4:3 is the original centred layout.'), c: { t: 'seg', opts: [['wide', N('Wide')], ['43', '4:3']] }, more: true },
    { key: 'movie_framing', label: N('Movie framing'), help: N('Fit keeps the 4:3 movies whole; fill crops them; stretch widens them.'), c: { t: 'seg', opts: [['fit', N('Fit')], ['fill', N('Fill')], ['stretch', N('Stretch')]] }, more: true },
    { key: 'cutscene_borders', label: N('Cutscene bars'), help: N('Letterbox bars across the whole width, or none.'), c: { t: 'seg', opts: [['full', N('Full')], ['off', N('Off')]] }, more: true },
    { key: 'other_headlights', label: N('Other cars\' headlights'), help: N('Light on the road from other cars, as your own headlights give. Players: friends in free roam and the other players in Online and LAN races. All: AI racers and traffic too. Off: only yours, as in the original game.'), c: { t: 'seg', opts: [['players', N('Players')], ['all', N('All cars')], ['off', N('Off')]] }, more: true },
    { key: 'fps_counter', label: N('Frame-rate counter'), help: N('In a corner of the game. F8 cycles it while you play.'), c: { t: 'seg', opts: [['off', N('Off')], ['on', N('On')], ['detailed', N('Detailed')]] }, more: true },
    { key: 'resolution', label: N('Fixed render size'), help: N('Renders every resolution at this size, for example 2560x1440. Empty uses the one chosen in the game.'), c: { t: 'text', placeholder: N('WxH') }, more: true },
    { key: 'max_pixels', label: N('Pixel limit'), help: N('Most pixels the screen-size resolution renders at 100% and below (0 = no limit). 150% and 200% scale up from it.'), c: { t: 'num', min: 0, step: 100000 }, more: true },
  ] },
  { title: N('Controller'), more: N('More controller settings'), items: [
    { key: 'rumble', label: N('Rumble'), help: N('On controllers that have it, and with the touch controls the phone\'s vibration: crashes, hard landings, the game\'s own force feedback and the tick of each button press.'), c: { t: 'switch' } },
    { key: 'gamepad', label: N('Controller'), help: N('On keeps a controller once it was connected; hotplug only while connected.'), c: { t: 'seg', opts: [['on', N('On')], ['hotplug', N('Hotplug')], ['off', N('Off')]] }, more: true },
    { key: 'gamepad_menu_keys', label: N('Menu buttons'), help: N('A, B and View also act as Enter, Esc and Q in the menus.'), c: { t: 'switch' }, more: true },
    { key: 'gamepad_deadzone', label: N('Stick dead zone'), help: N('Fraction of full travel, 0 to 0.9.'), c: { t: 'num', min: 0, max: 0.9, step: 0.01 }, more: true },
    { key: 'gamepad_trigger_deadzone', label: N('Trigger dead zone'), help: N('Fraction of full travel, 0 to 0.9.'), c: { t: 'num', min: 0, max: 0.9, step: 0.01 }, more: true },
    { key: 'gamepad_trigger_click', label: N('Trigger full press'), help: N('How far a trigger travels before it counts as fully pressed (full throttle, full brake).'), c: { t: 'num', min: 0, max: 1, step: 0.01 }, more: true },
  ] },
];

/** a row's second key, which its control writes with its own and its Reset resets too (not listed under "Other
 *  settings in this file"): the Crews row's colours */
const COMPANION: Record<string, string> = { roam_crew: 'roam_crew_colours', quick_chat_phrases: 'quick_chat_groups' };
const COMPANIONS = new Set(Object.values(COMPANION));
/** Every key the panel shows (the check in app-test.mjs compares this with the template). */
export const PANEL_KEYS = [...GROUPS.flatMap(g => g.items.map(i => i.key)), ...COMPANIONS];
const ITEMS = new Map(GROUPS.flatMap(g => g.items.map(i => [i.key, i] as const)));
const GROUP_OF = new Map(GROUPS.flatMap(g => g.items.map(i => [i.key, g] as const)));

const boolOn = (v: string) => v === 'on';
const READ_ONLY = N('Read only here: another tab of this browser runs the game and owns its saves and settings. Change them there; this tab takes over when that one closes.');
const NOTE = N('Saved in this browser. Changes apply the next time you press Play.');

/** a small button with a drawn sign (the Crews row's Join and Leave), its name for a screen reader and as its tooltip */
function iconButton(cls: string, name: string, path: string): HTMLButtonElement {
  const b = document.createElement('button'); b.type = 'button'; b.className = `icon-btn ${cls}`;
  b.setAttribute('aria-label', name); b.title = name;
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg'); svg.setAttribute('viewBox', '0 0 24 24'); svg.setAttribute('aria-hidden', 'true');
  const p = document.createElementNS('http://www.w3.org/2000/svg', 'path'); p.setAttribute('d', path);
  svg.append(p); b.append(svg);
  return b;
}
const PLUS = 'M12 5v14M5 12h14', CROSS = 'M6 6l12 12M18 6L6 18', PERSON = 'M12 12a4 4 0 1 0 0-8 4 4 0 0 0 0 8zM4 21c0-4 3.6-7 8-7s8 3 8 7';

/** What a crews control shows and does: the Crews row's, and the page menu's while a game is in free roam */
export interface CrewControl {
  /** the crews now, in the player's order, with their colours */
  list: readonly Crew[];
  /** the field's id (a label names it), and the label's (it names the list too) */
  id: string; labelId: string;
  placeholder: string;
  /** how many are in free roam in each crew, by hash (none: not shown) */
  counts?: Map<number, number>;
  /** what the field held when it was last drawn, and each change of it */
  draft: string; onDraft(v: string): void;
  /** the crews after a Leave, a Join or a colour picked; focus(root): where the keyboard goes once they are drawn
   *  again in root */
  change(list: Crew[], focus: (root: HTMLElement) => Element | null | undefined): void;
}

/**
 * A crews control into ctl: one line of pills that wraps, a chip per crew in the player's order (its colour in the
 * game, a button that opens the palette under the line; its name, how many are in free roam in it when the server
 * says, and Leave), then a value box with Join in it (or Enter).  Join waits for a name that can join (crews.ts checkCrew), and the note
 * under the box says why one cannot; with 4 crews the box waits for one to be left.  A Leave keeps the others'
 * colours; a Join takes the first colour no crew has.  A line under the chips names a crew counted with more than
 * CREW_BIG players (showCrewCounts).  Returns the box's field.
 */
export function crewControl(o: CrewControl, ctl: HTMLElement): HTMLInputElement {
  const list = o.list;
  const ul = document.createElement('ul'); ul.className = 'crews'; ul.setAttribute('aria-labelledby', o.labelId);
  list.forEach((c, i) => {
    const li = document.createElement('li'); li.className = 'crew'; li.dataset.hash = String(roomHash(c.name)); li.title = c.name;
    li.style.setProperty('--crew', `#${c.rgb}`);
    const what = t('Colour of {name}', { name: c.name }), change = t('Change the colour of {name}', { name: c.name });
    const dot = document.createElement('button'); dot.type = 'button'; dot.className = 'crew-dot';
    dot.setAttribute('aria-label', change); dot.title = change; dot.setAttribute('aria-expanded', 'false');
    dot.onclick = () => palette(dot, c, i, what);
    const text = document.createElement('span'); text.className = 'crew-text';
    const nm = document.createElement('span'); nm.className = 'crew-name'; nm.textContent = c.name;
    const n = document.createElement('span'); n.className = 'crew-count';
    text.append(nm, n);
    const leave = iconButton('crew-leave', t('Leave crew {name}', { name: c.name }), CROSS);
    // the keyboard goes on to the next crew's Leave (the last one's: the one before), or to the box
    leave.onclick = () => o.change(list.filter((_, k) => k !== i), root => root.querySelectorAll('.crew-leave')[Math.min(i, list.length - 2)]);
    li.append(dot, text, leave);
    ul.append(li);
  });
  const line = document.createElement('div'); line.className = 'crew-line';
  /** crew i's palette, under the line (the chips stay where they are), one open at a time: a radio group of the 8
   *  colours, its own marked; the arrows move along it, Enter, Space or a tap picks one and closes it, Esc (or the dot
   *  again) closes it.  Esc stays here: in the menu's Crews popup it does not close the popup too. */
  const palette = (dot: HTMLButtonElement, c: Crew, i: number, what: string) => {
    const was = ctl.querySelector<HTMLElement>('.crew-colours');
    was?.remove();
    for (const d of ul.querySelectorAll('.crew-dot')) d.setAttribute('aria-expanded', 'false');
    if (was?.dataset.crew === String(i)) { dot.focus(); return; }
    const g = document.createElement('div'); g.className = 'crew-colours'; g.id = `${o.id}-colours`; g.dataset.crew = String(i);
    g.setAttribute('role', 'radiogroup'); g.setAttribute('aria-label', what);
    dot.setAttribute('aria-controls', g.id); dot.setAttribute('aria-expanded', 'true');
    const close = () => { g.remove(); dot.setAttribute('aria-expanded', 'false'); dot.focus(); };
    const sw = CREW_PALETTE.map((rgb, k) => {
      const b = document.createElement('button'); b.type = 'button'; b.className = 'swatch'; b.setAttribute('role', 'radio');
      b.setAttribute('aria-checked', String(rgb === c.rgb)); b.tabIndex = rgb === c.rgb ? 0 : -1;
      const name = t(CREW_COLOUR_NAMES[k]); b.setAttribute('aria-label', name); b.title = name;
      b.style.setProperty('--sw', `#${rgb}`);
      b.onclick = () => {
        if (rgb === c.rgb) { close(); return; }
        o.change(list.map((x, j) => j === i ? { ...x, rgb } : x), root => root.querySelectorAll('.crew-dot')[i]);
      };
      return b;
    });
    if (!sw.some(b => b.tabIndex === 0)) sw[0].tabIndex = 0;          // a colour from the file the palette lacks
    g.onkeydown = e => {
      const at = sw.indexOf(document.activeElement as HTMLButtonElement);
      const to = e.key === 'ArrowRight' || e.key === 'ArrowDown' ? at + 1 : e.key === 'ArrowLeft' || e.key === 'ArrowUp' ? at - 1
        : e.key === 'Home' ? 0 : e.key === 'End' ? sw.length - 1 : null;
      if (to !== null) {
        e.preventDefault();
        const next = sw[(to + sw.length) % sw.length];
        for (const b of sw) b.tabIndex = b === next ? 0 : -1;
        next.focus();
      } else if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); close(); }
    };
    g.append(...sw);
    line.after(g);
    sw.find(b => b.tabIndex === 0)!.focus();
  };
  const big = document.createElement('p'); big.className = 'set-note warn crews-big'; big.hidden = true;
  const box = document.createElement('div'); box.className = 'crew-add';
  const inp = document.createElement('input'); inp.type = 'text'; inp.id = o.id; inp.placeholder = t(o.placeholder);
  inp.autocomplete = 'off'; inp.spellcheck = false; inp.enterKeyHint = 'done';
  inp.value = o.draft;
  const join = iconButton('crew-join', t('Join crew'), PLUS);
  const note = document.createElement('p'); note.className = 'set-note'; note.id = `${o.id}-note`; note.setAttribute('aria-live', 'polite');
  const check = () => {
    const r = checkCrew(inp.value, list.map(c => c.name)), full = list.length >= CREW_MAX;
    inp.disabled = full; join.disabled = !r.ok;
    note.textContent = r.ok || !r.why ? '' : t(r.why, { name: r.name ?? '' });
    note.classList.toggle('warn', !full);
    return r;
  };
  const add = () => {
    const r = check();
    if (!r.ok) return;
    o.onDraft('');
    // the keyboard stays in the box for one more (with 4: on the last crew's Leave)
    o.change([...list, { name: r.name, rgb: freeColour(list) }], root => root.querySelector('.crew-add input:enabled') ?? [...root.querySelectorAll('.crew-leave')].pop());
  };
  inp.oninput = () => { o.onDraft(inp.value); check(); };
  inp.onkeydown = e => { if (e.key === 'Enter') { e.preventDefault(); add(); } };
  join.onclick = add;
  box.append(inp, join);
  line.append(ul, box);
  ctl.append(line, big, note);
  if (o.counts) showCrewCounts(ctl, o.counts);
  check();
  return inp;
}

/** the counts on a crews control's chips: a figure by a person's outline (a screen reader gets "{n} in free roam"), the
 *  chip's tooltip its name and that; nothing for a crew counts lacks.  Under the chips, the crews counted with more
 *  than CREW_BIG: the server's hub no longer links them first (in one room; the count is of every room). */
export function showCrewCounts(root: ParentNode, counts: Map<number, number>) {
  for (const ul of root.querySelectorAll<HTMLElement>('ul.crews')) {
    const line = ul.closest('.crew-line')?.parentElement?.querySelector<HTMLElement>('.crews-big');
    if (!line) continue;
    const big = [...ul.querySelectorAll<HTMLElement>('.crew[data-hash]')].filter(li => (counts.get(Number(li.dataset.hash)) ?? 0) > CREW_BIG)
      .map(li => t('{name} has more than 64 players in free roam: in a crew that big, crew-mates far away no longer always show, only the ones nearby, as with everyone else. A less common name works better.',
        { name: li.querySelector('.crew-name')?.textContent ?? '' }));
    line.textContent = big.join(' '); line.hidden = !big.length;
  }
  for (const li of root.querySelectorAll<HTMLElement>('.crew[data-hash]')) {
    const n = counts.get(Number(li.dataset.hash)), el = li.querySelector('.crew-count')!, name = li.querySelector('.crew-name')!.textContent ?? '';
    if (n === undefined) { el.replaceChildren(); li.title = name; continue; }
    const said = t('{n} in free roam', { n });
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg'); svg.setAttribute('viewBox', '0 0 24 24'); svg.setAttribute('aria-hidden', 'true');
    const p = document.createElementNS('http://www.w3.org/2000/svg', 'path'); p.setAttribute('d', PERSON); svg.append(p);
    const fig = document.createElement('span'); fig.setAttribute('aria-hidden', 'true'); fig.textContent = String(n);
    const sr = document.createElement('span'); sr.className = 'vh'; sr.textContent = said;
    el.replaceChildren(svg, fig, sr);
    li.title = `${name} · ${said}`;
  }
}

/** an option's label in the page's language (a language: its name there, and its own name beside it) */
function optLabel(it: Item, value: string, label: string): string {
  if (it.key !== 'language') return t(label);
  const own = NATIVE[value as Language] ?? value, here = t(label);
  return here === own ? own : `${here} · ${own}`;
}

/** A value as the panel shows it: the option's label, on/off, or the value with its unit. */
function shown(it: Item, v: string): string {
  const c = it.c;
  if (c.t === 'switch') return (v === 'on') !== !!c.inv ? t('on') : t('off');
  if (c.t === 'seg' || c.t === 'select') { const o = c.opts.find(([x]) => x.toLowerCase() === v.toLowerCase()); return o ? optLabel(it, o[0], o[1]) : v; }
  if (c.t === 'phrases') return phrasesOf(v).map(p => t(PHRASES[p].text)).join(', ');
  if (v === '') return t('empty');
  return c.t === 'num' && c.unit ? `${v} ${c.unit}` : v;
}

/**
 * settingsPut, and then the key's variable really is what the row asked for: a line that sets the same
 * variable by its own name (NFSU2_ROAM = 1 for roam) and comes first would still win at Play (the first line
 * that sets a variable does), so it goes too.  Lines are found by their line in the file, not by the pieces
 * bundle.m reads a long line in (a long line before them shifted the piece numbers: the audit review's PAGE-2
 * finding, which removed the wrong line).  removed: how many such lines went.
 */
export function putKey(text: string, key: string, value: string | null): { text: string; removed: number } {
  let next = settingsPut(text, key, value), removed = 0;
  const d = keyDef(key);
  for (let i = 0; d && i < 64; i++) {
    const by = resolveEnv(next, {}, {}).byEnv[d.env];
    if (!by || keyDef(by.key)?.key === d.key) break;
    const lines = next.split('\n'); lines.splice(by.phys - 1, 1); next = lines.join('\n'); removed++;
  }
  return { text: next, removed };
}

/** What "Reset all settings" keeps: the name the other players know you by, your crews, and the language (this
 *  page's too). */
const RESET_KEEPS = ['roam_name', 'roam_crew', 'roam_crew_colours', 'language'];

/** "Reset all settings": the template, with the kept settings as the file has them (by their own key, also where
 *  the file sets the variable by its name, if the key reads the value the same).  One the file does not set, or
 *  refuses, is the default. */
export function resetAll(text: string | null): string {
  const r = resolveEnv(text, {}, {});
  let next = TEMPLATE;
  for (const key of RESET_KEEPS) {
    const env = keyDef(key)!.env, v = r.env[env], c = checkSetting(key, v ?? '');
    if (r.source[env] === 'settings.ini' && c?.ok && c.value === v) next = settingsPut(next, key, v);
  }
  return next;
}

/** A line the panel has no row for back to the default, by its line in the file (1-based): an option of the
 *  template gets the template's commented-out line in its place, any other line (a variable by its own name, the
 *  Mac app's options) goes. */
export function resetLine(text: string, phys: number): string {
  const lines = text.split('\n');
  const l = (lines[phys - 1] ?? '').trim();
  const eq = l.indexOf('=');
  if (eq < 0 || l[0] === '#' || l[0] === ';') return text;          // not a setting's line: nothing to reset
  const d = keyDef(l.slice(0, eq).trim());
  if (d && DEFAULTS.has(d.key)) lines[phys - 1] = `#${d.key} = ${DEFAULTS.get(d.key)}`.replace(/ = $/, ' =');
  else lines.splice(phys - 1, 1);
  return lines.join('\n');
}

export interface SettingsDeps {
  saveDir: string;
  /** the page address's environment (overrides the file) */
  environment: Record<string, string>;
  /** keys whose default is another on this device (device.ts), shown when the file does not set them */
  defaults?: Record<string, string>;
  /** after every write (it applies at the next Play, which reads the file again) */
  onSaved?(text: string): void;
  /** true in a tab that does not own the saves: the panel shows the file but does not write it */
  readOnly?(): boolean;
  /** a short message for the page: what was saved, or why not (a Msg: drawn again if the page's language changes) */
  say?(msg: string | Msg): void;
  /** after settings.ini was read again (refresh) */
  onLoaded?(): void;
  /** the page's question box (Reset all settings asks first) */
  confirm?(title: string, body: string, ok: string, danger?: boolean): Promise<boolean>;
}

export class SettingsPanel {
  text: string | null = null;
  private resolved!: Resolved;
  /** the file alone (no page address): what each row shows */
  private fileOnly!: Resolved;
  private saves = 0;                       // writes so far (tests wait on data-saved)
  private openMore = new Set<string>();    // "More ... settings" kept open across re-renders
  private status: Msg = () => '';          // the Online group's line (setStatus)
  private crewCounts = new Map<number, number>();   // how many are in free roam in each crew, by hash (setCrewCounts)
  private crewDraft = '';                  // the crew name being typed, kept across re-renders
  private phrasesOpen = false;             // Your phrases' Choose phrases open, kept across re-renders
  /** root: the Settings section; online: the section of the group that is its own (Online, under Saves) */
  constructor(private root: HTMLElement, private deps: SettingsDeps, private online: HTMLElement) {
    root.querySelector<HTMLButtonElement>('#settings-text-save')!.onclick = () => void this.saveRaw();
    root.querySelector<HTMLButtonElement>('#settings-reset-all')!.onclick = () => void this.resetAll();
  }

  async load(): Promise<string | null> {
    this.text = await readText(this.deps.saveDir, 'settings.ini');
    this.resolve(this.text);
    return this.text;
  }
  private resolve(text: string | null) {
    this.resolved = resolveEnv(text, this.deps.environment, {});
    this.fileOnly = resolveEnv(text, {}, {});
  }

  /** Read settings.ini again and show it. */
  async refresh() {
    await this.load();
    this.render();
    this.deps.onLoaded?.();
  }

  /** One setting set as its row would set it (the language list at the top of the page). */
  set(key: string, value: string | null) { return this.put(key, value); }

  /** What the file makes of a key's variable (on/off for booleans), else the template default.  The variable may
   *  be set by the key's own line or by a line with the variable's name (NFSU2_ROAM = 1): the row shows what
   *  applies either way (the audit's PAGE-2: such a line turned free roam on behind an "off" switch).  A raw
   *  value is read as the runtime reads it (an integer for NFSU2_ROAM, "starts with 0" for NOMOVIES). */
  current(key: string): string {
    const d = keyDef(key)!, r = this.fileOnly;
    if (r.source[d.env] === 'settings.ini') {
      const v = r.env[d.env];
      if (d.kind === 'bool') return parseInt(v, 10) === 1 ? 'on' : 'off';
      if (d.kind === 'boolInv') return v === '' || v.startsWith('0') ? 'on' : 'off';
      return v;
    }
    return this.deps.defaults?.[key] ?? DEFAULTS.get(key) ?? '';
  }

  private ro() { return !!this.deps.readOnly?.(); }

  /** key's line, and also's (a row's second key: COMPANION) in the same write */
  private async put(key: string, value: string | null, also: [string, string | null][] = []) {
    if (this.ro()) { this.deps.say?.(t(READ_ONLY)); return; }
    let next = this.text ?? TEMPLATE, removed = 0;
    for (const [k, v] of [[key, value] as [string, string | null], ...also]) { const r = putKey(next, k, v); next = r.text; removed += r.removed; }
    await writeFile(this.deps.saveDir, 'settings.ini', next);
    this.text = next;
    this.resolve(next);
    this.deps.onSaved?.(next);
    if (removed) this.render(); else { this.rerow(key); this.syncRaw(); }
    this.root.dataset.saved = String(++this.saves);
    const it = ITEMS.get(key), cur = this.current(key);
    this.deps.say?.(() => {
      const v = { what: it ? t(it.label) : key, value: it ? shown(it, cur) : value ?? '' };
      return value === null ? t('Saved: {what}, {value} (the default). Applies the next time you press Play.', v) : t('Saved: {what}, {value}. Applies the next time you press Play.', v);
    });
  }

  render() {
    if (!this.fileOnly) return;                  // not read yet (the page's language came first): load() draws it
    const ro = this.ro();
    this.root.toggleAttribute('data-readonly', ro);
    this.root.querySelector('#settings-note')!.textContent = t(ro ? READ_ONLY : NOTE);
    this.online.toggleAttribute('data-readonly', ro);
    const note = this.online.querySelector<HTMLElement>('#online-note')!;
    note.textContent = ro ? t(READ_ONLY) : ''; note.hidden = !ro;
    const focusKey = (document.activeElement as HTMLElement | null)?.closest<HTMLElement>('.set')?.dataset.key;
    this.root.querySelector('#settings-body')!.replaceChildren(...GROUPS.filter(g => !g.own).map(g => this.group(g)), ...this.others());
    this.online.querySelector('#online-body')!.replaceChildren(...GROUPS.filter(g => g.own).map(g => this.group(g)));
    this.syncRaw();
    const row = focusKey ? this.find<HTMLElement>(`.set[data-key="${focusKey}"]`) : null;
    if (row) this.focusRow(row);
  }

  /** an element of the Settings section or of the Online one */
  private find<T extends Element>(sel: string): T | null { return this.root.querySelector<T>(sel) ?? this.online.querySelector<T>(sel); }

  /** The Online group's line under its title ('' hides it); kept across re-renders, and drawn again in the page's
   *  language when that changes. */
  setStatus(line: string | Msg) {
    this.status = typeof line === 'string' ? () => line : line;
    const p = this.find<HTMLElement>('#group-status');
    if (p) { live(p, this.status); p.hidden = !p.textContent; }
  }

  /** How many are in free roam in each crew now, by its hash (players.ts crewCounts; a crew it lacks: not known),
   *  on the Crews row's lines; kept across re-renders. */
  setCrewCounts(counts: Map<number, number>) {
    this.crewCounts = counts;
    for (const r of [this.root, this.online]) showCrewCounts(r, counts);
  }

  private group(g: Group): HTMLElement {
    const box = document.createElement('div'); box.className = 'group';
    // a group in its own section has that section's heading
    if (!g.own) {
      const h = document.createElement('h3'); h.id = `group-${g.title.toLowerCase().replace(/\W+/g, '-')}`; h.textContent = t(g.title);
      box.setAttribute('role', 'group'); box.setAttribute('aria-labelledby', h.id);
      box.append(h);
    }
    if (g.status) {
      const p = document.createElement('p'); p.className = 'hint group-status'; p.id = 'group-status';
      live(p, this.status); p.hidden = !p.textContent;
      box.append(p);
    }
    box.append(...g.items.filter(i => !i.more).map(i => this.row(i)));
    const more = g.items.filter(i => i.more);
    if (more.length) {
      const d = document.createElement('details'); d.className = 'more';
      d.open = this.openMore.has(g.title);
      d.ontoggle = () => { if (d.open) this.openMore.add(g.title); else this.openMore.delete(g.title); };
      const s = document.createElement('summary'); s.textContent = this.moreLabel(g);
      d.append(s, ...more.map(i => this.row(i)));
      box.append(d);
    }
    return box;
  }

  /** "More ... settings", with how many of them the file changes: a changed setting is not out of sight unnoticed. */
  private moreLabel(g: Group): string {
    const n = g.items.filter(i => i.more && this.resolved.file[i.key]?.raw).length;
    return n ? t('{label} ({n} changed)', { label: t(g.more), n }) : t(g.more);
  }

  /** The lines with keys outside the table, and those of table keys the panel has no row for (roam_rate and the
   *  other tuning knobs, the Mac app's): listed, so nothing in the file acts unseen (the audit's PAGE-2). */
  private others(): HTMLElement[] {
    const unshown = Object.entries(this.fileOnly.file).filter(([k, f]) => !ITEMS.has(k) && !COMPANIONS.has(k) && f.raw !== '')
      .map(([key, f]) => ({ phys: f.phys, key, env: key, value: f.raw, refused: f.refused, why: f.why }));
    const other = [...this.fileOnly.other, ...unshown].sort((a, b) => a.phys - b.phys);
    if (!other.length) return [];
    const box = document.createElement('div'); box.className = 'group'; box.id = 'settings-other';
    const h = document.createElement('h3'); h.id = 'group-other'; h.textContent = t('Other settings in this file');
    box.setAttribute('role', 'group'); box.setAttribute('aria-labelledby', h.id);
    const p = document.createElement('p'); p.className = 'set-help';
    p.textContent = t('Lines this page has no control for: settings it does not show, and runtime variables set by their own name. They apply at Play; edit the file as text below to change them.');
    const ul = document.createElement('ul');
    for (const o of other) {
      const li = document.createElement('li'); li.className = 'set-help';
      const line = t('line {line}: {key} = {value}', { line: o.phys, key: o.key, value: o.value.length > 80 ? o.value.slice(0, 80) + '…' : o.value });
      li.textContent = o.refused ? t('{line} (ignored: {why})', { line, why: o.why ? tvars(o.why.t, o.why.v) : o.refused })
        : o.env !== o.key ? t('{line} (sets {env})', { line, env: o.env }) : line;
      const r = document.createElement('button'); r.type = 'button'; r.className = 'text-btn'; r.textContent = t('Reset');
      r.setAttribute('aria-label', t('Reset {label}', { label: o.key }));
      const def = DEFAULTS.get(keyDef(o.key)?.key ?? '');
      if (def !== undefined) r.title = t('Back to the default ({value})', { value: def === '' ? t('empty') : def });
      r.disabled = this.ro();
      r.onclick = () => void this.resetLine(o.phys, o.key, r);
      li.append(' ', r);
      ul.append(li);
    }
    box.append(h, p, ul);
    return [box];
  }

  /** One row again, after its key changed (the rest of the panel, and the focus, stay where they are). */
  private rerow(key: string) {
    const old = this.find<HTMLElement>(`.set[data-key="${key}"]`);
    const it = ITEMS.get(key);
    if (!old || !it) { this.render(); return; }
    const focused = old.contains(document.activeElement), was = focused ? document.activeElement!.id : '';
    const fresh = this.row(it);
    old.replaceWith(fresh);
    if (it.more) fresh.parentElement!.querySelector('summary')!.textContent = this.moreLabel(GROUP_OF.get(key)!);
    // the same control again where it has an id (Your phrases' lists), else the row's first
    const same = was ? fresh.querySelector<HTMLElement>(`#${CSS.escape(was)}`) : null;
    if (same) same.focus(); else if (focused) this.focusRow(fresh);
  }

  private focusRow(row: HTMLElement) {
    row.querySelector<HTMLElement>('input[type="radio"]:checked, input:not([type="radio"])')?.focus();
  }

  private syncRaw() {
    const ro = this.ro();
    const ta = this.root.querySelector<HTMLTextAreaElement>('#settings-text')!;
    if (document.activeElement !== ta) ta.value = this.text ?? TEMPLATE;
    ta.readOnly = ro;
    this.root.querySelector<HTMLButtonElement>('#settings-text-save')!.disabled = ro;
    // nothing to reset: the file is the template already, but for the name and language
    this.root.querySelector<HTMLButtonElement>('#settings-reset-all')!.disabled = ro || resetAll(this.text) === (this.text ?? TEMPLATE);
  }

  private row(it: Item): HTMLElement {
    const def = keyDef(it.key)!;
    const row = document.createElement('div'); row.className = 'set'; row.dataset.key = it.key;
    const id = `set-${it.key}`;
    const cur = this.current(it.key);
    const f = this.resolved.file[it.key], comp = COMPANION[it.key], f2 = comp ? this.resolved.file[comp] : undefined;
    const setInFile = (!!f && f.raw !== '') || (!!f2 && f2.raw !== '');   // set (or refused): Reset puts the template line back
    const c = it.c;
    // a choice is a radio group named by this label; any other control is labelled by it directly
    const label = document.createElement(c.t === 'seg' ? 'span' : 'label'); label.className = 'set-label'; label.id = `${id}-label`;
    label.textContent = t(it.label);
    if (label instanceof HTMLLabelElement) label.htmlFor = id;
    const help = document.createElement('p'); help.className = 'set-help'; help.id = `${id}-help`;
    let helpText = t(it.help);
    if (it.key === 'voices') {
      const curLang = this.current('language').toLowerCase();
      if (curLang === 'portuguese') {
        helpText = t(N('Dubbed: Brazilian Portuguese (PT-BR) voices and cutscenes by ZANIT. Original: original English voices and movies with Portuguese text.'));
      }
    }
    const linkText = it.link ? t(it.link.text) : '';
    const at = it.link ? helpText.indexOf(linkText) : -1;
    if (at < 0) help.textContent = helpText;
    else {
      const a = document.createElement('a'); a.href = it.link!.href; a.target = '_blank'; a.rel = 'noopener noreferrer'; a.textContent = linkText;
      help.append(helpText.slice(0, at), a, helpText.slice(at + linkText.length));
    }
    const ctl = document.createElement('div'); ctl.className = 'set-control';
    let described: HTMLElement;                      // what aria-describedby goes on
    if (c.t === 'switch') {
      const on = boolOn(cur || 'off') !== !!c.inv;
      const v = document.createElement('span'); v.className = 'val'; v.setAttribute('aria-hidden', 'true'); v.textContent = on ? t('On') : t('Off');
      const b = document.createElement('input'); b.type = 'checkbox'; b.className = 'switch'; b.id = id;
      b.setAttribute('role', 'switch');
      b.checked = on;
      b.onchange = () => void this.put(it.key, b.checked !== !!c.inv ? 'on' : 'off');
      ctl.append(v, b);
      described = b;
    } else if (c.t === 'seg') {
      const seg = document.createElement('div'); seg.className = 'seg';
      seg.setAttribute('role', 'radiogroup'); seg.setAttribute('aria-labelledby', label.id);
      const opts = [...c.opts];
      if (it.key === 'voices') {
        const curLang = this.current('language').toLowerCase();
        if (curLang === 'portuguese') {
          const dubbedIdx = opts.findIndex(([v]) => v === 'dubbed');
          if (dubbedIdx >= 0) opts[dubbedIdx] = ['dubbed', 'Dubbed (PT-BR)'];
        }
      }
      if (cur && !opts.some(([v]) => v.toLowerCase() === cur.toLowerCase())) opts.push([cur, cur]);   // a value set in the file
      opts.forEach(([v, l], i) => {
        const o = document.createElement('label'); o.className = 'opt';
        const r = document.createElement('input'); r.type = 'radio'; r.name = id; r.value = v;
        if (i === 0) r.id = id;
        r.checked = v.toLowerCase() === (cur || '').toLowerCase();
        r.onchange = () => { if (r.checked) void this.put(it.key, v); };
        o.append(r, optLabel(it, v, l));
        seg.append(o);
      });
      ctl.append(seg);
      described = seg;
    } else if (c.t === 'select') {
      const sel = document.createElement('select'); sel.id = id;
      const opts = [...c.opts];
      if (cur && !opts.some(([v]) => v.toLowerCase() === cur.toLowerCase())) opts.push([cur, cur]);   // a value set in the file
      for (const [v, l] of opts) {
        const o = document.createElement('option'); o.value = v; o.textContent = optLabel(it, v, l);
        o.selected = v.toLowerCase() === (cur || '').toLowerCase();
        sel.append(o);
      }
      sel.onchange = () => {
        void this.put(it.key, sel.value);
        if (it.key === 'language') this.rerow('voices');
      };
      ctl.append(sel);
      if (c.preview) {
        // plays what the list shows (also before it is saved); a second press stops it
        const pv = c.preview, b = document.createElement('button');
        const show = (on: boolean) => { b.setAttribute('aria-pressed', String(on)); b.textContent = b.title = on ? t('Stop') : t('Listen'); };
        b.type = 'button'; b.className = 'listen'; show(false);
        b.onclick = () => {
          if (b.getAttribute('aria-pressed') === 'true') { pv.stop(); return; }
          show(true);
          void pv.play(sel.value).catch(() => {}).finally(() => show(false));
        };
        ctl.append(b);
      }
      described = sel;
    } else if (c.t === 'crews') {
      row.classList.add('set-list');
      described = crewControl({ list: crewsOf(cur, this.current('roam_crew_colours')), id, labelId: label.id, placeholder: c.placeholder, counts: this.crewCounts,
        draft: this.crewDraft, onDraft: v => { this.crewDraft = v; }, change: (list, focus) => void this.setCrews(list, focus) }, ctl);
    } else if (c.t === 'phrases') {
      row.classList.add('set-list');
      described = this.phraseControl(ctl, id, phrasesOf(cur), groupsOf(this.current('quick_chat_groups')),
        setInFile ? () => void this.put(it.key, null, [[comp!, null]]) : null, t(it.label));
    } else {
      const inp = document.createElement('input'); inp.id = id;
      inp.type = c.t === 'num' ? 'number' : 'text';
      if (c.t === 'num') { if (c.min !== undefined) inp.min = String(c.min); if (c.max !== undefined) inp.max = String(c.max); if (c.step !== undefined) inp.step = String(c.step); }
      else if (c.placeholder) inp.placeholder = t(c.placeholder);
      inp.value = cur;
      inp.onchange = () => {
        const v = inp.value.trim();
        if (v === this.current(it.key) && (v !== '' || !this.resolved.file[it.key]?.raw)) return;   // unchanged (a late change event)
        if (v === '') { void this.put(it.key, null); return; }
        const chk = checkSetting(it.key, v);
        if (chk && !chk.ok) {
          const msg = t('Not saved: {key} = {why}.', { key: it.key, why: tvars(chk.why, chk.v) });
          this.state(row, msg, 'bad'); this.describe(inp, row); this.deps.say?.(msg);
          return;
        }
        void this.put(it.key, v);
      };
      ctl.append(inp);
      if (c.t === 'num' && c.unit) { const u = document.createElement('span'); u.className = 'unit'; u.textContent = c.unit; ctl.append(u); }
      described = inp;
    }
    if (setInFile && c.t !== 'crews' && c.t !== 'phrases') {   // (the Crews row has none: each crew's x leaves it; Your
                                                               // phrases has its own, beside Choose phrases)
      const r = document.createElement('button'); r.type = 'button'; r.className = 'text-btn'; r.textContent = t('Reset');
      r.setAttribute('aria-label', t('Reset {label}', { label: t(it.label) }));
      r.title = t('Back to the default ({value})', { value: shown(it, DEFAULTS.get(it.key) ?? '') });
      r.onclick = () => void this.put(it.key, null, comp ? [[comp, null]] : []);
      ctl.prepend(r);                                // first: the control keeps its place at the right edge
    }
    row.append(label, help, ctl);
    const bad = ([[it.key, f], [comp, f2]] as const).filter(([, x]) => x?.refused)
      .map(([k, x]) => t('settings.ini line {line}: {key} = {why}, so the default is used.', { line: x!.phys, key: k!, why: x!.why ? tvars(x!.why.t, x!.why.v) : x!.refused! }));
    if (bad.length) this.state(row, bad.join(' '), 'bad');
    const by = this.fileOnly.byEnv[def.env];
    if (by && keyDef(by.key)?.key !== it.key) this.state(row, t('settings.ini line {line} sets {env} = {value} by its own name: that is what applies.', { line: by.phys, env: def.env, value: this.fileOnly.env[def.env] }), 'env');
    const envName = def.env;
    if (envName in this.deps.environment) this.state(row, t('The page address sets {env}={value}, which wins over this setting.', { env: envName, value: this.deps.environment[envName] }), 'env');
    this.describe(described, row);
    if (this.ro()) for (const el of row.querySelectorAll<HTMLButtonElement | HTMLInputElement | HTMLSelectElement>('button, input, select')) el.disabled = true;
    return row;
  }

  /**
   * Your phrases into ctl: "Choose phrases" opens four columns, one for each of 1 to 4, of the group's name and its
   * four phrases, each a drop-down list (the phrases in the library's categories).  A change saves both lines at once
   * (the default: both back to the template's); reset: the row's Reset, or null when the file sets neither.
   */
  private phraseControl(ctl: HTMLElement, id: string, layout: number[], groups: string[], reset: (() => void) | null, label: string): HTMLElement {
    const det = document.createElement('details'); det.className = 'phrases'; det.open = this.phrasesOpen;
    det.ontoggle = () => { this.phrasesOpen = det.open; };
    const sum = document.createElement('summary'); sum.id = id; sum.textContent = t('Choose phrases');
    const grid = document.createElement('div'); grid.className = 'phrase-grid';
    const save = () => {
      const p = [...grid.querySelectorAll<HTMLSelectElement>('select[data-slot]')].map(s => +s.value);
      const g = [...grid.querySelectorAll<HTMLSelectElement>('select[data-group]')].map(s => s.value);
      const dflt = p.every((x, i) => x === DEFAULT_PHRASES[i]) && g.every((x, i) => x === DEFAULT_GROUPS[i]);
      void this.put('quick_chat_phrases', dflt ? null : p.join(','), [['quick_chat_groups', dflt ? null : g.join(',')]]);
    };
    for (let g = 0; g < 4; g++) {
      const col = document.createElement('div'); col.className = 'phrase-col';
      col.setAttribute('role', 'group'); col.setAttribute('aria-label', t('Group {n}', { n: g + 1 }));
      const head = document.createElement('div'); head.className = 'phrase-head';
      const key = document.createElement('kbd'); key.textContent = String(g + 1); key.setAttribute('aria-hidden', 'true');
      const name = document.createElement('select'); name.id = `${id}-g${g}`; name.dataset.group = String(g);
      name.setAttribute('aria-label', t('Group {n}: its name', { n: g + 1 }));
      for (const c of CATS) name.append(new Option(t(c.name), c.id, false, c.id === groups[g]));
      name.onchange = save;
      head.append(key, name);
      col.append(head);
      for (let i = 0; i < 4; i++) {
        const s = document.createElement('select'); s.id = `${id}-${g * 4 + i}`; s.dataset.slot = String(g * 4 + i);
        s.setAttribute('aria-label', t('Group {n}, phrase {i}', { n: g + 1, i: i + 1 }));
        for (const c of CATS) {
          const og = document.createElement('optgroup'); og.label = t(c.name);
          PHRASES.forEach((p, n) => { if (p.cat === c.id) og.append(new Option(t(p.text), String(n), false, n === layout[g * 4 + i])); });
          s.append(og);
        }
        s.onchange = save;
        col.append(s);
      }
      grid.append(col);
    }
    det.append(sum, grid);
    if (reset) {
      const r = document.createElement('button'); r.type = 'button'; r.className = 'text-btn'; r.textContent = t('Reset');
      r.setAttribute('aria-label', t('Reset {label}', { label }));
      r.title = t('Back to the default ({value})', { value: DEFAULT_PHRASES.slice(0, SLOTS).map(p => t(PHRASES[p].text)).join(', ') });
      r.onclick = reset;
      det.append(r);
    }
    ctl.append(det);
    return sum;
  }

  /** the control is described by its help line, its own note (the Crews box's) and any state line under it */
  private describe(el: HTMLElement, row: HTMLElement) {
    el.setAttribute('aria-describedby', [...row.querySelectorAll('.set-help, .set-note, .set-state')].map(p => p.id).join(' '));
  }

  /** The crews as a Leave, Join or colour left them, saved (names and colours in one write; none left: both lines
   *  back to the template's); then, if the row had the focus, it goes where focus() says. */
  private async setCrews(list: Crew[], focus: (row: HTMLElement) => Element | null | undefined) {
    const had = !!this.find('.set[data-key="roam_crew"]')?.contains(document.activeElement);
    await this.put('roam_crew', list.length ? crewsValue(list.map(c => c.name)) : null, [['roam_crew_colours', list.length ? rgbsValue(list) : null]]);
    const row = this.find<HTMLElement>('.set[data-key="roam_crew"]');
    if (had && row) ((focus(row) ?? row.querySelector('input:enabled')) as HTMLElement | null)?.focus();
  }

  private state(row: HTMLElement, msg: string, cls: string) {
    let p = row.querySelector<HTMLParagraphElement>(`.set-state.${cls}`);
    if (!p) { p = document.createElement('p'); p.id = `set-${row.dataset.key}-${cls}`; row.append(p); }
    p.className = `set-state ${cls}`; p.textContent = msg;
  }

  private async saveRaw() {
    const ta = this.root.querySelector<HTMLTextAreaElement>('#settings-text')!;
    const msg = this.root.querySelector('#settings-text-msg')!;
    if (this.ro()) { msg.textContent = t(READ_ONLY); return; }
    const next = ta.value.endsWith('\n') ? ta.value : ta.value + '\n';
    const r = resolveEnv(next, this.deps.environment, {});
    const bad = r.notes.filter(n => /ignored$|only the page sets it$/.test(n) && !/is set in the environment|already set/.test(n));
    await this.write(next);
    msg.textContent = bad.length ? tn(bad.length, 'Saved. {n} line is ignored: {first}', 'Saved. {n} lines are ignored: {first} …', { first: bad[0] }) : t('Saved.');
  }

  /** "Reset all settings", once the player said yes */
  private async resetAll() {
    if (this.ro()) { this.deps.say?.(t(READ_ONLY)); return; }
    if (this.deps.confirm && !await this.deps.confirm(t('Reset all settings?'),
      t('Every setting here goes back to its default, and any other line in settings.ini is removed. Your name, crews and language stay, as do the options set inside the game.'),
      t('Reset'), true)) return;
    if (this.ro()) { this.deps.say?.(t(READ_ONLY)); return; }       // another tab took the saves meanwhile
    await this.write(resetAll(this.text));
    this.deps.say?.(() => t('Settings are back to their defaults. Applies the next time you press Play.'));
  }

  /** a line of "Other settings in this file" back to its default; a keyboard's focus goes on to the next one */
  private async resetLine(phys: number, key: string, btn: HTMLButtonElement) {
    if (this.ro()) { this.deps.say?.(t(READ_ONLY)); return; }
    const at = [...this.root.querySelectorAll('#settings-other .text-btn')].indexOf(btn), focused = document.activeElement === btn;
    await this.write(resetLine(this.text ?? TEMPLATE, phys));
    if (focused) {
      const rest = this.root.querySelectorAll<HTMLElement>('#settings-other .text-btn');
      (rest[Math.min(at, rest.length - 1)] ?? this.root.querySelector<HTMLElement>('#settings-raw > summary'))?.focus();
    }
    this.deps.say?.(() => t('Saved: {key} is back to its default. Applies the next time you press Play.', { key }));
  }

  /** settings.ini as a whole (the text editor, a reset), and the panel drawn again */
  private async write(next: string) {
    await writeFile(this.deps.saveDir, 'settings.ini', next);
    this.text = next; this.resolve(next);
    this.deps.onSaved?.(next);
    this.render();
    this.root.dataset.saved = String(++this.saves);
  }
}
