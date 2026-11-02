// Photo mode's picture (runtime/photo.c, GwPhoto in runtime/web/gpu_proto.h).  While photo mode is on, every frame's
// back buffer goes into an average (rgba16float, the back buffer's size): a frame of a moving camera replaces it, and
// once the camera stands still the game draws each frame from a slightly different spot (within a pixel: smooth edges;
// the eye over a lens: the blur away from the focus), which add up, frame n weighing 1/n.  The canvas shows that
// average with the picture's look (photo mode's panel: exposure, filter, vignette) and the crop's preview (the rest
// dimmed, lines of thirds), and F12 keeps it: the same look at the picture's own size, the crop's rectangle of it, read
// back (index.ts).  The look is wgsl.ts PHOTO.
import { PHOTO } from './wgsl';

export const PH_OFF = 0, PH_LIVE = 1, PH_ADD = 2, PH_DONE = 3, PH_NOW = 4;

/** the picture's look (app/photo.ts) */
export interface PhotoLook {
  exposure: number;                     // times
  contrast: number;                     // 1 as drawn
  saturation: number;                   // 1 as drawn, 0 grey
  tint: [number, number, number];       // times, per channel
  fade: number;                         // blacks lifted to this
  vignette: number;                     // the corners darker by this part
  crop: [number, number, number, number]; // the part kept: left, top, right, bottom (0 .. 1 of the picture)
  grid: boolean;                        // the preview's lines of thirds
}
export const PLAIN_LOOK: PhotoLook = {
  exposure: 1, contrast: 1, saturation: 1, tint: [1, 1, 1], fade: 0, vignette: 0, crop: [0, 0, 1, 1], grid: false,
};

export class PhotoPicture {
  private mod: GPUShaderModule;
  private bgl: GPUBindGroupLayout;
  private addPipe: GPURenderPipeline;
  private lookPipes = new Map<GPUTextureFormat, GPURenderPipeline>();
  private bufShow: GPUBuffer; private bufShot: GPUBuffer;
  private args = new Float32Array(20);
  private acc: GPUTexture | null = null; private accView: GPUTextureView | null = null;
  private shotTex: GPUTexture | null = null; private shotView: GPUTextureView | null = null;
  private bgs = new WeakMap<GPUTextureView, Map<string, GPUBindGroup>>();
  /** the frames in the average */
  count = 0;
  /** the frames the still picture will have (0: the camera moves), from the game */
  target = 0;
  /** the average holds a whole still picture */
  done = false;

  constructor(private device: GPUDevice) {
    const d = device;
    this.mod = d.createShaderModule({ label: 'photo', code: PHOTO });
    this.bgl = d.createBindGroupLayout({ entries: [
      { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
      { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
      { binding: 2, visibility: GPUShaderStage.FRAGMENT, buffer: { type: 'uniform' } }] });
    const layout = d.createPipelineLayout({ bindGroupLayouts: [this.bgl] });
    // the new frame times the constant plus the average times one minus it: 1 replaces, 1/n adds the n-th
    const blend: GPUBlendComponent = { srcFactor: 'constant', dstFactor: 'one-minus-constant', operation: 'add' };
    this.addPipe = d.createRenderPipeline({ label: 'photo add', layout,
      vertex: { module: this.mod, entryPoint: 'vs_full' },
      fragment: { module: this.mod, entryPoint: 'fs_add', targets: [{ format: 'rgba16float', blend: { color: blend, alpha: blend } }] },
      primitive: { topology: 'triangle-list' } });
    this.bufShow = d.createBuffer({ label: 'photo look', size: 80, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    this.bufShot = d.createBuffer({ label: 'photo look (picture)', size: 80, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  }

  private bg(src: GPUTextureView, smp: GPUSampler, buf: GPUBuffer, key: string): GPUBindGroup {
    let m = this.bgs.get(src);
    if (!m) this.bgs.set(src, m = new Map());
    let g = m.get(key);
    if (!g) m.set(key, g = this.device.createBindGroup({ layout: this.bgl, entries: [
      { binding: 0, resource: src }, { binding: 1, resource: smp }, { binding: 2, resource: { buffer: buf } }] }));
    return g;
  }

  private lookPipe(format: GPUTextureFormat): GPURenderPipeline {
    let p = this.lookPipes.get(format);
    if (!p) this.lookPipes.set(format, p = this.device.createRenderPipeline({ label: `photo look ${format}`,
      layout: this.device.createPipelineLayout({ bindGroupLayouts: [this.bgl] }),
      vertex: { module: this.mod, entryPoint: 'vs_full' },
      fragment: { module: this.mod, entryPoint: 'fs_look', targets: [{ format }] },
      primitive: { topology: 'triangle-list' } }));
    return p;
  }

  /** still picture gen's frames in the average (for the game: GwPhoto ack) */
  gen = 0;

  /** a frame of photo mode: its back buffer (w x h) into the average, as the game's state says: a moving camera's
   *  replaces it; frame n of still picture gen adds to it when it is the next one (the game draws a frame again until
   *  it is in: frames the page skips never leave a gap, and a frame drawn twice counts once) */
  add(enc: GPUCommandEncoder, src: GPUTextureView, w: number, h: number, smp: GPUSampler, state: number, n: number, target: number, gen: number) {
    if (!this.acc || this.acc.width !== w || this.acc.height !== h) {
      this.acc?.destroy(); this.shotTex?.destroy(); this.shotTex = this.shotView = null;
      this.acc = this.device.createTexture({ label: 'photo average', size: [w, h], format: 'rgba16float',
        usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING });
      this.accView = this.acc.createView();
      this.count = 0; this.gen = 0;
    }
    if (state === PH_ADD || state === PH_DONE) {
      if (gen !== this.gen) { this.gen = gen; this.count = 0; }
      this.target = target; this.done = this.count > 0 && this.count >= target;
      if (state === PH_DONE || n !== this.count + 1) return;
      this.count = n; this.done = n >= target;
    } else { this.count = 0; this.target = 0; this.gen = 0; this.done = state === PH_NOW; }   // (now: this frame is it)
    const k = this.count > 0 ? 1 / this.count : 1;
    const pass = enc.beginRenderPass({ colorAttachments: [{ view: this.accView!, loadOp: k === 1 ? 'clear' : 'load', clearValue: [0, 0, 0, 1], storeOp: 'store' }] });
    pass.setPipeline(this.addPipe);
    pass.setBindGroup(0, this.bg(src, smp, this.bufShow, 'add'));
    pass.setBlendConstant([k, k, k, k]);
    pass.draw(3);
    pass.end();
  }

  private fill(look: PhotoLook, tone: ArrayLike<number> | null, preview: [number, number] | null) {
    const a = this.args, c = look.crop;
    a.set([look.exposure, look.contrast, look.saturation, look.vignette,
      look.tint[0], look.tint[1], look.tint[2], look.fade,
      tone ? tone[0] : 1, tone ? tone[1] : 1, tone ? tone[2] : 1, tone ? 1 : 0,
      c[0], c[1], c[2], c[3],
      look.grid ? 1 : 0, preview ? 1 : 0, preview ? preview[0] * (c[2] - c[0]) : 0, preview ? preview[1] * (c[3] - c[1]) : 0]);
  }

  /** the average with its look into dst (the canvas), in the viewport vp, with Exposure and White Level (tone) and the
   *  crop's preview */
  show(enc: GPUCommandEncoder, dst: GPUTextureView, format: GPUTextureFormat, smp: GPUSampler, vp: [number, number, number, number],
       look: PhotoLook, tone: ArrayLike<number> | null, ts?: GPURenderPassTimestampWrites) {
    if (!this.accView) return;
    this.fill(look, tone, [vp[2], vp[3]]);
    this.device.queue.writeBuffer(this.bufShow, 0, this.args);
    const pass = enc.beginRenderPass({ colorAttachments: [{ view: dst, loadOp: 'clear', clearValue: [0, 0, 0, 1], storeOp: 'store' }], timestampWrites: ts });
    pass.setPipeline(this.lookPipe(format));
    pass.setBindGroup(0, this.bg(this.accView, smp, this.bufShow, 'show'));
    pass.setViewport(vp[0], vp[1], vp[2], vp[3], 0, 1);
    pass.draw(3);
    pass.end();
  }

  /** the picture as kept: the look over the average at its own size (no Exposure and White Level, no preview), and the
   *  crop's rectangle of it in pixels */
  shot(enc: GPUCommandEncoder, smp: GPUSampler, look: PhotoLook): { tex: GPUTexture; x: number; y: number; w: number; h: number } | null {
    if (!this.acc || !this.accView) return null;
    const W = this.acc.width, H = this.acc.height;
    if (!this.shotTex) {
      this.shotTex = this.device.createTexture({ label: 'photo picture', size: [W, H], format: 'bgra8unorm',
        usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
      this.shotView = this.shotTex.createView();
    }
    this.fill(look, null, null);
    this.device.queue.writeBuffer(this.bufShot, 0, this.args);
    const pass = enc.beginRenderPass({ colorAttachments: [{ view: this.shotView!, loadOp: 'clear', clearValue: [0, 0, 0, 1], storeOp: 'store' }] });
    pass.setPipeline(this.lookPipe('bgra8unorm'));
    pass.setBindGroup(0, this.bg(this.accView, smp, this.bufShot, 'shot'));
    pass.draw(3);
    pass.end();
    const c = look.crop;
    const x = Math.max(0, Math.min(W - 1, Math.round(c[0] * W))), y = Math.max(0, Math.min(H - 1, Math.round(c[1] * H)));
    const w = Math.max(1, Math.min(W - x, Math.round((c[2] - c[0]) * W))), h = Math.max(1, Math.min(H - y, Math.round((c[3] - c[1]) * H)));
    return { tex: this.shotTex, x, y, w, h };
  }

  destroy() { this.acc?.destroy(); this.shotTex?.destroy(); this.acc = this.shotTex = null; this.accView = this.shotView = null; }
}
