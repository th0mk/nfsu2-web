// The races' achievements (web/src/app/achievements.ts; runtime/raceresult.c publishes each result):
//   node --test web/test/achievements.test.mjs
// - a result unpacks as raceresult.c packs it (its shifts are the ones in the C)
// - the totals: wins, streaks, the types won, tracks each way, cars, the best drift, the top speed, the way driven
// - what each achievement needs unlocks it once, at the result that reaches it; a broken file starts again
// The module is bundled from web/src with esbuild, as web/test/flags.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const js = (await esbuild.build({ stdin: { contents: `export * from './web/src/app/achievements.ts';`, resolveDir: ROOT, loader: 'ts' },
  bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const A = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

/** raceresult.c's rr_race and rr_more */
const pack = r => ({
  race: r.track + (r.reversed ? 1 : 0) * 2 ** 16 + r.type * 2 ** 17 + r.laps * 2 ** 20 + r.place * 2 ** 25 +
    (r.place > 0 ? 1 : 0) * 2 ** 28 + (r.mode ?? 0) * 2 ** 29 + (r.car ?? 127) * 2 ** 31 + (r.racers ?? 4) * 2 ** 38,
  more: Math.round((r.top ?? 0) * 10) + (r.margin ?? 0) * 2 ** 12 + (r.meters ?? 0) * 2 ** 28,
});
const result = r => { const p = pack(r); return A.unpack(p.race, r.score ?? 60000, p.more); };
const race = o => ({ track: 4001, reversed: false, type: A.CIRCUIT, laps: 2, place: 1, car: 5, racers: 4, margin: 150, meters: 5000, ...o });

test('a result unpacks as raceresult.c packs it', () => {
  const c = fs.readFileSync(path.join(ROOT, 'runtime/raceresult.c'), 'utf8');
  for (const s of ['rev << 16', 'type << 17', 'laps << 20', '7) << 25', 'fin << 28', 'mode << 29', 'cartype << 31',
    '7) << 38', '65535) << 12', '1u) << 28', '"rr_n"', '"rr_race"', '"rr_score"', '"rr_more"']) assert.ok(c.includes(s), s);
  const r = result({ track: 4305, reversed: true, type: A.DRIFT, laps: 3, place: 2, mode: A.CAREER, car: 41, racers: 6,
    top: 287.4, margin: 1234, meters: 98765, score: 43210 });
  assert.deepEqual(r, { track: 4305, reversed: true, type: A.DRIFT, laps: 3, place: 2, finished: true, mode: A.CAREER, car: 41,
    racers: 6, score: 43210, topKmh: 287.4, margin: 1234, meters: 98765 });
  assert.equal(result(race({ place: 0 })).finished, false);
  assert.equal(result(race({ car: 127 })).car, 127);
});

test('the totals', () => {
  let t = A.emptyTotals();
  t = A.add(t, result(race({})));
  t = A.add(t, result(race({ type: A.DRAG, track: 4002 })));
  t = A.add(t, result(race({ place: 3, top: 250 })));
  assert.equal(t.races, 3); assert.equal(t.wins, 2); assert.equal(t.streak, 0); assert.equal(t.bestStreak, 2);
  assert.deepEqual(t.typesWon.sort(), [A.CIRCUIT, A.DRAG]); assert.equal(t.topKmh, 250); assert.equal(t.meters, 15000);
  assert.deepEqual(t.tracks.sort(), ['4001', '4002']); assert.equal(t.bothWays, false);
  t = A.add(t, result(race({ reversed: true })));
  assert.equal(t.bothWays, true, 'the same track won both ways');
  t = A.add(A.emptyTotals(), result(race({ place: 4, reversed: true })));
  t = A.add(t, result(race({})));
  assert.equal(t.bothWays, false, 'a race finished, not won, the other way does not count');
  t = A.add(A.emptyTotals(), result(race({ type: A.DRIFT, score: 61000 })));
  t = A.add(t, result(race({ type: A.DRIFT, score: 52000, place: 2 })));
  assert.equal(t.bestDrift, 61000);
  assert.equal(A.add(A.emptyTotals(), result(race({ margin: 9 }))).closeWin, true);
  assert.equal(A.add(A.emptyTotals(), result(race({ margin: 9, racers: 1 }))).closeWin, false, 'alone: no photo finish');
  assert.equal(A.add(A.emptyTotals(), result(race({ margin: 9, type: A.DRIFT }))).closeWin, false, 'a drift margin is points');
  assert.equal(A.add(A.emptyTotals(), result(race({ mode: A.PLAYERS }))).playersWins, 1);
  assert.equal(A.add(A.emptyTotals(), result(race({ car: 127 }))).carsWon.length, 0, 'no car type: no car counted');
});

test('the achievements: each level once, at the result that reaches it', () => {
  let k = A.parse(null), got = [];
  const run = r => { const e = A.earn(k, r, 1000 + got.length); k = e.kept; const ids = e.got.map(g => A.levelKey(g.a, g.level)); got.push(...ids); return ids; };
  const lv = id => A.reached(A.ACHIEVEMENTS.find(a => a.id === id), k.got);
  assert.deepEqual(run(result(race({ place: 2 }))), []);
  assert.deepEqual(run(result(race({}))), ['wins:1']);
  run(result(race({}))); assert.deepEqual(run(result(race({}))), ['streak:1'], 'three in a row');
  for (const type of [A.SPRINT, A.DRAG, A.DRIFT, A.STREETX]) run(result(race({ type, score: type === A.DRIFT ? 1000 : 60000 })));
  assert.equal(lv('drift'), 0, 'a small drift');
  assert.ok(run(result(race({ type: A.URL }))).includes('all_types:1'));
  assert.equal(lv('wins'), 1, '8 wins: Winner I');
  assert.ok(run(result(race({ reversed: true }))).includes('both_ways:1'));
  assert.ok(run(result(race({ margin: 5 }))).includes('wins:2'), '10 wins: Winner II');
  assert.ok(got.includes('photo_finish:1'));
  assert.deepEqual(run(result(race({ type: A.DRIFT, score: 260000 }))).filter(x => x.startsWith('drift')), ['drift:1', 'drift:2', 'drift:3'],
    'one drift reaches three levels at once');
  assert.ok(run(result(race({ top: 300, place: 5 }))).includes('speed:2'), 'a top speed counts without a win');
  assert.equal(lv('speed'), 2);
  assert.ok(run(result(race({ mode: A.PLAYERS }))).includes('players:1'));
  assert.equal(new Set(got).size, got.length, 'none twice');
  for (const id of got) assert.ok(k.got[id] >= 1000, id);
  for (let car = 0; car < 10; car++) run(result(race({ car })));
  assert.equal(lv('cars'), 2);
  for (let i = 0; i < 30; i++) run(result(race({ track: 4100 + i, car: 3, meters: 40000 })));
  assert.equal(lv('tracks'), 2); assert.equal(lv('loyal'), 2); assert.equal(lv('streak'), 3); assert.equal(lv('km'), 3);
  assert.equal(A.ACHIEVEMENTS.length, new Set(A.ACHIEVEMENTS.map(a => a.id)).size, 'ids unique');
  for (const a of A.ACHIEVEMENTS) assert.deepEqual([...a.levels].sort((x, y) => x - y), a.levels, `${a.id}: levels in order`);
  assert.equal(A.LEVELS, A.ACHIEVEMENTS.reduce((s, a) => s + a.levels.length, 0));
});

test('the names and what each level asks', () => {
  const a = id => A.ACHIEVEMENTS.find(x => x.id === id);
  assert.equal(A.levelName(a('drift'), 2), 'Sideways II');
  assert.equal(A.levelName(a('both_ways'), 1), 'Both ways', 'one level: no number');
  assert.equal(a('wins').what(1), 'Win a race.');
  assert.equal(a('wins').what(250), 'Win 250 races.');
  assert.equal(a('drift').what(250000), 'Score 250,000 points in a drift.');
  assert.equal(a('km').what(5000), 'Race 5,000 km in all.');
});

test('achievements.json: kept and read back; a broken one starts again', () => {
  const k = A.earn(A.parse(null), result(race({})), 42).kept;
  const back = A.parse(JSON.stringify(k));
  assert.deepEqual(back, k);
  for (const bad of ['', 'nope', '{"v":2}', 'null', '[]']) assert.deepEqual(A.parse(bad), A.parse(null), bad);
  // a file of an earlier version of the totals gets the totals' new fields
  const old = JSON.parse(JSON.stringify(k)); delete old.totals.wonOn;
  assert.deepEqual(A.parse(JSON.stringify(old)).totals.wonOn, []);
});

test('a file whose totals reach levels it does not have gets them on load, silently, once', () => {
  const k = { v: 1, totals: { ...A.emptyTotals(), wins: 12, bestStreak: 3, bestDrift: 46755 }, got: { 'first_win': 5 } };
  const s = A.settle(k, 777);
  assert.deepEqual(Object.keys(s.got).filter(x => s.got[x] === 777).sort(), ['streak:1', 'wins:1', 'wins:2']);
  assert.equal(s.got.first_win, 5, 'an old key stays (it counts for nothing)');
  assert.equal(A.settle(s, 888), s, 'nothing more: the same object');
});
