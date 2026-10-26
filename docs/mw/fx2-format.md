# fx_2_0 effect binaries (D3DX 9) and their canonical dump

Status: **2026-10-26**, spec for M5-01. Consumers: the M5-01 dumpers, M5-02 (CTAB matching, parameter and annotation
model, pool), M5-03 (preshader evaluator), M5-04 (state table), M5-05/06 (the ID3DXEffect HLE).

This is the compiled effect format that `D3DXCreateEffect*` reads (tag `0xFEFF0901`, written by fxc for the `fx_2_0`
target), as D3DX 9 (`d3dx9_26.dll`, 9.07.239.0000) reads it, and a plain-text dump of it that two independent
programs can produce byte for byte. Format only: offsets are relative structure offsets, never addresses of the game
or of the DLL, and nothing here is game data. The effects themselves come from `tools/fxres.py` (§6) and stay under
`out/mw/fx/`.

**Marks.** Every field carries its source:

| Mark | Source |
|---|---|
| **V** | Verified on Most Wanted's 31 effects (`out/mw/fx/res/*.fxo`); the check is named (C1..C9, §7) |
| **D** | Read from `d3dx9_26.dll`'s data as bytes (no code of it was run or copied) |
| **W** | Wine's d3dx9 (`dlls/d3dx9_36/effect.c`, `preshader.c`, LGPL): facts only, no code |
| **K** | veritr1x recomp-kit `dx/d3dx9.cpp` (MIT) |
| **U** | UG2's port: its earlier effect dumper `tools/efxdump.py` and D3DX notes `D3DX_HLE.md` (neither in this repository) |
| **S** | Microsoft's SDK and DDK documentation and headers (`d3d9types.h`, `d3dx9shader.h`, `d3dx9effect.h`, "Direct3D shader codes") |

A field without **V** does not occur in Most Wanted's files, or occurs in a form that cannot test it. The dump rules in
§5 are the contract.

## 1. Conventions

- Every number is a little-endian 32-bit word ("dword") unless a field says otherwise; every structure is a whole
  number of dwords. **V** (C1)
- The **body** starts at file offset 8. Every offset stored in the file is a body offset ("body +x" below). **V** (C1),
  W, K, U
- Data is padded to a multiple of 4 bytes. **Pad bytes are not zero** and carry no meaning: readers must not check
  them, dumps never print them. **V** (nonzero pad bytes after names and after resource data in most files)
- A name offset of 0 means "no name". The dword at body +0 is never referenced by any offset. **V** (C1; it is 0 in all
  31 files)

## 2. The container

### 2.1 Header and layout

| File offset | Field | Src |
|---|---|---|
| +0 | Tag `0xFEFF0901` (bytes `01 09 FF FE`): `0xFEFF0000 \| major << 8 \| minor`, effect binary 9.1. D3DX compiles anything else as effect source text | **V** (31/31), W |
| +4 | `tables`: body offset of the table area (§2.5) | **V**, W, K, U |
| +8 | The body: the **data area** from body +0 up to `tables`, then the **table area** from `tables` to the end of the file | **V** (C1) |

The data area holds typedefs (§2.3), names (§2.2) and values (§2.4), reached only through offsets stored in the table
area and in typedefs. **V** (C1): no typedef, name or value is referenced twice; every offset is a multiple of 4 and
below `tables`; no two items overlap; the area is fully used except body +0 and pad bytes.

### 2.2 Name

| Off | Field | Src |
|---|---|---|
| +0 | Size S in bytes, **including** the terminating NUL | **V** (every name ends in NUL, none has a NUL inside), W |
| +4 | S bytes, then pad to 4 | **V** |

### 2.3 Typedef

| Off | Field | Src |
|---|---|---|
| +0 | Type (D3DXPARAMETER_TYPE, table below) | **V**, W, S |
| +4 | Class (D3DXPARAMETER_CLASS, table below) | **V**, W, S |
| +8 | Name offset (0 = none) | **V**, W |
| +12 | Semantic offset (0 = none) | **V**, W |
| +16 | Element count; 0 = not an array. One typedef describes the element and the count: D3DX reads the same typedef again for every element | **V** (C3), W |
| +20 | Class VECTOR: **columns**, then rows (+24) | **V** (C3: the second dword is 1 in all 141 vectors) |
| +20 | Class SCALAR, MATRIX_ROWS, MATRIX_COLUMNS: **rows**, then columns (+24) | W, U; **V** only for scalars and square matrices (no other shapes in MW) |
| +20 | Class STRUCT: member count M, then M member typedefs back to back, each a full typedef (nesting) | W, U |
| | Class OBJECT: nothing more | **V** |

Types (S, W): 0 VOID, 1 BOOL, 2 INT, 3 FLOAT, 4 STRING, 5 TEXTURE, 6 TEXTURE1D, 7 TEXTURE2D, 8 TEXTURE3D, 9 TEXTURECUBE,
10 SAMPLER, 11 SAMPLER1D, 12 SAMPLER2D, 13 SAMPLER3D, 14 SAMPLERCUBE, 15 PIXELSHADER, 16 VERTEXSHADER, 17 PIXELFRAGMENT,
18 VERTEXFRAGMENT, 19 UNSUPPORTED. Classes (S, W): 0 SCALAR, 1 VECTOR, 2 MATRIX_ROWS, 3 MATRIX_COLUMNS, 4 OBJECT,
5 STRUCT.

Seen in MW (**V**): INT, FLOAT, TEXTURE, SAMPLER, SAMPLERCUBE, PIXELSHADER, VERTEXSHADER; classes SCALAR, VECTOR,
MATRIX_ROWS, OBJECT. No STRING, no STRUCT, no annotation or parameter of class MATRIX_COLUMNS. State typedefs (§3.4)
use class MATRIX_ROWS for every numeric value.

### 2.4 Value

A value is stored element after element (one element when the typedef has no element count):

| Class / type | One element | Src |
|---|---|---|
| SCALAR, VECTOR, MATRIX_ROWS, MATRIX_COLUMNS | rows × columns dwords, in file order. FLOAT: IEEE single bits; INT: two's complement; BOOL: a dword | **V** (C1, including the arrays INT[5], float4[4] and float4x4[16]), W |
| OBJECT of type STRING, TEXTURE*, PIXELSHADER, VERTEXSHADER | one dword: an **object id** (§2.7) | **V** for TEXTURE, PIXELSHADER, VERTEXSHADER; W for the others |
| OBJECT of type SAMPLER* | a state count N, then N state entries (§2.5.5, 16 bytes each). This is the `sampler_state { … }` block | **V**, W |
| STRUCT | each member's value in member order | W, for members of numeric classes; see §8 for structs that mix numbers and objects |

### 2.5 The table area

From `tables`, in this order (**V**, C1: the walk ends exactly at the end of the file; W, K, U):

| Field | Src |
|---|---|
| Parameter count P | **V** |
| Technique count T | **V** |
| "Shader count": Wine reads it as `shader_count` ("roughly the shader variables plus one slot per pass") and does not use it; neither do K and U. Its meaning is not pinned down (§8); the dump prints it | **V** (present), W |
| Object count O | **V** (C2: O = 1 + the number of object ids that values carry) |
| P parameter entries (§2.5.1) | **V** |
| T technique entries (§2.5.3) | **V** |
| Object-data count N | **V** |
| Resource count R | **V** |
| N object-data entries (§2.5.6) | **V** |
| R resource entries (§2.5.7) | **V** |

#### 2.5.1 Parameter entry

| Off | Field | Src |
|---|---|---|
| +0 | Typedef offset | **V**, W |
| +4 | Value offset (the default value) | **V**, W |
| +8 | Flags. Bit 0 = shared: one value for every effect created with the same `ID3DXEffectPool` (`D3DX_PARAMETER_SHARED` = 1) | **V** (values 0 and 1 only), W, K, S |
| +12 | Annotation count A | **V**, W |
| +16 | A annotation entries (§2.5.2) | **V** (A = 0 for every parameter in MW), W |

#### 2.5.2 Annotation entry (8 bytes)

| Off | Field | Src |
|---|---|---|
| +0 | Typedef offset (the typedef's name is the annotation's name) | **V**, W |
| +4 | Value offset | **V**, W |

In MW only techniques have annotations, all INT scalars. **V**

#### 2.5.3 Technique entry

| Off | Field | Src |
|---|---|---|
| +0 | Name offset | **V**, W |
| +4 | Annotation count A | **V**, W |
| +8 | Pass count Q | **V**, W |
| +12 | A annotation entries, then Q pass entries (§2.5.4) | **V**, W |

#### 2.5.4 Pass entry

| Off | Field | Src |
|---|---|---|
| +0 | Name offset | **V**, W |
| +4 | Annotation count A | **V** (0 in MW), W |
| +8 | State count S | **V**, W |
| +12 | A annotation entries, then S state entries (§2.5.5) | **V**, W |

#### 2.5.5 State entry (16 bytes): one state assignment

| Off | Field | Src |
|---|---|---|
| +0 | Operation: the row of the state table (§3.1). D3DX refuses an operation past its table | **V** (C7), W |
| +4 | Index: stage, sampler, light or matrix number (§3.3) | **V**, W |
| +8 | Typedef offset of the value | **V**, W |
| +12 | Value offset: the literal value, or the object id for object types | **V**, W |

#### 2.5.6 Object-data entry

| Off | Field | Src |
|---|---|---|
| +0 | Object id | **V** (C2), W |
| +4 | Size S | **V**, W |
| +8 | S bytes, then pad to 4: the object's initial data. D3DX makes a string of it for a STRING object and a shader for a shader object | W; in MW every entry belongs to a TEXTURE parameter and has S = 0 (**V**) |

#### 2.5.7 Resource entry: data attached to one state

| Off | Field | Src |
|---|---|---|
| +0 | Technique index; `0xFFFFFFFF` = the state is in a parameter's sampler block | **V**, W, K, U |
| +4 | Pass index within the technique, or the parameter index | **V**, W |
| +8 | Element index: `0xFFFFFFFF` = none; otherwise the element of an array sampler parameter. For a sampler that is not an array, D3DX takes the parameter's own block whatever this field holds | **V** (pass states: `0xFFFFFFFF`; sampler states: 0 on non-array samplers), W |
| +12 | State index: position in the pass's state list or in the sampler block's state list (not the operation) | **V** (C4), W |
| +16 | Usage (below) | **V** (0 and 1), W |
| +20 | Size S | **V** |
| +24 | S bytes, then pad to 4 | **V** |

| Usage | Target state | Data | Src |
|---|---|---|---|
| 0 | VERTEXSHADER or PIXELSHADER type | D3D9 shader bytecode (§4.1) for the object whose id is the state's value | **V** (C4, C6), W, K |
| 0 | BOOL, INT, FLOAT or STRING type | An FXLC preshader (§4.3) that computes the state's value from parameters; the literal value is only its default. D3DX copies the blob into the state's object slot; a numeric value holds no object id, so that is slot 0 | **V** (C4, C5), W, K |
| 1 | an object type | The NUL-terminated name of a parameter whose value the state takes (`Texture = <name>;`, also shaders) | **V** (on TEXTURE states only), W, K |
| 2 | any | Array selector: a size S2, S2 bytes of NUL-terminated parameter name (S2 should be a multiple of 4), then a preshader (the rest), evaluated as INT, that picks the element of the named array parameter (D3DX checks that a shader array holds shaders) | W only; not in MW |

D3DX accepts usage 0 only on the types above. **V** (C4): every MW resource has an existing target, no state has two,
and the data's first dword agrees with the target type (shader version token or `0x46580200`).

### 2.6 Where each blob lives

| Blob | Place | Src |
|---|---|---|
| Shader bytecode | Resource entry, usage 0, targeting a VERTEXSHADER or PIXELSHADER state | **V** |
| FXLC preshader | Resource entry, usage 0, targeting a numeric state in a pass or in a sampler block | **V** |
| CTAB | A comment inside each preshader (§4.3) and inside a shader (§4.2), where fxc wrote one (two MW ps_1_1 shaders have none) | **V** |
| Parameter reference | Resource entry, usage 1 | **V** |
| String data | Object-data entry | W |
| Preshader inside a shader | `PRES` comment inside the shader bytecode (§4.4) | W; not in MW |

### 2.7 Objects

Object ids run 0..O−1. A value of an object type holds an id, and the id's data comes from one object-data entry or
from one resource. **V** (C2): ids 1..O−1 are each held by exactly one value and filled exactly once (by an
object-data entry or a resource); id 0 is held by no value (it is the slot numeric states use, W). At run time
`SetTexture` writes a texture parameter's object, and states read it from there (U: `D3DX_HLE.md`).

## 3. State assignments

### 3.1 The state table

The operation of a state entry is a row of D3DX's state table. `d3dx9_26.dll` holds the table four times, identical,
as 32-byte rows: name, value type, a flag, rows, columns, index count, `class << 24 | D3D9 value`, and a pointer to
the value-name list (**D**). Its 182 rows are below. Wine's table has the same rows 0..178 in the same order (**W**);
recomp-kit's render-state part matches rows 0..102 (**K**); UG2 found the same ranges in its own D3DX (**U**: "0..0x66
render state (WRAP0–15 together), 0x67.. texture stage, 0x7a FVF, 0x7b–0x7e transform, 0x92/0x93 VS/PS, 0xa4 Texture,
0xa5.. sampler state"). The D3D9 values were checked against the SDK's enums: they agree except rows 89 and 102
(§3.5). The **Value** column is the table's own value type and size (a rows×columns size where it is not 1×1, n for a
variable size); **Index** is the range the table allows.

| Row | Hex | Name (dump) | Class | D3D9 target | Value | Index |
|---|---|---|---|---|---|---|
| 0 | 0x00 | ZENABLE | render state | D3DRS_ZENABLE = 7 | dword | 0 |
| 1 | 0x01 | FILLMODE | render state | D3DRS_FILLMODE = 8 | dword | 0 |
| 2 | 0x02 | SHADEMODE | render state | D3DRS_SHADEMODE = 9 | dword | 0 |
| 3 | 0x03 | ZWRITEENABLE | render state | D3DRS_ZWRITEENABLE = 14 | dword | 0 |
| 4 | 0x04 | ALPHATESTENABLE | render state | D3DRS_ALPHATESTENABLE = 15 | dword | 0 |
| 5 | 0x05 | LASTPIXEL | render state | D3DRS_LASTPIXEL = 16 | dword | 0 |
| 6 | 0x06 | SRCBLEND | render state | D3DRS_SRCBLEND = 19 | dword | 0 |
| 7 | 0x07 | DESTBLEND | render state | D3DRS_DESTBLEND = 20 | dword | 0 |
| 8 | 0x08 | CULLMODE | render state | D3DRS_CULLMODE = 22 | dword | 0 |
| 9 | 0x09 | ZFUNC | render state | D3DRS_ZFUNC = 23 | dword | 0 |
| 10 | 0x0a | ALPHAREF | render state | D3DRS_ALPHAREF = 24 | dword | 0 |
| 11 | 0x0b | ALPHAFUNC | render state | D3DRS_ALPHAFUNC = 25 | dword | 0 |
| 12 | 0x0c | DITHERENABLE | render state | D3DRS_DITHERENABLE = 26 | dword | 0 |
| 13 | 0x0d | ALPHABLENDENABLE | render state | D3DRS_ALPHABLENDENABLE = 27 | dword | 0 |
| 14 | 0x0e | FOGENABLE | render state | D3DRS_FOGENABLE = 28 | dword | 0 |
| 15 | 0x0f | SPECULARENABLE | render state | D3DRS_SPECULARENABLE = 29 | dword | 0 |
| 16 | 0x10 | FOGCOLOR | render state | D3DRS_FOGCOLOR = 34 | dword | 0 |
| 17 | 0x11 | FOGTABLEMODE | render state | D3DRS_FOGTABLEMODE = 35 | dword | 0 |
| 18 | 0x12 | FOGSTART | render state | D3DRS_FOGSTART = 36 | float | 0 |
| 19 | 0x13 | FOGEND | render state | D3DRS_FOGEND = 37 | float | 0 |
| 20 | 0x14 | FOGDENSITY | render state | D3DRS_FOGDENSITY = 38 | float | 0 |
| 21 | 0x15 | RANGEFOGENABLE | render state | D3DRS_RANGEFOGENABLE = 48 | dword | 0 |
| 22 | 0x16 | STENCILENABLE | render state | D3DRS_STENCILENABLE = 52 | dword | 0 |
| 23 | 0x17 | STENCILFAIL | render state | D3DRS_STENCILFAIL = 53 | dword | 0 |
| 24 | 0x18 | STENCILZFAIL | render state | D3DRS_STENCILZFAIL = 54 | dword | 0 |
| 25 | 0x19 | STENCILPASS | render state | D3DRS_STENCILPASS = 55 | dword | 0 |
| 26 | 0x1a | STENCILFUNC | render state | D3DRS_STENCILFUNC = 56 | dword | 0 |
| 27 | 0x1b | STENCILREF | render state | D3DRS_STENCILREF = 57 | dword | 0 |
| 28 | 0x1c | STENCILMASK | render state | D3DRS_STENCILMASK = 58 | dword | 0 |
| 29 | 0x1d | STENCILWRITEMASK | render state | D3DRS_STENCILWRITEMASK = 59 | dword | 0 |
| 30 | 0x1e | TEXTUREFACTOR | render state | D3DRS_TEXTUREFACTOR = 60 | dword | 0 |
| 31 | 0x1f | WRAP0 | render state | D3DRS_WRAP0 = 128 | dword | 0 |
| 32 | 0x20 | WRAP1 | render state | D3DRS_WRAP1 = 129 | dword | 0 |
| 33 | 0x21 | WRAP2 | render state | D3DRS_WRAP2 = 130 | dword | 0 |
| 34 | 0x22 | WRAP3 | render state | D3DRS_WRAP3 = 131 | dword | 0 |
| 35 | 0x23 | WRAP4 | render state | D3DRS_WRAP4 = 132 | dword | 0 |
| 36 | 0x24 | WRAP5 | render state | D3DRS_WRAP5 = 133 | dword | 0 |
| 37 | 0x25 | WRAP6 | render state | D3DRS_WRAP6 = 134 | dword | 0 |
| 38 | 0x26 | WRAP7 | render state | D3DRS_WRAP7 = 135 | dword | 0 |
| 39 | 0x27 | WRAP8 | render state | D3DRS_WRAP8 = 198 | dword | 0 |
| 40 | 0x28 | WRAP9 | render state | D3DRS_WRAP9 = 199 | dword | 0 |
| 41 | 0x29 | WRAP10 | render state | D3DRS_WRAP10 = 200 | dword | 0 |
| 42 | 0x2a | WRAP11 | render state | D3DRS_WRAP11 = 201 | dword | 0 |
| 43 | 0x2b | WRAP12 | render state | D3DRS_WRAP12 = 202 | dword | 0 |
| 44 | 0x2c | WRAP13 | render state | D3DRS_WRAP13 = 203 | dword | 0 |
| 45 | 0x2d | WRAP14 | render state | D3DRS_WRAP14 = 204 | dword | 0 |
| 46 | 0x2e | WRAP15 | render state | D3DRS_WRAP15 = 205 | dword | 0 |
| 47 | 0x2f | CLIPPING | render state | D3DRS_CLIPPING = 136 | dword | 0 |
| 48 | 0x30 | LIGHTING | render state | D3DRS_LIGHTING = 137 | dword | 0 |
| 49 | 0x31 | AMBIENT | render state | D3DRS_AMBIENT = 139 | dword | 0 |
| 50 | 0x32 | FOGVERTEXMODE | render state | D3DRS_FOGVERTEXMODE = 140 | dword | 0 |
| 51 | 0x33 | COLORVERTEX | render state | D3DRS_COLORVERTEX = 141 | dword | 0 |
| 52 | 0x34 | LOCALVIEWER | render state | D3DRS_LOCALVIEWER = 142 | dword | 0 |
| 53 | 0x35 | NORMALIZENORMALS | render state | D3DRS_NORMALIZENORMALS = 143 | dword | 0 |
| 54 | 0x36 | DIFFUSEMATERIALSOURCE | render state | D3DRS_DIFFUSEMATERIALSOURCE = 145 | dword | 0 |
| 55 | 0x37 | SPECULARMATERIALSOURCE | render state | D3DRS_SPECULARMATERIALSOURCE = 146 | dword | 0 |
| 56 | 0x38 | AMBIENTMATERIALSOURCE | render state | D3DRS_AMBIENTMATERIALSOURCE = 147 | dword | 0 |
| 57 | 0x39 | EMISSIVEMATERIALSOURCE | render state | D3DRS_EMISSIVEMATERIALSOURCE = 148 | dword | 0 |
| 58 | 0x3a | VERTEXBLEND | render state | D3DRS_VERTEXBLEND = 151 | dword | 0 |
| 59 | 0x3b | CLIPPLANEENABLE | render state | D3DRS_CLIPPLANEENABLE = 152 | dword | 0 |
| 60 | 0x3c | POINTSIZE | render state | D3DRS_POINTSIZE = 154 | float | 0 |
| 61 | 0x3d | POINTSIZE_MIN | render state | D3DRS_POINTSIZE_MIN = 155 | float | 0 |
| 62 | 0x3e | POINTSIZE_MAX | render state | D3DRS_POINTSIZE_MAX = 166 | float | 0 |
| 63 | 0x3f | POINTSPRITEENABLE | render state | D3DRS_POINTSPRITEENABLE = 156 | dword | 0 |
| 64 | 0x40 | POINTSCALEENABLE | render state | D3DRS_POINTSCALEENABLE = 157 | dword | 0 |
| 65 | 0x41 | POINTSCALE_A | render state | D3DRS_POINTSCALE_A = 158 | float | 0 |
| 66 | 0x42 | POINTSCALE_B | render state | D3DRS_POINTSCALE_B = 159 | float | 0 |
| 67 | 0x43 | POINTSCALE_C | render state | D3DRS_POINTSCALE_C = 160 | float | 0 |
| 68 | 0x44 | MULTISAMPLEANTIALIAS | render state | D3DRS_MULTISAMPLEANTIALIAS = 161 | dword | 0 |
| 69 | 0x45 | MULTISAMPLEMASK | render state | D3DRS_MULTISAMPLEMASK = 162 | dword | 0 |
| 70 | 0x46 | PATCHEDGESTYLE | render state | D3DRS_PATCHEDGESTYLE = 163 | dword | 0 |
| 71 | 0x47 | DEBUGMONITORTOKEN | render state | D3DRS_DEBUGMONITORTOKEN = 165 | dword | 0 |
| 72 | 0x48 | INDEXEDVERTEXBLENDENABLE | render state | D3DRS_INDEXEDVERTEXBLENDENABLE = 167 | dword | 0 |
| 73 | 0x49 | COLORWRITEENABLE | render state | D3DRS_COLORWRITEENABLE = 168 | dword | 0 |
| 74 | 0x4a | TWEENFACTOR | render state | D3DRS_TWEENFACTOR = 170 | float | 0 |
| 75 | 0x4b | BLENDOP | render state | D3DRS_BLENDOP = 171 | dword | 0 |
| 76 | 0x4c | POSITIONDEGREE | render state | D3DRS_POSITIONDEGREE = 172 | dword | 0 |
| 77 | 0x4d | NORMALDEGREE | render state | D3DRS_NORMALDEGREE = 173 | dword | 0 |
| 78 | 0x4e | SCISSORTESTENABLE | render state | D3DRS_SCISSORTESTENABLE = 174 | dword | 0 |
| 79 | 0x4f | SLOPESCALEDEPTHBIAS | render state | D3DRS_SLOPESCALEDEPTHBIAS = 175 | float | 0 |
| 80 | 0x50 | ANTIALIASEDLINEENABLE | render state | D3DRS_ANTIALIASEDLINEENABLE = 176 | dword | 0 |
| 81 | 0x51 | MINTESSELLATIONLEVEL | render state | D3DRS_MINTESSELLATIONLEVEL = 178 | float | 0 |
| 82 | 0x52 | MAXTESSELLATIONLEVEL | render state | D3DRS_MAXTESSELLATIONLEVEL = 179 | float | 0 |
| 83 | 0x53 | ADAPTIVETESS_X | render state | D3DRS_ADAPTIVETESS_X = 180 | float | 0 |
| 84 | 0x54 | ADAPTIVETESS_Y | render state | D3DRS_ADAPTIVETESS_Y = 181 | float | 0 |
| 85 | 0x55 | ADAPTIVETESS_Z | render state | D3DRS_ADAPTIVETESS_Z = 182 | float | 0 |
| 86 | 0x56 | ADAPTIVETESS_W | render state | D3DRS_ADAPTIVETESS_W = 183 | float | 0 |
| 87 | 0x57 | ENABLEADAPTIVETESSELLATION | render state | D3DRS_ENABLEADAPTIVETESSELLATION = 184 | dword | 0 |
| 88 | 0x58 | TWOSIDEDSTENCILMODE | render state | D3DRS_TWOSIDEDSTENCILMODE = 185 | dword | 0 |
| 89 | 0x59 | CCW_STENCILFAIL | render state | **table: 53 (0x35, D3DRS_STENCILFAIL); D3D9 D3DRS_CCW_STENCILFAIL = 186** | dword | 0 |
| 90 | 0x5a | CCW_STENCILZFAIL | render state | D3DRS_CCW_STENCILZFAIL = 187 | dword | 0 |
| 91 | 0x5b | CCW_STENCILPASS | render state | D3DRS_CCW_STENCILPASS = 188 | dword | 0 |
| 92 | 0x5c | CCW_STENCILFUNC | render state | D3DRS_CCW_STENCILFUNC = 189 | dword | 0 |
| 93 | 0x5d | COLORWRITEENABLE1 | render state | D3DRS_COLORWRITEENABLE1 = 190 | dword | 0 |
| 94 | 0x5e | COLORWRITEENABLE2 | render state | D3DRS_COLORWRITEENABLE2 = 191 | dword | 0 |
| 95 | 0x5f | COLORWRITEENABLE3 | render state | D3DRS_COLORWRITEENABLE3 = 192 | dword | 0 |
| 96 | 0x60 | BLENDFACTOR | render state | D3DRS_BLENDFACTOR = 193 | dword | 0 |
| 97 | 0x61 | SRGBWRITEENABLE | render state | D3DRS_SRGBWRITEENABLE = 194 | dword | 0 |
| 98 | 0x62 | DEPTHBIAS | render state | D3DRS_DEPTHBIAS = 195 | float | 0 |
| 99 | 0x63 | SEPARATEALPHABLENDENABLE | render state | D3DRS_SEPARATEALPHABLENDENABLE = 206 | dword | 0 |
| 100 | 0x64 | SRCBLENDALPHA | render state | D3DRS_SRCBLENDALPHA = 207 | dword | 0 |
| 101 | 0x65 | DESTBLENDALPHA | render state | D3DRS_DESTBLENDALPHA = 208 | dword | 0 |
| 102 | 0x66 | BLENDOPALPHA | render state | **table: 171 (0xab, D3DRS_BLENDOP); D3D9 D3DRS_BLENDOPALPHA = 209** | dword | 0 |
| 103 | 0x67 | COLOROP | texture stage | D3DTSS_COLOROP = 1 | dword | 0-7 |
| 104 | 0x68 | COLORARG0 | texture stage | D3DTSS_COLORARG0 = 26 | dword | 0-7 |
| 105 | 0x69 | COLORARG1 | texture stage | D3DTSS_COLORARG1 = 2 | dword | 0-7 |
| 106 | 0x6a | COLORARG2 | texture stage | D3DTSS_COLORARG2 = 3 | dword | 0-7 |
| 107 | 0x6b | ALPHAOP | texture stage | D3DTSS_ALPHAOP = 4 | dword | 0-7 |
| 108 | 0x6c | ALPHAARG0 | texture stage | D3DTSS_ALPHAARG0 = 27 | dword | 0-7 |
| 109 | 0x6d | ALPHAARG1 | texture stage | D3DTSS_ALPHAARG1 = 5 | dword | 0-7 |
| 110 | 0x6e | ALPHAARG2 | texture stage | D3DTSS_ALPHAARG2 = 6 | dword | 0-7 |
| 111 | 0x6f | RESULTARG | texture stage | D3DTSS_RESULTARG = 28 | dword | 0-7 |
| 112 | 0x70 | BUMPENVMAT00 | texture stage | D3DTSS_BUMPENVMAT00 = 7 | float | 0-7 |
| 113 | 0x71 | BUMPENVMAT01 | texture stage | D3DTSS_BUMPENVMAT01 = 8 | float | 0-7 |
| 114 | 0x72 | BUMPENVMAT10 | texture stage | D3DTSS_BUMPENVMAT10 = 9 | float | 0-7 |
| 115 | 0x73 | BUMPENVMAT11 | texture stage | D3DTSS_BUMPENVMAT11 = 10 | float | 0-7 |
| 116 | 0x74 | TEXCOORDINDEX | texture stage | D3DTSS_TEXCOORDINDEX = 11 | dword | 0-7 |
| 117 | 0x75 | BUMPENVLSCALE | texture stage | D3DTSS_BUMPENVLSCALE = 22 | float | 0-7 |
| 118 | 0x76 | BUMPENVLOFFSET | texture stage | D3DTSS_BUMPENVLOFFSET = 23 | float | 0-7 |
| 119 | 0x77 | TEXTURETRANSFORMFLAGS | texture stage | D3DTSS_TEXTURETRANSFORMFLAGS = 24 | dword | 0-7 |
| 120 | 0x78 | CONSTANT | texture stage | D3DTSS_CONSTANT = 32 | dword | 0-7 |
| 121 | 0x79 | PATCHSEGMENTS | npatch | N-patch segments (SetNPatchMode); Wine names it NPatchMode | float | 0 |
| 122 | 0x7a | FVF | FVF | SetFVF | dword | 0 |
| 123 | 0x7b | PROJECTIONTRANSFORM | transform | D3DTS_PROJECTION = 3 | float 4x4 | 0 |
| 124 | 0x7c | VIEWTRANSFORM | transform | D3DTS_VIEW = 2 | float 4x4 | 0 |
| 125 | 0x7d | WORLDTRANSFORM | transform | D3DTS_WORLDMATRIX(index) = 256 + index | float 4x4 | 0-255 |
| 126 | 0x7e | TEXTURETRANSFORM | transform | D3DTS_TEXTURE0 + index = 16 + index | float 4x4 | 0-7 |
| 127 | 0x7f | MATERIALDIFFUSE | material | D3DMATERIAL9.Diffuse | float 1x4 | 0 |
| 128 | 0x80 | MATERIALAMBIENT | material | D3DMATERIAL9.Ambient | float 1x4 | 0 |
| 129 | 0x81 | MATERIALSPECULAR | material | D3DMATERIAL9.Specular | float 1x4 | 0 |
| 130 | 0x82 | MATERIALEMISSIVE | material | D3DMATERIAL9.Emissive | float 1x4 | 0 |
| 131 | 0x83 | MATERIALPOWER | material | D3DMATERIAL9.Power | float | 0 |
| 132 | 0x84 | LIGHTTYPE | light | D3DLIGHT9.Type | dword | any |
| 133 | 0x85 | LIGHTDIFFUSE | light | D3DLIGHT9.Diffuse | float 1x4 | any |
| 134 | 0x86 | LIGHTSPECULAR | light | D3DLIGHT9.Specular | float 1x4 | any |
| 135 | 0x87 | LIGHTAMBIENT | light | D3DLIGHT9.Ambient | float 1x4 | any |
| 136 | 0x88 | LIGHTPOSITION | light | D3DLIGHT9.Position | float 1x3 | any |
| 137 | 0x89 | LIGHTDIRECTION | light | D3DLIGHT9.Direction | float 1x3 | any |
| 138 | 0x8a | LIGHTRANGE | light | D3DLIGHT9.Range | float | any |
| 139 | 0x8b | LIGHTFALLOFF | light | D3DLIGHT9.Falloff | float | any |
| 140 | 0x8c | LIGHTATTENUATION0 | light | D3DLIGHT9.Attenuation0 | float | any |
| 141 | 0x8d | LIGHTATTENUATION1 | light | D3DLIGHT9.Attenuation1 | float | any |
| 142 | 0x8e | LIGHTATTENUATION2 | light | D3DLIGHT9.Attenuation2 | float | any |
| 143 | 0x8f | LIGHTTHETA | light | D3DLIGHT9.Theta | float | any |
| 144 | 0x90 | LIGHTPHI | light | D3DLIGHT9.Phi | float | any |
| 145 | 0x91 | LIGHTENABLE | light enable | LightEnable(index) | dword | any |
| 146 | 0x92 | VERTEXSHADER | vertex shader | SetVertexShader | vertexshader | 0 |
| 147 | 0x93 | PIXELSHADER | pixel shader | SetPixelShader | pixelshader | 0 |
| 148 | 0x94 | VERTEXSHADERCONSTANTF | shader constant | SetVertexShaderConstantF | float n | any |
| 149 | 0x95 | VERTEXSHADERCONSTANTB | shader constant | SetVertexShaderConstantB | bool n | any |
| 150 | 0x96 | VERTEXSHADERCONSTANTI | shader constant | SetVertexShaderConstantI | int n | any |
| 151 | 0x97 | VERTEXSHADERCONSTANT | shader constant | SetVertexShaderConstantF | float n | any |
| 152 | 0x98 | VERTEXSHADERCONSTANT1 | shader constant | SetVertexShaderConstantF, 1 register | float 1x4 | any |
| 153 | 0x99 | VERTEXSHADERCONSTANT2 | shader constant | SetVertexShaderConstantF, 2 registers | float 2x4 | any |
| 154 | 0x9a | VERTEXSHADERCONSTANT3 | shader constant | SetVertexShaderConstantF, 3 registers | float 3x4 | any |
| 155 | 0x9b | VERTEXSHADERCONSTANT4 | shader constant | SetVertexShaderConstantF, 4 registers | float 4x4 | any |
| 156 | 0x9c | PIXELSHADERCONSTANTF | shader constant | SetPixelShaderConstantF | float n | any |
| 157 | 0x9d | PIXELSHADERCONSTANTB | shader constant | SetPixelShaderConstantB | bool n | any |
| 158 | 0x9e | PIXELSHADERCONSTANTI | shader constant | SetPixelShaderConstantI | int n | any |
| 159 | 0x9f | PIXELSHADERCONSTANT | shader constant | SetPixelShaderConstantF | float n | any |
| 160 | 0xa0 | PIXELSHADERCONSTANT1 | shader constant | SetPixelShaderConstantF, 1 register | float 1x4 | any |
| 161 | 0xa1 | PIXELSHADERCONSTANT2 | shader constant | SetPixelShaderConstantF, 2 registers | float 2x4 | any |
| 162 | 0xa2 | PIXELSHADERCONSTANT3 | shader constant | SetPixelShaderConstantF, 3 registers | float 3x4 | any |
| 163 | 0xa3 | PIXELSHADERCONSTANT4 | shader constant | SetPixelShaderConstantF, 4 registers | float 4x4 | any |
| 164 | 0xa4 | TEXTURE | texture | SetTexture(index) | texture | 0-260 |
| 165 | 0xa5 | ADDRESSU | sampler state | D3DSAMP_ADDRESSU = 1 | dword | 0-260 |
| 166 | 0xa6 | ADDRESSV | sampler state | D3DSAMP_ADDRESSV = 2 | dword | 0-260 |
| 167 | 0xa7 | ADDRESSW | sampler state | D3DSAMP_ADDRESSW = 3 | dword | 0-260 |
| 168 | 0xa8 | BORDERCOLOR | sampler state | D3DSAMP_BORDERCOLOR = 4 | dword | 0-260 |
| 169 | 0xa9 | MAGFILTER | sampler state | D3DSAMP_MAGFILTER = 5 | dword | 0-260 |
| 170 | 0xaa | MINFILTER | sampler state | D3DSAMP_MINFILTER = 6 | dword | 0-260 |
| 171 | 0xab | MIPFILTER | sampler state | D3DSAMP_MIPFILTER = 7 | dword | 0-260 |
| 172 | 0xac | MIPMAPLODBIAS | sampler state | D3DSAMP_MIPMAPLODBIAS = 8 | float | 0-260 |
| 173 | 0xad | MAXMIPLEVEL | sampler state | D3DSAMP_MAXMIPLEVEL = 9 | dword | 0-260 |
| 174 | 0xae | MAXANISOTROPY | sampler state | D3DSAMP_MAXANISOTROPY = 10 | dword | 0-260 |
| 175 | 0xaf | SRGBTEXTURE | sampler state | D3DSAMP_SRGBTEXTURE = 11 | dword | 0-260 |
| 176 | 0xb0 | ELEMENTINDEX | sampler state | D3DSAMP_ELEMENTINDEX = 12 | dword | 0-260 |
| 177 | 0xb1 | DMAPOFFSET | sampler state | D3DSAMP_DMAPOFFSET = 13 | dword | 0-260 |
| 178 | 0xb2 | SAMPLER | sampler | a sampler parameter bound to a stage | sampler | 0-260 |
| 179 | 0xb3 | STATEBLOCK | state block | state block (not in Wine's table) | stateblock | 0 |
| 180 | 0xb4 | $PRESHADER_VS | preshader | internal (not in Wine's table) | vertexshader | 0 |
| 181 | 0xb5 | $PRESHADER_PS | preshader | internal (not in Wine's table) | pixelshader | 0 |

The table stores the transforms as 0..3 in its own numbering (projection, view, world, texture) and the material and
light fields as member numbers; the D3D9 targets in those rows are Wine's mapping (**W**). The shader-constant rows'
register counts are the table's (**D**).

### 3.2 The WRAP region and UG2's off-by-8

Rows 31..38 are WRAP0..WRAP7 (D3D9 128..135) and rows **39..46 are WRAP8..WRAP15** (D3D9 198..205). They come directly
after WRAP7 in the file's numbering, although their D3D9 values are far away (**D**, **W**, **K**). A table that leaves
out the eight WRAP8..15 rows maps every row from 39 on eight rows too far: recomp-kit's early table applied row 65
(POINTSCALE_A) as COLORWRITEENABLE (GAME.md §5.5, LESSONS L-084). The UG2 port's own D3DX was lifted code and had the
right table (**U**). MW's files use no row in 31..47, but they use rows 48, 49, 51, 54, 55, 56 and 73 (COLORWRITEENABLE)
directly after the region (**V**, C7), so a shifted table breaks them.

Rows MW uses (**V**): 0, 3, 4, 6..10, 13..15, 30, 48, 49, 51, 54..56, 73, 103..107, 109, 110, 116, 119, 122, 126, 127,
132, 133, 135, 137, 145..147, 164..166, 169..172, 174.

### 3.3 The index field

| Class | Index | Src |
|---|---|---|
| render state, FVF, npatch, shaders, view, projection, material | 0 | **D** (index count 1); **V** (always 0) |
| texture stage, TEXTURETRANSFORM | stage 0..7 | **D**; **V** (0..3 seen) |
| WORLDTRANSFORM | matrix 0..255 | **D** |
| light, light enable | light number | **D**; **V** (0..2 seen) |
| TEXTURE, sampler states, SAMPLER in a pass | sampler 0..15, 256 (`D3DDMAPSAMPLER`), 257..260 (vertex samplers) | **D** (index count 261), S |
| any state **inside a sampler block** | **256 in every MW file**. D3DX binds a sampler parameter to the register the shader's CTAB gives it, so this field is not the stage | **V**; W, K (the stage comes from the shader's constant table) |

### 3.4 The value

- A state's value is a literal of its typedef's type, or an object id for an object type. A usage 0 resource on the
  state (§2.5.7) replaces the literal with a preshader result at apply time. **V**, W
- A NULL shader or texture is written as an INT literal 0. D3DX accepts a shader or texture state with a numeric value
  only when it is an INT 0. **V** (32 VERTEXSHADER/PIXELSHADER states), W
- Numeric state values are class MATRIX_ROWS with **N rows by 1 column** for an N-component value, while the table
  gives such states as 1 row by N columns: MATERIALDIFFUSE, LIGHTDIFFUSE and LIGHTAMBIENT are written as 4×1,
  LIGHTDIRECTION as 3×1, TEXTURETRANSFORM as 4×4, everything else as 1×1. **V** (C7)
- C7 found one state whose type disagrees with the table: one TEXTURETRANSFORM whose value is an INT 1×1 literal 0.
  What D3DX does with it belongs to M5-04/05 (§8).

### 3.5 d3dx9_26 and Wine compared

| Row | d3dx9_26 (**D**) | Wine (**W**) |
|---|---|---|
| 89 CCW_STENCILFAIL | value 53, D3DRS_STENCILFAIL | D3DRS_CCW_STENCILFAIL (186) |
| 102 BLENDOPALPHA | value 171, D3DRS_BLENDOP | D3DRS_BLENDOPALPHA (209) |
| 121 | PATCHSEGMENTS | NPatchMode (same class) |
| 179..181 | STATEBLOCK, $PRESHADER_VS, $PRESHADER_PS | not present (Wine refuses rows past 178) |

MW uses none of these rows (**V**). Whether d3dx9_26 applies its table's value for rows 89 and 102 at run time was not
checked (§8).

## 4. Programs

### 4.1 D3D9 shader bytecode (S; **V**, C6)

| Token | Layout |
|---|---|
| Version (dword 0) | `0xFFFE0000 \| major << 8 \| minor` (vertex), `0xFFFF0000 \| major << 8 \| minor` (pixel). ps_2_x is `0xFFFF0201`; its CTAB target string reads `ps_2_b` in MW (**V**) |
| Comment | Low word `0xFFFE`; bits 16..30: length L in dwords that follow; bit 31 is 0. The first following dword is normally a FOURCC (`CTAB`, `PRES`, ...) |
| Instruction | Bits 0..15 opcode; 16..23 opcode-specific controls; 24..27 length (the number of tokens that follow) in shader model 2 and 3; 28 predicated; 30 co-issue (ps_1_x); 31 is 0. In models 1.x bits 24..27 are 0 (**V**) |
| Parameter | Every destination and source token has bit 31 set |
| End | `0x0000FFFF`, the last dword |

**Instruction length** (needed for models 1.x, which have no length field): with major ≥ 2, bits 24..27; with major < 2,
`def` (opcode `0x51`) takes 5 tokens, `defi` (`0x30`) 5, `defb` (`0x2F`) 2, and any other instruction takes every
following token with bit 31 set. **V** (C6): walking all 142 shaders this way stops exactly on the end token as the
last dword, and in the shader model 2 and 3 shaders the length field and the bit-31 rule agree on every instruction.

**Instruction count** (the dump's): the instruction tokens between the version token and the end token, comments not
counted (`dcl`, `def` and ps_1_4's `phase` (`0xFFFD`) count). In MW, comments appear only before the first instruction:
one CTAB in 140 shaders, none in 2 ps_1_1 shaders. **V** (C6)

### 4.2 CTAB, the constant table (S `d3dx9shader.h`; **V**, C6, C3)

A comment whose first dword is `CTAB` (`0x42415443`). All offsets in it are relative to the first byte after the
FOURCC.

D3DXSHADER_CONSTANTTABLE, 28 bytes:

| Off | Field | Src |
|---|---|---|
| +0 | Size, 28 | **V** |
| +4 | Creator: offset of a NUL-terminated string (the compiler's name and version) | **V** |
| +8 | Version: equals the program's version token (preshaders: `0x46580200`) | **V** |
| +12 | Constants: count | **V** |
| +16 | ConstantInfo: offset of the constant entries | **V** |
| +20 | Flags: the compile flags (`D3DXSHADER_*`) | **V** (`0x100` = NO_PRESHADER, `0x120` = NO_PRESHADER \| PARTIALPRECISION), S |
| +24 | Target: offset of a NUL-terminated string | **V** (`vs_1_1`, `vs_3_0`, `ps_1_1`, `ps_1_4`, `ps_2_0`, `ps_2_b`, `ps_3_0`; `tx` in every preshader) |

D3DXSHADER_CONSTANTINFO, 20 bytes each:

| Off | Field | Src |
|---|---|---|
| +0 | Name offset | **V** |
| +4 | RegisterSet, 16 bits: 0 BOOL, 1 INT4, 2 FLOAT4, 3 SAMPLER | **V** (2 and 3), S |
| +6 | RegisterIndex, 16 bits | **V** |
| +8 | RegisterCount, 16 bits | **V** |
| +10 | Reserved, 16 bits | **V** (always 0) |
| +12 | TypeInfo offset | **V** |
| +16 | DefaultValue offset, 0 = none | **V** (present for every numeric constant in a preshader or vertex shader, and for some pixel shader constants; never for samplers) |

D3DXSHADER_TYPEINFO, 16 bytes:

| Off | Field | Src |
|---|---|---|
| +0 | Class, 16 bits (the classes of §2.3) | **V** |
| +2 | Type, 16 bits (the types of §2.3) | **V** |
| +4 | Rows, 16 bits | **V** |
| +6 | Columns, 16 bits | **V** |
| +8 | Elements, 16 bits: 1 for a non-array | **V** (equals max(1, the effect parameter's element count)) |
| +10 | StructMembers, 16 bits | **V** (0) |
| +12 | StructMemberInfo offset: StructMembers entries of {name offset, TypeInfo offset} | S |

**V** (C3): every CTAB constant that has an effect parameter of the same name agrees with it on rows × columns
(vectors are 1 × N in both) and on elements. 4×4 MATRIX_ROWS parameters are MATRIX_COLUMNS 4×4 in the CTAB (the
packing in registers), and SAMPLER parameters are SAMPLER2D (or SAMPLERCUBE) in the CTAB.

### 4.3 The FXLC preshader (W, K; **V**, C5)

A preshader is a bytecode with its own version token, the comments below, and the end token:

| Part | Layout | Src |
|---|---|---|
| Version | `0x46580200` ("FX" 2.0). Wine defines `0x46580000` as the preshader signature but never checks it | **V** (all 616), W |
| Comments | D3DX finds each one by FOURCC among the comments that follow the version, so their order is free | W; **V**: always exactly CTAB, CLIT, FXLC in that order |
| End | `0x0000FFFF` right after the last comment, as the last dword | **V** |
| CTAB | The inputs: constants in registers, matched to effect parameters by name (§4.2) | **V**, W, K |
| CLIT (`0x54494C43`) | A count n, then n IEEE doubles (8 bytes each): the literal table | **V** (the comment holds exactly 1 + 2n dwords after its FOURCC), W, K |
| FXLC (`0x434C5846`) | An instruction count m, m instructions, then a trailer | **V**, W, K |
| Trailer | Two dwords, `0xF0F0F0F0 0x0F0F0F0F`, after the last instruction. Neither Wine nor recomp-kit reads them | **V** (all 616) |

**Instruction**:

| Word | Layout | Src |
|---|---|---|
| Token | Bits 0..15 component count (1..4); 16..19 zero; 20..30 opcode; 31 scalar flag | **V** (bits 16..19 are 0 in all 739 instructions), W, K |
| Input count k | Must equal the opcode's (D3DX picks the table row by opcode and count) | **V**, W |
| Operands | k inputs, then one output | **V**, W, K |

**Operand**: a flag word, then registers of two words each (table code, offset):

| Flag | Words | Meaning | Src |
|---|---|---|---|
| 0 | 3 | flag, table, offset | **V**, W, K |
| 1 | 5 | flag, index table, index offset, table, offset: relative addressing, the register is offset + the value of the index register. Wine refuses it on the output | W |
| other | | invalid | W |

**Tables** (W; **V** for 1, 2 and 4): offsets count components, four per register, so register = offset / 4 and
component = offset % 4, except where noted.

| Code | Dump | Table | Src |
|---|---|---|---|
| 1 | `imm` | the CLIT literals (doubles); offset = literal number | **V**, W, K |
| 2 | `c` | input constants: the registers the preshader's CTAB names (FLOAT4 set) | **V** (every such operand lies inside a FLOAT4 constant of the CTAB), W, K |
| 3 | `v` | Wine's "input" table, used only for texture shaders | W |
| 4 | `oc` | float outputs: for a state, components 0..N−1 are its value | **V** (outputs are `oc[0..N−1]`, N = the state type's rows × columns), W, K |
| 5 | `ob` | bool outputs; Wine divides the stored offset by 4 | W |
| 6 | `oi` | int outputs | W |
| 7 | `r` | temporaries | W, K |
| 0 | | invalid | W |

**Opcodes** (W: Wine's table, all of it; K agrees on the ones it evaluates):

| Opcode | Inputs | Dump | Notes |
|---|---|---|---|
| 0x000 | 0 | `nop` | |
| 0x100 | 1 | `mov` | **V**: the only opcode in MW (739 instructions, not scalar, 1, 3 or 4 components; sources `c` and `imm`, no relative addressing; 24 literals, all whole numbers) |
| 0x101 | 1 | `neg` | |
| 0x103 | 1 | `rcp` | not the D3D9 shader opcodes (RCP 6, RSQ 7), which recomp-kit's shader interpreter once swapped (GAME.md §5.5) |
| 0x104 | 1 | `frc` | |
| 0x105 | 1 | `exp` | |
| 0x106 | 1 | `log` | |
| 0x107 | 1 | `rsq` | |
| 0x108 | 1 | `sin` | |
| 0x109 | 1 | `cos` | |
| 0x10a | 1 | `asin` | |
| 0x10b | 1 | `acos` | |
| 0x10c | 1 | `atan` | |
| 0x200 | 2 | `min` | |
| 0x201 | 2 | `max` | |
| 0x202 | 2 | `lt` | 1.0 if x < y, else 0.0 (K) |
| 0x203 | 2 | `ge` | 1.0 if x ≥ y, else 0.0 (K) |
| 0x204 | 2 | `add` | |
| 0x205 | 2 | `mul` | |
| 0x206 | 2 | `atan2` | |
| 0x208 | 2 | `div` | Wine returns 0 ("according to the test results"; fxc emits rcp and mul instead) |
| 0x300 | 3 | `cmp` | x ≥ 0 ? y : z (K) |
| 0x301 | 3 | `movc` | x ≠ 0 ? y : z. **K only**: not in Wine's table |
| 0x500 | 2 | `dot` | all components at once: the sum of the products of the n components, written to component 0 (W) |
| 0x70e | 6 | `dotswiz6` | per output component j, the 3-component dot of inputs (0,3), (1,4), (2,5) at component j (W) |
| 0x70e | 8 | `dotswiz8` | the same with 4 components: inputs (0,4), (1,5), (2,6), (3,7) (W) |

With the scalar flag, input 0 is read at component 0 for every output component (W, K). The operations work on
doubles; the literal table holds doubles, while Wine keeps inputs, temporaries and outputs as floats (W; K keeps
temporaries as doubles). The evaluator's exact results are M5-03's, checked against the oracle.

### 4.4 PRES and PRSI in a shader (W; not in MW)

A shader can carry a preshader in a `PRES` comment (`0x53455250`) among its leading comments: the comment's payload is a
preshader as in §4.3. D3DX runs it to compute shader constants: in a shader, the CTAB's FLOAT4 registers are outputs
(Wine maps them to `oc`) (W). `PRSI` (`0x49535250`) is defined in Wine but never read; its layout is not pinned down
(§8). **V**: no MW shader has a PRES or PRSI comment, and every shader CTAB carries the NO_PRESHADER compile flag.

### 4.5 The array selector (usage 2; W; not in MW)

Data: size S2, S2 bytes holding the NUL-terminated name of an array parameter, then a preshader (§4.3) of
`size − 4 − S2` bytes. D3DX evaluates it as an INT and the state takes that element of the array (for shader arrays,
the element's shader).

## 5. The canonical dump (`fx2dump 1`)

### 5.1 Files and text

- One dump per effect, `<NAME>.txt` from `out/mw/fx/res/<NAME>.fxo`, where NAME is the file name without `.fxo`:
  implementation A (`runtime/mw/fx2.c` with `tools/mw/fxdump.c`, run by `tools/mw/fxdump.sh`) writes them to
  `out/mw/fx/a/`, implementation B (`tools/fx2dump.py`) to `out/mw/fx/b/`; `web/test/mw-fx.test.mjs` checks that the
  two agree byte for byte (§7, C10).
- ASCII only; lines end in LF (`\n`), the last line too; no CR, no tab, no trailing space, no empty line. Tokens are
  separated by exactly one space. Indentation is two spaces per level.
- **HEX8** is `0x` and 8 lowercase hex digits; **HEX16** is `0x` and 16. Decimal numbers have no leading zeros and no
  plus sign; negative numbers start with `-`. Every count, index, offset and size is the stored word as an unsigned
  decimal; only INT and BOOL values are signed, as INT(w).
- **QSTR(bytes)**: `"`, then each byte: 0x20..0x7E as itself except `"` → `\"` and `\` → `\\`; any other byte → `\x`
  and 2 lowercase hex digits; then `"`. A name, semantic or string at offset 0 prints as `-`. Names are the stored bytes
  up to the first NUL (all S bytes when there is none).
- **F32(w)**: HEX8 of the dword, `:`, then **G9** of its value as an IEEE single. **F64(q)**: HEX16 of the qword
  (little-endian), `:`, then **G17** of its value as an IEEE double. **INT(w)**: the dword as a signed 32-bit decimal.
- **Gp(x)** is C99 `printf("%.<p>g", x)` with x as a double (a single is widened exactly): with X the decimal
  exponent of x rounded to p significant digits, use fixed notation with p − 1 − X decimals when −4 ≤ X < p, else
  exponent notation `d.ddde±XX` with p − 1 decimals and at least two exponent digits; then remove trailing zeros, and
  the decimal point if nothing follows it. Rounding is correct rounding (round half to even on the exact binary
  value), as glibc, macOS libc and Python's `'%.9g' % x` all do. Special cases: NaN prints `nan` (whatever its sign
  and payload), infinities `inf` and `-inf`, negative zero `-0`.
- **UNK(v)**: `UNKNOWN(` HEX8 of v `)`. A dump's **unknown count** is the number of times `UNKNOWN(` occurs in it;
  M5-01's goal is 0 in all 31 dumps.

### 5.2 Order

1. The three header lines.
2. Parameter blocks, in file order.
3. Technique blocks, in file order.
4. Object lines, ids 0 to O−1, then one line for each object-data entry whose id is O or more (§5.7).
5. Resource blocks, in file order.
6. The line `end`.

Inside blocks everything is in file order: annotations, passes, states, sampler states, elements, struct members,
comments, CTAB constants, literals, instructions.

### 5.3 Header

```
fx2dump 1
effect QSTR(NAME) size <file bytes> sha256 <64 lowercase hex digits>
header tag HEX8 tables HEX8 data0 HEX8 params <P> techniques <T> shaders <shader count> objects <O> strings <N> resources <R>
```

`tag` and `tables` are the two file header dwords, `data0` the dword at body +0, then the four counts of §2.5 and the
object-data and resource counts, all decimal.

### 5.4 Types and values

**TT(typedef)** = TYPE `␠` CLASS, then ` <rows>x<columns>` for SCALAR, VECTOR, MATRIX_ROWS and MATRIX_COLUMNS (rows
first, whatever the file order: a float4 vector prints `1x4`), ` <M>` (member count) for STRUCT, and ` [<elements>]`
when the element count is not 0. TYPE and CLASS are the names of §2.3; a type of 20 or more prints UNK(type), a class
of 6 or more prints UNK(class) and the typedef has no further fields (` [<elements>]` still follows when the element
count, a common field, is not 0).

**Values.** An owner line OWN (a parameter, annotation, struct member or state line) with typedef t and its value:

- A **simple** value is one of class SCALAR, VECTOR, MATRIX_ROWS or MATRIX_COLUMNS, or of class OBJECT with a type other
  than SAMPLER, SAMPLER1D, SAMPLER2D, SAMPLER3D or SAMPLERCUBE.
- Simple, element count 0: the owner line ends with ` = ` FRAG.
- Otherwise the owner line ends after its own fields, and for each element e (one element, written without `[e]`,
  when the element count is 0), one level deeper:
  - simple: `value [e] = FRAG` (or `value = FRAG`);
  - a sampler: `sampler [e] states <n>` (or `sampler states <n>`), then its n state lines (§5.6) one level deeper;
  - a struct: `struct [e]` (or `struct`), then for each member m, one level deeper, the owner line
    `member <m> TT name QSTR sem QSTR` with the member's value by these same rules;
  - an unknown class: `value [e] = UNK(class)` (or `value = UNK(class)`).
- **FRAG** of a numeric element: its rows × columns dwords in file order, each as F32 for FLOAT, INT for BOOL and INT,
  HEX8 for any other type, separated by spaces; `-` if there are none. FRAG of an object element: `obj <id>`, or
  `obj UNK(id)` when the id is O or more.
- Value data is read element after element: rows × columns dwords for a numeric element, one dword for an object,
  a count and 16 bytes per state for a sampler, the members' values in order for a struct.

### 5.5 Parameters and techniques

```
param <i> TT name QSTR sem QSTR flags HEX8 annotations <A>[ = FRAG]
  annotation <k> TT name QSTR sem QSTR[ = FRAG]
technique <i> name QSTR annotations <A> passes <Q>
  annotation <k> TT name QSTR sem QSTR[ = FRAG]
  pass <j> name QSTR annotations <A> states <S>
    annotation <k> TT name QSTR sem QSTR[ = FRAG]
    state ...
```

A parameter's value lines come right after its line, then its annotations (each with its own value lines). A
technique's annotations come before its passes; a pass's annotations before its states.

### 5.6 States

```
state <k> <NAME>[<index>] op <operation> TT[ = FRAG][ res <r>]...
```

k is the position in the pass or sampler block. NAME is the state table's name (§3.1) for operations 0..181, else
UNK(operation); `<index>` is decimal with no space before `[`. The value follows §5.4. For every resource whose target
is this state, ` res <r>` is added to the end of the state's first line, in resource order. A resource targets:

- a pass state: technique t, pass j, state k;
- a state in the sampler block of parameter i: technique index `0xFFFFFFFF`, index i, state k, and element e for
  element e of an array, or any element for a sampler that is not an array (D3DX ignores the field there, §2.5.7;
  §5.8 counts a value other than 0 and `0xFFFFFFFF`).

Sampler blocks inside annotations, struct members or state values have no resources.

### 5.7 Objects

```
object <id> <TYPE>[ string <k> size <S>]...[ res <r>]...
```

TYPE is the type name of the first value that holds the id, in dump order (parameter values, each parameter's
annotations, then technique and pass annotations and states; a sampler block's states where the block is printed), or
`-` when no value holds it (UNK for a type of 20 or more). Then ` string <k> size <S>` for every object-data entry k
(numbered from 0 in file order) with that id, then ` res <r>` for every resource whose target state's own line ends in
` = obj <id>`: an object value of a non-sampler type with element count 0. A state whose value is an object array
(`value [e] = obj <id>` lines) gives its resources to no object.

After the lines for ids 0 to O−1 comes `object UNK(id) string <k> size <S>` for each object-data entry k whose id is
O or more, in file order. D3DX has no object for such an entry, so the dump counts it instead of leaving it out.

### 5.8 Resources

```
resource <r> technique <t> pass <j> state <k> usage <u> size <S>
resource <r> param <i> element <e|-> state <k> usage <u> size <S>
```

`-` stands for element `0xFFFFFFFF`. When the resource has a target state in the sampler block of a parameter that
is not an array, an element other than 0 and `0xFFFFFFFF` prints as UNK(e): D3DX ignores it (§2.5.7), but it is
not what a well-formed file holds. The content, one level deeper, depends on the usage and the type of the target
state's value:

| Usage | Target type | Content |
|---|---|---|
| 0 | VERTEXSHADER, PIXELSHADER | shader block (§5.9) |
| 0 | BOOL, INT, FLOAT, STRING | preshader block (§5.10) over the whole data |
| 1 | any | `name QSTR(the data up to the first NUL)` |
| 2 | any | `select QSTR(name)`, then a preshader block over the data from byte 4 + S2. If 4 + S2 > size: `select UNK(S2)` only. If size < 4 (no S2): `select UNK(size)` only |
| anything else, or no target state | | `UNK(usage)` |

### 5.9 Shader block

```
shader <VN> HEX8(version) dwords <n> instructions <m> <END>
comment QSTR(fourcc) dwords <L>
ctab size <Size> creator QSTR version HEX8 target QSTR flags HEX8 constants <c>
const <k> QSTR(name) <SET><index> count <count> <CLASS> <TYPE> <rows>x<columns> elements <e> members <sm> default <yes|no>
```

- n = size / 4, rounded down; a blob shorter than 4 bytes has version 0. VN is `vs_<major>_<minor>` for a version
  with high word `0xFFFE` and `ps_<major>_<minor>` for `0xFFFF` (major = bits 8..15, minor = bits 0..7), except major 2
  minor 1, which prints `vs_2_x` / `ps_2_x`. For any other high word the block is the single line
  `shader UNK(version) dwords <n>`.
- Walk from dword 1: the end token stops the walk; a token with low word `0xFFFE` is a comment of L = bits 16..30
  dwords (skip 1 + L); anything else is an instruction (count it, skip by §4.1's length rule). END is `end` when the
  walk stops on the end token at dword n − 1, else UNK(the dword index where it stopped, n or more if it ran out).
- One `comment` line per comment in walk order, after the shader line; the FOURCC is the 4 bytes of the comment's first
  dword (`""` when L = 0 or when that dword is not inside the blob, dword n or later).
- A comment's **payload** is its dwords after the FOURCC, up to its end or to dword n − 1, whichever comes first (whole
  dwords only: the 0..3 bytes past dword n − 1 of a blob whose size is no multiple of 4 never belong to a comment).
- After the comment lines, for each comment with FOURCC `CTAB` and L ≥ 8 whose payload holds the 28-byte header, its
  `ctab` line and `const` lines; the CTAB's offsets count from the payload's first byte. Strings are read
  NUL-terminated at their offset, up to the payload's end; offset 0 prints `-`, an offset at or past the payload's end
  UNK(offset). SET is `b`, `i`, `c`, `s` for register sets 0..3, else UNK(set); it is followed directly by the register
  index. CLASS and TYPE are the names of §2.3 (UNK beyond the table). `default` is `yes` when DefaultValue is not 0.
- A constant entry that does not fit in the payload (ConstantInfo + 20k + 20 > payload bytes) prints
  `const <k> UNK(<ConstantInfo + 20k, as a dword>)` and ends the list. A TypeInfo that does not fit (TypeInfo + 16 >
  payload bytes) prints UNK(TypeInfo) in place of `<CLASS> <TYPE> <rows>x<columns> elements <e> members <sm>`; the rest
  of the line stays.

### 5.10 Preshader block

```
preshader HEX8(version) dwords <n> <END>
comment QSTR(fourcc) dwords <L>
ctab ... / const ...                       (the first CTAB comment, as in §5.9)
literals <count>                           (the first CLIT comment; "literals -" when there is none)
lit <k> F64
instructions <m>                           (the first FXLC comment; "instructions -" when there is none)
ins <k> <MN> <ncomp>[ UNK(token)] <dst> <src0> ... <src(k-1)>
trailer[ HEX8]...
```

- n = size / 4, rounded down; a blob shorter than 4 bytes has version 0. If the version's high word is not `0x4658`,
  the block is the single line `preshader UNK(version) dwords <n>`.
- The comments are the run of comment tokens from dword 1, with FOURCCs and payloads as in §5.9. END is `end` when the
  dword after them is the end token and is dword n − 1, else UNK(that dword's index).
- `literals`: the first CLIT comment's payload is a count, then the doubles. No payload dword at all prints
  `literals UNK(L)`; a count whose doubles do not fit in the rest of the payload prints `literals UNK(count)` and no
  `lit` lines.
- `instructions`: the first FXLC comment's payload is the count m, then the instructions. No payload dword at all
  prints `instructions UNK(L)` and an empty `trailer`.
- MN is the opcode's name from §4.3, picked by opcode and input count, or UNK(opcode); `.s` is appended when the scalar
  flag is set. ncomp is bits 0..15, decimal. UNK(token) follows ncomp when bits 16..19 are not 0.
- The output operand is printed first, then the inputs. An operand prints `TAB[offset]` (flag 0) or
  `TAB[offset+ITAB[ioffset]]` (flag 1), with offsets as stored, in decimal; TAB is `imm`, `c`, `v`, `oc`, `ob`, `oi`,
  `r` for table codes 1..7, else UNK(code).
- An operand flag other than 0 or 1 ends decoding: that instruction's line is `ins <k> <MN> <ncomp>[ UNK(token)]
  UNK(flag)`, and the trailer starts at the flag's dword. If an instruction would read past the end of the FXLC
  payload, its line is `ins <k> UNK(<dwords left>)`, counted from the instruction's token to the payload's end, and
  decoding ends with an empty trailer.
- `trailer` lists the FXLC payload's dwords after the last decoded instruction, up to the payload's end.

### 5.11 Refused files

A dumper writes **no dump** (a message on stderr, exit status 1, no output file) for a file it cannot read as a
container. Everything else prints, with UNK where a value has no name. A file is refused when:

- it is shorter than 12 bytes (the tag, `tables` and body +0), or a table-area read (§2.5, entries, object data,
  resource headers and data) runs past the end of the file;
- the object count O is more than size / 4 + 1 (every id but 0 is held by a 4-byte value, §2.7);
- a name record (size and bytes, §2.2), a typedef (its 20 bytes, then the dimensions, the member count or the
  members), or a value is not wholly inside the data area, body [0, `tables`). This covers every name, typedef and
  value the tables reach, also the ones the dump never prints (the names and semantics of state typedefs). A value
  occupies rows × columns dwords per numeric element, one dword per object element, a count and 16 bytes per state
  per sampler element, and its members' values back to back for a struct; a numeric value with no bytes (0 rows or
  columns) must still start at or before `tables`, while a struct value with no members and a value of an unknown
  class read nothing and are not checked;
- the node budget runs out: every typedef read counts 1 (each struct member typedef too) and every value read counts
  its element count, or 1 when that is 0 (each struct member value and each sampler-block state's value too), over
  every parameter, annotation and state entry with their sampler blocks; a total above size + 64 is refused. This
  bounds time and memory (a numeric array of 0-column elements takes no bytes);
- anything is nested deeper than 32 levels: the typedef and the value of a parameter, annotation or state entry are
  at level 1; a struct member's typedef is one level below its struct's typedef; a struct member's value, and a
  sampler-block state's typedef and value, are one level below the value that holds them.

Bytes after the last resource are not read and do not refuse a file. A resource's data and an object-data entry are
never checked further: a shader, preshader or name that does not decode prints with UNK as §5.8..5.10 say.

### 5.12 Example

Made-up effect (one parameter of each common kind; not from any game), formatted by the rules above:

```
fx2dump 1
effect "EXAMPLE" size 1236 sha256 0000000000000000000000000000000000000000000000000000000000000000
header tag 0xfeff0901 tables 0x00000200 data0 0x00000000 params 3 techniques 1 shaders 3 objects 4 strings 1 resources 4
param 0 FLOAT VECTOR 1x4 name "tint" sem "TINT" flags 0x00000001 annotations 0 = 0x3f800000:1 0x3f000000:0.5 0x00000000:0 0xbdcccccd:-0.100000001
param 1 TEXTURE OBJECT name "base_map" sem - flags 0x00000000 annotations 0 = obj 1
param 2 SAMPLER OBJECT name "base_sampler" sem - flags 0x00000000 annotations 0
  sampler states 2
    state 0 TEXTURE[256] op 164 TEXTURE OBJECT = obj 2 res 3
    state 1 MINFILTER[256] op 170 INT MATRIX_ROWS 1x1 = 2
technique 0 name "t0" annotations 1 passes 1
  annotation 0 INT SCALAR 1x1 name "level" sem - = 3
  pass 0 name "p0" annotations 0 states 3
    state 0 ALPHABLENDENABLE[0] op 13 INT MATRIX_ROWS 1x1 = 0 res 2
    state 1 VERTEXSHADER[0] op 146 VERTEXSHADER OBJECT = obj 3 res 1
    state 2 PIXELSHADER[0] op 147 INT MATRIX_ROWS 1x1 = 0
object 0 -
object 1 TEXTURE string 0 size 0
object 2 TEXTURE res 3
object 3 VERTEXSHADER res 1
resource 0 technique 9 pass 0 state 0 usage 7 size 4
  UNKNOWN(0x00000007)
resource 1 technique 0 pass 0 state 1 usage 0 size 196
  shader vs_1_1 0xfffe0101 dwords 49 instructions 6 end
  comment "CTAB" dwords 30
  ctab size 28 creator "example compiler" version 0xfffe0101 target "vs_1_1" flags 0x00000100 constants 1
  const 0 "tint" c4 count 1 VECTOR FLOAT 1x4 elements 1 members 0 default yes
resource 2 technique 0 pass 0 state 0 usage 0 size 248
  preshader 0x46580200 dwords 62 end
  comment "CTAB" dwords 38
  comment "CLIT" dwords 4
  comment "FXLC" dwords 15
  ctab size 28 creator "example compiler" version 0x46580200 target "tx" flags 0x00000100 constants 1
  const 0 "tint" c0 count 1 VECTOR FLOAT 1x4 elements 1 members 0 default yes
  literals 1
  lit 0 0x3fe0000000000000:0.5
  instructions 1
  ins 0 ge 1 oc[0] c[3] imm[0]
  trailer 0xf0f0f0f0 0x0f0f0f0f
resource 3 param 2 element 0 state 0 usage 1 size 9
  name "base_map"
end
```

Resource 0 shows how a broken entry (an unknown usage on a technique that does not exist) is still printed and
counted. The example's sizes and counts are illustrative; it is not a file that exists.

## 6. Extraction and census (`tools/fxres.py`)

- `tools/fxres.py <speed.exe> [<outdir>]` checks the exe's SHA-256 against `games/mw/game.conf` `EXE_SHA256` and
  refuses any other file, then writes the 31 RT_RCDATA resources named `IDI_*_FX` (language 0x409) to
  `<outdir>/<NAME>.fxo` (default `out/mw/fx/res/`) with an `index.txt` (name, size, SHA-256). It stops unless there
  are exactly 31 and each starts with `01 09 FF FE`. It only reads the exe.
- `tools/fxres.py --census [<dir>]` counts, from the bytes alone, every **size-prefixed blob**: a dword N (≥ 8, a
  multiple of 4, inside the file) followed by N bytes whose first dword is a version token and whose last dword is
  the end token `0x0000FFFF`. The version tokens are high word `0xFFFE` (vs) or `0xFFFF` (ps) with major ≤ 3 and
  minor ≤ 4 (`0x....0201` named `_2_x`), and the preshader's `0x46580200` (`fxlc`). Every 4-aligned position of
  every file is tried, and nothing of §2's tables is used. It also counts raw version-token dwords, which come out
  about twice as high because every CTAB repeats its program's version (§4.2).

## 7. Checks run on MW's files

Run on 2026-10-26 over the 31 files `tools/fxres.py` wrote. C1..C7 and C9 used a scratch walker that implements §2 and
§4 (not committed); C8 is `tools/fxres.py --census`.

| Check | What was run | Result |
|---|---|---|
| C1 | Parse every file by §2, marking every byte read through the tables and offsets | All 31 files end exactly at their last byte. The only unread bytes are body +0 (a zero dword) and pad bytes. Nothing is read twice; every offset is 4-aligned and below `tables`; typedefs, names and values are each referenced once |
| C2 | Object ids held by values against O, and what fills each id | Ids 1..O−1 are each held by exactly one value and filled exactly once; id 0 by none; O = 1 + the ids held |
| C3 | Typedef dimensions; the effect parameters against same-named CTAB constants | The second dword of every VECTOR typedef is 1; 1,184 parameter/constant pairs agree on rows × columns and elements (scalars, vectors 1 × 3 and 1 × 4, matrices 4 × 4) |
| C4 | Every resource: target exists, unique, usage against target type, data signature | 836 resources (616 preshaders, 142 shaders, 78 parameter names), each on its own state |
| C5 | Every preshader: comments, CLIT size, instruction decode by §4.3, operand ranges, trailer | 616 preshaders, 739 instructions, all `mov`; 0 unknown opcodes, flags or tables; every `c` operand is inside a CTAB FLOAT4 register, every `imm` inside the CLIT table, outputs `oc[0..N−1]` with N the state's size; every FXLC ends with `0xF0F0F0F0 0x0F0F0F0F` |
| C6 | Every shader: token walk by §4.1, both length rules, CTAB header | 142 shaders end on the end token as the last dword; 0 length disagreements; CTAB Size 28 and Version equal to the version token in all 140 CTABs |
| C7 | Every state's operation and value type against `d3dx9_26.dll`'s table | 1,647 of 1,648 states agree (type, and N components as N × 1); the exception is §3.4's INT TEXTURETRANSFORM |
| C8 | `tools/fxres.py --census` | blobs: vs_1_1 69, vs_3_0 2, ps_1_1 30, ps_1_4 3, ps_2_0 18, ps_2_x 18, ps_3_0 2, fxlc 616 (GAME.md §5.3's counts); raw tokens 138, 4, 58, 6, 36, 36, 4, 1,232 |
| C9 | A scratch dumper that follows §5 | 0 `UNKNOWN(` in all 31 dumps; the shaders it lists per version and its 616 preshader blocks equal C8's counts |
| C10 | Implementations A and B (§5.1), written apart from this spec, on the 31 files; `web/test/mw-fx.test.mjs` | The 31 dumps are byte-identical (11,477 lines), with 0 `UNKNOWN(`; shader blocks per version and preshader blocks equal C8's per effect; the dumps' parameter, technique, pass, annotation, state, object and resource lines equal a separate count from the tables (533, 76, 87, 101, 1,648 of which 395 in sampler blocks, 313, 836). A and B also gave the same dump, or both refused, on 10,000 made-up effects that use every part of §2..§5 and on 9,300 corrupted copies of MW's; the rules for broken input in §5.4..§5.11 come from the cases where they first differed |

## 8. Not pinned down

- **The "shader count" header dword** (§2.5): it is not any sum or difference of two of twenty table counts in MW's
  files, and Wine's own comment calls it approximate. The dump prints it; nothing should depend on it until the
  oracle shows a use.
- **Matrix and scalar dimension order** (§2.3): rows first is Wine's and UG2's reading. MW's parameters only have
  scalars and square matrices; its 3- and 4-component state values are N × 1 where D3DX's table says 1 × N, which fits
  rows-first only if fxc writes such values as N × 1 matrices (§3.4).
- **Structs that mix numbers and objects** (§2.4): Wine does not advance its read position for numeric members, so it
  would read an object member's id from the start of the struct's value; the dump reads members back to back. Not in
  MW.
- **d3dx9_26's rows 89 and 102** (§3.5): whether its runtime uses the table's value (STENCILFAIL, BLENDOP) instead of
  D3D9's (CCW_STENCILFAIL, BLENDOPALPHA). Not used by MW.
- **The FXLC trailer** `0xF0F0F0F0 0x0F0F0F0F` (§4.3): constant in MW, read by neither Wine nor recomp-kit; its meaning
  is unknown.
- **PRSI** (§4.4): no source gives its layout and MW has none.
- **CTAB default values** (§4.2): their layout is not decoded; the dump prints only whether one is present.
- **`movc` (0x301)** comes from recomp-kit only; Wine's table does not have it. Not in MW.
- **The INT 1×1 TEXTURETRANSFORM value** (§3.4): how D3DX applies a state whose value type does not fit the table
  (M5-04/05, against the oracle).
