// LAN play's Winsock HLE in the browser, built for wasm:
//   node --test web/test/lan-wasm.test.mjs        (needs wasi-sdk in deps/)
// web/test/lan_unit.c: runtime/winsock.c (Windows semantics) over
// runtime/web/net_lan.c (the virtual-LAN backend), the test playing the page
// and the other browsers on the switch; plus SHA-256 (runtime/web/sha256.c,
// server.dll's pin) against the FIPS 180-4 vectors.  And MW's pin of its
// server.dll: games/mw/game.conf's SRV_SHA256 at SRV_BASE 0x00D00000, which
// GAME=mw gen.sh records in gen/mw/server/modinfo.c (checked when that lift is
// there), and the browser's SHA-256 of the game's file, which runtime/module.c
// compares with it: the file's is the pin, one changed byte's is not (checked
// when the game folder is there: NFSMW_DATA, default the ignored mw/ folder
// of the checkout or of the main clone).
import test from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const WASI = fs.readdirSync(path.join(REPO, 'deps')).filter(d => d.startsWith('wasi-sdk-')).map(d => path.join(REPO, 'deps', d)).pop();
const CC = WASI && path.join(WASI, 'bin/clang');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
// web/build.sh's target, features and defines
const FLAGS = ['--target=wasm32-wasip1-threads', '-pthread', '-O2', '-msimd128', '-mbulk-memory', '-mnontrapping-fptoint',
  '-mtail-call', '-mexception-handling', '-mllvm', '-wasm-enable-sjlj', '-mllvm', '-wasm-use-legacy-eh=false',
  '-D_WASI_EMULATED_MMAN', '-D_WASI_EMULATED_SIGNAL', '-D_WASI_EMULATED_PROCESS_CLOCKS', '-fno-delete-null-pointer-checks',
  '-include', 'runtime/web/compat/wasm_plat.h', '-Iruntime/web/compat', '-Iruntime', '-Iruntime/web'];
const LINK = ['-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=67108864,--max-memory=268435456',
  '-Wl,-z,stack-size=1048576', '-lsetjmp', '-lwasi-emulated-mman', '-lwasi-emulated-signal', '-lwasi-emulated-process-clocks'];

let built;
function build() {
  if (built) return built;
  const w = path.join(OUT, 'lan_unit.wasm');
  execFileSync(CC, [...FLAGS, '-o', w, 'web/test/lan_unit.c', 'runtime/winsock.c', 'runtime/web/net_lan.c', 'runtime/web/sha256.c',
    'runtime/mem.c', 'runtime/web/thread_exit.c', ...LINK], { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  return (built = w);
}

test('lan_unit on wasm (winsock.c + net_lan.c)', { skip: !CC && 'no wasi-sdk in deps/' }, () => {
  const w = build();
  const run = spawnSync(process.execPath, [path.join(REPO, 'web/test/wasi-run.mjs'), w], { encoding: 'utf8' });
  const out = run.stdout ?? '', err = run.stderr ?? '';
  if (run.status !== 0) console.log(err);
  console.log(out.trim().split('\n').filter(l => /FAIL|lan_unit:/.test(l)).join('\n'));
  const m = /lan_unit: (\d+) passed, (\d+) failed/.exec(out);
  assert.ok(m, 'lan_unit ran to the end');
  assert.equal(+m[2], 0, 'no failed check');
  assert.ok(+m[1] >= 80, 'every check ran');
  // the page closing its link at exit is logged as that, not as a lost switch
  const tail = err.slice(err.indexOf('lan_unit: the page closes the link'));
  assert.ok(err.includes('lan_unit: the page closes the link'), 'the test closed the link');
  assert.match(tail, /\[net\] virtual LAN closed/, 'a "closed" line');
  assert.doesNotMatch(tail, /reconnecting/, 'no "reconnecting" line');
  // the page letting the switch go when no network session wants it is logged as that too
  const idle = err.slice(err.indexOf('lan_unit: the page lets go'), err.indexOf('lan_unit: the page closes the link'));
  assert.match(idle, /\[net\] virtual LAN idle: no network session/, 'an "idle" line');
  assert.doesNotMatch(idle, /reconnecting/, 'no "reconnecting" line');
});

// MW's server.dll: v1.3's file (380,928 bytes), mapped at 0x00D00000 (PLAN D-08)
const conf = id => Object.fromEntries(fs.readFileSync(path.join(REPO, 'games', id, 'game.conf'), 'utf8').split('\n')
  .filter(l => /^[A-Z0-9_]+=/.test(l)).map(l => [l.slice(0, l.indexOf('=')), l.slice(l.indexOf('=') + 1)]));
const MW = conf('mw');
const MW_PIN = 'c4fdd0e9e779be7f4be7728b03339eda4fd27248ceb9d2c640403b8fc90587ff';
// the game folder: NFSMW_DATA, else the ignored mw/ folder of this checkout or, in a worktree, of the main clone
const mainClone = () => {
  const r = spawnSync('git', ['-C', REPO, 'rev-parse', '--path-format=absolute', '--git-common-dir'], { encoding: 'utf8' });
  return r.status === 0 ? path.dirname(r.stdout.trim()) : REPO;
};
const MW_DIR = process.env.NFSMW_DATA || [REPO, mainClone()].map(d => path.join(d, 'mw/Need For Speed Most Wanted Black Edition'))
  .find(d => fs.existsSync(path.join(d, 'server.dll'))) || path.join(REPO, 'mw/Need For Speed Most Wanted Black Edition');
const MW_SRV = path.join(MW_DIR, 'server.dll');
const MW_INFO = path.join(REPO, MW.GEN_DIR, 'server/modinfo.c');

test('MW server.dll pin: game.conf pins v1.3\'s file at 0x00D00000', () => {
  assert.equal(MW.SRV_SHA256, MW_PIN);
  assert.equal(MW.SRV_BASE, '0x00D00000');
});

test('MW server.dll pin: gen/mw/server/modinfo.c names the pinned file at 0x00D00000',
  { skip: !fs.existsSync(MW_INFO) && `no ${MW.GEN_DIR}/server (GAME=mw web/gen.sh <speed.exe> lifts it)` }, () => {
  const m = /g_srv_modinfo = \{\s*"([^"]*)", "([0-9a-f]*)",\s*0x([0-9a-f]+)u, 0x([0-9a-f]+)u, 0x([0-9a-f]+)u, 0x([0-9a-f]+)u,\s*0x([0-9a-f]+)u, 0x([0-9a-f]+)u, (\d+)u,/
    .exec(fs.readFileSync(MW_INFO, 'utf8'));
  assert.ok(m, 'a ModInfo of tools/prepare_module.py');
  const [, name, sha, base, pref, size, entry, , , fileSize] = m;
  assert.equal(name, 'server.dll');
  assert.equal(sha, MW_PIN, 'the lift is of the pinned file (an earlier stub module has no hash)');
  assert.equal(parseInt(base, 16), 0x00D00000);
  assert.equal(parseInt(pref, 16), 0x10000000);
  assert.equal(parseInt(size, 16), 0x6F000);
  assert.equal(parseInt(entry, 16), 0x00D00000 + 0x3FF65, '_DllMainCRTStartup (preferred 0x1003FF65)');
  assert.equal(+fileSize, 380928);
});

test('MW server.dll pin on wasm: the browser\'s SHA-256 of the game\'s file is the pin, one changed byte\'s is not',
  { skip: (!CC && 'no wasi-sdk in deps/') || (!fs.existsSync(MW_SRV) && `no ${MW_SRV} (NFSMW_DATA=<MW's folder>)`) }, () => {
  const w = build();
  const run = spawnSync(process.execPath, [path.join(REPO, 'web/test/wasi-run.mjs'), w, 'pin', '/game/server.dll'],
    { encoding: 'utf8', env: { ...process.env, WASI_DIR: `/game=${path.dirname(MW_SRV)}` } });
  const m = /lan_unit: pin (\d+) bytes, sha256 ([0-9a-f]{64}), one byte changed ([0-9a-f]{64})/.exec(run.stdout ?? '');
  assert.ok(m, `lan_unit hashed the file: ${run.stdout}${run.stderr}`);
  assert.equal(run.status, 0);
  assert.equal(+m[1], 380928);
  assert.equal(m[2], crypto.createHash('sha256').update(fs.readFileSync(MW_SRV)).digest('hex'), 'the same hash as node\'s');
  assert.equal(m[2], MW_PIN, 'the game\'s server.dll is the pinned file');
  assert.notEqual(m[3], MW_PIN, 'one changed byte: another hash, so runtime/module.c refuses the file');
});
