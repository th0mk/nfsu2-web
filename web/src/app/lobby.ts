// Free roam's race lobby over the game (roam_game.c mp_panel, NET_KIND_LOBBY): at a race's start what Enter does
// there, a lobby's six places with its players and its countdown; during the trip to the race and back a cover over
// the game's menus (the race and its results stay the game's own; the way back shows the results again).  The game
// sends its state as JSON when it changes, "{}" for nothing to show.

import { language, N, t, tn } from './i18n';
import { touchFirst } from './device';
import { drivingPad, PAD_DRIVER_EVENT } from '../input/gamepad';
import { gameHas, KEY_LOBBY, KEY_LOBBY_INVITE, KEY_LOBBY_NEW, KEY_LOBBY_PRIVATE } from './gamekeys';
import { keyName, siteKey, type OurId } from './sitekeys';
import { pageDialog } from './padnav';
import { act, block, el } from './cards';
import { LB_LEAVE, LB_NEW, LB_NEXT, LB_PRIVATE } from '../input/proto';
import type { GameSession } from './game';

/** what the game sends (roam_game.c mp_panel) */
interface LobbyState {
  /** spot: a multiplayer-only spot, or the "open a new lobby" page at a start, Enter opens a lobby; join: a lobby
   *  here to join (the game sends only the lobbies we may join: mp_shown); lobby: ours, or the one we are in; trip:
   *  the way to the race and back; note: a message */
  v?: 'spot' | 'join' | 'lobby' | 'trip' | 'note';
  /** lobby: host (ours), asked (joining), in (joined), count (the countdown, t seconds) */
  st?: 'host' | 'asked' | 'in' | 'count';
  /** the race; stage: its career stage */
  kind?: string; track?: string; laps?: number; stage?: number;
  /** the lobby's players, the host first; me: our place in it (-1: none); max: its places; t: the countdown's
   *  seconds; closes: the seconds left of a lobby nobody joined (its last minute); away: its host made no input
   *  lately, the seconds until it closes; fee: what quitting the race costs (the game also sends even, the race evens
   *  out the engines, and outpaced, a warning by our car's class: the speedometer says it now) */
  names?: string[]; me?: number; max?: number; t?: number; closes?: number; away?: number; fee?: number;
  why?: string; step?: string; text?: string;
  /** counts: 1 a win counts in our career (a race we could win with Solo here); c: each player's own (1 counts for
   *  them); fr: each player our friend (their win counts for us too); cr: each player a crew-mate, not a friend (the same) */
  counts?: number; c?: number[]; fr?: number[]; cr?: number[];
  /** trip, once the race's results came: their rows in order; prize: what ours won; credit: the race counts in our
   *  career, 1 we won it, 2 a friend or crew-mate did; credited -1: the career did not take it (the prize money instead) */
  res?: RaceResult[]; prize?: number; credit?: number; credited?: number;
  /** a URL race of heats: how many (the race then shows no track: each heat has its own); on the trip heat: the one
   *  raced now or last (1 ..) and track its track, fin: the last heat's results are in (res is the series' standing,
   *  each row's points), stand: the points after the heats raced so far, the leader first */
  heats?: number; heat?: number; fin?: number; stand?: Standing[];
  /** the lobbies at a start, a page each (join) and "open a new lobby" last (spot): this page pg of pgs (none: one
   *  page), nl of them lobbies; card: an open race's start, where Enter brings the race's own card, whose Join or
   *  Multiplayer does it */
  pg?: number; pgs?: number; nl?: number; card?: number;
  /** lobby: Friends & crew only (hosting: its switch); far: we joined it from afar (an invite), so driving away does
   *  not leave it */
  priv?: number; far?: number;
  /** hosting: who can join (the card's V, runtime mp_act): stage, its race's stage (upto) and below and the host's
   *  friends and crew; friends, Friends & crew only; all, everyone */
  who?: 'stage' | 'friends' | 'all'; upto?: number;
  /** join: its host is past the race's stage, we are not (the card says so) */
  hpast?: number;
  /** each player's car's type ("SKYLINE"; "" not known); vs: the fastest other car's rating over ours with the
   *  engines evened out (the speedometer under the players; none: not known) */
  car?: string[]; vs?: number;
}
/** the lobby the player hosts, as an invite tells a friend of it (friends.ts) */
export interface Hosted { kind: string; stage: number; n: number; max: number }

/** what the lobby card's keys and buttons do (shell.ts): one of the game's (input proto.ts LB_*), and the invite (the
 *  page menu's Friends popup, its Invite) */
export interface LobbyDeps {
  act(what: number): void;
  invite(): void;
  session(): GameSession | null;
}
/** a player's points in a URL race's heats so far, me 1 ours */
interface Standing { n: string; p: number; me?: number }
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
  N('Waiting for the others'), N('Loading the race'), N('Getting the next heat ready'),
  N('Multiplayer races need your own car: buy one in your career first'), N('Join multiplayer races from your career\'s free roam'),
  N('You are no longer in the race'), N('The race is full'),
  N('The race is off: its host is gone'), N('The race is off: you are not in it'), N('The race could not start'), N('The race did not take you'),
  N('Everyone else left: the race waits for players again'), N('Multiplayer race: you quit it'), N('Multiplayer race: not finished'),
  N('You seemed away: the lobby is closed'), N('You left the race'), N('Call off your own race first: drive away from its start'),
];
/** a race's kind in the page's language (an invite's line names it: friends.ts) */
export const raceKindText = (s: string) => game(s);
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
    [/^Multiplayer race: (\d+)(st|nd|rd|th) place: the win goes into your career at your next garage visit$/,
      m => t('Multiplayer race: {pos} place: the win goes into your career at your next garage visit', { pos: m[1] + m[2], n: m[1] })],
    [/^Multiplayer race: (\d+)(st|nd|rd|th) place\. (.+) won it: into your career at your next garage visit$/,
      m => t('Multiplayer race: {pos} place. {name} won it: into your career at your next garage visit', { pos: m[1] + m[2], n: m[1], name: m[3] })],
    [/^Multiplayer race: (\d+)(st|nd|rd|th) place, won in your career, \$(\d+)$/,
      m => t('Multiplayer race: {pos} place, won in your career, ${prize}', { pos: m[1] + m[2], n: m[1], prize: m[3] })],
    [/^Multiplayer race: (\d+)(st|nd|rd|th) place, won in your career$/, m => t('Multiplayer race: {pos} place, won in your career', { pos: m[1] + m[2], n: m[1] })],
    [/^Multiplayer race: (\d+)(st|nd|rd|th) place\. (.+) won it: it counts in your career too, \$(\d+)$/,
      m => t('Multiplayer race: {pos} place. {name} won it: it counts in your career too, ${prize}', { pos: m[1] + m[2], n: m[1], name: m[3], prize: m[4] })],
    [/^Multiplayer race: (\d+)(st|nd|rd|th) place\. (.+) won it: it counts in your career too$/,
      m => t('Multiplayer race: {pos} place. {name} won it: it counts in your career too', { pos: m[1] + m[2], n: m[1], name: m[3] })],
    [/^Multiplayer race: (\d+)(st|nd|rd|th) place, \$(\d+)\. Only a win counts in your career$/,
      m => t('Multiplayer race: {pos} place, ${prize}. Only a win counts in your career', { pos: m[1] + m[2], n: m[1], prize: m[3] })],
    [/^Multiplayer race: (\d+)(st|nd|rd|th) place\. Only a win counts in your career$/,
      m => t('Multiplayer race: {pos} place. Only a win counts in your career', { pos: m[1] + m[2], n: m[1] })],
    [/^Multiplayer race: (\d+)(st|nd|rd|th) place, \$(\d+)$/, m => t('Multiplayer race: {pos} place, ${prize}', { pos: m[1] + m[2], n: m[1], prize: m[3] })],
    [/^Multiplayer race: (\d+)(st|nd|rd|th) place$/, m => t('Multiplayer race: {pos} place', { pos: m[1] + m[2], n: m[1] })],
    [/^Multiplayer race: you quit it, \$(\d+) fee$/, m => t('Multiplayer race: you quit it, ${fee} fee', { fee: m[1] })],
  ];
  for (const [re, f] of forms) { const m = re.exec(s); if (m) return f(m) + engine; }
  return game(s) + engine;
}

/** the speedometer under the players (the fastest other car against ours with the engines evened out, vs: runtime
 *  roam_game.c js_players): its needle straight up for a car as fast as ours, at its ends for 1.6 times ours or more
 *  and as much less, in the red past 1.25 times (rated about 2.5 times ours: a stock car against a maxed one) */
const SVGNS = 'http://www.w3.org/2000/svg';
function speedometer(vs: number): SVGSVGElement {
  const svg = document.createElementNS(SVGNS, 'svg'), cx = 12, cy = 12, r = 10;
  svg.setAttribute('viewBox', '0 0 24 14'); svg.setAttribute('class', 'lobby-gauge'); svg.setAttribute('aria-hidden', 'true');
  const pt = (k: number, rr: number) => {
    const a = (90 - Math.max(-1, Math.min(1, Math.log(k) / Math.log(1.6))) * 80) * Math.PI / 180;
    return `${(cx + rr * Math.cos(a)).toFixed(2)} ${(cy - rr * Math.sin(a)).toFixed(2)}`;
  };
  const path = (d: string, cls: string) => { const e = document.createElementNS(SVGNS, 'path'); e.setAttribute('d', d); e.setAttribute('class', cls); svg.append(e); };
  path(`M${pt(1 / 1.6, r)} A${r} ${r} 0 0 1 ${pt(1.25, r)}`, 'track');
  path(`M${pt(1.25, r)} A${r} ${r} 0 0 1 ${pt(1.6, r)}`, 'red');
  path(`M${cx} ${cy} L${pt(vs, r - 2)}`, 'needle');
  return svg;
}

/** quick chat: a phrase stays beside its player's name this long (ms) */
const SAID_MS = 6000;



/** the card's keys: the player's for each (sitekeys.ts), and the status bit that says the game has one of its controls
 *  on it (then the key is the game's: gamekeys.ts) */
const KEYS: Record<'next' | 'fresh' | 'priv' | 'invite', { id: OurId; bit: number }> = {
  next: { id: 'lobby', bit: KEY_LOBBY }, fresh: { id: 'lobbyNew', bit: KEY_LOBBY_NEW }, priv: { id: 'lobbyPrivate', bit: KEY_LOBBY_PRIVATE },
  invite: { id: 'lobbyInvite', bit: KEY_LOBBY_INVITE },
};

export class LobbyPanel {
  private last = '';
  /** the state shown now */
  private st: LobbyState = {};
  constructor(private panel: HTMLElement, private cover: HTMLElement, private log?: (s: string) => void, private deps?: LobbyDeps) {
    // (caught before the game's input, and only while the card offers it: E, V and Z are the game's for nothing in its
    // driving map, but a player may put one of its controls on them)
    window.addEventListener('keydown', e => this.key(e), { capture: true });
    // (a controller that starts or stops driving: the keys' caps change, lobby.ts cap)
    window.addEventListener(PAD_DRIVER_EVENT, () => { if (!this.panel.hidden) this.redraw(); });
  }

  /** the lobby the player hosts now with room in it (null: none) */
  hosted(): Hosted | null {
    const s = this.st;
    if (s.v !== 'lobby' || s.st !== 'host' || (s.names?.length ?? 0) >= (s.max ?? 6)) return null;
    return { kind: s.kind ?? 'race', stage: s.stage ?? 0, n: s.names?.length ?? 1, max: s.max ?? 6 };
  }

  /** what the card offers now, by its keys: at a start the next lobby (two or more) and a new one beside them (where
   *  the start takes one: the game's pages, pgs, are its lobbies, nl, and a new one's), who can join and invite
   *  (hosting), leave (from afar) */
  private offers(s: LobbyState): { next?: () => void; fresh?: () => void; priv?: () => void; invite?: () => void } {
    const d = this.deps, o: { next?: () => void; fresh?: () => void; priv?: () => void; invite?: () => void } = {};
    if (!d) return o;
    if (s.v === 'join' && (s.nl ?? 0) > 1) o.next = () => d.act(LB_NEXT);
    if (s.v === 'join' && (s.pgs ?? 0) > (s.nl ?? 0)) o.fresh = () => d.act(LB_NEW);
    if (s.v === 'lobby' && s.st === 'host') {
      o.priv = () => d.act(LB_PRIVATE);
      if ((s.names?.length ?? 0) < (s.max ?? 6)) o.invite = () => d.invite();
    }
    if (s.v === 'lobby' && (s.st === 'in' || s.st === 'asked') && s.far) o.invite = () => d.act(LB_LEAVE);
    return o;
  }

  /** a card key's key now (null: none, off, or the game's) */
  private keyOf(which: keyof typeof KEYS): string | null {
    const k = siteKey(KEYS[which].id);
    return k && !gameHas(this.deps?.session(), KEYS[which].bit) ? k : null;
  }

  private key(e: KeyboardEvent) {
    if (this.panel.hidden || e.metaKey || e.ctrlKey || e.altKey || !this.deps) return;
    const el = e.target as HTMLElement | null;
    if (el?.closest?.('input, textarea, select') || pageDialog() || this.panel.closest('.photo')) return;
    const o = this.offers(this.st);
    const f = (Object.keys(KEYS) as (keyof typeof KEYS)[]).find(w => o[w] && this.keyOf(w) === e.code);
    if (!f) return;
    e.preventDefault(); e.stopImmediatePropagation();
    if (!e.repeat) o[f]!();
  }

  /** the game's state (JSON); a bad one shows nothing */
  show(json: string) {
    if (json === this.last) return;
    this.last = json;
    let s: LobbyState = {};
    try { s = JSON.parse(json) as LobbyState; } catch { this.log?.(`[lobby] not JSON: ${json.slice(0, 80)}`); }
    this.st = s;
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

  /** the race and how full it is; where it is (its stage, track, laps), a win's worth in our career */
  private head(s: LobbyState, trip = false) {
    const top = el('div', 'lobby-top');
    top.append(el('span', 'lobby-race ov-title', game(s.kind ?? 'race')));
    if (s.names?.length && !trip) top.append(el('span', 'lobby-count', t('Lobby {n}/{max}', { n: s.names.length, max: s.max ?? 6 })));
    const where = [s.stage ? t('Stage {n}', { n: s.stage }) : '', this.heats(s), s.track, s.laps ? tn(s.laps, '{n} lap', '{n} laps') : ''].filter(Boolean).join(' · ');
    const out = [top];
    if (where) out.push(el('div', 'ov-small', where));
    if (s.counts) {
      // (and a friend's or crew-mate's win, with such players in it: theirs counts for us too)
      const fr = (s.fr ?? []).some(Boolean), cr = (s.cr ?? []).some(Boolean), d = el('div', 'ov-small');
      d.append(el('span', 'lobby-star', '★ '), fr && cr ? t('A win counts in your career, and so does a friend\'s or crew-mate\'s')
        : fr ? t('A win counts in your career, and so does a friend\'s') : cr ? t('A win counts in your career, and so does a crew-mate\'s')
        : t('A win counts in your career'));
      out.push(d);
    }
    return out;
  }

  /** a URL race of heats: how many, or on the trip the heat raced now */
  private heats(s: LobbyState) {
    if (!s.heats || s.heats < 2) return '';
    return s.heat ? t('Heat {k} of {n}', { k: s.heat, n: s.heats }) : tn(s.heats, '{n} heat', '{n} heats');
  }

  /** between a URL race's heats: the points so far, the leader first, ours marked */
  private standings(s: LobbyState) {
    const box = el('div', 'lobby-results');
    box.append(el('div', 'lobby-head', t('Standings')));
    const ol = el('ol', 'lobby-places');
    (s.stand ?? []).forEach((r, i) => {
      const li = el('li', r.me ? 'taken mine' : 'taken');
      li.append(el('span', 'lobby-n', String(i + 1)), el('span', 'lobby-name', r.n));
      li.append(el('span', 'lobby-time', tn(r.p, '{n} point', '{n} points')));
      ol.append(li);
    });
    box.append(ol);
    return box;
  }

  /** the lobby's places: its players, the host first and marked, ★ where a win counts in that player's career, our
   *  friends and crew-mates in lime, ours bold (as over the cars in free roam); then the open ones */
  private players(s: LobbyState, open = true) {
    const ul = el('ul', s.st === 'asked' ? 'lobby-players dim' : 'lobby-players'), now = performance.now();
    const names = s.names ?? [], free = open ? Math.max(2, Math.min(6, s.max ?? 6)) - names.length : 0;
    for (const [k, w] of this.said) if (w.until <= now) this.said.delete(k);
    names.forEach((name, i) => {
      const li = el('li', i === s.me ? 'me' : s.fr?.[i] || s.cr?.[i] ? 'mate' : '');
      li.append(el('span', 'lobby-pname', name));
      const w = this.said.get(name);
      if (w) li.append(el('span', 'lobby-said', w.text));
      if (i === 0) li.append(el('span', 'ov-small', t('host')));
      if (s.c?.[i]) li.append(el('span', 'lobby-star', '★'));
      ul.append(li);
    });
    for (let i = 0; i < free; i++) ul.append(el('li', 'open', t('open')));
    return ul;
  }

  /** the fastest other car against ours, under the players */
  private gauge(s: LobbyState) {
    if (!(s.vs! > 0)) return [];
    const d = el('div', 'lobby-speed');
    d.append(speedometer(s.vs!), el('span', '', t('Speed compared to your car')));
    return [d];
  }

  /** a card key's cap now: the keyboard's (none on a touch screen: a tap, or where the game has the key), or with a
   *  controller driving its D-pad direction held (arrow: runtime dinput_pad.c lobby_pad), none where it has none */
  private cap(which: keyof typeof KEYS, arrow?: string): string | null {
    if (touchFirst()) return null;
    if (drivingPad()) return arrow ? t('Hold {arrow}', { arrow }) : null;
    const k = this.keyOf(which);
    return k ? keyName(k) : null;
  }
  /** Enter's cap: the game's Engage, D-pad up on a controller (the game's own layout) */
  private enter() { return drivingPad() && !touchFirst() ? '↑' : 'Enter'; }
  /** one of the card's own actions, with its key (on a touch screen none: a tap) */
  private own(which: keyof typeof KEYS, what: string, f: () => void, arrow?: string) {
    return act(this.cap(which, arrow), what, f);
  }
  /** where E goes at a start of several lobbies (this one pg of nl): the next one by its number */
  private nextPage(s: LobbyState) {
    const nl = s.nl ?? 1;
    return t('Lobby {k} of {n}', { k: (s.pg ?? 1) % nl + 1, n: nl });
  }

  /** The card, in blocks a line apart, a few lines each in one size (the small one only for what qualifies them):
   *  the race; its members (and the speedometer); the host's settings; this lobby or the next: Enter, E, what is going
   *  on and how to leave */
  private drawPanel(s: LobbyState) {
    const p = this.panel, names = s.names ?? [], host = names[0] ?? t('A player'), o = this.offers(s);
    p.dataset.v = s.v ?? '';
    if (s.v === 'note') { p.replaceChildren(block(el('div', '', noteText(s.text ?? '')))); return; }
    const settings: HTMLElement[] = [], go: HTMLElement[] = [];
    const line = (cls: string, text: string) => go.push(el('div', cls, text));
    if (s.v === 'spot') go.push(act(this.enter(), t('Open a lobby')));
    else if (s.v === 'join') go.push(act(this.enter(), t('Join')));
    else if (s.v === 'lobby') {
      const mine = s.me === 0;
      // (every game of the lobby, the host's too, 2 s from the host's Enter: no numbers to count)
      if (s.st === 'count') line('ov-go', t('Starting the race') + '…');
      else if (s.st === 'asked') line('ov-dim', t('Joining {host}\'s {kind}…', { host, kind: game(s.kind ?? 'race') }));
      else {
        if (s.away) line('ov-warn', mine ? t('You seem away: the lobby closes in {n} s unless you press a key', { n: s.away })
          : t('{host} seems away: the lobby closes in {n} s', { host, n: s.away }));
        if (mine && names.length >= 2) go.push(act(this.enter(), t('Start the race')));
        else if (!s.away) line('ov-dim', !mine ? t('Waiting for {host} to start', { host })
          : s.closes !== undefined ? t('Closes in {n} s if nobody joins', { n: s.closes }) : t('Waiting for players'));
      }
      // the host's settings: who can join (V goes round its choices), invite friends; joined from afar: leave
      if (o.priv) {
        const who = s.who ?? (s.priv ? 'friends' : 'all');
        settings.push(this.own('priv', who === 'stage' ? t('Stage {n} and below can join', { n: s.upto ?? 1 })
          : who === 'friends' ? t('Friends & crew can join') : t('Everyone can join'), o.priv, '←'));
      }
      // (no invite on a controller's card: the menu's Friends has it)
      if (o.invite && (!mine || !drivingPad() || touchFirst())) settings.push(this.own('invite', mine ? t('Invite friends') : t('Leave'), o.invite, mine ? undefined : '←'));
    }
    if (o.fresh) go.push(this.own('fresh', t('New lobby'), o.fresh, '←'));
    if (o.next) go.push(this.own('next', this.nextPage(s), o.next, '→'));
    // (how to leave: drive away, but from a lobby joined from afar)
    if (s.v === 'lobby' && s.st !== 'count' && s.st !== 'asked') go.push(el('div', 'ov-small', s.me === 0 ? t('Drive away to call it off')
      : s.far ? t('You joined from afar: the race leaves from where you are') : t('Drive away to leave')));
    const blocks = [block(...this.head(s))];
    if (s.v !== 'spot') blocks.push(block(this.players(s), ...this.gauge(s)));
    if (settings.length) blocks.push(block(...settings));
    if (go.length) blocks.push(block(...go));
    p.replaceChildren(...blocks);
  }

  /** the race's results on the way back: place, name, time (or points), ours marked with its prize */
  private results(s: LobbyState) {
    const box = el('div', 'lobby-results');
    // (a URL race of heats: the last heat's results between heats, the series' standing after the last)
    const head = !s.heats || s.heats < 2 || !s.heat ? t('Results') : s.fin ? t('Final standings')
      : t('Heat {k} results', { k: Math.max(1, s.heat - 1) });
    box.append(el('div', 'lobby-head', head));
    const ol = el('ol', 'lobby-places');
    let place = 0;
    for (const r of s.res ?? []) {
      const li = el('li', r.me ? 'taken mine' : 'taken');
      li.append(el('span', 'lobby-n', r.f ? String(++place) : '–'), el('span', 'lobby-name', r.n));
      const what = !r.f ? t('DNF') : r.p !== undefined ? tn(r.p, '{n} point', '{n} points') : r.t !== undefined ? raceTime(r.t) : '';
      if (what) li.append(el('span', 'lobby-time', what));
      if (r.me && s.credit && s.credited !== -1) li.append(el('span', 'lobby-prize', `★ ${s.credit === 2 ? t('Friend\'s win') : t('Career win')}`));
      else if (r.me && s.prize) li.append(el('span', 'lobby-prize', `+$${s.prize}`));
      ol.append(li);
    }
    box.append(ol);
    return box;
  }

  private drawCover(s: LobbyState) {
    const card = el('div', 'lobby-card');
    // (on the way: the race and its players, no places open; back: its results)
    card.append(block(...this.head(s, true)), block(s.res?.length ? this.results(s) : this.players(s, false)));
    if (s.stand?.length && !s.fin) card.append(block(this.standings(s)));
    const step = el('div', 'lobby-step');
    step.append(el('span', 'lobby-spin'), el('span', '', `${s.step ? game(s.step) : t('Loading')}…`));
    card.append(block(step));
    this.cover.replaceChildren(card);
  }
}
