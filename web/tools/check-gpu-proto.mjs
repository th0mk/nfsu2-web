// Checks that web/src/gpu/proto.ts matches runtime/web/gpu_proto.h: compiles a
// small C program against the header with the wasm toolchain (wasm32 layout),
// runs it under node:wasi, and compares every field offset and size and the
// protocol's constants with the TypeScript ones.  Also checks the group 1
// layouts of web/src/gpu/shader-abi.ts (docs/web/gpu-abi.md sections 2 and
// 11): UG2's two exactly as before version 6, and the 8-stage ones with cube
// and depth-compare stages.  Exit 1 on any difference.
//   node web/tools/check-gpu-proto.mjs
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { WASI } from 'node:wasi';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const wasi = fs.readdirSync(path.join(ROOT, 'deps')).filter(d => d.startsWith('wasi-sdk-')).sort().pop();
if (!wasi) { console.error('wasi-sdk not found in deps/'); process.exit(2); }
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'gwproto-'));
const fields = {
  GwDraw: ['first', 'count', 'tex', 'target', 'kind', 'blend', 'aref', 'depth', 'tex1', 'tex2', 'tex3', 'ps', 'psci', 'vsid', 'rawoff', 'koff',
    'proj', 'smp', 'blit_src', 'bcol', 'cull', 'fogv', 'clr_argb', 'clr_flags', 'clr_z', 'ksz', 'ix_isz', 'ix_ib', 'ix_base', 'clip',
    'tex4', 'tex5', 'tex6', 'tex7', 'smphi'],
  GwFrame: ['seq', 'dn', 'vn', 'rvn', 'kn', 'pscn', 'draws', 'tv', 'raw', 'k', 'psc', 'rq_end', 'bb_w', 'bb_h', 'clear_argb', 'pool_size', 'dump', 'dump_frame', 'dump_label',
    'rb_id', 'rb_tex', 'rb_dst', 'rb_pitch', 'rb_rows'],
  GwCtl: ['magic', 'version', 'layout', 'size', 'state', 'bell', 'attached', 'rq_head', 'rq_tail', 'rq_ptr', 'rq_cap', 'frames_done', 'view_w', 'view_h',
    'dpr1000', 'features', 'st_draws', 'st_passes', 'st_enc_us', 'st_errors', 'dumps_done', 'st_pipelines', 'rb_want', 'rb_done', 'stages', 'frame'],
  GwVsync: ['tick', 'seq', 'ts_ms', 'period_ms', 'hz', 'visible'],
};
let c = '#include <stdio.h>\n#include "gpu_proto.h"\nint main(void){\n';
for (const [s, fl] of Object.entries(fields)) {
  c += `printf("${s} sizeof %u\\n", (unsigned)sizeof(${s}));\n`;
  for (const f of fl) c += `printf("${s} ${f} %u\\n", (unsigned)offsetof(${s}, ${f}));\n`;
}
c += 'printf("RB_FLAG %u\\n", (unsigned)GW_RB_FLAG);\n';
c += 'printf("LAYOUT %u\\n", (unsigned)GW_LAYOUT);\nprintf("ATTACHED %u\\n", (unsigned)GW_ATTACHED);\nprintf("DETACHED %u\\n", (unsigned)GW_DETACHED);\n';
c += 'printf("K_HL %u\\n", (unsigned)GW_K_HL);\nprintf("HL_BYTES %u\\n", (unsigned)sizeof(GwHL));\nprintf("HL_MAX %u\\n", (unsigned)GW_HL_MAX);\nprintf("KWIN_ROWS %u\\n", (unsigned)GW_KWIN_ROWS);\n';
// the protocol's constants: name in the printout -> C expression
const consts = {
  MAGIC: 'GW_MAGIC', VERSION: 'GW_VERSION', NBUF: 'GW_NBUF', CMD_HDR: 'GW_CMD_HDR',
  KIND_DRAW: 'GW_DRAW', KIND_CLEAR: 'GW_CLEAR', KIND_BLIT: 'GW_BLIT',
  OP_PAD: 'GW_PAD', OP_TEX_CREATE: 'GW_TEX_CREATE', OP_TEX_UPLOAD: 'GW_TEX_UPLOAD', OP_TEX_FREE: 'GW_TEX_FREE',
  OP_POOL_GROW: 'GW_POOL_GROW', OP_POOL_WRITE: 'GW_POOL_WRITE', OP_SHADER_VS: 'GW_SHADER_VS', OP_SHADER_PS: 'GW_SHADER_PS',
  OP_Z_FORGET: 'GW_Z_FORGET', TF_RT: 'GW_TF_RT', TF_CUBE: 'GW_TF_CUBE', TF_DEPTH: 'GW_TF_DEPTH',
  FEAT_BC: 'GW_FEAT_BC', FEAT_SWIZZLE: 'GW_FEAT_SWIZZLE',
  Z_TEX: 'GW_Z_TEX', PS_DEPTH_SHIFT: 'GW_PS_DEPTH_SHIFT', STAGES_MAX: 'GW_STAGES_MAX',
  BK_2D: 'GW_BK_2D', BK_CUBE: 'GW_BK_CUBE', BK_DEPTH: 'GW_BK_DEPTH',
};
// GW_BK over a kinds word with every value at some stage: stage k of KINDS_WORD
const KINDS_WORD = 0x9E24;   // stages 0..7: 0, 1, 2, 0, 2, 3, 1, 2
for (const [n, e] of Object.entries(consts)) c += `printf("${n} %u\\n", (unsigned)(${e}));\n`;
for (let k = 0; k < 8; k++) c += `printf("BK${k} %u\\n", (unsigned)GW_BK(${KINDS_WORD}u, ${k}u));\n`;
c += 'printf("K_MARKS %u\\n", (unsigned)GW_K_MARKS);\nprintf("MARKS_BYTES %u\\n", (unsigned)sizeof(GwMarks));\nprintf("MARKS_MAX %u\\n", (unsigned)GW_MARKS_MAX);\nprintf("MARKS_MAGIC %u\\n", (unsigned)GW_MARKS_MAGIC);\nreturn 0; }\n';
fs.writeFileSync(path.join(tmp, 'p.c'), c);
execFileSync(path.join(ROOT, 'deps', wasi, 'bin/clang'), ['--target=wasm32-wasip1', '-O1', '-I', path.join(ROOT, 'runtime/web'), '-o', path.join(tmp, 'p.wasm'), path.join(tmp, 'p.c')]);
let out = '';
const w = new WASI({ version: 'preview1', args: [], env: {}, stdout: fs.openSync(path.join(tmp, 'out.txt'), 'w') });
const mod = await WebAssembly.compile(fs.readFileSync(path.join(tmp, 'p.wasm')));
w.start(await WebAssembly.instantiate(mod, w.getImportObject()));
out = fs.readFileSync(path.join(tmp, 'out.txt'), 'utf8');
const C = {};
for (const line of out.trim().split('\n')) { const p = line.split(' '); C[p.slice(0, -1).join(' ')] = +p[p.length - 1]; }

const js = (await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/gpu/proto.ts')], bundle: true, format: 'esm', write: false })).outputFiles[0].text;
fs.writeFileSync(path.join(tmp, 'proto.mjs'), js);
const T = await import(path.join(tmp, 'proto.mjs'));
const snake = s => s.replace(/_([a-z])/g, (_, x) => x.toUpperCase());
let bad = 0, checks = 0;
const eq = (what, a, b, la = 'C', lb = 'TS') => { checks++; if (a !== b) { bad++; console.log(`MISMATCH ${what}: ${la} ${a} vs ${lb} ${b}`); } };
eq('GwDraw size', C['GwDraw sizeof'], T.DRAW_BYTES); eq('GwDraw words', C['GwDraw sizeof'] / 4, T.DRAW_WORDS);
for (const f of fields.GwDraw) eq(`GwDraw.${f}`, C[`GwDraw ${f}`] / 4, T.D[f === 'blit_src' ? 'blitSrc' : f === 'smphi' ? 'smpHi' : snake(f)]);
eq('GwFrame size', C['GwFrame sizeof'], T.FRAME_BYTES);
for (const f of fields.GwFrame) eq(`GwFrame.${f}`, C[`GwFrame ${f}`] / 4, T.F[f === 'bb_w' ? 'bbW' : f === 'bb_h' ? 'bbH' : snake(f)]);
eq('GwCtl size', C['GwCtl sizeof'], T.CTL_BYTES);
for (const f of fields.GwCtl) eq(`GwCtl.${f}`, C[`GwCtl ${f}`] / 4, T.C[f === 'st_enc_us' ? 'stEncUs' : snake(f)]);
eq('GwVsync.tick', C['GwVsync tick'] / 4, T.V.tick); eq('GwVsync.seq', C['GwVsync seq'] / 4, T.V.seq);
eq('GwVsync.ts_ms', C['GwVsync ts_ms'] / 8, T.V.tsF64); eq('GwVsync.period_ms', C['GwVsync period_ms'] / 8, T.V.periodF64);
eq('GwVsync.hz', C['GwVsync hz'] / 4, T.V.hz); eq('GwVsync.visible', C['GwVsync visible'] / 4, T.V.visible);
eq('layout word', C['LAYOUT'], T.LAYOUT);
eq('GW_RB_FLAG', C['RB_FLAG'], T.RB_FLAG);
eq('GW_ATTACHED', C['ATTACHED'], T.ATTACHED); eq('GW_DETACHED', C['DETACHED'], T.DETACHED);
eq('GW_K_HL', C['K_HL'], T.K_HL); eq('GwHL size', C['HL_BYTES'], T.HL_BYTES); eq('GW_HL_MAX', C['HL_MAX'], T.HL_MAX);
eq('GW_KWIN_ROWS', C['KWIN_ROWS'], T.KWIN_ROWS);
eq('GW_K_MARKS', C['K_MARKS'], T.K_MARKS); eq('GwMarks size', C['MARKS_BYTES'], T.MARKS_BYTES); eq('GW_MARKS_MAX', C['MARKS_MAX'], T.MARKS_MAX);
eq('GW_MARKS_MAGIC', C['MARKS_MAGIC'], T.MARKS_MAGIC);
eq('GW_MAGIC', C['MAGIC'], T.GW_MAGIC); eq('GW_VERSION', C['VERSION'], T.GW_VERSION); eq('GW_NBUF', C['NBUF'], T.GW_NBUF);
eq('GW_CMD_HDR', C['CMD_HDR'], T.CMD_HDR);
eq('GW_DRAW', C['KIND_DRAW'], T.Kind.Draw); eq('GW_CLEAR', C['KIND_CLEAR'], T.Kind.Clear); eq('GW_BLIT', C['KIND_BLIT'], T.Kind.Blit);
for (const [c, t] of [['PAD', 'Pad'], ['TEX_CREATE', 'TexCreate'], ['TEX_UPLOAD', 'TexUpload'], ['TEX_FREE', 'TexFree'], ['POOL_GROW', 'PoolGrow'],
  ['POOL_WRITE', 'PoolWrite'], ['SHADER_VS', 'ShaderVs'], ['SHADER_PS', 'ShaderPs'], ['Z_FORGET', 'ZForget']]) eq(`GW_${c}`, C[`OP_${c}`], T.Op[t]);
for (const n of ['TF_RT', 'TF_CUBE', 'TF_DEPTH', 'FEAT_BC', 'FEAT_SWIZZLE', 'Z_TEX', 'PS_DEPTH_SHIFT', 'STAGES_MAX', 'BK_2D', 'BK_CUBE', 'BK_DEPTH'])
  eq(`GW_${n}`, C[n], T[n]);

// shader-abi.ts: the group 1 layouts (gpu-abi.md sections 2 and 11)
const ajs = (await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/gpu/shader-abi.ts')], bundle: true, format: 'esm', write: false })).outputFiles[0].text;
fs.writeFileSync(path.join(tmp, 'abi.mjs'), ajs);
const A = await import(path.join(tmp, 'abi.mjs'));
const F = 2;   // GPUShaderStage.FRAGMENT
const same = (what, a, b) => eq(what, JSON.stringify(a), JSON.stringify(b), 'shader-abi.ts', 'contract');
for (let k = 0; k < 8; k++) eq(`GW_BK(0x${KINDS_WORD.toString(16)}, ${k})`, C[`BK${k}`], A.stageKind(KINDS_WORD, k));
// UG2's two layouts as built before version 6: tex0..3 at 0..3 (tex0 a cube in the cube twin's), filtering smp0..3 at 4..7
const ug2 = cube => [0, 1, 2, 3].map(k => ({ binding: k, visibility: F, texture: { sampleType: 'float', viewDimension: cube && k === 0 ? 'cube' : '2d' } }))
  .concat([0, 1, 2, 3].map(k => ({ binding: 4 + k, visibility: F, sampler: { type: 'filtering' } })));
same('group 1 layout L2D', A.group1Entries(4, 0, false, F), ug2(false));
same('group 1 layout LCUBE', A.group1Entries(4, 0, true, F), ug2(true));
eq('VOut locations with 8 stages', A.VOUT_LOCATIONS_8, A.VOUT_LOCATIONS + 4, 'shader-abi.ts', 'contract');
eq('module ids below the depth bits (shader + cube twin)', 1 << T.PS_DEPTH_SHIFT, 2 * A.PSCUBE);
// 8 stages: 2D, cube, depth, 2D, depth, 2D, cube, depth
const kinds8 = [0, 1, 2, 0, 2, 0, 1, 2], word8 = kinds8.reduce((w, v, k) => w | v << (2 * k), 0);
const e8 = A.group1Entries(8, word8, false, F);
eq('8-stage entries', e8?.length, 16, 'shader-abi.ts', 'contract');
same('8-stage bindings in order', e8?.map(e => e.binding), [...Array(16).keys()]);
for (let k = 0; k < 8; k++) {
  const t = e8?.find(e => e.binding === A.texBinding(k)), s = e8?.find(e => e.binding === A.smpBinding(k));
  same(`stage ${k} texture`, t?.texture, kinds8[k] === 2 ? { sampleType: 'depth', viewDimension: '2d' } : { sampleType: 'float', viewDimension: kinds8[k] === 1 ? 'cube' : '2d' });
  same(`stage ${k} sampler`, s?.sampler, { type: kinds8[k] === 2 ? 'comparison' : 'filtering' });
  if (k < 4) { same(`stage ${k} texture binding`, A.texBinding(k), k); same(`stage ${k} sampler binding`, A.smpBinding(k), 4 + k); }
}
same('cube twin with 8 stages: stage 0 a cube', A.group1Entries(8, 0, true, F)?.[0]?.texture, { sampleType: 'float', viewDimension: 'cube' });
same('kind 3 (not defined) gives no layout', A.group1Entries(8, KINDS_WORD, false, F), null);
fs.rmSync(tmp, { recursive: true, force: true });
console.log(bad ? `${bad} mismatches` : `gpu proto OK: ${checks} checks of ${Object.keys(C).length} values pass (layout ${C.LAYOUT.toString(16)})`);
process.exit(bad ? 1 : 0);
