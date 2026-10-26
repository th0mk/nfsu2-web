# The effect model (ID3DXEffect HLE, part 0) and its canonical dump

Status: **2026-10-26**, design for M5-02 (parameters, annotations, CTAB matching, the effect pool) and M5-04 (the
state table as an API). Consumers: the two implementations of this contract (A in C, B in Python), M5-03 (the
preshader evaluator reads its inputs through §3), M5-05 (Begin/BeginPass/CommitChanges/EndPass/End, SetTexture),
M5-06 (the other vtable slots, GetDesc, GetParameterDesc), M5-09 (lockstep against the oracle).

`docs/mw/fx2-format.md` is the file format and the parser (`runtime/mw/fx2.h`); its section numbers are written
"F§n" here. This document is what D3DX 9 (`d3dx9_26.dll`, 9.07.239.0000, SDK version 26, June 2005) builds from a
parsed effect and what Most Wanted asks of it. Nothing here is game data: counts, addresses and the D3D9 facts the
state table needs. The effects and every dump stay under `out/mw/fx/` (L-006).

**Marks.** As in F§0, with one widening of **V**:

| Mark | Source |
|---|---|
| **V** | Verified on Most Wanted: on the 31 effects (`out/mw/fx/res/*.fxo`, through the M5-01 dumps or the scratch prototype of §7), or in `speed.exe`'s code (capstone over the image, base 0x400000), with the address |
| **D** | Read from `d3dx9_26.dll`'s data as bytes (pefile); none of its code was run or disassembled |
| **W** | Wine's d3dx9 (`dlls/d3dx9_36/effect.c`, `preshader.c`, `shader.c`, `d3dx9_private.h`, `include/d3dx9effect.h`, LGPL): facts only, no code |
| **K** | veritr1x recomp-kit `dx/d3dx9.cpp` (MIT) |
| **U** | UG2's port: `runtime/d3dx_hle.c` and its earlier D3DX notes, `D3DX_HLE.md` (not in this repository) |
| **S** | Microsoft's SDK headers and documentation (`d3d9types.h`, `d3dx9effect.h`, `d3dx9shader.h`) |

A rule marked only W or K is the contract until the oracle (M2-06's lifted DLL, M5-09's lockstep) says otherwise;
§8 lists the ones that matter.

## 1. What the game calls

### 1.1 The interface (d3dx9_26 = D3DX_SDK_VERSION 26)

ID3DXBaseEffect is slots 0..56 and ID3DXEffect adds 57..; the order is the SDK's (W, `d3dx9effect.h`): 0
QueryInterface, 1 AddRef, 2 Release, 3 GetDesc, 4 GetParameterDesc, 5 GetTechniqueDesc, 6 GetPassDesc, 7
GetFunctionDesc, 8 GetParameter, 9 GetParameterByName, 10 GetParameterBySemantic, 11 GetParameterElement, 12
GetTechnique, 13 GetTechniqueByName, 14 GetPass, 15 GetPassByName, 16 GetFunction, 17 GetFunctionByName, 18
GetAnnotation, 19 GetAnnotationByName, 20 SetValue, 21 GetValue, 22/23 Set/GetBool, 24/25 Set/GetBoolArray, 26/27
Set/GetInt, 28/29 Set/GetIntArray, 30/31 Set/GetFloat, 32/33 Set/GetFloatArray, 34/35 Set/GetVector, 36/37
Set/GetVectorArray, 38/39 Set/GetMatrix, 40/41 Set/GetMatrixArray, 42/43 Set/GetMatrixPointerArray, 44/45
Set/GetMatrixTranspose, 46/47 Set/GetMatrixTransposeArray, 48/49 Set/GetMatrixTransposePointerArray, 50/51
Set/GetString, 52/53 Set/GetTexture, 54 GetPixelShader, 55 GetVertexShader, 56 SetArrayRange; 57 GetPool, 58
SetTechnique, 59 GetCurrentTechnique, 60 ValidateTechnique, 61 FindNextValidTechnique, 62 IsParameterUsed, 63 Begin,
64 BeginPass, 65 CommitChanges, 66 EndPass, 67 End, 68 GetDevice, 69 OnLostDevice, 70 OnResetDevice, 71
SetStateManager, 72 GetStateManager, 73 BeginParameterBlock, 74 EndParameterBlock, 75 ApplyParameterBlock, 76
DeleteParameterBlock (only from version 26), 77 CloneEffect; SetRawValue comes only from version 27 (W). K's table
has the same order. ID3DXEffectPool has only the three IUnknown slots (W).

- **D** (inferred): the DLL holds exactly one run of 78 consecutive code pointers, at VA 0x413EA0, which fits
  ID3DXEffect with DeleteParameterBlock and without SetRawValue.
- **V**: every MW call below passes the argument count this layout gives (Begin two, BeginPass one, CommitChanges,
  EndPass and End none, GetAnnotationByName two, GetInt two, ...).
- U: UG2's statically linked D3DX is the 2004 layout (Set and Get for both shader types, BeginPass at 66 in
  `D3DX_HLE.md`'s table), so UG2's slot numbers do not carry over.

### 1.2 The slots MW uses (V)

Found by following every `call [vtable + n]` whose object comes from an effect wrapper's +0x48 (the effect pointer,
0x6C6085) in 0x6B8000..0x6E8000, then reading each site. GAME §5.4's heuristic list had three false hits and missed
three slots; this table replaces it.

| Slot | Method | Sites | Where |
|---|---|---|---|
| 2 | Release | 21 | 0x6C5DBF, 0x6D5C28, 0x6D63E6, 0x6C609D before a re-create, ... |
| 3 | GetDesc | 2 | 0x6C5512 (reads Parameters), 0x6D5D51 (reads Techniques) |
| 4 | GetParameterDesc | 1 | 0x6C553B (reads Semantic only) |
| 5 | GetTechniqueDesc | 2 | 0x6D5FC2, 0x6D75D4 (reads Passes) |
| 8 | GetParameter | 1 | 0x6C552A, parent NULL |
| 9 | GetParameterByName | 14 | 0x6D4DD7..0x6D4EDF (13), 0x6D5A5D; parent NULL. Five more are unreachable: 0x6BF241 (no caller, no pointer to it) and 0x6BF8CB..0x6BF9AF on the effect pointer [0x982B04], which nothing writes |
| 12 | GetTechnique | 6 | 0x6D5D69, 0x6D7578, 0x6D759C, 0x6E02B9, 0x6E02D1, 0x6E036C |
| 19 | GetAnnotationByName | 2 | 0x6D5D82, 0x6D5DAE, on technique handles |
| 26 | SetInt | 27 | 0x6C55EB, 0x6C5E79, 0x6D2ABA, ... |
| 27 | GetInt | 2 | 0x6D5D9D, 0x6D5DC1, on annotation handles |
| 28 | SetIntArray | 4 | 0x6C5620, 0x6C5ED2, 0x6C6AC6, 0x6C83BD |
| 30 | SetFloat | 29 | 0x6C55BB, 0x6C5B37, ... |
| 34 | SetVector | 36 | 0x6C567B, 0x6C62CC, ... |
| 36 | SetVectorArray | 5 | 0x6C56B0, 0x6D2BAD, 0x6D3203, 0x6D4003, 0x6D5A6E |
| 38 | SetMatrix | 12 | 0x6C56DB, 0x6C5A3C, ... (plus 3 unreachable on [0x982B04]) |
| 40 | SetMatrixArray | 2 | 0x6C5710, 0x6C7FF2 |
| 52 | SetTexture | 49 | 0x6C564B, 0x6C6116, ... (plus 1 unreachable, 0x6BF9BA) |
| 58 | SetTechnique | 3 | 0x6D75A9, 0x6E02DE, 0x6E0379 |
| 60 | ValidateTechnique | 1 | 0x6D5EB6 |
| 63 | Begin | 41 | 0x6C547E, 0x6D5A9F, ... |
| 64 | BeginPass | 23 | 0x6C54BB, 0x6D5AEE, ... |
| 65 | CommitChanges | 20 | 0x6C54D6, 0x6D5AE1, ... |
| 66 | EndPass | 22 | 0x6C54E6, 0x6D5B13, ... |
| 67 | End | 40 | 0x6C549E, 0x6D5B26, ... |
| 69 | OnLostDevice | 21 | 0x6DB0E8 (device lost, all 31), 0x6C6092 (before a re-create), 0x6D63D8 (after the probe, §1.3) |
| 70 | OnResetDevice | 2 | 0x6DB2A8 (reset, all 31); 0x6C5DFE (no caller found) |

Not used: 10 (GAME's hit 0x6AFA2A is a thiscall on another object), 16 (0x6BA169 and 0x6BBDCE are thiscalls), 51
(0x6C7617 is IDirect3DDevice9::SetLight), 11, 61 FindNextValidTechnique, 62, 71..77, and every Get* but GetInt. None
of the 168 Set*/Get* call sites the scan found pushes an immediate pointer as its handle, so MW never uses a name
as a handle.

### 1.3 Creation, the pool, lost and reset (V)

- **Wrappers.** 31 wrapper objects (0x50 bytes, constructor 0x6C3D20) live in the list at 0x93DE78; list index =
  effect id = row of the name table at 0x8F9BE8 (stride 0x90, +0 resource name). Each wrapper owns a table of 102
  entries of 40 bytes at [wrapper+0x44] (0x6C3D26: `new 0xFF0`): a semantic string (+0), its hash (+0x20) and an
  effect handle (+0x24), all handles 0 at first.
- **Create** (`0x6C6080`): OnLostDevice and Release of the old effect (0x6C6092, 0x6C609D), then
  `D3DXCreateEffectFromResourceA(device [0x982BDC], NULL, name, NULL, NULL, 0, pool [0x982C84], wrapper+0x48,
  &errors)` (0x6C60D2), then the handle table (0x6C5500) and the first values (0x6C5E60).
- **The pool** comes from `D3DXCreateEffectPool` (thunk 0x7E7D7C) twice. The probe (in 0x6D6000, call at 0x6D639D)
  makes a pool, initialises all 31 effects with the technique scan of §1.5 (0x6D63B8 → 0x6D5C10), then
  OnLostDevice and Release on all 31 and Release on the pool (0x6D63D0..0x6D640B): every pool slot dies there. The
  real one (0x6D7500, call at 0x6D7517, only while [0x982C84] is NULL) creates the missing effects and sets their
  techniques. A full rebuild releases all effects and the pool (0x6C5DA0, called at 0x6E724C and 0x6E7274) and runs
  0x6D7500 again (0x6E7251, 0x6E7279).
- **Lost and reset**: 0x6DB0D0 calls OnLostDevice on all 31; the reset path calls OnResetDevice on each and then runs
  0x6C5500 again (0x6DB2A8, 0x6DB2B0): the game fetches every handle again after a reset.

### 1.4 How parameters are found (V)

- **By semantic, at create and after reset** (0x6C5500): GetDesc; for i in 0..Parameters−1: `GetParameter(NULL, i)`
  (0x6C552A), `GetParameterDesc` (0x6C553B); when Semantic is not NULL the game hashes the string (0x460BF0) and
  stores the handle in the table entry with that hash (0x6C5557..0x6C5569). The model must give the semantic bytes
  exactly as stored (case kept). GetParameterBySemantic is never called.
- **By name, at (re)create** (0x6D4DC0, called at 0x6DC39C, 0x6E7260, 0x6E7288): 13 `GetParameterByName(NULL,
  name)` on effect 16, handles kept in a game object at +0x1FC..+0x230. All 13 names exist in effect 16.
- **By name, in a draw routine** (0x6D5A5D): one `GetParameterByName(NULL, name)` on effect 16 for a name that no
  effect holds, so the result is NULL, and the next call is `SetVectorArray(NULL, …, 16)` (0x6D5A6E). The model
  must answer that with D3DERR_INVALIDCALL and no side effect, every time.
- **Every setter** reads its handle from the wrapper's table ([wrapper+0x44] + 0x24 + 40·entry) or from the by-name
  handles. The draw-site setters test no handle for NULL (e.g. 0x6D2ABA SetInt(entry 9, 0xF)); an effect without
  the semantic gets NULL there. Entry 3 is BLENDSTATE: SetIntArray({0,0,0,5,6}, 5) on every new effect (0x6C5ED2).
- **Entry 9, COLORWRITEMODE**, is written through three fixed wrappers, not through whichever effect is current.
  [wrapper+4] is the effect id (0x6D5C40); the names are at 0x8F9BE8 + 0x90·id. The first values (0x6C5E60) write
  it (0xF, call at 0x6C600B) only for effect 11 (`cmp [esi+4], 0xB` at 0x6C5FF3), and test every other handle they
  use for NULL before the call (0x6C5E6C, 0x6C5E85).

| Wrapper (effect id) | SetInt sites (value) | ColorWriteMode there |
|---|---|---|
| [0x93DE78] (0) | 0x6CFF2D (7), 0x6E50FF (0 or 0xF), 0x6E51F4 (edi), 0x6E54B3 (7) | shared: reaches all 19 holders (§2.7) |
| [0x93DEC4] (19) | 0x6DED0C (7), 0x6DF421 (8), 0x6DF46F (7) | shared: the same slot |
| [0x93DEA4] (11) | 0x6D2ABA (0xF), 0x6D2D33 (0xF, or 8 by the byte [0x982C51]), 0x6D3E6C (0xF) | unshared: reaches no other effect |

### 1.5 How techniques are picked (V, 0x6D5C10 and 0x6D7500)

- For each technique t (GetDesc 0x6D5D51, GetTechnique(t) 0x6D5D69): `GetAnnotationByName(h, "shader")`
  (0x6D5D7B, string at 0x8AFB50) and, if found, `GetInt` (0x6D5D9D); the same for `"shadowLevel"` (0x8AFB44,
  0x6D5DAE/0x6D5DC1). A missing annotation reads as 0, which is how techniques without one count as fixed function
  (GAME §5.5).
- A cap at [0x8FAD24] (−1 in the file: none), vendor and device rules (0x6D5DDC..0x6D5EA9), then
  `ValidateTechnique(h)` (0x6D5EB6): any non-zero result drops the technique.
- Effects 0, 1, 3, 4, 5 and 18 (jump table 0x6D5FDC/0x6D5FE4) keep the best technique per shadowLevel (index at
  wrapper+0x18+4·level, its `shader` at +0x28+4·level, modes from [0x982C08]). The others keep the highest `shader`
  (index +0x0C, handle +0x10) and read GetTechniqueDesc's Passes into +0x14 (0x6D5FC2).
- Later the index is all that is kept: `SetTechnique(GetTechnique(index))` at 0x6D75A9, and again for every change
  of shadow level while drawing (0x6E02B9..0x6E0379). GetTechnique by index must be cheap; techniques are never
  looked up by name, and FindNextValidTechnique is not used.

### 1.6 Drawing (V)

- `Begin(&passes, flags)`: 10 sites push 0; the other 31 pass a register (0 at 0x6D5A98). `BeginPass(i)`,
  `CommitChanges`, `EndPass`, `End` follow. At 0x6D5AE1 the game calls CommitChanges **before** BeginPass (W:
  outside a pass it returns D3D_OK and applies nothing).
- Between them: SetTexture, SetInt, SetIntArray, SetFloat, SetVector, SetVectorArray, SetMatrix, SetMatrixArray.
- So the model must answer: top-level parameters by index with their semantic; names at the top level; technique
  annotations by name with an INT read; techniques by index; per-parameter storage with the eight setters above;
  NULL handles refused; per-technique shader versions for ValidateTechnique; and pool sharing that survives the
  re-creation of one effect.

## 2. The model API (`runtime/mw/fxmodel.h`)

### 2.1 Objects and ownership

- An `fxm_pool` is an `ID3DXEffectPool`'s shared storage. An `fxm_effect` is built from a parsed `fx2_effect` (which
  must outlive it) and an optional pool. Both are plain heap objects. M5-05 owns them and keeps the COM reference
  counts. The core has no globals and does no I/O. Every allocation is checked; creation fails only when memory
  runs out (unresolved items are recorded in the model and dumped as UNKNOWN, never refused or dropped silently).
  A's model also bounds its own memory at 1024 bytes per byte of the effect plus 16 MB and reports more as out of
  memory: only an effect that repeats huge names or arrays to blow up paths or CTAB leaves reaches it (none of
  MW's, none of the fuzz copies).
- In M5-05 the effect is parsed in place: the resource bytes are already in guest memory, and `fx2_str` pointers map
  back to guest addresses. GetParameterDesc can therefore return Name and Semantic as guest pointers into the
  resource (every name ends in NUL, F§2.2). MW only reads them (0x6C5547).
- Values cross the API as little-endian dwords (`uint32_t`); a float is its IEEE bits, so nothing is rounded in
  transit. Results are HRESULT-shaped: `FXM_OK` 0, `FXM_INVALIDCALL` 0x8876086C (D3DERR_INVALIDCALL), `FXM_FAIL`
  0x80004005 (E_FAIL), `FXM_OUTOFMEMORY` 0x8007000E, `FXM_NOTIMPL` 0x80004001.

### 2.2 Nodes and their ids

Every parameter, struct member, array element, annotation, technique and pass is a **node** with an id 1..N, stable
for the life of the effect and fixed by the file alone. Id 0 means "none". Ids are given in this order (the dump's
order, §5):

1. For each top-level parameter in file order: the parameter, then its subtree, then its annotations. The subtree
   comes before the annotations and is visited depth first: an array's elements 0..n−1; for a struct (or a struct
   element) its members in order, each with its own subtree.
2. For each technique in file order: the technique, its annotations, then for each pass the pass and its
   annotations.

Annotations get no child nodes (MW's are all INT scalars; an array or struct annotation is read whole, §8).

| Field | Meaning |
|---|---|
| kind | PARAM (top level), ELEMENT, MEMBER, ANNOT, TECH, PASS |
| parent | 0 for a parameter or technique; the array or struct for an element or member; the owner for an annotation; the technique for a pass |
| index | position under the parent (parameter, element, member, annotation, technique or pass number) |
| top | the top-level parameter whose storage holds it (itself for one); 0 for annotations, techniques, passes |
| name, sem | from its typedef. An element has its array's name and semantic (W). A name at offset 0 is no name (F§2.2): in a path it is empty (`""` for a parameter, `a.` for a member), and where names are compared with each other (the pool's match, §5.8's `apart`) or in the technique and pass lookups it equals the empty string and nothing else. A parameter or annotation lookup by `""` finds nothing, and one by NULL finds no child (§2.3, W), so an unnamed parameter or annotation is found by no name, an unnamed member only by its full path (`a.`). W would read the name record at body +0 there (`d3dx9_parse_name(data + offset)`) and compare NULL names with `strcmp`; MW has no unnamed parameter, annotation, technique or pass (V). A name record of size 0 at another offset is the empty string here (W: NULL; §8) |
| path | full name: `a`, `a.b` (member), `a[3]` (element, decimal), `a[3].b`; techniques, passes and annotations: their name |
| cls, type, rows, cols, elements, members | D3DXPARAMETER_DESC's fields; an element has elements 0 |
| bytes | §2.4 |
| flags | the parameter's file flags (F§2.5.1) for a parameter and all its elements and members (W); 4 (D3DX_PARAMETER_ANNOTATION) for an annotation (W) |
| annotations | count; non-zero only for top-level parameters, techniques and passes (W) |
| object | for an object element that is not a sampler: the file's object id (F§2.7) |
| block | for a sampler leaf: its `sampler_state` block (`fx2_block`) |

MW: 951 nodes (533 parameters, 154 elements, 0 members, 101 annotations, 76 techniques, 87 passes); no duplicate
names and no two semantics that differ only in case within one effect (V).

### 2.3 Lookups (W unless marked)

| Call | D3DX method | Rule |
|---|---|---|
| `fxm_param(e, parent, i)` | GetParameter | parent 0: top-level parameter i. A struct (elements 0): member i. An array: 0 |
| `fxm_element(e, parent, i)` | GetParameterElement | parent 0: top-level parameter i. A node with elements: element i (i < elements). Otherwise 0 |
| `fxm_by_name(e, parent, s, n)` | GetParameterByName | s `""` (n 0): 0, whatever the parent (W `get_parameter_by_name`: `if (!name \|\| !*name) return NULL`). s NULL: the parent itself when it is a parameter, element, member or annotation (W's GetParameterByName returns the parent's handle before it looks at a name), otherwise 0 (parent 0 included). Parent 0: the first parameter, element or member whose **path** equals the n bytes exactly (case kept), so `a`, `a.b`, `a[3]`, `a[3].b` work and `a[03]`, `a[ 3]`, `a[]` and `a[9]` (out of range) do not. A parent that is a parameter, element or member: the path `<parent path>.<s>`. Any other parent: 0 |
| `fxm_by_semantic(e, parent, s, n)` | GetParameterBySemantic | parent 0: the first top-level parameter whose semantic equals s with ASCII letters compared case-insensitively (stricmp, C locale; bytes ≥ 0x80 compared as they are). A struct parent: its direct members only. An array of structs: its first min(member count, elements) elements (W loops over the member count of the parent's `members[]`, which hold an array's elements, and reads past them when there are fewer; the model stops there); each has the array's semantic, so that gives element 0 or nothing. Any other parent: 0. s NULL: the first one without a semantic. Not used by MW |
| `fxm_annot(e, owner, i)`, `fxm_annot_by_name(e, owner, s, n)` | GetAnnotation, GetAnnotationByName | owner: a top-level parameter, technique or pass. By name: exact bytes (strcmp), first match; s `""` or NULL: 0 (W) |
| `fxm_tech(e, i)`, `fxm_tech_by_name(e, s, n)` | GetTechnique, GetTechniqueByName | index, or exact name (strcmp), first match; a name at offset 0 matches `""` (§2.2); s NULL: 0 (W `get_technique_by_name`) |
| `fxm_pass(e, tech, i)`, `fxm_pass_by_name(e, tech, s, n)` | GetPass, GetPassByName | index, or exact name as for techniques; s NULL: 0 (W) |

`n` counts the bytes of `s` (no NUL needed; ignored when s is NULL); a stored name matches only when it has the same
length. All index lookups are O(1). `fxm_by_name` with parent 0 is called while drawing (0x6D5A5D), so it must not
scan (hash or sorted table).

W's name tree also holds each top-level parameter's annotations under `<path>@<name>` (and their elements and members
below that), put in right after the parameter (`add_param_to_tree`), so a lookup with parent 0, a CTAB name or a
usage-1 name `p@k` finds annotation k there. The model has parameter, element and member paths only, so `p@k` finds
nothing (§8). No MW name contains `@` (V).

### 2.4 Storage and sizes

- **Bytes** (W): a numeric node (classes SCALAR..MATRIX_COLUMNS) takes 4 × rows × cols per element; an object of
  type STRING, TEXTURE..TEXTURECUBE, PIXELSHADER or VERTEXSHADER takes 4 per element (a guest pointer); a sampler
  takes 0, and so does an object of any other type (VOID, BOOL, INT, FLOAT or a type past those: W gives it no
  storage and reads no object id, so it has no value, Set*/Get* move no bytes and it is no dep, §3.5; the node
  still shows the file's id dword as `object`); a struct takes the sum of its members per element; an array takes
  elements × the element's bytes. D3DXPARAMETER_DESC.Bytes is this.
- **Layout**: one block of `bytes` per top-level parameter (in its pool slot when it is shared, §2.7). Element e sits
  at e × element bytes, and members follow one another in order. Numeric dwords are in file order (F§2.4: row-major
  for MATRIX_ROWS, so `data[i·cols + k]` is row i, column k). An object dword holds the guest object pointer, 0 at
  creation: MW's texture objects have no data (F§2.5.6), and shader objects are M5-05's (state values, never
  parameters in MW).
- **Initial values**: the file's default values (F§2.4); for a parameter that joins a live pool slot, the slot's
  current value (§2.7).
- `fxm_data(e, id)` returns the node's dwords (read only; `bytes/4` of them). M5-03 and M5-05 read constants and
  preshader inputs from here.

### 2.5 Set and Get (W; MW uses the rows in bold)

W checks shapes, not types (every guard below is W's, from `effect.c`): a numeric call takes any type in a
numeric class. Shape words: *numeric class* = SCALAR, VECTOR, MATRIX_ROWS or MATRIX_COLUMNS; *1×1* = rows = cols = 1
and elements 0, whatever the type (only the numeric classes have rows and columns, F§2.3). `conv(to, from, w)` is
D3DX's number conversion (W `set_number` with `get_bool`, `get_int`, `get_float`): w as it is when to = from; else 0
when the target is not BOOL, INT or FLOAT; else a VOID source is read as its raw bits and any other source that is
not BOOL, INT or FLOAT gives 0; else:

| from \ to | BOOL | INT | FLOAT |
|---|---|---|---|
| BOOL | w unchanged (copied, not normalised) | w ≠ 0 ? 1 : 0 | w ≠ 0 ? 1.0 : 0.0 |
| INT | w ≠ 0 ? 1 : 0 | w | (float)(int32)w |
| FLOAT | w ≠ 0 (bit test: −0.0 is TRUE) | truncation toward zero; NaN or \|x\| ≥ 2³¹ gives 0x80000000 (x86's integer indefinite; §8) | w |

| Call | Accepts | Writes | Version (§2.6) |
|---|---|---|---|
| SetValue | anything but a sampler object (class OBJECT with a sampler type: E_FAIL); `bytes ≥` node bytes | node bytes, raw (BOOL not normalised), when the node's own type is VOID, BOOL, INT, FLOAT, STRING or a texture type; any other type (PIXELSHADER, VERTEXSHADER, a sampler type outside class OBJECT, a type past the table) writes nothing (W's `set_value` ends in a FIXME there) and the version still rises. For object types M5-05 does AddRef/Release before the copy, reading the old pointers with GetValue | always |
| GetValue | same | node bytes; when the node's **own** type is BOOL, every dword normalised to 0/1 (W switches on the parameter's type: a struct's BOOL members come back raw through the struct, normalised through the member) | – |
| SetBool / GetBool | 1×1 | conv(type, BOOL, b) / conv(BOOL, type, w), so a BOOL comes back raw (SetBool(5), GetBool → 5) | always |
| **SetInt** | 1×1 | conv(type, INT, n) | **only if the bytes change** |
| SetInt, colour form | not 1×1, elements 0, type FLOAT, and class VECTOR with cols ≠ 2 or class MATRIX_ROWS with cols = 1 and rows ≠ 2 (W's "vector fixup"; MW: float3 and float4) | components 0, 1, 2, 3 ← bytes 2, 1, 0, 3 of n × (1/255), the first min(rows × cols, 4) of them (W writes 3, and the 4th when rows × cols > 3, even past a smaller storage) | always |
| **GetInt** | 1×1: conv(INT, type, w). Colour form: the reverse, each of the first min(rows × cols, 4) components clamped to [0,1], × 255, truncated, packed as bytes 2,1,0(,3); a component past them packs as 0 | – | – |
| **SetFloat** / GetFloat | 1×1 | conv(type, FLOAT, f) / conv(FLOAT, type, w) | **only if the bytes change** |
| SetBoolArray | class SCALAR, VECTOR or MATRIX_ROWS (any type) | k = min(count, bytes/4) dwords in storage order (across elements), each conv(type, **INT**, src) (D3DX does not crop BOOL input) | always |
| **SetIntArray**, SetFloatArray | same | k dwords, conv(type, INT or FLOAT, src) | **always** |
| Get{Bool,Int,Float}Array | also MATRIX_COLUMNS; a NULL pointer is refused whatever the count (W tests it first, with no count-0 shortcut) | k dwords, conv(BOOL/INT/FLOAT, type, w) (a BOOL comes back raw) | – |
| **SetVector** | elements 0, class SCALAR or VECTOR (any type) | INT 1×1: x, y, z, w each clamped to [0,1] × 255, truncated, packed into bits 16, 8, 0, 24. FLOAT: the first min(cols, 4) components, raw. Otherwise conv(type, FLOAT, ·) on them. Never past the storage (W writes `cols` components) | **always** |
| GetVector | same | INT 1×1: bytes 2,1,0,3 × (1/255), no clamp. Otherwise min(cols, 4) components (within the storage) as floats, the rest 0.0 | – |
| **SetVectorArray** | class VECTOR, an array (elements > 0, W) with elements ≥ count | count elements, the first min(cols, 4) components of each source float4 (FLOAT raw, otherwise conv), element stride rows × cols | **always** |
| GetVectorArray | count 0: FXM_OK whatever the id (W returns before it looks at the handle). Otherwise as SetVectorArray | as GetVector per element | – |
| **SetMatrix** / GetMatrix | elements 0, class MATRIX_ROWS (any type) | the top-left min(rows, 4) × min(cols, 4) of the row-major 4×4 m: `data[i·cols+k] = m[i][k]` (FLOAT raw, otherwise conv). Get fills the other cells with 0.0 | **always** |
| SetMatrixTranspose / GetMatrixTranspose | as SetMatrix; Get also takes SCALAR and VECTOR, which come back **not** transposed, in row 0 (W calls `get_matrix(…, FALSE)` for them) | `data[i·cols+k] = m[k][i]` | always |
| **SetMatrixArray**, SetMatrixTransposeArray | class MATRIX_ROWS with elements ≥ count, so a non-array takes count 0 (W: OK, and the version rises) | count elements, as above | **always** |
| GetMatrixArray, GetMatrixTransposeArray | count 0: FXM_OK whatever the id (W). Otherwise class MATRIX_ROWS with elements ≥ count | as GetMatrix per element | – |
| **SetTexture** / GetTexture | a texture type (TEXTURE, TEXTURE1D/2D/3D/CUBE) with elements 0 (a non-array parameter or one element); W checks no class | the guest pointer; returns the old one so M5-05 can Release it | **only if it changes** |
| SetString / GetString | type STRING. Set takes any class and element count (W checks the type only: an array takes the pointer in its first dword); Get needs elements 0 | the guest pointer (M5-05 makes the guest copy) | always |

- SetTexture, SetString and their Gets refuse a node with less than 4 bytes of storage (a malformed numeric-class
  node of a texture or string type; W would write past it).
- Everything else returns FXM_INVALIDCALL and changes nothing: id 0 or out of range, a technique or pass, a shape
  the row does not accept. A Set* on an annotation returns FXM_INVALIDCALL as well (annotations are read only here;
  §8). Every Get* also accepts annotations: a numeric or struct annotation reads its file value; an object
  annotation (STRING, texture) reads 0 here, where W returns the string pointer it made at load. Before M5-05/06
  expose GetString or GetValue on annotations, they must give those annotations guest pointers (the model has no
  setter for annotations).
- Every setter also reports `changed` (1 when any stored byte differs afterwards), separate from the version rule,
  for M5-05's memo.
- MW calls (V, §1.2): SetInt, SetIntArray, SetFloat, SetVector, SetVectorArray, SetMatrix, SetMatrixArray,
  SetTexture, and GetInt on annotations. Those rules are the ones M5-09 must confirm first. M5-05 logs the first
  non-OK result per (effect, node, method), so a MW call that a rule refuses shows up in the first run (the NULL
  handle of §1.4 included, once).

### 2.6 Change counters (for CommitChanges) and the verify hook

- Each top-level storage block (each pool slot for a shared parameter) has a 64-bit **version**. Versions come from
  one counter: the pool's when the effect has a pool, otherwise the effect's own (W). A write that the table above
  marks as a change sets the block's version to `++counter`. At creation each top-level parameter, in parameter
  order, sets its block's version to `++counter`: a block of its own, a slot it opens, and a live slot it joins
  alike (W: `d3dx_parse_effect` runs `d3dx_pool_sync_shared_parameter` and then `set_dirty`, which sets the shared
  data's `update_version` to `++counter`), so joining dirties that slot for every holder.
- A creation refused before its storage is made (§2.1's bound, or memory running out in the CTAB match or the deps)
  leaves every slot, every version and the counter as they were: the model makes the storage and joins the pool last
  (W syncs the pool at the end of `d3dx_parse_effect`, after every resource and its CTAB match).
- `fxm_version(e, id)` is the version of the node's block; `fxm_clock(e)` is the counter. M5-05 stamps a pass with
  the clock when it applies it. Each state has its own dirty test, and so does each constant and each sampler state
  a shader applies (W: `is_param_dirty` per state in `d3dx9_get_param_value_ptr`, per constant set in
  `set_constants`, per sampler state through `d3dx9_apply_state`): a state is dirty when one of **its own** inputs
  has a newer version. Those inputs come from `fxm_state_source` (the usage-1 parameter, or the matched constants of
  its program, `fxm_prog` consts) and from the sampler blocks those name; §3.5's `deps` is only their union over the
  pass, a filter (no dep newer than the stamp: nothing in the pass is dirty), never the input list of one state, so
  a test on `deps` alone would re-apply clean states (U's lesson below). Since a write through any holder raises the
  shared block, a change made through one effect dirties the passes of every effect in the pool.
- U (UG2's lesson, L-084): BeginPass in UG2 re-issued its device calls even for clean states; CommitChanges applied
  a dirty list that included passes other than the active one; memo stage 3 (skipping whole passes) was wrong in
  78 % of verified hits. So the model **caches nothing derived**: no conversion results, no evaluated preshaders, no
  "clean" flags of its own. Every memo lives in M5-05 and is keyed on these versions, behind a run-time switch
  `MWFXVERIFY=<n>`: on every n-th memo hit, the HLE also computes the result from the model and compares (device
  calls and values). The model's part of the hook is that versions never go back, never repeat within a pool, and
  rise on every write that D3DX treats as a change.

### 2.7 The effect pool

- **Which parameters**: top-level parameters with flag bit 0 (`D3DX_PARAMETER_SHARED`, F§2.5.1) whose type is not a
  sampler (W: samplers are never shared), created with a pool.
- **Matching** (W, `is_same_parameter`): a live slot (one holder or more) whose first parameter has the same name
  (strcmp; a name at offset 0 is the empty string, §2.2) and the same class, type, rows, columns, element count and
  member count, recursively over members (their names included).
  Semantic, flags and values are not compared. A match points the new parameter at the slot's storage and discards
  its own default; no match opens a new slot, seeded with this parameter's default. A mismatch is not an error: it
  is simply another slot.
- **Slot numbers** are the model's convention and the dump's numbering (§5.8): a new slot takes the lowest free
  number. W takes the last free entry of its table instead (its free-entry loop has no break; the table starts at
  16 entries and doubles), and no D3DX call can observe the number.
- **Lifetime** (W): freeing an effect removes it from its slots. A slot keeps its value while it has a holder; when
  the last holder goes, the slot is free and its value is gone (a later effect seeds it again from its own default).
  `fxm_effect_free(e, drop, ctx)` calls `drop(ctx, ptr)` for every non-zero object pointer whose storage dies with
  the effect (its unshared object parameters, and slots it was the last holder of), so M5-05 can Release them. An
  effect keeps its pool alive (W); `fxm_pool_free` is called only once no effect uses the pool.
- **MW** (V): 20 shared names (all non-array except one INT[5]; 3 TEXTURE, the rest INT and FLOAT
  scalars and vectors; no matrix, no sampler); every same-named shared parameter has the same typedef and the same
  default in every holder, so the creation order changes no value. Holders per slot: 27, 23, 19, seven slots with
  7, five with 6, one with 3, one with 2, three with 1 (156 shared top-level parameters). The two GAME §5.5 names:
  `ColorWriteMode` (semantic COLORWRITEMODE, INT, default 0) is shared in 19 effects and unshared in effect 11;
  `BlendState` (semantic BLENDSTATE, INT[5], default 0 0 0 0 0) is shared in 23 and unshared in effects 11, 12
  and 13. So "reaches every effect" means every effect that holds it shared; the game writes ColorWriteMode through
  effects 0 and 19, two of its holders, and through effect 11's own copy (§1.4). Nine top-level parameters share a
  name with a slot but not the flag: those 4, two FLOAT scalars and three textures of effect 3 (whose shared twins
  exist only in effect 1). A shared texture with one holder still goes through the slot.
- **Re-creating one effect** (0x6C6080, §1.3) makes it join the live slots (raising their versions, §2.6) and take
  their current values, not its defaults; the game then writes BLENDSTATE and its other first values through it
  (0x6C5E60), and a write to a shared one reaches every holder.

### 2.8 Guest handles (design for M5-05/06; not built here)

- Each effect gets from the HLE a guest block of 4 × (N + 1) bytes that the game never sees as data (allocated from
  the guest heap, freed at the effect's last Release). The guest handle of node id is `base + 4·id`, so it is never
  0 and never `base`.
- Resolving a parameter handle h, in Wine's order (pointer first, then string; W):
  1. h = 0: none (Set*/Get* return D3DERR_INVALIDCALL; GetParameter and friends take it as "top level").
  2. `base < h ≤ base + 4N` and `(h − base) % 4 = 0`: node `(h − base)/4`, which must be of a kind the method takes.
  3. h inside another live effect's block: refused (D3DERR_INVALIDCALL) and logged once. Wine would accept it
     (its handles are tagged pointers that work on any effect); MW never does it, since every handle it uses comes
     from the same effect's table (§1.4).
  4. Otherwise h is a guest `char *`: a bounded read (the effect's longest path plus 1 is enough, since nothing
     longer can match) and `fxm_by_name(e, 0, …)`. W skips this with D3DXFX_LARGEADDRESSAWARE; MW passes flags 0
     (0x6C60BC).
- Techniques: a TECH node, otherwise the string path through `fxm_tech_by_name` (W). Passes: PASS nodes only, no
  string (W). Annotation owners: a parameter, technique or pass node.
- After OnResetDevice the game fetches every handle again (§1.3) and expects the same answers: handles depend only
  on node ids and the block, never on device state.
- U: UG2's 2004 D3DX made a handle from the bitwise NOT of its record address; how d3dx9_26 forms handles is not
  known and does not matter, since MW never inspects handle values.

### 2.9 Declarations

```c
/* fxmodel.h -- what D3DX 9 builds from a parsed fx_2_0 effect (fx2.h):
 * parameters, members, elements, annotations, techniques and passes as
 * nodes with stable ids, D3DX's lookups, value storage with its Set/Get
 * conversions and change counters, the effect pool's shared parameters,
 * and the CTAB match of every shader and preshader.  docs/mw/fx-model.md
 * is the contract and the section numbers are its.  No globals, no I/O. */
#ifndef MW_FXMODEL_H
#define MW_FXMODEL_H
#include "fx2.h"

#define FXM_OK           0x00000000u
#define FXM_INVALIDCALL  0x8876086Cu   /* D3DERR_INVALIDCALL */
#define FXM_FAIL         0x80004005u   /* E_FAIL */
#define FXM_NOTIMPL      0x80004001u
#define FXM_OUTOFMEMORY  0x8007000Eu
#define FXM_SHARED       1u            /* D3DX_PARAMETER_SHARED */
#define FXM_ANNOTATION   4u            /* D3DX_PARAMETER_ANNOTATION */

enum { FXM_K_PARAM = 1, FXM_K_ELEMENT, FXM_K_MEMBER, FXM_K_ANNOT, FXM_K_TECH, FXM_K_PASS };

typedef struct fxm_pool fxm_pool;
typedef struct fxm_effect fxm_effect;

typedef struct {                 /* a node (§2.2) */
    uint32_t kind, parent, index, top;
    fx2_str name, sem, path;
    uint32_t cls, type, rows, cols, elements, members;
    uint32_t bytes, flags, nannots;
    uint32_t object;             /* object element: the file's object id */
    const fx2_type *t;           /* its typedef (an element: its array's) */
    const fx2_block *block;      /* sampler leaf: its sampler_state block */
} fxm_node;

fxm_pool *fxm_pool_new(void);
void      fxm_pool_free(fxm_pool *p);          /* no effect may use it any more */
uint32_t  fxm_pool_slots(const fxm_pool *p);   /* slot numbers handed out so far */

/* 0, or -1 with a message (out of memory only). fx must outlive the effect. */
int  fxm_effect_new(fxm_effect **out, const fx2_effect *fx, fxm_pool *pool, char *err, uint32_t errlen);
void fxm_effect_free(fxm_effect *e, void (*drop)(void *ctx, uint32_t ptr), void *ctx);

uint32_t fxm_nodes(const fxm_effect *e);                         /* N: ids 1..N */
const fxm_node *fxm_node_get(const fxm_effect *e, uint32_t id);  /* NULL for 0 or > N */
uint32_t fxm_shared_slot(const fxm_effect *e, uint32_t id);      /* top-level id: slot + 1, 0 = not shared */

/* §2.3 */
uint32_t fxm_param(const fxm_effect *e, uint32_t parent, uint32_t index);
uint32_t fxm_element(const fxm_effect *e, uint32_t parent, uint32_t index);
uint32_t fxm_by_name(const fxm_effect *e, uint32_t parent, const uint8_t *s, uint32_t n);
uint32_t fxm_by_semantic(const fxm_effect *e, uint32_t parent, const uint8_t *s, uint32_t n);  /* s NULL: none */
uint32_t fxm_annot(const fxm_effect *e, uint32_t owner, uint32_t index);
uint32_t fxm_annot_by_name(const fxm_effect *e, uint32_t owner, const uint8_t *s, uint32_t n);
uint32_t fxm_tech(const fxm_effect *e, uint32_t index);
uint32_t fxm_tech_by_name(const fxm_effect *e, const uint8_t *s, uint32_t n);
uint32_t fxm_pass(const fxm_effect *e, uint32_t tech, uint32_t index);
uint32_t fxm_pass_by_name(const fxm_effect *e, uint32_t tech, const uint8_t *s, uint32_t n);

/* §2.5: FXM_OK or an error; changed (may be NULL) is 1 when a stored byte differs */
uint32_t fxm_set_value(fxm_effect *e, uint32_t id, const uint32_t *src, uint32_t bytes, int *changed);
uint32_t fxm_get_value(const fxm_effect *e, uint32_t id, uint32_t *dst, uint32_t bytes);
uint32_t fxm_set_bool(fxm_effect *e, uint32_t id, uint32_t b, int *changed);
uint32_t fxm_get_bool(const fxm_effect *e, uint32_t id, uint32_t *b);
uint32_t fxm_set_int(fxm_effect *e, uint32_t id, uint32_t n, int *changed);
uint32_t fxm_get_int(const fxm_effect *e, uint32_t id, uint32_t *n);
uint32_t fxm_set_float(fxm_effect *e, uint32_t id, uint32_t f, int *changed);
uint32_t fxm_get_float(const fxm_effect *e, uint32_t id, uint32_t *f);
/* type FX2_T_BOOL, FX2_T_INT or FX2_T_FLOAT: Set/Get{Bool,Int,Float}Array */
uint32_t fxm_set_array(fxm_effect *e, uint32_t id, uint32_t type, const uint32_t *src, uint32_t count, int *changed);
uint32_t fxm_get_array(const fxm_effect *e, uint32_t id, uint32_t type, uint32_t *dst, uint32_t count);
uint32_t fxm_set_vector(fxm_effect *e, uint32_t id, const uint32_t v[4], int *changed);
uint32_t fxm_get_vector(const fxm_effect *e, uint32_t id, uint32_t v[4]);
uint32_t fxm_set_vector_array(fxm_effect *e, uint32_t id, const uint32_t *v, uint32_t count, int *changed);
uint32_t fxm_get_vector_array(const fxm_effect *e, uint32_t id, uint32_t *v, uint32_t count);
uint32_t fxm_set_matrix(fxm_effect *e, uint32_t id, const uint32_t m[16], int transpose, int *changed);
uint32_t fxm_get_matrix(const fxm_effect *e, uint32_t id, uint32_t m[16], int transpose);
uint32_t fxm_set_matrix_array(fxm_effect *e, uint32_t id, const uint32_t *m, uint32_t count, int transpose,
                              int *changed);
uint32_t fxm_get_matrix_array(const fxm_effect *e, uint32_t id, uint32_t *m, uint32_t count, int transpose);
/* SetTexture, SetString: *old (may be NULL) gets the previous pointer */
uint32_t fxm_set_object(fxm_effect *e, uint32_t id, uint32_t ptr, uint32_t *old, int *changed);
uint32_t fxm_get_object(const fxm_effect *e, uint32_t id, uint32_t *ptr);

const uint32_t *fxm_data(const fxm_effect *e, uint32_t id);      /* §2.4; NULL when it has no storage */
uint64_t fxm_version(const fxm_effect *e, uint32_t id);          /* §2.6 */
uint64_t fxm_clock(const fxm_effect *e);

/* §3 */
enum { FXM_L_SCALAR, FXM_L_VECTOR, FXM_L_ROWS, FXM_L_COLUMNS, FXM_L_SAMPLER, FXM_L_STRUCT };
typedef struct {                 /* one register range of a matched constant (§3.3) */
    uint32_t id;                 /* the leaf node */
    uint32_t reg, count;         /* first register and count (0: nothing uploaded) */
    uint32_t layout;             /* FXM_L_SCALAR .. FXM_L_COLUMNS */
} fxm_leaf;
typedef struct {                 /* one CTAB constant (§3.2) */
    uint32_t k;
    fx2_const c;                 /* the entry, as fx2_ctab_const reads it */
    fx2_str name;
    uint32_t why;                /* 0, or the reason of §3.2 */
    uint32_t id;                 /* the matched top-level parameter */
    uint32_t from, to;           /* FX2_T_BOOL/INT/FLOAT: the parameter's type, the register set's */
    uint32_t layout, per;        /* §3.3 */
    uint32_t nelem, used;        /* elements (1 for a non-array), those with registers */
    uint32_t stage;              /* sampler: the first sampler stage */
    uint32_t nleaves;
    const fxm_leaf *leaves;      /* every leaf in register order, used or not */
} fxm_const;
enum { FXM_P_SHADER, FXM_P_PRESHADER };
typedef struct {                 /* a shader or preshader resource with its matched CTAB */
    uint32_t kind;               /* FXM_P_* */
    const fx2_res *res;
    uint32_t version;            /* its version token */
    int ctab;                    /* a CTAB was found with its 28-byte header, Size 28 (§3.1) */
    uint32_t nconsts;            /* the CTAB's count, as stored */
    uint32_t nread;              /* entries read (fewer when one lies outside: reason 6) */
    const fxm_const *consts;
} fxm_prog;
enum { FXM_S_LITERAL, FXM_S_PRESHADER, FXM_S_SHADER, FXM_S_PARAM, FXM_S_UNKNOWN };
typedef struct {                 /* where a state's value comes from (§3.5) */
    uint32_t src;                /* FXM_S_* */
    uint32_t id;                 /* FXM_S_PARAM: the named parameter (0: not found) */
    const fxm_prog *prog;        /* FXM_S_PRESHADER, FXM_S_SHADER */
    const fx2_res *res;          /* the resource that decides: the last one (W); NULL for a literal */
    uint32_t nres;               /* resources on the state (1 in MW) */
} fxm_source;
fxm_source fxm_state_source(const fxm_effect *e, const fx2_state *s);
uint32_t fxm_pass_deps(const fxm_effect *e, uint32_t pass, const uint32_t **ids);   /* sorted, unique */
void     fxm_tech_versions(const fxm_effect *e, uint32_t tech, uint32_t *vs, uint32_t *ps);   /* 0: none */
const fxm_prog *fxm_progs(const fxm_effect *e, uint32_t *n);     /* every program in resource order (tests, tools) */
#endif
```

Pass states are reached through the PASS node (`fx2_pass` of the parsed tree), sampler-block states through a
sampler leaf's `block`; `fxm_state_source` answers for any state of either kind.

## 3. CTAB matching

### 3.1 The programs

Every resource with usage 0 (F§2.5.7) whose target state is a VERTEXSHADER or PIXELSHADER value is a **shader**
program; one whose target value is BOOL, INT, FLOAT or STRING is a **preshader** (FXLC). This covers pass states and
the states of every sampler block. The CTAB is the first comment with FOURCC `CTAB` (F§4.2): for a shader, the
first one in the token walk (F§5.9); for a preshader, `fx2_pres.ctab`. That is the model's rule: W searches
otherwise and differs on odd input (§8); on MW's programs both find the same comment (V). There is none when no
such comment exists, its payload is under 28 bytes or its header's Size field is not 28 (W:
`D3DXGetShaderConstantTableEx` refuses the table, and `get_constants_desc` then gives the program no inputs). MW
(V): 758 programs (142 shaders, 616 preshaders); 140 shader CTABs and 616 preshader CTABs, every Size 28; two
ps_1_1 shaders have none.

A shader program is **applied** when its state's row, its state's type and its version token agree: row 146
(VERTEXSHADER), type VERTEXSHADER and token high word 0xFFFE, or row 147 (PIXELSHADER), type PIXELSHADER and 0xFFFF.
W creates the code by the type (`d3dx9_create_object`) and uploads its constants and applies its sampler
constants' states from those two rows only, as a vertex shader on row 146 and a pixel shader on row 147
(`d3dx9_apply_state`, `d3dx_set_shader_constants`). Any other program W does not apply as a shader: another row
takes the object as its value, code of the wrong kind for its type is refused by the device (the state then sets a
NULL shader and uploads nothing), and an object of the other type would be handed to the other Set*Shader, which
the model does not follow (§8). Its CTAB is matched all the same (W matches it at load, §3.2). A preshader is
evaluated on any row. MW (V): every shader is applied.

A shader held in a VERTEXSHADER or PIXELSHADER **parameter** (its object data) is not a program here. W builds and
matches one when a usage-1 reference names that parameter (`VertexShader = <vsp>`, `d3dx_parse_resource`), uploads its
constants and applies its sampler constants' states from that state; the model gives the state `FXM_S_PARAM` only
(§3.5, §8). Nor is a `PRES` preshader inside a shader (F§4.4) modelled. MW has neither (V: no shader-typed parameter,
no PRES comment).

### 3.2 Matching a constant (W, `get_constants_desc`)

For constant k (F§4.2 entry, read with `fx2_ctab_const`), in this order; the first rule that fails gives the
reason, which the dump prints as UNKNOWN (§5.7) and M5-05 logs once per effect:

| Step | Rule | Reason when it fails |
|---|---|---|
| 1 | The entry lies inside the table (`fx2_ctab_const` = 0). Otherwise the list ends here | 6 |
| 2 | Its name is the path of a **top-level** parameter (exact bytes; W looks it up with no parent, and a matched node that is not top level is an error). A name at offset 0 or `""` matches nothing (§2.3) | 1 |
| 3 | Its TypeInfo lies inside the table (`typed`) | 5 |
| 4 | Register set s (3), shaders only: the parameter is an OBJECT of a sampler type, the constant's class is OBJECT, and its count cut to the constant's own elements, min(count, max(1, Elements)) (W's RegisterCount, §3.4), is ≤ max(1, elements) (W's object branch). Sets b, i, c (0..2): the parameter has a numeric class and type BOOL, INT or FLOAT and the constant a numeric class (SCALAR..MATRIX_COLUMNS: W refuses a class-OBJECT constant outside set s, and §3.3's registers need one), or the parameter is a struct (its members are step 6's). In a preshader only set c is accepted. Any other set or pairing | 2 |
| 5 | Not for a sampler (W's object branch checks no element count). Elements: an array parameter needs the constant's Elements equal to its element count, any other parameter Elements 0 or 1 (W `init_set_constants_param`); and StructMembers must equal the parameter's member count, 0 for a numeric parameter (W compares them for a parameter that is not an array and for each element of one) | 4 |
| 6 | A struct parameter (members matched by position, W) | 7 (not built: no struct in MW) |
| 7 | Size, numeric only: the constant's rows and columns each equal the parameter's (a VECTOR is 1 × cols in both; MATRIX_COLUMNS in the CTAB against MATRIX_ROWS in the effect is the usual pair, F§4.2). The model's rule: W compares no shape (§8) | 3 |

W skips a constant whose name matches no parameter (reason 1's first case) with a warning. Two pairings that get
reason 2 here W accepts: an OBJECT-class parameter (sampler, texture, string, shader) under a constant whose class is
not OBJECT, in any register set, and a sampler under a set-s OBJECT constant in a preshader. Both become inputs
without registers (`get_constants_desc` skips `init_set_constants_param` for an OBJECT-class parameter), and for a
sampler in a shader W then applies its sampler states at stages RegisterIndex + i for i < RegisterCount
(`d3dx_set_shader_constants`); the model binds nothing and adds no deps (§8). The other failures of steps 2 (a match
that is not top level), 4 and 5 make W refuse the whole effect (D3DERR_INVALIDCALL or E_FAIL from
`get_constants_desc`); sets b and i in a preshader, structs and constants of another shape are §8's. Here every unmatched
constant is kept with its reason and shows in the dump (§8). Reasons 4 and 7 are reachable for struct parameters
because step 4 lets them through. MW (V): all 1,355 constants match (reason 0 throughout).

### 3.3 Registers, conversion and layout (W, `shader.c`, `init_set_constants_param`)

- **Registers per element** (`per`), from the constant's own class (the CTAB's, not the effect's): sets c and i:
  SCALAR rows × cols, VECTOR 1, MATRIX_ROWS rows, MATRIX_COLUMNS cols. Set b: rows × cols. Set s: 1.
- **Leaves**: element e (or the parameter itself when it is not an array) is a leaf with first register `reg + e·per`
  and count `min(per, max(0, count − e·per))`. A leaf with count 0 is not uploaded. The CTAB's RegisterCount is the
  bound, not elements × per: MW's preshaders list only the registers up to the element they read (W: element counts
  are clamped to the registers left).
- **Layout**: `scalar` (component 0), `vector` (components 0..cols−1), `rows` (register r holds row r) or `columns`
  (register r holds column r), from the constant's class. Where W reads a matrix's components depends on the
  parameter's class too (`get_const_upload_info`, `set_constants`: transposed exactly when one class is MATRIX_ROWS
  and the other MATRIX_COLUMNS). Register r, component c: a MATRIX_ROWS parameter (or any other class; row-major,
  §2.4) gives `rows` `data[r·cols + c]` and `columns` `data[c·cols + r]`; a MATRIX_COLUMNS parameter, whose data W
  reads column-major, gives `rows` `data[c·rows + r]` and `columns` `data[r·rows + c]` (its dwords come from the
  file, SetValue and the array setters in storage order; SetMatrix refuses it, §2.5). MW has no MATRIX_COLUMNS
  parameter (V). For a truncated matrix only the first `count` registers (rows or columns) go up. Components past
  the shape are not written.
- **Conversion** from the parameter's type to the register set's: c → float, i → int, b → bool, with conv() of §2.5
  (an INT parameter in set c becomes a float). In a preshader, set c is the input table `c` (F§4.3), so INT
  parameters reach the FXLC code as floats.
- MW (V): no b or i set anywhere. A preshader reads only components inside its constant's columns (component 0 of
  every scalar register; checked over all `c` operands), so components past a constant's shape are never read.

### 3.4 Samplers

A matched set-s constant binds the sampler parameter's `sampler_state` blocks (element j's block, or the one block
of a non-array sampler) to sampler stages `reg + j` for j < used, the constant's count cut to its own elements:
min(count, max(1, Elements)) with the CTAB's Elements (W's RegisterCount: `parse_ctab_constant_type` gives a sampler
one register and an array one per element, at most the count; `d3dx_set_shader_constants` loops to it, and step 4
bounds it by max(1, elements)). Stages are plus 257 (D3DVERTEXTEXTURESAMPLER0) for a program on the VERTEXSHADER row
(W decides by the row, not by the version token; §3.1). The block's own index field (256 in every MW block, F§3.3)
is ignored there (W: the parent index wins). MW (V): 171 sampler constants, all in pixel shaders
(165 SAMPLER2D, 6 SAMPLERCUBE), each with count 1.

### 3.5 State sources, pass dependencies, technique versions

- **Source** of a state (`fxm_state_source`): its resources (F§5.6) decide. With none, the literal value
  (`FXM_S_LITERAL`; a NULL shader is an INT literal 0, F§3.4). Usage 0 makes it a preshader or shader by §3.1.
  Usage 1 means "the parameter named by the data" (`FXM_S_PARAM`, the name looked up with `fxm_by_name(e, 0, …)`;
  id 0 when it does not resolve, `""` included, where W refuses the whole effect: §8). Usage 2 and anything else is
  `FXM_S_UNKNOWN` (not in MW). More than one resource (not in MW): the last one in file order decides (W: each
  `d3dx_parse_resource` overwrites the state's type, its object data, the shader made from it, its evaluator and
  its referenced parameter); `res` is that one and `nres` says how many there are. The others are matched (§3.2)
  but add no deps and count in no version.
- **deps** of a pass (`fxm_pass_deps`): the sorted set of top-level ids with bytes > 0 that can change something the
  pass applies, namely, through the resource that decides each of its states: the matched parameters of every
  numeric constant in every preshader and every applied shader (§3.1), the top-level parameter of every usage-1
  name, and, for every matched sampler constant of an applied shader, the same two kinds over every state of the
  blocks it binds: the elements j < used of §3.4 (W applies only those). The same two kinds also come from the
  block of a sampler that a usage-1 name on a pass state of class SAMPLER names
  (`SAMPLER[i] = <smp>`, row 178, the only such row; W applies an SC_SETSAMPLER state's sampler states, each with its
  own dirty test): the named node's own block, element 0's for an array (the model's choice: W's sampler array has a
  NULL data pointer, which SC_SETSAMPLER dereferences; §8). A sampler constant of a program on such a block state,
  and a usage-1 sampler name there, are not followed further. On any other row (`TEXTURE[0] = <smp>`) W takes the
  sampler as the state's value and applies none of its states, so its block is not followed. A usage-1 name of a
  shader parameter adds that parameter only (§3.1). MW has no row-178 state (V). This union is M5-05's pass-level
  filter, not a state's input list: each state is tested against its own inputs (§2.6), and with those the "inactive
  passes" rule U describes can be reproduced.
- **Versions** of a technique (`fxm_tech_versions`): the highest version token (compared on major·256 + minor; the
  first of equal ones) among the applied shaders (§3.1) that decide its pass states, vertex (row 146) and pixel
  (row 147) apart; 0 when there is none. M5-06's ValidateTechnique compares them with the reported caps. A shader that is not
  applied counts for neither (where W's device refused its code, W's ValidateTechnique fails the technique through
  `creation_failed`; §8).

### 3.6 MW totals (V, §7's prototype and the M5-01 dumps)

| Programs | Set | Conversion and layout | Constants | Of which with registers cut |
|---|---|---|---|---|
| preshaders | c | int>float scalar | 597 | 288 (the INT[5] arrays with count 1..4, 72 of each) |
| preshaders | c | float columns | 7 | 0 |
| preshaders | c | float vector | 6 | 0 |
| shaders | c | float vector | 365 | 3 (one float4[4] with count 1, 2, 3) |
| shaders | c | float columns | 110 | 26 (4×4 matrices with count 1 (6), 2 (1), 3 (19)) |
| shaders | c | float scalar | 99 | 0 |
| shaders | s | sampler | 171 | 0 |

## 4. The state table (M5-04)

### 4.1 The API (in `fx2.h` and `fx2.c`)

`k_states` in `fx2.c` turns from a name list into the one table of rows, so there is still only one table:

```c
enum { FX2_SC_RENDER, FX2_SC_STAGE, FX2_SC_NPATCH, FX2_SC_FVF, FX2_SC_TRANSFORM, FX2_SC_MATERIAL, FX2_SC_LIGHT,
       FX2_SC_VSHADER, FX2_SC_PSHADER, FX2_SC_VSCONST, FX2_SC_PSCONST, FX2_SC_TEXTURE, FX2_SC_SAMPLERSTATE,
       FX2_SC_SAMPLER, FX2_SC_STATEBLOCK, FX2_SC_PRESHADER_VS, FX2_SC_PRESHADER_PS, FX2_NSCLASSES };
#define FX2_SV_DWORD      0x100u   /* value types beyond FX2_T_*: an integer state value */
#define FX2_SV_STATEBLOCK 0x101u
typedef struct {
    const char *name;   /* §3.1 of fx2-format.md */
    uint8_t  cls;       /* FX2_SC_* */
    uint8_t  rows, cols;/* the value size the table gives; 0x0: a register count taken from the value */
    uint16_t vtype;     /* FX2_T_FLOAT, _BOOL, _INT, _TEXTURE, _SAMPLER, _PIXELSHADER, _VERTEXSHADER, FX2_SV_* */
    uint16_t nindex;    /* index count: 1, 8, 256, 261; 0 = any */
    uint32_t d3d;       /* §4.2 */
} fx2_state_info;
const fx2_state_info *fx2_state_get(uint32_t op);   /* NULL for op >= FX2_NSTATES */
const char *fx2_sclass_name(uint32_t cls);          /* RENDER, STAGE, ... as in the enum; NULL past it */
```

`fx2_state_name` keeps its behaviour (it reads `.name`), so the M5-01 dumps do not change.

### 4.2 Classes, values and sizes (D for the table, S for the D3D9 values)

| Rows | Class | d3d | Value type and size | Index count |
|---|---|---|---|---|
| 0..102 | RENDER | D3DRENDERSTATETYPE (F§3.1's column) | FLOAT 1×1 for 18..20, 60..62, 65..67, 74, 79, 81..86, 98; DWORD 1×1 otherwise | 1 |
| 103..120 | STAGE | D3DTEXTURESTAGESTATETYPE | FLOAT for 112..115, 117, 118; DWORD otherwise | 8 |
| 121 | NPATCH | 0 (SetNPatchMode) | FLOAT 1×1 | 1 |
| 122 | FVF | 0 (SetFVF) | DWORD 1×1 | 1 |
| 123..126 | TRANSFORM | D3DTRANSFORMSTATETYPE base: 3 PROJECTION, 2 VIEW, 256 WORLD, 16 TEXTURE0; the target is d3d + index | FLOAT 4×4 | 1, 1, 256, 8 |
| 127..131 | MATERIAL | D3DMATERIAL9 member 0..4 (Diffuse, Ambient, Specular, Emissive, Power) | FLOAT 1×4, Power 1×1 | 1 |
| 132..145 | LIGHT | D3DLIGHT9 member 0..12 (Type, Diffuse, Specular, Ambient, Position, Direction, Range, Falloff, Attenuation0..2, Theta, Phi); 13 = LightEnable (the DLL's convention; W has a class of its own for it) | 132, 145 DWORD; 133..135 FLOAT 1×4; 136, 137 FLOAT 1×3; 138..144 FLOAT 1×1 | 0 |
| 146, 147 | VSHADER, PSHADER | 0 | VERTEXSHADER, PIXELSHADER | 1 |
| 148..155, 156..163 | VSCONST, PSCONST | register count: 0 (from the value) for F, B, I and the unnumbered row; 1..4 for CONSTANT1..4 | FLOAT 0×0, BOOL 0×0, INT 0×0, FLOAT 0×0, FLOAT 1×4, 2×4, 3×4, 4×4 | 0 |
| 164 | TEXTURE | 0 | TEXTURE | 261 |
| 165..177 | SAMPLERSTATE | D3DSAMPLERSTATETYPE 1..13 | FLOAT for 172; DWORD otherwise | 261 |
| 178 | SAMPLER | 0 | SAMPLER | 261 |
| 179 | STATEBLOCK | 0 | STATEBLOCK | 1 |
| 180, 181 | PRESHADER_VS, PRESHADER_PS | 0 | VERTEXSHADER, PIXELSHADER | 1 |

### 4.3 Rows 89 and 102, rows 179..181, the INT texture transform

- d3dx9_26's table gives row 89 (CCW_STENCILFAIL) the value 53 (D3DRS_STENCILFAIL) and row 102 (BLENDOPALPHA) 171
  (D3DRS_BLENDOP) (D, F§3.5). The runtime table holds the SDK's values, **186** and **209**: the documented meaning
  (L-084), and Wine's. MW uses neither row (V), so lockstep cannot tell the two apart. M5-05 logs the first use of
  89, 102 or 179..181 in any effect. Whether the DLL applies its own value at run time is an oracle question (§8).
- Rows 179..181 exist only in d3dx9_26 (W stops at 178). They are in the table so that every operation the DLL
  accepts has a row; MW uses none (V).
- One state of a fixed-function technique in effect 4 gives TEXTURETRANSFORM an INT 1×1 literal 0 (F§3.4). W hands
  the value's storage to SetTransform as a matrix without checking its type. The game picks that technique only if
  every shader technique of effect 4 fails ValidateTechnique (it has `shader` = 0, §1.5). The table keeps FLOAT 4×4;
  what M5-05 applies there is open (§8).

### 4.4 The DLL's table (for the test; D)

182 rows of 32 bytes plus a terminator row whose name is 0, held four times in the image (VAs 0x40BCC8, 0x40F6C8,
0x4127C0, 0x4147C8). The copies agree in bytes +0..+27 and differ only in +28 (F§3.1 says "identical"; it is
identical except for that field). Row layout: +0 name (pointer to a NUL-terminated string), +4 value type (D3DX's
type number for typed rows; one shared private code for DWORD rows and another for the state block, both ≥ 20), +8 a
flag (1 on rows 16, 122 and 168: shown in hex; the runtime does not use it), +12 rows, +16 columns, +20 index count,
+24 `class << 24 | value` (value: the D3D9 number, the table's own transform number 0..3, the member number, or for
constant rows the register count with the B and I rows marked in bits 16..23), +28 the value-name list. The test
finds the table at run time: the string `ZENABLE` (NUL on both sides) in the mapped image, then every 4-aligned
dword equal to its VA. Nothing of the DLL goes into a tracked file.

### 4.5 The state-table test

`tools/mw/fxmodel_test.c` (§6.1) checks the runtime table against (b) and, reading the DLL itself with its own
small PE reader, against (a); `--states` prints the table.

- **(a) d3dx9_26.dll**, when the environment variable `NFSMW_D3DX` (or `--dll`) names it. Otherwise this part is
  skipped and the skip names the variable. The file's SHA-256 must equal `games/mw/game.conf`'s `D3DX_SHA256`. The
  test checks:
  exactly four copies, equal in +0..+27 for all 182 rows, with a zero name at row 182. For every row: the name
  equals `fx2_state_get(op)->name`; rows, columns and index count are equal; typed rows have the same type number,
  DWORD rows all share one code ≥ 20 and the state block has another; the rows of each of our classes share one DLL
  class code and different classes have different codes (a bijection, so no DLL numbering is written in the test);
  the value equals `d3d` for RENDER, STAGE, SAMPLERSTATE, MATERIAL and LIGHT, **except rows 89 and 102, where the
  DLL must hold 53 and 171** and ours 186 and 209. TRANSFORM goes through the stated map {0→3, 1→2, 2→256, 3→16}
  (W, S). For constant rows the low 16 bits equal `d3d`, and bits 16..23 are non-zero exactly on the BOOL and INT
  rows. Every other class has value 0.
- **(b) the SDK list**, written in the test from `d3d9types.h` as name → value pairs (`D3DRS_ZENABLE` 7 … the whole
  of D3DRENDERSTATETYPE, D3DTEXTURESTAGESTATETYPE and D3DSAMPLERSTATETYPE; D3DTS_VIEW 2, D3DTS_PROJECTION 3,
  D3DTS_TEXTURE0 16, D3DTS_WORLD 256; the D3DMATERIAL9 and D3DLIGHT9 member orders). Every row maps by name: RENDER
  `D3DRS_` + name, STAGE `D3DTSS_` + name, SAMPLERSTATE `D3DSAMP_` + name; TRANSFORM, MATERIAL and LIGHT by the
  row name's field; constant rows by their digit. All 182 must agree. Explicit checks: WRAP0..WRAP7 are rows 31..38
  with 128..135 and WRAP8..WRAP15 are rows 39..46 with 198..205; POINTSCALE_A is row 65 with 158, and row 65 is not
  168 (D3DRS_COLORWRITEENABLE, the old kit's off-by-8); COLORWRITEENABLE is row 73 with 168; rows 89 and 102 hold
  186 and 209.
- **Rows MW uses**: with the effects given on the command line, every state operation in every pass and sampler
  block is below 182, its row has an SDK value by (b), and its index is below the row's index count when that is
  not 0. The test prints how many distinct rows are used: 46 (V, F§3.2's list).

## 5. The canonical model dump (`fxmodel 1`)

### 5.1 Files and text

- Input: every `out/mw/fx/res/*.fxo`, taken in **byte order of NAME** (strcmp on the file name without `.fxo`, not
  the locale's order). One pool; the effects are created in that order and all stay alive; the dump is written
  after the last one is created and before any Set*.
- Implementation A (`tools/mw/fxmodeldump.c` with `runtime/mw/fxmodel.c` and `runtime/mw/fx2.c`) writes
  `out/mw/fx/ma/all.txt`; implementation B (`tools/fxmodel.py`, its own model on top of `tools/fx2dump.py`'s
  reader, written from this document and not from A) writes `out/mw/fx/mb/all.txt`. The two must be byte for
  byte the same. An input that does not parse (F§5.11) gives no output and exit status 1.
- Text: F§5.1's rules (ASCII, LF, no tab, no trailing space, no empty line, single spaces, two-space indentation,
  HEX8, QSTR, F32, INT, UNK and the unknown count). The goal is 0 `UNKNOWN(` on MW.
- **TT** is F§5.4's, except that an element prints its array's typedef without the ` [n]`.
- **VAL(type, dwords)**: each dword as F32 for FLOAT, INT for BOOL and INT, HEX8 for any other type, separated by
  spaces; `-` when there are none.
- **FRAG** is F§5.4's (a literal state value).
- **TYPED** (an annotation's value): non-array numeric 1×1: `bool INT`, `int INT`, `float F32`, or `raw HEX8` for
  another type. Other numeric: `bool`, `int`, `float` or `raw` followed by VAL over all its dwords, all elements.
  STRING, per element and separated by spaces: `string QSTR(the bytes of the first object-data entry with that id,
  up to the first NUL)`, or `string UNK(id)` when the id is O or more (no object; as `obj UNK(id)`) or no entry has
  it. Other objects: `obj <id>` per element (`obj UNK(id)` when the id is O or more), separated by spaces. Sampler,
  struct or an unknown class: `UNK(class)`.
- **VN**: F§5.9's version names (`vs_1_1`, `ps_2_x`, ...).

### 5.2 Order

1. `fxmodel 1`, then `effects <n>`.
2. Each effect in input order: its two header lines, its parameter blocks in file order, its technique blocks in
   file order.
3. The pool section.
4. `end`.

### 5.3 Effect header

```
effect <i> QSTR(NAME) size <file bytes> sha256 <64 lowercase hex digits>
desc parameters <P> techniques <T> nodes <N> shared <S>
```

i counts from 0 in input order. P and T are the stored counts, N the node count, S the number of top-level
parameters that hold a pool slot.

### 5.4 Parameter blocks

Depth 1 (two spaces) for a top-level parameter; each child one level deeper than its parent:

```
param #<id> <i> path QSTR TT sem QSTR flags HEX8 bytes <B> shared <slot|-> annotations <A>[ = V]
element #<id> <e> path QSTR TT flags HEX8 bytes <B>[ = V]
member #<id> <m> path QSTR TT name QSTR sem QSTR flags HEX8 bytes <B>[ = V]
sampler states <n>
annotation #<id> <k> name QSTR TT flags HEX8 = TYPED
```

- `sem` and `name` are QSTR with `-` for offset 0 (F§5.1); `path` is QSTR of the built path, never `-` (an
  unnamed parameter's path is `""`).
- ` = V` ends the line of a **leaf** with storage: numeric with elements 0: `= VAL(type, its rows × cols dwords as
  stored)`; an object with storage (§2.4), elements 0: `= obj <id> data HEX8(its stored dword)`, with
  `obj UNK(id)` when id ≥ O (an object of another type has no ` = `). A shared parameter prints its slot's value.
  An unknown class prints `= UNK(class)`.
  Arrays and structs have no ` = `; their elements or members carry the values.
- A **sampler leaf** (a non-array sampler parameter, or one element of a sampler array) has no ` = `. The next line,
  one level deeper, is `sampler states <n>`, and its n states (§5.6) follow one level deeper still.
- After a node's line: its sampler block (if any), then its children in order, each with its own subtree, then (top
  level only) its annotations.

### 5.5 Technique blocks

```
technique #<id> <i> name QSTR passes <Q> annotations <A> vs <VN|-> ps <VN|->
  annotation #<id> <k> name QSTR TT flags HEX8 = TYPED
  pass #<id> <j> name QSTR states <S> annotations <A> deps <d>[ #<id>]...
    annotation #<id> <k> name QSTR TT flags HEX8 = TYPED
    state ...
```

`vs` and `ps` are §3.5's versions (`-` for 0). `deps` is the count, then the sorted ids of §3.5. A technique's
annotations come before its passes; a pass's annotations before its states.

### 5.6 States

```
state <k> <NAME>[<index>] <CLASS> <d3d> SOURCE
```

NAME is F§3.1's, CLASS is `fx2_sclass_name`, d3d is decimal (§4.2). For operation 182 or more: `UNK(op)[<index>]
- -` in place of the three fields. SOURCE:

| Source | Text |
|---|---|
| no resource, simple value with element count 0 | `literal = FRAG` |
| no resource, anything else | `literal UNK(class)` |
| preshader | `preshader res <r> outputs <rows × cols of the state's value, 1 for a STRING> ctab <count or ->`, then its `const` lines one level deeper |
| shader of a known version | `shader res <r> <VN> ctab <count or ->`, then its `const` lines one level deeper |
| shader of an unknown version | `shader res <r> UNK(version)` |
| usage 1 | `param #<id>`, or `param UNK(0x00000001) name QSTR(data up to the first NUL)` when the name does not resolve |
| usage 2, any other usage, or usage 0 on a type of neither kind | `UNK(usage)` |

When more than one resource targets the state, SOURCE is that of the last (the one that decides, §3.5), followed
by ` UNK(<number of resources>)`.

### 5.7 Constant lines

```
const <k> NAMEQ <SET><reg> count <count> -> MATCH
```

NAMEQ is QSTR of the name (`-` for offset 0, UNK(offset) when it lies outside, as F§5.9); SET is `b`, `i`, `c`, `s`
or UNK(set). MATCH:

| Match | Text |
|---|---|
| numeric | `#<id> <CONV> <LAYOUT> per <per> elements <used>/<nelem>` |
| sampler | `#<id> sampler stage <first stage> elements <used>/<nelem>` |
| unmatched (§3.2) | `UNK(<reason>)` |

CONV is the parameter's type and the set's type as `bool`, `int` or `float`, written once when they are equal and
as `<from>><to>` otherwise (`int>float`). LAYOUT is `scalar`, `vector`, `rows` or `columns`. An entry outside the
table prints `const <k> UNK(0x00000006)` and ends the list.

### 5.8 The pool section

```
pool slots <n>
  slot <s> name QSTR TT bytes <B> holders <h> = VAL(type, all its dwords)
    holder <i> QSTR(NAME) #<id>
    apart <i> QSTR(NAME) #<id> flags HEX8
```

Slots in slot-number order, live slots only (all of them, at dump time). For an object type the value is
`= data HEX8 ...` (its stored dwords). Holders are listed in input order, then id order. `apart` lists every
top-level parameter, in input order and then id order, whose name equals the slot's (names compare as in §2.7) and
which is not one of its holders (unshared twins and typedef mismatches).

### 5.9 Example

A made-up effect (not from any game), formatted by these rules:

```
fxmodel 1
effects 1
effect 0 "EXAMPLE" size 1236 sha256 0000000000000000000000000000000000000000000000000000000000000000
desc parameters 4 techniques 1 nodes 9 shared 1
  param #1 0 path "tint" FLOAT VECTOR 1x4 sem "TINT" flags 0x00000001 bytes 16 shared 0 annotations 0 = 0x3f800000:1 0x3f000000:0.5 0x00000000:0 0xbdcccccd:-0.100000001
  param #2 1 path "blend" INT SCALAR 1x1 [2] sem - flags 0x00000000 bytes 8 shared - annotations 0
    element #3 0 path "blend[0]" INT SCALAR 1x1 flags 0x00000000 bytes 4 = 1
    element #4 1 path "blend[1]" INT SCALAR 1x1 flags 0x00000000 bytes 4 = 0
  param #5 2 path "base_map" TEXTURE OBJECT sem - flags 0x00000000 bytes 4 shared - annotations 0 = obj 1 data 0x00000000
  param #6 3 path "base_sampler" SAMPLER OBJECT sem - flags 0x00000000 bytes 0 shared - annotations 0
    sampler states 2
      state 0 TEXTURE[256] TEXTURE 0 param #5
      state 1 MINFILTER[256] SAMPLERSTATE 6 literal = 2
  technique #7 0 name "t0" passes 1 annotations 1 vs vs_1_1 ps -
    annotation #8 0 name "level" INT SCALAR 1x1 flags 0x00000004 = int 3
    pass #9 0 name "p0" states 3 annotations 0 deps 2 #1 #2
      state 0 ALPHABLENDENABLE[0] RENDER 27 preshader res 2 outputs 1 ctab 1
        const 0 "blend" c0 count 1 -> #2 int>float scalar per 1 elements 1/2
      state 1 VERTEXSHADER[0] VSHADER 0 shader res 1 vs_1_1 ctab 1
        const 0 "tint" c4 count 1 -> #1 float vector per 1 elements 1/1
      state 2 PIXELSHADER[0] PSHADER 0 literal = 0
pool slots 1
  slot 0 name "tint" FLOAT VECTOR 1x4 bytes 16 holders 1 = 0x3f800000:1 0x3f000000:0.5 0x00000000:0 0xbdcccccd:-0.100000001
    holder 0 "EXAMPLE" #1
end
```

The sampler is used by no shader here, so `base_map` is not among the pass's deps.

## 6. Test plan

### 6.1 C unit tests (`tools/mw/fxmodel_test.c`)

The tests are built like `tools/mw/fxdump.sh` builds its tools (clang `-Wall -Wextra -Werror`, ASan and UBSan, any
report fatal) and run on the effect paths given as arguments. Each failure prints the effect, node path and
expectation.

- **Synthetic effects**: a small builder in the test writes fx_2_0 bytes in memory (F§2), covering what MW lacks:
  BOOL, INT and FLOAT scalars; vectors 1×2, 1×3, 1×4; MATRIX_ROWS 3×3, 4×3, 4×4; a MATRIX_COLUMNS parameter;
  arrays of each; a struct and an array of structs; a texture and a texture array; a STRING annotation; a sampler
  with a usage-1 texture reference; a shared parameter; an unnamed parameter, member and annotation, with a CTAB
  constant and a usage-1 reference named `""` (§2.3: none of them matches), GetParameterByName(parent, NULL)
  giving the parent, an unnamed technique and pass found by `""` and not by NULL; pass states `SAMPLER[i] = <smp>`
  and `<smpa>` whose blocks feed the deps (§3.5: smp's block and element 0's of the array) and `TEXTURE[i] =
  <smpb>`, whose block does not; a creation the memory bound refuses, which leaves the pool's versions and counter
  alone (§2.6).
- **Conversions**: every row of §2.5 with exact dwords. That includes 1.9 → 1 and −1.9 → −1 into INT; NaN and 3e9
  → 0x80000000; −0.0 → TRUE; SetBool(5) on BOOL stored raw, then GetBool → 5 (raw: `set_number` copies equal
types) and GetValue → 1; SetBoolArray(5) on FLOAT
  → 5.0; the SetInt colour form on float3 and float4 and GetInt back; SetVector into INT 1×1 (clamping); SetVector
  into a 1×2 FLOAT leaving nothing past cols; SetMatrix and SetMatrixTranspose into 3×3 and 4×3; SetMatrixArray and
  SetVectorArray with count > elements → INVALIDCALL; SetIntArray with count 3 on INT[5] writing 3 of 5 elements and
  count 9 writing 5; every refused shape; id 0 → INVALIDCALL with nothing changed; Set* on an annotation and on
  technique and pass ids → INVALIDCALL; SetValue on a sampler → E_FAIL.
- **Versions and changed**: per §2.5's last column. SetInt and SetFloat with the same value leave the version
  alone; SetIntArray, SetVector and SetMatrix with the same values raise it; SetTexture with the same pointer does
  not. `changed` follows the bytes in every case. Versions only rise.
- **Lookups on all 31 effects**: for every parameter, element and member, `fxm_by_name(e, 0, path)` returns its id;
  for every member and element child, `fxm_by_name(e, parent, member name)` returns the member. Out-of-range,
  `[]`, leading-zero, unknown and case-changed names return 0. For every top-level parameter with a semantic,
  `fxm_by_semantic` with the semantic as stored, all upper case and all lower case returns its id; NULL gives the
  first parameter without one. `fxm_param(e, 0, i)` and `fxm_element(e, 0, i)` give the i-th top-level parameter and
  0 at P; `fxm_element` on an array gives its elements and 0 at n; `fxm_param` on an array gives 0. Every technique
  annotation is found by name and `fxm_get_int` on it returns its file dword. Techniques and passes are found by
  index and name.
- **CTAB**: over the 31 effects, 0 unmatched constants and §3.6's totals. Synthetic CTABs give every reason 1..7
  (reason 4 also for StructMembers on a numeric parameter and array), and each one stays in the list.
- **State table**: §4.5 (b) and the rows MW uses. `--states` prints the table for §4.5 (a) as one line per row:
  `<op> <name> <class> <vtype> <rows>x<cols> <nindex> <d3d>`.

### 6.2 The pool test (M5-02's done-when)

Over all 31 effects, created in byte order with one pool:

- For every slot and **for every holder in turn as the writer**: write a value that cannot occur by chance, of the
  slot's type (INT: `0x5A000000 + 256·slot + writer`, per element plus the element number; FLOAT: the bits of
  `slot + writer/64 + 0.125`; TEXTURE: the fake guest pointer `0x13570000 + 16·slot + writer`). Write it with the
  setter MW uses for that shape (SetIntArray, SetInt, SetFloat, SetVector, SetTexture) and once more with SetValue.
  Read it back with GetValue through **every** holder: equal. Each holder's `fxm_version` equals the writer's and
  is higher than before the write.
- Every `apart` parameter (same name, not shared or another typedef) keeps its value and its version through the
  writes to that slot, and one that holds no slot keeps its default (a holder of a twin slot is written with it).
- Named checks, the only MW names the test states: `ColorWriteMode` and `BlendState` (GAME §5.5) each have one slot
  with 19 and 23 holders and 1 and 3 apart parameters. SetInt(0xF) through one holder of ColorWriteMode, and
  SetIntArray({0,0,0,5,6}, 5) through one holder of BlendState (what 0x6C5ED2 writes), read back in all of them.
- Lifetime (on BlendState's slot; the named checks and that choice only when the input is the 31 effects, any
  other set takes the first slot with the most holders and skips this part when none has two): free the first
  holder; the others still read the value; create that effect again and it reads the
  slot's current value, not its default, and the slot's version rose (§2.6). Free every holder of a slot; a new
  effect with that parameter starts from its default. `drop` is called exactly for the object pointers that die.
- Matching, on synthetic effects: same name with a different type, element count or member count gives separate
  slots; a joiner sets the slot's version to `++counter`; a sampler with the shared flag gets no slot; an effect
  without a pool shares nothing.

### 6.3 The A/B dump compare

`tools/mw/fxdump.sh --model` builds A, runs it, runs B, and compares `out/mw/fx/ma/all.txt` with
`out/mw/fx/mb/all.txt`. On a split it prints the first differing line and fails; it also fails on any `UNKNOWN(`.
It checks the line counts against §7. It then compares the two **Set/Get traces** (`fxmodeldump --ops` into
`ma/ops.txt`, `tools/fxmodel.py --ops` into `mb/ops.txt`, text `fxmodel ops 1`): for every node id 0..N+1 of every
effect, in one pool, the same fixed calls in the same order (GetValue, Set/GetBool, Set/GetInt twice, Set/GetFloat
twice, Set/Get{Bool,Int,Float}Array, Set/GetVector, Set/GetVectorArray with count elements, 0 and elements + 1,
Set/GetMatrix plain and transposed, Set/GetMatrixArray, Set/GetTexture or String twice, SetValue, the lookups by
name, `""` for an unnamed node, and by NULL), each with its result, changed flag, the node's version and what it
read; then the first effect freed and made again and every effect freed, with the dropped pointers. The dump does
not show §2.5 and §2.6; the trace does. Then B's self-test and pool test.

`--model --diff <N>` also runs both, dump and trace, on N mutated copies of each effect
(`tools/mw/fxdiff_mutate.py --model`; each copy alone or in one pool with an unchanged copy of another effect)
and on 20 × N made-up sets of one to four effects (`tools/mw/fxdiff_synth.py --model`): names from a small
vocabulary so CTAB names, usage-1 names and pool matches hit, typedefs reused or changed by one field across the
set, structs, BOOL and VOID- or STRING-typed numeric parameters, string annotations (ids past O included), sampler
arrays bound in part, CTAB constants built from the parameters with one field changed (every reason of §3.2), and
shaders, preshaders and parameter references on pass and sampler states. M5-01 showed that the rules for odd input
are only settled where the two first differ; every rule in §2..§5 that is not V was settled that way (and against
W where W says something).

### 6.4 wasm

`tools/mw/fxdump.sh --wasm` also compiles `runtime/mw/fxmodel.c` with the same target and flags as `fx2.c`, warnings
fatal.

### 6.5 Node test (`web/test/mw-fxmodel.test.mjs`)

In `mw-fx.test.mjs`'s style and with its skip rules (no `speed.exe` in `NFSMW_DATA`, no clang, or no Python with
pefile → skipped with that reason). It extracts the effects to `out/mw/fx/mtest/` (removed afterwards), builds
`fxmodel_test` and `fxmodeldump` (ASan and UBSan, warnings fatal), compares A's and B's dumps byte for byte with 0
UNKNOWN and §7's counts, compares their Set/Get traces (§6.3), runs A's unit tests (§6.1, §6.2) on the 31 files
with `--mw`, B's self-test and pool test, and 300 made-up sets through both (`fxdiff_synth.py --model`). The state
table against the DLL (§4.5 a) is its own subtest, run with `NFSMW_D3DX`; when that is unset it is a skipped
subtest named after the variable, not a pass.

## 7. Expected numbers on MW

From a scratch prototype of §2..§3 over the 31 effects (not committed; these are checks for the first run, the A/B
equality is the contract): 31 effects, 951 nodes (533 parameters, 154 elements, 101 annotations, 76 techniques, 87
passes); 1,355 `const` lines (1,184 in set c, 171 in set s), 0 UNKNOWN; 20 pool slots with 156 holders and 9
`apart` lines; technique versions: 16 without shaders, 21 vs_1_1/ps_1_1, 1 vs_1_1/ps_1_4, 18 vs_1_1/ps_2_0, 18
vs_1_1/ps_2_x, 2 vs_3_0/ps_3_0; pass deps: 87 passes, 1,145 ids in all, at most 29 in one pass.

## 8. Not pinned down

Settled only by the oracle (M2-06's lifted `d3dx9_26.dll` in a unit harness, or M5-09's lockstep):

- **Rows 89 and 102** (§4.3): whether d3dx9_26 applies 53 and 171. MW does not use them.
- **The INT TEXTURETRANSFORM literal** (§4.3): what d3dx9_26 does with a value whose type the table does not
  expect.
- **Conversions MW uses** (§2.5, bold rows): W's rules are the contract until M5-09; in particular float→int
  (truncation, and 0x80000000 for NaN), the SetInt colour form's factor (× 1/255 as a float or ÷ 255), and which
  writes count as changes (versions decide what CommitChanges re-applies, so they show in the device-call stream).
- **Name lookups beyond MW's** (§2.3): W matches whole paths (no `a[03]`); older D3DX may parse indices with atoi.
  MW looks up only plain top-level names.
- **Set* on annotations, and cross-effect handles** (§2.5, §2.8): refused here; W accepts both. MW does neither.
- **GetParameterDesc.Flags of elements and members**, and **D3DXEFFECT_DESC.Functions and Creator** (M5-06): W
  gives the top-level flags and leaves Functions unset; MW reads neither.
- **Array and struct annotations** (§2.2): read whole, no element nodes. Not in MW.
- **CTAB constants of another shape** (§3.2 step 7): W never compares the constant's Rows and Columns with the
  parameter's. `init_set_constants_param` takes the parameter's own rows and columns, the constant's class gives
  only the orientation (§3.3), and the effect is refused only when the constant's registers exceed the parameter's
  major dimension (rows, or columns under a MATRIX_COLUMNS constant). So W accepts a float3 parameter under a float4
  constant (3 components go up), a float4 under a float3 (4 go up, a direct copy), a matrix constant with fewer
  registers than the parameter has rows, and the like. This design asks for equal rows and columns and gives
  anything else reason 3 (MW: all equal, C3). Struct constants (reason 7) and sets b and i in a preshader (reason
  2) are not built.
- **Begin flags in registers** (§1.6): 31 of 41 sites pass a register; M5-05 logs any flag value other than 0.

Where this design departs from W on purpose (none reachable from MW's effects or calls):

- **Unmatched constants** (§3.2): W refuses the effect for most failures (§3.2 says which); the model keeps the
  constant with its reason.
- **Usage-1 names that do not resolve** (§3.5), `""` included: W refuses the whole effect (`d3dx_parse_resource`,
  D3DXERR_INVALIDDATA), and so it does for a usage-2 array selector whose name does not resolve. The model keeps the
  state as `FXM_S_PARAM` with id 0 (a usage-2 state is `FXM_S_UNKNOWN`), and M5-05 logs it once per effect.
- **Other resources W refuses** (§3.5, §5.6): `d3dx_parse_resource` fails the whole effect (E_FAIL) for a usage-0
  resource on a state whose type is none of VERTEXSHADER, PIXELSHADER, BOOL, INT, FLOAT and STRING, for a usage other
  than 0, 1 and 2, and for a technique, pass, parameter, element or state index out of range; `d3dx_create_param_eval`
  fails it (D3DXERR_INVALIDDATA) for a shader resource with data whose version token fails `(token & 0xFFFE0000) ==
  0xFFFE0000`. The model keeps the first two as `FXM_S_UNKNOWN` (`UNK(usage)` in the dump) and the last as a shader
  of an unknown version (`shader res <r> UNK(version)`); a resource whose target does not exist is on no state (F§5.6)
  and the model ignores it. M5-05 logs each once per effect. MW has none (V: every resource has a target, usages 0
  and 1 only, no UNKNOWN in the dump).
- **Shaders that are not applied** (§3.1): they add no deps and count in no technique version. Where the device
  refused the code, W's ValidateTechnique fails the technique (`creation_failed`); an object of the other type on a
  shader row W hands to the other Set*Shader and uploads its constants there. M5-06 logs such a program and fails
  ValidateTechnique for its technique instead of following either.
- **OBJECT-class inputs without registers** (§3.2): an OBJECT-class parameter under a constant of another class, and
  a sampler under a set-s OBJECT constant in a preshader, get reason 2. W accepts both as inputs without registers
  and, for a sampler in a shader, applies its sampler states at stages RegisterIndex + i for i < RegisterCount
  (whatever the register set). The model binds nothing and adds no deps.
- **Where the CTAB is found** (§3.1): the model takes the first CTAB comment of a shader's token walk and of a
  preshader's leading comments. W finds it with `D3DXFindShaderComment`, which steps dword by dword after the version
  token, takes any dword whose low word is 0xFFFE for a comment and stops at the first dword 0x0000FFFF, even inside
  an instruction's operands; and for a preshader it reads the CTAB only when `find_bytecode_comment` finds an FXLC
  in the leading comment run, a search that also stops at a comment of length 0 (`parse_preshader` returns before
  `get_constants_desc`). So a preshader without an FXLC, or with a length-0 comment before it, has no inputs in W
  but its CTAB matched here, and a shader whose CTAB follows an operand 0x0000FFFF (a `def` literal) has no inputs
  in W but its CTAB matched here. MW (V): both rules find the same comment in all 758 programs (and none in the two
  ps_1_1 shaders without one), and every preshader's FXLC is found.
- **Shader parameters and preshaders inside shaders** (§3.1): a shader in a VERTEXSHADER or PIXELSHADER parameter,
  reached through a usage-1 reference, gets no program, no CTAB match and no deps beyond the parameter itself, where
  W matches its CTAB and uploads its constants when the state is applied; a PRES preshader in a shader is not
  modelled either. M5-05 logs such a state (once per effect, as for unmatched constants) instead of applying it
  silently.
- **`SAMPLER[i] = <sampler array>`** (§3.5): the deps follow element 0's block. W reads the array's data pointer,
  which is NULL for a sampler array, and SC_SETSAMPLER dereferences it.
- **`@` names** (§2.3): W's name tree holds `<path>@<name>` for every annotation of a top-level parameter, so such a
  name finds the annotation in GetParameterByName, in a CTAB match (which W then refuses: not top level) and in a
  usage-1 reference, and a later parameter literally named `p@k` loses to the annotation. The model has no such
  paths: `p@k` finds nothing (a CTAB constant gets reason 1, a usage-1 reference `UNK(0x00000001)`), and a
  parameter named `p@k` is found as itself.
- **Writes past the storage** (§2.5): W's SetVector and SetMatrix copy `cols` (and `rows`) components even past
  4, SetInt's colour form writes 3 or 4 floats whatever the storage, SetTexture and SetString write a pointer into
  a node of any size. The model clamps to 4 × 4 and to the storage and refuses objects without storage.
- **SetVectorArray's stride**: W steps by `cols`, the model by rows × cols (equal unless a VECTOR has rows ≠ 1,
  which fxc does not write).
- **Names at offset 0** (§2.2): compared as the empty string in the pool's match, §5.8's `apart` and the technique
  and pass lookups, where W compares the string it reads at body +0. A parameter or annotation lookup by `""`
  finds nothing, as in W, so an unnamed parameter or annotation is found by no name here (W's would be found by
  that string).
- **Name records of size 0** (§2.2): at a non-zero offset such a record is the empty string here (fx2 gives it
  p non-NULL, n 0), where W leaves the name or semantic NULL (`d3dx9_parse_name` returns before it allocates). So a
  semantic lookup by NULL skips such a parameter and one by `""` finds it, where W does the reverse
  (GetParameterBySemantic matches a NULL semantic only to a NULL query), GetParameterDesc (M5-06) would give `""`
  where W gives NULL, and MW's handle-table pass (0x6C5500, §1.4), which hashes only a non-NULL semantic, would put
  a handle under the hash of `""`. MW has no record of size 0 (V).
- **A NaN colour component** (GetInt's colour form, SetVector into INT 1×1): W gives 255 (windef's ternary
  `min`/`max`: NaN passes `max` and `min` returns the bound). The model gives 0 on purpose. What d3dx9_26 itself
  does is M5-09's to check if MW ever produces one.
