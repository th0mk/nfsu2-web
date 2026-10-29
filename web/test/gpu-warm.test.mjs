// The pipeline warm-up (web/src/gpu/encoder.ts loadWarm / warmUp): a pipeline an earlier session made is made again,
// as its shaders arrive, under the key the draws look it up by: a depth variant's (pixel module id 512 and up, a
// pipeline-key slot) and a depth-biased one's with 8 stages, and UG2's 4-stage ones under the key they always had.
//   node --test web/test/gpu-warm.test.mjs
// A fake GPUDevice: every call returns a stand-in, pipelines and layouts keep their descriptors.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const out = await esbuild.build({ stdin: { contents: "export { Encoder } from './encoder'; export * as P from './proto';",
  resolveDir: path.join(HERE, '../src/gpu'), loader: 'ts' }, bundle: true, write: false, format: 'esm', platform: 'neutral' });
const { Encoder, P } = await import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));

globalThis.GPUShaderStage ??= { VERTEX: 1, FRAGMENT: 2, COMPUTE: 4 };
globalThis.GPUBufferUsage ??= { MAP_READ: 1, MAP_WRITE: 2, COPY_SRC: 4, COPY_DST: 8, INDEX: 16, VERTEX: 32, UNIFORM: 64, STORAGE: 128,
  INDIRECT: 256, QUERY_RESOLVE: 512 };
globalThis.GPUTextureUsage ??= { COPY_SRC: 1, COPY_DST: 2, TEXTURE_BINDING: 4, STORAGE_BINDING: 8, RENDER_ATTACHMENT: 16 };

const stub = (props = {}) => new Proxy(props, {
  get: (t, k) => k in t ? t[k] : (typeof k === 'symbol' || k === 'then') ? undefined : () => stub() });
const device = () => stub({
  features: new Set(), queue: stub(),
  createShaderModule: d => stub({ label: d.label, getCompilationInfo: async () => ({ messages: [] }) }),
  pushErrorScope() {}, popErrorScope: async () => null,
  createRenderPipeline: d => stub({ desc: d, made: 'sync' }),
  createRenderPipelineAsync: async d => stub({ desc: d, made: 'async' }),
});

/** an encoder as a page attaches one: the module's stage count in the control block */
function session(stages) {
  const mem = new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true });
  const ctl = 1024;
  new Uint32Array(mem.buffer)[(ctl >> 2) + P.C.stages] = stages;
  const log = [];
  return { enc: new Encoder(device(), mem, ctl, s => log.push(s)), log };
}
const settle = () => new Promise(r => setImmediate(r));
/** a draw's pipeline as the draw loop makes it (biasKey, pipeKey, pipeline) */
function draw(enc, blend, ps, vs, cull, dm, nd, zb) {
  const zs = zb ? enc.biasKey(zb) : 0;
  const key = enc.pipeKey(blend, ps, vs, cull, dm, nd, zs);
  return { key, pipe: enc.pipes.get(key) ?? enc.pipeline(key, blend, ps, vs, cull, dm, nd, zb) };
}

const VS = '// vs a', VS2 = '// vs b', PS = '// ps 37', PSV = '// ps 37, stage 1 a depth texture', PSW = '// ps 37, stage 2 a depth texture';
const VAR = 37 | 2 << 9, VARW = 37 | 4 << 9;        // gpu_proto.h: ps bit 9 + k, stage k's texture a depth texture
const blend = 0x3C001, cull = 2, dm = 1 | 2 | 4 << 2, nd = true;
const ZB = 4 | 4 << 4;                              // Most Wanted's shadow pass: 2^-22 and 4.0 (GW_ZBIAS_SHIFT packing)

test('8 stages: a depth variant and its bias are warmed under the key its draws use', async () => {
  const a = session(P.STAGES_MAX);
  a.enc.shaderPut(true, 5, VS, 0); a.enc.shaderPut(true, 6, VS2, 0);
  a.enc.shaderPut(false, 37, PS, 0); a.enc.shaderPut(false, VAR, PSV, 2 << 2);
  draw(a.enc, blend, VAR, 5, cull, dm, nd, ZB);
  draw(a.enc, blend, 37, 6, cull, dm, nd, 0);
  const list = JSON.parse(JSON.stringify(a.enc.warmEntries()));
  assert.equal(list.length, 2);
  assert.equal(Math.floor(list[0].s / 134217728), ZB, 'the bias is in the persisted state');

  // the next session: another variant takes the first slot, the modules arrive in another order
  const b = session(P.STAGES_MAX);
  b.enc.loadWarm(list);
  b.enc.shaderPut(false, VARW, PSW, 2 << 4); b.enc.psKey(VARW);
  b.enc.shaderPut(false, VAR, PSV, 2 << 2); b.enc.shaderPut(false, 37, PS, 0);
  b.enc.shaderPut(true, 6, VS2, 0); b.enc.shaderPut(true, 5, VS, 0);
  await settle();
  assert.equal(b.enc.pipelinesAsync, 2);
  const v = draw(b.enc, blend, VAR, 5, cull, dm, nd, ZB).pipe;
  assert.equal(v.made, 'async', 'the variant draw finds the warmed pipeline');
  assert.equal(v.desc.fragment.module.label, `ps_${VAR}`);
  assert.equal(v.desc.vertex.module.label, 'vs_5');
  assert.equal(v.desc.layout, b.enc.layout8(2 << 2, false).pl, 'its layout has the depth binding');
  assert.equal(v.desc.depthStencil.depthBias, 4);
  assert.equal(v.desc.depthStencil.depthBiasSlopeScale, 4);
  // the key the raw id would give (ps 37 with vs 6): the plain pipeline's
  const p = draw(b.enc, blend, 37, 6, cull, dm, nd, 0).pipe;
  assert.equal(p.made, 'async');
  assert.equal(p.desc.fragment.module.label, 'ps_37');
  assert.equal(p.desc.vertex.module.label, 'vs_6');
  assert.equal(p.desc.depthStencil.depthBias, undefined);
  assert.equal(b.enc.pipelinesSync, 0);
});

test('4 stages (UG2): the persisted state and the warmed key are as before', async () => {
  const a = session(0);
  a.enc.shaderPut(true, 3, VS, 0); a.enc.shaderPut(false, 9, PS, 0);
  const { key } = draw(a.enc, blend, 9, 3, cull, dm, nd, 0);
  assert.equal(key, blend + 9 * 262144 + 3 * 268435456 + cull * 137438953472 + dm * 549755813888 + 35184372088832);
  const list = a.enc.warmEntries();
  assert.equal(list[0].s, blend + cull * 262144 + dm * 1048576 + 67108864);
  const b = session(0);
  b.enc.loadWarm(list); b.enc.shaderPut(false, 9, PS, 0); b.enc.shaderPut(true, 3, VS, 0);
  await settle();
  const p = b.enc.pipes.get(key);
  assert.equal(p.made, 'async');
  assert.equal(p.desc.depthStencil.depthBias, undefined);
});
