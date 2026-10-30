#!/usr/bin/env node
// DXT as ASTC (web/src/gpu/astc.ts) checked on a real GPU: random DXT1, DXT3 and DXT5 blocks of every kind go up as
// ASTC 4x4 (dxt1ToAstc, dxtAlphaToAstc) and as BC1/2/3, a draw reads every texel back (textureLoad into an RGBA8
// target), and each is compared with the CPU's unpacking (dxt.ts unpackDxt, what a GPU without BC shows now):
//   node web/tools/astc-check.mjs [--browser chrome|webkit|both] [--blocks N]
// (CHROME_ARGS: more Chrome arguments, space-separated: in WSL the SwiftShader ones.)
// Exits 1 when a texel is further off than the transcoding allows: DXT1's four-colour blocks 2 of 255 (BC1's thirds
// against ASTC's 21 and 43 of 64), three-colour 1 (the midpoint rounded), alpha exact; dual-plane blocks (a
// transparent texel in a texture already ASTC) 3 (7-bit endpoints), their transparent texels alpha 0.  DXT3 and DXT5
// blocks of one alpha: colour 2, alpha exact; of varying alpha (two planes, range-48 endpoints, 4 alpha levels): colour
// 8, alpha 48.  The GPU's DXT3 and DXT5 texels are also compared with the tests' reference decoder
// (web/test/astc-decode.mjs): a texel further than its 16-bit value rounded or cut to 8 bits fails.  BC on the same
// GPU is printed beside it: how far a GPU's own BC is from the CPU's unpacking.
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { webkit, chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const which = arg('--browser', 'both'), BLOCKS = Number(arg('--blocks', 16384));

const entry = `export { unpackDxt } from './web/src/gpu/dxt.ts'; export { dxt1ToAstc, dxt1HasPunch, dxtAlphaToAstc } from './web/src/gpu/astc.ts';
` +
  `export { decodeAstc4x4 } from './web/test/astc-decode.mjs';`;
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
    // drawn texel for texel into an RGBA8 target (a compute pass's textureLoad of a compressed texture reads 0 on
    // SwiftShader, Chrome 156 in WSL)
    const rt = device.createTexture({ size: [W, H], format: 'rgba8unorm', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
    const rb = device.createBuffer({ size: W * H * 4, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    const mod = device.createShaderModule({ code: `
      @group(0) @binding(0) var t: texture_2d<f32>;
      @vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
        let p = array(vec2f(-1, -1), vec2f(3, -1), vec2f(-1, 3));
        return vec4f(p[i], 0, 1);
      }
      @fragment fn fs(@builtin(position) q: vec4f) -> @location(0) vec4f { return textureLoad(t, vec2i(q.xy), 0); }` });
    const pipe = device.createRenderPipeline({ layout: 'auto', vertex: { module: mod, entryPoint: 'vs' },
      fragment: { module: mod, entryPoint: 'fs', targets: [{ format: 'rgba8unorm' }] } });
    const bg = device.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries: [{ binding: 0, resource: tex.createView() }] });
    const e = device.createCommandEncoder();
    const p = e.beginRenderPass({ colorAttachments: [{ view: rt.createView(), loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 0] }] });
    p.setPipeline(pipe); p.setBindGroup(0, bg); p.draw(3); p.end();
    e.copyTextureToBuffer({ texture: rt }, { buffer: rb, bytesPerRow: W * 4 }, [W, H, 1]); device.queue.submit([e.finish()]);
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

  // DXT3 and DXT5: kinds 0 one alpha 255, 1 one alpha below, 2 varying, 3 varying with 0 and 255 (DXT5: its six-level
  // mode); the colour blocks random, every eighth of extreme colours
  for (const dxt5 of [false, true]) {
    const s16 = new Uint8Array(bw * bh * 16), k4 = new Uint8Array(bw * bh);
    for (let i = 0; i < bw * bh; i++) {
      const kind = i % 4, o = i * 16;
      for (let j = 0; j < 16; j++) s16[o + j] = rnd() & 255;
      if ((i >> 2) % 8 === 0) {
        const c = [0, 0xFFFF, 0xF800, 0x07E0, 0x001F], c0 = c[rnd() % 5], c1 = c[rnd() % 5];
        s16.set([c0 & 255, c0 >> 8, c1 & 255, c1 >> 8], o + 8);
      }
      if (kind < 2) {
        const a = kind === 0 ? 255 : [0, 17, 136, 238][rnd() % 4];
        if (dxt5) { s16[o] = s16[o + 1] = a; s16.fill(0, o + 2, o + 8); } else s16.fill((a / 17) * 0x11, o, o + 8);
      } else if (dxt5) {
        if (kind === 3 && s16[o] > s16[o + 1]) [s16[o], s16[o + 1]] = [s16[o + 1], s16[o]];
        if (kind === 2 && s16[o] <= s16[o + 1]) [s16[o], s16[o + 1]] = [s16[o + 1] | 1, s16[o] & ~1];
      } else if (kind === 3) { s16[o] &= 0xF0; s16[o + 7] |= 0xF0; }
      k4[i] = kind;
    }
    const fmt = dxt5 ? 0x35545844 : 0x33545844, want3 = L.unpackDxt(fmt, s16, 0, bw * 16, W, H);
    const dual = { blocks: 0 }, a35 = L.dxtAlphaToAstc(dxt5, s16, 0, bw * 16, bw, bh, undefined, dual);
    const cmp = (px, ref) => {
      const r = [0, 1, 2, 3].map(() => ({ maxColour: 0, sumColour: 0, maxAlpha: 0, sumAlpha: 0, n: 0, notRef: 0 }));
      for (let b = 0; b < bw * bh; b++) {
        const dec = ref ? L.decodeAstc4x4(a35.subarray(b * 16, b * 16 + 16)) : null, s = r[k4[b]];
        if (typeof dec === 'string') { s.notRef += 16; continue; }
        for (let t = 0; t < 16; t++) {
          const x = (b % bw) * 4 + (t & 3), y = Math.floor(b / bw) * 4 + (t >> 2), i = y * W + x;
          const d = Math.max(Math.abs(px[4 * i] - want3[4 * i + 2]), Math.abs(px[4 * i + 1] - want3[4 * i + 1]), Math.abs(px[4 * i + 2] - want3[4 * i]));
          const da = Math.abs(px[4 * i + 3] - want3[4 * i + 3]);
          s.maxColour = Math.max(s.maxColour, d); s.sumColour += d; s.maxAlpha = Math.max(s.maxAlpha, da); s.sumAlpha += da; s.n++;
          // the 16-bit value as 8 bits: rounded or cut (SwiftShader cuts), never further
          if (dec && [0, 1, 2, 3].some(c => Math.abs(px[4 * i + c] - dec[t][c] / 257) >= 1)) s.notRef++;
        }
      }
      return r.map(s => ({ maxColour: s.maxColour, meanColour: +(s.sumColour / s.n).toFixed(3), maxAlpha: s.maxAlpha, meanAlpha: +(s.sumAlpha / s.n).toFixed(3), notRef: s.notRef }));
    };
    const key = dxt5 ? 'dxt5' : 'dxt3';
    res[key] = { dualPlane: dual.blocks, astc: cmp(await read('astc-4x4-unorm', a35, bw * 16), true) };
    if (bc) res[key].bc = cmp(await read(dxt5 ? 'bc3-rgba-unorm' : 'bc2-rgba-unorm', s16, bw * 16), false);
  }
  await device.queue.onSubmittedWorkDone();
  res.errors = errs;
  return res;
}

const LIMIT = [{ maxColour: 2 }, { maxColour: 1 }, { maxColour: 3 }, { maxColour: 0 }];
const KIND = ['four colours', 'three colours', 'three with holes (dual plane)', 'one colour'];
let failed = false;
for (const name of which === 'both' ? ['chrome', 'webkit'] : [which]) {
  const browser = name === 'chrome'
    ? await chromium.launch({ ...pickBrowser(), headless: true, args: ['--enable-unsafe-webgpu', ...(process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ') : [])] })
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
  const LIMIT35 = [{ maxColour: 2, maxAlpha: 0 }, { maxColour: 2, maxAlpha: 0 }, { maxColour: 8, maxAlpha: 48 }, { maxColour: 8, maxAlpha: 48 }];
  const KIND35 = ['one alpha, opaque', 'one alpha, below 255', 'varying alpha (dual plane)', 'varying, with 0 and 255'];
  for (const key of ['dxt3', 'dxt5']) {
    console.log(`  ${key.toUpperCase()}: ${r[key].dualPlane} dual-plane`);
    r[key].astc.forEach((s, k) => {
      const bad = s.maxColour > LIMIT35[k].maxColour || s.maxAlpha > LIMIT35[k].maxAlpha || s.notRef;
      if (bad) failed = true;
      const b = r[key].bc ? `   BC on this GPU: colour max ${r[key].bc[k].maxColour}, alpha max ${r[key].bc[k].maxAlpha}` : '';
      console.log(`    ${bad ? 'FAIL' : 'ok  '} ${KIND35[k].padEnd(28)} ASTC: colour max ${s.maxColour} mean ${s.meanColour}, alpha max ${s.maxAlpha} mean ${s.meanAlpha}, texels off the reference decoder ${s.notRef}${b}`);
    });
  }
  if (r.errors.length) failed = true;
}
process.exit(failed ? 1 : 0);
