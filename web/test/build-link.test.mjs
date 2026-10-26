// The lifted objects a link takes, in stub trees with a stand-in wasi-sdk
// (its clang writes each -o file and logs the command; no real compile):
// web/build.sh links the objects of the current lift only and lists them in
// lift.objs; web/tools/corelink.sh and web/tools/main_all.sh link that list,
// never an object an older lift left in out/web/obj; corelink refuses a build
// older than the lift or the runtime sources and prints the link's errors; a
// build with the D3DX oracle linked has "oracle":1 in its json, whatever the
// game (review round 2).  The pipeline's switches (GAME, NFSU2_GAME, ...) in
// the runner's environment are left out of every run.
//   node --test web/test/build-link.test.mjs
import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const base = fs.mkdtempSync(path.join(os.tmpdir(), 'nfsu2-buildlink-'));
after(() => fs.rmSync(base, { recursive: true, force: true }));

let n = 0;
const w = (t, p, s = '', mode) => { const f = path.join(t, p); fs.mkdirSync(path.dirname(f), { recursive: true }); fs.writeFileSync(f, s); if (mode) fs.chmodSync(f, mode); };
const read = (t, p) => fs.readFileSync(path.join(t, p), 'utf8');
const h16 = s => crypto.createHash('sha256').update(s).digest('hex').slice(0, 16);
const STAMP = '# gen.sh 2026-10-26T00:00:00Z: what gen/all and gen/server were lifted from\noverrides 0 (0)\nhooks 0 (0)\nlifter 0\n';

/** a tree with the stand-in wasi-sdk in wasi/: clang logs each command to clang.log and writes its -o file;
 *  a link fails with the lines of link-error.txt when the tree has one */
function tree() {
  const t = path.join(base, `t${n++}`);
  w(t, 'wasi/bin/clang', `#!/bin/bash
o=; p=; for a in "$@"; do [ "$p" = -o ] && o="$a"; p="$a"; done
echo "$*" >> "${t}/clang.log"
case " $* " in *" -c "*) echo object > "$o"; exit 0 ;; esac
if [ -f "${t}/link-error.txt" ]; then cat "${t}/link-error.txt" >&2; exit 1; fi
echo module > "$o"
`, 0o755);
  w(t, 'wasi/bin/llvm-objdump', '#!/bin/sh\nexit 0\n', 0o755);
  return t;
}
/** the env for a script in t: the runner's without the pipeline's switches, the stand-in wasi-sdk, plus extra */
const envFor = (t, extra = {}) => {
  const e = { ...process.env };
  for (const k of ['GAME', 'NFSU2_GAME', 'GEN', 'GEN_STALE_OK', 'D3DX_ORACLE', 'FULL', 'MEMCHECK', 'OPT', 'J', 'INITIAL_MB', 'OBJCACHE', 'WASI']) delete e[k];
  return { ...e, WASI: path.join(t, 'wasi'), OBJCACHE: path.join(t, 'objcache'), J: '2', ...extra };
};
const sh = (t, script, args = [], extra = {}) => {
  const r = spawnSync('bash', [path.join(t, script), ...args], { cwd: t, encoding: 'utf8', timeout: 60000, env: envFor(t, extra) });
  return { code: r.status, out: (r.stdout ?? '') + (r.stderr ?? '') };
};
/** the objects of the clang.log line that wrote <out> (the link), relative to the tree */
const linked = (t, out) => {
  const line = read(t, 'clang.log').split('\n').filter(l => l.includes(` -o ${path.join(t, out)} `) || l.includes(` -o ${out} `)).pop();
  assert.ok(line, `no link of ${out} in clang.log`);
  return line.split(' ').filter(a => a.endsWith('.o')).map(a => path.relative(t, path.resolve(t, a)));
};

/** a lift for build.sh: gen folder g (gen, gen/mw) with all/ and server/, and d3dx/ when d3dx */
function lift(t, g, { d3dx = false } = {}) {
  const mods = { all: ['shard_0000.c', 'shard_0001.c', 'fntab.c', 'impargs.c'], server: ['shard_0000.c', 'fntab.c', 'impargs.c', 'modinfo.c'] };
  if (d3dx) mods.d3dx = ['shard_0000.c', 'fntab.c', 'impargs.c', 'modinfo.c'];
  for (const [m, files] of Object.entries(mods)) {
    for (const f of files) w(t, `${g}/${m}/${f}`, `/* ${m} ${f} */\n`);
    w(t, `${g}/${m}/decls.h`); w(t, `${g}/${m}/stamp.txt`, STAMP);
  }
  w(t, `${g}/all/web-lift.txt`, 'FLAGABI X87OPT\n');
  w(t, `${g}/all/gameinfo.h`, '#define GAMEINFO_TEXT_BASE 0x00401000u\n');
}
/** build.sh's tree: the script, game.conf (with D3DX_BASE when given) and tools/gameconf.sh, stand-ins for
 *  aliasguard and gencheck, a runtime of one core file, one UG2 feature file and one runtime/web file */
function buildTree({ game = 'ug2', d3dxBase } = {}) {
  const t = tree();
  w(t, 'web/build.sh', read(ROOT, 'web/build.sh'), 0o755);
  w(t, 'tools/gameconf.sh', read(ROOT, 'tools/gameconf.sh'));
  for (const id of ['ug2', 'mw']) {
    let conf = read(ROOT, `games/${id}/game.conf`);
    if (id === game && d3dxBase !== undefined) conf = conf.replace(/^D3DX_BASE=.*$/m, `D3DX_BASE=${d3dxBase}`);
    w(t, `games/${id}/game.conf`, conf);
  }
  for (const s of ['aliasguard', 'gencheck']) w(t, `web/tools/${s}.sh`, '#!/bin/bash\nexit 0\n', 0o755);
  w(t, 'runtime/core.list', 'mem.c   # the core\n');
  for (const f of ['runtime/mem.c', 'runtime/game_ug2.c', 'runtime/web/main_web.c', 'runtime/cpu.h', 'runtime/module.h', 'runtime/web/compat/wasm_plat.h']) w(t, f);
  return t;
}

test('build.sh links the objects of the current lift only, and lists them in lift.objs with their lifts\' stamps', () => {
  const t = buildTree();
  lift(t, 'gen');
  // objects an older lift left: a shard this lift has not, and a stubs.c it did not write
  for (const o of ['out/web/obj/shard_0099.o', 'out/web/obj/stubs.o', 'out/web/obj/srv/shard_0005.o']) w(t, o, 'old object');
  const r = sh(t, 'web/build.sh');
  assert.equal(r.code, 0, r.out);
  assert.match(r.out, /link OK: out\/web\/nfsu2\.wasm/);
  const want = ['obj/fntab.o', 'obj/impargs.o', 'obj/shard_0000.o', 'obj/shard_0001.o', 'obj/srv/fntab.o', 'obj/srv/impargs.o', 'obj/srv/modinfo.o', 'obj/srv/shard_0000.o'];
  const list = read(t, 'out/web/lift.objs').split('\n').filter(Boolean);
  assert.deepEqual(list.filter(l => !l.startsWith('#')).sort(), want);
  assert.deepEqual(list.filter(l => l.startsWith('#stamp ')), [`#stamp gen/all ${h16(STAMP)}`, `#stamp gen/server ${h16(STAMP)}`]);
  const objs = linked(t, 'out/web/nfsu2.wasm');
  assert.deepEqual(objs.filter(o => o.startsWith('out/web/obj/')).sort(), want.map(o => `out/web/${o}`));
  assert.ok(objs.includes('out/web/rt/mem.o') && objs.includes('out/web/rt/game_ug2.o') && objs.includes('out/web/rt/web_main_web.o'), objs.join(' '));
  // UG2's json: the fields it always had and the game id, nothing more
  assert.match(read(t, 'out/web/nfsu2.json'), /^\{"pages":16384,"maxPages":65536,"guestEnd":671088640,"rev":"[^"]*","size":7,"opt":"-O3","lift":"FLAGABI X87OPT","game":"ug2"\}\n$/);
  assert.doesNotMatch(r.out, /D3DX oracle/);
});

test('build.sh: a UG2 build with the D3DX oracle linked says "oracle":1 in its json, as another game\'s does', () => {
  const t = buildTree({ d3dxBase: '0x00D80000' });
  lift(t, 'gen', { d3dx: true });
  const r = sh(t, 'web/build.sh');
  assert.equal(r.code, 0, r.out);
  assert.match(read(t, 'out/web/nfsu2.json'), /"game":"ug2","oracle":1\}\n$/);
  assert.match(r.out, /the D3DX oracle \(gen\/d3dx\) is linked/);
  assert.match(read(t, 'out/web/lift.objs'), /^#stamp gen\/d3dx [0-9a-f]{16}$/m);
  assert.ok(linked(t, 'out/web/nfsu2.wasm').includes('out/web/obj/d3dx/fntab.o'));
  // D3DX_ORACLE=0 leaves it out: no oracle objects, no key
  const off = sh(t, 'web/build.sh', [], { D3DX_ORACLE: '0' });
  assert.equal(off.code, 0, off.out);
  assert.match(read(t, 'out/web/nfsu2.json'), /"game":"ug2"\}\n$/);
  assert.doesNotMatch(read(t, 'out/web/lift.objs'), /d3dx/);
});

test('build.sh: another game\'s json has its stamp, and "oracle":1 only with the oracle linked', () => {
  const t = buildTree({ game: 'mw' });
  lift(t, 'gen/mw', { d3dx: true });
  const on = sh(t, 'web/build.sh', [], { GAME: 'mw' });
  assert.equal(on.code, 0, on.out);
  assert.match(read(t, 'out/web/mw/nfsmw.json'), /"game":"mw","stamp":"overrides 0 \(0\); hooks 0 \(0\); lifter 0","oracle":1\}\n$/);
  assert.match(read(t, 'out/web/mw/lift.objs'), /^# web\/build\.sh: the lifted objects its link takes, under out\/web\/mw$/m);
  const off = sh(t, 'web/build.sh', [], { GAME: 'mw', D3DX_ORACLE: '0' });
  assert.equal(off.code, 0, off.out);
  assert.match(read(t, 'out/web/mw/nfsmw.json'), /"game":"mw","stamp":"[^"]*"\}\n$/);
});

/** corelink's tree: a build in out/web whose lift.objs names obj/shard_0000.o and obj/srv/fntab.o (with an
 *  object of an older lift beside them), runtime sources older than the list */
function corelinkTree() {
  const t = tree();
  w(t, 'web/tools/corelink.sh', read(ROOT, 'web/tools/corelink.sh'), 0o755);
  w(t, 'tools/genstamp.sh', '#!/bin/bash\n[ "$1" = overrides ] && echo "401000,402000"\n', 0o755);
  w(t, 'runtime/core.list', 'mem.c\n');
  for (const f of ['runtime/mem.c', 'runtime/cpu.h', 'runtime/web/main_web.c', 'runtime/web/compat/wasm_plat.h']) w(t, f);
  for (const d of ['all', 'server']) w(t, `gen/${d}/stamp.txt`, STAMP);
  for (const o of ['rt/mem.o', 'rt/game_ug2.o', 'rt/web_main_web.o', 'obj/shard_0000.o', 'obj/srv/fntab.o', 'obj/shard_0099.o']) w(t, `out/web/${o}`, 'object');
  w(t, 'out/web/lift.objs', `# web/build.sh: the lifted objects its link takes, under out/web\n#stamp gen/all ${h16(STAMP)}\n#stamp gen/server ${h16(STAMP)}\nobj/shard_0000.o\nobj/srv/fntab.o\n`);
  const past = new Date(Date.now() - 3600e3);
  for (const f of ['runtime/core.list', 'runtime/mem.c', 'runtime/cpu.h', 'runtime/web/main_web.c', 'runtime/web/compat/wasm_plat.h']) fs.utimesSync(path.join(t, f), past, past);
  return t;
}

test('corelink.sh links lift.objs\' objects (never an older lift\'s), the core files and runtime/web, not the game\'s', () => {
  const t = corelinkTree();
  const r = sh(t, 'web/tools/corelink.sh');
  assert.equal(r.code, 0, r.out);
  assert.match(r.out, /corelink: OK, no undefined symbol/);
  assert.match(r.out, /left out: game_ug2 /);
  assert.deepEqual(linked(t, 'out/web/corelink/core.wasm').sort(),
    ['out/web/corelink/overrides_stub.o', 'out/web/obj/shard_0000.o', 'out/web/obj/srv/fntab.o', 'out/web/rt/mem.o', 'out/web/rt/web_main_web.o']);
});

test('corelink.sh prints the link\'s errors on failure, a duplicate symbol and its objects too', () => {
  const t = corelinkTree();
  w(t, 'link-error.txt', [
    'wasm-ld: error: duplicate symbol: sub_00401000', '>>> defined in out/web/obj/shard_0000.o', '>>> defined in out/web/obj/shard_0099.o',
    'wasm-ld: error: out/web/rt/mem.o: undefined symbol: game_hook', 'wasm-ld: error: out/web/rt/mem.o: undefined symbol: game_hook',
    'clang: error: linker command failed with exit code 1 (use -v to see invocation)', ''].join('\n'));
  const r = sh(t, 'web/tools/corelink.sh');
  assert.equal(r.code, 1, r.out);
  assert.match(r.out, /corelink: FAILED, 2 undefined symbols/);
  assert.match(r.out, /^\s+2 undefined symbol: game_hook$/m);
  assert.match(r.out, /^wasm-ld: error: duplicate symbol: sub_00401000\n>>> defined in out\/web\/obj\/shard_0000\.o\n>>> defined in out\/web\/obj\/shard_0099\.o$/m);
  assert.match(r.out, /^clang: error: linker command failed/m);
});

test('corelink.sh refuses a build older than the lift or the runtime sources, or one that is not there', () => {
  let t = corelinkTree();
  // a new lift since web/build.sh ran: its stamp has another date line
  w(t, 'gen/all/stamp.txt', STAMP.replace('T00:00:00Z', 'T01:00:00Z'));
  let r = sh(t, 'web/tools/corelink.sh');
  assert.equal(r.code, 2, r.out);
  assert.match(r.out, /older than the lift in gen\/all .*run web\/build\.sh/);
  assert.doesNotMatch(r.out, /OK/);
  t = corelinkTree(); fs.rmSync(path.join(t, 'gen/server/stamp.txt'));
  r = sh(t, 'web/tools/corelink.sh');
  assert.equal(r.code, 2, r.out); assert.match(r.out, /older than the lift in gen\/server/);
  for (const f of ['runtime/mem.c', 'runtime/cpu.h', 'runtime/web/compat/wasm_plat.h', 'runtime/core.list']) {
    t = corelinkTree(); fs.utimesSync(path.join(t, f), new Date(), new Date(Date.now() + 5000));
    r = sh(t, 'web/tools/corelink.sh');
    assert.equal(r.code, 2, `${f}: ${r.out}`); assert.match(r.out, new RegExp(`older than ${f.replace(/\./g, '\\.')} `));
  }
  // another game's runtime folder is not UG2's build
  t = corelinkTree(); w(t, 'runtime/mw/x.c');
  r = sh(t, 'web/tools/corelink.sh'); assert.equal(r.code, 0, r.out);
  t = corelinkTree(); fs.rmSync(path.join(t, 'out/web/obj/srv/fntab.o'));
  r = sh(t, 'web/tools/corelink.sh');
  assert.equal(r.code, 2, r.out); assert.match(r.out, /out\/web\/obj\/srv\/fntab\.o missing/);
  t = corelinkTree(); fs.rmSync(path.join(t, 'out/web/lift.objs'));
  r = sh(t, 'web/tools/corelink.sh');
  assert.equal(r.code, 2, r.out); assert.match(r.out, /no build in out\/web \(run web\/build\.sh\)/);
});

test('main_all.sh links lift.objs\' objects, never an older lift\'s, and needs the list', () => {
  const t = corelinkTree();
  w(t, 'web/tools/main_all.sh', read(ROOT, 'web/tools/main_all.sh'), 0o755);
  w(t, 'web/tools/run-page.mjs', "console.log('[1] EXIT 0');\n");
  w(t, 'tests/main_all.c'); w(t, 'gen/all/web-lift.txt', 'FLAGABI\n');
  // no server answers there: curl's 000 is not the 404 of a server without --dev
  const r = sh(t, 'web/tools/main_all.sh', ['http://127.0.0.1:1']);
  assert.equal(r.code, 0, r.out);
  assert.deepEqual(linked(t, 'web/dist-dev/test/nfsu2_all.wasm').sort(),
    ['out/web/obj/shard_0000.o', 'out/web/obj/srv/fntab.o', 'out/web/rt/game_ug2.o', 'out/web/rt/mem.o', 'out/web/test/main_all.o']);
  fs.rmSync(path.join(t, 'out/web/lift.objs'));
  const none = sh(t, 'web/tools/main_all.sh', ['http://127.0.0.1:1']);
  assert.equal(none.code, 1, none.out); assert.match(none.out, /run web\/build\.sh first/);
});
