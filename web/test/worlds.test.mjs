// The worlds of free roam (web/src/app/worlds.ts, players.ts worldCounts):
//   node --test web/test/worlds.test.mjs
// - World 1 to 4 by their fixed English names, whatever the spelling (the game's hash: capitals and blanks do not
//   count); the main world is no name, or blanks; anything else a world of the player's own
// - their names as a list and a sentence show them (English here: the page's language picks the words); World 4 has
//   Ghost mode off for everyone
// - the rooms /players?rooms= is asked for, and its answer by hash
// The modules are bundled from web/src with esbuild, as web/test/crews.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * from './web/src/app/worlds.ts'; export * as P from './web/src/app/players.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const W = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
const { P } = W;

test('World 1 to 4, the main world and a world of the player\'s own', () => {
  assert.deepEqual(W.WORLDS, ['World 1', 'World 2', 'World 3', 'World 4']);
  assert.deepEqual(['World 1', 'world 2', 'WORLD3', ' World  4 '].map(W.worldOf), [1, 2, 3, 4], 'as the game hashes them');
  assert.deepEqual(['', '  ', 'World 5', 'World', 'Blue Room'].map(W.worldOf), [0, 0, 0, 0, 0]);
  assert.deepEqual(['', ' \t', 'World 2', 'Blue Room'].map(W.isMain), [true, true, false, false]);
  assert.deepEqual(['', 'World 2', 'Blue Room', 'World 5'].map(W.isOwn), [false, false, true, true]);
});

test('their names: in a list, and in a sentence', () => {
  assert.deepEqual(['', 'world 3', 'World 4', ' Blue Room '].map(W.worldName), ['Main world', 'World 3', 'World 4 (Ghost mode off)', 'Blue Room']);
  assert.deepEqual(['', 'World 3', 'World 4', 'Blue Room'].map(W.worldNamed), ['Main world', 'World 3', 'World 4', '"Blue Room"'], 'a world of the player\'s own in quotes');
});

test('World 4: Ghost mode off for everyone', () => {
  assert.equal(W.NO_GHOST, 4);
  assert.deepEqual(['World 4', 'world4', 'World 3', '', 'Blue Room'].map(W.noGhost), [true, true, false, false, false]);
});

test('/players?rooms=: the main world, World 1 to 4, a world of the player\'s own; the counts by hash', () => {
  const four = W.WORLDS.map(P.roomHash), own = P.roomHash('Blue Room');
  assert.deepEqual(W.worldRooms(''), [0, ...four]);
  assert.deepEqual(W.worldRooms('World 2'), [0, ...four], 'World 2 is asked for already');
  assert.deepEqual(W.worldRooms('Blue Room'), [0, ...four, own]);
  assert.deepEqual([...P.worldCounts({ players: 9, online: 0, rooms: [7, 1, 0, 0, 0, 2] }, W.worldRooms('Blue Room'))],
    [[0, 7], [four[0], 1], [four[1], 0], [four[2], 0], [four[3], 0], [own, 2]]);
  assert.deepEqual([...P.worldCounts({ players: 9, online: 0 }, W.worldRooms(''))], [], 'an older server: not known');
});
