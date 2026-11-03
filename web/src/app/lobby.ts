// Free roam's race lobby over the game (roam_game.c mp_panel, NET_KIND_LOBBY): at a race's start what Enter does
// there, a lobby's six places with its players and its countdown; during the trip to the race and back a cover over
// the game's menus (the race and its results stay the game's own; the way back shows the results again).  The game
// sends its state as JSON when it changes, "{}" for nothing to show.

import { language, N, t, tn } from './i18n';

/** what the game sends (roam_game.c mp_panel) */
interface LobbyState {
  /** spot: a multiplayer-only spot, Enter opens a lobby; join: a lobby here to join; full: the lobby here is full,
   *  Enter opens another (or one we are kept out of: only, Enter opens none); lobby: ours, or the one we are in; trip:
   *  the way to the race and back; note: a message */
  v?: 'spot' | 'join' | 'full' | 'lobby' | 'trip' | 'note';
  /** lobby: host (ours), asked (joining), in (joined), count (the countdown, t seconds) */
  st?: 'host' | 'asked' | 'in' | 'count';
  /** the race; stage: its career stage */
  kind?: string; track?: string; laps?: number; stage?: number;
  /** the lobby's players, the host first; me: our place in it (-1: none); max: its places; t: the countdown's
   *  seconds; closes: the seconds left of a lobby nobody joined (its last minute); away: its host made no input
   *  lately, the seconds until it closes; fee: what quitting the race costs; even: the race evens out the cars
   *  (their engines); outpaced: our car's class is one the race's stage's cars usually leave far behind (runtime
   *  mp_outpaced) */
  names?: string[]; me?: number; max?: number; t?: number; closes?: number; away?: number; fee?: number; even?: number;
  outpaced?: number;
  why?: string; step?: string; text?: string;
  /** counts: 1 a win counts in our career (a race we could win with Solo here); c: each player's own (1 counts for
   *  them); fr: each player our friend (their win counts for us too); cr: each player a crew-mate, not a friend (the same) */
  counts?: number; c?: number[]; fr?: number[]; cr?: number[];
  /** a career lobby we are kept out of (runtime mp_kept_out): it takes players up to this stage and its host's friends
   *  and crew-mates */
  only?: number;
  /** trip, once the race's results came: their rows in order; prize: what ours won; credit: the race counts in our
   *  career, 1 we won it, 2 a friend or crew-mate did; credited -1: the career did not take it (the prize money instead) */
  res?: RaceResult[]; prize?: number; credit?: number; credited?: number;
  /** a URL race of heats: how many (the race then shows no track: each heat has its own); on the trip heat: the one
   *  raced now or last (1 ..) and track its track, fin: the last heat's results are in (res is the series' standing,
   *  each row's points), stand: the points after the heats raced so far, the leader first */
  heats?: number; heat?: number; fin?: number; stand?: Standing[];
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
    const sub = [s.stage ? t('Stage {n}', { n: s.stage }) : '', this.heats(s), s.track, s.laps ? tn(s.laps, '{n} lap', '{n} laps') : ''].filter(Boolean).join(' · ');
    return sub ? [head, el('div', 'lobby-sub', sub)] : [head];
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
      if (r.me) li.append(el('span', 'lobby-tag', t('you')));
      li.append(el('span', 'lobby-time', tn(r.p, '{n} point', '{n} points')));
      ol.append(li);
    });
    box.append(ol);
    return box;
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
      if (i < names.length && s.fr?.[i] && i !== s.me) li.append(el('span', 'lobby-tag', t('friend')));
      else if (i < names.length && s.cr?.[i] && i !== s.me) li.append(el('span', 'lobby-tag', t('crew')));
      if (i < names.length && (i === 0 || i === s.me)) li.append(el('span', 'lobby-tag', i === 0 ? t('host') : t('you')));
      if (i < names.length && s.c?.[i]) {
        const star = el('span', 'lobby-star', '★');
        star.title = i === s.me ? t('A win counts in your career') : t('A win counts in their career');
        li.append(star);
      }
      const w = i < names.length ? this.said.get(names[i]) : undefined;
      if (w) li.append(el('span', 'lobby-said', w.text));
      ol.append(li);
    }
    return ol;
  }

  /** the race counts in our career: what a win does (and a friend's or crew-mate's, with such players in it) */
  private career(s: LobbyState) {
    if (!s.counts) return [];
    const friends = (s.fr ?? []).some(Boolean), crew = (s.cr ?? []).some(Boolean);
    const d = el('div', 'lobby-why');
    d.append(el('span', 'lobby-star', '★ '), friends && crew ? t('A win counts in your career, and so does a friend\'s or crew-mate\'s')
      : friends ? t('A win counts in your career, and so does a friend\'s')
      : crew ? t('A win counts in your career, and so does a crew-mate\'s') : t('A win counts in your career'));
    return [d];
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
        kids.push(...this.title(s), ...(s.counts ? this.career(s)
          : [el('div', 'lobby-why', t('Multiplayer only here: {why}', { why: game(s.why ?? 'not open in your career') }))]),
          this.act('Enter', t('Open a lobby')));
        break;
      case 'join':
        kids.push(...this.title(s, host), this.places(s), ...this.career(s));
        // (a warning only: the player may join anyway)
        if (s.outpaced && s.stage) kids.push(el('div', 'lobby-warn', t('Stage {n} race: the cars here are usually much faster than yours', { n: s.stage })));
        kids.push(this.act('Enter', t('Join')));
        break;
      case 'full':
        // (kept out: no other lobby opens beside it, one race at a start)
        kids.push(...this.title(s, host), this.places(s), el('div', 'lobby-why', s.only
          ? t('A career race for players up to stage {n} and {host}\'s friends and crew.', { n: s.only, host })
          : t('This lobby is full.')));
        if (!s.only) kids.push(this.act('Enter', t('Open another')));
        break;
      case 'lobby': {
        const mine = s.me === 0;
        kids.push(...this.title(s, mine ? undefined : host), this.places(s, s.st === 'asked'), ...this.career(s));
        // (every game of the lobby, the host's too, 2 s from the host's Enter: no numbers to count)
        if (s.st === 'count') kids.push(el('div', 'lobby-count', t('Starting the race') + '…'));
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
    // (what the evening out does, not that it makes the cars even: under the outpaced warning, none)
    if (s.v === 'join' && s.even && !s.outpaced) kids.push(el('div', 'lobby-hint', t('Engine power is evened out: slower cars get more, faster ones a little less')));
    if (s.v === 'join' && s.fee) kids.push(el('div', 'lobby-hint', t('Quitting the race costs ${fee}', { fee: s.fee })));
    p.replaceChildren(...kids);
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
      if (r.me) li.append(el('span', 'lobby-tag', t('you')));
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
    card.append(...this.title(s, undefined), s.res?.length ? this.results(s) : this.places(s));
    if (s.stand?.length && !s.fin) card.append(this.standings(s));
    const step = el('div', 'lobby-step');
    step.append(el('span', 'lobby-spin'), el('span', '', `${s.step ? game(s.step) : t('Loading')}…`));
    card.append(step);
    this.cover.replaceChildren(card);
  }
}
