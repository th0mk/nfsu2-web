// Tracks, career free roam on the race tracks' own maps (web/src/app/tracks.ts, runtime/travel.c):
//   node --test web/test/tracks.test.mjs
// - a spot's room: the world's room hash with the spot's key mixed in, as roam_game.c room_at makes it (uint32 maths:
//   these numbers are the C's), never 0; the nine spots' rooms in travel.c's order
// - the game's status "travel": where the car is and why a trip cannot go now, or no trips at all
// - a friend's race track from the server's answer (its world's name, its room's hash), its world's name held against
//   that hash, and the game's word on a trip to a friend ("travel_said")
// The module is bundled from web/src with esbuild, as web/test/worlds.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * from './web/src/app/tracks.ts'; export * as P from './web/src/app/players.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const T = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

test('a spot\'s room is the world\'s with the spot\'s key mixed in, as the game makes it', () => {
  // (base ^ key * 0x9E3779B1) as uint32 in C, 0 made 1
  assert.equal(T.trackRoom(0, 4711), 2397071415);
  assert.equal(T.trackRoom(0, 4605), 199754989);
  assert.equal(T.trackRoom(0x12345678, 4301), 942920901);
  assert.equal(T.trackRoom(0xffffffff, 4201), 2745435750);
  const base = T.P.roomHash('World 2');
  assert.equal(T.trackRoom(base, 4711), ((base ^ 2397071415) >>> 0) || 1);
  assert.notEqual(T.trackRoom(base, 4711), base, 'not the world\'s own room');
});

test('the nine spots in travel.c\'s order, each a room of its own', () => {
  assert.deepEqual(T.TRACKS.map(s => s.key), [4711, 4701, 4311, 4301, 4201, 4211, 4221, 4601, 4605]);
  const rooms = T.trackRooms(0);
  assert.equal(rooms.length, 9);
  assert.equal(new Set(rooms).size, 9);
  assert.ok(!rooms.includes(0), 'none is the main world');
});

test('the game\'s "travel": where the car is and why a trip cannot go now', () => {
  assert.equal(T.travelState(undefined), null, 'a game without trips');
  assert.equal(T.travelState(0), null, 'not in a career');
  assert.deepEqual(T.travelState(65536), { at: 0, why: 0 }, 'in the city, a trip can go');
  assert.deepEqual(T.travelState(65536 + 3), { at: 3, why: 0 }, 'at the Stadium\'s drift');
  assert.deepEqual(T.travelState(65536 + 5 * 256 + 0), { at: 0, why: 5 }, 'on a marker');
  assert.deepEqual(T.travelState(65536 + 6 * 256 + 1), { at: 1, why: 6 }, 'on the way');
  assert.equal(T.travelState(65536 + 10), null, 'a spot this page does not know');
  assert.equal(T.travelState(65536 + 99 * 256), null, 'a reason this page does not know');
  assert.equal(T.whyText(0), '');
  assert.equal(T.whyText(5), 'Drive off the marker first.');
  assert.equal(T.atName(0), '');
  assert.equal(T.atName(1), 'URL: Airport Circuit');
});

test('a friend\'s race track, from its world\'s name and its room\'s hash', () => {
  const w2 = T.P.roomHash('World 2');
  assert.equal(T.friendSpot('', 0), 0, 'the main world\'s city');
  assert.equal(T.friendSpot('World 2', w2), 0, 'World 2\'s city');
  assert.equal(T.friendSpot('', T.trackRoom(0, 4311)), 3, 'the main world\'s Stadium drift');
  assert.equal(T.friendSpot('World 2', T.trackRoom(w2, 4605)), 9, 'World 2\'s Street X Parkade');
  assert.equal(T.friendSpot(undefined, T.trackRoom(0, 4701)), 2, 'no world\'s name: the main world\'s');
  assert.equal(T.friendSpot('World 2', T.trackRoom(0, 4311)), 0, 'another world\'s spot: not this one\'s');
  assert.equal(T.friendSpot('World 2', undefined), 0, 'a server of before: no hash');
});

test('a friend\'s world, held against its room\'s hash', () => {
  const w2 = T.P.roomHash('World 2');
  assert.equal(T.friendRoom('', 0), '', 'the main world\'s city');
  assert.equal(T.friendRoom('player', T.trackRoom(0, 4311)), '', 'the main world\'s track, whatever name came with it');
  assert.equal(T.friendRoom(undefined, T.trackRoom(0, 4711)), '', 'the main world\'s track, no name');
  assert.equal(T.friendRoom('World 2', w2), 'World 2');
  assert.equal(T.friendRoom('world2', T.trackRoom(w2, 4605)), 'world2', 'its spot, the name as the game hashes it');
  assert.equal(T.friendRoom('Blue Room', w2), undefined, 'a name that leads elsewhere: none');
  assert.equal(T.friendRoom(undefined, w2), undefined, 'no name');
  assert.equal(T.friendRoom('Blue Room', undefined), 'Blue Room', 'a server of before: no hash');
});

test('the game\'s word on a trip to a friend, once each', () => {
  assert.equal(T.travelSaid(undefined, 0), null);
  assert.equal(T.travelSaid(0, 0), null, 'none yet');
  assert.deepEqual(T.travelSaid(1 * 256 + T.SAID_LOCKED, 0), { n: 1, what: T.SAID_LOCKED, why: 0 });
  assert.equal(T.travelSaid(1 * 256 + T.SAID_LOCKED, 1), null, 'the word seen already');
  assert.deepEqual(T.travelSaid(2 * 256 + 5 * 16 + T.SAID_NOT_NOW, 1), { n: 2, what: T.SAID_NOT_NOW, why: 5 }, 'not now: on a marker');
});
