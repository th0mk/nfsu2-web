// The game the server's tests run against: NFSU2_TEST_GAME=ug2|mw (ug2 unless set), as the server's --game names
// them (web/server-rs/src/game.rs).  What a test needs to know of it:
//   import { G } from './game-fixture.mjs';
//   G.id         'ug2' or 'mw'
//   G.args       the server's options for it: [] for Underground 2; Most Wanted's --game and its made-up module
//   G.base       where its routes are on this machine ('' for Underground 2, '/mw' for Most Wanted: PLAN D-38)
//   G.exe        its program as the folder holds it ('SPEED2.EXE', 'speed.exe')
//   G.data       its game folder: Underground 2's real one (NFSU2_DATA), for Most Wanted a made-up one (below)
//   G.wasm       its module (Underground 2's built one, out/web/nfsu2.wasm; Most Wanted's made up)
//   G.dist       its page, built (web/dist, web/dist-mw: node build.mjs [--game mw])
//   G.online     whether its server has an Online mode (Most Wanted's none yet: /players says null)
//   G.big        a large file of the folder, as /data/ serves it (a download that takes a while)
//   G.private    the names of its install folder that the server never serves (the audit's PRIV-1), made in the
//                made-up folder
//   G.skip       why the tests cannot run here (false: they can)
// Most Wanted's folder and module are made up, in a temporary folder of their own (gone when the test process
// ends): no Most Wanted file is needed to test what the server serves of it.  The folder has the game's entries
// (speed.exe, 3 MiB that start "MZ", so that the data's runs have blocks; server.dll; GLOBAL, CARS, TRACKS, SOUND,
// MOVIES with a 16 MiB file),
// and what an install holds besides the game, which is never served: SAVE, scripts, Support, Uninstall,
// 00000000.256, dinput8.dll.  The module: a few bytes with the wasm magic, and its json naming the game.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const id = (process.env.NFSU2_TEST_GAME || 'ug2').toLowerCase();
if (id !== 'ug2' && id !== 'mw') throw new Error(`NFSU2_TEST_GAME=${process.env.NFSU2_TEST_GAME}: ug2 or mw`);

/** bytes that start "MZ" and do not compress much (a xorshift's), n of them */
export function exeBytes(n) {
  const b = Buffer.alloc(n);
  let x = 0x2545f491;
  for (let i = 0; i < n; i++) { x ^= x << 13; x ^= x >>> 17; x ^= x << 5; b[i] = i % 7 ? x & 0x3f : x & 0xff; }
  b.write('MZ', 0);
  return b;
}

/** Most Wanted's made-up install folder and module, in `root` */
export function madeUpMw(root) {
  const data = path.join(root, 'Need For Speed Most Wanted');
  const put = (rel, body) => { fs.mkdirSync(path.dirname(path.join(data, rel)), { recursive: true }); fs.writeFileSync(path.join(data, rel), body); };
  put('speed.exe', exeBytes(3 << 20));
  put('server.dll', exeBytes(64 << 10));
  put('GLOBAL/GLOBALB.BUN', 'global');
  put('CARS/CAR/GEOMETRY.BIN', 'car');
  put('TRACKS/STREAM.BUN', 'track');
  put('SOUND/PFDATA/music.mus', 'music');
  // (a movie large enough that a download nobody reads fills the sockets' buffers: the audit's F3)
  put('MOVIES/BIG.VP6', exeBytes(16 << 20));
  // what an install holds besides the game (PRIV-1): never served, never listed
  put('SAVE/NFS Most Wanted/profile', 'SECRET-SAVE');
  put('scripts/mod.ini', 'SECRET-INI');
  put('Support/dxdiag.log', 'SECRET-PC-NAME');
  put('Uninstall/uninstall.log', 'SECRET-PC-NAME');
  put('00000000.256', 'SECRET-256');
  put('dinput8.dll', 'SECRET-DLL');
  const wasm = path.join(root, 'module', 'nfsmw.wasm');
  fs.mkdirSync(path.dirname(wasm), { recursive: true });
  fs.writeFileSync(wasm, Buffer.concat([Buffer.from([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]), exeBytes(256 << 10)]));
  fs.writeFileSync(wasm.replace(/\.wasm$/, '.json'), JSON.stringify({ pages: 16, maxPages: 16, rev: 'test', game: 'mw', stamp: 'made up' }));
  return { data, wasm };
}

function game() {
  if (id === 'ug2') {
    const data = process.env.NFSU2_DATA || path.join(os.homedir(), 'projects/nsfu2/Need for Speed - Underground 2');
    const wasm = path.join(REPO, 'out/web/nfsu2.wasm'), dist = path.join(REPO, 'web/dist');
    return {
      id, args: [], base: '', exe: 'SPEED2.EXE', data, wasm, dist, online: true, big: '/data/MOVIES/DragTut.vp6',
      private: ['unins000.dat', 'SAVE', 'scripts'],
      skip: !fs.existsSync(path.join(data, 'SPEED2.EXE')) ? 'no game folder (NFSU2_DATA)'
        : !fs.existsSync(wasm) ? 'no out/web/nfsu2.wasm (web/build.sh)' : false,
    };
  }
  const root = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'nfsmw-test-game-'));
  process.on('exit', () => fs.rmSync(root, { recursive: true, force: true }));
  const { data, wasm } = madeUpMw(root);
  const dist = path.join(REPO, 'web/dist-mw');
  return {
    id, args: ['--game', 'mw', '--wasm', wasm], base: '/mw', exe: 'speed.exe', data, wasm, dist, online: false, big: '/data/MOVIES/BIG.VP6',
    private: ['SAVE', 'scripts', 'Support', 'Uninstall', '00000000.256', 'dinput8.dll'],
    skip: false,
  };
}

export const G = game();
/** the page is built (node build.mjs [--game mw]) */
G.page = fs.existsSync(path.join(G.dist, 'main.js'));
