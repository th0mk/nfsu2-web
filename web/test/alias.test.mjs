// Guest memory type punning on WebAssembly:
//   node --test web/test/alias.test.mjs        (needs wasi-sdk in deps/)
// 1. web/tools/aliasguard.sh: every guest access macro of runtime/cpu.h's wasm
//    branches keeps its may_alias type, and the guard itself fails on a cpu.h
//    with MR16/MR64/MRF64 back to plain types (the glow review's experiment,
//    which tests/web/aliastest.c alone passed);
// 2. web/tools/aliastest.sh: the lifted-style vector normalize in V8;
// 3. web/tools/aliasguard.sh --lift: the game's lifted C (GAME, default ug2)
//    reaches guest memory only through those macros and byte copies, and the
//    guard fails on lifted C with a typed access of its own.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const hasWasi = fs.readdirSync(path.join(REPO, 'deps')).some(d => d.startsWith('wasi-sdk-'));
const sh = (script, env = {}, args = []) => spawnSync('bash', [path.join(REPO, script), ...args], { cwd: REPO, encoding: 'utf8', env: { ...process.env, ...env } });

test('aliasguard: the guest access macros keep their may_alias types', { skip: !hasWasi && 'no wasi-sdk in deps/' }, () => {
  const r = sh('web/tools/aliasguard.sh');
  console.log(r.stdout.trim());
  assert.equal(r.status, 0, r.stdout + r.stderr);
  // a cpu.h whose 16- and 64-bit accesses lost the type: the guard refuses it
  const dir = fs.mkdtempSync(path.join(REPO, 'out/web/aliasguard-'));
  try {
    const h = fs.readFileSync(path.join(REPO, 'runtime/cpu.h'), 'utf8')
      .replace(/\(gm_u16 +\*\)/g, '(uint16_t *)').replace(/\(gm_u64 +\*\)/g, '(uint64_t *)').replace(/\(gm_f64 +\*\)/g, '(double *)');
    fs.writeFileSync(path.join(dir, 'cpu.h'), h);
    const bad = sh('web/tools/aliasguard.sh', { CPU_H: dir });
    console.log(bad.stdout.trim().split('\n').slice(0, 3).join('\n'));
    assert.notEqual(bad.status, 0, 'the guard fails on plain-typed MR16/MR64/MRF64');
    assert.match(bad.stdout, /MR16 does not access guest memory as gm_u16/);
    assert.match(bad.stdout, /MRF64 \(-DNFSU2_MEMCHECK\) does not access guest memory as gm_f64/);
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});

test('aliastest: an f32 store then an i32 load of the same guest address stay in order', { skip: !hasWasi && 'no wasi-sdk in deps/' }, () => {
  const r = sh('web/tools/aliastest.sh');
  console.log(r.stdout.trim().split('\n').slice(-2).join('\n'));
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.match(r.stdout, /alias test: passed/);
});

const conf = Object.fromEntries(fs.readFileSync(path.join(REPO, 'games', process.env.GAME || 'ug2', 'game.conf'), 'utf8')
  .split('\n').filter(l => /^[A-Z0-9_]+=/.test(l)).map(l => [l.slice(0, l.indexOf('=')), l.slice(l.indexOf('=') + 1)]));
const LIFTED = path.join(REPO, conf.GEN_DIR, 'all');
test('aliasguard --lift: the lifted C reaches guest memory through the macros and byte copies only',
  { skip: (!hasWasi && 'no wasi-sdk in deps/') || (!fs.existsSync(path.join(LIFTED, 'fntab.c')) && `no lift in ${conf.GEN_DIR}/all`) }, () => {
  const r = sh('web/tools/aliasguard.sh', {}, ['--lift']);
  console.log(r.stdout.trim());
  assert.equal(r.status, 0, r.stdout + r.stderr);
  assert.match(r.stdout, new RegExp(`${conf.GEN_DIR}/all: \\d+ files, \\d+ uses of the guest base, all in byte copies`));
  // a lifted function with a typed access of its own, one keeping the base in a pointer, and one on a line longer
  // than the report shows (as lifted lines are; its report still ends its line): refused
  const dir = fs.mkdtempSync(path.join(REPO, 'out/web/aliasguard-'));
  try {
    fs.writeFileSync(path.join(dir, 'shard_0000.c'), '#include "cpu.h"\n' +
      'void sub_00401000(CPU *c){ memcpy(&c->xmm[0], g_mem + (uint32_t)(c->eax), 16); }\n' +
      'void sub_00401010(CPU *c){ *(float *)(g_mem + (uint32_t)(c->eax)) = 1.0f; }\n' +
      'void sub_00401020(CPU *c){ uint8_t *p = g_mem + c->eax; memcpy(p, &c->ebx, 4); }\n' +
      `void sub_00401030(CPU *c){ ${'c->ecx += 1; '.repeat(14)}*(uint32_t *)(g_mem + (uint32_t)(c->eax)) = c->ecx; }\n`);
    const bad = sh('web/tools/aliasguard.sh', { LIFT_DIR: dir }, ['--lift']);
    console.log(bad.stdout.trim().split('\n').slice(-3).join('\n'));
    assert.notEqual(bad.status, 0, 'the guard fails on a typed access outside the macros');
    assert.match(bad.stdout, /shard_0000\.c:3: guest base outside a byte copy/);
    assert.match(bad.stdout, /shard_0000\.c:4: guest base outside a byte copy/);
    assert.match(bad.stdout, /^aliasguard: \S+shard_0000\.c:5: guest base outside a byte copy: void sub_00401030[^\n#]{100,}$/m);
    assert.match(bad.stdout, /: 3 uses of the guest base outside a byte copy/);
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});
