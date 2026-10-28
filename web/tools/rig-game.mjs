// The game a test tool runs: --game ug2|mw, Underground 2 unless given, as the server (web/server-rs/src/game.rs)
// and the page's build (web/build.mjs --game) name them.  What every tool needs to know about the game: the program
// the test pages start, the folder of its test scripts, seeds and checks, and the registry its runs start from.
// The server with --dev serves the game's test folder at /tests/ (tests/ for Underground 2, tests/mw/ for Most
// Wanted), so a script is /tests/scripts/<name> for either game.  A tool keeps its own per-game data (its scenarios,
// script generators, screens) in a table of its own.
//   const GAME = gameOf(argv);                                    // an unknown game stops the tool
//   if (argv.includes('--help')) usage(import.meta.url, GAME, 'Scenarios', lines, 'none yet: ...');
//   if (!table[GAME]) nothingFor(GAME, 'none yet: ...');          // nothing to run for that game yet
//   await requireGame(base, GAME);                                // the server at base serves that game's module
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

export const GAMES = {
  ug2: {
    name: 'NFS Underground 2',
    /** the program, as the test pages pass it (arg=) */
    exe: '/data/SPEED2.EXE',
    /** the test folder in the repository: scripts/, checks/ (the suite's), web/seed/ */
    tests: 'tests',
    /** the registry the runs start from, as the server serves it */
    reg: '/tests/web/seed/registry_perf4_native.txt',
  },
  mw: {
    name: 'NFS Most Wanted',
    exe: '/data/speed.exe',
    /** scripts/, seed/ and checks/ */
    tests: 'tests/mw',
    /** none yet: the game's own first start */
    reg: '',
  },
};

/** the --game of a tool's arguments (ug2 without one), in any case; an unknown one stops the tool */
export function gameOf(argv) {
  const i = argv.indexOf('--game');
  if (i < 0) return 'ug2';
  const g = String(argv[i + 1] ?? '').toLowerCase();
  if (!Object.hasOwn(GAMES, g)) {
    console.error(`--game ${argv[i + 1] ?? ''}: no such game (${Object.keys(GAMES).join(', ')})`);
    process.exit(2);
  }
  return g;
}

/** --help: the tool's header comment (its usage), then its `title` for the game: `lines`, or `none` without any */
export function usage(url, game, title, lines, none = 'none yet') {
  const head = [];
  for (const l of fs.readFileSync(fileURLToPath(url), 'utf8').split('\n')) {
    if (!l.startsWith('//')) break;
    head.push(l.replace(/^\/\/ ?/, ''));
  }
  console.log(`${head.join('\n')}\n\n${title} for ${GAMES[game].name} (--game ${game}):`);
  for (const l of lines.length ? lines : [none]) console.log(`  ${l}`);
  process.exit(0);
}

/** stops a tool that has nothing to run for the game yet */
export function nothingFor(game, why) {
  console.error(`--game ${game}: ${why}`);
  process.exit(2);
}

/** stops a tool whose server serves the other game's module (/game.json's game; a module of before the field was
 *  Underground 2's).  A server it cannot reach is left to the tool to report. */
export async function requireGame(base, game) {
  let served;
  try { served = (await (await fetch(new URL('game.json', base.replace(/\/*$/, '/')))).json()).game ?? 'ug2'; } catch { return; }
  if (served !== game) {
    console.error(`${base} serves the ${GAMES[served]?.name ?? served} module, not ${GAMES[game].name}: start the server with ` +
      `--game ${game}, or give this tool --game ${served}`);
    process.exit(2);
  }
}

/** the module the server at base sends, as its /game.json names it, in one line for a run's output (a result counts
 *  only for the module that made it): revision, game, lift options, the module's hash (/game.wasm?v=) and the lift
 *  stamp where the json has one (Underground 2's has none).  null when there is no json to read. */
export async function moduleLine(base) {
  let j;
  try { j = await (await fetch(new URL('game.json', base.replace(/\/*$/, '/')))).json(); } catch { return null; }
  if (!j || typeof j !== 'object') return null;
  return `module: rev ${j.rev ?? '?'}, game ${j.game ?? 'ug2 (no game field)'}, lift ${j.lift || '-'}, hash ${j.module ?? '?'}` +
    (j.stamp ? `, stamp ${j.stamp}` : '');
}
