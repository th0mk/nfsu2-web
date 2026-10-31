// Free roam's race lobby over the game (roam_game.c mp_panel, NET_KIND_LOBBY): at a race's start what Enter does
// there, a lobby's six places with its players and its countdown; during the trip to the race and back a cover over
// the game's menus (the race and its results stay the game's own; the way back shows the results again).  The game
// sends its state as JSON when it changes, "{}" for nothing to show.

import { language, N, t, tn } from './i18n';

/** what the game sends (roam_game.c mp_panel) */
interface LobbyState {
  /** spot: a multiplayer-only spot, Enter opens a lobby; join: a lobby here to join; full: the lobby here is full,
   *  Enter opens another; lobby: ours, or the one we are in; trip: the way to the race and back; note: a message */
  v?: 'spot' | 'join' | 'full' | 'lobby' | 'trip' | 'note';
  /** lobby: host (ours), asked (joining), in (joined), count (the countdown, t seconds) */
  st?: 'host' | 'asked' | 'in' | 'count';
  /** the race; stage: its career stage */
  kind?: string; track?: string; laps?: number; stage?: number;
  /** the lobby's players, the host first; me: our place in it (-1: none); max: its places; t: the countdown's
   *  seconds; closes: the seconds left of a lobby nobody joined (its last minute); away: its host made no input
   *  lately, the seconds until it closes; fee: what quitting the race costs; even: the race evens out the cars
   *  (their engines) */
  names?: string[]; me?: number; max?: number; t?: number; closes?: number; away?: number; fee?: number; even?: number;
  why?: string; step?: string; text?: string;
  /** trip, once the race's results came: their rows in order; prize: what ours won */
  res?: RaceResult[]; prize?: number;
}
/** a row of the race's results: the player's name, f 1 finished, t its time (s) or p a drift's points, me 1 ours */
interface RaceResult { n: string; f: number; t?: number; p?: number; me?: number }

/** a race's time as the game shows it: 1:02.34 */
const raceTime = (s: number) => `${Math.floor(s / 60)}:${(s % 60).toFixed(2).padStart(5, '0')}`;

// The game's own English in the state (roam_game.c race_kind, mpw_text, mp_note; mprace.c's steps), translated here:
// in English the page shows it as the game sent it.
const GAME_TEXT = [
  N('race'), N('URL race'), N('circuit'), N('sprint'), N('Street X'), N('drag'), N('drift'),
  N('you won it in your career'), N('not open in your career yet'), N('not open in your career'), N('sign with a sponsor first'),
  N('your sponsor\'s contract has no place for it'), N('it opens after more wins'), N('it takes an SUV'),
  N('Back to free roam'), N('Leaving free roam'), N('Setting up the race'), N('Looking for the host\'s race'), N('Starting the race'),
  N('Waiting for the others'), N('Loading the race'),
  N('Multiplayer races need your own car: buy one in your career first'), N('You are no longer in the race'), N('The race is full'),
  N('The race is off: its host is gone'), N('The race is off: you are not in it'), N('The race could not start'), N('The race did not take you'),
  N('Everyone else left: the race waits for players again'), N('Multiplayer race: you quit it'), N('Multiplayer race: not finished'),
  N('You seemed away: the lobby is closed'),
];
const game = (s: string) => language() === 'english' || !GAME_TEXT.includes(s) ? s : t(s);
/** a note the game sent (mp_note): its fixed ones, and the ones with a name or a number in them */
function noteText(s: string): string {
  if (language() === 'english') return s;
  let engine = '';
  const e = / \(your engine ([+-]\d+)% to even out the cars\)$/.exec(s);
  if (e) { s = s.slice(0, e.index); engine = ' ' + t('(your engine {pct}% to even out the cars)', { pct: e[1] }); }
  const forms: [RegExp, (m: RegExpExecArray) => string][] = [
    [/^(.+) called off the race$/, m => t('{name} called off the race', { name: m[1] })],
    [/^(.+) seemed away: the race is called off$/, m => t('{name} seemed away: the race is called off', { name: m[1] })],
    [/^The multiplayer race did not happen: (.*)$/, m => t('The multiplayer race did not happen: {why}', { why: m[1] })],
    [/^Multiplayer race: (\d+)(st|nd|rd|th) place, \$(\d+)$/, m => t('Multiplayer race: {pos} place, ${prize}', { pos: m[1] + m[2], n: m[1], prize: m[3] })],
    [/^Multiplayer race: (\d+)(st|nd|rd|th) place$/, m => t('Multiplayer race: {pos} place', { pos: m[1] + m[2], n: m[1] })],
    [/^Multiplayer race: you quit it, \$(\d+) fee$/, m => t('Multiplayer race: you quit it, ${fee} fee', { fee: m[1] })],
  ];
  for (const [re, f] of forms) { const m = re.exec(s); if (m) return f(m) + engine; }
  return game(s) + engine;
}

/** quick chat: a phrase stays beside its player's name this long (ms) */
const SAID_MS = 6000;

const el = <K extends keyof HTMLElementTagNameMap>(tag: K, cls?: string, text?: string) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
};

export class LobbyPanel {
  private last = '';
  constructor(private panel: HTMLElement, private cover: HTMLElement, private log?: (s: string) => void) {}

  /** the game's state (JSON); a bad one shows nothing */
  show(json: string) {
    if (json === this.last) return;
    this.last = json;
    let s: LobbyState = {};
    try { s = JSON.parse(json) as LobbyState; } catch { this.log?.(`[lobby] not JSON: ${json.slice(0, 80)}`); }
    const was = !this.cover.hidden;
    this.cover.hidden = s.v !== 'trip';
    if (s.v === 'trip') this.drawCover(s);
    else if (was) this.cover.replaceChildren();
    this.panel.hidden = !s.v || s.v === 'trip';
    if (!this.panel.hidden) this.drawPanel(s);
    if (was !== !this.cover.hidden) this.log?.(`[lobby] cover ${this.cover.hidden ? 'off' : 'on'}`);
  }

  /** gone with the game */
  clear() { this.show('{}'); }

  /** the same state again, in the page's language */
  redraw() { const j = this.last; this.last = ''; this.show(j || '{}'); }

  /** quick chat (quickchat.ts): what a player said, beside their name for SAID_MS (by name, as the lobby has them) */
  private said = new Map<string, { text: string; until: number }>();
  say(name: string, text: string) {
    this.said.set(name, { text, until: performance.now() + SAID_MS });
    if (this.last.includes('"names"')) this.redraw();
    setTimeout(() => { if (this.last.includes('"names"')) this.redraw(); }, SAID_MS + 50);
  }

  /** the panel's place: under the game's Bank box, its right edge on the box's (x 299, y -198 to -168 in the HUD's
   *  units from the middle of the picture; one unit is u px), out to the edge of a wide HUD with hud = wide, as the
   *  radar's side (shell.ts placeNotices); v: the game's picture on the page */
  place(v: { x: number; y: number; w: number; h: number }, wide: boolean) {
    const u = Math.min(v.w / 640, v.h / 480), e169 = 320 * (16 / 9 * 0.75 - 1);
    const out = wide ? Math.min(Math.max(320 * (v.w / v.h * 0.75 - 1), 0), e169) * 109 / e169 : 0;
    const s = this.panel.style;
    s.left = `${v.x + v.w / 2 + (299 + out) * u}px`;
    s.top = `${v.y + v.h / 2 - 158 * u}px`;
    s.setProperty('--u', `${u}px`);
    this.cover.style.setProperty('--u', `${u}px`);
  }

  private title(s: LobbyState, host?: string) {
    const kind = game(s.kind ?? 'race');
    const head = el('div', 'lobby-title', host ? t('{host}\'s {kind}', { host, kind }) : t('Multiplayer {kind}', { kind }));
    const sub = [s.stage ? t('Stage {n}', { n: s.stage }) : '', s.track, s.laps ? tn(s.laps, '{n} lap', '{n} laps') : ''].filter(Boolean).join(' · ');
    return sub ? [head, el('div', 'lobby-sub', sub)] : [head];
  }

  /** the lobby's places: its players (the host marked, ours "you"), the rest open */
  private places(s: LobbyState, dim = false) {
    const ol = el('ol', 'lobby-places' + (dim ? ' dim' : ''));
    const names = s.names ?? [], n = Math.max(2, Math.min(6, s.max ?? 6)), now = performance.now();
    for (const [k, w] of this.said) if (w.until <= now) this.said.delete(k);
    for (let i = 0; i < n; i++) {
      const li = el('li', i < names.length ? 'taken' : 'open');
      li.append(el('span', 'lobby-n', String(i + 1)));
      li.append(el('span', 'lobby-name', i < names.length ? names[i] : t('open')));
      if (i < names.length && (i === 0 || i === s.me)) li.append(el('span', 'lobby-tag', i === 0 ? t('host') : t('you')));
      const w = i < names.length ? this.said.get(names[i]) : undefined;
      if (w) li.append(el('span', 'lobby-said', w.text));
      ol.append(li);
    }
    return ol;
  }

  /** a key and what it does */
  private act(key: string | null, what: string) {
    const d = el('div', 'lobby-act');
    if (key) d.append(el('kbd', '', key));
    d.append(el('span', '', what));
    return d;
  }

  private drawPanel(s: LobbyState) {
    const p = this.panel, names = s.names ?? [], host = names[0] ?? t('A player');
    const kids: HTMLElement[] = [];
    p.dataset.v = s.v ?? '';
    switch (s.v) {
      case 'spot':
        kids.push(...this.title(s), el('div', 'lobby-why', t('Multiplayer only here: {why}', { why: game(s.why ?? 'not open in your career') })),
          this.act('Enter', t('Open a lobby')));
        break;
      case 'join':
        kids.push(...this.title(s, host), this.places(s), this.act('Enter', t('Join')));
        break;
      case 'full':
        kids.push(...this.title(s, host), this.places(s), el('div', 'lobby-why', t('This lobby is full.')), this.act('Enter', t('Open another')));
        break;
      case 'lobby': {
        const mine = s.me === 0;
        kids.push(...this.title(s, mine ? undefined : host), this.places(s, s.st === 'asked'));
        if (s.st === 'count') kids.push(el('div', 'lobby-count', t('Starting in {n}', { n: s.t ?? 0 })));
        else if (s.st === 'asked') kids.push(this.act(null, t('Joining {host}\'s {kind}…', { host, kind: game(s.kind ?? 'race') })));
        else if (mine && names.length >= 2) kids.push(this.act('Enter', t('Start the race')), s.away
          ? el('div', 'lobby-warn', t('You seem away: the lobby closes in {n} s unless you press a key', { n: s.away }))
          : el('div', 'lobby-hint', t('Drive away to call it off')));
        else if (mine) kids.push(this.act(null, t('Waiting for players to drive up and join…')),
          el('div', 'lobby-hint', s.closes !== undefined ? t('Closes in {n} s if nobody joins', { n: s.closes }) : t('Drive away to call it off')));
        else kids.push(this.act(null, t('Waiting for {host} to start', { host })), s.away
          ? el('div', 'lobby-warn', t('{host} seems away: the lobby closes in {n} s', { host, n: s.away }))
          : el('div', 'lobby-hint', t('Drive away to leave')));
        break;
      }
      case 'note':
        kids.push(el('div', 'lobby-note', noteText(s.text ?? '')));
        break;
    }
    if (s.v === 'join' && s.even) kids.push(el('div', 'lobby-hint', t('Cars are balanced: slower ones get more power, faster ones a little less')));
    if (s.v === 'join' && s.fee) kids.push(el('div', 'lobby-hint', t('Quitting the race costs ${fee}', { fee: s.fee })));
    p.replaceChildren(...kids);
  }

  /** the race's results on the way back: place, name, time (or points), ours marked with its prize */
  private results(s: LobbyState) {
    const box = el('div', 'lobby-results');
    box.append(el('div', 'lobby-head', t('Results')));
    const ol = el('ol', 'lobby-places');
    let place = 0;
    for (const r of s.res ?? []) {
      const li = el('li', r.me ? 'taken mine' : 'taken');
      li.append(el('span', 'lobby-n', r.f ? String(++place) : '–'), el('span', 'lobby-name', r.n));
      if (r.me) li.append(el('span', 'lobby-tag', t('you')));
      const what = !r.f ? t('DNF') : r.p !== undefined ? tn(r.p, '{n} point', '{n} points') : r.t !== undefined ? raceTime(r.t) : '';
      if (what) li.append(el('span', 'lobby-time', what));
      if (r.me && s.prize) li.append(el('span', 'lobby-prize', `+$${s.prize}`));
      ol.append(li);
    }
    box.append(ol);
    return box;
  }

  private drawCover(s: LobbyState) {
    const card = el('div', 'lobby-card');
    card.append(...this.title(s, undefined), s.res?.length ? this.results(s) : this.places(s));
    const step = el('div', 'lobby-step');
    step.append(el('span', 'lobby-spin'), el('span', '', `${s.step ? game(s.step) : t('Loading')}…`));
    card.append(step);
    this.cover.replaceChildren(card);
  }
}
