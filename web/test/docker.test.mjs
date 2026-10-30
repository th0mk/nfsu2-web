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
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { serverCommand } from '../tools/server-cmd.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
// the games the server serves (its --game names)
const GAMES = ['ug2', 'mw'];
// the game's data files by their extensions (web/deploy/pack.sh refuses the same in its bundle), a lifted or the
// original DLL (Most Wanted's D3DX oracle is lifted from d3dx9_26.dll into gen/mw/d3dx) and the module (*.wasm)
const GAME_EXTS = /\.(BUN|BIN|LZC|VIV|MUS|ASF|VP6|ABK|GIN|CSI|MPF|AUD|LZW|DLL|EXE|WASM)$/i;
// a game's page as web/build.mjs writes it: web/dist for UG2, web/dist-<id> for build.mjs --game <id>
const pageOf = game => game === 'ug2' ? 'dist' : `dist-${game}`;
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

// one image for both games (PLAN U-03): each game's page, built in the page stage and copied to where the server looks
// for it (src/game.rs: the profile's dist under NFSU2_REPO=/app); of the page stage only the pages and the empty state
// folder, never a test page or source map (web/dist*-dev), and no module (it is mounted, /module)
test(`${DOCKERFILE} holds each game's page where its server looks for it, and no module`, () => {
  const text = fs.readFileSync(path.join(REPO, DOCKERFILE), 'utf8');
  const builds = text.split(/\r?\n/).filter(l => /^RUN\s.*node build\.mjs/.test(l)).join(' ');
  const fromPage = text.split(/\r?\n/).filter(l => /^COPY\s+(--\S+\s+)*--from=page\s/.test(l))
    .map(l => l.replace(/^COPY\s+/, '').split(/\s+/).filter(w => !w.startsWith('--')));
  const gameRs = fs.readFileSync(path.join(REPO, 'web/server-rs/src/game.rs'), 'utf8');
  const dists = Object.fromEntries([...gameRs.matchAll(/\bid: "(\w+)",[\s\S]*?\bdist: "([^"]+)"/g)].map(m => [m[1], m[2]]));
  for (const g of GAMES) {
    const page = pageOf(g);
    assert.equal(dists[g], `web/${page}`, `src/game.rs: ${g}'s page`);
    assert.ok(g === 'ug2' ? /node build\.mjs(?! --game)/.test(builds) : builds.includes(`node build.mjs --game ${g}`), `${g}'s page is built: ${builds}`);
    assert.ok(fromPage.some(([src, dst]) => src === `/src/web/${page}` && dst === `web/${page}`), `COPY --from=page /src/web/${page} web/${page}`);
  }
  for (const [src] of fromPage) assert.ok(/^\/src\/web\/dist(-[a-z0-9]+)?$/.test(src) && !src.endsWith('-dev') || src === '/state', `COPY --from=page ${src}`);
  assert.ok(!/^COPY\s.*\.wasm\b/m.test(text), 'no module copied into the image');
  assert.match(text, /^\s*NFSU2_WASM=\/module\//m, 'the module comes from the mounted /module');
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

// Each game's compose (web/deploy): Underground 2's compose.yml (with compose.tunnel.yml for its tunnel) as it is, and
// Most Wanted's box the same files with compose.mw.yml on top: the same image and settings, Most Wanted's names (the
// project, the slice, --game mw, its module).  As `docker compose config` resolves them, where Docker's CLI is there
// (Docker Desktop on Windows: node --test web/test/docker.test.mjs from a Windows shell); else skipped.
const DEPLOY = path.join(REPO, 'web/deploy');
function composeConfig(files) {
  const r = spawnSync('docker', ['compose', ...files.flatMap(f => ['-f', path.join(DEPLOY, f)]), 'config', '--format', 'json'],
    { encoding: 'utf8', env: { ...process.env, NFSU2_HOST: 'game.example.com', TUNNEL_TOKEN: 'token', COMPOSE_FILE: '', NFSU2_SLICE: '', NFSU2_IMAGE: '' } });
  if (r.error?.code === 'ENOENT') return null;
  assert.equal(r.status, 0, r.stderr);
  return JSON.parse(r.stdout);
}
const hasDocker = spawnSync('docker', ['compose', 'version'], { encoding: 'utf8' }).status === 0;

test('Most Wanted\'s compose: Underground 2\'s settings with its own names', { skip: !hasDocker && 'no docker compose here' }, () => {
  const ug2 = composeConfig(['compose.yml', 'compose.tunnel.yml']);
  const mw = composeConfig(['compose.yml', 'compose.tunnel.yml', 'compose.mw.yml']);
  assert.equal(ug2.name, 'nfsu2-web');
  assert.equal(mw.name, 'nfsmw-web');
  assert.deepEqual(ug2.services.server.command, ['--behind-proxy']);
  assert.deepEqual(mw.services.server.command, ['--game', 'mw', '--behind-proxy']);
  assert.equal(ug2.services.server.environment.NFSU2_WASM, undefined, 'UG2: the image\'s own (/module/nfsu2.wasm)');
  assert.equal(mw.services.server.environment.NFSU2_WASM, '/module/nfsmw.wasm');
  for (const [cfg, slice] of [[ug2, 'nfsu2.slice'], [mw, 'nfsmw.slice']])
    for (const [name, s] of Object.entries(cfg.services)) if (name !== 'caddy') assert.equal(s.cgroup_parent, slice, `${cfg.name} ${name}`);
  // the same image and the same settings, but for the names
  const same = s => { const c = structuredClone(s); delete c.cgroup_parent; delete c.command; delete c.environment?.NFSU2_WASM; return c; };
  for (const name of ['server', 'net', 'cloudflared']) assert.deepEqual(same(mw.services[name]), same(ug2.services[name]), name);
  const srv = mw.services.server;
  assert.equal(srv.network_mode, 'service:net');
  assert.equal(String(srv.mem_limit), String(8 * 2 ** 30));
  assert.equal(srv.ulimits.nofile, 65536);
  assert.equal(mw.services.net.sysctls['net.ipv4.tcp_rmem'], '4096 65536 131072');
  assert.equal(mw.services.cloudflared.scale, 4);
  assert.equal(mw.volumes.state.name, 'nfsmw-web_state');
});

test('Most Wanted\'s slice: its own name, Underground 2\'s quota', () => {
  const quota = f => fs.readFileSync(path.join(DEPLOY, f), 'utf8').match(/^CPUQuota=(\d+)%$/m)?.[1];
  assert.ok(quota('nfsmw.slice') && quota('nfsmw.slice') === quota('nfsu2.slice'));
  const mw = fs.readFileSync(path.join(DEPLOY, 'compose.mw.yml'), 'utf8');
  assert.match(mw, /^name: nfsmw-web$/m);
  assert.ok(mw.match(/\$\{NFSU2_SLICE:-nfsmw\.slice\}/g).length >= 3);
  assert.ok(!/nfsu2\.slice/.test(mw));
});

// The operator's commands on each box (docs/web/VPS.md, "Kick and ban"): ./nfsu2 next to Underground 2's compose files,
// ./nfsmw next to Most Wanted's, each the server's ctl in its own container (its own state folder and bans), with
// --game for Most Wanted's (tests/binary.rs kicks_and_bans_on_most_wanted runs that command)
test('each box\'s operator command: ctl in its own server, Most Wanted\'s with --game mw', () => {
  const last = f => fs.readFileSync(path.join(DEPLOY, f), 'utf8').trim().split('\n').at(-1);
  assert.equal(last('nfsu2'), 'cd "$(dirname "$0")" && exec docker compose exec -T server /app/nfsu2-server ctl "$@"');
  assert.equal(last('nfsmw'), 'cd "$(dirname "$0")" && exec docker compose exec -T server /app/nfsu2-server ctl --game mw "$@"');
  if (process.platform !== 'win32') for (const f of ['nfsu2', 'nfsmw']) assert.ok(fs.statSync(path.join(DEPLOY, f)).mode & 0o100, `${f} is executable`);
});
