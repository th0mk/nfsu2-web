// web/deploy/release.sh and pack.sh refuse a module built with the D3DX oracle linked (web/build.sh writes "oracle"
// into its json; dev only, PLAN D-09), and pack.sh takes the game's entries from out/web/server-rs/nfsu2-server only
// when that build is newer than their source, web/server-rs/src/game.rs (a stale one prints an old list).  Both run
// in stub trees; release.sh there only as far as its refusal (-n, before the registry and ssh), pack.sh to its archive
// in the stub tree.  release.sh per game (--game mw: Most Wanted's folder and files; another game's module or folder
// refused; Underground 2's arguments as before) runs whole against a fake VPS: stand-ins for ssh, scp, curl and
// docker that keep it on this machine, in a folder of the test.
//   node --test web/test/release-pack.test.mjs
import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const base = fs.mkdtempSync(path.join(os.tmpdir(), 'nfsu2-relpack-'));
after(() => fs.rmSync(base, { recursive: true, force: true }));
const w = (t, p, s, mode) => { fs.mkdirSync(path.dirname(path.join(t, p)), { recursive: true }); fs.writeFileSync(path.join(t, p), s); if (mode) fs.chmodSync(path.join(t, p), mode); };
const json = (oracle, size) => `{"pages":16384,"maxPages":65536,"guestEnd":671088640,"rev":"0123abc","size":${size},"opt":"-O3","lift":"plain","game":"ug2"${oracle ? ',"oracle":1' : ''}}\n`;

test('release.sh refuses a module whose json says "oracle"', () => {
  const m = path.join(base, 'module');
  w(m, 'nfsu2.wasm', 'wasm');
  w(m, 'nfsu2.json', json(true, 4));
  const r = spawnSync('bash', [path.join(ROOT, 'web/deploy/release.sh'), '-n', 'root@vps.invalid', m], { cwd: base, encoding: 'utf8' });
  assert.equal(r.status, 1, r.stdout + r.stderr);
  assert.match(r.stderr, /nfsu2\.json has "oracle": a dev-only module with the D3DX oracle/);
  assert.doesNotMatch(r.stdout, /release: module/);
});

test('release.sh refuses any json of the folder it is given that says "oracle", before the names: web/build.sh\'s out/web/mw (Most Wanted\'s nfsmw.json), or nfsmw.* copied next to nfsu2.*', () => {
  for (const [name, sub] of [['buildsh', 'mw'], ['copied', '']]) {
    const t = path.join(base, name), m = path.join(t, sub);
    w(t, 'nfsu2.wasm', 'wasm');
    w(t, 'nfsu2.json', json(false, 4));
    w(m, 'nfsmw.wasm', 'wasm');
    w(m, 'nfsmw.json', json(true, 4).replace('"game":"ug2"', '"game":"mw"'));
    const r = spawnSync('bash', [path.join(ROOT, 'web/deploy/release.sh'), '-n', 'root@vps.invalid', m], { cwd: base, encoding: 'utf8' });
    assert.equal(r.status, 1, r.stdout + r.stderr);
    assert.ok(r.stderr.includes(`${path.join(m, 'nfsmw.json')} has "oracle": a dev-only module with the D3DX oracle`), r.stderr);
    assert.doesNotMatch(r.stderr, /needs nfsu2\.wasm/);
    assert.doesNotMatch(r.stdout, /release: module/);
  }
});

let n = 0;
/** a tree with pack.sh, a page, a module, the deploy files, a local server build that prints entries, and a git commit */
function tree({ oracle = false } = {}) {
  const t = path.join(base, `pack${n++}`);
  w(t, 'web/deploy/pack.sh', fs.readFileSync(path.join(ROOT, 'web/deploy/pack.sh')), 0o755);
  w(t, 'web/dist/index.html', '<!doctype html>\n');
  w(t, 'out/web/nfsu2.wasm', 'wasm');
  w(t, 'out/web/nfsu2.json', json(oracle, 4));
  for (const f of ['nfsu2-web.service', 'Caddyfile', 'cloudflared.yml', 'README.md']) w(t, `web/deploy/${f}`, `${f}\n`);
  w(t, 'web/server-rs/crates/headers/cloudflare-origin-pull-ca.pem', 'pem\n');
  w(t, 'web/server-rs/src/game.rs', '// the entries\n');
  w(t, 'out/web/server-rs/nfsu2-server', '#!/bin/sh\n[ "$*" = "--game ug2 --print-entries" ] && printf "speed2.exe\\ncars\\n"\n', 0o755);
  // a Linux binary to bundle (--server): only its ELF magic, so it runs nowhere
  w(t, 'elf/nfsu2-server', Buffer.from([0x7f, 0x45, 0x4c, 0x46, 1, 1, 1, 0]));
  w(t, 'game/SPEED2.EXE', 'x'); fs.mkdirSync(path.join(t, 'game/CARS')); w(t, 'game/extra.txt', 'x');
  const git = (...a) => execFileSync('git', ['-c', 'user.name=t', '-c', 'user.email=t@t.invalid', '-c', 'commit.gpgsign=false', '-c', 'core.hooksPath=/dev/null', ...a], { cwd: t });
  git('init', '-q'); git('add', '-A'); git('commit', '-q', '-m', 'stub');
  return t;
}
const pack = t => spawnSync('bash', [path.join(t, 'web/deploy/pack.sh'), '--no-build', '--server', path.join(t, 'elf/nfsu2-server'), '--data', path.join(t, 'game'), '--out', path.join(t, 'deploy-out')],
  { cwd: t, encoding: 'utf8', timeout: 60000 });

test('pack.sh refuses a module whose json says "oracle", before it bundles anything', () => {
  const t = tree({ oracle: true });
  const r = pack(t);
  assert.equal(r.status, 1, r.stdout + r.stderr);
  assert.match(r.stdout, /pack\.sh: REFUSED: out\/web\/nfsu2\.json has "oracle"/);
  assert.ok(!fs.existsSync(path.join(t, 'deploy-out')), 'no bundle');
});

test('pack.sh reads the entries from the local server build only when it is newer than game.rs', () => {
  const t = tree();
  const past = new Date(Date.now() - 3600e3), older = new Date(Date.now() - 7200e3);
  fs.utimesSync(path.join(t, 'web/server-rs/src/game.rs'), older, older);
  let r = pack(t);
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.match(r.stdout, /never serves \(leave them off the VPS\): extra\.txt\n/);
  assert.match(r.stdout, /nfsu2-web-\w+-amd64\.tar\.gz/);
  // game.rs changed after that build: its list may be an old one, so none is printed
  fs.utimesSync(path.join(t, 'out/web/server-rs/nfsu2-server'), past, past);
  fs.utimesSync(path.join(t, 'web/server-rs/src/game.rs'), new Date(), new Date());
  r = pack(t);
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.match(r.stdout, /NOTE: the list of the game's own files was not read: .*older than web\/server-rs\/src\/game\.rs/);
  assert.doesNotMatch(r.stdout, /never serves/);
});

// release.sh against a fake VPS (never a real one): ssh runs the command here, with /srv/ inside a folder of the
// test; scp copies there; curl answers as a registry that has the image; docker answers as a server that gets healthy.
// A log of every call, with the module docker sees at each of its calls (pull before the swap, up after it).
const FAKE = path.join(base, 'fake-bin');
w(FAKE, 'ssh', `#!/bin/bash
echo "ssh $*" >> "$FAKE_LOG"
shift
cmd="$*"
exec bash -c "\${cmd//\\/srv\\//$FAKE_HOST/srv/}"
`, 0o755);
w(FAKE, 'scp', `#!/bin/bash
echo "scp $*" >> "$FAKE_LOG"
[ "$1" = -q ] && shift
dst="\${@: -1}"; dst="\${dst#*:}"; dst="\${dst/\\/srv\\//$FAKE_HOST/srv/}"
cp "\${@:1:$(($# - 1))}" "$dst"
`, 0o755);
w(FAKE, 'curl', `#!/bin/bash
echo "curl \${@: -1}" >> "$FAKE_LOG"
case " $* " in *" -w "*) printf 200 ;; *) printf '{"token":"t"}' ;; esac
`, 0o755);
w(FAKE, 'docker', `#!/bin/bash
echo "docker $* | image=\${NFSU2_IMAGE-} module=$(cat module/*.wasm 2>/dev/null)" >> "$FAKE_LOG"
case "$*" in
  "compose ps -q server") echo cid ;;
  *State.Health.Status*) echo healthy ;;
  *image.revision*) echo "$FAKE_LABEL" ;;
esac
`, 0o755);
const REG = 'registry.invalid/test/nfsu2-web-rs';
const modJson = (rev, size, game) => `{"pages":16384,"maxPages":65536,"guestEnd":671088640,"rev":"${rev}","size":${size},"opt":"-O3","lift":"plain"${game ? `,"game":"${game}"` : ''}}\n`;
let hosts = 0;
/** a fake VPS with a game's folder (/srv/<dir>): its .env (COMPOSE_FILE) and the running module ('old') */
function fakeHost(dir, name, composeFile) {
  const h = path.join(base, `host${hosts++}`), d = path.join(h, 'srv', dir);
  w(d, '.env', `COMPOSE_FILE=${composeFile}\nNFSU2_HOST=game.example.com\nNFSU2_IMAGE=${REG}:sha-1111111\n`);
  w(d, `module/${name}.wasm`, 'old');
  w(d, `module/${name}.json`, modJson('1111111', 3, name === 'nfsmw' ? 'mw' : null));
  return { h, d, log: path.join(h, 'calls.log') };
}
/** a module folder: name.wasm ('new') and its json, of that game (null: none, as UG2's before Most Wanted) */
function moduleDir(name, game) {
  const m = fs.mkdtempSync(path.join(base, 'mod-'));
  w(m, `${name}.wasm`, 'new');
  w(m, `${name}.json`, modJson('2222222', 3, game));
  return m;
}
const release = (host, args, env = {}) => spawnSync('bash', [path.join(ROOT, 'web/deploy/release.sh'), ...args], {
  cwd: base, encoding: 'utf8', timeout: 60000,
  env: { ...process.env, PATH: `${FAKE}:${process.env.PATH}`, FAKE_HOST: host.h, FAKE_LOG: host.log, FAKE_LABEL: '2222222abcdef', NFSU2_REPO: REG, NFSU2_DIR: '', ...env },
});
const read = (d, f) => fs.readFileSync(path.join(d, f), 'utf8');
const calls = host => fs.existsSync(host.log) ? read(host.h, 'calls.log').trim().split('\n') : [];
const MW_FILES = 'compose.yml:compose.tunnel.yml:compose.mw.yml', UG2_FILES = 'compose.yml:compose.tunnel.yml';

test('release.sh --game mw on a fake VPS: Most Wanted\'s folder and files, the image pulled first, the module swapped, the server restarted last; --undo goes back', () => {
  const host = fakeHost('nfsmw', 'nfsmw', MW_FILES), m = moduleDir('nfsmw', 'mw');
  let r = release(host, ['--game', 'mw', 'root@vps.invalid', m]);
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.match(r.stdout, /release: module 2222222 \(3 bytes\) and image registry\.invalid\/test\/nfsu2-web-rs:sha-2222222 to root@vps\.invalid:\/srv\/nfsmw/);
  assert.match(r.stdout, /Undo: web\/deploy\/release\.sh --game mw root@vps\.invalid --undo/);
  assert.equal(read(host.d, 'module/nfsmw.wasm'), 'new');
  assert.equal(read(host.d, 'module.prev/nfsmw.wasm'), 'old');
  assert.ok(!fs.existsSync(path.join(host.d, 'module.new')) && !fs.existsSync(path.join(host.d, 'module/nfsu2.wasm')));
  assert.match(read(host.d, '.env'), new RegExp(`^NFSU2_IMAGE=${REG}:sha-2222222$`, 'm'));
  assert.match(read(host.d, '.env.prev'), /:sha-1111111$/m);
  assert.equal(read(host.d, 'module/release.json'), '{"module":"2222222","image":"2222222"}\n');
  const log = calls(host);
  assert.ok(log.includes(`scp -q ${m}/nfsmw.wasm ${m}/nfsmw.json root@vps.invalid:/srv/nfsmw/module.new/`), log.join('\n'));
  const pull = log.findIndex(l => l.startsWith('docker compose pull -q server'));
  const up = log.findIndex(l => l.startsWith('docker compose up -d server'));
  assert.ok(pull >= 0 && up > pull, log.join('\n'));
  assert.match(log[pull], /image=registry\.invalid\/test\/nfsu2-web-rs:sha-2222222 module=old$/, 'pulled while the old module still runs');
  assert.match(log[up], /module=new$/, 'restarted once the new module is in place');

  r = release(host, ['--game', 'mw', 'root@vps.invalid', '--undo']);
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.equal(read(host.d, 'module/nfsmw.wasm'), 'old');
  assert.match(read(host.d, '.env'), new RegExp(`^NFSU2_IMAGE=${REG}:sha-1111111$`, 'm'));
  assert.equal(read(host.d, 'module/release.json'), '{"module":"1111111","image":"2222222"}\n');
});

test('release.sh refuses another game\'s module and another game\'s folder, Most Wanted\'s anywhere but /srv/nfsmw', () => {
  const cases = [
    // [args, env, what it says]: each before the VPS is reached
    [() => ['--game', 'mw', 'root@vps.invalid', moduleDir('nfsmw', 'ug2')], {}, 'is a module of ug2, not of mw (--game)'],
    [() => ['--game', 'mw', 'root@vps.invalid', moduleDir('nfsmw', null)], {}, 'is a module of ug2, not of mw (--game)'],
    [() => ['root@vps.invalid', moduleDir('nfsu2', 'mw')], {}, 'is a module of mw, not of ug2 (--game)'],
    [() => ['--game', 'mw', 'root@vps.invalid', moduleDir('nfsmw', 'mw')], { NFSU2_DIR: '/srv/nfsu2' }, '/srv/nfsu2 is not Most Wanted\'s folder'],
    [() => ['--game', 'mw', 'root@vps.invalid', '--undo'], { NFSU2_DIR: '/srv/mw' }, '/srv/mw is not Most Wanted\'s folder'],
    [() => ['root@vps.invalid', moduleDir('nfsu2', 'ug2')], { NFSU2_DIR: '/srv/nfsmw/' }, '/srv/nfsmw/ is Most Wanted\'s folder: --game mw'],
    [() => ['--game', 'nfs6', 'root@vps.invalid', moduleDir('nfsu2', 'ug2')], {}, '--game: ug2 or mw'],
  ];
  for (const [args, env, says] of cases) {
    const host = fakeHost('nfsmw', 'nfsmw', MW_FILES);
    const r = release(host, args(), env);
    assert.equal(r.status, 1, r.stdout + r.stderr);
    assert.ok(r.stderr.includes(says), `${says}: ${r.stderr}`);
    assert.deepEqual(calls(host).filter(l => /^(ssh|scp|docker) /.test(l)), [], says);
    assert.equal(read(host.d, 'module/nfsmw.wasm'), 'old');
  }
  // on the VPS: a folder whose .env does not run the game's compose files is left as it was (nothing pulled or moved)
  for (const [dir, name, files, args, says] of [
    ['nfsmw', 'nfsmw', UG2_FILES, ['--game', 'mw'], '/srv/nfsmw is not Most Wanted\'s: its .env\'s COMPOSE_FILE has no compose.mw.yml'],
    ['nfsu2', 'nfsu2', MW_FILES, [], '/srv/nfsu2 is Most Wanted\'s (compose.mw.yml in its .env): --game mw'],
  ]) {
    const host = fakeHost(dir, name, files);
    const r = release(host, [...args, 'root@vps.invalid', moduleDir(name, name === 'nfsmw' ? 'mw' : 'ug2')]);
    assert.equal(r.status, 1, r.stdout + r.stderr);
    assert.ok(r.stderr.includes(`release: ${host.h}${says}`), r.stderr);
    assert.ok(!calls(host).some(l => l.startsWith('docker')), calls(host).join('\n'));
    assert.equal(read(host.d, `module/${name}.wasm`), 'old');
    assert.match(read(host.d, '.env'), /:sha-1111111$/m);
  }
});

test('release.sh with Underground 2\'s arguments: its files to its paths, as before Most Wanted', () => {
  // a json without "game", as main's web/build.sh writes UG2's
  const host = fakeHost('nfsu2', 'nfsu2', UG2_FILES), m = moduleDir('nfsu2', null);
  const r = release(host, ['root@vps.invalid', m]);
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.match(r.stdout, /Undo: web\/deploy\/release\.sh root@vps\.invalid --undo/);
  assert.deepEqual(calls(host).filter(l => /^(ssh|scp) /.test(l)), [
    "ssh root@vps.invalid mkdir -p '/srv/nfsu2/module.new'",
    `scp -q ${m}/nfsu2.wasm ${m}/nfsu2.json root@vps.invalid:/srv/nfsu2/module.new/`,
    "ssh root@vps.invalid NFSU2_DIR='/srv/nfsu2' bash -s -- release 'registry.invalid/test/nfsu2-web-rs:sha-2222222' '2222222' 'ug2' 'nfsu2'",
  ]);
  assert.deepEqual(fs.readdirSync(path.join(host.d, 'module')).sort(), ['nfsu2.json', 'nfsu2.wasm', 'release.json']);
  assert.equal(read(host.d, 'module/nfsu2.wasm'), 'new');
  assert.equal(read(host.d, 'module.prev/nfsu2.wasm'), 'old');
});
