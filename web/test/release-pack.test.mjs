// web/deploy/release.sh and pack.sh refuse a module built with the D3DX oracle linked (web/build.sh writes "oracle"
// into its json; dev only, PLAN D-09), and pack.sh takes the game's entries from out/web/server-rs/nfsu2-server only
// when that build is newer than their source, web/server-rs/src/game.rs (a stale one prints an old list).  Both run
// in stub trees; release.sh only as far as its refusal (-n, before the registry and ssh), pack.sh to its archive in
// the stub tree.
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
