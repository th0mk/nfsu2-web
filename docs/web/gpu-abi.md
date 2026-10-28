# WGSL shader ABI (translator ↔ WebGPU renderer)

Status: **2026-10-18**, authoritative; §11 (Most Wanted: 8 texture stages, binding kinds, depth textures; GW_VERSION
6) added 2026-10-25. Owner: track `gpu-shaders`. Consumers: track `gpu-renderer` (`runtime/web/gpu_web.c`,
`web/src/gpu/*`).

This is the contract between the WGSL emitted on the game thread (the D3D9 shader translator, `runtime/vsemu.c` +
`runtime/web/vsemu_wgsl.inc`) and the TypeScript WebGPU encoder. It follows `DESIGN.md` §3; every deviation is listed in
§10 with the reason. The semantics of the emitted code are the Mac port's (`vs_to_msl`, `ps_to_msl_ex2`, `metal.m`
`kPrelude`/`kShaderSrc`), including its known quirks (§8), so the web build looks like the Mac build.

## 1. Modules and entry points

Every module is **self-contained WGSL text** (structs, bindings, helpers and one entry point). The renderer passes it to
`createShaderModule` unchanged; nothing is prepended. Each module has exactly **one** entry point, so `entryPoint` may be
omitted in the pipeline descriptor.

| Module | Produced by | Entry point | Used when (MDraw) |
|---|---|---|---|
| Game VS, one per (VS object, declaration) pair | `vs_to_wgsl(prog, gid, decl, …)`, from `gpuvs_id` (`com.c`) | `@vertex fn vs_<gid>` | `vsid = gid > 0` |
| Game PS, one per pixel shader object | `ps_to_wgsl(prog, id, …)` at CreatePixelShader | `@fragment fn ps_<id>` (id 1..255) | `ps = id` |
| Game PS, stage-0 cube twin | `ps_to_wgsl_ex(prog, id + 256, …, cubemask = 1)` | `@fragment fn ps_<id+256>` | `ps = id + 256` (stage-0 texture is a cube) |
| TVert VS (CPU-transformed vertices) | `wgsl_tvert_module()` | `@vertex fn vs_0` | `vsid = 0` |
| Fixed-function PS (`col0 × tex0`) | `wgsl_ff_module(0, …, 0)` / `wgsl_ff_module(256, …, 1)` | `@fragment fn ps_0` / `ps_256` | `ps = 0` (cube twin: stage-0 texture is a cube) |

- Any VS module combines with any PS module: all of them use the same `VOut` (§3) and the same bind group layouts (§2).
- The **no-discard twin** of every PS is not a separate module: it is the same module with the override constant
  `ALPHA_FUNC = 8` (§5).
- **Registration.** On wasm, `com.c` calls `metal_register_vs(gid, wgsl)` and `metal_register_ps(id, wgsl)` (and
  `id + 256`) exactly where the Mac passes MSL. The text is NUL-terminated ASCII in a static buffer that is reused by the
  next translation: `gpu_web.c` must copy it into its resource queue during the call. `com.c` never registers the id-0
  modules; `gpu_web.c` gets them once at init from `wgsl_tvert_module()` / `wgsl_ff_module()` (declared in
  `runtime/web/wgsl.h`) and queues them like any other shader (suggested ids: VS 0, PS 0 and 256).
- **`metal_vs_ready(gid)`** cannot compile synchronously on the web. Answer **1** whenever the text was queued: the
  translator only emits constructs that the CI harness (`web/src/test/shadercheck.ts`, §9) compiles with zero errors for
  every shader the game creates. If `createShaderModule`/`getCompilationInfo` or pipeline creation still reports an error
  at run time, the renderer logs it loudly (once per module, with the module id) and skips draws that use that module.
  It must not block the game thread on a WebGPU promise.
- A translation that fails returns 0 to `com.c` before anything is registered, exactly as on the Mac: an unsupported VS
  opcode or vertex declaration type makes `gpuvs_id` return -1 and the draw takes the CPU (`vs_run` → TVert) path.

## 2. Bind groups (explicit layouts, shared by every pipeline)

```
group 0  (shared by runs of draws; K and P with dynamic offsets)
  binding 0  VERTEX    buffer read-only-storage   VB : array<u32>   hasDynamicOffset false
  binding 1  VERTEX    buffer uniform             K  : VSWin        hasDynamicOffset true   minBindingSize 65536
  binding 2  FRAGMENT  buffer uniform             P  : PSPar        hasDynamicOffset true   minBindingSize 560
  binding 3  FRAGMENT  buffer uniform             H  : HLPar        hasDynamicOffset false  minBindingSize 656
group 1  (textures and samplers of stages 0..3)
  binding 0  FRAGMENT  texture  sampleType float, viewDimension "2d"   (layout CUBE: "cube")   tex0
  binding 1  FRAGMENT  texture  sampleType float, viewDimension "2d"                             tex1
  binding 2  FRAGMENT  texture  sampleType float, viewDimension "2d"                             tex2
  binding 3  FRAGMENT  texture  sampleType float, viewDimension "2d"                             tex3
  binding 4..7  FRAGMENT  sampler type "filtering"                                               smp0..smp3
```

- Two pipeline layouts: **L2D** = `[G0, G1_2d]` and **LCUBE** = `[G0, G1_cube]`. A pipeline uses LCUBE iff its fragment
  module is a cube twin (`ps ≥ 256`: `ps_<id+256>` or `ps_256`); everything else uses L2D. This is the 4-stage layout
  with every binding kind 2D (§11): a game with 8 stages, or a module that declares another kind, gets the group 1 of
  §11.3 instead.
- `setBindGroup(0, g0, [kwin, poff])`: the dynamic offsets are in binding order (K, then P) and must be multiples of
  `minUniformBufferOffsetAlignment` (256). K is a **64 KB window** of the frame's VS constant arena (4096 rows, the
  default `maxUniformBufferBindingSize`), so the VS constant buffer needs 64 KB of slack after the last block. A draw's
  block starts at row `kb` of the window, given as the draw's `firstInstance` (§3): consecutive draws share one bind
  group, and a new one is set only for another vertex source, other PS parameters, or a block past the window
  (`kb > 3839`, so that the block's 257 rows stay inside it). VB has no dynamic offset: the byte offset of a draw's
  vertex 0 is the block header's `base` (§4), because storage-buffer dynamic offsets must be 256-aligned and pool
  offsets are not.
- VB is the whole buffer the draw reads: the raw-vertex arena (`metal_draw_raw`), the mirror pool (`metal_draw_ix`) or
  the TVert arena (`metal_draw_tv`). A buffer bound as VB needs usage `STORAGE`; the pool also needs `INDEX`.
  `maxStorageBufferBindingSize` is 128 MiB by default; request more at `requestDevice` if the pool can grow beyond it.
- Missing or freed textures: bind a 1×1 white texture (cube: a white 1×1 cube) as `metal.m` does. Samplers follow the
  `MDraw.smp[k]` keys (`gfx-d3d9.md` §2.3) and must be `filtering` samplers.
- A module may use any subset of these bindings; WebGPU allows layout entries that a shader does not use.
- `createShaderAbi(device)` in `web/src/gpu/shader-abi.ts` builds exactly these layouts (and exports the sizes,
  `PSCUBE`, `alphaFunc()`); the shader check builds its pipelines with it, so use it in the renderer too.

## 3. Structs

```wgsl
struct VOut {                      // interstage, identical in every module (7 of 16 locations; 11 with 8 stages, §11.2)
  @builtin(position) pos: vec4f,
  @location(0) col0: vec4f,  @location(1) col1: vec4f,          // oD0, oD1 (not clamped, as on the Mac)
  @location(2) uv0: vec4f,   @location(3) uv1: vec4f,           // stage k reads uvk (TEXCOORDINDEX applied in the VS)
  @location(4) uv2: vec4f,   @location(5) uv3: vec4f,
  @location(6) fog: f32,                                        // vertex fog factor, 1 = no fog
}
struct VSWin { c: array<vec4f, 4096> }   // 65536 bytes, uniform: the window
// a draw's block at row kb (= instance_index, the draw's firstInstance), 16-byte aligned in the arena:
//   row kb      the header, read as bitcast<vec4u>: x = stride (vertex stride in bytes), y = tsel (byte k =
//               TEXCOORDINDEX of stage k, 0..3), z = base (byte offset of this draw's vertex 0 in VB), w = 0
//   row kb+1+n  vertex shader constant cn, n = 0..255
struct PSPar {                     // 560 bytes, uniform (= the Mac's PSPar)
  aref: f32,                       //  0  MDraw.alpha_ref
  usec: f32, pad0: f32, pad1: f32, //  4  0 (HD fields on the Mac)
  proj: vec4f,                     // 16  MDraw.proj: stage k = coordIndex + 16 * projMode (as f32)
  c: array<vec4f, 32>,             // 32  pixel shader constants c0..c31 (MDraw.psci block)
  fog: vec4f,                      // 544 MDraw.fogv: FOGCOLOR rgb, a = FOGENABLE (0 or 1)
}
struct HLight { t: mat4x4f, col: vec4f }   // 80 bytes
struct HLPar {                     // 656 bytes, uniform (gpu_proto.h GwHL)
  n: u32,                          //  0  other cars' headlights this frame (0..8)
  s: f32, o: f32, pad: f32,        //  4  falloff: ramp u = s * w + o (the lit VS's own constants)
  l: array<HLight, 8>,             // 16  car k: its light coordinate is uv1 * t; col = its HEADLIGHTCOLOR
}
```

- **K header** is what `raw_rec` (`metal.m`) writes, plus `base`: `stride` = the draw's stride, `tsel` = `Σ ((int)proj[k] & 3) << 8k`,
  `base` = `rawoff` (raw arena offset for `metal_draw_raw`, the VB mirror's pool offset for `metal_draw_ix`). Only rows
  `c[0 .. vs_kmax)` need to be written; the shader reads no other row except through relative addressing, which is
  clamped to `[0, 255]` exactly like the Mac (rows past `vs_kmax` read whatever the arena holds there, as before).
  Constant blocks can stay deduplicated (the header is part of the key). The window offset is the uniform's dynamic
  offset, the row the draw's `firstInstance`: `instance_index` includes `firstInstance` on every backend (WGSL), and
  the draws are not instanced.
- **TVert draws** (`vsid = 0`): any K window; draw with `firstVertex = MDraw.first`. `vs_0` reads no K at all:
  `VB[vertex_index * 112]` from the start of the TVert arena (a TVert is always 112 bytes: pos, col0, col1, t[4], all
  `f32`, `gfx.h`).
- **P** is the Mac's per-draw `PSPar` (`metal.m` encoder): the renderer fills it from the MDraw record and the PS constant
  pool, 256-aligned (a 768-byte slot stride is the simplest).
- **H** is the frame's, not the draw's: bytes `[GW_K_HL, + 656)` of the frame's K arena (`gpu_web.c` keeps the draws'
  blocks after them), bound at that fixed offset of the K buffer. Only the worldheadlight pixel shaders declare it: the
  translator adds `hl_more()` (the other cars' headlights, `runtime/headlights.c`) after their `t1 * t2 * c0 + v0`
  (`vsemu_wgsl.inc` `W_HL`); a web-only addition, the Mac has no such term.

## 4. Vertex pulling (game VS modules)

- Vertex address: `a = base + vertex_index * stride` (the block header, §3; u32 arithmetic). `vertex_index` includes `baseVertex` of
  `drawIndexed`, so D3D9 `BaseVertexIndex` is `drawIndexed(count, 1, firstIndex, ix_base)`. `firstVertex` of `draw()`
  also adds to `vertex_index`.
- Every input register `vN` that the declaration maps is decoded at `a + offset` with the formula below, baked into the
  module (offset and type come from the declaration, as `decl_read_msl` does). Loads are little-endian byte loads from
  `array<u32>` and work at **any byte alignment** (a u32 load at a misaligned address combines two words).
- Unmapped input registers read `(0, 0, 0, 1)`.
- The normalising divisions are rounded like C's (reciprocal product plus one FMA residual step), so the decoded values
  are **bit-exact** with `decl_fetch`, the CPU path. A plain WGSL division is not correctly rounded on the GPU (measured:
  126 of the 256 byte values of b/255 off by one ulp); the Mac's MSL path has the same one-ulp drift under fast math.

| D3DDECLTYPE | Decoded `vec4f` (bytes b[i] at `a + offset + i`) | = `com.c decl_fetch` |
|---|---|---|
| 0 FLOAT1 | (f32 b0..3, 0, 0, 1) | ✓ |
| 1 FLOAT2 | (f32, f32, 0, 1) | ✓ |
| 2 FLOAT3 | (f32, f32, f32, 1) | ✓ |
| 3 FLOAT4 | (f32, f32, f32, f32) | ✓ |
| 4 D3DCOLOR | (b2, b1, b0, b3) / 255 | ✓ (memory is B,G,R,A) |
| 5 UBYTE4 | (b0, b1, b2, b3), not normalised | ✓ |
| 8 UBYTE4N | (b0, b1, b2, b3) / 255 | ✓ |
| 6 SHORT2 | (s16, s16, 0, 1) | ✓ |
| 7 SHORT4 | (s16, s16, s16, s16) | ✓ |
| 9 SHORT2N | (s16 / 32767, s16 / 32767, 0, 1) | ✓ (−32768 → −1.00003, unclamped) |
| 10 SHORT4N | s16 / 32767 ×4 | ✓ |
| 11 USHORT2N | (u16 / 65535, u16 / 65535, 0, 1) | ✓ |
| 12 USHORT4N | u16 / 65535 ×4 | ✓ |
| 13 UDEC3, 14 DEC3N, 15 FLOAT16_2, 16 FLOAT16_4, ≥ 17 | **rejected**: no module, `gpuvs_id` → -1, CPU path | as the Mac |

## 5. Override constants (every PS module declares exactly these)

| Name | Type, default | Meaning |
|---|---|---|
| `ALPHA_FUNC` | `u32`, `7` | Alpha test of the final colour against `P.aref`, D3DCMP numbering: 1 NEVER, 2 LESS, 3 EQUAL, 4 LESSEQUAL, 5 GREATER, 6 NOTEQUAL, 7 GREATEREQUAL, **8 ALWAYS = no test, no `discard` in the pipeline** |
| `PS20_TEXCOORD` | `bool`, `true` | ps_2_0 only: `true` = D3D behaviour (`t#` = `uv#`), the default; `false` = the Mac's look (the `t#` coordinate registers read 0, §8), for an A/B |

- **Mac parity:** `ALPHA_FUNC = 7` when `MDraw.alpha_ref > 0`, else `8`. The second is the Mac's no-discard twin
  (`g_psfn_nd`), chosen for the same reason: `discard` disables hidden-surface removal on Apple GPUs. The comparison for
  7 is written `!(a < aref)` so NaN behaves like the Mac's `if (a < aref) discard`. The other values give full D3D9
  ALPHAFUNC support if the recorder ever passes the real function (`com.c` today folds 5, 6, 7 into "≥ aref").
- Because unknown keys are a pipeline-creation error in WebGPU and declared-but-unused ones are not (both checked in
  Chrome 155), the renderer can pass the same `constants` record to every PS: `{ ALPHA_FUNC: n }` (and
  `PS20_TEXCOORD` when the A/B is wired). VS modules declare **no** override constants.
- Pipeline key: (VS module, PS module, ALPHA_FUNC == 8 ? 1 : 0, …render state). The cube twin is a different PS module.

## 6. Pixel shader semantics (summary; `vsemu.c` is the reference)

- Inputs: `v0 = col0`, `v1 = col1`. Constants: `def` → literal (emitted as a function-scope `let`, so constant folding can
  never hit a WGSL const-expression error), else `P.c[n & 31]`.
- ps_1_1–1_3 `tex tN`: `textureSample(texN, smpN, uvN.xy / d)`, `d` = 1, `uvN.z` or `uvN.w` for `P.proj[N] >> 4` = 0, 1,
  2. ps_1_4 `texld rN, tM`: sampler N, coordinate M with M's divide. ps_1_4 dependent `texld rN, rM`: `rM.xy` (no
  divide). ps_2_0 `texld d, tK, sN`: `tK.xy`. Cube stages sample `uvM.xyz` (no divide) or `src.xyz`.
- Result: `r0` (ps_1_x) or `oC0` (ps_2_0) → alpha test (§5) → fog
  `rgb = mix(P.fog.rgb, rgb, mix(1, saturate(fog), P.fog.a))` → `@location(0)`.
- `texkill` (no NFSU2 shader has one): its register is the instruction's destination token. ps_1_x: discard if any of
  the first three components is negative, of texture coordinate set N (`texkill tN` → `uvN.xyz`) or of a temp
  (ps_1_4 `texkill rN`); ps_2_0: all four components of the register (as Wine). This is D3D9's rule, not the Mac's:
  `ps_to_msl_ex2` reads the instruction's unset source operand (stale text from the previous instruction), a bug not
  carried over. The reference interpreter `ps_ref_run` does the same.
- `textureSample` is only ever called in uniform control flow (straight-line code; the projective divide uses `select`).
- Fixed-function PS (`ps_0`/`ps_256`): `col0 * textureSample(tex0, smp0, uv0.xy)` (cube: `uv0.xyz`), then the same alpha
  test and fog. The cube twin has no Mac equivalent (the Mac would bind a cube to a 2D slot); it exists so a cube at stage
  0 with no pixel shader cannot invalidate a bind group.

## 7. Vertex shader semantics (summary)

- Registers `r0..r31`, `a0` (`vec4i`), outputs `oPos` = (0,0,0,1), `oD0` = (1,1,1,1), `oD1` = 0, `oT0..oT3` = 0,
  `oFog` = (1,1,1,1) (no fog), all written as on the Mac (`oT4..7` alias `oT0..3` through `& 3`, as `vs_dst_msl` does).
- Epilogue: `pos = oPos` (no half-pixel offset), `col0/col1 = oD0/oD1` (no clamp), `uvK = oT[(tsel >> 8K) & 3]` (the header's `tsel`),
  `fog = oFog.x`.
- `vs_0` (TVert): pass-through of the 112-byte record, `fog = 1` (the Mac's `vmain`).

## 8. Mac-parity quirks kept on purpose (do not "fix" in the renderer)

| Quirk | Mac source | Note |
|---|---|---|
| No half-pixel offset; oD0/oD1 not clamped | `vs_to_msl` epilogue | |
| `mov a0.x, r` truncates toward zero; `mova` rounds half away from zero | MSL implicit conversion / `round()` | WGSL `round` is half-to-even, so the emitter writes the Mac's rounding explicitly |
| ps_1_x unsupported texture ops (`texreg2ar/gb`, `texbem`, `texm3x*`, `bem`) and `phase` are skipped | `default: continue` | Only the SCREENFILTER fallback technique has them (two ps_1_1 shaders the game creates but never selects, because the device reports PS 2.0) |
| VS `logp` = `log` (one component); PS `logp` is the 4-component form | `vs_to_msl` / `ps_to_msl_ex2` | |
| `m4x4`-family rows always read `K.c[n + row]` (ignore `def` and relative addressing) | `vs_to_msl` | NFSU2 does not use them |
| ps_1_4 `texcrd rN, tM` writes `t[N] = uvN`; co-issued pairs run sequentially | `ps_to_msl_ex2` | NFSU2 has no `texcrd`; its 15 co-issued instructions are safe sequentially (`gfx-d3d9.md` §3.5) |
| Alpha test is "≥ aref" only (from `com.c`) | `com.c:719` | §5 allows the full set later |

**Not kept: the ps_2_0 `t#` registers.** The Mac never loads them (`vsemu.c` ps_to_msl_ex2): they read 0, so depth of
field (`ps_30`) and rain drops (`ps_31`) sampled their textures at (0,0). Depth of field runs in some of the camera
scenes before and after a race. There every pixel took the top-left texel, the black letterbox bar, which the tint
passes after it turned into a flat colour, and only the light glows (added later) showed. The game's random choices
decide whether a race intro is one of those: 5 of 18 quick-race runs (2026-10-20). The rain drops on the lens did
not show at all. Since then `PS20_TEXCOORD` defaults to `true` (D3D: `t#` = `uv#`), `false` gives the Mac's look,
and `ps_ref_run` loads them too. `uvK` has TEXCOORDINDEX applied in the vertex epilogue; for both shaders' draws that
is the identity (`tsel` 0x03020100), as D3D requires with a vertex shader.

Deliberate differences from the Mac's **text** (not its semantics), both toward the Mac's CPU reference path (`vs_run`,
`decl_fetch`): `def` constants are printed with 9 significant digits (`%.9g`, exact for f32) instead of `%g` (6 digits),
and the vertex-pulling divisions are correctly rounded (§4). Both differences are below 1e-6 relative.

## 9. Validation

Run it all with one command (the server on port 8612, `web/dist` built, the wasm game built):

```
node web/tools/shader-check.mjs --game        # dump + pairs + check; exit 0 = all good
node web/tools/shader-check.mjs --from <dir>  # re-check a dump on disk (--census: also the game-run floor)
```

Exit 0 means all of: every check below passes; no `REJECTED` line in the dump's `index.txt` or in `pairs.txt`; and for
`--game`, the game ran the whole quick race (`EXIT 0`, the race reached, `quit at guest frame`, no fault marker) and
the dump has at least a full quick race's census: 16 distinct VS objects, 24 distinct PS objects, 11 distinct
declarations, 17 registered (VS, declaration) modules, 68 PS modules (with the cube twins). A run that crashes or stops
early gives a smaller dump and fails (`--script` and `--env` run another scene for a dump).

1. **Dump.** `NFSU2_SHADER_DUMP=<dir>` (wasm build, `runtime/web/shader_dump.c`) makes the game write every shader
   object it creates (`vsobj_<n>.bin`/`psobj_<id>.bin` + disassembly), every vertex declaration (`decl_<obj>.bin`),
   every module it registers (`vs_<gid>.wgsl`, `ps_<id>.wgsl`, `ps_<id+256>.wgsl`), the built-ins and
   `pulltest.wgsl`, with `index.txt`. `--game` runs `tests/scripts/quickrace.txt` headless (null renderer, DESIGN.md §5
   environment) and copies the dump from OPFS to `out/web/shaders/` (game-derived: never committed).
2. **Pairs.** A quick race draws only 10 of the 16 distinct vertex shaders, so `web/tools/wgsl-xl --pairs` (the same C
   translator built natively) translates every vertex shader object against every dumped declaration that supplies all
   of its inputs (ids from 1001).
3. **Check** (`/test/shadercheck.html`, `web/src/test/shadercheck.ts`):
   - every module: `createShaderModule` + `getCompilationInfo` (+ a validation error scope): **0 errors**;
   - render pipelines with the explicit layouts of §2 (`web/src/gpu/shader-abi.ts`): each game VS with `ps_0`; `vs_0`
     with every PS and cube twin at `ALPHA_FUNC` 7, 8 and with `PS20_TEXCOORD` off; `&cross` adds every VS × every PS;
   - vertex pulling: `pulltest.wgsl` decodes all 13 declaration types at byte offsets 0..7 from 8 bases and must equal
     the CPU decoder (`decl_fetch` semantics) **bit for bit**;
   - vertex shaders: each WGSL vertex function runs as a compute function on random vertices and constants and is
     compared with `vs_run` (the Mac's CPU interpreter) on the same bytes;
   - pixel shaders: each fragment function runs as a compute function (analytic texture, `discard` as a flag) and is
     compared with `ps_ref_run`, a reference interpreter of the Mac's pixel shader semantics (but the ps_2_0 `t#`
     registers, §8). Colours within 1e-3
     relative; kill flags equal, except an alpha-test decision that flips because both alphas are within that
     tolerance of `aref` (counted as `edgeFlips`; SwiftShader/Vulkan has one, Metal none).
     `ps_ref_run` covers ps_1_x and ps_2_0 only: it refuses ps_2_x and ps_3_0 (their vectors hold zeros), whose
     reference is `web/tools/psref.c`, which follows D3D9 instead of the Mac (`docs/web/psref.md`); vs_3_0's is too.

Results on Chrome 155 / Apple M4 Pro (2026-10-18): 167 modules, 0 compile errors, 307 pipelines, 0 errors; vertex
pulling 3,328/3,328 exact; 96 vertex shader modules max relative error 2.0e-6; 68 pixel shader modules max relative
error 1.1e-6, all kill flags equal.

**Note for the page:** the game only takes the GPU vertex shader path when `GPUVS=1` is in its environment (the Mac's
`bundle.m` and `run.sh` set it). Without it every vertex shader runs in `vs_run` on the CPU (in wasm) and draws go
through `vs_0`.

## 10. Deviations from DESIGN.md §3

| DESIGN.md §3 | Here | Why |
|---|---|---|
| `K = {stride, tsel, base, pad; c[NK]}` | K is a 64 KB window, `array<vec4f, 4096>`; the draw's block `{stride, tsel, base, pad; c[256]}` starts at row `firstInstance` | One bind group for many draws: a per-draw dynamic offset cost ~1,300 `setBindGroup` calls a race frame (page thread and GPU process, ~0.45 ms of CPU per frame on a Ryzen 5900X); the layout still fixes `minBindingSize` (no per-draw size validation), relative addressing never indexes past the window, and the recorder uploads only `vs_kmax` rows. The price: the constant reads use a per-vertex index on the GPU (+3.7 % GPU time per frame, 0.89 → 0.92 ms at 4K) |
| group 0 "dynamic offsets" | K and P dynamic, VB not | WebGPU storage dynamic offsets must be 256-aligned; mirror-pool vertex offsets are arbitrary, so the offset is the header's `base` |
| Alpha test as an override "no-discard variant" | `ALPHA_FUNC: u32` (D3DCMP, 8 = no discard) | Same twin, plus room for the full ALPHAFUNC set |
| `t0..t3`, `s0..s3` | `tex0..tex3`, `smp0..smp3` | Names only (the D3D register names `t#`, `r#` are used for registers); bindings are as in DESIGN |
| (not in DESIGN) | `PS20_TEXCOORD` override, FF PS cube twin, TVert `firstVertex` convention | Documented above |

## 11. Most Wanted: 8 texture stages, binding kinds, depth textures (GW_VERSION 6)

The additions Most Wanted's shaders need, written before the translator, D3DX and renderer work split (MW tracker
M1-24). They are the contract; nothing here is implemented beyond the protocol (`gpu_proto.h`, `proto.ts`), the group 1
layouts (`shader-abi.ts` `group1Entries`) and their check (`web/tools/check-gpu-proto.mjs`). **UG2 uses none of it**:
every new field is 0 in UG2's records, and 0 means exactly what the protocol meant before version 6.

### 11.1 What the game's shaders use

From Most Wanted's 31 compiled effects (blobs counted to their end token; register use from the bytecode):

| Need | Where | ABI before 6 |
|---|---|---|
| 5 samplers, s0..s4 | the reflective world effect's ps_2_0 and ps_2_x (s4 is the shadow map) | 4 stages |
| a cube at stage 1 (`dcl_cube s1`) | the car effect's environment map: 1 ps_2_0, 4 ps_2_x | cube only at stage 0 (+256 twin) |
| 8 texture coordinates | ps_2_0 and ps_2_x read t0..t7 (t4..t7 in 14, 12, 4 and 8 of the 18 ps_2_x); vs_1_1 writes oT4..oT7 (20, 17, 6 and 12 of 69 shaders); vs_3_0 writes texcoord0..4, ps_3_0 reads texcoord0..4 and color0..1 | uv0..uv3, oT4..oT7 alias oT0..oT3 (§7) |
| depth-compare sampling | the shadow map at stage 1, 2, 3 or 4 depending on the effect, read by `texldp` (1, 4 or 16 taps) and only its `.x` used | none |

Not used anywhere, so not in the ABI: more than one colour output (every pixel shader writes oC0 only, ps_1_x r0), oDepth,
vPos and vFace, dsx/dsy, texldd, texldb, texldl (the two vs_3_0 shaders fetch no textures), if/loop/call/label/break,
predicates, boolean constants. ps_3_0 has `rep i0` with `defi i0` (a literal count) in its 2 shaders: no integer
constants from the game. Pixel shader constants stop at c20 (ps_2_x), c7 (ps_2_0) and c3 (ps_3_0): PSPar's 32 rows
suffice. Vertex shader constants named directly stop at c83 (vs_1_1) and c20 (vs_3_0), inside a K block's 256 rows.

**Float render targets: none.** The exe's creation calls push only A8R8G8B8, X8R8G8B8, D24S8 and 'DF16' as immediate
formats, its render-target table holds A8R8G8B8 in every format slot (the code writes only its size slots), and no
float D3DFMT (111..116) is pushed or moved in the 30 instructions before any CreateTexture, CreateCubeTexture,
CreateRenderTarget, CreateDepthStencilSurface, CreateOffscreenPlainSurface or CheckDeviceFormat call. So the ABI adds
no float formats, and the MW runtime answers CheckDeviceFormat for them with D3DERR_NOTAVAILABLE (§11.4). This is a
static reading; a run that logs CheckDeviceFormat's arguments confirms it (today's com.c answers D3D_OK for every
format).

### 11.2 Texture stages: `GwCtl.stages`

The game writes `stages` before it registers the control block and never changes it: **0 (UG2) or 4** = the ABI of
§1-§10; **8** (`GW_STAGES_MAX`) = every draw has 8 texture stages and every module of the process is built for them:

- **Draws.** `GwDraw.tex4..tex7` are the texture ids of stages 4-7 (0 = white) and `GwDraw.smphi[0..3]` their packed
  sampler keys, encoded as `smp` (com.c `smp_push`: bits 0-1 MINFILTER, 2-3 MAGFILTER, 4-5 MIPFILTER, 6-8 ADDRESSU, 9-11
  ADDRESSV, 12-14 log2 MAXANISOTROPY). The page reads them only with 8 stages. `proj` stays 4 wide: only ps_1_x and
  the fixed-function stage read it, and no MW ps_1_x shader samples past stage 3.
- **Group 1.** Stage k's texture at binding `texBinding(k)` and its sampler at `smpBinding(k)` (`shader-abi.ts`):
  stages 0-3 as in §2 (0..3 and 4..7), stages 4-7 at 8..11 and 12..15. 16 entries; WebGPU's default limits allow 16
  sampled textures and 16 samplers per stage.
- **VOut.** Locations 0..6 as in §3, then `@location(7) uv4: vec4f` .. `@location(10) uv7: vec4f` (`VOUT_LOCATIONS_8` =
  11 of the 16 inter-stage variables WebGPU guarantees). Every module of the process declares the same VOut, the
  built-ins (`vs_0`, `ps_0`, `ps_256`) included, so any vertex module still pairs with any pixel module. A vertex
  shader's oT4..oT7 are four more registers (no `& 3` alias), and uv4..uv7 are oT4..oT7 as they are: the K header's
  `tsel` (TEXCOORDINDEX) covers stages 0-3 only. ps_2_x and ps_3_0 inputs follow their declarations (texcoord N = uvN,
  color0/1 = col0/col1). `vs_0` writes uv4..uv7 = 0, since a TVert holds 4 coordinates: a pixel shader that reads t4..t7
  on the CPU vertex path reads 0 there, which is one more reason M5 allows no CPU fallback (L-083).

### 11.3 Binding kinds: `GW_SHADER_PS` a[2]

Each pixel shader module record says how the module declares each stage's texture: 2 bits per stage k at bit 2k
(`GW_BK(kinds, k)`).

| Kind | WGSL texture | WGSL sampler | Bind group layout entries |
|---|---|---|---|
| 0 `GW_BK_2D` | `texture_2d<f32>` | `sampler` | `sampleType 'float'`, `'2d'`; `'filtering'` |
| 1 `GW_BK_CUBE` | `texture_cube<f32>` | `sampler` | `sampleType 'float'`, `'cube'`; `'filtering'` |
| 2 `GW_BK_DEPTH` | `texture_depth_2d` | `sampler_comparison` | `sampleType 'depth'`, `'2d'`; `'comparison'` |
| 3 | not defined: the page makes no pipeline for the module and logs it | | |

- ps_2_0 and later take the kind from their declarations (`dcl_2d` 0, `dcl_cube` 1); a `dcl_volume` stays what it is
  today, a translation that fails before registration (§1). ps_1_x and the fixed-function stage register 0 everywhere;
  their stage-0 cube stays the +256 twin, whose stage 0 is a cube whatever the word says. A depth stage (§11.4) turns
  its 0 into 2.
- **The page** builds a pipeline's layout as `[G0, group1Entries(stages, kinds, ps & 256)]` and caches it by those
  three. With 4 stages and kinds 0 that is exactly L2D or LCUBE (check-gpu-proto.mjs compares the entries).
- **Bind groups.** Each stage gets its bound texture when that texture is of the stage's kind (a 2D texture for 0, a cube
  for 1, a depth texture for 2), and otherwise the kind's stand-in: the 1×1 white 2D or cube texture as today, and for
  a depth stage a 1×1 depth texture cleared to 1.0, whose compare always passes (no shadow). A texture that is the
  pass's colour target or Z gets the stand-in too: WebGPU allows no texture as attachment and binding in one pass.

### 11.4 Depth textures

- **Creation.** CreateTexture with D3DUSAGE_DEPTHSTENCIL (2) and D3DFMT_D16 (80), D24X8 (77) or D24S8 (75) queues
  `GW_TEX_CREATE` with `GW_TF_DEPTH`. The page creates it as `depth32float`, the format of every Z surface (so a
  pipeline's `depthStencil.format` stays the one it is), with usage RENDER_ATTACHMENT | TEXTURE_BINDING. It is never
  uploaded, never read back (`rb_*`) and no GW_BLIT source or target (such a blit is skipped and logged).
- **As Z.** Its level-0 surface set with SetDepthStencilSurface makes the draws' and clears' `depth` field bits 8+
  `GW_Z_TEX` + its texture id (64 + id; created Z surfaces stay at 2..63). The colour target must have the same size
  (WebGPU; D3D9 only asks for a Z at least as large); otherwise the draw gets the page's scratch Z and a log line.
- **As a texture.** At each draw the game finds the stages whose bound texture is a depth texture and draws with module
  `ps = shader | 256 × (stage-0 cube twin) | stages << GW_PS_DEPTH_SHIFT` (bit 9 + k for stage k). The first draw that
  needs such a variant registers it under that whole id: the same shader translated with those stages of kind 2. Its
  record precedes the frame in the resource queue, as every shader's does. UG2 never sets bits 9 and up. (gpu_web.c's
  shader table holds ids below 512 today; the implementation widens it.)
- **Sampling** (NVIDIA's hardware shadow maps: the vendor id 0x10DE takes MW to them). `texld`, `texldp` and ps_1_x `tex`
  on a depth stage take the coordinate after the divide the instruction asks for (`texldp`: xyz / w; ps_1_x: the
  P.proj divide) and return `vec4f(textureSampleCompareLevel(texK, smpK, q.xy, q.z))`: the comparison result in all
  four components. The comparison sampler has `compare: 'less-equal'` (1 when the reference is at or in front of the
  stored depth, D3D's LESSEQUAL) and the filters of the stage's sampler key, so LINEAR gives the hardware's 2×2 PCF.
  `textureSampleCompareLevel` reads level 0 (a depth texture has one) and is allowed in any control flow, the ps_3_0
  `rep` loops included.
- **Not provided: the raw-depth path.** Some of the game's shadow shaders read the map with a plain `texld` and compare
  in the shader (ATI's DF16/DF24 and INTZ formats, where a lookup returns the stored depth). The game takes that path
  only when CheckDeviceFormat accepts 'DF16' as a depth-stencil texture (the probe at 0x6C174E), so the MW runtime
  answers D3DERR_NOTAVAILABLE (0x8876086A) for DF16, DF24 and INTZ, and D3D_OK for D24S8 as a depth-stencil texture
  (0x6C1725). Kind 3 is left free for it.
- **Not provided: a border colour.** WebGPU has none; the page approximates D3DTADDRESS_BORDER by clamping and logs it
  (L-088). If the shadow map's sampler uses BORDER, the translator adds "outside [0, 1]: 1" to its depth stages.

### 11.5 Deploy order

Version 6 changes the layout word (GwDraw 156 → 188 bytes, GwCtl 408 → 412). A page and a module work together only
from one commit: release UG2's module and image together, as for version 5. A page of one build with a module of the
other stops at Play with "GPU control block mismatch" and reloads once around the service worker (`OtherBuild`,
e005ab5); there is no mixed window in either direction.

### 11.6 Fields, UG2 values, first users

| Field | Meaning | UG2 | First used by (MW tracker) |
|---|---|---|---|
| `GwCtl.stages` | texture stages of every draw and module: 0/4 or 8 | 0 | the first MW world draws (ps_2_0 world shaders read t7 and s4 already at SM2): M5-12 for the translator, M5-18 for the reflective world and the car's cube |
| `GwDraw.tex4..tex7` | stage 4-7 textures | 0 | as `stages` |
| `GwDraw.smphi[4]` | stage 4-7 sampler keys | 0 | as `stages` |
| VOut uv4..uv7 (locations 7..10) | coordinates 4-7 | absent (7 locations) | as `stages` |
| `GW_SHADER_PS` a[2], `GW_BK_*` | binding kind per stage | 0 | M5-12 (cube at stage 1), M5-13 (depth) |
| `GW_TF_DEPTH` | a depth texture | never set | M5-13 |
| `GW_Z_TEX` | depth texture as a draw's Z | never reached (zid < 64) | M5-13 |
| `GW_PS_DEPTH_SHIFT` bits of `GwDraw.ps` | stages sampled with a depth compare | 0 | M5-13 |
| float render-target formats | not added (§11.1) | | |
