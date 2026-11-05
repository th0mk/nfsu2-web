// Achievements: earned in the races the player finishes, kept with the saves (achievements.json in the save folder, so
// a backup and the cloud saves carry them) and listed in the start page's Achievements tab.
//
// runtime/raceresult.c publishes each race's result in four status slots once its results screen is up (rr_n counts
// them: a new value is a new race; rr_race, rr_score and rr_more hold it, packed as there).  While a game runs this
// reads them twice a second, folds each new result into the totals (Totals) and unlocks what they reach, with a toast
// over the game.  Nothing goes to the server: the totals are the player's own, from their own races.
import type { GameSession } from './game';
import { count, dateLocale, language, live, LOCALE, N, onLanguage, t, tn, type Msg } from './i18n';

export const FILE = 'achievements.json';

/** a race's result as runtime/raceresult.c packs it */
export interface RaceResult {
  track: number; reversed: boolean; type: number; laps: number; place: number; finished: boolean; mode: number;
  car: number; racers: number; score: number; topKmh: number; margin: number; meters: number;
}
/** the types (raceresult.c RR_*) */
export const CIRCUIT = 0, SPRINT = 1, DRAG = 2, DRIFT = 3, STREETX = 4, URL = 5, KNOCKOUT = 6;
/** the modes */
export const QUICK = 0, CAREER = 1, PLAYERS = 2;

const bits = (v: number, at: number, n: number) => Math.floor(v / 2 ** at) % 2 ** n;
export function unpack(race: number, score: number, more: number): RaceResult {
  return {
    track: bits(race, 0, 16), reversed: bits(race, 16, 1) === 1, type: bits(race, 17, 3), laps: bits(race, 20, 5),
    place: bits(race, 25, 3), finished: bits(race, 28, 1) === 1, mode: bits(race, 29, 2), car: bits(race, 31, 7),
    racers: bits(race, 38, 3), score, topKmh: bits(more, 0, 12) / 10, margin: bits(more, 12, 16), meters: bits(more, 28, 20),
  };
}

/** what the results add up to (achievements.json's "totals") */
export interface Totals {
  races: number; wins: number; streak: number; bestStreak: number; meters: number; topKmh: number; bestDrift: number;
  closeWin: boolean; playersWins: number;
  typesWon: number[]; tracks: string[]; wonOn: string[]; bothWays: boolean; carsWon: number[];
  winsByCar: Record<string, number>;
}
export const emptyTotals = (): Totals => ({
  races: 0, wins: 0, streak: 0, bestStreak: 0, meters: 0, topKmh: 0, bestDrift: 0, closeWin: false, playersWins: 0,
  typesWon: [], tracks: [], wonOn: [], bothWays: false, carsWon: [], winsByCar: {},
});

/** a result into the totals (a new object) */
export function add(t0: Totals, r: RaceResult): Totals {
  const s = structuredClone(t0), won = r.place === 1;
  s.races++;
  s.meters += r.meters;
  s.topKmh = Math.max(s.topKmh, r.topKmh);
  if (r.type === DRIFT && r.finished) s.bestDrift = Math.max(s.bestDrift, r.score);
  const key = `${r.track}${r.reversed ? 'r' : ''}`, other = `${r.track}${r.reversed ? '' : 'r'}`;
  if (r.finished && !s.tracks.includes(key)) s.tracks.push(key);
  if (!won) { s.streak = 0; return s; }
  s.wins++;
  s.streak++; s.bestStreak = Math.max(s.bestStreak, s.streak);
  if (!s.typesWon.includes(r.type)) s.typesWon.push(r.type);
  // a win by less than a tenth of a second over the next car home (a drift's margin is points)
  if (r.type !== DRIFT && r.racers > 1 && r.margin > 0 && r.margin < 10) s.closeWin = true;
  if (r.mode === PLAYERS) s.playersWins++;
  if (r.car < 127) {
    if (!s.carsWon.includes(r.car)) s.carsWon.push(r.car);
    s.winsByCar[r.car] = (s.winsByCar[r.car] ?? 0) + 1;
  }
  if (!s.wonOn.includes(key)) s.wonOn.push(key);
  if (s.wonOn.includes(other)) s.bothWays = true;
  return s;
}

/** each achievement: its name, its levels (what each needs, in order; one level: no level numbers shown), what a
 *  level asks in the page's language and how far the totals are (a level is reached when have reaches its need) */
export interface Achievement {
  id: string; name: string; levels: number[]; what: (need: number) => string; have: (t: Totals) => number;
}
const ALL_TYPES = [CIRCUIT, SPRINT, DRAG, DRIFT, STREETX, URL];
const n = (x: number) => ({ n: count(x) });                   // (the count in the page's way, 250.000; tn picks by x)
export const ACHIEVEMENTS: Achievement[] = [
  { id: 'wins', name: N('Winner'), levels: [1, 10, 50, 100, 250], have: t => t.wins,
    what: x => tn(x, 'Win a race.', 'Win {n} races.', n(x)) },
  { id: 'streak', name: N('Winning streak'), levels: [3, 5, 10], have: t => t.bestStreak,
    what: x => tn(x, 'Win {n} race in a row.', 'Win {n} races in a row.', n(x)) },
  { id: 'all_types', name: N('All-rounder'), levels: [ALL_TYPES.length], have: t => ALL_TYPES.filter(x => t.typesWon.includes(x)).length,
    what: () => t('Win a circuit, a sprint, a drag, a drift, a Street X and a URL race.') },
  { id: 'both_ways', name: N('Both ways'), levels: [1], have: t => +t.bothWays, what: () => t('Win on the same track in both directions.') },
  { id: 'photo_finish', name: N('Photo finish'), levels: [1], have: t => +t.closeWin,
    what: () => t('Win a race by less than a tenth of a second.') },
  { id: 'drift', name: N('Sideways'), levels: [50000, 100000, 250000, 500000], have: t => t.bestDrift,
    what: x => tn(x, 'Score {n} point in a drift.', 'Score {n} points in a drift.', n(x)) },
  { id: 'speed', name: N('Speed demon'), levels: [250, 300, 350], have: t => Math.floor(t.topKmh),
    what: x => t('Reach {n} km/h in a race.', n(x)) },
  { id: 'km', name: N('Road trip'), levels: [100, 500, 1000, 5000], have: t => Math.floor(t.meters / 1000),
    what: x => t('Race {n} km in all.', n(x)) },
  { id: 'tracks', name: N('Tourist'), levels: [10, 25, 50], have: t => t.tracks.length,
    what: x => tn(x, 'Finish races on {n} different track (each direction counts).', 'Finish races on {n} different tracks (each direction counts).', n(x)) },
  { id: 'cars', name: N('Collector'), levels: [5, 10, 20], have: t => t.carsWon.length,
    what: x => tn(x, 'Win with {n} different car.', 'Win with {n} different cars.', n(x)) },
  { id: 'loyal', name: N('Loyal'), levels: [10, 25, 50], have: t => Math.max(0, ...Object.values(t.winsByCar)),
    what: x => tn(x, 'Win {n} race with the same car.', 'Win {n} races with the same car.', n(x)) },
  { id: 'players', name: N('Bragging rights'), levels: [1, 10, 50], have: t => t.playersWins,
    what: x => tn(x, 'Win a race against other players.', 'Win {n} races against other players.', n(x)) },
];
/** a level's key in achievements.json's "got" ("wins:2": Winner's second level) */
export const levelKey = (a: Achievement, level: number) => `${a.id}:${level}`;
/** the levels reached (0 none) */
export const reached = (a: Achievement, got: Record<string, number>) => a.levels.filter((_, i) => got[levelKey(a, i + 1)]).length;
/** an achievement's name with its level, as a toast says it ("Sideways II"; one level: its name) */
const ROMAN = ['I', 'II', 'III', 'IV', 'V', 'VI', 'VII', 'VIII'];
export const levelName = (a: Achievement, level: number) => a.levels.length > 1 ? `${t(a.name)} ${ROMAN[level - 1]}` : t(a.name);
/** a level's need, short, in the page's language (50K, 1,5 Tsd.) */
const compact = (x: number) =>
  new Intl.NumberFormat(language() === 'english' ? 'en-GB' : LOCALE[language()], { notation: 'compact' }).format(x);
/** every level of every achievement */
export const LEVELS = ACHIEVEMENTS.reduce((s, a) => s + a.levels.length, 0);

/** achievements.json */
export interface Kept { v: 1; totals: Totals; got: Record<string, number> }
export function parse(text: string | null): Kept {
  try {
    const k = JSON.parse(text ?? '') as Partial<Kept>;
    if (k && k.v === 1 && k.totals && k.got) return { v: 1, totals: { ...emptyTotals(), ...k.totals }, got: { ...k.got } };
  } catch { /* none, or not ours: start again */ }
  return { v: 1, totals: emptyTotals(), got: {} };
}
/** the levels the totals reach that the file does not have yet (an earlier list's file, a level added since), at
 *  `when`: a new object, or k itself when there are none */
export function settle(k: Kept, when: number): Kept {
  let got: Record<string, number> | null = null;
  for (const a of ACHIEVEMENTS) {
    const have = a.have(k.totals);
    a.levels.forEach((need, i) => { if (!k.got[levelKey(a, i + 1)] && have >= need) (got ??= { ...k.got })[levelKey(a, i + 1)] = when; });
  }
  return got ? { ...k, got } : k;
}
/** the result in: the new totals, and the levels it reached (at `when`), each achievement's in order */
export function earn(k: Kept, r: RaceResult, when: number): { kept: Kept; got: { a: Achievement; level: number }[] } {
  const totals = add(k.totals, r), got = { ...k.got }, now: { a: Achievement; level: number }[] = [];
  for (const a of ACHIEVEMENTS) {
    const have = a.have(totals);
    a.levels.forEach((need, i) => {
      const key = levelKey(a, i + 1);
      if (!got[key] && have >= need) { got[key] = when; now.push({ a, level: i + 1 }); }
    });
  }
  return { kept: { v: 1, totals, got }, got: now };
}

export interface AchievementsDeps {
  read: () => Promise<string | null>;
  write: (text: string) => Promise<void>;
  /** the card over the game (#ach-pop) */
  pop: HTMLElement;
  log: (line: string) => void;
}

const POP_MS = 4800, POP_GAP_MS = 350;
const TROPHY = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M7 4h10v5a5 5 0 0 1-10 0V4z"/>' +
  '<path d="M7 6H4v1a3 3 0 0 0 3 3M17 6h3v1a3 3 0 0 1-3 3M12 14v3M8.5 20h7M10 17h4"/></svg>';

export class Achievements {
  private kept: Kept = parse(null);
  private session: GameSession | null = null;
  private timer: ReturnType<typeof setInterval> | undefined;
  private seen = 0;
  private last = '';
  private queue: { a: Achievement; level: number }[] = [];
  private popping = false;
  private busy: Promise<void> = Promise.resolve();

  constructor(private root: HTMLElement, private deps: AchievementsDeps) {
    onLanguage(() => this.render());           // (the levels' short numbers and their tips too)
  }

  /** achievements.json again (a restore, the cloud saves), and the list */
  async load() {
    const read = parse(await this.deps.read().catch(() => null));
    this.kept = settle(read, Date.now());
    if (this.kept !== read) await this.deps.write(JSON.stringify(this.kept)).catch(() => {});
    this.render();
  }

  /** a game started: its results until stop() */
  start(s: GameSession) {
    this.session = s; this.seen = 0; this.last = '';
    clearInterval(this.timer);
    this.timer = setInterval(() => this.poll(), 500);
  }
  stop() {
    clearInterval(this.timer); this.timer = undefined;
    this.session = null;
  }

  private poll() {
    const st = this.session?.status?.read();
    if (!st) return;
    const n = st.get('rr_n') ?? 0;
    if (n <= this.seen) return;
    this.seen = n;
    // (the same result again is the same race: a race's time to the millisecond and its way driven do not repeat)
    const raw = `${st.get('rr_race')} ${st.get('rr_score')} ${st.get('rr_more')}`;
    if (raw === this.last) return;
    this.last = raw;
    const r = unpack(st.get('rr_race') ?? 0, st.get('rr_score') ?? 0, st.get('rr_more') ?? 0);
    this.busy = this.busy.then(() => this.take(r));
  }

  private async take(r: RaceResult) {
    // the file as it is now: another device's (the cloud saves) may have come since
    this.kept = parse(await this.deps.read().catch(() => JSON.stringify(this.kept)));
    const { kept, got } = earn(this.kept, r, Date.now());
    this.kept = kept;
    for (const { a, level } of got) {
      this.queue.push({ a, level });
      this.deps.log(`[app] achievement: ${levelKey(a, level)}`);
    }
    this.next();
    await this.deps.write(JSON.stringify(kept)).catch(e => this.deps.log(`[app] achievements: not written (${e})`));
    this.render();
  }

  /** the next level reached onto the card over the game: its name and level, what it took, its levels (the new one
   *  popping in); one after another, each POP_MS */
  private next() {
    if (this.popping) return;
    const n = this.queue.shift(), el = this.deps.pop;
    if (!n) return;
    this.popping = true;
    const { a, level } = n, need = a.levels[level - 1];
    el.replaceChildren();
    el.insertAdjacentHTML('beforeend', TROPHY);
    const body = document.createElement('div'), label = document.createElement('span'), name = document.createElement('strong'),
      what = document.createElement('span');
    body.className = 'ach-pop-body'; label.className = 'ach-pop-label'; name.className = 'ach-pop-name'; what.className = 'ach-pop-what';
    label.textContent = t('Achievement unlocked'); name.textContent = levelName(a, level); what.textContent = a.what(need);
    body.append(label, name, what);
    if (a.levels.length > 1) {
      const chips = document.createElement('ol');
      chips.className = 'ach-levels';
      a.levels.forEach((x, i) => {
        const c = document.createElement('li');
        c.className = i < level - 1 ? 'reached' : i === level - 1 ? 'reached new' : '';
        c.textContent = compact(x);
        chips.append(c);
      });
      body.append(chips);
    }
    el.append(body);
    el.hidden = false;
    el.classList.remove('out'); void el.offsetWidth; el.classList.add('in');         // (the animation again for the next)
    setTimeout(() => {
      el.classList.replace('in', 'out');
      setTimeout(() => { el.hidden = true; el.classList.remove('out'); this.popping = false; this.next(); }, POP_GAP_MS);
    }, POP_MS);
  }

  /** the tab: how many levels, then each achievement with the level reached, what the next asks and how far (the
   *  last reached: its date) */
  render() {
    const { got, totals } = this.kept;
    const all = ACHIEVEMENTS.reduce((s, a) => s + reached(a, got), 0);
    const head = this.root.querySelector('[data-ach-count]');
    if (head) live(head, (() => t('{n} of {total} achievements', { n: count(all), total: count(LEVELS) })) as Msg);
    const list = this.root.querySelector('[data-ach-list]');
    if (!list) return;
    list.replaceChildren(...ACHIEVEMENTS.map(a => {
      const li = document.createElement('li'), level = reached(a, got), next = a.levels[level];
      li.className = level ? (next === undefined ? 'ach got done' : 'ach got') : 'ach';
      const name = document.createElement('strong'), what = document.createElement('span'), state = document.createElement('span');
      live(name, () => level ? levelName(a, level) : t(a.name));
      live(what, () => a.what(next ?? a.levels[a.levels.length - 1]));
      state.className = 'ach-state';
      const when = got[levelKey(a, level)];
      live(state, () => next === undefined ? new Date(when).toLocaleDateString(dateLocale())
        : `${count(Math.min(Math.floor(a.have(totals)), next))} / ${count(next)}`);
      li.append(name, what, state);
      if (next !== undefined && next > 1) {
        const bar = document.createElement('progress');
        bar.className = 'ach-bar'; bar.max = next; bar.value = Math.min(a.have(totals), next);
        li.append(bar);
      }
      if (a.levels.length > 1) {
        const chips = document.createElement('ol');
        chips.className = 'ach-levels';
        a.levels.forEach((need, i) => {
          const c = document.createElement('li');
          c.className = i < level ? 'reached' : i === level ? 'next' : '';
          c.textContent = compact(need);
          c.title = `${levelName(a, i + 1)}: ${a.what(need)}`;
          chips.append(c);
        });
        li.append(chips);
      }
      return li;
    }));
  }
}
