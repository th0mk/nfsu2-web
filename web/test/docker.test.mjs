// The container image (web/server-rs/Dockerfile) never holds the game or anything made from it: its .dockerignore
// lets only the server's and the page's sources into the build, the Dockerfile copies nothing else from the
// checkout, and none of those files has a game file's name or is a link out of its folder.  The image itself, built
// and run with Caddy in Chrome: node web/tools/deploy-e2e.mjs --mode docker (docs/web/VPS.md).
//   node --test web/test/docker.test.mjs
// The games' own file names come from the server (out/web/server-rs/nfsu2-server, web/server-rs/build.sh; or
// $NFSU2_SERVER_BIN), as web/deploy/pack.sh takes them: nfsu2-server --game <id> --print-entries.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { serverCommand } from '../tools/server-cmd.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
// the games the server serves (its --game names)
const GAMES = ['ug2', 'mw'];
// the game's data files by their extensions (web/deploy/pack.sh refuses the same in its bundle)
const GAME_EXTS = /\.(BUN|BIN|LZC|VIV|MUS|ASF|VP6|ABK|GIN|CSI|MPF|AUD|LZW|DLL|EXE)$/i;
/** the top-level entries of a game's folder that the server serves (its program, server.dll, server.cfg, folders) */
function entries(game) {
  const [cmd, args] = serverCommand(['--game', game, '--print-entries']);
  return execFileSync(cmd, args, { encoding: 'utf8' }).split('\n').filter(Boolean);
}

// the Dockerfile, its ignore file, and what may go into its build (sources only, never gen/ (the lifted game code),
// out/ (the module) or the game folder)
const DOCKERFILE = 'web/server-rs/Dockerfile', IGNORE = 'web/server-rs/Dockerfile.dockerignore';
const ALLOWED = ['app/registry-defaults.txt', 'web/package.json', 'web/package-lock.json', 'web/build.mjs', 'web/index.html', 'web/src/',
  'web/server-rs/'];

const ignore = fs.readFileSync(path.join(REPO, IGNORE), 'utf8').split('\n').map(l => l.trim()).filter(l => l && !l.startsWith('#'));
// after "*": exceptions (!x), and exclusions inside one of them (a build folder), which only narrow what goes in
const excluded = ignore.slice(1).filter(l => !l.startsWith('!')).map(l => l.replace(/\/$/, ''));

test(`${IGNORE} leaves everything out, then lets in only the allowed sources`, () => {
  assert.equal(ignore[0], '*', 'the first pattern leaves everything out');
  for (const l of ignore.slice(1)) {
    if (l.startsWith('!')) assert.ok(ALLOWED.includes(l.slice(1)), `"${l.slice(1)}" is not one of the server's or the page's sources`);
    else assert.ok(ALLOWED.some(a => a.endsWith('/') && l.startsWith(a)), `"${l}": after "*", only exceptions, or exclusions inside them`);
  }
});

test(`${DOCKERFILE} copies from the checkout only what ${IGNORE} lets in`, () => {
  const copies = fs.readFileSync(path.join(REPO, DOCKERFILE), 'utf8').split(/\r?\n/)
    .filter(l => /^COPY\s/.test(l) && !/--from=/.test(l))
    .flatMap(l => l.replace(/^COPY\s+/, '').split(/\s+/).filter(w => !w.startsWith('--')).slice(0, -1));
  assert.ok(copies.length > 0);
  for (const c of copies) assert.ok(ALLOWED.some(a => c === a || c === a.replace(/\/$/, '') || c.startsWith(a)), `COPY ${c}`);
});

test('the build context has no game file names and no links', () => {
  // the games' own files at the top of their folders (the folders' names are common words: their files have the
  // extensions)
  const tops = new Set(GAMES.flatMap(entries).filter(e => e.includes('.')));
  assert.ok(tops.has('speed2.exe') && tops.has('speed.exe') && tops.has('server.dll'), [...tops].join(' '));
  const gameName = f => tops.has(path.posix.basename(f).toLowerCase()) || GAME_EXTS.test(f);
  const files = [];
  const walk = rel => {
    if (excluded.includes(rel)) return;
    const st = fs.lstatSync(path.join(REPO, rel));
    assert.ok(!st.isSymbolicLink(), `${rel} is a link`);
    if (st.isDirectory()) for (const e of fs.readdirSync(path.join(REPO, rel))) walk(`${rel}/${e}`);
    else files.push(rel);
  };
  for (const a of ALLOWED) walk(a.replace(/\/$/, ''));
  assert.ok(files.length > 20, `${files.length} files`);
  const bad = files.filter(gameName);
  assert.deepEqual(bad, []);
});
