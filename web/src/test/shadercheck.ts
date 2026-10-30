// Shader ABI check (docs/web/gpu-abi.md section 9): compiles every WGSL module
// of a shader dump in this browser's WebGPU, builds render pipelines with the
// ABI's explicit layouts, and runs the vertex-pulling compute test against a
// CPU decoder with com.c decl_fetch semantics, and runs every vertex shader
// that has test vectors (vs_<id>.vec) against the CPU interpreter, every
// ps_2_x, ps_3_0 and vs_3_0 module with vectors (.p3vec, .v3vec) against
// psref, the D3D9 reference.  Result: one line "SHADERCHECK {json}" and
// document.title = 'DONE'.
//
//   /test/shadercheck.html?opfs=<dir>/<sub>   the dump in OPFS (default shadercheck/shaders)
//   &cross                                    also every game VS (ids < 1000) x every game PS
import { ALPHA_ALWAYS, ALPHA_GREATEREQUAL, KWIN_SIZE, HLPAR_SIZE, createShaderAbi, group1Entries, isCubePs, isDepthPs, type ShaderAbi } from '../gpu/shader-abi';

const q = new URLSearchParams(location.search);
const pre = document.createElement('pre'); document.body.append(pre);
const log = (s: string) => { pre.textContent += s + '\n'; console.log(s); };

interface ModuleResult { name: string; bytes: number; ms: number; errors: string[]; warnings: number }
interface Summary {
  adapter: string; modules: number; vs: number; ps: number; psCube: number; psDepth: number; compileErrors: number; warnings: number;
  compileMsTotal: number; compileMsMax: number; pipelines: number; pipelineErrors: number; pipelineMsTotal: number;
  pipelineMsMax: number; pull: { cases: number; exact: number; maxUlp: number; flushed: number; bad: number } | null;
  diff: DiffResult | null; psdiff: DiffResult | null; vs3diff: DiffResult | null; ps3diff: DiffResult | null;
  failures: string[]; ok: boolean;
}

async function readDump(path: string): Promise<{ files: Map<string, string>; vecs: Map<string, ArrayBuffer>; vecs3: Map<string, ArrayBuffer> }> {
  let dir = await navigator.storage.getDirectory();
  for (const part of path.split('/').filter(Boolean)) dir = await dir.getDirectoryHandle(part);
  const files = new Map<string, string>(), vecs = new Map<string, ArrayBuffer>(), vecs3 = new Map<string, ArrayBuffer>();
  for await (const [name, h] of (dir as any).entries() as AsyncIterable<[string, FileSystemHandle]>) {
    if (h.kind !== 'file') continue;
    const file = await (h as FileSystemFileHandle).getFile();
    if (name.endsWith('.wgsl')) files.set(name, await file.text());
    else if (name.endsWith('.vec') || name.endsWith('.pvec')) vecs.set(name.replace(/\.p?vec$/, ''), await file.arrayBuffer());
    else if (name.endsWith('.p3vec') || name.endsWith('.v3vec')) vecs3.set(name.replace(/\.[pv]3vec$/, ''), await file.arrayBuffer());
  }
  return { files, vecs, vecs3 };
}

/** what a module's first line says: its texture stages (", 8 stages"; else 4) and binding kinds (", kinds 0x..") */
function moduleStages(code: string): number { return /^\/\/[^\n]*, 8 stages/.test(code) ? 8 : 4; }
function moduleKinds(code: string): number { const m = /^\/\/[^\n]*, kinds 0x([0-9a-f]+)/.exec(code); return m ? parseInt(m[1], 16) : 0; }

// ---- differential test: the WGSL vertex function vs the CPU interpreter ----
// vs_<id>.vec comes from web/tools/wgsl-xl --pairs (vs_run on the same bytes
// and constants).  The vertex entry point becomes a plain function called by
// a compute shader that stores the 29 VOut floats per vertex ('VSV2', an
// 8-stage module's: 45, uv4..uv7 after the fog).
interface DiffResult { modules: number; vertices: number; components: number; bad: number; maxRel: number; worst: string;
  /** pixel shaders: alpha-test decisions that differ only because alpha is within the tolerance of aref */
  edgeFlips?: number;
  /** psref vectors: pixels or vertices the reference marks fragile (its own result jumps past the tolerance when
   *  its inputs move by 1e-5: a comparison at its threshold), how many of them differ (not counted as bad) */
  fragile?: number; fragileDiffer?: number }
async function vsDiff(device: GPUDevice, name: string, code: string, vec: ArrayBuffer, res: DiffResult, failures: string[]) {
  const u = new Uint32Array(vec, 0, 4);
  const v2 = u[0] === 0x32565356, NO = v2 ? 45 : 29;
  if (u[0] !== 0x31565356 && !v2) { failures.push(`${name}.vec: bad magic`); return; }
  const nv = u[1], stride = u[2];
  const vbBytes = new Uint8Array(vec, 16, nv * stride);
  const consts = new Float32Array(vec, 16 + nv * stride, 1024);
  const want = new Float32Array(vec, 16 + nv * stride + 4096, nv * NO);
  const head = code.match(/@vertex fn (vs_\d+)\(@builtin\(vertex_index\) vid: u32, @builtin\(instance_index\) iid: u32\) -> VOut \{/);
  if (!head) { failures.push(`${name}: no vertex entry point`); return; }
  // the draw's K block at row KB of the window, as the renderer passes it in firstInstance
  const KB = 37;
  const cs = code.replace(head[0], 'fn vsfn(vid: u32, iid: u32) -> VOut {') + `
@group(0) @binding(3) var<storage, read_write> OUTV: array<f32>;
@compute @workgroup_size(64) fn cs(@builtin(global_invocation_id) g: vec3u) {
  if (g.x >= arrayLength(&OUTV) / ${NO}u) { return; }
  let o = vsfn(g.x, ${KB}u);
  let b = g.x * ${NO}u;
  for (var c = 0u; c < 4u; c++) {
    OUTV[b + c] = o.pos[c]; OUTV[b + 4u + c] = o.col0[c]; OUTV[b + 8u + c] = o.col1[c];
    OUTV[b + 12u + c] = o.uv0[c]; OUTV[b + 16u + c] = o.uv1[c]; OUTV[b + 20u + c] = o.uv2[c]; OUTV[b + 24u + c] = o.uv3[c];${v2 ? `
    OUTV[b + 29u + c] = o.uv4[c]; OUTV[b + 33u + c] = o.uv5[c]; OUTV[b + 37u + c] = o.uv6[c]; OUTV[b + 41u + c] = o.uv7[c];` : ''}
  }
  OUTV[b + 28u] = o.fog;
}`;
  const module = device.createShaderModule({ code: cs, label: name + '.cs' });
  const ci = await module.getCompilationInfo();
  const errs = ci.messages.filter(m => m.type === 'error');
  if (errs.length) { failures.push(`${name} (compute twin): ${errs.map(m => m.message).join('; ')}`); return; }
  const pipe = await device.createComputePipelineAsync({ layout: 'auto', compute: { module } });
  const vbSize = Math.max(16, (vbBytes.length + 3) & ~3);
  const vb = device.createBuffer({ size: vbSize, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
  const vbCopy = new Uint8Array(vbSize); vbCopy.set(vbBytes);
  device.queue.writeBuffer(vb, 0, vbCopy);
  // a 64 KB window (KWIN_SIZE) with the block at row KB; the rows around it hold other values,
  // so a shader that read outside its block would differ from the CPU
  const k = new ArrayBuffer(KWIN_SIZE);
  new Float32Array(k).fill(-7.25);
  new Uint32Array(k, KB * 16, 4).set([stride, 0x03020100, 0, 0]);    // tsel: stage k reads oTk
  new Float32Array(k, KB * 16 + 16, 1024).set(consts);
  const kb = device.createBuffer({ size: KWIN_SIZE, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  device.queue.writeBuffer(kb, 0, k);
  const out = device.createBuffer({ size: nv * NO * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
  const rd = device.createBuffer({ size: nv * NO * 4, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
  const bg = device.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries: [
    { binding: 0, resource: { buffer: vb } }, { binding: 1, resource: { buffer: kb } }, { binding: 3, resource: { buffer: out } }] });
  const enc = device.createCommandEncoder();
  const pass = enc.beginComputePass(); pass.setPipeline(pipe); pass.setBindGroup(0, bg); pass.dispatchWorkgroups(Math.ceil(nv / 64)); pass.end();
  enc.copyBufferToBuffer(out, 0, rd, 0, nv * NO * 4);
  device.queue.submit([enc.finish()]);
  await rd.mapAsync(GPUMapMode.READ);
  const got = new Float32Array(rd.getMappedRange().slice(0)); rd.unmap();
  vb.destroy(); kb.destroy(); out.destroy(); rd.destroy();
  const FIELD = ['pos', 'col0', 'col1', 'uv0', 'uv1', 'uv2', 'uv3'];
  const field = (k: number) => k === 28 ? 'fog' : k < 28 ? FIELD[k >> 2] + '.' + 'xyzw'[k & 3] : `uv${4 + ((k - 29) >> 2)}.${'xyzw'[(k - 29) & 3]}`;
  let bad = 0;
  for (let i = 0; i < nv * NO; i++) {
    const g = got[i], w = want[i];
    res.components++;
    if (g === w || (Number.isNaN(g) && Number.isNaN(w))) continue;
    const rel = Math.abs(g - w) / Math.max(1, Math.abs(w));
    if (rel > res.maxRel) { res.maxRel = rel; res.worst = `${name} v${Math.floor(i / NO)} ${field(i % NO)}: gpu ${g} cpu ${w}`; }
    if (!(rel <= 1e-4)) {
      bad++;
      if (bad <= 3) failures.push(`${name} vertex ${Math.floor(i / NO)} out ${i % NO}: gpu ${g} cpu ${w}`);
    }
  }
  res.modules++; res.vertices += nv; res.bad += bad;
}

// ---- vertex pulling: CPU reference = com.c decl_fetch ----
// f32 arithmetic in JS: a double division of small integers rounded once to
// f32 equals the correctly rounded f32 division, as C does in decl_fetch.
function declFetch(b: Uint8Array, a: number, type: number): number[] {
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const f = (x: number) => dv.getFloat32(x, true), s = (x: number) => dv.getInt16(x, true), u = (x: number) => dv.getUint16(x, true);
  const F = Math.fround;
  switch (type) {
    case 0: return [f(a), 0, 0, 1];
    case 1: return [f(a), f(a + 4), 0, 1];
    case 2: return [f(a), f(a + 4), f(a + 8), 1];
    case 3: return [f(a), f(a + 4), f(a + 8), f(a + 12)];
    case 4: return [F(b[a + 2] / 255), F(b[a + 1] / 255), F(b[a] / 255), F(b[a + 3] / 255)];
    case 5: return [b[a], b[a + 1], b[a + 2], b[a + 3]];
    case 8: return [F(b[a] / 255), F(b[a + 1] / 255), F(b[a + 2] / 255), F(b[a + 3] / 255)];
    case 6: return [s(a), s(a + 2), 0, 1];
    case 7: return [s(a), s(a + 2), s(a + 4), s(a + 6)];
    case 9: return [F(s(a) / 32767), F(s(a + 2) / 32767), 0, 1];
    case 10: return [F(s(a) / 32767), F(s(a + 2) / 32767), F(s(a + 4) / 32767), F(s(a + 6) / 32767)];
    case 11: return [F(u(a) / 65535), F(u(a + 2) / 65535), 0, 1];
    case 12: return [F(u(a) / 65535), F(u(a + 2) / 65535), F(u(a + 4) / 65535), F(u(a + 6) / 65535)];
  }
  return [0, 0, 0, 1];
}
const f32bits = (x: number) => { const t = new Float32Array([x]); return new Uint32Array(t.buffer)[0]; };
function ulpDiff(x: number, y: number): number {
  const a = f32bits(x) | 0, b = f32bits(y) | 0;
  const oa = a < 0 ? 0x80000000 - a : a, ob = b < 0 ? 0x80000000 - b : b;   // ordered integers
  return Math.abs(oa - ob);
}
const isDenormal = (x: number) => x !== 0 && Math.abs(x) < 1.1754943508222875e-38;

async function pullTest(device: GPUDevice, code: string, failures: string[]) {
  const module = device.createShaderModule({ code, label: 'pulltest' });
  const info = await module.getCompilationInfo();
  const errs = info.messages.filter(m => m.type === 'error');
  if (errs.length) { failures.push('pulltest: ' + errs.map(m => m.message).join('; ')); return null; }
  const pipeline = await device.createComputePipelineAsync({ layout: 'auto', compute: { module } });
  // deterministic bytes (xorshift32), long enough for base + offset + 16
  const bytes = new Uint8Array(8192);
  let x = 0x12345678;
  for (let i = 0; i < bytes.length; i++) { x ^= x << 13; x ^= x >>> 17; x ^= x << 5; bytes[i] = x & 0xff; }
  // a few readable values among the random bits (exact small floats, short extremes)
  new DataView(bytes.buffer).setFloat32(16, 1.5, true); new DataView(bytes.buffer).setInt16(40, -32768, true);
  const vb = device.createBuffer({ size: bytes.length, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
  device.queue.writeBuffer(vb, 0, bytes);
  const N = 13 * 8, outSize = N * 16;
  const out = device.createBuffer({ size: outSize, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
  const rd = device.createBuffer({ size: outSize, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
  const pt = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const bg = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [
    { binding: 0, resource: { buffer: vb } }, { binding: 1, resource: { buffer: out } }, { binding: 2, resource: { buffer: pt } }] });
  const res = { cases: 0, exact: 0, maxUlp: 0, flushed: 0, bad: 0 };
  for (const base of [0, 1, 2, 3, 4, 4093, 6002, 7999 - 32]) {
    device.queue.writeBuffer(pt, 0, new Uint32Array([base, 0, 0, 0]));
    const enc = device.createCommandEncoder();
    const pass = enc.beginComputePass(); pass.setPipeline(pipeline); pass.setBindGroup(0, bg); pass.dispatchWorkgroups(1); pass.end();
    enc.copyBufferToBuffer(out, 0, rd, 0, outSize);
    device.queue.submit([enc.finish()]);
    await rd.mapAsync(GPUMapMode.READ);
    const got = new Float32Array(rd.getMappedRange().slice(0));
    rd.unmap();
    for (let t = 0; t <= 12; t++) for (let o = 0; o < 8; o++) {
      const want = declFetch(bytes, base + o, t);
      for (let c = 0; c < 4; c++) {
        const g = got[(t * 8 + o) * 4 + c], w = want[c];
        res.cases++;
        if (f32bits(g) === f32bits(w) || (Number.isNaN(g) && Number.isNaN(w))) { res.exact++; continue; }
        if (isDenormal(w) && g === 0) { res.flushed++; continue; }
        // bit-exact is required (the helpers round divisions like C does);
        // the ulp distance is only reported
        const d = ulpDiff(g, w);
        res.maxUlp = Math.max(res.maxUlp, d);
        res.bad++;
        if (res.bad <= 10) failures.push(`pull type ${t} base ${base} off ${o} comp ${c}: gpu ${g} cpu ${w} (${d} ulp)`);
      }
    }
  }
  return res;
}


// ---- differential test: the WGSL pixel function vs the reference interpreter ----
// ps_<id>.pvec comes from web/tools/wgsl-xl --pairs (ps_ref_run, the Mac's
// pixel shader semantics executed on the bytecode).  The fragment entry point
// becomes a plain function; textureSample becomes the same analytic "texture"
// the reference uses, and discard sets a flag, so it runs in a compute shader.
const FTEX = `
fn ftex2(k: u32, c: vec2f) -> vec4f {
  let f = f32(k);
  return vec4f(0.5 + 0.4 * sin(c.x * 3.1 + f * 0.37), 0.5 + 0.4 * cos(c.y * 2.3 + f * 0.5),
               0.5 + 0.4 * sin((c.x + c.y) * 1.7 + f), 0.5 + 0.4 * cos(c.x * c.y * 5.3 + 0.2 * f));
}
fn ftex3(k: u32, c: vec3f) -> vec4f { return ftex2(k, vec2f(c.x + c.z * 0.3, c.y - c.z * 0.2)); }
`;
// 'PSV2' (8 stages): sums, products and floor, FTEX3's bump below (wgsl-xl.c ftex8): a GPU's sin may be 2^-11 off,
// which Most Wanted's shaders amplify past the tolerance.  The bump is even about every multiple of 0.5, so each
// channel's offset (c + d * stage) stays at least 0.13 away from one: a negated coordinate shows at every stage
const FTEX8 = `
fn fbump(t: f32) -> f32 { let u = t - floor(t); return 4.0 * u * (1.0 - u); }
fn ftex2(k: u32, c: vec2f) -> vec4f {
  let f = f32(k);
  return vec4f(0.1 + 0.8 * fbump(c.x * 3.1 + (0.15 + 0.53 * f)), 0.1 + 0.8 * fbump(c.y * 2.3 + (0.35 + 0.47 * f)),
               0.1 + 0.8 * fbump((c.x + c.y) * 1.7 + (0.13 + 0.53 * f)), 0.1 + 0.8 * fbump(c.x * c.y * 5.3 + (0.37 + 0.47 * f)));
}
fn ftex3(k: u32, c: vec3f) -> vec4f { return ftex2(k, vec2f(c.x + c.z * 0.3, c.y - c.z * 0.2)); }
`;
async function psDiff(device: GPUDevice, name: string, code: string, vec: ArrayBuffer, res: DiffResult, failures: string[]) {
  const u = new Uint32Array(vec, 0, 4);
  // 'PSV2' (an 8-stage module): 41 inputs per pixel, uv4..uv7 after the fog, and each binding's kind as the module
  // declares it (ps_2_0 declares its samplers' types; ps_ref_run follows the declarations on its side)
  const v2 = u[0] === 0x32565350, NIN = v2 ? 41 : 25;
  if (u[0] !== 0x31565350 && !v2) { failures.push(`${name}.pvec: bad magic`); return; }
  const np = u[1], cubemask = u[2];
  const par = new Uint8Array(vec, 16, 560);
  const inputs = new Float32Array(vec, 576, np * NIN);
  const want = new Float32Array(vec, 576 + np * NIN * 4, np * 5);
  const head = code.match(/@fragment fn (ps_\d+)\(i: VOut\) -> @location\(0\) vec4f \{/);
  if (!head) { failures.push(`${name}: no fragment entry point`); return; }
  const cubes = new Set([...code.matchAll(/var tex(\d+): texture_cube<f32>/g)].map(m => +m[1]));
  const isCube = (k: number) => v2 ? cubes.has(k) : ((cubemask >> k) & 1) !== 0;
  const body = code.replace(head[0], 'fn psfn(i: VOut) -> vec4f {').replaceAll('{ discard; }', '{ KILL = 1u; }')
    .replace(/textureSample\(tex(\d), smp\d, /g, (_m, k) => isCube(+k) ? `ftex3(${k}u, ` : `ftex2(${k}u, `);
  const uv8 = v2 ? Array.from({ length: 4 }, (_, k) =>
    `\n  i.uv${4 + k} = vec4f(PIN[b + ${25 + 4 * k}u], PIN[b + ${26 + 4 * k}u], PIN[b + ${27 + 4 * k}u], PIN[b + ${28 + 4 * k}u]);`).join('') : '';
  const cs = body + (v2 ? FTEX8 : FTEX) + `
var<private> KILL: u32;
@group(0) @binding(4) var<storage, read> PIN: array<f32>;
@group(0) @binding(5) var<storage, read_write> POUT: array<f32>;
@compute @workgroup_size(64) fn cs(@builtin(global_invocation_id) g: vec3u) {
  if (g.x >= arrayLength(&POUT) / 5u) { return; }
  let b = g.x * ${NIN}u;
  var i: VOut;
  i.pos = vec4f(0.0);
  i.col0 = vec4f(PIN[b], PIN[b + 1u], PIN[b + 2u], PIN[b + 3u]);
  i.col1 = vec4f(PIN[b + 4u], PIN[b + 5u], PIN[b + 6u], PIN[b + 7u]);
  i.uv0 = vec4f(PIN[b + 8u], PIN[b + 9u], PIN[b + 10u], PIN[b + 11u]);
  i.uv1 = vec4f(PIN[b + 12u], PIN[b + 13u], PIN[b + 14u], PIN[b + 15u]);
  i.uv2 = vec4f(PIN[b + 16u], PIN[b + 17u], PIN[b + 18u], PIN[b + 19u]);
  i.uv3 = vec4f(PIN[b + 20u], PIN[b + 21u], PIN[b + 22u], PIN[b + 23u]);
  i.fog = PIN[b + 24u];${uv8}
  KILL = 0u;
  let c = psfn(i);
  let o = g.x * 5u;
  POUT[o] = c.x; POUT[o + 1u] = c.y; POUT[o + 2u] = c.z; POUT[o + 3u] = c.w; POUT[o + 4u] = f32(KILL);
}`;
  const module = device.createShaderModule({ code: cs, label: name + '.cs' });
  const ci = await module.getCompilationInfo();
  const errs = ci.messages.filter(m => m.type === 'error');
  if (errs.length) { failures.push(`${name} (compute twin): ${errs.map(m => m.message).join('; ')}`); return; }
  const pipe = await device.createComputePipelineAsync({ layout: 'auto', compute: { module } });
  const mk = (size: number, usage: number) => device.createBuffer({ size, usage });
  const pb = mk(560, GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST);
  device.queue.writeBuffer(pb, 0, par);
  const ib = mk(np * NIN * 4, GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST);
  device.queue.writeBuffer(ib, 0, inputs);
  const ob = mk(np * 20, GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC);
  const rd = mk(np * 20, GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST);
  // the lit pixel shaders also read H, the other cars' headlights: none (n = 0), as the
  // reference interpreter has no such term (a web-only addition, gpu-abi.md section 3)
  const hb = /@group\(0\) @binding\(3\)/.test(code) ? mk(HLPAR_SIZE, GPUBufferUsage.UNIFORM) : null;
  const bg = device.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries: [
    { binding: 2, resource: { buffer: pb } }, { binding: 4, resource: { buffer: ib } }, { binding: 5, resource: { buffer: ob } },
    ...(hb ? [{ binding: 3, resource: { buffer: hb } }] : [])] });
  const enc = device.createCommandEncoder();
  const pass = enc.beginComputePass(); pass.setPipeline(pipe); pass.setBindGroup(0, bg); pass.dispatchWorkgroups(Math.ceil(np / 64)); pass.end();
  enc.copyBufferToBuffer(ob, 0, rd, 0, np * 20);
  device.queue.submit([enc.finish()]);
  await rd.mapAsync(GPUMapMode.READ);
  const got = new Float32Array(rd.getMappedRange().slice(0)); rd.unmap();
  pb.destroy(); ib.destroy(); ob.destroy(); rd.destroy(); hb?.destroy();
  const aref = new Float32Array(vec, 16, 1)[0];
  const TOL = 1e-3;                         // relative, as for the colours below
  let bad = 0;
  for (let px = 0; px < np; px++) {
    const gk = got[px * 5 + 4], wk = want[px * 5 + 4];
    res.components++;
    if (gk !== wk) {
      // The alpha test (alpha >= aref) of a pixel whose alpha is within the
      // colour tolerance of aref may go either way on another GPU: accept the
      // flip when both alphas are that close and lie on opposite sides of
      // aref, i.e. the alpha test alone explains it; the colours must still match.
      const ga = got[px * 5 + 3], wa = want[px * 5 + 3], tol = TOL * Math.max(1, Math.abs(aref));
      const edge = Math.abs(ga - aref) <= tol && Math.abs(wa - aref) <= tol && (ga >= aref) === !gk && (wa >= aref) === !wk;
      if (!edge) { bad++; if (bad <= 3) failures.push(`${name} pixel ${px}: kill gpu ${gk} ref ${wk} (alpha gpu ${ga} ref ${wa}, aref ${aref})`); continue; }
      res.edgeFlips = (res.edgeFlips ?? 0) + 1;
    } else if (wk) continue;                // discarded on both sides: colour irrelevant
    for (let c = 0; c < 4; c++) {
      const g = got[px * 5 + c], w = want[px * 5 + c];
      res.components++;
      if (g === w || (Number.isNaN(g) && Number.isNaN(w))) continue;
      const rel = Math.abs(g - w) / Math.max(1, Math.abs(w));
      if (rel > res.maxRel) { res.maxRel = rel; res.worst = `${name} pixel ${px}.${'rgba'[c]}: gpu ${g} ref ${w}`; }
      if (!(rel <= 1e-3)) { bad++; if (bad <= 3) failures.push(`${name} pixel ${px}.${'rgba'[c]}: gpu ${g} ref ${w}`); }
    }
  }
  res.modules++; res.vertices += np; res.bad += bad;
}

// ---- ps_2_x, ps_3_0 and vs_3_0 against psref, the D3D9 reference ----
// The vectors come from web/tools/wgsl-xl.c (write_sm3_ps_vectors,
// write_sm3_vs_vectors).  The texture is the analytic function of its sm3_tex,
// with the level of detail from the coordinate's fine derivatives (implicit),
// plus the bias, the level, or texldd's gradients.  Only products, sums and
// floor (a GPU's sin may be 2^-11 off, enough to flip a depth comparison).
const FTEX3 = `
fn bump(t: f32) -> f32 { let u = t - floor(t); return 4.0 * u * (1.0 - u); }
fn ftc(k: u32, x: f32, y: f32, lod: f32) -> vec4f {
  let f = f32(k);
  return vec4f(0.1 + 0.8 * bump(x * 3.1 + f * 0.37 + 0.05 * lod), 0.1 + 0.8 * bump(y * 2.3 + f * 0.5),
               0.1 + 0.8 * bump((x + y) * 1.7 + f * 0.13), 0.1 + 0.8 * bump(x * y * 5.3 + 0.2 * f));
}
fn flod(gx: vec2f, gy: vec2f) -> f32 {
  let g = max(max(abs(gx.x), abs(gx.y)), max(abs(gy.x), abs(gy.y))) * 256.0;
  return select(-16.0, log2(g), g > 0.0);
}
fn ft2(k: u32, c: vec2f) -> vec4f { return ftc(k, c.x, c.y, flod(dpdxFine(c), dpdyFine(c))); }
fn ft2Bias(k: u32, c: vec2f, b: f32) -> vec4f { return ftc(k, c.x, c.y, flod(dpdxFine(c), dpdyFine(c)) + b); }
fn ft2Level(k: u32, c: vec2f, l: f32) -> vec4f { return ftc(k, c.x, c.y, l); }
fn ft2Grad(k: u32, c: vec2f, gx: vec2f, gy: vec2f) -> vec4f { return ftc(k, c.x, c.y, flod(gx, gy)); }
fn ft3(k: u32, c: vec3f) -> vec4f { return ftc(k, c.x + c.z * 0.3, c.y - c.z * 0.2, flod(dpdxFine(c).xy, dpdyFine(c).xy)); }
fn ft3Bias(k: u32, c: vec3f, b: f32) -> vec4f { return ftc(k, c.x + c.z * 0.3, c.y - c.z * 0.2, flod(dpdxFine(c).xy, dpdyFine(c).xy) + b); }
fn ft3Level(k: u32, c: vec3f, l: f32) -> vec4f { return ftc(k, c.x + c.z * 0.3, c.y - c.z * 0.2, l); }
fn ft3Grad(k: u32, c: vec3f, gx: vec3f, gy: vec3f) -> vec4f { return ftc(k, c.x + c.z * 0.3, c.y - c.z * 0.2, flod(gx.xy, gy.xy)); }
`;
const P3_NIN = 42, P3_W = 16;            // inputs per pixel; the target is 16 pixels (8 quads) wide
/** A pixel shader module runs as the fragment stage of a full-target draw, one 2x2 quad per vector quad, so
 *  dpdxFine/dpdyFine see the quad's four pixels; the inputs come from a storage buffer by pixel position. */
async function ps3Diff(device: GPUDevice, name: string, code: string, vec: ArrayBuffer, res: DiffResult, failures: string[]) {
  const u = new Uint32Array(vec, 0, 4);
  if (u[0] !== 0x31563350) { failures.push(`${name}.p3vec: bad magic`); return; }
  const nq = u[1], npx = nq * 4, rows = Math.ceil(nq / 8) * 2;
  const par = new Uint8Array(vec, 16, 560);
  const inputs = new Float32Array(vec, 576, npx * P3_NIN);
  const want = new Float32Array(vec, 576 + npx * P3_NIN * 4, npx * 8);     // rgba, killed, depth, fragile, 0
  const head = code.match(/@fragment fn (ps_\d+)\(i: VOut(, @builtin\(front_facing\) ff: bool)?\) -> (@location\(0\) vec4f|PSOut) \{/);
  if (!head) { failures.push(`${name}: no fragment entry point`); return; }
  const depth = head[3] === 'PSOut', stages8 = /@location\(7\) uv4/.test(code);
  const cube = new Set([...code.matchAll(/var tex(\d+): texture_cube<f32>/g)].map(m => +m[1]));
  const body = code.replace(head[0], `fn psfn(i: VOut, ff: bool) -> ${depth ? 'PSOut' : 'vec4f'} {`)
    .replace(/textureSample(Bias|Level|Grad)?\(tex(\d+), smp\d+, /g, (_m, kind, k) => `ft${cube.has(+k) ? 3 : 2}${kind ?? ''}(${k}u, `);
  const uvs = Array.from({ length: stages8 ? 8 : 4 }, (_, k) =>
    `  i.uv${k} = vec4f(PIN[b + ${8 + 4 * k}u], PIN[b + ${9 + 4 * k}u], PIN[b + ${10 + 4 * k}u], PIN[b + ${11 + 4 * k}u]);`).join('\n');
  const twin = body + FTEX3 + `
@group(0) @binding(4) var<storage, read> PIN: array<f32>;
struct TOut { @location(0) c: vec4f, @location(1) k: vec4f }
@vertex fn tvs(@builtin(vertex_index) v: u32) -> @builtin(position) vec4f {
  let p = array(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  return vec4f(p[v], 0.0, 1.0);
}
@fragment fn tfs(@builtin(position) fp: vec4f) -> TOut {
  let px = vec2u(fp.xy);
  let b = (((px.y >> 1u) * 8u + (px.x >> 1u)) * 4u + (px.x & 1u) + ((px.y & 1u) << 1u)) * ${P3_NIN}u;
  var i: VOut;
  i.pos = fp;
  i.col0 = vec4f(PIN[b], PIN[b + 1u], PIN[b + 2u], PIN[b + 3u]);
  i.col1 = vec4f(PIN[b + 4u], PIN[b + 5u], PIN[b + 6u], PIN[b + 7u]);
${uvs}
  i.fog = PIN[b + 40u];
  let r = psfn(i, PIN[b + 41u] > 0.0);
  var o: TOut;
  o.c = ${depth ? 'r.c' : 'r'};
  o.k = vec4f(0.0, ${depth ? 'r.z' : '0.0'}, 0.0, 0.0);
  return o;
}`;
  const module = device.createShaderModule({ code: twin, label: name + '.quad' });
  const ci = await module.getCompilationInfo();
  const errs = ci.messages.filter(m => m.type === 'error');
  if (errs.length) { failures.push(`${name} (quad twin): ${errs.map(m => `${m.lineNum}:${m.linePos} ${m.message}`).join('; ')}`); return; }
  const fmt: GPUTextureFormat = 'rgba32float';
  const pipe = await device.createRenderPipelineAsync({ layout: 'auto', vertex: { module, entryPoint: 'tvs' },
    fragment: { module, entryPoint: 'tfs', targets: [{ format: fmt }, { format: fmt }] }, primitive: { topology: 'triangle-list' } });
  const mk = (size: number, usage: number) => device.createBuffer({ size, usage });
  const pb = mk(560, GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST);
  device.queue.writeBuffer(pb, 0, par);
  const ib = mk(inputs.byteLength, GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST);
  device.queue.writeBuffer(ib, 0, inputs);
  const tex = [0, 1].map(() => device.createTexture({ size: [P3_W, rows], format: fmt, usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC }));
  const rb = [0, 1].map(() => mk(P3_W * 16 * rows, GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST));
  const bg = device.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries: [
    { binding: 2, resource: { buffer: pb } }, { binding: 4, resource: { buffer: ib } }] });
  const enc = device.createCommandEncoder();
  // target 1 starts at x = 1 and the shader writes 0 there: a pixel left at 1 was discarded
  const pass = enc.beginRenderPass({ colorAttachments: [
    { view: tex[0].createView(), loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 0] },
    { view: tex[1].createView(), loadOp: 'clear', storeOp: 'store', clearValue: [1, 0, 0, 0] }] });
  pass.setPipeline(pipe); pass.setBindGroup(0, bg); pass.draw(3); pass.end();
  for (let t = 0; t < 2; t++) enc.copyTextureToBuffer({ texture: tex[t] }, { buffer: rb[t], bytesPerRow: P3_W * 16 }, [P3_W, rows]);
  device.queue.submit([enc.finish()]);
  const got: Float32Array[] = [];
  for (const b of rb) { await b.mapAsync(GPUMapMode.READ); got.push(new Float32Array(b.getMappedRange().slice(0))); b.unmap(); }
  pb.destroy(); ib.destroy(); for (const t of tex) t.destroy(); for (const b of rb) b.destroy();
  const aref = new Float32Array(vec, 16, 1)[0], TOL = 1e-3;
  let bad = 0;
  for (let p = 0; p < npx; p++) {
    const w = want.subarray(p * 8, p * 8 + 8), fragile = w[6] !== 0;
    if (w[4] < 0) continue;                            // psref refused this quad (D3D9 leaves it undefined)
    const qd = p >> 2, l = p & 3, x = (qd % 8) * 2 + (l & 1), y = Math.floor(qd / 8) * 2 + (l >> 1), o = (y * P3_W + x) * 4;
    const gc = got[0].subarray(o, o + 4), gk = got[1][o] !== 0 ? 1 : 0, gz = got[1][o + 1];
    res.components++;
    if (fragile) res.fragile = (res.fragile ?? 0) + 1;
    let differ = '';
    if (gk !== w[4]) {
      // an alpha test whose two alphas are both within the tolerance of aref may go either way
      const tol = TOL * Math.max(1, Math.abs(aref));
      if (Math.abs(gc[3] - aref) <= tol && Math.abs(w[3] - aref) <= tol) { res.edgeFlips = (res.edgeFlips ?? 0) + 1; continue; }
      differ = `${name} pixel ${p}: kill gpu ${gk} ref ${w[4]} (alpha gpu ${gc[3]} ref ${w[3]}, aref ${aref})`;
    } else if (w[4]) continue;                          // discarded on both sides
    else for (let c = 0; c < (depth ? 5 : 4) && !differ; c++) {
      const g = c < 4 ? gc[c] : gz, r = c < 4 ? w[c] : w[5];
      res.components++;
      if (g === r || (Number.isNaN(g) && Number.isNaN(r))) continue;
      const rel = Math.abs(g - r) / Math.max(1, Math.abs(r));
      if (!fragile && rel > res.maxRel) { res.maxRel = rel; res.worst = `${name} pixel ${p}.${'rgbaz'[c]}: gpu ${g} ref ${r}`; }
      if (!(rel <= TOL)) differ = `${name} pixel ${p}.${'rgbaz'[c]}: gpu ${g} ref ${r}`;
    }
    if (!differ) continue;
    if (fragile) { res.fragileDiffer = (res.fragileDiffer ?? 0) + 1; continue; }
    bad++;
    if (bad <= 3) failures.push(differ);
  }
  res.modules++; res.vertices += npx; res.bad += bad;
}

/** A vs_3_0 module as a compute function, as vsDiff; 45 outputs (pos, col0, col1, uv0..uv7, fog), compared where
 *  the shader's output declarations cover them. */
async function vs3Diff(device: GPUDevice, name: string, code: string, vec: ArrayBuffer, res: DiffResult, failures: string[]) {
  const u = new Uint32Array(vec, 0, 4);
  if (u[0] !== 0x31563356) { failures.push(`${name}.v3vec: bad magic`); return; }
  const nv = u[1], stride = u[2];
  const vbBytes = new Uint8Array(vec, 16, nv * stride);
  const consts = new Float32Array(vec, 16 + nv * stride, 1024);
  const care = new Float32Array(vec, 16 + nv * stride + 4096, 45);
  const want = new Float32Array(vec, 16 + nv * stride + 4096 + 180, nv * 46);    // 45 outputs, fragile
  const head = code.match(/@vertex fn (vs_\d+)\(@builtin\(vertex_index\) vid: u32, @builtin\(instance_index\) iid: u32\) -> VOut \{/);
  if (!head) { failures.push(`${name}: no vertex entry point`); return; }
  const stages8 = /@location\(7\) uv4/.test(code);
  const KB = 37;
  const fields = ['pos', 'col0', 'col1', 'uv0', 'uv1', 'uv2', 'uv3', ...(stages8 ? ['uv4', 'uv5', 'uv6', 'uv7'] : [])];
  const cs = code.replace(head[0], 'fn vsfn(vid: u32, iid: u32) -> VOut {') + `
@group(0) @binding(3) var<storage, read_write> OUTV: array<f32>;
@compute @workgroup_size(64) fn cs(@builtin(global_invocation_id) g: vec3u) {
  if (g.x >= arrayLength(&OUTV) / 45u) { return; }
  let o = vsfn(g.x, ${KB}u);
  let b = g.x * 45u;
  for (var c = 0u; c < 4u; c++) {
${fields.map((f, k) => `    OUTV[b + ${4 * k}u + c] = o.${f}[c];`).join('\n')}
  }
  OUTV[b + 44u] = o.fog;
}`;
  const module = device.createShaderModule({ code: cs, label: name + '.cs' });
  const ci = await module.getCompilationInfo();
  const errs = ci.messages.filter(m => m.type === 'error');
  if (errs.length) { failures.push(`${name} (compute twin): ${errs.map(m => m.message).join('; ')}`); return; }
  const pipe = await device.createComputePipelineAsync({ layout: 'auto', compute: { module } });
  const vbSize = Math.max(16, (vbBytes.length + 3) & ~3);
  const vb = device.createBuffer({ size: vbSize, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
  const vbCopy = new Uint8Array(vbSize); vbCopy.set(vbBytes);
  device.queue.writeBuffer(vb, 0, vbCopy);
  const k = new ArrayBuffer(KWIN_SIZE);
  new Float32Array(k).fill(-7.25);
  new Uint32Array(k, KB * 16, 4).set([stride, 0x03020100, 0, 0]);
  new Float32Array(k, KB * 16 + 16, 1024).set(consts);
  const kb = device.createBuffer({ size: KWIN_SIZE, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  device.queue.writeBuffer(kb, 0, k);
  const out = device.createBuffer({ size: nv * 45 * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
  const rd = device.createBuffer({ size: nv * 45 * 4, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
  // a shader that reads no vertex element never touches VB, and the 'auto' layout leaves it out
  const usesVB = /\bld\w*\(B \+/.test(code);
  const bg = device.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries: [
    ...(usesVB ? [{ binding: 0, resource: { buffer: vb } }] : []), { binding: 1, resource: { buffer: kb } }, { binding: 3, resource: { buffer: out } }] });
  const enc = device.createCommandEncoder();
  const pass = enc.beginComputePass(); pass.setPipeline(pipe); pass.setBindGroup(0, bg); pass.dispatchWorkgroups(Math.ceil(nv / 64)); pass.end();
  enc.copyBufferToBuffer(out, 0, rd, 0, nv * 45 * 4);
  device.queue.submit([enc.finish()]);
  await rd.mapAsync(GPUMapMode.READ);
  const got = new Float32Array(rd.getMappedRange().slice(0)); rd.unmap();
  vb.destroy(); kb.destroy(); out.destroy(); rd.destroy();
  const FIELD = ['pos', 'col0', 'col1', 'uv0', 'uv1', 'uv2', 'uv3', 'uv4', 'uv5', 'uv6', 'uv7'];
  let bad = 0;
  for (let v = 0; v < nv; v++) {
    const fragile = want[v * 46 + 45] !== 0;
    if (fragile) res.fragile = (res.fragile ?? 0) + 1;
    let differ = '';
    for (let c = 0; c < 45; c++) {
      if (!care[c]) continue;
      const g = got[v * 45 + c], w = want[v * 46 + c];
      res.components++;
      if (g === w || (Number.isNaN(g) && Number.isNaN(w))) continue;
      const rel = Math.abs(g - w) / Math.max(1, Math.abs(w));
      const what = `${name} v${v} ${c === 44 ? 'fog' : FIELD[c >> 2] + '.' + 'xyzw'[c & 3]}: gpu ${g} ref ${w}`;
      if (!fragile && rel > res.maxRel) { res.maxRel = rel; res.worst = what; }
      if (!(rel <= 1e-4) && !differ) differ = what;
    }
    if (!differ) continue;
    if (fragile) { res.fragileDiffer = (res.fragileDiffer ?? 0) + 1; continue; }
    bad++;
    if (bad <= 3) failures.push(differ);
  }
  res.modules++; res.vertices += nv; res.bad += bad;
}

async function main() {
  const path = q.get('opfs') ?? 'shadercheck/shaders';
  const { files, vecs, vecs3 } = await readDump(path);
  const failures: string[] = [];
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) throw new Error('no WebGPU adapter');
  const device = await adapter.requestDevice();
  device.addEventListener('uncapturederror', e => failures.push('uncaptured: ' + (e as GPUUncapturedErrorEvent).error.message));
  const abi: ShaderAbi = createShaderAbi(device);
  const info = adapter.info;
  log(`adapter: ${info.vendor} ${info.architecture} ${info.description}; ${files.size} modules from OPFS ${path}`);

  // ---- 1. modules ----
  const mods = new Map<string, GPUShaderModule>();
  const results: ModuleResult[] = [];
  const names = [...files.keys()].filter(n => n !== 'pulltest.wgsl').sort((a, b) => a.localeCompare(b, 'en', { numeric: true }));
  for (const name of names) {
    const code = files.get(name)!;
    const t0 = performance.now();
    device.pushErrorScope('validation');
    const m = device.createShaderModule({ code, label: name });
    const ci = await m.getCompilationInfo();
    const scope = await device.popErrorScope();
    const ms = performance.now() - t0;
    const errors = ci.messages.filter(x => x.type === 'error').map(x => `${x.lineNum}:${x.linePos} ${x.message}`);
    if (scope) errors.push(scope.message);
    results.push({ name, bytes: code.length, ms, errors, warnings: ci.messages.filter(x => x.type === 'warning').length });
    if (errors.length) { failures.push(`${name}: ${errors.join(' | ')}`); log(`ERROR ${name}: ${errors.join(' | ')}`); }
    else mods.set(name.replace(/\.wgsl$/, ''), m);
  }
  const vsNames = [...mods.keys()].filter(n => n.startsWith('vs_'));
  const psNames = [...mods.keys()].filter(n => n.startsWith('ps_'));
  const idOf = (n: string) => parseInt(n.slice(3), 10);

  // ---- 2. pipelines with the explicit ABI layouts ----
  // 4 stages and kinds 0: L2D or LCUBE; else group 1 from the pixel module's stages and kinds (gpu-abi.md 11.3)
  const layouts = new Map<string, GPUPipelineLayout>();
  function layoutOf(ps: string): GPUPipelineLayout {
    const code = files.get(ps + '.wgsl')!, stages = moduleStages(code), kinds = moduleKinds(code), cube = isCubePs(idOf(ps));
    if (stages === 4 && kinds === 0) return cube ? abi.layoutCube : abi.layout2d;
    const key = `${stages} ${kinds} ${cube}`;
    let l = layouts.get(key);
    if (!l) {
      const g1 = device.createBindGroupLayout({ label: `abi.group1 ${key}`, entries: group1Entries(stages, kinds, cube, GPUShaderStage.FRAGMENT)! });
      l = device.createPipelineLayout({ label: `abi.layout ${key}`, bindGroupLayouts: [abi.group0, g1] });
      layouts.set(key, l);
    }
    return l;
  }
  const pipeMs: number[] = [];
  let pipelineErrors = 0;
  async function pipeline(vs: string, ps: string, constants: Record<string, number>) {
    const t0 = performance.now();
    try {
      await device.createRenderPipelineAsync({
        label: `${vs}+${ps} ${JSON.stringify(constants)}`,
        layout: layoutOf(ps),
        vertex: { module: mods.get(vs)!, entryPoint: vs },
        fragment: { module: mods.get(ps)!, entryPoint: ps, constants, targets: [{ format: 'bgra8unorm' }] },
        primitive: { topology: 'triangle-list', frontFace: 'cw', cullMode: 'back' },
        depthStencil: { format: 'depth24plus-stencil8', depthWriteEnabled: true, depthCompare: 'less-equal' },
      });
    } catch (e) {
      pipelineErrors++;
      const msg = `pipeline ${vs}+${ps} ${JSON.stringify(constants)}: ${(e as Error).message}`;
      failures.push(msg); log('ERROR ' + msg);
    }
    pipeMs.push(performance.now() - t0);
  }
  const ff = mods.has('ps_0') ? 'ps_0' : null;
  for (const vs of vsNames) if (ff) await pipeline(vs, ff, { ALPHA_FUNC: ALPHA_ALWAYS });
  if (mods.has('vs_0'))
    for (const ps of psNames) {
      await pipeline('vs_0', ps, { ALPHA_FUNC: ALPHA_GREATEREQUAL });
      await pipeline('vs_0', ps, { ALPHA_FUNC: ALPHA_ALWAYS });
      await pipeline('vs_0', ps, { ALPHA_FUNC: ALPHA_GREATEREQUAL, PS20_TEXCOORD: 0 });
    }
  // &cross: the game's own VS modules (ids below 1000; the --pairs extras are left out) x every game PS
  if (q.has('cross'))
    for (const vs of vsNames) if (vs !== 'vs_0' && idOf(vs) < 1000) for (const ps of psNames) if (ps !== 'ps_0' && ps !== 'ps_256')
      await pipeline(vs, ps, { ALPHA_FUNC: ALPHA_GREATEREQUAL });

  // ---- 3. shaders against the CPU interpreters ----
  let diff: DiffResult | null = null, psdiff: DiffResult | null = null;
  if (vecs.size) {
    diff = { modules: 0, vertices: 0, components: 0, bad: 0, maxRel: 0, worst: '' };
    psdiff = { modules: 0, vertices: 0, components: 0, bad: 0, maxRel: 0, worst: '', edgeFlips: 0 };
    for (const [n, vec] of vecs) {
      if (!files.has(n + '.wgsl')) continue;
      if (n.startsWith('vs_')) await vsDiff(device, n, files.get(n + '.wgsl')!, vec, diff, failures);
      else await psDiff(device, n, files.get(n + '.wgsl')!, vec, psdiff, failures);
    }
    log(`vertex shader differential test: ${diff.modules} modules, ${diff.vertices} vertices, ${diff.bad} components over 1e-4 relative, max ${diff.maxRel.toExponential(2)} (${diff.worst})`);
    log(`pixel shader differential test: ${psdiff.modules} modules, ${psdiff.vertices} pixels, ${psdiff.bad} mismatches (kill flag or over 1e-3 relative), ${psdiff.edgeFlips} alpha-test flips within 1e-3 of aref accepted, max ${psdiff.maxRel.toExponential(2)} (${psdiff.worst})`);
  }
  // ps_2_x, ps_3_0 and vs_3_0 against psref
  let vs3diff: DiffResult | null = null, ps3diff: DiffResult | null = null;
  if (vecs3.size) {
    vs3diff = { modules: 0, vertices: 0, components: 0, bad: 0, maxRel: 0, worst: '' };
    ps3diff = { modules: 0, vertices: 0, components: 0, bad: 0, maxRel: 0, worst: '', edgeFlips: 0 };
    for (const [n, vec] of vecs3) {
      if (!files.has(n + '.wgsl')) { failures.push(`${n}: vectors without a module`); continue; }
      if (n.startsWith('vs_')) await vs3Diff(device, n, files.get(n + '.wgsl')!, vec, vs3diff, failures);
      else await ps3Diff(device, n, files.get(n + '.wgsl')!, vec, ps3diff, failures);
    }
    log(`vs_3_0 against psref: ${vs3diff.modules} modules, ${vs3diff.vertices} vertices, ${vs3diff.components} components, ${vs3diff.bad} vertices over 1e-4 relative, ${vs3diff.fragile ?? 0} fragile (${vs3diff.fragileDiffer ?? 0} of them differ), max ${vs3diff.maxRel.toExponential(2)} (${vs3diff.worst})`);
    log(`ps_2_x/ps_3_0 against psref: ${ps3diff.modules} modules, ${ps3diff.vertices} pixels, ${ps3diff.components} components, ${ps3diff.bad} pixels mismatch (kill flag or over 1e-3 relative), ${ps3diff.edgeFlips} alpha-test flips within 1e-3 of aref accepted, ${ps3diff.fragile ?? 0} fragile (${ps3diff.fragileDiffer ?? 0} of them differ), max ${ps3diff.maxRel.toExponential(2)} (${ps3diff.worst})`);
  }

  // ---- 4. vertex pulling ----
  const pull = files.has('pulltest.wgsl') ? await pullTest(device, files.get('pulltest.wgsl')!, failures) : null;
  if (!files.has('pulltest.wgsl')) failures.push('pulltest.wgsl missing');
  if (pull && pull.bad) log(`ERROR vertex pulling: ${pull.bad} mismatches`);

  const cms = results.map(r => r.ms);
  const s: Summary = {
    adapter: `${info.vendor} ${info.architecture}`.trim(), modules: results.length,
    vs: results.filter(r => r.name.startsWith('vs_')).length,
    ps: results.filter(r => r.name.startsWith('ps_') && !isCubePs(idOf(r.name)) && !isDepthPs(idOf(r.name))).length,
    psCube: results.filter(r => r.name.startsWith('ps_') && isCubePs(idOf(r.name)) && !isDepthPs(idOf(r.name))).length,
    psDepth: results.filter(r => r.name.startsWith('ps_') && isDepthPs(idOf(r.name))).length,
    compileErrors: results.filter(r => r.errors.length).length, warnings: results.reduce((a, r) => a + r.warnings, 0),
    compileMsTotal: +cms.reduce((a, b) => a + b, 0).toFixed(1), compileMsMax: +Math.max(0, ...cms).toFixed(1),
    pipelines: pipeMs.length, pipelineErrors, pipelineMsTotal: +pipeMs.reduce((a, b) => a + b, 0).toFixed(1),
    pipelineMsMax: +Math.max(0, ...pipeMs).toFixed(1), pull, diff, psdiff, vs3diff, ps3diff, failures, ok: false,
  };
  s.ok = failures.length === 0 && s.compileErrors === 0 && pipelineErrors === 0 && !!pull && pull.bad === 0 && (!diff || diff.bad === 0) &&
    (!psdiff || psdiff.bad === 0) && (!vs3diff || vs3diff.bad === 0) && (!ps3diff || ps3diff.bad === 0);
  for (const r of results) log(`module ${r.name} ${r.bytes} B ${r.ms.toFixed(1)} ms${r.errors.length ? ' ERROR' : ''}${r.warnings ? ` ${r.warnings} warnings` : ''}`);
  log('SHADERCHECK ' + JSON.stringify(s));
}

main().catch(e => log('SHADERCHECK ' + JSON.stringify({ ok: false, failures: ['exception: ' + (e as Error).message] })))
  .finally(() => { document.title = 'DONE'; });
