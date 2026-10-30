# The ID3DXEffect HLE (M5-05, M5-06) and what d3dx9_26 does on the device

`runtime/mw/d3dxfx_mw.c` answers Most Wanted's two effect imports, `D3DXCreateEffectPool` and
`D3DXCreateEffectFromResourceA`, and the `ID3DXEffect` and `ID3DXEffectPool` objects they make, so speed.exe runs
without `d3dx9_26.dll` (a dev oracle that never ships, GAME.md 5.1). It stands on the effect model (`fxmodel.c`,
fx-model.md) and the preshader evaluator (`fxpres.c`, fx-preshader.md); this file is what it adds: what D3DX does
with an effect on the device. The facts tagged **X** were read from the DLL's code (addresses are d3dx9_26's at its
preferred base 0x400000), **O** seen in the oracle's device-call recordings (`NFSU2_DXREC`, docs/web/dxrec.md),
**V** checked by a run.

## 1. Objects and binding

- The two imports are bound as the math is (d3dxmath_mw.c): each IAT slot (0x8904C0, 0x8904C4) to the exe's own
  `jmp [slot]` thunk (0x7E7D76, 0x7E7D7C), the thunk's dispatch entry to the answer. A build without the oracle
  always binds them; an oracle build only with `NFSMW_D3DX_FX=hle`, and the DLL is then not loaded (all 13 imports
  answered). **V**
- The objects are com.c's (`com_new_d3dx`): their vtables call `g_game.d3dx_call`, their last Release
  `g_game.d3dx_free`; AddRef and Release are com.c's own counts. An effect object holds its native index at +4 and
  the handle block after it: node id's handle is `obj + 8 + 4 id` (fx-model 2.8). A handle outside the block is a
  name.
- Every device call goes through the device's vtable (`call_guest`), as the DLL's do, so the recorder sees both alike.

## 2. Creation (O, X)

`D3DXCreateEffectFromResourceA(device, NULL, name, ...)`: the exe's RT_RCDATA resource, parsed in place (fx2.c),
its model with the pool, its preshaders. On the device: `AddRef` of the device; one `CreatePixelShader` or
`CreateVertexShader` per shader program in resource order (the code from the resource, no copy; X 0x4F2506,
0x4F2569); then, after them all, one device `Release` per shader (the oracle asks each shader's device, which the
recorder does not write, and lets it go). **O**: the same calls in the same order.

## 3. A technique's saved state (Begin, End; X 0x505F9B, 0x4FD731, 0x5058EF, 0x505A65, 0x502D46, 0x502E7C, 0x502FB2,
0x5030E8, 0x500518, 0x500642, 0x5006EC, 0x500816)

Begin's flags become three bits: save (`!(flags & 1)`), save the shader part (`!(flags & 3)`), save the sampler
part (`!(flags & 5)`). Each technique keeps five state blocks and what they cover:

| Block | Recorded with | Covers |
|---|---|---|
| vertex shader (+0xA4) | `SetVertexShader(NULL)` when the technique sets one, zero `SetVertexShaderConstantF/B/I` from 0 | F, B, I registers |
| vertex samplers (+0x9C) | per sampler 257 + i: `SetTexture(NULL)`, sampler states 1..13 | count |
| pixel shader (+0xA8) | as the vertex one, with the pixel calls | F, B, I |
| pixel samplers (+0xA0) | per sampler i: `SetTexture(NULL)`, sampler states 1..12 | count |
| main (+0x98) | every pass run once in the record mode (below) | |

The sampler states recorded are D3DX's table at 0x415ED0: types 1..13 with the values 3,3,3,0,1,1,1,0,0,1,0,0,0. At
creation a technique's blocks are absent, its shader flags say whether its passes set a vertex or pixel shader, and
its sampler counts are the highest sampler index its passes' own sampler states name, plus one; the constant counts
are 0.

**Growing** (0x5058EF vertex, 0x505A65 pixel), with a shader's needs (the highest register plus one its constant table
names per set, and its samplers): when saving, the shader block is made again when it is missing, when the shader is
set but the block does not set one, or when a count is short; the sampler block when missing or short. Making one
(0x502D46 and kin) records two blocks (`BeginStateBlock`, the calls, `EndStateBlock`), then: Capture the first, Apply
the old block, Capture the second, Apply the first, Release the old, keep the second, Release the first. A failed
`BeginStateBlock` (nested, in the record mode) ends it there.

**Begin**: with saving, grow both sides to what they cover (so missing blocks are made), Capture the vertex and pixel
shader blocks (shader part), the vertex and pixel sampler blocks (sampler part); the first time, record the main
block (`BeginStateBlock`, the record mode, `BeginPass(p)`/`EndPass` for each pass, `EndStateBlock`); Capture it.
**End**: Apply the main block, the pixel and vertex sampler blocks (sampler part), the pixel and vertex shader blocks
(shader part). **OnLostDevice** (0x4F9DA3): Release ValidateTechnique's block, then per technique the main, vertex
shader, vertex sampler, pixel shader and pixel sampler blocks; the counts stay. **O**: the same Begin and End calls,
block for block, in the menus.

## 4. A pass (BeginPass, CommitChanges, EndPass; X 0x507DE9, 0x507C7C, 0x4FD8AA, 0x506C33, 0x506D2D)

- **BeginPass**: every state of the pass in file order. A shader state grows the saved state to the shader's needs
  first (§3), then sets the shader, then uploads the program's float registers from its lowest to its highest in
  one call (its registers as last uploaded: the parameters' values in the CTAB layout, fx-model 3.3; a register
  no constant fills stays 0), then the states of the sampler blocks its sampler constants bind, by stage. A light's
  members are set as one `SetLight` after the pass's last state of that light, the material as one `SetMaterial`
  after the pass's last material state. **O** for the menus' passes.
- **CommitChanges**: outside a pass nothing. In one: the parameters written since BeginPass, in the order they were
  written (a write through any effect of the pool reaches every effect in a pass that holds the parameter,
  0x5008B5), each once; for each, the active pass's states that read it, the pass's last state first, each state
  once; a shader constant's registers as one upload of that parameter's. **O**: the menus' commits (textures by
  stage, a pixel constant, BlendState's five render states last to first).
- **EndPass** empties the list.

**The record mode** (Begin's main block): the sampler states and the shaders are left to their own blocks: no
sampler state, no `Set*Shader`, no constants; a shader state still tries to grow (its `BeginStateBlock` fails,
nested) and the textures of its sampler blocks are recorded.

## 5. ValidateTechnique (X 0x5098F8, 0x509BCB)

On the effect's first call, a declaration of a FLOAT3 position and eight FLOAT4 texture coordinates (0x416458) and a
block recording it with `SetVertexShader(NULL)` and `SetPixelShader(NULL)`; then each call: Capture that block, set the
three, Begin the technique with its state saved, and in the validation mode each pass: BeginPass, `ValidateDevice`,
EndPass; End; Apply the block. The current technique is the game's again afterwards. In the validation mode a
vertex shader state first makes, sets and releases a declaration of its inputs (D3DXGetShaderInputSemantics: each
`dcl` of an input register, in code order, as FLOAT3 elements one after another), and a texture state asks
`GetDeviceCaps` first. **O**: the validation calls, decl for decl, for every technique the game validates.

## 6. What still differs from the oracle (menu lockstep, docs/web/dxrec.md)

Measured with `boot_menu.txt` in lockstep (`NFSU2_HFR_LOCKSTEP=1 VCLOCK=fixed`), two oracle runs against each other
(the noise floor) and the HLE against an oracle run, `web/tools/dxrec-diff.mjs`:

- **Values recorded in the record mode**: the oracle records render states its preshaders compute (SRCBLEND,
  DESTBLEND, CULLMODE, ...) and FVF with values that are not the parameters' (D3D9-default-looking: 2, 1, 3; FVF 2),
  where the HLE records the evaluated value or the literal. Capture replaces them before any Apply in com.c, so no
  draw sees them; the source of D3DX's value (a stale register table, a cache in the state record, 0x5061DC's
  `test al, 4` path) is not pinned down.
- **A sampler block sized for one sampler more** than the HLE's count in a few techniques: D3DX's sampler need is
  not only the constant table's (to find: the shader's `dcl` samplers, or the technique's other passes).
- **The first light of a fixed-function technique**: the oracle sets it from memory it never wrote (its technique's
  light copy before the Begin that zeroes it); the HLE's is zero. Only fixed-function techniques use it, and no draw
  with LIGHTING on sees it in the menus.
- Everything else in the menu frames is the scripted run's own noise (loading timing: buffer and texture creation
  in other frames, a loading bar's vertices), which two oracle runs show as well.
