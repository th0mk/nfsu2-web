// Stale lifted C (the perf review): gen.sh stamps gen/all and gen/server
// (tools/genstamp.sh), web/build.sh refuses a gen made from another override
// list, hook list or lifter (web/tools/gencheck.sh), and a plain ./gen.sh
// leaves no web-lift.txt behind.  Per game (GAME=, games/<id>/game.conf):
// genstamp greps UG2's files as always and another game's runtime/<id>/*.c,
// both refuse a core file holding an override or hook marker, every game's
// lifter hash covers its game.conf, gencheck refuses another game's gen, an
// exe line that is not game.conf's and a D3DX oracle gen without a stamp of
// this checkout, and gen.sh and web/gen.sh refuse a link to the shared gen.
// The pipeline's switches (GAME, GEN, GEN_STALE_OK, GEN_SHARED, D3DX_ORACLE,
// D3DX_DLL) in the runner's environment are left out of every run.
//   node --test web/test/gencheck.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
/** the environment without the pipeline's switches (no GAME: UG2, the default), plus extra */
const envWith = (extra = {}) => {
  const e = { ...process.env };
  for (const k of ['GAME', 'GEN', 'GEN_STALE_OK', 'GEN_SHARED', 'D3DX_ORACLE', 'D3DX_DLL']) delete e[k];
  return { ...e, ...extra };
};
const stamp = (args, extra) => execFileSync('bash', [path.join(REPO, 'tools/genstamp.sh'), ...args], { encoding: 'utf8', env: envWith(extra) }).trim();
const sources = stamp(['sources']);
const orig = stamp(['overrides']).split(',').filter(Boolean);
/** a game's exe line, as gen.sh writes it: the SHA-256 prefixes of game.conf's exe and server.dll */
const conf = id => fs.readFileSync(path.join(REPO, 'games', id, 'game.conf'), 'utf8');
const exeLine = id => `exe ${conf(id).match(/^EXE_SHA256=(\w{16})/m)[1]} server.dll ${conf(id).match(/^SRV_SHA256=(\w{16})/m)[1]}`;
const STAMP = `# gen.sh 2026-10-18T00:00:00Z: what gen/all and gen/server were lifted from\n${sources}\n${exeLine('ug2')}\noptions X87PC=1 X87OPT=1 FLAGABI=0\n`;
const MW = { GAME: 'mw' };
const mwOrig = stamp(['overrides'], MW).split(',').filter(Boolean);
const MW_STAMP = `# gen.sh 2026-10-25T00:00:00Z: what gen/mw/all and gen/mw/server were lifted from\n${stamp(['sources'], MW)}\n${exeLine('mw')}\noptions X87PC=1 LOCKSTRIP=1 X87ENV=1\n`;

/** a fake gen dir: decls.h with the runtime's _orig declarations, the given stamps, and a gameinfo.h when info */
function gen(name, { all = STAMP, server = all, decls = orig, info = false } = {}) {
  const g = path.join(OUT, `gencheck-${name}`);
  fs.rmSync(g, { recursive: true, force: true });
  for (const d of ['all', 'server']) fs.mkdirSync(path.join(g, d), { recursive: true });
  fs.writeFileSync(path.join(g, 'all/decls.h'), decls.map(a => `void sub_00${a}_orig(CPU*);\n`).join(''));
  if (info) fs.writeFileSync(path.join(g, 'all/gameinfo.h'), '#define GAMEINFO_TEXT_BASE 0x00401000u\n');
  if (all !== null) fs.writeFileSync(path.join(g, 'all/stamp.txt'), all);
  if (server !== null) fs.writeFileSync(path.join(g, 'server/stamp.txt'), server);
  return g;
}
/** games/<id>/game.conf and tools/gameconf.sh into a copy t; exeSha replaces the game's EXE_SHA256 */
function games(t, { exeSha, ids = ['ug2'] } = {}) {
  fs.mkdirSync(path.join(t, 'tools'), { recursive: true });
  fs.copyFileSync(path.join(REPO, 'tools/gameconf.sh'), path.join(t, 'tools/gameconf.sh'));
  for (const id of ids) {
    let conf = fs.readFileSync(path.join(REPO, 'games', id, 'game.conf'), 'utf8');
    if (exeSha) conf = conf.replace(/^EXE_SHA256=.*$/m, `EXE_SHA256=${exeSha}`);
    fs.mkdirSync(path.join(t, 'games', id), { recursive: true });
    fs.writeFileSync(path.join(t, 'games', id, 'game.conf'), conf);
  }
}
const sha256 = s => crypto.createHash('sha256').update(s).digest('hex');
const check = (g, env = {}) => spawnSync('bash', [path.join(REPO, 'web/tools/gencheck.sh')], { cwd: REPO, encoding: 'utf8', env: envWith({ GEN: g, ...env }) });

test('gencheck: a gen stamped from this checkout passes', () => {
  const r = check(gen('ok'));
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.equal(r.stdout, '');
});

test('gencheck: another hook list, override list or lifter is refused, and says which', () => {
  for (const [k, what] of [['hooks', /hook list/], ['overrides', /override list/], ['lifter', /lifter \(tools\/lift\.py/]]) {
    const stale = STAMP.replace(new RegExp(`^${k} \\S+`, 'm'), `${k} 0000000000000000`);
    const r = check(gen(`stale-${k}`, { all: stale }));
    assert.equal(r.status, 1, `${k}: ${r.stdout}`);
    assert.match(r.stdout, /is stale/);
    assert.match(r.stdout, what);
    const ok = check(gen(`stale-ok-${k}`, { all: stale }), { GEN_STALE_OK: '1' });
    assert.equal(ok.status, 0, ok.stdout);
    assert.match(ok.stdout, /building anyway/);
  }
});

test('gencheck: an unfinished lift, a gen/server from another run, missing _orig declarations are refused', () => {
  let r = check(gen('incomplete', { all: 'incomplete: gen.sh has not finished (or failed)\n', server: null }));
  assert.equal(r.status, 1); assert.match(r.stdout, /not a finished lift/);
  r = check(gen('server', { server: STAMP.replace('2026-10-18T00:00:00Z', '2026-10-17T00:00:00Z') }));
  assert.equal(r.status, 1); assert.match(r.stdout, /server is not from the gen\.sh run/);
  r = check(gen('server-missing', { server: null }));
  assert.equal(r.status, 1); assert.match(r.stdout, /differs or is missing/);
  r = check(gen('decls', { decls: orig.slice(1) }));
  assert.equal(r.status, 1); assert.match(r.stdout, /older than the runtime/);
});

test('gencheck: a gen without a stamp (made before stamps) gets a note only', () => {
  const r = check(gen('nostamp', { all: null, server: null }));
  assert.equal(r.status, 0, r.stdout);
  assert.match(r.stdout, /has no stamp/);
});

test('a plain ./gen.sh drops web-lift.txt and leaves gen/all unstamped until it finishes', () => {
  // gen.sh in a copy of the checkout's scripts and runtime sources; it fails
  // at its first lifter step (no Python there), after its preamble
  const t = path.join(OUT, 'gencheck-gensh');
  fs.rmSync(t, { recursive: true, force: true });
  fs.mkdirSync(path.join(t, 'tools'), { recursive: true }); fs.mkdirSync(path.join(t, 'runtime')); fs.mkdirSync(path.join(t, 'gen/all'), { recursive: true });
  fs.copyFileSync(path.join(REPO, 'gen.sh'), path.join(t, 'gen.sh'));
  fs.copyFileSync(path.join(REPO, 'tools/genstamp.sh'), path.join(t, 'tools/genstamp.sh'));
  for (const f of fs.readdirSync(path.join(REPO, 'runtime'))) if (/\.(c|m)$/.test(f) || f === 'core.list') fs.copyFileSync(path.join(REPO, 'runtime', f), path.join(t, 'runtime', f));
  for (const f of ['lift.py', 'genall.py', 'reloc2.py', 'argcount.py', 'flagabi.py', 'prepare_module.py']) fs.writeFileSync(path.join(t, 'tools', f), '');
  fs.writeFileSync(path.join(t, 'gen/all/web-lift.txt'), 'FLAGABI X87OPT\n');
  fs.writeFileSync(path.join(t, 'gen/all/stamp.txt'), STAMP);
  fs.writeFileSync(path.join(t, 'SPEED2.EXE'), 'not a PE file');
  games(t, { exeSha: sha256('not a PE file') });   // this "exe" passes the game's hash check
  // a core file holding a hook marker stops it before it writes anything
  const heap = path.join(t, 'runtime/heap.c'), heapSrc = fs.readFileSync(heap, 'utf8');
  fs.appendFileSync(heap, '/* HOOK_SITE(0x00401000) */\n');
  let r = spawnSync('bash', [path.join(t, 'gen.sh'), path.join(t, 'SPEED2.EXE')], { cwd: t, encoding: 'utf8', env: envWith() });
  assert.equal(r.status, 1, r.stdout + r.stderr);
  assert.match(r.stderr, /runtime\/heap\.c:\d+:\/\* HOOK_SITE/);
  assert.doesNotMatch(r.stdout, /OVERRIDE/);
  assert.equal(fs.readFileSync(path.join(t, 'gen/all/stamp.txt'), 'utf8'), STAMP, 'the stamp untouched');
  assert.ok(fs.existsSync(path.join(t, 'gen/all/web-lift.txt')), 'web-lift.txt untouched');
  fs.writeFileSync(heap, heapSrc);
  r = spawnSync('bash', [path.join(t, 'gen.sh'), path.join(t, 'SPEED2.EXE')], { cwd: t, encoding: 'utf8', env: envWith() });
  assert.notEqual(r.status, 0, 'gen.sh failed (no lifter in the copy)');
  assert.match(r.stdout, /OVERRIDE \(\d+\)/, 'it got past its preamble');
  assert.ok(!fs.existsSync(path.join(t, 'gen/all/web-lift.txt')), 'web-lift.txt is gone');
  assert.match(fs.readFileSync(path.join(t, 'gen/all/stamp.txt'), 'utf8'), /^incomplete/);
});

test('gen.sh and web/gen.sh refuse to lift through a worktree\'s link to the shared gen', () => {
  const t = path.join(OUT, 'gencheck-shared');
  fs.rmSync(t, { recursive: true, force: true });
  fs.mkdirSync(path.join(t, 'web'), { recursive: true }); fs.mkdirSync(path.join(t, 'gen'));
  for (const d of ['all', 'server']) { fs.mkdirSync(path.join(t, 'shared', d), { recursive: true }); fs.writeFileSync(path.join(t, 'shared', d, 'stamp.txt'), STAMP); }
  fs.writeFileSync(path.join(t, 'shared/all/web-lift.txt'), 'FLAGABI X87OPT\n');
  fs.writeFileSync(path.join(t, 'shared/server/shard_0000.c'), '/* lifted */\n');
  fs.copyFileSync(path.join(REPO, 'gen.sh'), path.join(t, 'gen.sh'));
  fs.copyFileSync(path.join(REPO, 'web/gen.sh'), path.join(t, 'web/gen.sh'));
  fs.writeFileSync(path.join(t, 'SPEED2.EXE'), 'x');
  games(t, { ids: ['ug2', 'mw'] });
  const SCRIPTS = { 'gen.sh': '\\./gen\\.sh', 'web/gen.sh': 'web/gen\\.sh' };
  const lift = (script, env) => spawnSync('bash', [path.join(t, script), path.join(t, 'SPEED2.EXE')], { cwd: t, encoding: 'utf8', env: envWith(env) });
  const untouched = what => {
    assert.ok(fs.existsSync(path.join(t, 'shared/all/web-lift.txt')), `${what}: the shared web-lift.txt is there`);
    assert.ok(fs.existsSync(path.join(t, 'shared/server/shard_0000.c')), `${what}: the shared gen/server is not emptied`);
    for (const d of ['all', 'server']) assert.equal(fs.readFileSync(path.join(t, 'shared', d, 'stamp.txt'), 'utf8'), STAMP, `${what}: the shared ${d}/stamp.txt`);
  };
  const refused = (link, env, hint = '') => {
    for (const [script, re] of Object.entries(SCRIPTS)) {
      const r = lift(script, env);
      assert.equal(r.status, 1, `${script}: ${r.stdout}${r.stderr}`);
      assert.match(r.stdout, new RegExp(`^${re}: ${link} is a link to .*which other worktrees build against;\\n.*GEN_SHARED=1 ${hint}${re} `));
      untouched(`${script}, ${link}`);
    }
  };
  fs.symlinkSync(path.join(t, 'shared/all'), path.join(t, 'gen/all'));
  refused('gen/all');
  fs.unlinkSync(path.join(t, 'gen/all')); fs.mkdirSync(path.join(t, 'gen/all'));
  fs.symlinkSync(path.join(t, 'shared/server'), path.join(t, 'gen/server'));
  refused('gen/server');
  // GEN_SHARED=1 gets past the link (here to the exe's hash check, which stops it before it writes)
  for (const script of Object.keys(SCRIPTS)) {
    const r = lift(script, { GEN_SHARED: '1' });
    assert.equal(r.status, 1, `${script}: ${r.stdout}${r.stderr}`);
    assert.doesNotMatch(r.stdout, /is a link/);
    assert.match(r.stdout, /not ug2's SPEED2\.EXE/);
    untouched(`${script}, GEN_SHARED=1`);
  }
  // another game's GEN_DIR (gen/mw), and a link on its way there
  fs.mkdirSync(path.join(t, 'gen/mw'));
  fs.symlinkSync(path.join(t, 'shared/all'), path.join(t, 'gen/mw/all'));
  refused('gen/mw/all', MW, 'GAME=mw ');
  fs.rmSync(path.join(t, 'gen/mw'), { recursive: true });
  fs.symlinkSync(path.join(t, 'shared'), path.join(t, 'gen/mw'));
  refused('gen/mw', MW, 'GAME=mw ');
});

test('gen.sh and web/gen.sh refuse an exe or a D3DX_DLL that is not the game\'s (game.conf SHA-256) before writing anything', () => {
  const t = path.join(OUT, 'gencheck-exehash');
  fs.rmSync(t, { recursive: true, force: true });
  fs.mkdirSync(path.join(t, 'web'), { recursive: true }); fs.mkdirSync(path.join(t, 'gen/all'), { recursive: true }); fs.mkdirSync(path.join(t, 'out'));
  fs.copyFileSync(path.join(REPO, 'gen.sh'), path.join(t, 'gen.sh'));
  fs.copyFileSync(path.join(REPO, 'web/gen.sh'), path.join(t, 'web/gen.sh'));
  games(t, { ids: ['ug2', 'mw'] });
  fs.writeFileSync(path.join(t, 'gen/all/web-lift.txt'), 'FLAGABI X87OPT\n');
  fs.writeFileSync(path.join(t, 'gen/all/stamp.txt'), STAMP);
  fs.writeFileSync(path.join(t, 'out/relocs.txt'), '00401000');
  fs.writeFileSync(path.join(t, 'SPEED2.EXE'), 'SPEED2.EXE with one byte changed');
  for (const [script, env, what] of [['gen.sh', {}, /not ug2's SPEED2\.EXE \(SHA-256 [0-9a-f]{64}; games\/ug2\/game\.conf wants f9dd86c0/],
                                     ['web/gen.sh', {}, /not ug2's SPEED2\.EXE/], ['web/gen.sh', { GAME: 'mw' }, /not mw's speed\.exe/]]) {
    const r = spawnSync('bash', [path.join(t, script), path.join(t, 'SPEED2.EXE')], { cwd: t, encoding: 'utf8', env: envWith(env) });
    assert.equal(r.status, 1, `${script}: ${r.stdout}${r.stderr}`);
    assert.match(r.stdout, what);
  }
  assert.equal(fs.readFileSync(path.join(t, 'gen/all/web-lift.txt'), 'utf8'), 'FLAGABI X87OPT\n', 'web-lift.txt untouched');
  assert.equal(fs.readFileSync(path.join(t, 'gen/all/stamp.txt'), 'utf8'), STAMP, 'the stamp untouched');
  assert.equal(fs.readFileSync(path.join(t, 'out/relocs.txt'), 'utf8'), '00401000', 'out/ untouched');
  assert.ok(!fs.existsSync(path.join(t, 'gen/mw')) && !fs.existsSync(path.join(t, 'out/mw')), 'no game folders made');
  const r = spawnSync('bash', [path.join(t, 'gen.sh'), path.join(t, 'SPEED2.EXE')], { cwd: t, encoding: 'utf8', env: envWith({ GAME: 'nosuch' }) });
  assert.equal(r.status, 1); assert.match(r.stdout, /GAME=nosuch: no games\/nosuch\/game\.conf/);
  // the D3DX oracle's DLL (D3DX_DLL, which both scripts check right after the exe): only the file game.conf's
  // D3DX_SHA256 names, and only for a game with a D3DX_BASE
  fs.writeFileSync(path.join(t, 'd3dx9_26.dll'), 'd3dx9_26.dll with one byte changed');
  for (const [env, what] of [[{ GAME: 'mw' }, /d3dx9_26\.dll: not mw's D3DX DLL \(SHA-256 [0-9a-f]{64}; games\/mw\/game\.conf wants 8ea96fe0/],
                             [{}, /D3DX_DLL: ug2 has no D3DX oracle \(games\/ug2\/game\.conf has no D3DX_BASE\)/]]) {
    const d = spawnSync('bash', ['-c', '. tools/gameconf.sh; gameconf_d3dx; echo passed'],
      { cwd: t, encoding: 'utf8', env: envWith({ ...env, D3DX_DLL: path.join(t, 'd3dx9_26.dll') }) });
    assert.equal(d.status, 1, d.stdout); assert.match(d.stdout, what);
  }
});

/** a copy t of genstamp and gencheck with both games, empty lifter files and the given runtime files */
function stampTree(name, files) {
  const t = path.join(OUT, `gencheck-${name}`);
  fs.rmSync(t, { recursive: true, force: true });
  fs.mkdirSync(path.join(t, 'web/tools'), { recursive: true });
  games(t, { ids: ['ug2', 'mw'] });
  fs.copyFileSync(path.join(REPO, 'tools/genstamp.sh'), path.join(t, 'tools/genstamp.sh'));
  fs.copyFileSync(path.join(REPO, 'web/tools/gencheck.sh'), path.join(t, 'web/tools/gencheck.sh'));
  for (const f of ['lift.py', 'lregs.py', 'genall.py', 'inline_hot.txt', 'retpast.txt', 'reloc2.py', 'argcount.py', 'flagabi.py', 'prepare_module.py']) fs.writeFileSync(path.join(t, 'tools', f), '');
  fs.writeFileSync(path.join(t, 'games/mw/inline_hot.txt'), '# none yet\n');
  fs.writeFileSync(path.join(t, 'games/mw/retpast.txt'), '007cf3ae\n');
  for (const [f, s] of Object.entries(files)) {
    fs.mkdirSync(path.dirname(path.join(t, f)), { recursive: true }); fs.writeFileSync(path.join(t, f), s); }
  return t;
}
const run = (t, script, args, env) => spawnSync('bash', [path.join(t, script), ...args], { cwd: t, encoding: 'utf8', env: envWith(env) });
// UG2's files in runtime/ (as always) and MW's in runtime/mw/; a clean core
const RUNTIME = {
  'runtime/core.list': '# the core\ncore.c\ncore.h   # a header\ngone.c\n',
  'runtime/core.c': 'int core(void){ return 0; }\n',
  'runtime/core.h': '#define CORE 1\n',
  'runtime/web/plat.c': 'int plat;\n',
  'runtime/feat.c': 'void sub_00512340_orig(CPU*);\nHOOK_SITE(0x00512345)\n',
  'runtime/feat.m': '/* calls sub_00498760_orig */\n',
  'runtime/d3dx_hle.c': 'DX_WRAP(D3DXFOO, 00650010)\n',
  'runtime/mw/game_mw.c': 'void sub_00611110_orig(CPU*);\nHOOK_SITE(0x00622220)\nDX_WRAP(D3DXBAR, 00d81000)\n',
};

test('genstamp: UG2 greps its files as always and names no game; GAME=mw greps runtime/mw/*.c and names the game', () => {
  const t = stampTree('games', RUNTIME);
  const gs = (c, env) => { const r = run(t, 'tools/genstamp.sh', [c], env); assert.equal(r.status, 0, r.stdout + r.stderr); return r.stdout.trim(); };
  assert.equal(gs('overrides'), '498760,512340,650010');
  assert.equal(gs('hooks'), '0x00512345');
  assert.equal(gs('origs'), 'sub_00498760_orig\nsub_00512340_orig');
  for (const c of ['overrides', 'hooks', 'origs', 'sources']) assert.equal(gs(c, { GAME: 'ug2' }), gs(c));
  const ug2 = gs('sources').split('\n');
  assert.deepEqual(ug2.map(l => l.split(' ')[0]), ['overrides', 'hooks', 'lifter']);
  assert.match(ug2[0], /^overrides [0-9a-f]{16} \(3\)$/);
  assert.equal(gs('overrides', { GAME: 'mw' }), '611110,d81000');
  assert.equal(gs('hooks', { GAME: 'mw' }), '0x00622220');
  assert.equal(gs('origs', { GAME: 'mw' }), 'sub_00611110_orig');
  const mw = gs('sources', { GAME: 'mw' }).split('\n');
  assert.deepEqual(mw.map(l => l.split(' ')[0]), ['game', 'overrides', 'hooks', 'lifter']);
  assert.equal(mw[0], 'game mw');
  assert.match(mw[1], /^overrides [0-9a-f]{16} \(2\)$/);
  assert.match(mw[2], /^hooks [0-9a-f]{16} \(1\)$/);
  // each game's lifter covers its own game.conf and tools/gameconf.sh (they decide FTOL2_VA, LOCKSTRIP, SRV_BASE) and
  // its own retpast list
  const lifter = () => [gs('sources').split('\n')[2], gs('sources', { GAME: 'mw' }).split('\n')[3]];
  let before = lifter();
  assert.deepEqual(before, [ug2[2], mw[3]]);
  for (const [f, changes] of [['games/mw/game.conf', [false, true]], ['games/ug2/game.conf', [true, false]], ['tools/gameconf.sh', [true, true]],
                              ['tools/retpast.txt', [true, false]], ['games/mw/retpast.txt', [false, true]]]) {
    fs.appendFileSync(path.join(t, f), '# a change\n');
    const now = lifter();
    for (const i of [0, 1]) assert.equal(now[i] !== before[i], changes[i], `${f}: ${['UG2', 'MW'][i]}'s lifter line`);
    before = now;
  }
  // without MW files: no overrides, no hooks
  fs.rmSync(path.join(t, 'runtime/mw'), { recursive: true });
  assert.equal(gs('overrides', { GAME: 'mw' }), '');
  assert.match(gs('sources', { GAME: 'mw' }).split('\n')[1], /^overrides e3b0c44298fc1c14 \(0\)$/);
  assert.equal(gs('overrides'), '498760,512340,650010');
  // this checkout: GAME=ug2 prints the stamp of no GAME
  assert.equal(stamp(['sources'], { GAME: 'ug2' }), sources);
});

test('genstamp and gencheck refuse a core file holding _orig, DX_WRAP or HOOK_SITE, comments included, for every game', () => {
  const clean = stampTree('core-clean', RUNTIME);
  assert.equal(run(clean, 'tools/genstamp.sh', ['check']).status, 0, 'a listed file that is missing is tools/gamelit.sh\'s to report');
  for (const [f, line] of [['runtime/core.c', '/* HOOK_SITE(0x00401000) */'], ['runtime/core.h', '// calls sub_00401000_orig'],
                           ['runtime/core.c', '#define W DX_WRAP'], ['runtime/web/plat.c', 'int keep_orig;']]) {
    const t = stampTree('core', { ...RUNTIME, [f]: RUNTIME[f] + line + '\n' });
    for (const GAME of ['ug2', 'mw']) {
      for (const c of ['sources', 'overrides', 'hooks', 'origs', 'check']) {
        const r = run(t, 'tools/genstamp.sh', [c], { GAME });
        assert.equal(r.status, 1, `${f} ${GAME} ${c}: ${r.stdout}${r.stderr}`);
        assert.equal(r.stdout, '', `${f} ${GAME} ${c}: no list printed`);
        assert.ok(r.stderr.includes(`${f}:2:${line}`), r.stderr);
      }
      const r = run(t, 'web/tools/gencheck.sh', [], { GAME, GEN: path.join(t, 'gen') });
      assert.equal(r.status, 1, r.stdout);
      assert.match(r.stdout, /a core file .* holds a game's override or hook marker/);
      assert.ok(r.stdout.includes(`${f}:2:${line}`), r.stdout);
    }
  }
});

test('gencheck: another game\'s gen is refused (GEN_STALE_OK or not), and so are an exe line that is not game.conf\'s and another game\'s lift without gameinfo.h', () => {
  assert.match(MW_STAMP, /\ngame mw\n/);
  let r = check(gen('mw-ok', { all: MW_STAMP, decls: mwOrig, info: true }), MW);
  assert.equal(r.status, 0, r.stdout); assert.equal(r.stdout, '');
  // without gameinfo.h the runtime would build with UG2's values (cpu.h): refused for MW, fine for UG2
  r = check(gen('mw-noinfo', { all: MW_STAMP, decls: mwOrig }), MW);
  assert.equal(r.status, 1, r.stdout); assert.match(r.stdout, /has no gameinfo\.h \(made before tools\/genall\.py wrote one\): run GAME=mw web\/gen\.sh/);
  r = check(gen('ug2-info', { info: true }));
  assert.equal(r.status, 0, r.stdout);
  // GAME=ug2 (or no GAME) on MW's gen, GAME=mw on UG2's
  for (const env of [{}, { GAME: 'ug2' }, { GEN_STALE_OK: '1' }]) {
    r = check(gen('mw-as-ug2', { all: MW_STAMP, decls: orig }), env);
    assert.equal(r.status, 1, r.stdout); assert.match(r.stdout, /is a lift of mw, not of ug2/);
  }
  r = check(gen('ug2-as-mw'), MW);
  assert.equal(r.status, 1, r.stdout); assert.match(r.stdout, /is a lift of ug2, not of mw/);
  // the exe line against game.conf: another exe, another server.dll, none; a stub server.dll is fine
  const [, exe, , srv] = exeLine('ug2').split(' ');
  for (const [name, line, what] of [['exe', `exe 0123456789abcdef server.dll ${srv}`, /exe 0123456789abcdef/], ['srv', `exe ${exe} server.dll 0123456789abcdef`, /server\.dll 0123456789abcdef/],
                                    ['noexe', null, /no exe line/], ['mwexe', exeLine('mw'), /not lifted from ug2's files/]]) {
    const s = line ? STAMP.replace(exeLine('ug2'), line) : STAMP.replace(`${exeLine('ug2')}\n`, '');
    for (const env of [{}, { GEN_STALE_OK: '1' }]) {
      r = check(gen(`exeline-${name}`, { all: s }), env);
      assert.equal(r.status, 1, `${name}: ${r.stdout}`);
      assert.match(r.stdout, /was not lifted from ug2's files/);
      assert.match(r.stdout, what);
      assert.match(r.stdout, /games\/ug2\/game\.conf: SPEED2\.EXE f9dd86c054878ce6/);
    }
  }
  r = check(gen('exeline-stub', { all: STAMP.replace(exeLine('ug2'), `exe ${exe}`) }));
  assert.equal(r.status, 0, r.stdout);
  r = check(gen('mw-exeline', { all: MW_STAMP.replace(exeLine('mw'), exeLine('ug2')), decls: mwOrig, info: true }), MW);
  assert.equal(r.status, 1, r.stdout); assert.match(r.stdout, /was not lifted from mw's files/);
  // MW's gen/mw/server: lifted from another server.dll (GEN_STALE_OK or not), or from another gen.sh run
  const mwExe = exeLine('mw').split(' ')[1];
  for (const env of [MW, { ...MW, GEN_STALE_OK: '1' }]) {
    r = check(gen('mw-srv', { all: MW_STAMP.replace(exeLine('mw'), `exe ${mwExe} server.dll 0123456789abcdef`), decls: mwOrig, info: true }), env);
    assert.equal(r.status, 1, r.stdout); assert.match(r.stdout, /was not lifted from mw's files/); assert.match(r.stdout, /server\.dll c4fdd0e9e779be7f/);
  }
  r = check(gen('mw-server', { all: MW_STAMP, server: MW_STAMP.replace('2026-10-25T00:00:00Z', '2026-10-24T00:00:00Z'), decls: mwOrig, info: true }), MW);
  assert.equal(r.status, 1, r.stdout); assert.match(r.stdout, /mw-server\/server is not from the gen\.sh run/);
  // MW's: no stamp, a stale lifter
  r = check(gen('mw-nostamp', { all: null, server: null, decls: mwOrig, info: true }), MW);
  assert.equal(r.status, 1, r.stdout); assert.match(r.stdout, /has no stamp, so no finished gen\.sh lift: run GAME=mw web\/gen\.sh <path to speed\.exe>/);
  r = check(gen('mw-stale', { all: MW_STAMP.replace(/^lifter \S+/m, 'lifter 0000000000000000'), decls: mwOrig, info: true }), MW);
  assert.equal(r.status, 1, r.stdout); assert.match(r.stdout, /is stale/); assert.match(r.stdout, /games\/mw\/game\.conf, games\/mw\/inline_hot\.txt/);
});

test('gencheck: the D3DX oracle\'s gen (gen/<game>/d3dx) needs a stamp of this checkout unless D3DX_ORACLE=0; UG2 has none', () => {
  const g = gen('mw-d3dx', { all: MW_STAMP, decls: mwOrig, info: true });
  const D = path.join(g, 'd3dx');
  fs.mkdirSync(D); fs.writeFileSync(path.join(D, 'fntab.c'), '');
  for (const env of [{}, { GEN_STALE_OK: '1' }]) {
    const r = check(g, { ...MW, ...env });
    assert.equal(r.status, 1, r.stdout); assert.match(r.stdout, /mw-d3dx\/d3dx \(the D3DX oracle, linked unless D3DX_ORACLE=0\) has no stamp of a finished lift/);
  }
  let r = check(g, { ...MW, D3DX_ORACLE: '0' });
  assert.equal(r.status, 0, r.stdout); assert.equal(r.stdout, '');
  fs.writeFileSync(path.join(D, 'stamp.txt'), 'incomplete: not finished\n');
  r = check(g, MW);
  assert.equal(r.status, 1, r.stdout); assert.match(r.stdout, /has no stamp of a finished lift/);
  fs.writeFileSync(path.join(D, 'stamp.txt'), MW_STAMP);
  r = check(g, MW);
  assert.equal(r.status, 0, r.stdout); assert.equal(r.stdout, '');
  // another lifter, or UG2's stamp (no game line): stale; GEN_STALE_OK=1 builds anyway
  for (const s of [MW_STAMP.replace(/^lifter \S+/m, 'lifter 0000000000000000'), STAMP]) {
    fs.writeFileSync(path.join(D, 'stamp.txt'), s);
    r = check(g, MW);
    assert.equal(r.status, 1, r.stdout); assert.match(r.stdout, /d3dx \(the D3DX oracle\) is stale/);
    r = check(g, { ...MW, GEN_STALE_OK: '1' });
    assert.equal(r.status, 0, r.stdout); assert.match(r.stdout, /building anyway/);
  }
  // UG2's game.conf has no D3DX_BASE: a d3dx folder in its gen is never linked, nor checked
  const u = gen('ug2-d3dx');
  fs.mkdirSync(path.join(u, 'd3dx')); fs.writeFileSync(path.join(u, 'd3dx/fntab.c'), '');
  r = check(u);
  assert.equal(r.status, 0, r.stdout); assert.equal(r.stdout, '');
});
