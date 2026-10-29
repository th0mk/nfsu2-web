# psref: the D3D9 reference for ps_2_x, ps_3_0 and vs_3_0

`web/tools/psref.c` (interface `web/tools/psref.h`) runs Direct3D 9 shader bytecode of the three models Most Wanted
adds to UG2's: ps_2_x (its 18 world, car and glossy-window pixel shaders), ps_3_0 and vs_3_0 (the glossy window's
SM3 technique). It is the oracle for the WGSL translation of those models, as `ps_ref_run`
(`runtime/web/vsemu_psref.inc`) is for ps_1_x and ps_2_0. Native test tools only: it is not in `runtime/web/`,
whose every `.c` file goes into the game's module.

Why a second interpreter: `ps_ref_run` follows the Mac port's semantics on purpose (gpu-abi.md §8), with its
shortcuts: temporaries `r & 15`, texture coordinates `t & 3`, colours `v & 1`, no projective divide in `texldp`,
unknown instructions skipped. The WGSL translator of the older models has the same shortcuts, so a differential test
between the two cannot see them (ps_2_x, ps_3_0 and vs_3_0 now have a translator of their own, below). Ten of Most Wanted's 18 ps_2_x shaders use r16..r30, and with `r & 15` seven of them give other
results (measured below). psref decodes the bytecode itself, shares no code with `vsemu.c`, and follows the D3D9
documents; `ps_ref_run` now refuses ps_2_x and ps_3_0 (returns 0) so it can no longer act as their oracle.

## Interface

- `psref_load(tokens, dwords, caps, err)`: decodes and validates; `NULL` with the reason when the program breaks a
  rule of its model. `caps` is what the device reports (`PsrefCaps`: D3DPSHADERCAPS2_0 for ps_2_x,
  MaxVertexShaderConst, the executed-instruction limit, the render target count); `psref_caps_sm3` fills in an SM3
  device (every ps_2_x cap at its maximum, 256 vertex constants, 65,535 instructions, 4 targets).
- `psref_ps_run(prog, consts, in[4], tex, user, out[4], err)`: one 2x2 quad. Lane 0 is pixel (x, y), 1 is (x+1, y),
  2 is (x, y+1), 3 is (x+1, y+1). Inputs per lane: `v0..v9`, `t0..t7`, vPos, vFace. Outputs: `oC0..oC3` with the
  components written, oDepth, the texkill flag.
- `psref_vs_run(prog, consts, v[16], tex, user, o[12], omask[12], err)`: one vertex.
- `PsrefConsts`: the application's float, integer and boolean constants. A program's own `def`, `defi`, `defb` take
  precedence in its constant space (the constant-register pages), relative reads included.
- `PsrefTex`: each lookup as a `PsrefSample`: stage, sampler type from the `dcl`, mode (implicit, bias, level,
  gradients), the coordinate after the instruction's divide and which of its components the shader defined, the bias
  or level (`src0.w`), and the derivatives: across the quad for implicit and biased lookups, `src2`/`src3` for
  `texldd`. Sampler states (filters, `MIPMAPLODBIAS`, `MAXMIPLEVEL`), formats and their default components are the
  callback's business; so is a depth texture's compare.
- `psref_dcl`: the declarations (usage and index of each `v#`/`o#`, sampler types, vPos, vFace) for linking.

Not in it: what the pipeline does around a shader (interpolation, the vertex-to-pixel linkage, the alpha test, the
fixed-function fog that still follows a ps_2_x shader).

## The rules it checks when loading

Sources: the Direct3D 9 assembly reference (learn.microsoft.com, "ps_2_x Registers", "ps_3_0 Registers",
"Registers - vs_3_0", "Flow Control Limitations", "Flow Control Nesting Limits", the instruction pages, "Pixel Shader
Source Register Modifiers").

| | ps_2_x | ps_3_0 | vs_3_0 |
|---|---|---|---|
| r# | NumTemps (12..32) | 32 | 32 |
| inputs | v0, v1 (colours), t0..t7 | v0..v9 | v0..v15 |
| c# | 32 | 224 | MaxVertexShaderConst (>= 256) |
| i#, b# | 16 each (flow control only) | 16 each | 16 each |
| s# | 16 | 16 | 4 (`texldl` only) |
| outputs | oC0..oC3, oDepth | oC0..oC3, oDepth | o0..o11 |
| other | p0 with the predication cap | p0, aL, vPos, vFace | p0, a0, aL |
| relative addressing | none | `v[aL + n]` | `v#`, `c#` by a0 or aL; `o[aL + n]` |
| labels | l0..l15 | l0..l2047 | l0..l2047 |
| loop/rep, call nesting | StaticFlowControlDepth | 4, 4 | 4, 4 |
| dynamic, static if nesting | DynamicFlowControlDepth; 24 with static flow | 24, 24 | 24, 24 |

- Instructions by model: ps_2_x has ps_2_0's set plus, by its caps, static flow (`if b#`, `rep`, `call`,
  `callnz b#`, `label`, `ret`, `break`), dynamic flow (`if_comp`, `break_comp`), predication (`setp`, predicated
  instructions, `if p0`, `breakp`, `callnz p0`) and gradients (`dsx`, `dsy`, `texldd`). ps_3_0 adds `loop`, `texldl`,
  vPos and vFace. vs_3_0 has the vertex set (`slt`, `sge`, `sgn`, `lit`, `dst`, `mova`, `expp`, `logp`) and `texldl`;
  no `texld`, `texkill`, `cmp`, `dp2add` or gradients. ps_1_x's instructions (`cnd`, `bem`, `tex*` address ops) are
  in none of the three.
- Modifiers: negate everywhere; abs and negated abs in ps_3_0 and vs_3_0 only; NOT on `b#`/`p0` conditions. No
  ps_1_x source modifier, no result shift; `_sat` and `_pp` (computed at full precision, which `_pp` allows).
- ps_2_x without `ARBITRARYSWIZZLE`: ps_2_0's swizzles only (identity, a replicate, `.yzxw`, `.zxyw`, `.wzyx`).
  ps_2_x `texld`/`texldp`/`texldb` write `r#.xyzw` from `t#` or `r#` without swizzle or modifier, and take no sampler
  swizzle (ps_3_0 and vs_3_0 do: it applies to the result).
- Scalar instructions take a replicate swizzle (`rcp`, `rsq`, `exp`, `log`, `pow`, `expp`, `logp`, `sincos`, the
  comparisons of `if_comp`/`break_comp`); `sincos` writes `r#.x`, `.y` or `.xy` (ps_2_x with its two constants);
  `nrm` and `crs` write a temporary other than their sources; the `m*` instructions write their fixed masks and take
  no modifier on the matrix.
- Declarations: every `v#`, `t#`, `s#`, `o#`, vPos and vFace used is declared; a read stays inside the declared
  components (ps_3_0 inputs, `t#`, vPos: no defaults) and a write to `o#` inside its declaration. Packing several
  semantics into one register is allowed with disjoint masks (ps_3_0 inputs, vs_3_0 outputs). vPos takes `.x`, `.y`
  or `.xy`; vFace is only a condition (`setp`, `if_comp`, `break_comp`).
- Structure: blocks nest without straddling; `break` inside a loop; aL inside a `loop` block; calls reach defined
  labels without recursion; nesting depths are added up through the call graph.
- Gradients: `dsx`/`dsy` of a temporary, and `texld`/`texldp`/`texldb` of a temporary coordinate, are refused inside
  flow control that may differ per pixel: a dynamic `if`, a loop that a per-pixel break may leave early, or under a
  predicate ("Interaction of Per-Pixel Flow Control With Screen Gradients"). Of an input they are allowed anywhere,
  and so are `texldl` and `texldd`.

Not checked: instruction slot counts, read ports per instruction, the dependent-read limit without
`NODEPENDENTREADLIMIT`, ps_3_0's "every c# with abs or none", the values of ps_2_x `sincos`'s constants.

## Running

- Each instruction runs on the active lanes of the quad (SIMT): `if`/`else`/`endif`, loops whose lanes break at
  different times and `callnz` keep a mask, and the lanes meet again after each block. dsx/dsy and implicit
  lookups read all four lanes at the same instruction.
- Arithmetic is IEEE single precision, written as the instruction pages' pseudo-code: products and sums one rounding
  each (no fused `mad`), dot products summed left to right, `min` as `a < b ? a : b` and `max` as `a >= b ? a : b`
  (a NaN takes the second source), `cmp` as `a >= 0` (-0 counts), comparisons as C's (`ne` is true for a NaN),
  `frc` as `x - floor(x)`. `exp`, `log`, `pow`, `sincos`, `rsq` and `nrm`'s root are computed in double and rounded
  once.
- Special values: `rcp 0` and `rsq 0` give FLT_MAX, `log 0` -FLT_MAX, `nrm` of a zero vector FLT_MAX times each
  component (the pseudo-code of the vs pages and of `nrm`, `log`; the ps pages' text says "infinity" for
  `rcp`/`rsq`, and FLT_MAX is the only value any page writes as code). `rcp 1` and `rsq 1` are exactly 1. `pow` uses
  |src0|; `lit` clamps its power to ±127.9961.
- `texldp` divides x, y and z by w and passes w on; `texldb` passes `src0.w` as the bias (ps_2_x [-3, 3], ps_3_0
  [-16, 15]); `texldl` passes `src0.w` as the level; `texldd` passes its gradients.
- `texkill` kills when a component in its write mask is below zero; the pixel runs on (its derivatives stay valid).
- `mova` rounds to nearest, halves away from zero (the page says "round to nearest"; ties are not specified).
- `loop aL, i#`: count `i.x` in [0, 255], start `i.y` in [0, 255], step `i.z` in [-128, 127], `i.w` 0; aL is the
  innermost `loop`'s and its outer one's again after it. `rep i#`: count `i.x` in [0, 255].
- Relative constant reads outside the constant file give (0, 0, 0, 0) (the vs constant-register page).
- ps_2_x colour inputs take (0, 0, 0, 1) outside their declaration; a vertex shader's inputs come in whole.
- Written as undefined after the instruction (so a later read is an error): ps_2_x `sincos`'s `.z` and the unwritten
  one of `.x`/`.y`; `sgn`'s two scratch registers.

What D3D9 leaves undefined is an error with the instruction, lane and register named, never a made-up value: a
temporary, p0 or a0 read before it is written (per component, per lane), a loop count, start or step outside its
range, a `texldb` bias outside its range, `sincos` outside [-pi, pi], a gradient in divergent flow (checked again
while running), a relative input or output outside its file or undeclared, more instructions executed than the caps
allow.

Choices where the documents leave room, each named above: fine derivatives (dsx is the neighbour in the same row
minus this pixel's pair, dsy in the same column; D3D9 leaves the formula to the hardware, so a translation that
wants the same values uses fine derivatives too), `texkill`'s write mask (the pages describe ps_1_x's xyz), `mova`'s
halves, `_sat` leaving a NaN as NaN, a0 and aL readable as values (aL in every component), and `ret` only at the
end of a function (the reference refuses a `ret` inside a block).

## Tests

`node --test web/test/psref.test.mjs` builds `web/test/psref_unit.c` with ASan and UBSan:

- unit tests, each program assembled token by token with its result worked out by hand, per class: arithmetic,
  modifiers and special values; dynamic flow control (divergent `if`/`else`, `break_comp` and `break` in a dynamic
  `if` with per-lane round counts, `call`, `callnz p0`, `if b#`); loops with aL (`v[aL]`, nested loops, `c[aL]`,
  `o[aL]`, a0, ranges, the executed-instruction limit); predicates (per-component masks with swizzle and NOT, `if
  p0`, `breakp`); dsx/dsy (fine, of inputs and temporaries, the gradient rule); `texld`, `texldp`, `texldb`,
  `texldl`, `texldd`, sampler swizzles, vertex texture fetch; vFace and vPos; the register limits of each model at
  both edges and each model's features; undefined reads, defaults, `texkill`, outputs;
- with Most Wanted's game folder: every ps_2_x, ps_3_0 and vs_3_0 blob of the 31 effects (22: 18, 2 and 2) loads
  with an SM3 device's caps and the 18 ps_2_x also with ps_2_b's (no flow control, no arbitrary swizzle, 32
  temporaries), then runs on 16 quads or 64 vertices of made-up input with an analytic texture: every output is
  finite, oC0 is written whole, each vertex shader writes exactly its declared outputs, and a blob found twice gives
  the same result.

Checked once while writing it (2026-10-28), not part of the test: each of 40 single changes to psref.c's rules
(coarse derivatives, no divide of z, NOT ignored, `rcp 0` = inf, a fixed loop step, ...) makes the unit tests fail;
and on Most Wanted's 18 ps_2_x and 2 vs_3_0 blobs, `ps_ref_run` and `vs_run` with only their temporaries widened to
r0..r31, given inputs on which their semantics and D3D9's coincide (t4..t7 = t0..t3, w = 1, no alpha test or fog),
agree with psref within 4.2e-6 relative on all 16,128 values (most bit for bit), while with `r & 15` as shipped 2,229
values of 7 ps_2_x shaders differ.

## The WGSL translator of these models

`runtime/web/vsemu_sm3.inc` (included by `vsemu_wgsl.inc`) translates ps_2_x, ps_3_0 and vs_3_0 with these
semantics; `vs_compile` keeps such a program's tokens (`VSProg.raw`) and `ps_to_wgsl` / `vs_to_wgsl` hand them over.
The older models' path keeps the Mac's semantics and, with 4 stages, is unchanged: UG2 has none of these three models,
and its 45 distinct shaders in SPEED2.EXE (18 vs_1_1, 22 ps_1_1, 3 ps_1_4, 2 ps_2_0) translate to the same bytes as
before, with the same `.pvec` vectors. With 8 stages its modules (Most Wanted's other 75 shaders: 29 vs_1_1, 28
ps_1_1, 3 ps_1_4, 15 ps_2_0) are checked against `vs_run` and `ps_ref_run`, which follow the 8-stage rules too
(gpu-abi.md §9), not against psref. One of those rules is D3D9's and not the Mac's: with 8 stages a ps_2_0 `texldp`
divides x, y and z by w (both sides; with 4 stages it reads xy as it always did).

**Depth stages** (gpu-abi.md §11.4): `wgsl_set_depth_stages(mask)` before a pixel translation makes each 2D stage in
the mask a `texture_depth_2d` with a `sampler_comparison` (binding kind 2), and its lookups (`texld`, `texldp`, ps_1_x
`tex`) `vec4f(textureSampleCompareLevel(texK, smpK, uv, ref))` with uv and ref after the divide the instruction asks
for. The game's draws ask for such a variant when a depth texture is bound (gpu_web.c `ps_depth`). With 8 stages
`web/tools/wgsl-xl` also writes each pixel shader's variant with every 2D stage a depth stage, as
`ps_<id | 0xFF << 9>.wgsl`, so `shader-check.mjs --sm3` compiles them and builds their pipelines (`psDepth`). No
oracle compares their lookups yet: psref has no depth compare.

- **Registers.** r0..r31; ps_2_x t0..t7 and v0/v1 (a colour outside its declaration reads (0, 0, 0, 1)); ps_3_0
  v0..v9 and vs_3_0 o0..o11 by their declarations, packed ones included (texcoord N = `uvN`, color0/1 = `col0/col1`,
  fog); vPos (the pixel's corner) and vFace. Constants: the program's own `def`/`defi`/`defb`, then `P.c` (32 rows)
  or the draw's K block (256 rows; a relative read outside them is 0).
- **8 stages.** t4..t7, s4..s7 and the outputs texcoord4..7 need the 8-stage modules (gpu-abi.md §11.2,
  `wgsl_set_stages(8)`); every Most Wanted effect needs them. With 4 stages 17 of its 95 distinct shaders are refused,
  all for a texture coordinate 4..7, under three reasons: 14 ps_2_x read t4..t7 ("texture coordinate 4..7 without 8
  stages"), the 2 ps_3_0 declare such an input and the vs_3_0 such an output ("a ps_3_0 input" / "a vs_3_0 output
  the module ABI has no place for").
  `wgsl_ps_kinds()` gives the last pixel module's binding kinds (§11.3), also in its first line (`, kinds 0x..`).
- **Flow control** as WGSL blocks: `if` b#/p0/comparison, `rep`/`loop` with literal counts, the breaks, `call` and
  `callnz` inlined, predicated writes per component. WGSL takes implicit derivatives only in uniform flow; inside
  flow that may differ per pixel, a lookup with an implicit level of detail of an input or a constant is made at the
  top of the function and the block uses its result, and dsx/dsy of one are taken there. Derivatives taken at the top
  feeding a `textureSampleGrad` in the block were not enough: on an AMD RDNA 4 GPU in Chrome the gradients of a cube
  `texldb` inside an `if_comp` came out as if from pixels that do not run the block (a level about 6 too high), while
  the lookup made at the top matched.
- **Special values** as psref's: `rcp`/`rsq` of 0 FLT_MAX and of 1 exactly 1 (a `select` each: WGSL allows its
  division 2.5 ulp and `inverseSqrt` 2), `log` of 0 -FLT_MAX, `nrm` of a zero vector FLT_MAX times it, `cmp` at -0
  as at 0. A corpus program makes 0, -0 and 1 at run time from an input (no compiler can fold them) and checks these
  and `pow` of 0 and of a negative base; its exact-1 result is the difference from 1 times 2^20, so one ulp shows.
  Left to the GPU, as WGSL leaves them open and no test input reaches them: `min`/`max` with a NaN (psref: the
  second source, the pages' `a < b ? a : b`) and `pow(0, y)` with y <= 0 (psref: 1 for y = 0, +inf below, as the
  pow page's expansion `exp(src1 * log(src0))` with `log 0` = -FLT_MAX gives).
- **Refused** (a reason from `wgsl_refusal()`; the game then has no module for it): what the module ABI has no place
  for (MRT, a vertex texture fetch, a ps_3_0 input or vs_3_0 output of another usage, an `i#`/`b#` the application
  sets, a constant past c31 in a pixel shader), a volume sampler, an implicit-LOD lookup or dsx/dsy of a temporary in
  per-pixel flow (D3D9 forbids it), and bytecode it cannot decode. None of Most Wanted's shaders is refused.

The differential test: `web/tools/wgsl-xl.c` writes for each translated SM3-class module random inputs (64 quads of
a 16-pixel-wide target, or 64 vertices) and psref's results (`ps_<id>.p3vec`, `vs_<id>.v3vec`), with an analytic
texture of the coordinate and level of detail; `web/src/test/shadercheck.ts` runs the module on the GPU with the same
texture function and compares (1e-3 relative for pixels, 1e-4 for vertices, the kill flag exactly). A result that
psref itself moves past the tolerance when every input moves by 1e-5 (a comparison at its threshold) is counted as
fragile, not as a failure. A quad psref refuses (an undefined read, say) is left out; a module psref refuses on all
64 gets no vectors, so it does not count as checked.

```
node web/tools/shader-check.mjs --sm3      # the unit tests' programs and, with NFSMW_DATA, Most Wanted's 31 effects
node --test web/test/psref.test.mjs        # without a GPU: both translate (with ASan, UBSan), refusals as allowed
```

Measured 2026-10-28, 8 stages: Most Wanted's 95 distinct shaders all translate, the 20 distinct SM3-class ones (17
ps_2_x, 2 ps_3_0, 1 vs_3_0) with vectors, and the unit tests' 65 programs give 47 modules (18 refused as above); 0
mismatches on Chrome's SwiftShader (each shader's own seed and three more) and on an AMD RDNA 4 GPU (its own seed
and two more).

Measured 2026-10-29 on Chrome's SwiftShader only (each shader's own seed), 8 stages: the unit tests' 67 programs
give 49 modules, 71 pixel modules (cube twins included) and 4 vertex modules compared with psref; Most Wanted's 19
pixel and 1 vertex SM3-class modules with psref, its 92 older-model pixel modules (46 shaders and their twins) with
`ps_ref_run` and its 29 vs_1_1 modules with `vs_run`: 0 mismatches. Three translators changed on purpose fail it:
`rcp 1` one ulp low (512 pixels, all in the special-value program and its twin), and for the older models oT5 written
as oT4 or ps_2_0's t4..t7 read as t0..t3.
