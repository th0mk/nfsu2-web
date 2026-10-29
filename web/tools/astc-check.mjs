#!/usr/bin/env node
// DXT1 as ASTC (web/src/gpu/astc.ts) checked on a real GPU: random DXT1 blocks of every kind go up as ASTC 4x4
// (dxt1ToAstc) and as BC1, a compute pass reads every texel back (textureLoad), and each is compared with the CPU's
// unpacking (dxt.ts unpackDxt, what a GPU without BC shows now):
//   node web/tools/astc-check.mjs [--browser chrome|webkit|both] [--blocks N]
// Exits 1 when a texel is further off than the transcoding allows: four-colour blocks 2 of 255 (BC1's thirds against
// ASTC's 21 and 43 of 64), three-colour 1 (the midpoint rounded), alpha exact; dual-plane blocks (a transparent
// texel in a texture already ASTC) 3 (7-bit endpoints), their transparent texels alpha 0.  BC1 on the same GPU is
// printed beside it: how far a GPU's own BC1 is from the CPU's unpacking.
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { webkit, chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const which = arg('--browser', 'both'), BLOCKS = Number(arg('--blocks', 16384));

const entry = `export { unpackDxt } from './web/src/gpu/dxt.ts'; export { dxt1ToAstc, dxt1HasPunch } from './web/src/gpu/astc.ts';`;
const lib = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;

// runs in the page: lib's exports as L
async function inPage({ BLOCKS }) {
  const L = window.L, DXT1 = 0x31545844;
  const adapter = await navigator.gpu?.requestAdapter();
  if (!adapter) return { skip: 'no WebGPU adapter' };
  if (!adapter.features.has('texture-compression-astc')) return { skip: 'no texture-compression-astc' };
  const bc = adapter.features.has('texture-compression-bc');
  const device = await adapter.requestDevice({ requiredFeatures: ['texture-compression-astc', ...(bc ? ['texture-compression-bc'] : [])] });
  const errs = []; device.addEventListener('uncapturederror', e => errs.push(e.error.message));

  // the blocks: a fixed pseudo-random sequence, every kind of block in turn
  let seed = 12345;
  const rnd = () => (seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) >>> 8;
  const bw = 128, bh = Math.ceil(BLOCKS / bw), W = bw * 4, H = bh * 4, src = new Uint8Array(bw * bh * 8);
  const kinds = new Uint8Array(bw * bh);     // 0 four colours, 1 three (no hole), 2 three with holes, 3 one colour
  for (let i = 0; i < bw * bh; i++) {
    const kind = i % 4, extreme = (i >> 2) % 7 === 0;
    let c0 = extreme ? [0, 0xFFFF, 0xF800, 0x07E0, 0x001F][rnd() % 5] : rnd() & 0xFFFF;
    let c1 = extreme ? [0, 0xFFFF, 0xF800, 0x07E0, 0x001F][rnd() % 5] : rnd() & 0xFFFF;
    if (kind === 3) c1 = c0;
    else if (c0 === c1) c1 = c0 ^ 1;
    if ((kind === 0) !== (c0 > c1) && kind !== 3) [c0, c1] = [c1, c0];
    let idx = (rnd() << 8 ^ rnd()) >>> 0;
    if (kind === 1 || kind === 3) { let m = 0; for (let t = 0; t < 16; t++) if (((idx >>> 2 * t) & 3) === 3) m |= 1 << 2 * t; idx = (idx & ~m) >>> 0; }
    if (kind === 2 && !L.dxt1HasPunch(new Uint8Array([c0, c0 >> 8, c1, c1 >> 8, idx, idx >> 8, idx >> 16, idx >>> 24]), 0, 8, 1, 1)) idx = (idx | 3) >>> 0;
    if (kind === 3 && c0 > c1) c0 = c1;     // one colour: c0 = c1, three-colour mode
    src.set([c0 & 255, c0 >> 8, c1 & 255, c1 >> 8, idx & 255, (idx >> 8) & 255, (idx >> 16) & 255, idx >>> 24], i * 8);
    kinds[i] = kind;
  }
  const want = L.unpackDxt(DXT1, src, 0, bw * 8, W, H);           // BGRA
  const punch = { blocks: 0 };
  const astc = L.dxt1ToAstc(src, 0, bw * 8, bw, bh, undefined, punch);

  const read = async (format, data, bytesPerRow) => {
    const tex = device.createTexture({ size: [W, H], format, usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
    device.queue.writeTexture({ texture: tex }, data, { bytesPerRow, rowsPerImage: bh }, [W, H, 1]);
    const out = device.createBuffer({ size: W * H * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
    const rb = device.createBuffer({ size: W * H * 4, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    const mod = device.createShaderModule({ code: `
      @group(0) @binding(0) var t: texture_2d<f32>;
      @group(0) @binding(1) var<storage, read_write> o: array<u32>;
      @compute @workgroup_size(8, 8) fn main(@builtin(global_invocation_id) g: vec3u) {
        if (g.x >= ${W}u || g.y >= ${H}u) { return; }
        o[g.y * ${W}u + g.x] = pack4x8unorm(textureLoad(t, vec2i(g.xy), 0));
      }` });
    const pipe = device.createComputePipeline({ layout: 'auto', compute: { module: mod, entryPoint: 'main' } });
    const bg = device.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries: [{ binding: 0, resource: tex.createView() }, { binding: 1, resource: { buffer: out } }] });
    const e = device.createCommandEncoder(), p = e.beginComputePass();
    p.setPipeline(pipe); p.setBindGroup(0, bg); p.dispatchWorkgroups(W / 8, H / 8); p.end();
    e.copyBufferToBuffer(out, 0, rb, 0, W * H * 4); device.queue.submit([e.finish()]);
    await rb.mapAsync(GPUMapMode.READ);
    return new Uint8Array(rb.getMappedRange().slice(0));         // RGBA
  };
  const compare = (got) => {
    // per kind: the largest colour difference, alpha mismatches; holes: their alpha only
    const r = [0, 1, 2, 3].map(() => ({ maxColour: 0, alphaWrong: 0, holeAlphaWrong: 0, sumColour: 0, n: 0 }));
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      const i = y * W + x, k = kinds[(y >> 2) * bw + (x >> 2)], s = r[k];
      const wr = want[4 * i + 2], wg = want[4 * i + 1], wb = want[4 * i], wa = want[4 * i + 3];
      const gr = got[4 * i], gg = got[4 * i + 1], gb = got[4 * i + 2], ga = got[4 * i + 3];
      if (wa === 0) { if (ga !== 0) s.holeAlphaWrong++; continue; }
      if (ga !== wa) s.alphaWrong++;
      const d = Math.max(Math.abs(gr - wr), Math.abs(gg - wg), Math.abs(gb - wb));
      s.maxColour = Math.max(s.maxColour, d); s.sumColour += d; s.n++;
    }
    return r.map(s => ({ maxColour: s.maxColour, meanColour: +(s.sumColour / Math.max(1, s.n)).toFixed(3), alphaWrong: s.alphaWrong, holeAlphaWrong: s.holeAlphaWrong }));
  };
  const res = { blocks: bw * bh, dualPlane: punch.blocks, astc: compare(await read('astc-4x4-unorm', astc, bw * 16)) };
  if (bc) res.bc1 = compare(await read('bc1-rgba-unorm', src, bw * 8));
  await device.queue.onSubmittedWorkDone();
  res.errors = errs;
  return res;
}

const LIMIT = [{ maxColour: 2 }, { maxColour: 1 }, { maxColour: 3 }, { maxColour: 0 }];
const KIND = ['four colours', 'three colours', 'three with holes (dual plane)', 'one colour'];
let failed = false;
for (const name of which === 'both' ? ['chrome', 'webkit'] : [which]) {
  const browser = name === 'chrome'
    ? await chromium.launch({ ...pickBrowser(), headless: true, args: ['--enable-unsafe-webgpu'] })
    : await webkit.launch({ headless: false });
  const page = await browser.newPage();
  await page.route('https://astc.test/', r => r.fulfill({ contentType: 'text/html', body: '<!doctype html><title>astc</title>' }));
  await page.goto('https://astc.test/');
  await page.addScriptTag({ type: 'module', content: lib.replace(/export\s*\{([^}]*)\};?\s*$/, (_, names) => `window.L = { ${names.replace(/(\w+) as (\w+)/g, '$2: $1')} };`) });
  await page.waitForFunction(() => window.L);
  const r = await page.evaluate(inPage, { BLOCKS });
  await browser.close();
  if (r.skip) { console.log(`${name}: skipped (${r.skip})`); continue; }
  console.log(`${name}: ${r.blocks} blocks, ${r.dualPlane} dual-plane${r.errors.length ? `, WebGPU errors: ${r.errors.join('; ')}` : ''}`);
  r.astc.forEach((s, k) => {
    const bad = s.maxColour > LIMIT[k].maxColour || s.alphaWrong || s.holeAlphaWrong;
    if (bad) failed = true;
    const bc1 = r.bc1 ? `   BC1 on this GPU: max ${r.bc1[k].maxColour} mean ${r.bc1[k].meanColour}` : '';
    console.log(`  ${bad ? 'FAIL' : 'ok  '} ${KIND[k].padEnd(30)} ASTC: max ${s.maxColour} mean ${s.meanColour}, alpha wrong ${s.alphaWrong}, holes not transparent ${s.holeAlphaWrong}${bc1}`);
  });
  if (r.errors.length) failed = true;
}
process.exit(failed ? 1 : 0);
