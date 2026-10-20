// The settings on the start page: a form over settings.ini, in Settings and,
// for the Online group, in a tab of its own beside Settings and Saves.  Every change
// rewrites the one line of that key (settingsPut, bundle.m's settings_put), so
// the file stays the source of truth and keeps the player's own comments;
// "Reset" puts the template's commented-out line back.  Native controls
// (switches are checkboxes, choices are radio groups), each described by its
// help line, so a screen reader and the keyboard get them as they are.

import { checkSetting, DEFAULTS, keyDef, resolveEnv, settingsPut, TEMPLATE, type Resolved } from './settings';
import { readText, writeFile } from './savefs';

type Control =
  | { t: 'switch' }
  | { t: 'seg'; opts: [value: string, label: string][] }
  | { t: 'num'; min?: number; max?: number; step?: number; unit?: string }
  | { t: 'text'; placeholder?: string };

interface Item { key: string; label: string; help: string; c: Control; more?: boolean }
/** status: the group shows a line of the page's own under its title (Online: who plays on this server now);
 *  own: the group is a section of the page of its own (#online, under Saves), named by that section's heading */
interface Group { title: string; items: Item[]; status?: boolean; own?: boolean }

const GROUPS: Group[] = [
  { title: 'Online', status: true, own: true, items: [
    { key: 'roam', label: 'Free roam with others', help: 'Everyone on this server who turns this on sees your car in career free roam, and you see theirs, with their names over them.', c: { t: 'switch' } },
    { key: 'roam_name', label: 'Your name', help: 'What other players see over your car in free roam, and your name in the Online menu (there only letters, digits, - and _, at most 15). Empty uses your profile name.', c: { t: 'text', placeholder: 'profile name' } },
    { key: 'roam_room', label: 'Room', help: 'Only players in the same room see each other\'s cars. A filter, not privacy: everyone on this server still sees your name and whether you play.', c: { t: 'text', placeholder: 'none' } },
    { key: 'online', label: 'Online mode', help: 'The game\'s Online menu takes you straight to the Online Main Menu under your name, without the EA Login screens: lobby, chat and races with everyone playing from this server.', c: { t: 'switch' } },
    { key: 'roam_collide', label: 'Solid cars', help: 'You collide with the other players\' cars. A car that appears on yours lets you through until you are apart. Off: you drive through them.', c: { t: 'switch' }, more: true },
    { key: 'roam_max_players', label: 'Largest group', help: 'You and the players you exchange cars with at once, 2 to 33: the server picks the nearest.', c: { t: 'num', min: 2, max: 33, step: 1 }, more: true },
    { key: 'roam_max_cars', label: 'Players\' cars shown', help: 'The nearest, 0 to 32.', c: { t: 'num', min: 0, max: 32, step: 1 }, more: true },
    { key: 'roam_discovery', label: 'Find players automatically', help: 'When the server does not pick the players near you: looks for players on this server with a broadcast every 2 s.', c: { t: 'switch' }, more: true },
    { key: 'roam_peers', label: 'Players\' addresses', help: 'a.b.c.d or a.b.c.d:port, separated by commas.', c: { t: 'text', placeholder: 'none' }, more: true },
    { key: 'roam_port', label: 'Port', help: 'The same for every player, 1024 to 65535.', c: { t: 'num', min: 1024, max: 65535, step: 1 }, more: true },
    { key: 'roam_rate', label: 'Updates per second', help: 'How often your car is sent to the others, 10 to 60.', c: { t: 'num', min: 10, max: 60, step: 1 }, more: true },
    { key: 'roam_delay', label: 'Display delay', help: 'With Solid cars off, the other players\' cars are shown this far behind, 0 to 1000. Solid cars are shown where they are now.', c: { t: 'num', min: 0, max: 1000, step: 10, unit: 'ms' }, more: true },
    { key: 'roam_extrap', label: 'Longest extrapolation', help: 'How long another player\'s car keeps moving on its own when updates are late, 0 to 1000.', c: { t: 'num', min: 0, max: 1000, step: 10, unit: 'ms' }, more: true },
    { key: 'roam_carpool_kb', label: 'Car memory pool', help: 'In KB; never below the game\'s own 11000.', c: { t: 'num', min: 0, max: 128000, step: 1000, unit: 'KB' }, more: true },
  ] },
  { title: 'Game', items: [
    { key: 'movies', label: 'Movies', help: 'Intro, story and FMV movies.', c: { t: 'switch' } },
    { key: 'autosave', label: 'Autosave', help: 'Sets each profile\'s Save option in the game to Autosave once, so races, purchases and changes are saved without asking. Prompt or Off chosen afterwards in the game\'s Options, Gameplay stays.', c: { t: 'switch' } },
    { key: 'intro', label: 'Startup movies', help: 'The EA and THX logos, the PSA and the intro. Skippable: Enter, Esc, Space, a left click, or A, B or Start ends each one. Original: as in the game, where only the intro ends early.', c: { t: 'seg', opts: [['skippable', 'Skippable'], ['original', 'Original'], ['off', 'Off']] }, more: true },
    { key: 'sound', label: 'Sound', help: 'All sound output.', c: { t: 'switch' }, more: true },
    { key: 'background', label: 'When the tab is in the background', help: 'Pause freezes the game and its sound, as alt-tab does on Windows. In LAN and Online play it keeps running, without sound.', c: { t: 'seg', opts: [['pause', 'Pause'], ['run', 'Keep running']] }, more: true },
    { key: 'audio_latency', label: 'Output latency', help: 'Lowest reacts fastest; Robust survives a busy PC best.', c: { t: 'seg', opts: [['interactive', 'Lowest'], ['balanced', 'Balanced'], ['playback', 'Robust']] }, more: true },
    { key: 'seed_registry', label: 'Highest graphics for a new save', help: 'The game\'s own graphics options at their highest, at this screen\'s resolution. Off lets the game run its own hardware check.', c: { t: 'switch' }, more: true },
    { key: 'quit_grace', label: 'Time to quit', help: 'Seconds the game gets to shut down after Quit game before it is stopped.', c: { t: 'num', min: 0, step: 1, unit: 's' }, more: true },
    { key: 'watchdog', label: 'Stall report', help: 'Seconds without a frame before the log gets a report of every game thread (0 = off).', c: { t: 'num', min: 0, step: 1, unit: 's' }, more: true },
  ] },
  { title: 'Display', items: [
    { key: 'fullscreen', label: 'Full screen on Play', help: 'Off plays in this browser window; F9 has full screen while you play. Hold Esc to leave it.', c: { t: 'switch' } },
    { key: 'render', label: 'Resolution scale', help: 'Scales the game\'s screen-size resolution (its default), per side: 50% renders a quarter of the pixels. Lower is faster and softer.', c: { t: 'seg', opts: [['scale:0.5', '50%'], ['scale:0.75', '75%'], ['backing', '100%'], ['scale:1.5', '150%'], ['scale:2', '200%']] } },
    { key: 'fps', label: 'Frame rate', help: 'Auto follows the display (120 on a 120 Hz screen, 240 on a 240 Hz one). Online and LAN races run at up to 120, as on a PC; cutscenes stay at 30.', c: { t: 'seg', opts: [['auto', 'Auto'], ['60', '60'], ['120', '120'], ['240', '240']] } },
    { key: 'hud', label: 'Race HUD', help: 'Wide anchors it to the screen edges; 4:3 is the original centred layout.', c: { t: 'seg', opts: [['wide', 'Wide'], ['43', '4:3']] }, more: true },
    { key: 'movie_framing', label: 'Movie framing', help: 'Fit keeps the 4:3 movies whole; fill crops them; stretch widens them.', c: { t: 'seg', opts: [['fit', 'Fit'], ['fill', 'Fill'], ['stretch', 'Stretch']] }, more: true },
    { key: 'cutscene_borders', label: 'Cutscene bars', help: 'Letterbox bars across the whole width, or none.', c: { t: 'seg', opts: [['full', 'Full'], ['off', 'Off']] }, more: true },
    { key: 'other_headlights', label: 'Other cars\' headlights', help: 'Light on the road from other cars, as your own headlights give. Players: friends in free roam and the other players in Online and LAN races. All: AI racers and traffic too. Off: only yours, as in the original game.', c: { t: 'seg', opts: [['players', 'Players'], ['all', 'All cars'], ['off', 'Off']] }, more: true },
    { key: 'fps_counter', label: 'Frame-rate counter', help: 'In a corner of the game. F8 cycles it while you play.', c: { t: 'seg', opts: [['off', 'Off'], ['on', 'On'], ['detailed', 'Detailed']] }, more: true },
    { key: 'resolution', label: 'Fixed render size', help: 'Renders every resolution at this size, for example 2560x1440. Empty uses the one chosen in the game.', c: { t: 'text', placeholder: 'WxH' }, more: true },
    { key: 'max_pixels', label: 'Pixel limit', help: 'Most pixels the screen-size resolution renders (0 = no limit).', c: { t: 'num', min: 0, step: 100000 }, more: true },
    { key: 'hfr_fixes', label: 'High-frame-rate fixes', help: 'For the parts of the game that count frames: default, all, none, or a list of ticks, timer, particles, trail, audio, rtf.', c: { t: 'text', placeholder: 'default' }, more: true },
    { key: 'nos_trail_length', label: 'Nitrous trail length', help: 'Length factor of the nitrous and speed-streak trails (1 = normal).', c: { t: 'num', min: 0, step: 0.1 }, more: true },
  ] },
  { title: 'Controller', items: [
    { key: 'rumble', label: 'Rumble', help: 'On controllers that have it: crashes, hard landings and the game\'s own force feedback.', c: { t: 'switch' } },
    { key: 'gamepad', label: 'Controller', help: 'On keeps a controller once it was connected; hotplug only while connected.', c: { t: 'seg', opts: [['on', 'On'], ['hotplug', 'Hotplug'], ['off', 'Off']] }, more: true },
    { key: 'gamepad_menu_keys', label: 'Menu buttons', help: 'A, B and View also act as Enter, Esc and Q in the menus.', c: { t: 'switch' }, more: true },
    { key: 'gamepad_deadzone', label: 'Stick dead zone', help: 'Fraction of full travel, 0 to 0.9.', c: { t: 'num', min: 0, max: 0.9, step: 0.01 }, more: true },
    { key: 'gamepad_trigger_deadzone', label: 'Trigger dead zone', help: 'Fraction of full travel, 0 to 0.9.', c: { t: 'num', min: 0, max: 0.9, step: 0.01 }, more: true },
    { key: 'gamepad_trigger_click', label: 'Trigger full press', help: 'How far a trigger travels before it counts as fully pressed (full throttle, full brake).', c: { t: 'num', min: 0, max: 1, step: 0.01 }, more: true },
  ] },
];

/** Every key the panel shows (the check in app-test.mjs compares this with the template). */
export const PANEL_KEYS = GROUPS.flatMap(g => g.items.map(i => i.key));
const ITEMS = new Map(GROUPS.flatMap(g => g.items.map(i => [i.key, i] as const)));
const GROUP_OF = new Map(GROUPS.flatMap(g => g.items.map(i => [i.key, g] as const)));

const boolOn = (v: string) => v === 'on';
const READ_ONLY = 'Read only here: another tab of this browser runs the game and owns its saves and settings. Change them there; this tab takes over when that one closes.';

/** A value as the panel shows it: the option's label, on/off, or the value with its unit. */
function shown(it: Item, v: string): string {
  const c = it.c;
  if (c.t === 'switch') return v === 'on' ? 'on' : 'off';
  if (c.t === 'seg') return c.opts.find(([x]) => x.toLowerCase() === v.toLowerCase())?.[1] ?? v;
  if (v === '') return 'empty';
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

export interface SettingsDeps {
  saveDir: string;
  /** the page address's environment (overrides the file) */
  environment: Record<string, string>;
  /** after every write (it applies at the next Play, which reads the file again) */
  onSaved?(text: string): void;
  /** true in a tab that does not own the saves: the panel shows the file but does not write it */
  readOnly?(): boolean;
  /** a short message for the page: what was saved, or why not */
  say?(msg: string): void;
}

export class SettingsPanel {
  text: string | null = null;
  private resolved!: Resolved;
  /** the file alone (no page address): what each row shows */
  private fileOnly!: Resolved;
  private saves = 0;                       // writes so far (tests wait on data-saved)
  private openMore = new Set<string>();    // "More ... settings" kept open across re-renders
  private note0: string;                   // the section's own note, back once the tab owns the saves
  private status = '';                     // the Online group's line (setStatus)
  /** root: the Settings section; online: the section of the group that is its own (Online, under Saves) */
  constructor(private root: HTMLElement, private deps: SettingsDeps, private online: HTMLElement) {
    this.note0 = root.querySelector('#settings-note')!.textContent ?? '';
    root.querySelector<HTMLButtonElement>('#settings-text-save')!.onclick = () => void this.saveRaw();
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
  }

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
    return DEFAULTS.get(key) ?? '';
  }

  private ro() { return !!this.deps.readOnly?.(); }

  private async put(key: string, value: string | null) {
    if (this.ro()) { this.deps.say?.(READ_ONLY); return; }
    const { text: next, removed } = putKey(this.text ?? TEMPLATE, key, value);
    await writeFile(this.deps.saveDir, 'settings.ini', next);
    this.text = next;
    this.resolve(next);
    this.deps.onSaved?.(next);
    if (removed) this.render(); else { this.rerow(key); this.syncRaw(); }
    this.root.dataset.saved = String(++this.saves);
    const it = ITEMS.get(key);
    this.deps.say?.(`Saved: ${it?.label ?? key}, ${it ? shown(it, this.current(key)) : value}${value === null ? ' (the default)' : ''}. Applies the next time you press Play.`);
  }

  render() {
    const ro = this.ro();
    this.root.toggleAttribute('data-readonly', ro);
    this.root.querySelector('#settings-note')!.textContent = ro ? READ_ONLY : this.note0;
    this.online.toggleAttribute('data-readonly', ro);
    const note = this.online.querySelector<HTMLElement>('#online-note')!;
    note.textContent = ro ? READ_ONLY : ''; note.hidden = !ro;
    const focusKey = (document.activeElement as HTMLElement | null)?.closest<HTMLElement>('.set')?.dataset.key;
    this.root.querySelector('#settings-body')!.replaceChildren(...GROUPS.filter(g => !g.own).map(g => this.group(g)), ...this.others());
    this.online.querySelector('#online-body')!.replaceChildren(...GROUPS.filter(g => g.own).map(g => this.group(g)));
    this.syncRaw();
    const row = focusKey ? this.find<HTMLElement>(`.set[data-key="${focusKey}"]`) : null;
    if (row) this.focusRow(row);
  }

  /** an element of the Settings section or of the Online one */
  private find<T extends Element>(sel: string): T | null { return this.root.querySelector<T>(sel) ?? this.online.querySelector<T>(sel); }

  /** The Online group's line under its title ('' hides it); kept across re-renders. */
  setStatus(line: string) {
    this.status = line;
    const p = this.find<HTMLElement>('#group-status');
    if (p) { p.textContent = line; p.hidden = !line; }
  }

  private group(g: Group): HTMLElement {
    const box = document.createElement('div'); box.className = 'group';
    // a group in its own section has that section's heading
    if (!g.own) {
      const h = document.createElement('h3'); h.id = `group-${g.title.toLowerCase().replace(/\W+/g, '-')}`; h.textContent = g.title;
      box.setAttribute('role', 'group'); box.setAttribute('aria-labelledby', h.id);
      box.append(h);
    }
    if (g.status) {
      const p = document.createElement('p'); p.className = 'hint group-status'; p.id = 'group-status';
      p.textContent = this.status; p.hidden = !this.status;
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
    return `More ${g.title.toLowerCase()} settings${n ? ` (${n} changed)` : ''}`;
  }

  /** The lines with keys outside the table: listed, so nothing in the file acts unseen (the audit's PAGE-2). */
  private others(): HTMLElement[] {
    const other = this.fileOnly.other;
    if (!other.length) return [];
    const box = document.createElement('div'); box.className = 'group'; box.id = 'settings-other';
    const h = document.createElement('h3'); h.id = 'group-other'; h.textContent = 'Other variables in this file';
    box.setAttribute('role', 'group'); box.setAttribute('aria-labelledby', h.id);
    const p = document.createElement('p'); p.className = 'set-help';
    p.textContent = 'Lines that set a runtime variable by its own name. They apply at Play; edit the file as text below to change them.';
    const ul = document.createElement('ul');
    for (const o of other) {
      const li = document.createElement('li'); li.className = 'set-help';
      li.textContent = `line ${o.phys}: ${o.key} = ${o.value.length > 80 ? o.value.slice(0, 80) + '…' : o.value}` +
        (o.refused ? ` (ignored: ${o.refused})` : o.env !== o.key ? ` (sets ${o.env})` : '');
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
    const focused = old.contains(document.activeElement);
    const fresh = this.row(it);
    old.replaceWith(fresh);
    if (it.more) fresh.parentElement!.querySelector('summary')!.textContent = this.moreLabel(GROUP_OF.get(key)!);
    if (focused) this.focusRow(fresh);
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
  }

  private row(it: Item): HTMLElement {
    const def = keyDef(it.key)!;
    const row = document.createElement('div'); row.className = 'set'; row.dataset.key = it.key;
    const id = `set-${it.key}`;
    const cur = this.current(it.key);
    const f = this.resolved.file[it.key];
    const setInFile = !!f && f.raw !== '';          // set (or refused): Reset puts the template line back
    const c = it.c;
    // a choice is a radio group named by this label; any other control is labelled by it directly
    const label = document.createElement(c.t === 'seg' ? 'span' : 'label'); label.className = 'set-label'; label.id = `${id}-label`;
    label.textContent = it.label;
    if (label instanceof HTMLLabelElement) label.htmlFor = id;
    const help = document.createElement('p'); help.className = 'set-help'; help.id = `${id}-help`; help.textContent = it.help;
    const ctl = document.createElement('div'); ctl.className = 'set-control';
    let described: HTMLElement;                      // what aria-describedby goes on
    if (c.t === 'switch') {
      const on = boolOn(cur || 'off');
      const v = document.createElement('span'); v.className = 'val'; v.setAttribute('aria-hidden', 'true'); v.textContent = on ? 'On' : 'Off';
      const b = document.createElement('input'); b.type = 'checkbox'; b.className = 'switch'; b.id = id;
      b.setAttribute('role', 'switch');
      b.checked = on;
      b.onchange = () => void this.put(it.key, b.checked ? 'on' : 'off');
      ctl.append(v, b);
      described = b;
    } else if (c.t === 'seg') {
      const seg = document.createElement('div'); seg.className = 'seg';
      seg.setAttribute('role', 'radiogroup'); seg.setAttribute('aria-labelledby', label.id);
      const opts = [...c.opts];
      if (cur && !opts.some(([v]) => v.toLowerCase() === cur.toLowerCase())) opts.push([cur, cur]);   // a value set in the file
      opts.forEach(([v, l], i) => {
        const o = document.createElement('label'); o.className = 'opt';
        const r = document.createElement('input'); r.type = 'radio'; r.name = id; r.value = v;
        if (i === 0) r.id = id;
        r.checked = v.toLowerCase() === (cur || '').toLowerCase();
        r.onchange = () => { if (r.checked) void this.put(it.key, v); };
        o.append(r, l);
        seg.append(o);
      });
      ctl.append(seg);
      described = seg;
    } else {
      const inp = document.createElement('input'); inp.id = id;
      inp.type = c.t === 'num' ? 'number' : 'text';
      if (c.t === 'num') { if (c.min !== undefined) inp.min = String(c.min); if (c.max !== undefined) inp.max = String(c.max); if (c.step !== undefined) inp.step = String(c.step); }
      else if (c.placeholder) inp.placeholder = c.placeholder;
      inp.value = cur;
      inp.onchange = () => {
        const v = inp.value.trim();
        if (v === this.current(it.key) && (v !== '' || !this.resolved.file[it.key]?.raw)) return;   // unchanged (a late change event)
        if (v === '') { void this.put(it.key, null); return; }
        const chk = checkSetting(it.key, v);
        if (chk && !chk.ok) {
          const msg = `Not saved: ${it.key} = ${chk.reason}.`;
          this.state(row, msg, 'bad'); this.describe(inp, row); this.deps.say?.(msg);
          return;
        }
        void this.put(it.key, v);
      };
      ctl.append(inp);
      if (c.t === 'num' && c.unit) { const u = document.createElement('span'); u.className = 'unit'; u.textContent = c.unit; ctl.append(u); }
      described = inp;
    }
    if (setInFile) {
      const r = document.createElement('button'); r.type = 'button'; r.className = 'text-btn'; r.textContent = 'Reset';
      r.setAttribute('aria-label', `Reset ${it.label}`);
      r.title = `Back to the default (${shown(it, DEFAULTS.get(it.key) ?? '')})`;
      r.onclick = () => void this.put(it.key, null);
      ctl.prepend(r);                                // first: the control keeps its place at the right edge
    }
    row.append(label, help, ctl);
    if (f?.refused) this.state(row, `settings.ini line ${f.phys}: ${it.key} = ${f.refused}, so the default is used.`, 'bad');
    const by = this.fileOnly.byEnv[def.env];
    if (by && keyDef(by.key)?.key !== it.key) this.state(row, `settings.ini line ${by.phys} sets ${def.env} = ${this.fileOnly.env[def.env]} by its own name: that is what applies.`, 'env');
    const envName = def.env;
    if (envName in this.deps.environment) this.state(row, `The page address sets ${envName}=${this.deps.environment[envName]}, which wins over this setting.`, 'env');
    this.describe(described, row);
    if (this.ro()) for (const el of row.querySelectorAll<HTMLButtonElement | HTMLInputElement>('button, input')) el.disabled = true;
    return row;
  }

  /** the control is described by its help line and any state line under it */
  private describe(el: HTMLElement, row: HTMLElement) {
    el.setAttribute('aria-describedby', [...row.querySelectorAll('.set-help, .set-state')].map(p => p.id).join(' '));
  }

  private state(row: HTMLElement, msg: string, cls: string) {
    let p = row.querySelector<HTMLParagraphElement>(`.set-state.${cls}`);
    if (!p) { p = document.createElement('p'); p.id = `set-${row.dataset.key}-${cls}`; row.append(p); }
    p.className = `set-state ${cls}`; p.textContent = msg;
  }

  private async saveRaw() {
    const ta = this.root.querySelector<HTMLTextAreaElement>('#settings-text')!;
    const msg = this.root.querySelector('#settings-text-msg')!;
    if (this.ro()) { msg.textContent = READ_ONLY; return; }
    const next = ta.value.endsWith('\n') ? ta.value : ta.value + '\n';
    const r = resolveEnv(next, this.deps.environment, {});
    const bad = r.notes.filter(n => /ignored$|only the page sets it$/.test(n) && !/is set in the environment|already set/.test(n));
    await writeFile(this.deps.saveDir, 'settings.ini', next);
    this.text = next; this.resolve(next);
    this.deps.onSaved?.(next);
    this.render();
    this.root.dataset.saved = String(++this.saves);
    msg.textContent = bad.length ? `Saved. ${bad.length} line${bad.length > 1 ? 's are' : ' is'} ignored: ${bad[0]}${bad.length > 1 ? ' …' : ''}` : 'Saved.';
  }
}
