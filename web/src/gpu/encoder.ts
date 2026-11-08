// The WebGPU encoder: applies the resource queue and turns a handed-off frame
// (runtime/web/gpu_web.c records) into render passes.  A port of the encode
// half of runtime/metal.m,
// reading the records zero-copy from the wasm memory.
//
// Semantics kept from the Mac port, on purpose (the reference screenshots come
// from it):
//   - the first pass of a target in a frame starts with a clear to the clear
//     colour (alpha 1), later passes load; a colour Clear only matters there;
//   - Z clears are deferred to the next pass that binds the Z surface;
//   - every Z surface is depth32float, created at the size of the first target
//     it is used with (the implicit one follows the screen); a size mismatch
//     binds a scratch depth and turns the Z test off;
//   - draw viewports follow Direct3D; a scissor also keeps the 2D UI in its
//     4:3 band on a screen narrower than 4:3 (GwDraw.clip);
//   - StretchRect: a copy when size and format match, else a scaled quad; from a
//     source rectangle (the cover shoot's picture) always a scaled quad, opaque from
//     the screen (the D3D back buffer has no alpha).
// Deliberate differences: the clear colour is the frame's own (Clear records
// in stream order) rather than a global read at encode time; scaled blits do
// not blend; draws into a texture that no longer exists are dropped (the Mac
// drew them on the screen).
import * as P from './proto';
import { D, F, Kind, Op } from './proto';
import { COPY, FF_FS, MS_RESOLVE, TONE, TVERT_VS } from './wgsl';
import { createShaderAbi, alphaFunc, PSCUBE, VSPAR_SIZE, KWIN_SIZE, PSPAR_SIZE, HLPAR_SIZE } from './shader-abi';
import { unpackDxt } from './dxt';
import { drawViewport, drawScissor } from './viewport';
import { dxt1HasPunch, dxt1ToAstc } from './astc';
import type { GameMemory } from '../host/memory';
import { HqSkins, type DataReader } from './hqskin';

export interface EncodeStats {
  draws: number; passes: number; skipped: number; encodeMs: number;
  /** the frame was drawn straight into the texture given to encodeFrame (no back buffer, no composite) */
  direct: boolean;
}

interface Tex {
  serial: number; tex: GPUTexture; fmt: number; w: number; h: number; levels: number; cube: boolean; rt: boolean;
  view: GPUTextureView;            // sampling view (2d, or cube)
  rtViews: GPUTextureView[];       // render target views: [0] = level 0 (2d) or faces 0..5 (cube)
  expandA8: boolean; bc: boolean;
  unpack: boolean;                 // DXT on a GPU without texture-compression-bc: BGRA8, unpacked on upload
  astc: boolean;                   // DXT1 on a GPU with ASTC but not BC: ASTC 4x4, rewritten on upload (astc.ts)
  filled: boolean;                 // a level has gone up (an ASTC texture's choice is made at its first upload)
  bytes: number;                   // its size on the GPU, every level and face
  face0?: GPUTextureView;          // a cube's face 0, level 0 as a 2D view (StretchRect from it), made at its first use
  ms?: MsTwin;                     // a render target made with TF_MS: its 4-sample twin (runtime/com.c MULTISAMPLING)
}

/** a 4-sample colour texture that a target's passes draw into and resolve from (runtime/com.c MULTISAMPLING); stale:
 *  the target was written another way (a blit, a copy, a clear pass of its own) since the twin last resolved into it */
interface MsTwin { tex: GPUTexture; view: GPUTextureView; stale: boolean }
/** HQ vinyls (hqskin.ts): skins painted again at 1024 shown at once, at most; the one drawn longest ago goes */
const OVER_MAX = 16;
/** a sampler key (encoder.sampler) with a mip filter: linear where it had none */
const overSmp = (k: number) => ((k >> 4) & 3) ? k : (k & ~(3 << 4)) | (2 << 4);

const enum Src { TVert = 0, Raw = 1, Pool = 2 }
const PSLOT = 768;                 // PSPar (560 bytes) at 256-aligned slots
const PSPAR_BYTES = PSPAR_SIZE;

/** FNV-1a of a shader's text: identifies a module across sessions */
function fnv(t: string): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < t.length; i++) { h ^= t.charCodeAt(i); h = Math.imul(h, 0x01000193); }
  return h >>> 0;
}

const BLEND: GPUBlendFactor[] = ['one', 'zero', 'one', 'src', 'one-minus-src', 'src-alpha', 'one-minus-src-alpha', 'dst-alpha',
  'one-minus-dst-alpha', 'dst', 'one-minus-dst', 'src-alpha-saturated', 'src-alpha', 'one-minus-src-alpha', 'constant', 'one-minus-constant'];
const BLENDOP: GPUBlendOperation[] = ['add', 'add', 'subtract', 'reverse-subtract', 'min', 'max'];
const CMP: GPUCompareFunction[] = ['always', 'never', 'less', 'equal', 'less-equal', 'greater', 'not-equal', 'greater-equal', 'always'];
const ADDR: GPUAddressMode[] = ['repeat', 'repeat', 'mirror-repeat', 'clamp-to-edge', 'clamp-to-edge', 'mirror-repeat'];

export class Encoder {
  private regionClearPipes = new Map<string, GPURenderPipeline>();
  /** Attachment clears ignore viewports. A triangle clears only this player's
   * pixels and preserves the colour/depth belonging to the other players. */
  private regionClearPipe(flags: number, argb: number, z: number, ms = false): GPURenderPipeline {
    const key = `${flags & 3}:${argb}:${z}${ms ? ':x4' : ''}`;
    let pipe = this.regionClearPipes.get(key);
    if (pipe) return pipe;
    const rgba = [(argb >>> 16 & 255) / 255, (argb >>> 8 & 255) / 255, (argb & 255) / 255, (argb >>> 24) / 255];
    const module = this.device.createShaderModule({ label: 'split region clear', code: `
      @vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
        let xy = array<vec2f, 3>(vec2f(-1., -1.), vec2f(3., -1.), vec2f(-1., 3.));
        return vec4f(xy[i], ${z.toFixed(9)}, 1.);
      }
      @fragment fn fs() -> @location(0) vec4f { return vec4f(${rgba.map(v => v.toFixed(9)).join(',')}); }
    ` });
    pipe = this.device.createRenderPipeline({ label: 'split region clear', layout: 'auto',
      vertex: { module, entryPoint: 'vs' }, fragment: { module, entryPoint: 'fs', targets: [{ format: 'bgra8unorm', writeMask: flags & 1 ? 15 : 0 }] },
      primitive: { topology: 'triangle-list' },
      depthStencil: { format: 'depth32float', depthWriteEnabled: !!(flags & 2), depthCompare: 'always' },
      ...(ms ? { multisample: { count: 4 } } : {}),
    });
    this.regionClearPipes.set(key, pipe);
    return pipe;
  }
  readonly device: GPUDevice;
  private queue: GPUQueue;
  private memory: GameMemory;
  private sab!: SharedArrayBuffer;
  private u8!: Uint8Array; private u32!: Uint32Array; private i32!: Int32Array; private f32!: Float32Array;
  private ctlW: number;              // word index of GwCtl
  private log: (s: string) => void;
  private feat: { bc: boolean; astc: boolean; swizzle: boolean; unaligned: boolean };

  // resources
  private tex: (Tex | undefined)[] = [];
  // HQ vinyls: the 1024 skins shown in the game's skins' places (texFor), by the game's texture id; the latest paint
  // asked for each id (a newer one, the game painting it again or freeing it: an older paint is dropped)
  private hq: HqSkins | null = null;
  private over = new Map<number, Tex & { drawn: number }>();
  private hqWant = new Map<number, number>();
  private hqSeq = 0;
  private drawClock = 0;
  private serial = 1;
  private white!: Tex; private whiteCube!: Tex;
  private zTex: (GPUTexture | undefined)[] = [];
  private zView: (GPUTextureView | undefined)[] = [];
  private zpend = new Uint8Array(64); private zpendV = new Float32Array(64);
  private scratchZ = new Map<string, GPUTextureView>();
  private pool: GPUBuffer | null = null;
  private vsMods = new Map<number, GPUShaderModule>();
  private psMods = new Map<number, GPUShaderModule>();
  private samplers = new Map<number, GPUSampler>();
  private pipes = new Map<number, GPURenderPipeline | null>();
  private bg1 = new Map<string, GPUBindGroup>();
  private destroyLater: { destroy(): void }[] = [];

  // per-frame buffers (one each: writeBuffer is ordered before the next submit)
  private tvBuf: GPUBuffer; private rawBuf: GPUBuffer; private kBuf: GPUBuffer; private pBuf: GPUBuffer;
  private bufGen = 0; private bg0: (GPUBindGroup | null)[] = [null, null, null]; private bg0Gen = -1;
  private pStage = new Float32Array(192 * 1024);
  // per-draw scratch, reused so that a draw allocates nothing: group 0's dynamic
  // offsets, and the group 1 tuple of the previous draw (cube, 4 texture serials,
  // 4 sampler keys; [0] = -1 when the pass has none bound yet)
  private dyn = new Uint32Array(3);
  private b1 = new Int32Array(9);

  // layouts and built-in pipelines
  private bgl0: GPUBindGroupLayout; private bgl1: GPUBindGroupLayout; private bgl1c: GPUBindGroupLayout;
  private pl2d: GPUPipelineLayout; private plCube: GPUPipelineLayout;
  private tvertMod: GPUShaderModule; private ffMod: GPUShaderModule; private ffCubeMod: GPUShaderModule;
  private copyMod: GPUShaderModule; private copyBgl: GPUBindGroupLayout;
  private copyPipes = new Map<string, GPURenderPipeline>();
  private linear: GPUSampler; private nearest: GPUSampler;
  // the composite with Exposure and White Level (toneQuad), made at its first use
  private toneRes: { mod: GPUShaderModule; bgl: GPUBindGroupLayout; buf: GPUBuffer; pipes: Map<string, GPURenderPipeline>;
    bgs: WeakMap<GPUTextureView, Map<GPUSampler, GPUBindGroup>> } | null = null;
  private toneArgs = new Float32Array(4);

  // back buffer (the game's device-size "screen"), and the texture the current frame
  // draws its screen into: the back buffer, or the canvas texture when sizes match
  bb: GPUTexture | null = null; private bbView: GPUTextureView | null = null;
  screen: GPUTexture | null = null; screenView: GPUTextureView | null = null;
  /** frames that copied from the screen before drawing to it (they keep the back buffer) */
  screenReadFirst = 0;
  // multisampling (runtime/com.c MULTISAMPLING: the game's Full Scene Anti-Aliasing): the back buffer with 4 samples
  // (Op.BbSamples) and a render target made with TF_MS each have a 4-sample twin; their passes draw into the twin, with
  // a 4-sample twin of their Z surface (by surface and size), and resolve into the screen or the texture at the pass's
  // end; a blit or copy into them leaves the twin behind (stale) until the next pass that loads draws the picture into it.
  // The resolve is a pass of ours (msResolve: the samples' average), not the GPU's own (a resolveTarget): the twin is
  // kept for the next pass that loads, and the GPU's resolve of a picture it also keeps failed on two computers'
  // graphics on 2026-11-08 (an AMD Ryzen 5600GT's Vega in Chrome and Edge: the GPU hung at the third frame, every game;
  // a Qualcomm Snapdragon laptop: the picture in scrambled tiles once frames had several passes on the screen)
  private bbSamples = 1;
  // msResolve's shader, layout, pipelines by format and bind groups by twin, made at its first use
  private msRes: { mod: GPUShaderModule; bgl: GPUBindGroupLayout; layout: GPUPipelineLayout; pipes: Map<string, GPURenderPipeline>;
    bgs: WeakMap<GPUTexture, GPUBindGroup> } | null = null;
  private msScreen: MsTwin | null = null;
  private msZ = new Map<string, { tex: GPUTexture; view: GPUTextureView }>();
  private msZpend = new Uint8Array(64); private msZpendV = new Float32Array(64);
  private pipesMs = new Map<number, GPURenderPipeline | null>();
  /** passes drawn at 4 samples (resolved at their end), pictures drawn into a twin again after a blit or copy */
  msPasses = 0; msRefreshes = 0;

  /** guest frames whose records are logged in full (diagnostics), and "trace every dumped frame" */
  traceSeq = new Set<number>(); traceDumps = false; logShaders = false;
  /** diagnostics: log every create / upload / free of these texture ids */
  watchTex = new Set<number>();
  /** pixelated textures (the page's pixelated_textures, NFSU2_PIXELATED_TEXTURES): the game's own textures sampled
   *  nearest, their mip levels too, as dgVoodoo's "Point sampled", whatever filter the game asks for; its render
   *  targets (the screen copies of the post passes, reflections, the mirror) stay as it sets them */
  pointTex = false;
  /** anisotropic filtering where the game asks for it: the game gives its world's textures MAXANISOTROPY 16 with linear
   *  filters (every Texture Filtering level, 2026-11-08), which D3D9 filters trilinear; on, those draws filter at the
   *  anisotropy asked (anisoKey) */
  aniso = false;
  private tr: string[] | null = null;

  // stats
  totalDraws = 0; totalPasses = 0; pipelinesMade = 0; missingTarget = 0; noVs = 0; zMismatch = 0; conflicts = 0;
  /** the game's textures on the GPU, bytes: all of them, those unpacked from DXT and those DXT1 as ASTC (no
   *  texture-compression-bc) */
  texBytes = 0; unpackBytes = 0; astcBytes = 0;
  /** textures made and freed since the log's last minute line (index.ts zeroes them): friends' cars coming and
   *  going, the world after a shop */
  texMade = 0; texMadeBytes = 0; texFreed = 0; texFreedBytes = 0;
  get bindGroups(): number { return this.bg1.size; }
  /** blocks with a transparent texel that went into a texture already ASTC: two planes (astc.ts) */
  private astcHoles = { blocks: 0 };
  /** WebGPU calls made by encodeFrame, by kind (totals; what the GPU process decodes and records) */
  calls = { pipe: 0, bg0: 0, bg1: 0, bg1New: 0, blend: 0, ix: 0, draw: 0, pass: 0, copy: 0 };
  private seenFmt = new Set<string>();

  constructor(device: GPUDevice, memory: GameMemory, ctlPtr: number, log: (s: string) => void) {
    this.device = device; this.queue = device.queue; this.memory = memory; this.ctlW = ctlPtr >>> 2; this.log = log;
    const f = device.features;
    this.feat = { bc: f.has('texture-compression-bc'), astc: f.has('texture-compression-astc'), swizzle: f.has('texture-component-swizzle'),
      unaligned: f.has('texture-compression-unaligned' as GPUFeatureName) };
    this.views();
    const d = device;
    // the shader ABI's explicit layouts, shared with the shader check
    const abi = createShaderAbi(d);
    this.bgl0 = abi.group0; this.bgl1 = abi.group1; this.bgl1c = abi.group1Cube;
    this.pl2d = abi.layout2d; this.plCube = abi.layoutCube;
    this.tvertMod = this.module('tvert', TVERT_VS);
    this.ffMod = this.module('ff', FF_FS(false));
    this.ffCubeMod = this.module('ff-cube', FF_FS(true));
    this.copyMod = this.module('copy', COPY);
    this.copyBgl = d.createBindGroupLayout({ entries: [
      { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
      { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } }] });
    this.linear = d.createSampler({ minFilter: 'linear', magFilter: 'linear', mipmapFilter: 'linear', addressModeU: 'repeat', addressModeV: 'repeat' });
    this.nearest = d.createSampler({ minFilter: 'nearest', magFilter: 'nearest' });
    const buf = (label: string, size: number, usage: number) => d.createBuffer({ label, size, usage });
    this.tvBuf = buf('tvert', 1 << 20, GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST);
    this.rawBuf = buf('raw', 1 << 20, GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST);
    this.kBuf = buf('vsconst', 1 << 20, GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST);
    this.pBuf = buf('pspar', 1 << 20, GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST);
    this.pool = buf('pool', 1 << 16, GPUBufferUsage.STORAGE | GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC);
    this.white = this.makeWhite(false); this.whiteCube = this.makeWhite(true);
  }

  private module(label: string, code: string): GPUShaderModule {
    const m = this.device.createShaderModule({ label, code });
    m.getCompilationInfo().then(ci => { for (const e of ci.messages) if (e.type === 'error') this.log(`[gpu] WGSL ${label}:${e.lineNum}: ${e.message}`); });
    return m;
  }

  /** (re)create the typed views when the wasm memory grew.  The queue's writes take a subarray of just their bytes,
   *  never u8 with an offset: Firefox's WebGPU refuses a view of 2 GB or more (a computer's memory with the guest
   *  heap's window, host/memory.ts) */
  views(): void {
    const b = this.memory.buffer as unknown as SharedArrayBuffer;
    if (b === this.sab) return;
    this.sab = b;
    this.u8 = new Uint8Array(b); this.u32 = new Uint32Array(b); this.i32 = new Int32Array(b); this.f32 = new Float32Array(b);
  }

  private makeWhite(cube: boolean): Tex {
    const t = this.device.createTexture({ label: cube ? 'white-cube' : 'white', size: [1, 1, cube ? 6 : 1], format: 'bgra8unorm',
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
    const px = new Uint8Array(24).fill(255);
    this.queue.writeTexture({ texture: t }, px, { bytesPerRow: 4, rowsPerImage: 1 }, [1, 1, cube ? 6 : 1]);
    return { serial: 0, tex: t, fmt: 21, w: 1, h: 1, levels: 1, cube, rt: false, expandA8: false, bc: false, unpack: false, astc: false, filled: true,
      rtViews: [], view: t.createView({ dimension: cube ? 'cube' : '2d' }), bytes: 0 };
  }

  // ------------------------------------------------------------ resource queue
  /** Apply queued resource commands from the tail up to `limit`; returns the new tail. */
  drain(limit: number): number {
    const c = this.ctlW, u32 = this.u32;
    const cap = u32[c + P.C.rqCap], base = u32[c + P.C.rqPtr];
    let tail = u32[c + P.C.rqTail];
    // the queue past this page's buffer of a memory that grew (host/memory.ts): left as it is until the new buffer came
    if (!(base + cap <= this.u8.length)) return tail;
    let n = 0;
    while (((limit - tail) | 0) > 0) {
      const p = base + (tail & (cap - 1)), w = p >>> 2;
      const op = u32[w], len = u32[w + 1];
      if (len < 16 || len > cap) { this.log(`[gpu] corrupt resource queue record op=${op} len=${len} at ${tail}`); tail = limit; break; }
      const a0 = u32[w + 2], a1 = u32[w + 3], a2 = u32[w + 4], a3 = u32[w + 5], a4 = u32[w + 6], a5 = u32[w + 7];
      const data = p + P.CMD_HDR;
      switch (op) {
        case Op.Pad: break;
        case Op.TexCreate: this.texCreate(a0, a1, a2, a3, a4, a5);
          if (this.watchTex.has(a0)) this.log(`[watch] tex ${a0} create ${a1}x${a2} levels ${a3} fmt ${a4.toString(16)} flags ${a5}`); break;
        case Op.TexUpload: this.texUpload(a0, a1 & 0xFFFF, a1 >>> 16, a2, a3, a4, a5, data);
          if (this.watchTex.has(a0)) {
            let nz = 0; for (let k = 0; k < a4 * a5; k++) if (this.u8[data + k]) nz++;
            this.log(`[watch] tex ${a0} upload level ${a1 & 0xFFFF} face ${a1 >>> 16} ${a2}x${a3} pitch ${a4} rows ${a5}: ${nz} nonzero of ${a4 * a5} bytes, first ${[...this.u8.subarray(data, data + 16)].map(x => x.toString(16).padStart(2, '0')).join('')}`);
          } break;
        case Op.TexFree: if (this.watchTex.has(a0)) this.log(`[watch] tex ${a0} free`); this.texFree(a0); break;
        case Op.PoolGrow: this.poolGrow(a0); break;
        case Op.PoolWrite: if (this.pool && a0 + a1 <= this.pool.size) this.queue.writeBuffer(this.pool, a0, this.u8.subarray(data, data + a1)); break;
        case Op.ShaderVs: case Op.ShaderPs: {
          const text = new TextDecoder().decode(this.u8.slice(data, data + Math.max(0, a1 - 1)));
          const mod = this.module(`${op === Op.ShaderVs ? 'vs' : 'ps'}_${a0}`, text);
          (op === Op.ShaderVs ? this.vsMods : this.psMods).set(a0, mod);
          if (this.logShaders) this.log(`[shader ${op === Op.ShaderVs ? 'vs' : 'ps'}_${a0}]\n${text}\n[/shader]`);
          const h = fnv(text);
          if (op === Op.ShaderVs) { this.vsHash.set(a0, h); this.vsByHash.set(h, a0); } else { this.psHash.set(a0, h); this.psByHash.set(h, a0); }
          this.warmUp();
          break;
        }
        case Op.ZForget: if (a0 < 64 && this.zTex[a0]) { this.zTex[a0]!.destroy(); this.zTex[a0] = undefined; this.zView[a0] = undefined; }
          for (const [k, z] of this.msZ) if (k.startsWith(`${a0}:`)) { z.tex.destroy(); this.msZ.delete(k); } break;
        case Op.BbSamples: this.setBbSamples(a0); break;
        case Op.SkinHq: this.skinHq(a0, a1, a2, data); break;
        default: this.log(`[gpu] unknown resource op ${op}`);
      }
      tail = (tail + len) >>> 0;
      if (++n % 64 === 0) { Atomics.store(this.i32, c + P.C.rqTail, tail | 0); Atomics.notify(this.i32, c + P.C.rqTail); }
    }
    Atomics.store(this.i32, c + P.C.rqTail, tail | 0); Atomics.notify(this.i32, c + P.C.rqTail);
    return tail;
  }

  private gpuFormat(fmt: number): { f: GPUTextureFormat; bc: boolean; a8: boolean; unpack?: boolean; astc?: boolean } | null {
    switch (fmt) {
      case P.Fmt.A8R8G8B8: case P.Fmt.X8R8G8B8: return { f: 'bgra8unorm', bc: false, a8: false };
      case P.Fmt.A8: return this.feat.swizzle ? { f: 'r8unorm', bc: false, a8: false } : { f: 'bgra8unorm', bc: false, a8: true };
      case P.Fmt.P8: return { f: 'r8unorm', bc: false, a8: false };         // samples (L,0,0,1), as on the Mac
      case P.Fmt.DXT1: case P.Fmt.DXT3: case P.Fmt.DXT5:
        if (!this.feat.bc && fmt === P.Fmt.DXT1 && this.feat.astc) return { f: 'astc-4x4-unorm', bc: false, a8: false, astc: true };
        if (!this.feat.bc) return { f: 'bgra8unorm', bc: false, a8: false, unpack: true };
        return { f: fmt === P.Fmt.DXT1 ? 'bc1-rgba-unorm' : fmt === P.Fmt.DXT3 ? 'bc2-rgba-unorm' : 'bc3-rgba-unorm', bc: true, a8: false };
    }
    return null;
  }

  /** unpack: DXT unpacked to BGRA8 even where it could go up as ASTC (a DXT1 texture with transparent texels) */
  private texCreate(id: number, w: number, h: number, levels: number, fmt: number, flags: number, unpack = false): void {
    this.texFree(id);
    let gf = this.gpuFormat(fmt);
    if (!gf) { this.log(`[gpu] texture ${id}: format ${fmt} not supported`); return; }
    // ASTC wants whole blocks (as BC does without texture-compression-unaligned): a level 0 of another size unpacks
    if (gf.astc && (unpack || (w & 3) || (h & 3))) gf = { f: 'bgra8unorm', bc: false, a8: false, unpack: true };
    const cube = (flags & P.TF_CUBE) !== 0, rt = (flags & P.TF_RT) !== 0 && !gf.bc && !gf.unpack && !gf.astc && fmt !== P.Fmt.A8 && fmt !== P.Fmt.P8;
    if (gf.bc && ((w & 3) || (h & 3)) && !this.feat.unaligned) {
      const k = `bc-unaligned ${w}x${h}`;
      if (!this.seenFmt.has(k)) { this.seenFmt.add(k); this.log(`[gpu] texture ${id}: ${k} without texture-compression-unaligned (drawn white)`); }
      return;
    }
    const maxLv = Math.floor(Math.log2(Math.max(w, h))) + 1;
    levels = Math.max(1, Math.min(levels, maxLv));
    let usage = GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.COPY_SRC;
    if (rt) usage |= GPUTextureUsage.RENDER_ATTACHMENT;
    const tex = this.device.createTexture({ label: `t${id}`, size: [w, h, cube ? 6 : 1], format: gf.f, mipLevelCount: levels, usage });
    const vd: GPUTextureViewDescriptor = { dimension: cube ? 'cube' : '2d' };
    if (fmt === P.Fmt.A8 && !gf.a8) vd.swizzle = '000r';
    const rtViews: GPUTextureView[] = [];
    if (rt) {
      if (cube) for (let f = 0; f < 6; f++) rtViews.push(tex.createView({ dimension: '2d', baseArrayLayer: f, arrayLayerCount: 1, baseMipLevel: 0, mipLevelCount: 1 }));
      else rtViews.push(tex.createView({ dimension: '2d', baseMipLevel: 0, mipLevelCount: 1 }));
    }
    // its size on the GPU, for the log (memory runs out first on phones, whose GPUs unpack DXT)
    const bpp = gf.bc ? (fmt === P.Fmt.DXT1 ? 0.5 : 1) : gf.astc || gf.f === 'r8unorm' ? 1 : 4;
    let bytes = 0;
    for (let l = 0; l < levels; l++) {
      let lw = Math.max(1, w >> l), lh = Math.max(1, h >> l);
      if (gf.bc || gf.astc) { lw = (lw + 3) & ~3; lh = (lh + 3) & ~3; }
      bytes += lw * lh * bpp;
    }
    bytes *= cube ? 6 : 1;
    this.texBytes += bytes; if (gf.unpack) this.unpackBytes += bytes; if (gf.astc) this.astcBytes += bytes;
    this.texMade++; this.texMadeBytes += bytes;
    // a multisampled render target (runtime/com.c MULTISAMPLING): its 4-sample twin, in the texture's own format
    let ms: MsTwin | undefined;
    if ((flags & P.TF_MS) && rt && !cube && levels === 1) {
      const mt = this.device.createTexture({ label: `t${id} x4`, size: [w, h], format: gf.f, sampleCount: 4,
        usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING });
      ms = { tex: mt, view: mt.createView(), stale: true };
      bytes += w * h * 4 * 4; this.texBytes += w * h * 4 * 4; this.texMadeBytes += w * h * 4 * 4;
    }
    this.tex[id] = { serial: this.serial++, tex, fmt, w, h, levels, cube, rt, view: tex.createView(vd), rtViews, expandA8: gf.a8, bc: gf.bc,
      unpack: !!gf.unpack, astc: !!gf.astc, filled: false, bytes, ms };
  }

  private texFree(id: number): void {
    if (this.hqWant.size) this.overGone(id);
    const t = this.tex[id];
    if (!t) return;
    t.tex.destroy(); t.ms?.tex.destroy();
    this.texBytes -= t.bytes; if (t.unpack) this.unpackBytes -= t.bytes; if (t.astc) this.astcBytes -= t.bytes;
    this.texFreed++; this.texFreedBytes += t.bytes;
    this.tex[id] = undefined;
    if (this.bg1.size > 8192) this.bg1.clear();       // keys carry serials: stale entries are never hit again
  }

  private uploads = new Map<number, number>();   // diagnostics: uploads per texture serial
  private unpackBuf = new Uint8Array(0);           // DXT unpacked or rewritten as ASTC for an upload (no texture-compression-bc)
  private texUpload(id: number, level: number, face: number, lw: number, lh: number, pitch: number, rows: number, data: number): void {
    let t = this.tex[id];
    if (level === 0 && this.hqWant.size) this.overGone(id);      // the game painted it again: its own until the new paint
    if (t && this.traceDumps) this.uploads.set(t.serial, (this.uploads.get(t.serial) ?? 0) + 1);
    if (!t || level >= t.levels) return;
    if (t.astc) {
      const bw = (lw + 3) >> 2, bh = (lh + 3) >> 2;
      if (!t.filled && dxt1HasPunch(this.u8, data, pitch, bw, bh)) {
        // transparent texels in the first level to go up: one ASTC plane cannot give transparent black, so the
        // texture is unpacked as before (nothing of it is on the GPU yet)
        this.texCreate(id, t.w, t.h, t.levels, t.fmt, t.cube ? P.TF_CUBE : 0, true);
        t = this.tex[id]!;
      } else {
        t.filled = true;
        if (this.unpackBuf.length < bw * bh * 16) this.unpackBuf = new Uint8Array(bw * bh * 16);
        const holes = this.astcHoles.blocks;
        const dst: GPUTexelCopyTextureInfo = { texture: t.tex, mipLevel: level, origin: { x: 0, y: 0, z: t.cube ? Math.min(face, 5) : 0 } };
        this.queue.writeTexture(dst, dxt1ToAstc(this.u8, data, pitch, bw, bh, this.unpackBuf, this.astcHoles), { bytesPerRow: bw * 16, rowsPerImage: bh }, [bw * 4, bh * 4, 1]);
        if (this.astcHoles.blocks > holes && !this.seenFmt.has('astc-holes')) {
          this.seenFmt.add('astc-holes');
          this.log(`[gpu] texture ${id}: transparent texels in level ${level}, not in the first level that went up: two-plane ASTC blocks (black under them not kept)`);
        }
        return;
      }
    }
    let w = lw, h = lh;
    if (t.bc) { w = (lw + 3) & ~3; h = (lh + 3) & ~3; }
    const dst: GPUTexelCopyTextureInfo = { texture: t.tex, mipLevel: level, origin: { x: 0, y: 0, z: t.cube ? Math.min(face, 5) : 0 } };
    if (t.expandA8) {
      const out = new Uint8Array(lw * lh * 4);
      for (let y = 0; y < lh; y++) for (let x = 0; x < lw; x++) out[(y * lw + x) * 4 + 3] = this.u8[data + y * pitch + x];
      this.queue.writeTexture(dst, out, { bytesPerRow: lw * 4, rowsPerImage: lh }, [lw, lh, 1]);
      return;
    }
    if (t.unpack) {
      // into one buffer for every upload (writeTexture copies it at once): no array of up to 16 MB left to the page's
      // garbage collector for every level of every texture a phone loads
      if (this.unpackBuf.length < lw * lh * 4) this.unpackBuf = new Uint8Array(lw * lh * 4);
      this.queue.writeTexture(dst, unpackDxt(t.fmt, this.u8, data, pitch, lw, lh, this.unpackBuf), { bytesPerRow: lw * 4, rowsPerImage: lh }, [lw, lh, 1]);
      return;
    }
    this.queue.writeTexture(dst, this.u8.subarray(data, data + pitch * rows), { bytesPerRow: pitch, rowsPerImage: rows }, [w, h, 1]);
  }

  private poolGrow(size: number): void {
    size = (size + 3) & ~3;
    const nb = this.device.createBuffer({ label: 'pool', size, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC });
    if (this.pool) {
      const e = this.device.createCommandEncoder();
      e.copyBufferToBuffer(this.pool, 0, nb, 0, Math.min(this.pool.size, size));
      this.queue.submit([e.finish()]);
      this.pool.destroy();
    }
    this.pool = nb; this.bufGen++;
  }

  // ------------------------------------------------------------------ helpers
  private grow(b: GPUBuffer, need: number, label: string, usage: number): GPUBuffer {
    if (b.size >= need) return b;
    const size = Math.ceil(need * 1.25 / 262144) * 262144;
    this.destroyLater.push(b);
    this.bufGen++;
    return this.device.createBuffer({ label, size, usage });
  }

  /** pointTex: a sampler key with nearest min, mag and mip filters (so no anisotropy) for one of the game's own
   *  textures; a mip filter of none (the top level only) stays */
  private pointKey(k: number, t: Tex): number {
    if (t.rt) return k;
    return (k & ~0x3f) | 1 | 1 << 2 | ((k >> 4) & 3 ? 1 << 4 : 0);
  }

  /** aniso: a sampler key with linear filters, mips and a MAXANISOTROPY above 1 on one of the game's own textures as
   *  anisotropic (D3DTEXF_ANISOTROPIC min filter: sampler() gives it the anisotropy asked) */
  private anisoKey(k: number, t: Tex): number {
    return !t.rt && (k & 3) === 2 && ((k >> 2) & 3) === 2 && (k >> 4) & 3 && (k >> 12) & 7 ? (k & ~3) | 3 : k;
  }

  private sampler(k: number): GPUSampler {
    let s = this.samplers.get(k);
    if (s) return s;
    const mn = k & 3, mg = (k >> 2) & 3, mp = (k >> 4) & 3, au = (k >> 6) & 7, av = (k >> 9) & 7, al = (k >> 12) & 7;
    const aniso = (mn === 3 || mg === 3) ? Math.min(16, 1 << al) : 1;
    const d: GPUSamplerDescriptor = {
      minFilter: mn === 1 ? 'nearest' : 'linear', magFilter: mg === 1 ? 'nearest' : 'linear',
      mipmapFilter: mp === 2 ? 'linear' : 'nearest',
      addressModeU: ADDR[au < 6 ? au : 1], addressModeV: ADDR[av < 6 ? av : 1],
      lodMinClamp: 0, lodMaxClamp: mp === 0 ? 0 : 32,
    };
    if (aniso > 1) {   // WebGPU: anisotropy needs linear min, mag and mip filters
      d.minFilter = 'linear'; d.magFilter = 'linear'; d.mipmapFilter = 'linear'; d.maxAnisotropy = aniso;
    }
    if (au >= 4 || av >= 4) { const key = `addr ${au},${av}`; if (!this.seenFmt.has(key)) { this.seenFmt.add(key); this.log(`[gpu] sampler address mode ${au}/${av} (BORDER/MIRRORONCE) approximated`); } }
    s = this.device.createSampler(d);
    this.samplers.set(k, s);
    return s;
  }

  private copyPipe(format: GPUTextureFormat, opaque: boolean, ms = false): GPURenderPipeline {
    const key = format + (opaque ? '/o' : '') + (ms ? '/x4' : '');
    let p = this.copyPipes.get(key);
    if (!p) {
      p = this.device.createRenderPipeline({ label: `copy ${key}`, layout: this.device.createPipelineLayout({ bindGroupLayouts: [this.copyBgl] }),
        vertex: { module: this.copyMod, entryPoint: 'vs_full' },
        fragment: { module: this.copyMod, entryPoint: opaque ? 'fs_opaque' : 'fs_copy', targets: [{ format }] },
        primitive: { topology: 'triangle-list' }, ...(ms ? { multisample: { count: 4 } } : {}) });
      this.copyPipes.set(key, p);
    }
    return p;
  }

  /** the back buffer's samples from the next frame on (Op.BbSamples: 1 or 4) */
  private setBbSamples(n: number): void {
    n = n === 4 ? 4 : 1;
    if (n === this.bbSamples) return;
    this.bbSamples = n;
    this.log(`[gpu] back buffer: ${n} sample${n > 1 ? 's' : ''} (the game's Full Scene Anti-Aliasing ${n > 1 ? 'on' : 'off'})`);
    if (n === 1) {
      if (this.msScreen) { this.destroyLater.push(this.msScreen.tex); this.msScreen = null; }
      for (const z of this.msZ.values()) this.destroyLater.push(z.tex);
      this.msZ.clear();
    }
  }

  /** the screen's 4-sample twin, of the back buffer's size; a new one holds nothing yet (stale).  A new size (a window
   *  resized with native resolution) also drops the Z surfaces' twins of other sizes: made again, cleared, if used */
  private screenTwin(w: number, h: number): MsTwin {
    const s = this.msScreen;
    if (s && s.tex.width === w && s.tex.height === h) return s;
    if (s) this.destroyLater.push(s.tex);
    for (const [k, z] of this.msZ) if (!k.endsWith(`:${w}x${h}`)) { this.destroyLater.push(z.tex); this.msZ.delete(k); }
    const tex = this.device.createTexture({ label: `backbuffer x4 ${w}x${h}`, size: [w, h], format: 'bgra8unorm', sampleCount: 4,
      usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING });
    return this.msScreen = { tex, view: tex.createView(), stale: true };
  }

  /** a twin's picture into its target (dst: the target's view, of the twin's size), each pixel the average of its 4
   *  samples, in a pass of its own after the twin's (see the multisampling fields: not the GPU's own resolve) */
  private msResolve(enc: GPUCommandEncoder, twin: MsTwin, dst: GPUTextureView): void {
    const d = this.device;
    const r = this.msRes ??= (() => {
      const bgl = d.createBindGroupLayout({ entries: [
        { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'unfilterable-float', multisampled: true } }] });
      return { mod: this.module('ms-resolve', MS_RESOLVE), bgl, layout: d.createPipelineLayout({ bindGroupLayouts: [bgl] }),
        pipes: new Map<string, GPURenderPipeline>(), bgs: new WeakMap<GPUTexture, GPUBindGroup>() };
    })();
    const fmt = twin.tex.format;
    let p = r.pipes.get(fmt);
    if (!p) r.pipes.set(fmt, p = d.createRenderPipeline({ label: `ms-resolve ${fmt}`, layout: r.layout,
      vertex: { module: r.mod, entryPoint: 'vs_full' }, fragment: { module: r.mod, entryPoint: 'fs_resolve', targets: [{ format: fmt }] },
      primitive: { topology: 'triangle-list' } }));
    let g = r.bgs.get(twin.tex);
    if (!g) r.bgs.set(twin.tex, g = d.createBindGroup({ layout: r.bgl, entries: [{ binding: 0, resource: twin.view }] }));
    // every pixel is written: nothing of the target's to load
    const pass = enc.beginRenderPass({ colorAttachments: [{ view: dst, loadOp: 'clear', clearValue: [0, 0, 0, 1], storeOp: 'store' }] });
    pass.setPipeline(p);
    pass.setBindGroup(0, g);
    pass.draw(3);
    pass.end();
  }

  /** the 4-sample twin of Z surface zid at a pass's size (one per surface and size: the implicit Z serves the screen
   *  and a smaller target alike); fresh: made just now, so cleared */
  private msZView(zid: number, w: number, h: number): { view: GPUTextureView; fresh: boolean } {
    const k = `${zid}:${w}x${h}`;
    let z = this.msZ.get(k), fresh = false;
    if (!z) {
      const tex = this.device.createTexture({ label: `z${zid} x4 ${w}x${h}`, size: [w, h], format: 'depth32float', sampleCount: 4,
        usage: GPUTextureUsage.RENDER_ATTACHMENT });
      this.msZ.set(k, z = { tex, view: tex.createView() }); fresh = true;
    }
    return { view: z.view, fresh };
  }

  /** a target's picture (src: its sampling view) drawn into its twin at every sample, before a pass that loads */
  private msRefresh(enc: GPUCommandEncoder, twin: MsTwin, src: GPUTextureView): void {
    const pass = enc.beginRenderPass({ colorAttachments: [{ view: twin.view, loadOp: 'clear', clearValue: [0, 0, 0, 1], storeOp: 'store' }] });
    pass.setPipeline(this.copyPipe(twin.tex.format, false, true));
    pass.setBindGroup(0, this.copyBg(src, this.nearest));
    pass.draw(3);
    pass.end();
    twin.stale = false; this.msRefreshes++;
  }

  // A copy's bind group, by its source view and sampler.  Made anew for every copy, the composite alone was one a
  // frame (a phone's render scale always composites), and WebKit frees what a bind group holds in its graphics
  // process only when the page's garbage collector gets to it.
  private copyBgs = new WeakMap<GPUTextureView, Map<GPUSampler, GPUBindGroup>>();
  private copyBg(src: GPUTextureView, smp: GPUSampler): GPUBindGroup {
    let m = this.copyBgs.get(src);
    if (!m) this.copyBgs.set(src, m = new Map());
    let g = m.get(smp);
    if (!g) m.set(smp, g = this.device.createBindGroup({ layout: this.copyBgl, entries: [{ binding: 0, resource: src }, { binding: 1, resource: smp }] }));
    return g;
  }

  /** draw `src` over the whole of `dst` (StretchRect between sizes, composite) */
  copyQuad(enc: GPUCommandEncoder, src: GPUTextureView, dst: GPUTextureView, format: GPUTextureFormat, smp: GPUSampler,
           vp?: [number, number, number, number], opaque = false, ts?: GPURenderPassTimestampWrites): void {
    const pass = enc.beginRenderPass({ colorAttachments: [{ view: dst, loadOp: 'clear', clearValue: [0, 0, 0, 1], storeOp: 'store' }], timestampWrites: ts });
    pass.setPipeline(this.copyPipe(format, opaque));
    pass.setBindGroup(0, this.copyBg(src, smp));
    if (vp) pass.setViewport(vp[0], vp[1], vp[2], vp[3], 0, 1);
    pass.draw(3);
    pass.end();
  }

  /** copyQuad's opaque composite with Exposure and White Level (wgsl.ts TONE): tone = mult, white, knee */
  toneQuad(enc: GPUCommandEncoder, src: GPUTextureView, dst: GPUTextureView, format: GPUTextureFormat, smp: GPUSampler,
           tone: ArrayLike<number>, vp?: [number, number, number, number], ts?: GPURenderPassTimestampWrites): void {
    const d = this.device;
    const r = this.toneRes ??= {
      mod: this.module('tone', TONE),
      bgl: d.createBindGroupLayout({ entries: [
        { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
        { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
        { binding: 2, visibility: GPUShaderStage.FRAGMENT, buffer: { type: 'uniform' } }] }),
      buf: d.createBuffer({ label: 'tone', size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST }),
      pipes: new Map(), bgs: new WeakMap(),
    };
    let p = r.pipes.get(format);
    if (!p) r.pipes.set(format, p = d.createRenderPipeline({ label: `tone ${format}`, layout: d.createPipelineLayout({ bindGroupLayouts: [r.bgl] }),
      vertex: { module: r.mod, entryPoint: 'vs_full' }, fragment: { module: r.mod, entryPoint: 'fs_tone', targets: [{ format }] },
      primitive: { topology: 'triangle-list' } }));
    let m = r.bgs.get(src);
    if (!m) r.bgs.set(src, m = new Map());
    let g = m.get(smp);
    if (!g) m.set(smp, g = d.createBindGroup({ layout: r.bgl, entries: [{ binding: 0, resource: src }, { binding: 1, resource: smp }, { binding: 2, resource: { buffer: r.buf } }] }));
    this.toneArgs.set([tone[0], tone[1], tone[2], 0]);
    this.queue.writeBuffer(r.buf, 0, this.toneArgs);
    const pass = enc.beginRenderPass({ colorAttachments: [{ view: dst, loadOp: 'clear', clearValue: [0, 0, 0, 1], storeOp: 'store' }], timestampWrites: ts });
    pass.setPipeline(p);
    pass.setBindGroup(0, g);
    if (vp) pass.setViewport(vp[0], vp[1], vp[2], vp[3], 0, 1);
    pass.draw(3);
    pass.end();
  }

  private pipeDesc(key: number, blend: number, ps: number, vs: number, cull: number, dm: number, nd: boolean, ms = false): GPURenderPipelineDescriptor | null {
    // the translator's modules (vs_0 / ps_0 / ps_256 included once registered), else the built-in fallbacks
    const cube = ps >= PSCUBE;
    const vq = this.vsMods.get(vs), pq = this.psMods.get(ps);
    const vmod = vq ?? (vs === 0 ? this.tvertMod : undefined);
    const fmod = pq ?? (ps === 0 ? this.ffMod : ps === PSCUBE ? this.ffCubeMod : undefined);
    if (!vmod || !fmod) return null;
    const en = blend & 1, sf = (blend >> 1) & 31, df = (blend >> 6) & 31, op = (blend >> 11) & 7, mask = (blend >> 14) & 15;
    let bs: GPUBlendState | undefined;
    if (en) {
      const o = BLENDOP[op] ?? 'add', mm = o === 'min' || o === 'max';
      let s = BLEND[sf] ?? 'one', d = BLEND[df] ?? 'one';
      if (sf === 12) d = 'one-minus-src-alpha';                 // BOTHSRCALPHA sets both factors
      if (sf === 13) { s = 'one-minus-src-alpha'; d = 'src-alpha'; }   // BOTHINVSRCALPHA
      if (mm) { s = 'one'; d = 'one'; }                          // WebGPU: min/max take factor one (D3D ignores them)
      bs = { color: { srcFactor: s, dstFactor: d, operation: o }, alpha: { srcFactor: s, dstFactor: d, operation: o } };
    }
    return {
      label: `p${key}`, layout: cube ? this.plCube : this.pl2d,
      vertex: { module: vmod, entryPoint: vq ? undefined : 'vs_tvert' },
      fragment: { module: fmod, entryPoint: pq ? undefined : 'fs_ff', targets: [{ format: 'bgra8unorm', blend: bs, writeMask: mask }],
        constants: { ALPHA_FUNC: alphaFunc(nd ? 0 : 1) } },
      primitive: { topology: 'triangle-list', frontFace: 'cw', cullMode: cull === 1 ? 'none' : cull === 2 ? 'front' : 'back' },
      depthStencil: { format: 'depth32float', depthWriteEnabled: (dm & 2) !== 0, depthCompare: (dm & 1) ? CMP[(dm >> 2) & 7] : 'always' },
      // a pass on a 4-sample target (runtime/com.c MULTISAMPLING)
      ...(ms ? { multisample: { count: 4 } } : {}),
    };
  }

  /** a pipeline needed now: created synchronously (a first use may hitch; warmUp() avoids most) */
  private pipeline(key: number, blend: number, ps: number, vs: number, cull: number, dm: number, nd: boolean, ms = false): GPURenderPipeline | null {
    const pipes = ms ? this.pipesMs : this.pipes;
    const desc = this.pipeDesc(key, blend, ps, vs, cull, dm, nd, ms);
    if (!desc) { pipes.set(key, null); return null; }
    this.device.pushErrorScope('validation');
    const tp = performance.now();
    const p = this.device.createRenderPipeline(desc);
    const dt = performance.now() - tp;
    this.pipeMs += dt; if (dt > this.pipeMsMax) this.pipeMsMax = dt;
    this.device.popErrorScope().then(e => { if (e) { this.log(`[gpu] pipeline ps=${ps} vs=${vs} blend=${blend.toString(16)}${ms ? ' x4' : ''}: ${e.message}`); pipes.set(key, null); } });
    pipes.set(key, p);
    this.pipelinesMade++; this.pipelinesSync++;
    // remember it for the next session, by the modules' text hashes (the state: blend, cull, depth mode, alpha test
    // off, 4 samples at 2^35; an entry from before the samples has 1)
    const vh = this.vsHash.get(vs), ph = this.psHash.get(ps);
    if (vh !== undefined && ph !== undefined) {
      const st = blend + cull * 262144 + dm * 1048576 + (nd ? 67108864 : 0) + (ms ? 34359738368 : 0), k = `${vh}:${ph}:${st}`;
      if (!this.warmSeen.has(k)) { this.warmSeen.add(k); this.warmList.push({ v: vh, p: ph, s: st }); this.warmDirty = true; }
    }
    return p;
  }

  // ---- pipeline warm-up: what earlier sessions used, created asynchronously as soon as both modules exist ----
  private vsHash = new Map<number, number>(); private psHash = new Map<number, number>();
  private vsByHash = new Map<number, number>(); private psByHash = new Map<number, number>();
  private warmList: { v: number; p: number; s: number }[] = [];
  private warmSeen = new Set<string>();
  private warmTodo: { v: number; p: number; s: number }[] = [];
  warmDirty = false; pipelinesSync = 0; pipelinesAsync = 0;
  /** time spent creating pipelines synchronously on the encoder (total, worst) */
  pipeMs = 0; pipeMsMax = 0;

  /** the persisted list from an earlier session */
  loadWarm(list: { v: number; p: number; s: number }[]): void {
    for (const e of list) {
      const k = `${e.v}:${e.p}:${e.s}`;
      if (this.warmSeen.has(k)) continue;
      this.warmSeen.add(k); this.warmList.push(e); this.warmTodo.push(e);
    }
    this.warmUp();
  }
  /** the list to persist */
  warmEntries(): { v: number; p: number; s: number }[] { this.warmDirty = false; return this.warmList; }

  private warmUp(): void {
    if (!this.warmTodo.length) return;
    this.warmTodo = this.warmTodo.filter(e => {
      const vs = this.vsByHash.get(e.v), ps = this.psByHash.get(e.p);
      if (vs === undefined || ps === undefined) return true;
      const blend = e.s % 262144, cull = Math.floor(e.s / 262144) % 4, dm = Math.floor(e.s / 1048576) % 64;
      const nd = Math.floor(e.s / 67108864) % 2 === 1, ms = Math.floor(e.s / 34359738368) % 2 === 1, pipes = ms ? this.pipesMs : this.pipes;
      const key = blend + ps * 262144 + vs * 268435456 + cull * 137438953472 + dm * 549755813888 + (nd ? 35184372088832 : 0);
      if (pipes.has(key)) return false;
      const desc = this.pipeDesc(key, blend, ps, vs, cull, dm, nd, ms);
      if (!desc) return false;
      this.device.createRenderPipelineAsync(desc).then(p => {
        if (!pipes.has(key)) { pipes.set(key, p); this.pipelinesMade++; this.pipelinesAsync++; }
      }).catch(x => this.log(`[gpu] warm-up pipeline ps=${ps} vs=${vs}: ${x}`));
      return false;
    });
  }

  private group0(src: Src): GPUBindGroup {
    if (this.bg0Gen !== this.bufGen) { this.bg0 = [null, null, null]; this.bg0Gen = this.bufGen; }
    let g = this.bg0[src];
    if (!g) {
      const vb = src === Src.TVert ? this.tvBuf : src === Src.Raw ? this.rawBuf : this.pool!;
      g = this.device.createBindGroup({ layout: this.bgl0, entries: [
        { binding: 0, resource: { buffer: vb } },
        { binding: 1, resource: { buffer: this.kBuf, size: KWIN_SIZE } },
        { binding: 2, resource: { buffer: this.pBuf, size: PSPAR_BYTES } },
        // the frame's other cars' headlights, in its VS constant arena (gpu_proto.h GwHL)
        { binding: 3, resource: { buffer: this.kBuf, size: HLPAR_SIZE } }] });
      this.bg0[src] = g;
    }
    return g;
  }

  private depthView(zid: number, w: number, h: number, ms = false): GPUTextureView {
    const k = `${w}x${h}${ms ? 'x4' : ''}`;
    let v = this.scratchZ.get(k);
    if (!v) {
      v = this.device.createTexture({ label: `scratch-z ${k}`, size: [w, h], format: 'depth32float', sampleCount: ms ? 4 : 1,
        usage: GPUTextureUsage.RENDER_ATTACHMENT }).createView();
      this.scratchZ.set(k, v);
    }
    void zid;
    return v;
  }

  private ensureBackBuffer(w: number, h: number): void {
    if (this.bb && this.bb.width === w && this.bb.height === h) return;
    if (this.bb) this.destroyLater.push(this.bb);
    this.bb = this.device.createTexture({ label: `backbuffer ${w}x${h}`, size: [w, h], format: 'bgra8unorm',
      usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST });
    this.bbView = this.bb.createView();
    this.log(`[gpu] back buffer ${w}x${h}`);
  }

  // -------------------------------------------------------------------- frame
  /** timestamp write for the frame's first render pass (GPU timing), consumed by encodeFrame */
  firstPassTs: GPURenderPassTimestampWrites | undefined;

  /** Does the frame copy from the screen before anything draws to it?  It then
   *  reads the previous frame, which only the back buffer still holds. */
  private readsScreenFirst(drawsW: number, dn: number): boolean {
    const u32 = this.u32;
    for (let i = 0; i < dn; i++) {
      const w = drawsW + i * P.DRAW_WORDS, tgt = u32[w + D.target];
      if (u32[w + D.kind] === Kind.Blit) {
        const sid = u32[w + D.blitSrc];
        if (!sid && tgt) return true;
        if (sid && !tgt) return false;
      } else if (!tgt) return false;      // a draw or clear on the screen opens a pass there
    }
    return false;
  }

  /** Encode the frame in record buffer `b` into `enc`.  The canvas composite and
   * the submit are the caller's.  With `direct`, a bgra8unorm texture with
   * RENDER_ATTACHMENT, TEXTURE_BINDING, COPY_SRC and COPY_DST usage (the canvas
   * texture), the frame draws its screen straight into it when the sizes match:
   * the same passes, one full-screen copy fewer. */
  encodeFrame(b: number, enc: GPUCommandEncoder, direct?: GPUTexture): EncodeStats {
    const t0 = performance.now();
    this.drawClock++;
    this.views();
    const u32 = this.u32, i32 = this.i32, f32 = this.f32, dev = this.device;
    const fw = this.ctlW + P.C.frame + b * (P.FRAME_BYTES >> 2);
    const dn = u32[fw + F.dn], vn = u32[fw + F.vn], rvn = u32[fw + F.rvn], kn = u32[fw + F.kn];
    const drawsW = u32[fw + F.draws] >>> 2, pscW = u32[fw + F.psc] >>> 2;
    const bbW = u32[fw + F.bbW] || 1280, bbH = u32[fw + F.bbH] || 720;
    let isDirect = !!direct && direct.width === bbW && direct.height === bbH;
    if (isDirect && this.readsScreenFirst(drawsW, dn)) {
      if (this.screenReadFirst++ === 0) this.log(`[gpu] frame ${u32[fw + F.seq]} copies from the screen before drawing to it: drawn into the back buffer`);
      isDirect = false;
    }
    if (isDirect) {
      this.screen = direct!; this.screenView = direct!.createView();
      // the back buffer of earlier frames (before the canvas size settled) is not needed
      if (this.bb) { this.destroyLater.push(this.bb); this.bb = null; this.bbView = null; }
    } else { this.ensureBackBuffer(bbW, bbH); this.screen = this.bb; this.screenView = this.bbView; }
    const seq = u32[fw + F.seq];
    this.tr = this.traceSeq.has(seq) || (this.traceDumps && u32[fw + F.dump]) ? [] : null;
    const tr = this.tr;
    if (tr) tr.push(`[trace] frame ${seq}: ${dn} records, ${vn} tverts, raw ${rvn} B, consts ${kn} B, back buffer ${bbW}x${bbH}, clear ${u32[fw + F.clearArgb].toString(16)}`);

    // arenas: one writeBuffer each, in the queue before this frame's submit
    const SV = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST, UV = GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST;
    if (vn) { this.tvBuf = this.grow(this.tvBuf, vn * P.TVERT_BYTES, 'tvert', SV); this.queue.writeBuffer(this.tvBuf, 0, this.u8.subarray(u32[fw + F.tv], u32[fw + F.tv] + vn * P.TVERT_BYTES)); }
    if (rvn) { const n = (rvn + 3) & ~3; this.rawBuf = this.grow(this.rawBuf, n, 'raw', SV); this.queue.writeBuffer(this.rawBuf, 0, this.u8.subarray(u32[fw + F.raw], u32[fw + F.raw] + n)); }
    if (kn) { const n = (kn + 3) & ~3; this.kBuf = this.grow(this.kBuf, n + KWIN_SIZE, 'vsconst', UV); this.queue.writeBuffer(this.kBuf, 0, this.u8.subarray(u32[fw + F.k], u32[fw + F.k] + n)); }
    this.pBuf = this.grow(this.pBuf, (dn + 1) * PSLOT, 'pspar', UV);
    if (this.pStage.length < (dn + 1) * (PSLOT / 4)) this.pStage = new Float32Array((dn + 64) * (PSLOT / 4));
    const ps = this.pStage;
    let pslots = 0, lastPsci = -1, lastAref = NaN;
    const lastPF = new Float32Array(8);

    let pass: GPURenderPassEncoder | null = null;
    let curT = -1, curZ = -1, realZ = false, passKey = -2, passCube = -1;
    let screenTouched = false;
    const touched = new Set<number>();
    let clearArgb = u32[fw + F.clearArgb];
    let cPipe: GPURenderPipeline | null = null, cSrc = -1, cK = -1, cP = -1, cBcol = -1, cIx = '';
    // the pass's size, the scissor in effect in it (a GwDraw.clip value; 0 = the whole target), whether a split
    // screen's cells came in it (passCells: viewports and scissors then by viewport.ts), the headlights block bound
    let passW = 0, passH = 0, cClip = 0, passCells = false, cH = -1;
    // the viewport and scissor in effect in the pass (when it opens WebGPU's: the whole target), and the draw's
    // (viewport.ts fills them in place, so a draw allocates nothing and sets either only when it changes)
    const vpNow = [0, 0, 0, 0, 0, 1], vpDraw = [0, 0, 0, 0, 0, 1], scNow = [0, 0, 0, 0], scDraw = [0, 0, 0, 0];
    // the previous draw's pipeline (pipelines outlive passes, so this survives a pass change)
    let lastKey = -1, lastMs = false, lastPipe: GPURenderPipeline | null | undefined;
    // the pass draws at 4 samples: the screen of a multisampled back buffer, or a TF_MS target (runtime/com.c MULTISAMPLING)
    let passMs = false;
    const b1 = this.b1, dyn = this.dyn;
    b1[0] = -1;
    let draws = 0, passes = 0, skipped = 0;
    // the twin of the pass that is open and its target's view: resolved (msResolve) as the pass ends
    let msOut: { twin: MsTwin; dst: GPUTextureView } | null = null;
    const endPass = () => {
      if (!pass) return;
      pass.end(); pass = null;
      if (msOut) { this.msResolve(enc, msOut.twin, msOut.dst); msOut = null; passes++; }
    };

    const openPass = (target: number, zid: number): GPURenderPassEncoder | null => {
      let view: GPUTextureView, w: number, h: number, key: number;
      // a 4-sample target: its twin, and the view its picture is drawn into the twin from
      let twin: MsTwin | null = null, twinSrc: GPUTextureView | null = null;
      if (!target) {
        view = this.screenView!; w = bbW; h = bbH; key = -1; passCube = -1;
        if (this.bbSamples === 4) { twin = this.screenTwin(w, h); twinSrc = view; }
      } else {
        const t = this.tex[target & 0xFFFF], face = (target >>> 16) & 7;
        if (!t || !t.rt) { this.missingTarget++; return null; }
        view = t.cube ? t.rtViews[face ? Math.min(face - 1, 5) : 0] : t.rtViews[0];
        w = t.w; h = t.h; key = (target & 0xFFFF) * 8 + face; passCube = target & 0xFFFF;
        if (t.ms) { twin = t.ms; twinSrc = t.view; }
      }
      passW = w; passH = h;
      vpNow[0] = 0; vpNow[1] = 0; vpNow[2] = w; vpNow[3] = h; vpNow[4] = 0; vpNow[5] = 1;
      scNow[0] = 0; scNow[1] = 0; scNow[2] = w; scNow[3] = h; cClip = 0; passCells = false;
      passKey = target ? (target & 0xFFFF) : -1;
      const first = key === -1 ? !screenTouched : !touched.has(key);
      let zv: GPUTextureView | undefined, zclear = false, zval = 1;
      passMs = !!twin;
      if (twin) {
        // drawn into the twin and resolved into the target as the pass ends (endPass); a twin behind its target (a blit
        // or copy into it) first gets the target's picture when the pass loads; the Z surface's own 4-sample twin
        if (!first && twin.stale) { this.msRefresh(enc, twin, twinSrc!); passes++; }
        if (zid && zid < 64) {
          const z = this.msZView(zid, w, h);
          zv = z.view; if (z.fresh) zclear = true;
          if (this.msZpend[zid]) { zclear = true; zval = this.msZpendV[zid]; this.msZpend[zid] = 0; }
        }
        realZ = !!zv;
        const c = clearArgb;
        const ts = this.firstPassTs; this.firstPassTs = undefined;
        const p = enc.beginRenderPass({
          timestampWrites: ts,
          colorAttachments: [{ view: twin.view, loadOp: first ? 'clear' : 'load', storeOp: 'store',
            clearValue: { r: ((c >>> 16) & 255) / 255, g: ((c >>> 8) & 255) / 255, b: (c & 255) / 255, a: 1 } }],
          depthStencilAttachment: zv
            ? { view: zv, depthLoadOp: zclear ? 'clear' : 'load', depthClearValue: zval, depthStoreOp: 'store' }
            : { view: this.depthView(0, w, h, true), depthLoadOp: 'clear', depthClearValue: 1, depthStoreOp: 'discard' },
        });
        twin.stale = false; msOut = { twin, dst: view };
        if (key === -1) screenTouched = true; else touched.add(key);
        passes++; this.calls.pass++; this.msPasses++;
        if (tr) tr.push(`  pass ${passes}: ${target ? `tex ${target & 0xFFFF}` : 'SCREEN'} x4 ${w}x${h} color ${first ? `clear ${c.toString(16)}` : 'load'}, z ${zv ? `zid ${zid} ${zclear ? `clear ${zval}` : 'load'}` : `none (zid ${zid})`}`);
        cPipe = null; cSrc = -1; cK = -1; cP = -1; b1[0] = -1; cBcol = -1; cIx = ''; cH = -1;
        return p;
      }
      if (zid && zid < 64) {
        let z = this.zTex[zid];
        if (!z || z.width !== w || z.height !== h) {
          if (!z || zid === 1) {
            if (z) this.destroyLater.push(z);
            z = this.zTex[zid] = dev.createTexture({ label: `z${zid} ${w}x${h}`, size: [w, h], format: 'depth32float', usage: GPUTextureUsage.RENDER_ATTACHMENT });
            this.zView[zid] = z.createView(); zclear = true;
          } else { z = undefined; this.zMismatch++; }
        }
        if (z) { zv = this.zView[zid]; if (this.zpend[zid]) { zclear = true; zval = this.zpendV[zid]; this.zpend[zid] = 0; } }
      }
      realZ = !!zv;
      const c = clearArgb;
      const ts = this.firstPassTs; this.firstPassTs = undefined;
      const p = enc.beginRenderPass({
        timestampWrites: ts,
        colorAttachments: [{ view, loadOp: first ? 'clear' : 'load', storeOp: 'store',
          clearValue: { r: ((c >>> 16) & 255) / 255, g: ((c >>> 8) & 255) / 255, b: (c & 255) / 255, a: 1 } }],
        depthStencilAttachment: zv
          ? { view: zv, depthLoadOp: zclear ? 'clear' : 'load', depthClearValue: zval, depthStoreOp: 'store' }
          : { view: this.depthView(0, w, h), depthLoadOp: 'clear', depthClearValue: 1, depthStoreOp: 'discard' },
      });
      if (key === -1) screenTouched = true; else touched.add(key);
      passes++; this.calls.pass++;
      if (tr) tr.push(`  pass ${passes}: ${target ? `tex ${target & 0xFFFF}${(target >>> 16) ? ` face ${(target >>> 16) - 1}` : ''}` : 'SCREEN'} ${w}x${h} color ${first ? `clear ${c.toString(16)}` : 'load'}, z ${zv ? `zid ${zid} ${zclear ? `clear ${zval}` : 'load'}` : `none (zid ${zid})`}`);
      cPipe = null; cSrc = -1; cK = -1; cP = -1; b1[0] = -1; cBcol = -1; cIx = ''; cH = -1;
      return p;
    };

    for (let i = 0; i < dn; i++) {
      const w = drawsW + i * P.DRAW_WORDS;
      const kind = u32[w + D.kind];
      if (kind === Kind.Blit) {
        const sid = u32[w + D.blitSrc], tgt = u32[w + D.target];
        const st = sid ? this.tex[sid & 0xFFFF] : null, dt = tgt ? this.tex[tgt & 0xFFFF] : null;
        // Packed surface faces use 1..6; zero denotes an ordinary 2D target.
        const sf = Math.max(0, ((sid >>> 16) & 7) - 1), df = Math.max(0, ((tgt >>> 16) & 7) - 1);
        const sTex = sid ? st?.tex : this.screen!, dTex = tgt ? dt?.tex : this.screen!;
        const r0 = u32[w + D.first], r1 = u32[w + D.count];   // a source rectangle (r1 0: the whole source)
        if (tr) tr.push(`  #${i} blit ${sid || 'SCREEN'}${r1 ? ` (${r0 & 0xFFFF},${r0 >>> 16})-(${r1 & 0xFFFF},${r1 >>> 16})` : ''} -> ${tgt ? tgt & 0xFFFF : 'SCREEN'}${!sTex || !dTex ? ' (missing: skipped)' : ''}`);
        if (!sTex || !dTex || sTex === dTex || (tgt && !dt!.rt)) { skipped++; continue; }
        endPass(); curT = -1; curZ = -1;
        if (r1 && this.blitRect(enc, sTex, r0, r1, tgt ? dt!.rtViews[0] : this.screenView!, !sid)) passes++;
        else if (sTex.width === dTex.width && sTex.height === dTex.height && sTex.format === dTex.format && sf < sTex.depthOrArrayLayers && df < dTex.depthOrArrayLayers)
          { enc.copyTextureToTexture({ texture: sTex, origin: [0, 0, sf] }, { texture: dTex, origin: [0, 0, df] }, [sTex.width, sTex.height, 1]); this.calls.copy++; }
        else {
          const sv = sid ? (st!.cube ? st!.tex.createView({ dimension: '2d', baseArrayLayer: sf, arrayLayerCount: 1, baseMipLevel: 0, mipLevelCount: 1 }) : st!.view) : this.screenView!;
          const dv = tgt ? dt!.rtViews[df] : this.screenView!;
          this.copyQuad(enc, sv, dv, 'bgra8unorm', this.linear);
          passes++;
        }
        if (tgt) { touched.add((tgt & 0xFFFF) * 8 + ((tgt >>> 16) & 7)); if (dt!.ms) dt!.ms.stale = true; }
        else { screenTouched = true; if (this.msScreen) this.msScreen.stale = true; }
        continue;
      }
      const depth = u32[w + D.depth], zid = depth >>> 8, target = u32[w + D.target];
      if (kind === Kind.Clear && f32[w + D.viewport + 2] > 0) {
        endPass(); curT = target; curZ = zid;
        pass = openPass(target, zid);
        const vp = drawViewport(f32, w + D.viewport, passW, passH, vpDraw);
        const z = f32[w + D.clrZ], flags = u32[w + D.clrFlags];
        if (pass && vp && Number.isFinite(z) && (flags & 3)) {
          pass.setViewport(vp[0], vp[1], vp[2], vp[3], 0, 1);
          pass.setPipeline(this.regionClearPipe(flags, u32[w + D.clrArgb], Math.max(0, Math.min(1, z)), passMs));
          pass.draw(3);
        }
        endPass(); curT = -1;
        continue;
      }
      if (kind === Kind.Clear) {
        const fl = u32[w + D.clrFlags];
        if (tr) tr.push(`  #${i} clear flags ${fl} argb ${u32[w + D.clrArgb].toString(16)} z ${f32[w + D.clrZ]} target ${target & 0xFFFF} zid ${zid}`);
        if (fl & 1) clearArgb = u32[w + D.clrArgb];
        if ((fl & 2) && zid && zid < 64) {
          const z = f32[w + D.clrZ];
          this.zpend[zid] = 1; this.zpendV[zid] = z < 0 ? 0 : z > 1 ? 1 : z;
          // and its 4-sample twins (one Z surface for the game)
          this.msZpend[zid] = 1; this.msZpendV[zid] = this.zpendV[zid];
          if (pass && curZ === zid) { endPass(); curT = -1; }
        }
      }
      const zsplit = zid !== curZ && !(pass && kind !== Kind.Clear && (depth & 3) === 0);
      if (curT === -1 || target !== curT || zsplit) {
        endPass();
        curT = target; curZ = zid;
        pass = openPass(target, zid);
      }
      if (kind === Kind.Clear) continue;
      const count = u32[w + D.count];
      if (!pass || !count) { if (tr) tr.push(`  #${i} draw skipped: ${pass ? 'no vertices' : 'no pass (target missing)'}`); skipped++; continue; }
      const p: GPURenderPassEncoder = pass;
      // a split screen's cell or effect region (GwDraw.viewport, clrFlags): from then on in this pass every draw
      // takes the general way; until then (and in any frame of one view) the whole target, as before split screen
      const cell = f32[w + D.viewport + 2] !== 0 || u32[w + D.clrFlags] !== 0;
      if (cell) passCells = true;
      if (passCells) {
        if (!drawViewport(f32, w + D.viewport, passW, passH, vpDraw)) { skipped++; continue; }
        if (vpDraw[0] !== vpNow[0] || vpDraw[1] !== vpNow[1] || vpDraw[2] !== vpNow[2] || vpDraw[3] !== vpNow[3]
            || vpDraw[4] !== vpNow[4] || vpDraw[5] !== vpNow[5]) {
          p.setViewport(vpDraw[0], vpDraw[1], vpDraw[2], vpDraw[3], vpDraw[4], vpDraw[5]);
          for (let q = 0; q < 6; q++) vpNow[q] = vpDraw[q];
        }
      }

      // pipeline
      const psid = i32[w + D.ps], vsid = i32[w + D.vsid];
      if (vsid > 0 && !this.vsMods.has(vsid)) { this.noVs++; skipped++; if (tr) tr.push(`  #${i} draw skipped: no module for vs ${vsid}`); continue; }
      const aref = f32[w + D.aref];
      const blend = u32[w + D.blend] & 0x3FFFF, cull = u32[w + D.cull] & 3, dm = realZ ? (depth & 63) : 0;
      // a pixel shader without a module draws with the fixed-function stage; its cube twin when tex0 is a cube
      const t0 = u32[w + D.tex];
      let psEff = psid > 0 && this.psMods.has(psid) ? psid : psid >= PSCUBE ? PSCUBE : 0;
      if (psEff === 0 && t0 && this.tex[t0]?.cube) psEff = PSCUBE;
      const nd = !(aref > 0);
      const key = blend + psEff * 262144 + vsid * 268435456 + cull * 137438953472 + dm * 549755813888 + (nd ? 35184372088832 : 0);
      let pipe = key === lastKey && passMs === lastMs ? lastPipe : (passMs ? this.pipesMs : this.pipes).get(key);
      if (pipe === undefined) pipe = this.pipeline(key, blend, psEff, vsid, cull, dm, nd, passMs);
      lastKey = key; lastMs = passMs; lastPipe = pipe;
      if (!pipe) { skipped++; if (tr) tr.push(`  #${i} draw skipped: no pipeline (ps ${psEff} vs ${vsid})`); continue; }
      if (pipe !== cPipe) { p.setPipeline(pipe); cPipe = pipe; this.calls.pipe++; }
      // the 2D UI on a screen narrower than 4:3 stays in its 4:3 band (gpu_proto.h GwDraw.clip), on the
      // back buffer or a target of its size
      const clip = passW === bbW && passH === bbH ? u32[w + D.clip] : 0;
      if (passCells) {
        // A viewport transforms coordinates; explicitly restrict rasterization
        // too. Screen-space quads and light sprites must stay in their cell.
        drawScissor(vpDraw, passW, passH, clip, u32[w + D.clrArgb], u32[w + D.clrFlags], scDraw);
        if (scDraw[0] !== scNow[0] || scDraw[1] !== scNow[1] || scDraw[2] !== scNow[2] || scDraw[3] !== scNow[3]) {
          p.setScissorRect(scDraw[0], scDraw[1], scDraw[2], scDraw[3]);
          for (let q = 0; q < 4; q++) scNow[q] = scDraw[q];
        }
      } else if (clip !== cClip) {
        const y0 = Math.min(clip & 0xFFFF, passH), y1 = Math.min(clip >>> 16, passH);
        scNow[0] = 0; scNow[1] = clip ? y0 : 0; scNow[2] = passW; scNow[3] = clip ? Math.max(0, y1 - y0) : passH;
        p.setScissorRect(scNow[0], scNow[1], scNow[2], scNow[3]);
        cClip = clip;
      }

      // PS parameters: a new slot only when they differ from the previous draw's
      const psci = u32[w + D.psci];
      let same = psci === lastPsci && aref === lastAref;
      if (same) for (let q = 0; q < 4; q++) if (lastPF[q] !== f32[w + D.proj + q] || lastPF[4 + q] !== f32[w + D.fogv + q]) { same = false; break; }
      if (!same) {
        const o = pslots * (PSLOT / 4);
        ps[o] = aref; ps[o + 1] = 0; ps[o + 2] = 0; ps[o + 3] = 0;
        for (let q = 0; q < 4; q++) { ps[o + 4 + q] = lastPF[q] = f32[w + D.proj + q]; ps[o + 136 + q] = lastPF[4 + q] = f32[w + D.fogv + q]; }
        ps.set(f32.subarray(pscW + psci * 128, pscW + psci * 128 + 128), o + 8);
        lastPsci = psci; lastAref = aref; pslots++;
      }
      const poff = (pslots - 1) * PSLOT;

      // group 0: vertex source + VS constants + PS parameters.  K is a 64 KB window of the
      // frame's constant arena that many draws share: the draw's block is row kb of it, given
      // as firstInstance.  A new bind group only for another vertex
      // source, other PS parameters or a block past the window.
      const isz = u32[w + D.ixIsz];
      const src = isz ? Src.Pool : vsid > 0 ? Src.Raw : Src.TVert;
      const koff = vsid > 0 ? u32[w + D.koff] : -1;
      const hoff = u32[w + D.blitSrc];
      if (src !== cSrc || poff !== cP || hoff !== cH || (koff >= 0 && (cK < 0 || koff < cK || koff - cK > KWIN_SIZE - VSPAR_SIZE))) {
        // the window starts at this draw's block (256-aligned below it); TVert draws read no K
        if (koff >= 0) cK = koff & ~255; else if (cK < 0) cK = 0;
        dyn[0] = cK; dyn[1] = poff; dyn[2] = hoff;
        p.setBindGroup(0, this.group0(src), dyn, 0, 3); cSrc = src; cP = poff; cH = hoff; this.calls.bg0++;
      }
      const kb = koff >= 0 ? (koff - cK) >> 4 : 0;

      // group 1: textures and samplers (a texture bound as this pass's target samples white)
      const cube = psEff >= PSCUBE;
      const t1 = u32[w + D.tex1], t2 = u32[w + D.tex2], t3 = u32[w + D.tex3];
      let s0 = u32[w + D.smp], s1 = u32[w + D.smp + 1], s2 = u32[w + D.smp + 2], s3 = u32[w + D.smp + 3];
      const v0 = this.texFor(t0, cube, passCube), v1 = this.texFor(t1, false, passCube), v2 = this.texFor(t2, false, passCube), v3 = this.texFor(t3, false, passCube);
      // a 1024 skin (HQ vinyls) has its mipmaps: sampled with them, whatever the game's 512 one (one level) asked for
      if (this.over.size) {
        if (this.over.get(t0) === v0) s0 = overSmp(s0);
        if (this.over.get(t1) === v1) s1 = overSmp(s1);
        if (this.over.get(t2) === v2) s2 = overSmp(s2);
        if (this.over.get(t3) === v3) s3 = overSmp(s3);
      }
      if (this.pointTex) { s0 = this.pointKey(s0, v0); s1 = this.pointKey(s1, v1); s2 = this.pointKey(s2, v2); s3 = this.pointKey(s3, v3); }
      else if (this.aniso) { s0 = this.anisoKey(s0, v0); s1 = this.anisoKey(s1, v1); s2 = this.anisoKey(s2, v2); s3 = this.anisoKey(s3, v3); }
      // the same textures and samplers as the previous draw (the common case): nothing to do
      if (b1[0] !== (cube ? 1 : 0) || b1[1] !== v0.serial || b1[2] !== v1.serial || b1[3] !== v2.serial || b1[4] !== v3.serial
          || b1[5] !== s0 || b1[6] !== s1 || b1[7] !== s2 || b1[8] !== s3) {
        const k1 = `${cube ? 1 : 0}${v0.serial},${v1.serial},${v2.serial},${v3.serial}|${s0},${s1},${s2},${s3}`;
        let g = this.bg1.get(k1);
        if (!g) {
          g = dev.createBindGroup({ layout: cube ? this.bgl1c : this.bgl1, entries: [
            { binding: 0, resource: v0.view }, { binding: 1, resource: v1.view }, { binding: 2, resource: v2.view }, { binding: 3, resource: v3.view },
            { binding: 4, resource: this.sampler(s0) }, { binding: 5, resource: this.sampler(s1) },
            { binding: 6, resource: this.sampler(s2) }, { binding: 7, resource: this.sampler(s3) }] });
          this.bg1.set(k1, g); this.calls.bg1New++;
        }
        p.setBindGroup(1, g); this.calls.bg1++;
        b1[0] = cube ? 1 : 0; b1[1] = v0.serial; b1[2] = v1.serial; b1[3] = v2.serial; b1[4] = v3.serial;
        b1[5] = s0; b1[6] = s1; b1[7] = s2; b1[8] = s3;
      }
      const bcol = u32[w + D.bcol];
      if (bcol !== cBcol) { p.setBlendConstant({ r: ((bcol >>> 16) & 255) / 255, g: ((bcol >>> 8) & 255) / 255, b: (bcol & 255) / 255, a: (bcol >>> 24) / 255 }); cBcol = bcol; this.calls.blend++; }

      if (isz) {
        const fmt: GPUIndexFormat = isz === 4 ? 'uint32' : 'uint16';
        if (fmt !== cIx) { p.setIndexBuffer(this.pool!, fmt); cIx = fmt; this.calls.ix++; }
        p.drawIndexed(count, 1, u32[w + D.ixIb] / isz, i32[w + D.ixBase], kb);
      } else if (vsid > 0) p.draw(count, 1, 0, kb);
      else p.draw(count, 1, u32[w + D.first], 0);
      draws++; this.calls.draw++;
      if (tr) tr.push(`  #${i} ${isz ? `ix${isz * 8} ib ${u32[w + D.ixIb]} base ${i32[w + D.ixBase]}` : vsid > 0 ? 'raw' : `tvert ${u32[w + D.first]}`} n ${count} vs ${vsid} ps ${psid}${psEff !== psid ? `->${psEff}` : ''} blend ${blend.toString(16)} depth ${(depth & 63).toString(16)}${realZ ? '' : '(off)'} cull ${cull} aref ${aref.toFixed(3)} bcol ${bcol.toString(16)} tex ${t0}${v0.serial === 0 && t0 ? '(white)' : ''},${t1},${t2},${t3} smp ${s0.toString(16)} koff ${koff} kb ${kb} poff ${poff}`);
    }
    endPass();
    if (tr) {
      // the first vertices of every TVert draw: clip position, oD0, oT0
      const tv = u32[fw + F.tv] >>> 2;
      for (let i = 0; i < dn; i++) {
        const w = drawsW + i * P.DRAW_WORDS;
        if (u32[w + D.kind] !== Kind.Draw || i32[w + D.vsid] > 0) continue;
        const first = u32[w + D.first], n = Math.min(u32[w + D.count], 6);
        for (let v = 0; v < n; v++) {
          const b = tv + (first + v) * 28, f = (k: number) => f32[b + k].toPrecision(4);
          tr.push(`  tvert #${i}.${v}: pos ${f(0)} ${f(1)} ${f(2)} ${f(3)} col0 ${f(4)} ${f(5)} ${f(6)} ${f(7)} uv0 ${f(12)} ${f(13)}`);
        }
      }
      // the first vertex of every raw draw: 8 words as hex, the K header and c0..c4
      const raw = u32[fw + F.raw], kb = u32[fw + F.k];
      for (let i = 0; i < dn; i++) {
        const w = drawsW + i * P.DRAW_WORDS;
        if (u32[w + D.kind] !== Kind.Draw || i32[w + D.vsid] <= 0 || u32[w + D.ixIsz]) continue;
        const a = (raw + u32[w + D.rawoff]) >>> 2, k = (kb + u32[w + D.koff]) >>> 2;
        const hex = [...u32.subarray(a, a + 8)].map(x => x.toString(16).padStart(8, '0')).join(' ');
        const flt = [...f32.subarray(a, a + 3)].map(x => x.toPrecision(5)).join(' ');
        const kc = [...f32.subarray(k + 4, k + 4 + 20)].map(x => x.toPrecision(4)).join(' ');
        tr.push(`  raw #${i}: v0 ${hex} (xyz ${flt}) K stride ${u32[k]} tsel ${u32[k + 1].toString(16)} base ${u32[k + 2]} c0-4 ${kc}`);
        const st = u32[k] >>> 2, n = Math.min(u32[w + D.count], 6);
        if (st) for (let v = 1; v < n; v++) tr.push(`  raw #${i}.${v}: xyz ${[...f32.subarray(a + v * st, a + v * st + 3)].map(x => x.toPrecision(6)).join(' ')} word3 ${u32[a + v * st + 3].toString(16)}`);
      }
      // per vertex shader, the first draw's constants c0..c15 (matrices)
      const seenVs = new Set<number>();
      for (let i = 0; i < dn; i++) {
        const w = drawsW + i * P.DRAW_WORDS, vs = i32[w + D.vsid];
        if (u32[w + D.kind] !== Kind.Draw || vs <= 0 || seenVs.has(vs)) continue;
        seenVs.add(vs);
        const k = (kb + u32[w + D.koff]) >>> 2, nk = (u32[w + D.ksz] - 16) >> 4;
        const rows: string[] = [];
        for (let r = 0; r < Math.min(nk, 16); r++) rows.push(`c${r}(${[...f32.subarray(k + 4 + r * 4, k + 8 + r * 4)].map(x => x.toPrecision(4)).join(' ')})`);
        tr.push(`  consts vs ${vs} (#${i}, ${nk} rows): ${rows.join(' ')}`);
      }
      const top = [...this.uploads].filter(([, n]) => n > 8).sort((a, b) => b[1] - a[1]).slice(0, 12);
      for (const [ser, n] of top) {
        const id = this.tex.findIndex(t => t?.serial === ser), t = this.tex[id];
        tr.push(`  re-uploaded: tex ${id} ${n}x ${t ? `${t.w}x${t.h} fmt ${t.fmt} levels ${t.levels}` : '(freed)'}`);
      }
      this.log(tr.join('\n')); this.tr = null;
    }
    if (!screenTouched) {
      const c = clearArgb;
      enc.beginRenderPass({ colorAttachments: [{ view: this.screenView!, loadOp: 'clear', storeOp: 'store',
        clearValue: { r: ((c >>> 16) & 255) / 255, g: ((c >>> 8) & 255) / 255, b: (c & 255) / 255, a: 1 } }] }).end();
      passes++; if (this.msScreen) this.msScreen.stale = true;
    }
    if (pslots) this.queue.writeBuffer(this.pBuf, 0, ps, 0, pslots * (PSLOT / 4));
    this.totalDraws += draws; this.totalPasses += passes;
    return { draws, passes, skipped, encodeMs: performance.now() - t0, direct: isDirect };
  }

  /** StretchRect from a rectangle of `src` (r0 left | top << 16, r1 right | bottom << 16): the rectangle copied into
   *  a texture of its own, then drawn over the whole of `dst`; false when there is nothing to copy that way */
  private blitRect(enc: GPUCommandEncoder, src: GPUTexture, r0: number, r1: number, dst: GPUTextureView, opaque: boolean): boolean {
    const x0 = Math.min(r0 & 0xFFFF, src.width), y0 = Math.min(r0 >>> 16, src.height);
    const x1 = Math.min(r1 & 0xFFFF, src.width), y1 = Math.min(r1 >>> 16, src.height);
    if (x1 <= x0 || y1 <= y0 || src.format !== 'bgra8unorm') return false;
    const part = this.device.createTexture({ label: `blit rect ${x1 - x0}x${y1 - y0}`, size: [x1 - x0, y1 - y0], format: 'bgra8unorm',
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
    enc.copyTextureToTexture({ texture: src, origin: [x0, y0, 0] }, { texture: part }, [x1 - x0, y1 - y0, 1]);
    this.calls.copy++;
    this.copyQuad(enc, part.createView(), dst, 'bgra8unorm', this.linear, undefined, opaque);
    this.destroyLater.push(part);
    return true;
  }

  /** texture `id` in bgra8unorm for a copy into a buffer (read back for the game): itself when it is a 2D texture in
   *  that format (its level 0 is copied), else its level 0 drawn into a new one (alpha 1); null when there is none */
  readable(enc: GPUCommandEncoder, id: number): GPUTexture | null {
    const t = this.tex[id];
    if (!t) return null;
    return t.tex.format === 'bgra8unorm' && !t.cube ? t.tex : this.snapshot(enc, id)?.tex ?? null;
  }

  /** diagnostics: texture `id` (level 0, face 0) drawn into a new bgra8 texture of its size, or null */
  snapshot(enc: GPUCommandEncoder, id: number): { tex: GPUTexture; info: string } | null {
    const t = this.tex[id];
    if (!t) return null;
    const out = this.device.createTexture({ size: [t.w, t.h], format: 'bgra8unorm', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
    const v = t.tex.createView({ dimension: '2d', baseArrayLayer: 0, arrayLayerCount: 1, baseMipLevel: 0, mipLevelCount: 1 });
    this.copyQuad(enc, v, out.createView(), 'bgra8unorm', this.nearest, undefined, true);
    this.destroyLater.push(out);
    return { tex: out, info: `${t.w}x${t.h} ${t.tex.format} fmt ${t.fmt} levels ${t.levels}${t.cube ? ' cube' : ''}${t.rt ? ' rt' : ''}` };
  }

  private texFor(id: number, cube: boolean, passTex: number): Tex {
    const t = id ? this.tex[id] : undefined;
    if (!t || t.cube !== cube) return cube ? this.whiteCube : this.white;
    if (id === passTex) { this.conflicts++; return cube ? this.whiteCube : this.white; }
    if (this.over.size) { const o = this.over.get(id); if (o) { o.drawn = this.drawClock; return o; } }
    return t;
  }

  // ------------------------------------------------------------ HQ vinyls
  /** this page paints skins again at 1024 (GW_FEAT_HQSKIN): BC compression (a PC's GPU), and a reader of the game data */
  get hqCapable(): boolean { return this.feat.bc; }
  hqSkins(read: DataReader): void {
    if (this.hq || !this.feat.bc) return;
    this.hq = new HqSkins(this.device, read, this.log, (enc, tex) => this.mipChain(enc, tex), P.SKIN_HQ);
  }
  get hqStats(): { painted: number; failed: number; shown: number } {
    return { painted: this.hq?.painted ?? 0, failed: this.hq?.failed ?? 0, shown: this.over.size };
  }

  /** each level of tex drawn from the one above it */
  private mipChain(enc: GPUCommandEncoder, tex: GPUTexture): void {
    for (let l = 1; l < tex.mipLevelCount; l++)
      this.copyQuad(enc, tex.createView({ baseMipLevel: l - 1, mipLevelCount: 1 }), tex.createView({ baseMipLevel: l, mipLevelCount: 1 }),
        'bgra8unorm', this.linear);
  }

  /** the game painted (or freed) texture id: its 1024 one goes, and any paint of it still under way is dropped */
  private overGone(id: number): void {
    if (!this.hqWant.has(id)) return;
    this.hqWant.set(id, ++this.hqSeq);
    const o = this.over.get(id);
    if (o) { this.over.delete(id); this.texBytes -= o.bytes; this.destroyLater.push(o.tex); }
  }

  /** GW_SKIN_HQ: the game painted texture id at 512 from this recipe (+ alpha planes); paint it at 1024 */
  private skinHq(id: number, recipeBytes: number, nplanes: number, data: number): void {
    const t = this.tex[id];
    if (!this.hq || !t || t.cube || t.w !== P.SKIN_HQ.SIDE || t.h !== P.SKIN_HQ.SIDE || recipeBytes !== P.SKIN_HQ.size || nplanes > P.SKIN_HQ.LAYERS) return;
    const token = ++this.hqSeq, serial = t.serial, plane = P.SKIN_HQ.SIDE * P.SKIN_HQ.SIDE;
    this.hqWant.set(id, token);
    // the payload is the queue's only until drain returns: copied
    const recipe = this.u8.slice(data, data + recipeBytes), planes = this.u8.slice(data + recipeBytes, data + recipeBytes + nplanes * plane);
    const current = () => this.hqWant.get(id) === token && this.tex[id]?.serial === serial;
    void this.hq.paint({ id, recipe, planes, nplanes, game: () => current() ? this.tex[id]!.view : null }).then(out => {
      if (!out) return;
      if (!current()) { out.destroy(); return; }
      const old = this.over.get(id);
      if (old) { this.over.delete(id); this.texBytes -= old.bytes; this.destroyLater.push(old.tex); }
      if (this.over.size >= OVER_MAX) {
        let far = -1, at = Infinity;
        for (const [k, o] of this.over) if (o.drawn < at) { at = o.drawn; far = k; }
        const o = this.over.get(far)!;
        this.over.delete(far); this.texBytes -= o.bytes; this.destroyLater.push(o.tex);
      }
      const bytes = Math.round(out.width * out.height * 4 * 4 / 3);
      this.texBytes += bytes;
      this.over.set(id, { serial: this.serial++, tex: out, fmt: P.Fmt.A8R8G8B8, w: out.width, h: out.height, levels: out.mipLevelCount,
        cube: false, rt: false, view: out.createView(), rtViews: [], expandA8: false, bc: false, unpack: false, astc: false, filled: true,
        bytes, drawn: this.drawClock });
    });
  }

  /** after the frame's submit: drop what the frame may still have referenced */
  afterSubmit(): void {
    for (const r of this.destroyLater) r.destroy();
    this.destroyLater.length = 0;
  }
}
