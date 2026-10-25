// The guest layout's boot check (runtime/memmap.c, runtime/memmap.h) built for
// wasm with the game's maps, UG2's (the default) and MW's (-DNFSU2_GAME_MW),
// and run on images of both games' sizes:
//   node --test web/test/memmap.test.mjs        (needs wasi-sdk in deps/)
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const WASI = fs.readdirSync(path.join(REPO, 'deps')).filter(d => d.startsWith('wasi-sdk-')).map(d => path.join(REPO, 'deps', d)).pop();
const CC = WASI && path.join(WASI, 'bin/clang');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
const UG2_IMAGE = ['0x00400000', '0x00932000'], MW_IMAGE = ['0x00400000', '0x00a79000'];

/** memmap_unit.wasm for one map and one lifted server.dll */
function build(name, defs) {
  const w = path.join(OUT, `memmap_unit_${name}.wasm`);
  execFileSync(CC, ['--target=wasm32-wasip1-threads', '-pthread', '-O2', '-include', 'runtime/web/compat/wasm_plat.h',
    '-Iruntime/web/compat', '-Iruntime', ...defs, '-o', w, 'web/test/memmap_unit.c', 'runtime/memmap.c',
    '-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=67108864,--max-memory=268435456'],
  { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  return w;
}
const run = (w, ...args) => {
  const r = spawnSync(process.execPath, [path.join(REPO, 'web/test/wasi-run.mjs'), w, ...args], { encoding: 'utf8' });
  return { status: r.status, out: r.stdout + r.stderr };
};
const skip = !CC && 'no wasi-sdk in deps/';

test('memmap: UG2\'s map holds for UG2\'s image and logs one [mem] line', { skip }, () => {
  const w = build('ug2', ['-DSRV_BASE=0x00a00000u', '-DSRV_SIZE=0x58000u']);
  const r = run(w, ...UG2_IMAGE);
  assert.equal(r.status, 0, r.out);
  const mem = r.out.split('\n').filter(l => l.startsWith('[mem]'));
  assert.equal(mem.length, 1, r.out);
  assert.equal(mem[0], '[mem] ug2: image 0x00400000-0x00932000, server.dll 0x00a00000-0x00a58000, winsock buffers 0x00be0000-0x00be8000, ' +
    'TEBs 0x00c00000-0x00d00000, modheap 0x01000000-0x05000000, main stack 0x05000000-0x05ff0000, thread stacks 0x06000000-0x08000000, ' +
    'heap 0x08000000-0x28000000; handle values: files 0x00900000-0x00900100, objects 0x00a00000-0x00b00000, ' +
    'windows 0x00b00000-0x00b20002; no overlap (handle values may lie on memory)');
});

test('memmap: an overlap stops the run and names both ranges', { skip }, () => {
  // MW's image under UG2's map: it runs over UG2's server.dll slot
  const w = build('ug2', ['-DSRV_BASE=0x00a00000u', '-DSRV_SIZE=0x58000u']);
  const r = run(w, ...MW_IMAGE);
  assert.equal(r.status, 1, r.out);
  assert.match(r.out, /\[mem\] LAYOUT BROKEN: memory image 0x00400000-0x00a79000 and server\.dll 0x00a00000-0x00a58000 overlap/);
  assert.doesNotMatch(r.out, /^\[mem\] ug2:/m);
  // fatal, as win32_init calls it: the first overlap ends the run
  const f = run(w, ...MW_IMAGE, '1');
  assert.equal(f.status, 3, f.out);
  assert.equal(f.out.match(/LAYOUT BROKEN/g).length, 1, f.out);
  assert.doesNotMatch(f.out, /memmap_unit:/);
  // an image reaching into the TEBs
  assert.match(run(w, '0x00400000', '0x00c00001').out, /image 0x00400000-0x00c00001 and TEBs 0x00c00000-0x00d00000 overlap/);
});

test('memmap: a server.dll lifted outside its slot stops the run', { skip }, () => {
  const w = build('ug2-srv-mw', ['-DSRV_BASE=0x00d00000u', '-DSRV_SIZE=0x6f000u']);
  const r = run(w, ...UG2_IMAGE);
  assert.equal(r.status, 1, r.out);
  assert.match(r.out, /server\.dll was lifted at 0x00d00000-0x00d6f000, outside its slot 0x00a00000-0x00a58000 \(runtime\/memmap\.h, ug2\)/);
});

test('memmap: MW\'s map holds for MW\'s image, with no handle value on memory', { skip }, () => {
  const w = build('mw', ['-DNFSU2_GAME_MW', '-DSRV_BASE=0x00d00000u', '-DSRV_SIZE=0x6f000u']);
  const r = run(w, ...MW_IMAGE);
  assert.equal(r.status, 0, r.out);
  assert.match(r.out, /^\[mem\] mw: image 0x00400000-0x00a79000, server\.dll 0x00d00000-0x00d6f000, d3dx9_26\.dll 0x00d80000-0x00fca000, /m);
  assert.match(r.out, /; handle values: files 0x00a80000-0x00a80100, objects 0x00a90000-0x00b00000, windows 0x00b00000-0x00b20002; no overlap$/m);
  // MW keeps handle values off guest memory: an image one page longer would hold the file handles
  const b = run(w, '0x00400000', '0x00a81000');
  assert.equal(b.status, 1, b.out);
  assert.match(b.out, /handle values on memory: files 0x00a80000-0x00a80100 and image 0x00400000-0x00a81000 overlap/);
  // UG2's server.dll base is inside MW's image
  const s = build('mw-srv-ug2', ['-DNFSU2_GAME_MW', '-DSRV_BASE=0x00a00000u', '-DSRV_SIZE=0x58000u']);
  assert.match(run(s, ...MW_IMAGE).out, /server\.dll was lifted at 0x00a00000-0x00a58000, outside its slot 0x00d00000-0x00d6f000 \(runtime\/memmap\.h, mw\)/);
});

test('memmap: each lifted DLL\'s heaps lie in the modheap region, apart from the other DLLs\' heaps', { skip }, () => {
  // MW's map: server.dll and the D3DX oracle get half of the region each; it holds as it is (test above)
  const o = build('mw-heaps-overlap', ['-DNFSU2_GAME_MW', '-DSRV_BASE=0x00d00000u', '-DSRV_SIZE=0x6f000u', '-DD3DX_HEAP_BASE=0x01800000u']);
  const r = run(o, ...MW_IMAGE);
  assert.equal(r.status, 1, r.out);
  assert.match(r.out, /\[mem\] LAYOUT BROKEN: module heaps server\.dll 0x01000000-0x03000000 and d3dx9_26\.dll 0x01800000-0x03800000 overlap \(runtime\/memmap\.h, mw\)/);
  const x = build('mw-heaps-outside', ['-DNFSU2_GAME_MW', '-DSRV_BASE=0x00d00000u', '-DSRV_SIZE=0x6f000u', '-DD3DX_HEAP_BASE=0x04000000u']);
  const s = run(x, ...MW_IMAGE, '1');
  assert.equal(s.status, 3, s.out);
  assert.match(s.out, /\[mem\] LAYOUT BROKEN: d3dx9_26\.dll heaps 0x04000000-0x06000000, outside modheap 0x01000000-0x05000000 \(runtime\/memmap\.h, mw\)/);
});
