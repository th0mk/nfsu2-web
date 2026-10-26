// The WGSL shader ABI (docs/web/gpu-abi.md) as WebGPU objects: the bind group
// layouts and pipeline layouts every game pipeline uses, the uniform sizes and
// the override constants.  The renderer and the shader check both build their
// pipelines from here, so the check validates exactly what the game will run.

/** A draw's VS constant block: {stride, tsel, base, pad} + 256 vec4f (the rows it may read). */
export const VSPAR_SIZE = 4112;
/** The K binding: a 64 KB window of the frame's VS constant arena (gpu_proto.h GW_KWIN_ROWS rows); a draw's
 *  block starts at row firstInstance of it, at most KWIN_SIZE - VSPAR_SIZE bytes in. */
export const KWIN_SIZE = 65536;
/** PSPar: {aref, usec, pad0, pad1}, proj, 32 vec4f, fog. */
export const PSPAR_SIZE = 560;
/** HLPar (gpu_proto.h GwHL): the other cars' headlights, {n, s, o, pad} + 8 x {mat4x4f, vec4f}. */
export const HLPAR_SIZE = 656;
/** One CPU-transformed vertex (gfx.h TVert): pos, col0, col1, t[4], all f32. */
export const TVERT_SIZE = 112;
/** Pixel shader id offset of the stage-0 cube twin (com.c / metal.m PSCUBE). */
export const PSCUBE = 256;
/** Interstage locations used by VOut (col0, col1, uv0..uv3, fog). */
export const VOUT_LOCATIONS = 7;
/** With 8 texture stages (gpu_proto.h GwCtl.stages): uv4..uv7 at locations 7..10 after them (gpu-abi.md section 11). */
export const VOUT_LOCATIONS_8 = 11;

/** Group 1 binding of stage k's texture: 0..3 as ever, stages 4..7 at 8..11 (after smp0..smp3). */
export function texBinding(k: number): number { return k < 4 ? k : k + 4; }
/** Group 1 binding of stage k's sampler: 4..7, stages 4..7 at 12..15. */
export function smpBinding(k: number): number { return k < 4 ? k + 4 : k + 8; }
/** Stage k's binding kind in a pixel shader module's kinds word (proto.ts BK_*: 0 2D, 1 cube, 2 depth compare). */
export function stageKind(kinds: number, k: number): number { return (kinds >>> (2 * k)) & 3; }

/** The group 1 entries of a pipeline: `stages` stages (4 or 8), each a texture and a sampler of the kind the
 *  module's kinds word gives it; a cube twin (ps >= PSCUBE) has a cube at stage 0 whatever the word says.  Kind 3 is
 *  no binding the ABI defines (null).  4 stages, kinds 0: the layout every UG2 pipeline uses (L2D, or LCUBE for a
 *  cube twin). */
export function group1Entries(stages: number, kinds: number, cube0: boolean, visibility: number): GPUBindGroupLayoutEntry[] | null {
  const tex: GPUBindGroupLayoutEntry[] = [], smp: GPUBindGroupLayoutEntry[] = [];
  for (let k = 0; k < stages; k++) {
    const kind = k === 0 && cube0 ? 1 : stageKind(kinds, k);
    if (kind > 2) return null;
    tex.push({ binding: texBinding(k), visibility,
      texture: kind === 2 ? { sampleType: 'depth', viewDimension: '2d' } : { sampleType: 'float', viewDimension: kind === 1 ? 'cube' : '2d' } });
    smp.push({ binding: smpBinding(k), visibility, sampler: { type: kind === 2 ? 'comparison' : 'filtering' } });
  }
  // in binding order: tex0..3, smp0..3, then tex4..7, smp4..7
  return [...tex.slice(0, 4), ...smp.slice(0, 4), ...tex.slice(4), ...smp.slice(4)];
}

/** D3DCMP values for the ALPHA_FUNC override: 7 = the Mac's "a >= aref" test, 8 = no test, no discard. */
export const ALPHA_GREATEREQUAL = 7;
export const ALPHA_ALWAYS = 8;

/** ALPHA_FUNC for a draw, as the Mac picks its no-discard twin (MDraw.alpha_ref <= 0). */
export function alphaFunc(alphaRef: number): number {
  return alphaRef > 0 ? ALPHA_GREATEREQUAL : ALPHA_ALWAYS;
}

/** Override constants every pixel shader module declares (unknown keys are a pipeline error). */
export interface PsConstants { ALPHA_FUNC: number; PS20_TEXCOORD?: number }

/** True when pixel shader module `ps_<id>` is a cube twin and needs the cube layout. */
export function isCubePs(id: number): boolean { return id >= PSCUBE; }

export interface ShaderAbi {
  /** group 0: VB (storage), K and P (uniform, dynamic offsets in that order), H (uniform, the frame's). */
  group0: GPUBindGroupLayout;
  /** group 1: tex0..tex3 (2d) at bindings 0..3, smp0..smp3 at 4..7. */
  group1: GPUBindGroupLayout;
  /** group 1 with tex0 as a cube texture. */
  group1Cube: GPUBindGroupLayout;
  layout2d: GPUPipelineLayout;
  layoutCube: GPUPipelineLayout;
}

export function createShaderAbi(device: GPUDevice): ShaderAbi {
  const V = GPUShaderStage.VERTEX, F = GPUShaderStage.FRAGMENT;
  const group0 = device.createBindGroupLayout({
    label: 'abi.group0',
    entries: [
      { binding: 0, visibility: V, buffer: { type: 'read-only-storage', hasDynamicOffset: false } },
      { binding: 1, visibility: V, buffer: { type: 'uniform', hasDynamicOffset: true, minBindingSize: KWIN_SIZE } },
      { binding: 2, visibility: F, buffer: { type: 'uniform', hasDynamicOffset: true, minBindingSize: PSPAR_SIZE } },
      { binding: 3, visibility: F, buffer: { type: 'uniform', hasDynamicOffset: false, minBindingSize: HLPAR_SIZE } },
    ],
  });
  const group1 = device.createBindGroupLayout({ label: 'abi.group1', entries: group1Entries(4, 0, false, F)! });
  const group1Cube = device.createBindGroupLayout({ label: 'abi.group1.cube', entries: group1Entries(4, 0, true, F)! });
  return {
    group0, group1, group1Cube,
    layout2d: device.createPipelineLayout({ label: 'abi.layout2d', bindGroupLayouts: [group0, group1] }),
    layoutCube: device.createPipelineLayout({ label: 'abi.layoutCube', bindGroupLayouts: [group0, group1Cube] }),
  };
}
