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
  /** the treasure hunts finished, the days in a row up to the last one, the most days in a row, the last one's day */
  hunts: number; huntStreak: number; bestHuntStreak: number; lastHuntDay: number;
  /** the quickest hunt (ms of free roam from the first diamond to the last; 0: none timed yet) */
  bestHuntMs: number;
}
export const emptyTotals = (): Totals => ({
  races: 0, wins: 0, streak: 0, bestStreak: 0, meters: 0, topKmh: 0, bestDrift: 0, closeWin: false, playersWins: 0,
  typesWon: [], tracks: [], wonOn: [], bothWays: false, carsWon: [], winsByCar: {},
  hunts: 0, huntStreak: 0, bestHuntStreak: 0, lastHuntDay: -1, bestHuntMs: 0,
});
/** a treasure hunt finished on day (the UTC day's number) into the totals (a new object; a day once only) */
export function addHunt(t0: Totals, day: number, ms = 0): Totals {
  if (t0.lastHuntDay === day) return t0;
  const s = structuredClone(t0);
  s.hunts++;
  s.huntStreak = s.lastHuntDay === day - 1 ? s.huntStreak + 1 : 1;
  s.bestHuntStreak = Math.max(s.bestHuntStreak, s.huntStreak);
  s.lastHuntDay = day;
  if (ms > 0 && (!s.bestHuntMs || ms < s.bestHuntMs)) s.bestHuntMs = ms;
  return s;
}

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
  { id: 'hunts', name: N('Treasure hunter'), levels: [1, 10, 50], have: t => t.hunts,
    what: x => tn(x, 'Find all of a day\'s diamonds in free roam.', 'Find all of a day\'s diamonds in free roam on {n} days.', n(x)) },
  { id: 'hunt_streak', name: N('Daily driver'), levels: [3, 7, 30], have: t => t.bestHuntStreak,
    what: x => tn(x, 'Find all of the diamonds {n} day in a row.', 'Find all of the diamonds {n} days in a row.', n(x)) },
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
/** a hunt's time, m:ss (h:mm:ss from an hour) */
export function huntClock(ms: number): string {
  const s = Math.max(0, Math.round(ms / 1000)), h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, x = String(s % 60).padStart(2, '0');
  return h ? `${h}:${String(m).padStart(2, '0')}:${x}` : `${m}:${x}`;
}
/** the UTC day's number (days since 1970-01-01): the treasure hunt's day */
export const utcDay = (ms = Date.now()) => Math.floor(ms / 86_400_000);
export const bitCount = (x: number) => { let n = 0; for (; x; x &= x - 1) n++; return n; };
/** every level of every achievement */
export const LEVELS = ACHIEVEMENTS.reduce((s, a) => s + a.levels.length, 0);

/** achievements.json */
/** hunt: the day's treasure hunt so far (the diamonds found, a bit each; its time, ms) */
export interface Kept { v: 1; totals: Totals; got: Record<string, number>; hunt?: { day: number; found: number; ms?: number } }
export function parse(text: string | null): Kept {
  try {
    const k = JSON.parse(text ?? '') as Partial<Kept>;
    if (k && k.v === 1 && k.totals && k.got)
      return { v: 1, totals: { ...emptyTotals(), ...k.totals }, got: { ...k.got }, ...(k.hunt ? { hunt: { ...k.hunt } } : {}) };
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
/** the levels the totals reach that got does not have, at `when`: got with them, and them in order */
function reach(totals: Totals, got0: Record<string, number>, when: number) {
  const got = { ...got0 }, now: { a: Achievement; level: number }[] = [];
  for (const a of ACHIEVEMENTS) {
    const have = a.have(totals);
    a.levels.forEach((need, i) => {
      const key = levelKey(a, i + 1);
      if (!got[key] && have >= need) { got[key] = when; now.push({ a, level: i + 1 }); }
    });
  }
  return { got, now };
}
/** the result in: the new totals, and the levels it reached (at `when`), each achievement's in order */
export function earn(k: Kept, r: RaceResult, when: number): { kept: Kept; got: { a: Achievement; level: number }[] } {
  const totals = add(k.totals, r), { got, now } = reach(totals, k.got, when);
  return { kept: { ...k, totals, got }, got: now };
}
/** a day's treasure hunt finished: the new totals (once a day), and the levels it reached */
export function earnHunt(k: Kept, day: number, when: number, ms = 0): { kept: Kept; got: { a: Achievement; level: number }[] } {
  const totals = addHunt(k.totals, day, ms), { got, now } = reach(totals, k.got, when);
  return { kept: { ...k, totals, got }, got: now };
}

export interface AchievementsDeps {
  read: () => Promise<string | null>;
  write: (text: string) => Promise<void>;
  /** the card over the game (#ach-pop) */
  pop: HTMLElement;
  log: (line: string) => void;
}

/** a card over the game: a level reached, a diamond found, a hunt finished */
interface Card {
  icon: 'trophy' | 'diamond'; label: string; name: string; what: string; chips?: { texts: string[]; reached: number };
  /** a small one: one line under the hunt's pill, a short while (a diamond found) */
  small?: boolean;
  /** ...in gold (the golden diamond) */
  gold?: boolean;
}
const POP_MS = 4800, POP_SMALL_MS = 2500, POP_GAP_MS = 350;
const ICONS = {
  trophy: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M7 4h10v5a5 5 0 0 1-10 0V4z"/>' +
    '<path d="M7 6H4v1a3 3 0 0 0 3 3M17 6h3v1a3 3 0 0 1-3 3M12 14v3M8.5 20h7M10 17h4"/></svg>',
  diamond: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M7 4h10l4 5-9 11L3 9z"/><path d="M3 9h18M9.5 4 8 9l4 11 4-11-1.5-5"/></svg>',
};
const levelCard = (a: Achievement, level: number): Card => ({
  icon: 'trophy', label: t('Achievement unlocked'), name: levelName(a, level), what: a.what(a.levels[level - 1]),
  ...(a.levels.length > 1 ? { chips: { texts: a.levels.map(compact), reached: level } } : {}),
});

export class Achievements {
  private kept: Kept = parse(null);
  private session: GameSession | null = null;
  private timer: ReturnType<typeof setInterval> | undefined;
  private seen = 0;
  private last = '';
  private queue: Card[] = [];
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
    this.take(r);
  }

  /** the file again, f's change to it, the cards for the levels it reached, the file written */
  private change(f: (k: Kept) => { kept: Kept; got: { a: Achievement; level: number }[] }, before: Card[] = []) {
    this.busy = this.busy.then(async () => {
      // the file as it is now: another device's (the cloud saves) may have come since
      this.kept = parse(await this.deps.read().catch(() => JSON.stringify(this.kept)));
      const { kept, got } = f(this.kept);
      this.kept = kept;
      this.queue.push(...before);
      for (const { a, level } of got) { this.queue.push(levelCard(a, level)); this.deps.log(`[app] achievement: ${levelKey(a, level)}`); }
      this.next();
      await this.deps.write(JSON.stringify(kept)).catch(e => this.deps.log(`[app] achievements: not written (${e})`));
      this.render();
    });
  }
  private take(r: RaceResult) { this.change(k => earn(k, r, Date.now())); }

  /** the treasure hunt (treasure.ts): day's diamonds found so far, as the file has them (0: none, or another day) */
  huntFound(day: number): number { return this.kept.hunt?.day === day ? this.kept.hunt.found : 0; }
  /** ...the day's hunt's time so far (ms) */
  huntMs(day: number): number { return this.kept.hunt?.day === day ? this.kept.hunt.ms ?? 0 : 0; }
  /** ...a diamond found (the golden one: what it paid, $): its card and the day's progress kept */
  diamond(day: number, found: number, total: number, paid: number, ms: number, golden = false) {
    const n = bitCount(found);
    this.change(k => ({ kept: { ...k, hunt: { day, found, ms } }, got: [] }),
      [{ icon: 'diamond', label: '', name: golden ? t('Golden diamond') : t('Diamond {n} of {total}', { n: count(n), total: count(total) }),
         what: paid > 0 ? `+$${count(paid)}` : '', small: true, ...(golden ? { gold: true } : {}) }]);
  }
  /** ...all of them found: the card (with the prize the game paid into the career's bank, $, and the hunt's time), the
   *  hunt in the totals; the last one golden: what it paid on its own card first */
  huntDone(day: number, found: number, prize: number, ms: number, goldPaid = 0) {
    const time = ms > 0 ? t('All 15 in {time}.', { time: huntClock(ms) }) + ' ' : '';
    this.change(k => earnHunt({ ...k, hunt: { day, found, ms } }, day, Date.now(), ms),
      [...(goldPaid > 0 ? [{ icon: 'diamond' as const, label: '', name: t('Golden diamond'), what: `+$${count(goldPaid)}`, small: true, gold: true }] : []),
       { icon: 'diamond', label: t('Treasure hunt'), name: t('Hunt complete'),
         what: time + (prize > 0 ? t('${n} into your career\'s bank.', { n: count(prize) }) : t('All of today\'s diamonds found.')) }]);
  }
  /** ...today's progress for the tab */
  private huntLine(): string {
    const best = this.kept.totals.bestHuntMs;
    return t('Today\'s treasure hunt: {n} of {total} diamonds.', { n: count(bitCount(this.huntFound(utcDay()))), total: count(15) }) +
      (best > 0 ? ' ' + t('Your quickest: {time}.', { time: huntClock(best) }) : '');
  }

  /** the next card over the game: its icon, label, name and line, the levels (the new one popping in); one after
   *  another, each POP_MS */
  private next() {
    if (this.popping) return;
    const c = this.queue.shift(), el = this.deps.pop;
    if (!c) return;
    this.popping = true;
    el.replaceChildren();
    el.insertAdjacentHTML('beforeend', ICONS[c.icon]);
    const body = document.createElement('div'), label = document.createElement('span'), name = document.createElement('strong'),
      what = document.createElement('span');
    body.className = 'ach-pop-body'; label.className = 'ach-pop-label'; name.className = 'ach-pop-name'; what.className = 'ach-pop-what';
    label.textContent = c.label; name.textContent = c.name; what.textContent = c.what;
    if (c.label) body.append(label);
    body.append(name);
    if (c.what) body.append(what);
    el.classList.toggle('small', !!c.small);
    el.classList.toggle('gold', !!c.gold);
    if (c.chips) {
      const chips = document.createElement('ol');
      chips.className = 'ach-levels';
      c.chips.texts.forEach((x, i) => {
        const li = document.createElement('li');
        li.className = i < c.chips!.reached - 1 ? 'reached' : i === c.chips!.reached - 1 ? 'reached new' : '';
        li.textContent = x;
        chips.append(li);
      });
      body.append(chips);
    }
    el.append(body);
    el.hidden = false;
    el.classList.remove('out'); void el.offsetWidth; el.classList.add('in');         // (the animation again for the next)
    setTimeout(() => {
      el.classList.replace('in', 'out');
      setTimeout(() => { el.hidden = true; el.classList.remove('out'); this.popping = false; this.next(); }, POP_GAP_MS);
    }, c.small ? POP_SMALL_MS : POP_MS);
  }

  /** the tab: how many levels, then each achievement with the level reached, what the next asks and how far (the
   *  last reached: its date) */
  render() {
    const { got, totals } = this.kept;
    const all = ACHIEVEMENTS.reduce((s, a) => s + reached(a, got), 0);
    const hunt = this.root.querySelector('[data-ach-hunt]');
    if (hunt) live(hunt, () => this.huntLine());
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
