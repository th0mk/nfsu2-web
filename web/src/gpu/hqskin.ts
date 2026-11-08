// HQ vinyls (runtime/hqvinyls.c, the Mods tab's "HQ Vinyls & Doorlines" by AdrienShadow, nfsmods.xyz/mod/8064): a car
// skin the game has just painted at 512x512 (DUMMY_SKIN<n>: base paint, vinyl layers, doorline) is painted again here
// at 1024x1024 from the mod's pictures, with the game's own arithmetic (0x611F10, 0x611B00), and shown in its place
// (encoder.ts texFor) until the game paints that texture again.
//
// The pictures: HQVINYLS/CARS/<car>.hqv in the game folder (tools/hqvinyls.py: rows by name hash, AVIF pictures), each
// read as a byte range of its own (the I/O worker; a vinyl's picture and mask ~30 KB, where the game data's 1 MiB
// blocks would fetch ~1 MB for each), kept here for the session, and decoded by the browser.  Offline, or where the
// pack is not there, the game's skins stay.
// The game's 512 skin stays on the GPU under it: the paint swatches (the parts painted in the body's colour take it
// from there) are copied from it, and it is what shows until the 1024 one is ready, or for good when a picture is
// missing (Art Factory 28 to 34 are not the mod's) or the pack is not there.
import type { GwSkinHqLayout } from './proto';

export type DataReader = (path: string, off: number, len: number) => Promise<Uint8Array<ArrayBuffer>>;

const SIDE = 1024, GAME = 512, LEVELS = 11;
const MAGIC = 0x31565148;                 // "HQV1"
const ROW = 16, ROWS_MAX = 4096;
const PLANE = 0x100;                      // a layer's alpha: the game's 512 plane (GW_SKIN_HQ_PLANE)
const BYTES_KEPT = 8 << 20;               // pictures' bytes kept for the next paint (a shop's previews repaint often)
const AT_ONCE = 2;                        // paints decoding at once
const LOG_MAX = 40;
const RETRY_MS = 60_000;                  // a car's pack that did not come: asked again after this

/** a 1x1 AVIF: whether this browser decodes AVIF */
const TINY_AVIF = 'AAAAHGZ0eXBhdmlmAAAAAGF2aWZtaWYxbWlhZgAAAOltZXRhAAAAAAAAACFoZGxyAAAAAAAAAABwaWN0AAAAAAAAAAAAAAAAAAAAAA5waXRtAAAAAAABAAAAHmlsb2MAAAAARAAAAQABAAAAAQAAAQ0AAAAYAAAAKGlpbmYAAAAAAAEAAAAaaW5mZQIAAAAAAQAAYXYwMUNvbG9yAAAAAGhpcHJwAAAASWlwY28AAAAUaXNwZQAAAAAAAAABAAAAAQAAAA5waXhpAAAAAAEIAAAADGF2MUOBABwAAAAAE2NvbHJuY2x4AAEADQAGgAAAABdpcG1hAAAAAAAAAAEAAQQBAoMEAAAAIG1kYXQSAAoHGAAGmAhoNTILGUYhh55oAJBA2aA=';
export async function avifDecodes(): Promise<boolean> {
  try {
    const b = Uint8Array.from(atob(TINY_AVIF), ch => ch.charCodeAt(0));
    const bmp = await createImageBitmap(new Blob([b], { type: 'image/avif' }));
    const ok = bmp.width === 1 && bmp.height === 1;
    bmp.close();
    return ok;
  } catch { return false; }
}

// One pass over the 1024 target.  u: [0] base paint RGBA, [1].x layers, then per layer 4 words: (remap, alpha,
// plane, 0), colours 0, 1, 2 as RGBA (0..255).  The arithmetic is the game's, in integers (0x611B00's float form
// equals it for every byte pair): a REMAP layer's colour is sum(colour_i * w_i / 255) with the picture's R, G, B as
// w (0 and 1 dropped), each term truncated, clamped to 255, alpha the largest colour_i.a * w_i / 255; a mask of
// 1..127 blends by m/128, more is drawn as it is.  Rows 1016-1023 from x 968: the swatches, the game's 512 skin's.
const WGSL = /* wgsl */`
@group(0) @binding(0) var<uniform> u: array<vec4<u32>, 34>;
@group(0) @binding(1) var pics: texture_2d_array<f32>;
@group(0) @binding(2) var masks: texture_2d_array<f32>;
@group(0) @binding(3) var planes: texture_2d_array<f32>;
@group(0) @binding(4) var game: texture_2d<f32>;
@group(0) @binding(5) var smp: sampler;

@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
  let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
  return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
}
fn b8(x: f32) -> u32 { return u32(clamp(x, 0.0, 1.0) * 255.0 + 0.5); }
@fragment fn fs(@builtin(position) fp: vec4<f32>) -> @location(0) vec4<f32> {
  let p = vec2<i32>(fp.xy);
  if (p.y >= 1016 && p.x >= 968) { return textureLoad(game, p / 2, 0); }
  var d = u[0];
  let n = u[1].x;
  for (var i = 0u; i < n; i++) {
    let li = i32(i);
    let m = b8(textureLoad(masks, p, li, 0).r);
    if (m == 0u) { continue; }
    let L = u[2u + 4u * i];
    let px = textureLoad(pics, p, li, 0);
    var s = vec4<u32>(b8(px.r), b8(px.g), b8(px.b), 255u);
    if (L.x != 0u) {
      let v = select(vec3<u32>(0u), s.rgb, s.rgb >= vec3<u32>(2u));
      let c0 = u[3u + 4u * i]; let c1 = u[4u + 4u * i]; let c2 = u[5u + 4u * i];
      let rgb = (c0.rgb * v.x) / 255u + (c1.rgb * v.y) / 255u + (c2.rgb * v.z) / 255u;
      let a = max(max((c0.a * v.x) / 255u, (c1.a * v.y) / 255u), (c2.a * v.z) / 255u);
      s = vec4<u32>(min(rgb, vec3<u32>(255u)), min(a, 255u));
    } else if (L.y == ${PLANE}u) {
      s.a = b8(textureSampleLevel(planes, smp, fp.xy / ${SIDE}.0, i32(L.z), 0.0).r);
    } else {
      s.a = L.y;
    }
    if (m > 127u) { d = s; }
    else { d = ((s * m) >> vec4<u32>(7u)) + ((d * (128u - m)) >> vec4<u32>(7u)); }
  }
  return vec4<f32>(d) / 255.0;
}`;

interface Row { off: number; size: number }
interface Layer { hash: number; mask: number; remap: boolean; alpha: number; col: number[] }
interface Recipe { serial: number; car: string; base: number; layers: Layer[] }
/** a skin to paint: the recipe's bytes, the alpha planes after it; game() = the game's 512 skin's view while it is
 *  still the one this recipe painted (null once it is not: the paint is dropped) */
export interface SkinJob { id: number; recipe: Uint8Array; planes: Uint8Array; nplanes: number; game: () => GPUTextureView | null }

export class HqSkins {
  private packs = new Map<string, Promise<Map<number, Row> | null>>();
  private kept = new Map<string, Uint8Array<ArrayBuffer>>();
  private keptBytes = 0;
  private waiting: { job: SkinJob; done: (t: GPUTexture | null) => void }[] = [];
  private running = 0;
  private logged = 0;
  private missing = new Set<string>();
  private pipe: GPURenderPipeline | null = null;
  private bgl: GPUBindGroupLayout | null = null;
  private smp: GPUSampler;
  painted = 0; failed = 0;

  constructor(private device: GPUDevice, private read: DataReader, private log: (s: string) => void,
              private mips: (enc: GPUCommandEncoder, tex: GPUTexture) => void, private layout: GwSkinHqLayout) {
    this.smp = device.createSampler({ minFilter: 'linear', magFilter: 'linear' });
  }

  /** paint a skin at 1024: the texture (LEVELS levels, bgra8unorm), or null (left to the game's) */
  paint(job: SkinJob): Promise<GPUTexture | null> {
    return new Promise(done => { this.waiting.push({ job, done }); this.next(); });
  }

  private next(): void {
    while (this.running < AT_ONCE && this.waiting.length) {
      const w = this.waiting.shift()!;
      this.running++;
      this.run(w.job).then(w.done, e => { this.failed++; this.say(`[hqskin] t${w.job.id}: ${e instanceof Error ? e.message : e}`); w.done(null); })
        .finally(() => { this.running--; this.next(); });
    }
  }

  private say(s: string): void {
    if (this.logged < LOG_MAX) { this.logged++; this.log(this.logged === LOG_MAX ? `${s} (no more of these lines)` : s); }
  }

  private parse(b: Uint8Array): Recipe | null {
    const L = this.layout, v = new DataView(b.buffer, b.byteOffset, b.byteLength);
    if (b.byteLength < L.size || v.getUint32(L.magic, true) !== L.MAGIC) return null;
    const n = v.getUint32(L.n, true);
    if (n < 1 || n > L.LAYERS) return null;
    let car = '';
    for (let k = 0; k < 16; k++) { const ch = b[L.car + k]; if (!ch) break; car += String.fromCharCode(ch); }
    if (!/^[A-Z0-9]{1,15}$/.test(car)) return null;
    const layers: Layer[] = [];
    for (let i = 0; i < n; i++) {
      const o = L.layer + i * L.layerSize;
      layers.push({ hash: v.getUint32(o + L.lHash, true), mask: v.getUint32(o + L.lMask, true), remap: v.getUint32(o + L.lRemap, true) !== 0,
        alpha: v.getUint32(o + L.lAlpha, true), col: [0, 1, 2, 3].map(k => v.getUint32(o + L.lCol + 4 * k, true)) });
    }
    return { serial: v.getUint32(L.serial, true), car, base: v.getUint32(L.base, true), layers };
  }

  /** a car's pack's rows: name hash -> its picture; null when it is not there */
  private pack(car: string): Promise<Map<number, Row> | null> {
    let p = this.packs.get(car);
    if (!p) {
      const path = `HQVINYLS/CARS/${car}.hqv`;
      p = (async () => {
        try {
          const h = await this.read(path, 0, 16), v = new DataView(h.buffer, h.byteOffset, h.byteLength);
          if (h.byteLength < 16 || v.getUint32(0, true) !== MAGIC || v.getUint32(4, true) !== 1) throw new Error('not a HQV1 file');
          const n = v.getUint32(8, true), start = v.getUint32(12, true);
          if (n < 1 || n > ROWS_MAX || start < 16 + ROW * n) throw new Error(`${n} rows from ${start}`);
          const r = await this.read(path, 16, ROW * n), rv = new DataView(r.buffer, r.byteOffset, r.byteLength);
          if (r.byteLength !== ROW * n) throw new Error('cut short');
          const rows = new Map<number, Row>();
          for (let i = 0; i < n; i++) {
            const off = rv.getUint32(ROW * i + 4, true), size = rv.getUint32(ROW * i + 8, true);
            if (off >= start && size > 0 && size < (4 << 20)) rows.set(rv.getUint32(ROW * i, true), { off, size });
          }
          return rows;
        } catch (e) {
          // asked again after a while (the network may be back; a pack the folder lacks costs a 404 a minute)
          this.say(`[hqskin] ${path}: ${e instanceof Error ? e.message : e}; this car keeps the game's skins for now`);
          setTimeout(() => { if (this.packs.get(car) === p) this.packs.delete(car); }, RETRY_MS);
          return null;
        }
      })();
      this.packs.set(car, p);
    }
    return p;
  }

  private async bytes(car: string, hash: number, row: Row): Promise<Uint8Array<ArrayBuffer>> {
    const key = `${car}/${hash}`, hit = this.kept.get(key);
    if (hit) { this.kept.delete(key); this.kept.set(key, hit); return hit; }
    const b = await this.read(`HQVINYLS/CARS/${car}.hqv`, row.off, row.size);
    if (b.byteLength !== row.size) throw new Error(`${car} 0x${(hash >>> 0).toString(16)}: cut short`);
    this.kept.set(key, b); this.keptBytes += b.byteLength;
    for (const [k, v] of this.kept) { if (this.keptBytes <= BYTES_KEPT) break; this.kept.delete(k); this.keptBytes -= v.byteLength; }
    return b;
  }

  private async run(job: SkinJob): Promise<GPUTexture | null> {
    const t0 = performance.now();
    const r = this.parse(job.recipe);
    if (!r) throw new Error('a recipe this page does not read');
    const rows = await this.pack(r.car);
    if (!rows) return null;
    const want = r.layers.flatMap(l => [l.hash, l.mask]);
    for (const h of want) if (!rows.has(h)) {
      const k = `${r.car}/${h}`;
      if (!this.missing.has(k)) { this.missing.add(k); this.say(`[hqskin] ${r.car}: picture 0x${(h >>> 0).toString(16)} not in its pack (not the mod's): this skin stays the game's`); }
      return null;
    }
    const got = await Promise.all(want.map(h => this.bytes(r.car, h, rows.get(h)!)));
    const bmps = await Promise.all(got.map(b => createImageBitmap(new Blob([b], { type: 'image/avif' }),
      { premultiplyAlpha: 'none', colorSpaceConversion: 'none' })));
    try {
      if (bmps.some(b => b.width !== SIDE || b.height !== SIDE)) throw new Error('a picture not 1024x1024');
      const game = job.game();
      if (!game) return null;                                     // painted again (or gone) meanwhile
      const tex = this.compose(r, bmps, job, game);
      this.painted++;
      this.say(`[hqskin] t${job.id} ${r.car}: 1024 in ${Math.round(performance.now() - t0)} ms (${r.layers.length} layer(s), ` +
        `${Math.round(got.reduce((a, b) => a + b.byteLength, 0) / 1024)} KB)`);
      return tex;
    } finally { for (const b of bmps) b.close(); }
  }

  private compose(r: Recipe, bmps: ImageBitmap[], job: SkinJob, game: GPUTextureView): GPUTexture {
    const d = this.device, n = r.layers.length;
    const arr = (format: GPUTextureFormat, side: number, layers: number, label: string) => d.createTexture({ label, format,
      size: [side, side, Math.max(1, layers)], usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT });
    const pics = arr('rgba8unorm', SIDE, n, 'hqskin pics'), masks = arr('r8unorm', SIDE, n, 'hqskin masks');
    const planes = arr('r8unorm', GAME, job.nplanes, 'hqskin planes');
    for (let i = 0; i < n; i++) {
      d.queue.copyExternalImageToTexture({ source: bmps[2 * i] }, { texture: pics, origin: [0, 0, i] }, [SIDE, SIDE]);
      d.queue.copyExternalImageToTexture({ source: bmps[2 * i + 1] }, { texture: masks, origin: [0, 0, i] }, [SIDE, SIDE]);
    }
    for (let k = 0; k < job.nplanes; k++)
      d.queue.writeTexture({ texture: planes, origin: [0, 0, k] }, job.planes.subarray(k * GAME * GAME, (k + 1) * GAME * GAME),
        { bytesPerRow: GAME }, [GAME, GAME]);
    const u = new Uint32Array(34 * 4), rgba = (c: number, o: number) => { u[o] = c & 255; u[o + 1] = (c >>> 8) & 255; u[o + 2] = (c >>> 16) & 255; u[o + 3] = c >>> 24; };
    rgba(r.base, 0); u[4] = n;
    let plane = 0;
    r.layers.forEach((l, i) => {
      const o = (2 + 4 * i) * 4;
      u[o] = l.remap ? 1 : 0; u[o + 1] = l.remap ? 0 : l.alpha; u[o + 2] = !l.remap && l.alpha === PLANE ? plane++ : 0;
      for (let k = 0; k < 3; k++) rgba(l.col[k], o + 4 + 4 * k);
    });
    const ubuf = d.createBuffer({ size: u.byteLength, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    d.queue.writeBuffer(ubuf, 0, u);
    const out = d.createTexture({ label: `hqskin t${job.id}`, format: 'bgra8unorm', size: [SIDE, SIDE], mipLevelCount: LEVELS,
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
    if (!this.pipe) {
      const mod = d.createShaderModule({ label: 'hqskin', code: WGSL });
      this.bgl = d.createBindGroupLayout({ entries: [
        { binding: 0, visibility: GPUShaderStage.FRAGMENT, buffer: { type: 'uniform' } },
        { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { viewDimension: '2d-array', sampleType: 'float' } },
        { binding: 2, visibility: GPUShaderStage.FRAGMENT, texture: { viewDimension: '2d-array', sampleType: 'float' } },
        { binding: 3, visibility: GPUShaderStage.FRAGMENT, texture: { viewDimension: '2d-array', sampleType: 'float' } },
        { binding: 4, visibility: GPUShaderStage.FRAGMENT, texture: { viewDimension: '2d', sampleType: 'float' } },
        { binding: 5, visibility: GPUShaderStage.FRAGMENT, sampler: {} }] });
      this.pipe = d.createRenderPipeline({ label: 'hqskin', layout: d.createPipelineLayout({ bindGroupLayouts: [this.bgl] }),
        vertex: { module: mod, entryPoint: 'vs' }, fragment: { module: mod, entryPoint: 'fs', targets: [{ format: 'bgra8unorm' }] },
        primitive: { topology: 'triangle-list' } });
    }
    const bg = d.createBindGroup({ layout: this.bgl!, entries: [
      { binding: 0, resource: { buffer: ubuf } },
      { binding: 1, resource: pics.createView({ dimension: '2d-array' }) },
      { binding: 2, resource: masks.createView({ dimension: '2d-array' }) },
      { binding: 3, resource: planes.createView({ dimension: '2d-array' }) },
      { binding: 4, resource: game },
      { binding: 5, resource: this.smp }] });
    const enc = d.createCommandEncoder({ label: 'hqskin' });
    const pass = enc.beginRenderPass({ colorAttachments: [{ view: out.createView({ baseMipLevel: 0, mipLevelCount: 1 }), loadOp: 'clear',
      clearValue: [0, 0, 0, 0], storeOp: 'store' }] });
    pass.setPipeline(this.pipe); pass.setBindGroup(0, bg); pass.draw(3); pass.end();
    this.mips(enc, out);
    d.queue.submit([enc.finish()]);
    pics.destroy(); masks.destroy(); planes.destroy(); ubuf.destroy();
    return out;
  }
}
