// Built-in WGSL: the copy/composite passes, and fallbacks for the
// pretransformed-vertex (TVert) path and the fixed-function fragment stage
// (the translator's vs_0 / ps_0 / ps_256 replace them as soon as the game
// registers those, docs/web/gpu-abi.md).  The game's own shaders come
// from the translator (track gpu-shaders) over the same bindings:
//   group 0: VB (storage, vertex pulling), K (VS constants, dynamic offset),
//            P (PS parameters, dynamic offset)
//   group 1: t0..t3 at bindings 0..3, s0..s3 at bindings 4..7
// and the same inter-stage block (VOut), so a translated pixel shader can sit
// behind the TVert vertex stage and the other way round.

export const VOUT = /* wgsl */`
struct VOut {
  @builtin(position) pos: vec4f,
  @location(0) col0: vec4f,
  @location(1) col1: vec4f,
  @location(2) uv0: vec4f,
  @location(3) uv1: vec4f,
  @location(4) uv2: vec4f,
  @location(5) uv3: vec4f,
  @location(6) fog: f32,
}`;

const PSPAR = /* wgsl */`
struct PSPar { aref: f32, usec: f32, pad0: f32, pad1: f32, proj: vec4f, c: array<vec4f, 32>, fog: vec4f }
@group(0) @binding(2) var<uniform> P: PSPar;`;

// The CPU vertex path (com.c draw_up -> metal_draw_tv): clip-space vertices,
// both colours and four 4-component texture coordinates, pulled from the
// frame's TVert arena (28 floats per vertex).  As metal.m vmain: no fog.
export const TVERT_VS = /* wgsl */`
${VOUT}
@group(0) @binding(0) var<storage, read> VB: array<f32>;
@vertex fn vs_tvert(@builtin(vertex_index) vi: u32) -> VOut {
  let b = vi * 28u;
  var o: VOut;
  o.pos  = vec4f(VB[b],       VB[b + 1u],  VB[b + 2u],  VB[b + 3u]);
  o.col0 = vec4f(VB[b + 4u],  VB[b + 5u],  VB[b + 6u],  VB[b + 7u]);
  o.col1 = vec4f(VB[b + 8u],  VB[b + 9u],  VB[b + 10u], VB[b + 11u]);
  o.uv0  = vec4f(VB[b + 12u], VB[b + 13u], VB[b + 14u], VB[b + 15u]);
  o.uv1  = vec4f(VB[b + 16u], VB[b + 17u], VB[b + 18u], VB[b + 19u]);
  o.uv2  = vec4f(VB[b + 20u], VB[b + 21u], VB[b + 22u], VB[b + 23u]);
  o.uv3  = vec4f(VB[b + 24u], VB[b + 25u], VB[b + 26u], VB[b + 27u]);
  o.fog  = 1.0;
  return o;
}`;

// metal.m fmain/fmain_nd: col0 x tex0, alpha test (a < ref discards), vertex
// fog.  ALPHA_FUNC as in docs/web/gpu-abi.md: 7 = the Mac's test, 8 = the
// no-discard twin (alpha_ref <= 0) that keeps hidden-surface removal on tile
// GPUs.  Used only until the translator's ps_0 / ps_256 arrive.
export const FF_FS = (cube: boolean) => /* wgsl */`
${VOUT}
${PSPAR}
@group(1) @binding(0) var t0: ${cube ? 'texture_cube<f32>' : 'texture_2d<f32>'};
@group(1) @binding(4) var s0: sampler;
override ALPHA_FUNC: u32 = 7u;
@fragment fn fs_ff(i: VOut) -> @location(0) vec4f {
  var c = i.col0 * textureSample(t0, s0, ${cube ? 'i.uv0.xyz' : 'i.uv0.xy'});
  if (ALPHA_FUNC == 7u && c.a < P.aref) { discard; }
  return vec4f(mix(P.fog.rgb, c.rgb, mix(1.0, saturate(i.fog), P.fog.a)), c.a);
}`;

// Full-target triangle sampling one texture: StretchRect between differently
// sized targets and the final composite to the canvas.  uv (0,0) = top left,
// as metal.m's blit quad.
export const COPY = /* wgsl */`
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var smp: sampler;
struct BOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f }
@vertex fn vs_full(@builtin(vertex_index) i: u32) -> BOut {
  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
  var o: BOut;
  o.pos = vec4f(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
  o.uv = p;
  return o;
}
@fragment fn fs_copy(i: BOut) -> @location(0) vec4f { return textureSample(src, smp, i.uv); }
@fragment fn fs_opaque(i: BOut) -> @location(0) vec4f { return vec4f(textureSample(src, smp, i.uv).rgb, 1.0); }`;

// The composite with Exposure and White Level (runtime/tonemenu.c, GwTone): each channel times mult (at most 1),
// then a value above knee * white bends down so that 1 lands on white (a rational shoulder: slope 1 where it starts,
// so no edge shows).  tone: mult, white, knee.
export const TONE = COPY + /* wgsl */`
@group(0) @binding(2) var<uniform> tone: vec4f;
@fragment fn fs_tone(i: BOut) -> @location(0) vec4f {
  let y = min(textureSample(src, smp, i.uv).rgb * tone.x, vec3f(1.0));
  let s = tone.z * tone.y;
  let m = (1.0 - s) / (tone.y - s);
  let t = max(y - s, vec3f(0.0)) / (1.0 - s);
  let b = s + (tone.y - s) * (m * t) / (1.0 + (m - 1.0) * t);
  return vec4f(select(y, b, y > vec3f(s)), 1.0);
}`;
