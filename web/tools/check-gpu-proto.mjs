// Checks that web/src/gpu/proto.ts matches runtime/web/gpu_proto.h: compiles a
// small C program against the header with the wasm toolchain (wasm32 layout),
// runs it under node:wasi, and compares every field offset and size with the
// TypeScript constants.  Exit 1 on any difference.
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
    'proj', 'smp', 'blit_src', 'bcol', 'cull', 'fogv', 'clr_argb', 'clr_flags', 'clr_z', 'ksz', 'ix_isz', 'ix_ib', 'ix_base'],
  GwFrame: ['seq', 'dn', 'vn', 'rvn', 'kn', 'pscn', 'draws', 'tv', 'raw', 'k', 'psc', 'rq_end', 'bb_w', 'bb_h', 'clear_argb', 'pool_size', 'dump', 'dump_frame', 'dump_label',
    'rb_id', 'rb_tex', 'rb_dst', 'rb_pitch', 'rb_rows'],
  GwCtl: ['magic', 'version', 'layout', 'size', 'state', 'bell', 'attached', 'rq_head', 'rq_tail', 'rq_ptr', 'rq_cap', 'frames_done', 'view_w', 'view_h',
    'dpr1000', 'features', 'st_draws', 'st_passes', 'st_enc_us', 'st_errors', 'dumps_done', 'st_pipelines', 'rb_want', 'rb_done', 'frame'],
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
c += 'printf("K_MARKS %u\\n", (unsigned)GW_K_MARKS);\nprintf("MARKS_BYTES %u\\n", (unsigned)sizeof(GwMarks));\nprintf("MARKS_MAX %u\\n", (unsigned)GW_MARKS_MAX);\nprintf("MARKS_MAGIC %u\\n", (unsigned)GW_MARKS_MAGIC);\n';
c += 'printf("K_TONE %u\\n", (unsigned)GW_K_TONE);\nprintf("TONE_BYTES %u\\n", (unsigned)sizeof(GwTone));\nprintf("TONE_MAGIC %u\\n", (unsigned)GW_TONE_MAGIC);\n';
c += 'printf("K_PHOTO %u\\n", (unsigned)GW_K_PHOTO);\nprintf("PHOTO_BYTES %u\\n", (unsigned)sizeof(GwPhoto));\nprintf("PHOTO_MAGIC %u\\n", (unsigned)GW_PHOTO_MAGIC);\nprintf("PHOTO_ACK %u\\n", (unsigned)offsetof(GwPhoto, ack));\nreturn 0; }\n';
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
let bad = 0;
const eq = (what, a, b) => { if (a !== b) { bad++; console.log(`MISMATCH ${what}: C ${a} vs TS ${b}`); } };
eq('GwDraw size', C['GwDraw sizeof'], T.DRAW_BYTES); eq('GwDraw words', C['GwDraw sizeof'] / 4, T.DRAW_WORDS);
for (const f of fields.GwDraw) eq(`GwDraw.${f}`, C[`GwDraw ${f}`] / 4, T.D[f === 'blit_src' ? 'blitSrc' : snake(f)]);
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
eq('GW_K_TONE', C['K_TONE'], T.K_TONE); eq('GwTone size', C['TONE_BYTES'], T.TONE_BYTES); eq('GW_TONE_MAGIC', C['TONE_MAGIC'], T.TONE_MAGIC);
eq('GW_K_PHOTO', C['K_PHOTO'], T.K_PHOTO); eq('GwPhoto size', C['PHOTO_BYTES'], T.PHOTO_BYTES); eq('GW_PHOTO_MAGIC', C['PHOTO_MAGIC'], T.PHOTO_MAGIC); eq('GwPhoto.ack', C['PHOTO_ACK'], T.PHOTO_ACK);
fs.rmSync(tmp, { recursive: true, force: true });
console.log(bad ? `${bad} mismatches` : `gpu proto OK: ${Object.keys(C).length} values match (layout ${C.LAYOUT.toString(16)})`);
process.exit(bad ? 1 : 0);
