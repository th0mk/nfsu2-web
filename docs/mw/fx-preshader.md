# The FXLC preshader evaluator (M5-03) and its checks

Status: **2026-10-28**, design and results for M5-03. Consumers: the two implementations of this contract (A in C,
`runtime/mw/fxpres.{c,h}`; B in Python, `tools/fxpres.py`), M5-05/06 (the ID3DXEffect HLE: BeginPass and
CommitChanges evaluate every preshader state through `fxp_state_value`), M5-09 (lockstep against the oracle).

`docs/mw/fx2-format.md` is the file format ("F§n" here) and `docs/mw/fx-model.md` the effect model ("M§n"). This
document is what `d3dx9_26.dll` (9.07.239.0000, game.conf's `D3DX_SHA256`) does with an FXLC preshader: how its
loader checks one, what its interpreter computes, under which x87 precision, how the effect fills the interpreter's
input registers from parameters and turns its outputs into a state's value. It also records where A departs from the
DLL on purpose and where the oracle departs from the hardware. Nothing here is game data or DLL data: addresses,
counts, and facts about the code. Review round 1 (R1-01..R1-11) changed where A stops (§3.3, §5.1), `log` (§3.2),
the API (§4.4), the oracle and the comparison (§6), and the effect model's typed setters (fx-model.md §2.5). Review
round 2 (R2-01..R2-09) changed the input table's size (§2.5), the values A scatters (§4.2), the temporaries in an
effect (§5.1), the API's control word (§4.4), and added checks where none could fail before: effect rounds on
patched copies (§6.3), rounding corners, signalling NaNs in memory operands, noise's reads, unknown temporaries
(§6.2), and wasm runs (§6.6).

**Marks.** As in F§0 and M§0, plus two:

| Mark | Source |
|---|---|
| **X** | `d3dx9_26.dll`'s code, read by disassembly (capstone over the image at its preferred base 0x400000), at the address given: facts only, nothing copied |
| **O** | The DLL run under Unicorn (`tools/mw/fxoracle.c`, §6), with the check named in §7 |
| **V** | Verified on Most Wanted's 31 effects (`out/mw/fx/res/*.fxo`) |
| **W** | Wine's d3dx9 (`preshader.c`, `effect.c`, LGPL): facts only, no code. Where W and X differ, X wins and the row says so |
| **S** | Intel's SDM (vol. 1 §4.8, §8; vol. 2 FLD, FSIN, FCOS) and Microsoft's documentation |

## 1. Where D3DX evaluates a preshader (X)

| VA | What |
|---|---|
| 0x4DA2E5 | `D3DXFindShaderComment` (exported): the comment search of §2.1 |
| 0x4E9225 → 0x4E9146 | The loader's check of a preshader, stdcall `(blob, size, &fxlc, &clit)`: §2.2. The effect loader calls it for every preshader resource (0x4F34A0, 0x4F3615, 0x4F490A, 0x4F5965); `D3DXFillTextureTX` too (0x4BEDCF) |
| 0x4E86B6, 0x4E872A | Its input and output operand checks |
| **0x4E8817** | **The interpreter**, stdcall, 14 arguments, below. Called by the effect (0x503813, 0x503977) and by texture fills (0x4BEC5F, 0x4BECDB). Its frame (0x4E881C, 0xC1C bytes): the 256 temporaries at ebp − 0xC1C, the 256 input slots (pointers) at ebp − 0x41C, then its locals. An input's slot is written at 0x4E89A8; `mov` is one `rep movsd` (0x4E8D5F) |
| 0x4E8771, 0x4E87C0 | The two noise opcodes (0x502, 0x711): read their inputs as floats (§3.2), then call 0x44318F, whatever the component or input count |
| 0x4FB2E7, 0x4FB050 | Builds a preshader's tables: the CTAB's register ends per set give the input table's size and mask (§2.5) |
| 0x4F362E..0x4F368A, 0x4FA3C8 | The output table's size and mask from the state value's register count (§2.5) |
| 0x5036A4 | The effect's evaluation of one preshader (thiscall): the precision switch (§3.4), the gathers, the interpreter |
| 0x50269A, 0x502A38 | The gathers: parameters into the float and int registers, and into the bool registers (§4.1) |
| 0x500C68 | The scatter: outputs into the state's value, by type (§4.2) |
| 0x4FC056 | The array selector (usage 2, F§4.5): element `_ftol2(x + 0.001)`. Not in MW |
| 0x600050 | `_ftol2`, linked into the DLL (§3.1) |
| 0x44318F | Perlin noise for opcodes 0x502 and 0x711, from permutation and gradient tables in the DLL's data (§5.1) |
| 0x550680..0x5508E2 | The DLL's own HLSL compiler writing an FXLC: the FOURCC, the count, the instructions, then `0xF0F0F0F0 0x0F0F0F0F` (F§4.3's trailer: written, never read) |
| 0x4DD1A0.. | The disassembler's opcode names: mov neg rcp frc exp log rsq sin cos asin acos atan min max lt ge add mul atan2 cmp dot noise dotswiz. No name for 0x208 (`div`), 0x301 (`movc`) or 0x711 |

The interpreter's arguments (X, 0x4E8817; the effect path's values from 0x5037EE..0x503813, O):

| # | Meaning | Effect path | `D3DXFillTextureTX` |
|---|---|---|---|
| 0, 1 | FXLC data (the count), CLIT data (the count) | the preshader's | the shader's |
| 2, 8 | Table 3 (`v`) base, mask | a local of 0x5036A4, mask 0: one stack double nobody writes | the texel position and size, mask 7 |
| 3, 9 | Table 2 (`c`) base, mask | the input table, mask of §2.5 | the constants, as doubles |
| 4, 10 | Table 9 base, mask | 0, 0 | 0, 0 |
| 5, 11 | Table 4 (`oc`) base, mask | the output table, mask of §2.5 | four doubles, mask 3 |
| 6, 12 / 7, 13 | Tables 6 (`oi`) and 5 (`ob`) | **the same table and mask as 4** | the same |

## 2. The program as D3DX reads it

### 2.1 Finding the comments (X, 0x4DA2E5)

- The version's high word must be 0x4658, 0x5458, 0x7FFE, 0x7FFF, 0xFFFE or 0xFFFF (else `D3DXERR_INVALIDDATA`).
- From dword 1, token by token: a token with bit 31 set is skipped (one dword). Low word 0xFFFF is the end: not
  found (`S_FALSE`). Low word 0xFFFE is a comment of L = bits 16..30 dwords: when L > 1 and the dword after the token
  is the FOURCC, the data starts after the FOURCC; otherwise L dwords are skipped. Any other token is an instruction:
  with version low word ≥ 0x200 its bits 24..27 are the dwords that follow it; below that, `def` (0x51) takes 5 more
  and anything else none (its operand tokens, bit 31 set, are skipped one by one).
- There is **no size**: the search reads until it finds the end token. A walks only inside the blob and calls a search
  that leaves it `FXP_E_SCAN` (§5.1). The oracle stops the DLL's search at its first read past the blob (§6.2), so
  that verdict is checked too (**O**: 35 generated cases, 2,140 mutations). Every one of MW's 616 preshaders is the
  version, then exactly CTAB, CLIT, FXLC, then the end token (F§4.3, **V**), so the order of comments never matters
  for MW.

### 2.2 The loader's check (X, 0x4E9146; **O** on every case of §6.2)

In this order; the first failure returns `E_FAIL`, and D3DXCreateEffect then fails with it (**O**, §4.4):

1. Version: 0x46580200 ≤ v ≤ 0x46580201, or v = 0x54580100 (`tx_1_0`). MW: 0x46580200 in all 616 (**V**).
2. An FXLC comment and a CLIT comment (§2.1). There is no CTAB check here (the table builder reads the CTAB, M§3).
3. **A CLIT count of 0 is overwritten with 1** in the effect's copy: every literal read divides by the count (§2.4),
   so with count 0 literal 0 is the 8 bytes after the count, the start of the next comment. 610 of MW's preshaders
   have count 0 (24 literals in 6 tables, F§4.3); none reads a literal there (**V**).
4. The instruction count is FXLC's first dword. For each instruction, with q the read position and `end` the blob's
   end (the resource size, not the comment's length): `q < end`; the token is not read; the input count k is read
   at q + 4. For each of the k inputs: N, the number of index pairs, then `q + 4 < end`; each index pair needs
   `q + 8 < end` and a table other than 9; the register pair needs `q + 8 ≤ end` and a table 1..9. The output: its
   flag word must be 0, then one pair with `q + 8 < end` and a table 4, 5, 6 or 7.
5. Nothing else: not the opcode, not the component count, not the trailer, not whether a register is inside a table.

A read the operand checks make past `end` always ends in one of these comparisons failing (the pointer only grows),
so A refuses such a program at once with the same verdict (`FXP_E_OPERAND`), without reading past the blob. The
check's other reads past the end decide its verdict: the version (0x4E9158, a blob under 4 bytes), the CLIT count
(0x4E91BE) and the FXLC count (0x4E91C9) past the end, and the comment search; A stops at them with `FXP_E_SCAN`.

### 2.3 An instruction (X, 0x4E884C; **V** for MW's 739)

| Word | D3DX's reading |
|---|---|
| Token | bits 0..19 the component count n (**20 bits**; F§4.3 says 0..15 and MW's are 1, 3 or 4), bits 20..30 the opcode, bit 31 the scalar form |
| k | the input count: any number (§3.3) |
| Operands | k inputs, then the output (§2.4) |

### 2.4 Operands and addressing (X, 0x4E886F..0x4E8A06; **O**)

An input is N (the flag word, F§4.3) then N index pairs then one register pair; F§4.3's flag 1 is N = 1. The output
is a flag 0 and one pair. The element is computed in unsigned 32-bit arithmetic:

```
e = 0
for each index pair (table t, offset f): e = f + 4e; if t is 1..7: e = low dword of _ftol2(value of (t, e))
register pair (table t, offset f):       e = f + 4e  -> the element below
```

An index pair whose table is 0, 8 or ≥ 10 reads nothing: its offset just enters the sum. The value of the register is
truncated toward zero (`_ftol2`, §3.1); NaN, infinities and |x| ≥ 2^63 give 0. Decoding reads nothing but the index
registers: the register pair only gives a pointer, which goes into the input's slot (§3.3).

| Table | Element | Read as |
|---|---|---|
| 1 literal | `e mod count` (unsigned, the count of step 3 above), then 8 bytes each: component j reads the next 8 bytes, past the literals into the blob if need be | doubles from the blob (byte offsets modulo 2^32, as D3DX's pointers wrap) |
| 2 `c`, 3 `v`, 4/5/6 the output table | `e & mask` | component j is element + j, **not masked again** |
| 7 temporaries | `e & 0xFF` | 256 doubles on the interpreter's stack, component j at + j |
| 9 | `e & mask9`, 4-byte elements read as doubles | in both D3DX paths base 0 and mask 0: every read through it is at address 0 (A: `FXP_E_RANGE` at the read, **O**) |
| 8 (input only) | no case: the slot is **not written**, it keeps the pointer an earlier input of the run left there (0x4E899D) | A: the same (§3.3, **O**: `slot_t8_*`) |

### 2.5 The tables in the effect path (X; **O**: the masks D3DX passed in BeginPass for MW's programs)

- **Input table** (`c`, 0x4FB2E7): from the CTAB, the end (register + count) of the float set (`fend`), the bool set
  (`bend`) and the int set (`iend`) over every CTAB entry. The table holds `fend` float registers of 4 doubles, then
  `bend` bool registers of one double, then `iend` int registers of 4 doubles, and its mask is the next power of two
  of `bend + 4(fend + iend)` less one (0 when that is 0). MW's masks: 3, 7, 15, 31 (**O**). D3DX reserves **mask + 1
  doubles** for it (0x4FB467..0x4FB4D8: the reservation ends at its start + (mask + 1) × 8, the three parts inside
  it) and they are **zero**: registers no constant fills, and every double past the three parts up to the mask, read
  as 0, and with no constant at all (mask 0) the table is one double, 0 (**O**: the `pad` copies of §6.3, 2,700 state
  values; the same with an oracle whose malloc, operator new and HeapAlloc without HEAP_ZERO_MEMORY return memory
  filled with 0x40, so D3DX zeroes it itself). 144 of MW's 616 tables are longer than their float registers (`fend`
  3: mask 15; 5: mask 31), and 6 have no constant; MW's programs read none of that (review R2-01: A had 4 `fend`
  doubles and stopped there).
- **Output table** (oc, ob and oi alike, 0x4F362E): `R = ceil(r/4) × c × max(1, elements)` registers for a state
  value typedef of `r × c` (F§2.3's rows and columns; 0x4FA3C8), at least 1; the mask is the next power of two of
  4R, less one. MW: 3 for every value of up to 4 components, 15 for the 4 × 4 TEXTURETRANSFORMs (**O**).
- **Temporaries**: 256 doubles in the interpreter's frame, never cleared (§5.1).

## 3. Evaluation

### 3.1 The number model (X for the code; S for the x87; **O**)

The interpreter keeps every value as an IEEE double in memory and computes on the x87:

- **Loads** (`fld qword`) are exact; a signalling NaN is loaded **quiet** (invalid masked, S). Stores (`fstp qword`)
  round to nearest-even into a double, subnormals and overflow to infinity included.
- **Arithmetic** (`fadd`, `fmul`, `fdiv`, `fsqrt`) rounds its exact result to the **precision control's** 24, 53 or
  64 bits with the x87's 15-bit exponent (never subnormal there), and the store rounds again: a result can be rounded
  twice. Rounding control: the caller's (D3DX never changes it); A models nearest only and refuses a control word with
  another (§4.4). A computes the exact result in integers (a 128-bit
  significand, a sticky bit, a bit-serial square root and two extra quotient bits) and rounds it the same two ways;
  B does the same with Python's rationals.
- **NaNs** (S, table 4-7): an operation on a register and a memory operand returns a NaN operand made quiet; a quiet
  register NaN with a signalling memory NaN gives the quiet one; two quiet NaNs give the larger significand; with
  equal significands the positive one (the SDM does not say; Unicorn's QEMU does this, §5.2). Invalid operations
  (∞ − ∞, 0 × ∞, 0/0, ∞/∞, `fsin`/`fcos` of ∞) give the x87's default NaN, stored as 0xFFF8000000000000.
- **Comparisons** (`fcomp`): unordered with any NaN; −0 equals +0.
- **`_ftol2`** (0x600050): `fistp` with the current rounding (nearest), then a correction from the sign of
  `x − round(x)` stored as a float: truncation toward zero to 64 bits whatever the precision (the correction only
  reads the sign); NaN, infinities and |x| ≥ 2^63 give 0x8000000000000000.

How often the precision shows (A on the exact synthetic and random cases of §6.2 that run, against the same cases
at 53 bits): at 64 bits 86 of 1,221 cases differ (165 values), from double rounding alone; at 24 bits 702 of 1,097
(1,868 values).

### 3.2 The opcodes (X, the switch at 0x4E8A14 (scalar) and 0x4E8BF6; **O** on every one)

`a`, `b`, `c` are inputs 0..2 at component j; the result goes to output component j, for j < n, unless the row says
otherwise. "Load-store" means `fld qword`/`fstp qword` (a signalling NaN comes back quiet); "rounded" means §3.1. An
opcode reads its inputs only inside its loop over j < n (with n = 0, nothing; `dot` and `dotswiz` write their 0
first even then), in the order the row gives, and only the inputs the row names: this is where a read through a slot
can fail (§3.3).

| Op | Name | Result | Scalar form (bit 31) | Class |
|---|---|---|---|---|
| 0x000 | nop | nothing | nothing | exact |
| 0x100 | mov | the 8 bytes copied (`rep movsd`, 0x4E8D54): **bits as they are**, a signalling NaN too (**O**: the `snan_100_*` cases compare exactly) | nothing | exact |
| 0x101 | neg | −a, load-store (`fld`/`fchs`/`fstp`, 0x4E8D42: X and S; Unicorn keeps a signalling NaN, §5.2) | nothing | exact |
| 0x103 | rcp | `fld1; fdiv a`: 1/a rounded | nothing | exact |
| 0x104 | frc | `a − floor(a)` rounded, floor by msvcrt's `floor` (exact) | nothing | exact |
| 0x105 | exp | msvcrt `_CIpow(2, a)` | nothing | approx |
| 0x106 | log | first **slot j's** component 0 (§3.3): when it equals 0 (±0), 0, and `a` is not read; else `fld a; fabs; fldln2; fxch; fyl2x; fmul` by the double 1.4426950408889634 (rounded): ln\|a\| × log2e. ±0 gives `fyl2x`'s −∞ (zero divide, masked; S) and −∞; a NaN comes back quiet with its sign cleared (`fabs`). Neither is O: Unicorn's `fyl2x` leaves ±0 alone (§5.2) | nothing | approx |
| 0x107 | rsq | `fld a; fabs; fsqrt; fdivr 1.0`: 1/√\|a\|, rounded twice at the precision (sqrt, then the division) | nothing | exact |
| 0x108, 0x109 | sin, cos | `fsin`/`fcos`: \|a\| > 2^63 leaves a unchanged (C2 set); ±∞ is invalid | nothing | approx |
| 0x10A, 0x10B | asin, acos | msvcrt `_CIasin`, `_CIacos` | nothing | approx |
| 0x10C | atan | `fld a; fld1; fpatan` | nothing | approx |
| 0x200 | min | a < b ? a : b, load-store (NaN: b) | input 0 at component 0 | exact |
| 0x201 | max | a > b ? a : b, load-store (NaN: b) | the same | exact |
| 0x202 | lt | a < b ? 1.0 : 0.0 (NaN: 0) | the same | exact |
| 0x203 | ge | a ≥ b ? 1.0 : 0.0 (NaN: 0) | the same | exact |
| 0x204 | add | `fld a; fadd b` | `fld b[j]; fadd a[0]` | exact |
| 0x205 | mul | `fld a; fmul b` | `fld b[j]; fmul a[0]` | exact |
| 0x206 | atan2 | `fld a; fld b; fpatan`: atan2(a, b) | input 0 at component 0 | approx |
| 0x208 | div (W) | **nothing**: no case in the switch (W: returns 0) | nothing | exact |
| 0x300 | cmp | a ≥ 0 ? b : c, load-store (−0 ≥ 0; NaN: c); only the input chosen is read (0x4E90E5..0x4E9127) | nothing | exact |
| 0x301 | movc (K) | **nothing** | nothing | exact |
| 0x500 | dot | output component 0 ← +0 first, then for j < n: `fld a; fmul b; fadd [out]; fstp [out]` | the same with `fld b[j]; fmul a[0]` | exact |
| 0x502 | noise | Perlin noise (0x44318F) of input 0's components 0..min(n, 3) − 1 as floats, zero-filled to three, into output component 0; the inputs are read first, and the noise runs with n = 0 too (0x4E8771) | nothing | A stops (§5.1) |
| 0x70E | dotswiz | output component 0 ← +0, then for i < ⌊k/2⌋: `fld [input i]; fmul [input ⌊k/2⌋ + i]; fadd; fstp`, each at component 0; n is not used | nothing | exact |
| 0x711 | noise | the same from inputs 0..min(k, 3) − 1, component 0 of each (0x4E87C0) | nothing | A stops |
| any other | — | nothing | nothing | exact |

W's table differs in four places: W evaluates `div` (returning 0) and has no `noise`; W's `dotswiz` works per
component; W reads every input with its own operand. MW uses only `mov` (739 instructions, **V**). The scalar switch
(0x4E8A14) has cases for 0x200..0x206 and 0x500 only: every other opcode with bit 31, the noise opcodes included,
does nothing (**O**: the `syn_502_*_s*` and `noise_*_s*` cases run and compare). `rcp` is 0x103 and
`rsq` 0x107 here, checked against the DLL on every form (**O**); the D3D9 shader opcodes RCP 6 and RSQ 7, which
recomp-kit's shader interpreter swapped (GAME §5.5, L-084), are another table.

### 3.3 Order, aliasing and the input slots (X; **O**: the `alias_*`, `slots_*`, `slot_*` and `temp_*` cases)

- Each instruction decodes its k inputs into pointer **slot** 0..k−1 of one array of 256 (the interpreter's stack, the
  same for the whole run, not cleared at the start), then the output. An input whose register is in table 8 does not
  write its slot (§2.4). The opcode then reads through fixed slots whatever k is: slot 0 for the unary opcodes, 0 and
  1 for the binary ones and `dot`, 0 and the one it picks for `cmp`, slot j for `log`'s zero test, k/2 pairs for
  `dotswiz`, and only inside its loop (§3.2). A slot past k holds the pointer an earlier input of the same run left
  there; one that no input of this run set holds whatever the stack held.
- A follows the slots across the run and stops (`FXP_E_STALE`) at the first read **through** a slot no input of
  this run set, exactly where D3DX would read through its stack: not before (a `cmp` with two inputs whose input 0 is
  never negative runs; any opcode with n = 0 runs), not for a slot that is only loaded. fxc's `log` of a float4 reads
  slots 1..3: such a program runs in A only when an earlier instruction had that many inputs. Slot 256 and up (the
  zero test of a `log` with more than 256 components) is D3DX's frame: `FXP_E_RANGE`.
- Components are processed j = 0, 1, … with every read and write in memory: an output that overlaps an input of the
  same instruction reads the values the instruction has already written (a `mov` to one register up repeats
  component 0; `dot` zeroes its output before it reads its inputs).
- The 257th input writes past the slot array into the interpreter's locals (0x4E89A8): A stops there
  (`FXP_E_INPUTS`), after the inputs before it are decoded and before the opcode runs.

### 3.4 Which precision (X 0x5036A4, 0x4FA917, 0x4FC9A2; **O**)

0x5036A4's third argument chooses: when it is not 0 and the effect has not done so yet, it saves the control word and
sets 64-bit precision (`_controlfp(0, _MCW_PC)`); two sites restore the saved word (0x4FA917, 0x4FC9A2). When it is 0,
the interpreter runs at the **caller's** control word. Its callers (X, review R1-09): 0x4FC0CB, 0x4FC2D3, 0x506FBD and
0x507113 pass 0; only 0x50622D passes something else, 0x5061DC's third argument, which 0x5061DC's two callers make
from bit 20 (0x100000) of the state record's flags dword at +0x10 (read at 0x506D53): 0 or −1 by `shl edi, 0xB; sar
edi, 0x1F` (0x506E9E, 0x507179). What sets that bit is not known. Under the oracle (§7, all 1,566 BeginPass calls):
every evaluation in BeginPass after `Begin(D3DXFX_DONOTSAVESTATE)` ran at the control word set before the call
(0x007F, 0x027F, 0x037F alike); `Begin(0)`, which records a state block first, ran some evaluations at 0x037F (64
bits) in 16 of 72 Begin calls. MW's game thread runs at 24 bits (CreateDevice without `D3DCREATE_FPU_PRESERVE`,
L-041), so BeginPass evaluates its preshaders at 24 bits. M5-05 passes `pc` from the guest's control word, and 64
where D3DX's flag path applies (§9).

For MW the precision cannot change a value: `mov` copies bits, and the gather's and scatter's conversions (§4) are
exact or independent of it.

## 4. The effect path: gather and scatter

### 4.1 Gather: parameters into the input table (X 0x50269A; **O** for MW's three kinds)

For every matched CTAB constant of the float set (M§3.2), its leaves (M§3.3) fill registers `reg ..`; each value is
converted by the **parameter's** type: FLOAT by `fld dword; fstp qword` (exact; a signalling NaN quiet), INT and BOOL
by `fild dword` (the signed integer, **exact**: W converts an INT through a float, which rounds above 2^24). Where
each value goes:

| Parameter | Register r, component j | Src |
|---|---|---|
| SCALAR, VECTOR | the value's component j, j < columns | **O** (597 INT scalars, 6 FLOAT vectors) |
| MATRIX_ROWS | **row r**: data[r·cols + j], for r < rows, j < cols — whatever the constant's class | **O** (the 7 `float4x4` inputs under MATRIX_COLUMNS constants) |
| MATRIX_COLUMNS | W's rule (M§3.3) | W only (not in MW) |

- The MATRIX_ROWS rule is where D3DX differs from W and from its own shader uploads: under the oracle,
  `SetMatrix(TextureOffsetMatrix, m)` gave `SetTransform(D3DTS_TEXTURE0, mᵀ)`, because the constant is
  MATRIX_COLUMNS and fxc's code transposes it back (`oc[4i + j] = c[4j + i]`), while D3DX's gather put rows, not
  columns, in the registers; the same `SetMatrix` on `WorldViewProj` gave the vertex shader its columns (W's rule).
  (X, inferred: GetValue returns what SetMatrix and SetValue were given, while 0x5027B1 reads down the columns of
  D3DX's 16-byte internal rows, so D3DX keeps such a parameter transposed inside.) So MW's texture
  transforms are the transpose of what the game sets, and M5-05 must not "fix" that.
- D3DX copies whole 16-byte internal rows for FLOAT scalars and vectors, so components past a constant's columns
  carry its padding; A writes 0 there. MW's preshaders read no component past a constant's columns (M§3.3, **V**).
- Only the float set: M§3.2 refuses sets b and i in a preshader (D3DX gathers them into their own parts of the
  table, 0x502A38 for bools; not in MW).

### 4.2 Scatter: outputs into the value (X 0x500C68; **O** for MW's shapes)

What A does and the oracle checks: for a state value of typedef `r × c` with r ≤ 4, no elements, class SCALAR,
VECTOR or MATRIX_ROWS/COLUMNS and type INT or FLOAT (F§2.3; MW's 3- and 4-component values are `N × 1`, its matrices
4 × 4, its other values 1 × 1, all MATRIX_ROWS by class, none BOOL): column i < c, component j < r takes output
`4i + j`, in that order (i outer). FLOAT: `fld qword; fstp dword` (nearest-even, subnormals, overflow; a NaN quiet
with the payload's top 22 bits). INT: `_ftol2`, the low dword. For MW every value is `o[0..N−1]` (**O**: the
transforms, materials and lights), and the conversions are checked on rounded, inexact, huge and NaN outputs through
D3DX's own BeginPass by the patched copies of §6.3 (review R2-02).

What D3DX's code does beyond that, read but **not run** (X only; A refuses these with `FXP_E_TYPE`, review R2-06):

- Column i starts at output `4i` whatever `ceil(r/4)` is (`shl edx, 5`, 0x500DB5), and reads r outputs from there, so with
  r > 4 a column runs into the next column's register; element e + 1 starts `c·ceil(r/4)` **doubles** after element e
  (`lea ecx, [ecx + eax*8]`, 0x500DEF), not registers. A count of output registers (0x500C7C's object, + 0) clips the
  copy when it runs out. W's formula (`4(e·ceil(r/4)·c + i·ceil(r/4)) + j`, which A had) is the same only for r ≤ 4
  and one element.
- BOOL takes the INT path's code (0x500EC4, `_ftol2`): the low dword, not made 0 or 1.
- STRUCT (class 5) is scattered member by member for each element (0x500CBF..0x500D29, through 0x50094E), not refused;
  OBJECT and any other class or type: `E_FAIL`, no value.

A synthetic effect through the oracle (an array, a 5-row or BOOL state value) would settle these; M5-05 meets none in
MW.

### 4.3 What this gives Most Wanted (V, O)

All 739 instructions are `mov`, so each state dword is one input converted twice: an INT state from an INT
parameter is the parameter exactly (`fild` then truncation); a FLOAT state from a FLOAT parameter is the parameter's
bits with a signalling NaN made quiet; a FLOAT state from a literal is the literal rounded to float. The four kinds
of state MW evaluates: 597 dword states (render and sampler states from INT parameters, BlendState's elements among
them), 7 texture transforms (4 × 4, transposed, §4.1), 9 material and light colours, 3 light directions.

### 4.4 The API's edges (`fxpres.h`; review R1-08)

- `fxp_effect_new` refuses the whole effect (`*out` NULL) with the loader's error when D3DX's loader refuses any of
  its preshaders: D3DXCreateEffect then returns E_FAIL (**O**: an effect whose first preshader's version was made
  0x46580300 came back 0x80004005). A preshader A refuses on its own (`FXP_E_SCAN`) is kept with its error, which
  `fxp_prog_error` and `fxp_state_value` return.
- `pc` is 24, 53 or 64; anything else is `FXP_E_ARG`, not 64. M5-05 maps the guest's control word with
  `fxp_pc_from_cw`: PC field `(cw >> 8) & 3` = 0, 2, 3 gives 24, 53, 64 when the rounding control `(cw >> 10) & 3` is
  0 (nearest); the reserved PC field 1, and RC down, up or chop, give 0, which every call refuses (`FXP_E_ARG`).
  D3DX computes under the caller's whole control word, RC included (SetVector's colour form too, M§2.5), and A models
  rounding to nearest only: with a game thread that set another RC, A refuses rather than rounds to nearest (review
  R2-09; MW's mov-only programs would not change, and nothing shows MW sets one).
- In the effect path the temporaries start unknown (`fxp_regs.temps_unknown`, §5.1): a program that reads a
  temporary before writing it gives `FXP_E_STALE`. `fxp_eval` refuses (`FXP_E_TYPE`) the values of §4.2 A does not
  scatter; MW has none.
- `FXP_E_ARG` also for a state whose source is not a preshader, a program not of the effect, and a short `cap`.
- Cost (M4 Pro, clang -O2, MW's 492 pass states): 90 ns per `fxp_state_value` before review round 1, about 71 ns after
  clearing only the table parts a run can read (`cs` and `os` up to their sizes; the 2 KB of temporaries and the 256
  slot flags are still cleared on every call), about 48 ns after round 2 (the temporaries are not cleared: unknown,
  none is read before the run writes it; their 256 write flags are). UG2 spent 48 % of a race frame's guest time on this path (L-084), so
  M5-05's memo (§8) matters more than this number; 10,000 evaluations a frame would cost about 0.7 ms natively.

## 5. Departures

### 5.1 A from D3DX, on purpose

A stops exactly where D3DX's next step depends on memory A does not have, and refuses the two things it does not
model (the last two rows: shapes of value not run against D3DX, a rounding control other than nearest); nowhere
else. Each stop is checked against what the oracle's DLL did at that point (§6.2, §6.4), not taken on trust.

| Where D3DX | A | Why |
|---|---|---|
| reads noise tables from its own data (0x502, 0x711, not their scalar forms) | stops when the instruction runs, after reading its inputs (`FXP_E_NOISE`) | the tables are DLL data, which the repo must not carry; HLSL's `noise` exists only for `tx_1_0` texture shaders, never in an effect |
| reads through table 9 (address 0 in both paths) | stops at the read (`FXP_E_RANGE`) | a NULL read |
| writes the 257th input's slot past its array | stops there (`FXP_E_INPUTS`) | its own locals |
| reads the version, a count or the next comment token past the blob's end | refuses (`FXP_E_SCAN`) | the outcome depends on the memory after the blob |
| reads or writes past a table, the literals, the blob or the 256 temporaries | stops (`FXP_E_RANGE`) | the neighbouring memory |
| reads through a slot no input of the run set (§3.3) | stops at that read (`FXP_E_STALE`) | the stack as it was |
| starts the temporaries with whatever its stack held | in the effect path (`fxp_eval`, `temps_unknown`) stops at the first read of a temporary no instruction of the run wrote (`FXP_E_STALE`); `fxp_run` with `temps_unknown` 0 takes the caller's (the vm cases: +0, as the oracle clears them) | the stack as it was (review R2-09: A computed on with +0). **O**: the `temp_unknown_*` and `mw_*` cases, where the oracle records the run's writes to the 256 doubles and stops at the first read of one not written (§6.2). fxc never reads a temporary before it writes it |
| computes exp, log, sin, cos, asin, acos, atan, atan2 with x87 microcode or msvcrt | the C library on the double operand(s), in D3DX's order (log: ln, then the multiply rounded at the precision; sin and cos keep \|x\| > 2^63; ∞ is invalid) | an extended-precision copy of the x87's transcendentals is out of proportion: MW uses none. **Not verified against hardware**: the oracle computes these with the host's C library as well (§5.2), so the 1 ulp of §7 only says A and QEMU call the same library |
| (v table in effects: one stack double) | the caller's v table, none in `fxp_eval` (a read is `FXP_E_RANGE`) | undefined in effects |
| fills float components past a constant's columns with its internal padding (§4.1) | 0 | the model does not keep padding; MW reads none |
| scatters a struct, an array, more than 4 rows and BOOL as §4.2's X reading says | refuses them (`FXP_E_TYPE`) | not run against D3DX (review R2-06); MW has none |
| computes under the caller's rounding control | refuses a control word whose RC is not nearest, or whose PC field is the reserved 1 (`fxp_pc_from_cw` gives 0) | A models rounding to nearest only (review R2-09) |

### 5.2 The oracle from the hardware

The oracle is the DLL under Unicorn 2.1.4, whose x87 is QEMU's. Where that is not what an x87 does, A follows the
hardware (S) and the comparison counts the case apart, each as narrowly as it shows (§6.4). The rules these rows
cover are X and S, not O: the oracle cannot see them.

| Case | x87 (S), and A | Unicorn | Where |
|---|---|---|---|
| `fld` of a signalling NaN (double or float) | made quiet | kept signalling, through every later load and store | `neg`, `min`, `max`, `cmp`, the gather's `fld dword` |
| Arithmetic whose register operand was loaded from a signalling NaN | the load made it quiet: two quiet NaNs, the larger significand | still signalling, so a quiet memory operand wins | `add`, `mul`, `dot` |
| `fsin`, `fcos`, `fpatan`, `fyl2x` | microcode: 64-bit significands, `fsin` and `fcos` reduce by a 66-bit π | QEMU's helpers convert to a double and call the host's C library (`sin`, `cos`, `atan2`, `log(x)/log(2)`): sin(double π) is libm's 0x3CA1A62633145C07 (run on Unicorn 2.1.4 directly), where Intel's documentation and the published measurements give 0x3CA1A60000000000 for an x87; `fcos` of a negative NaN drops its sign (run) | the whole approximate class (§5.1) |
| `fyl2x` of ±0 (`log` behind a non-zero test) | −∞ (zero divide, masked) | does nothing: ST0 stays +0, C2 set, no pop (run: TOP 7 after one `log`), so +0 | log |
| `fsin`, `fcos` of ±∞ | invalid: the default NaN | ±∞ kept (QEMU tests \|x\| > 2^63 only) | sin, cos |
| ±2^63 exactly | the SDM's range "−2^63 to +2^63" does not say; A follows QEMU (computed) | computed | sin, cos |

The oracle's imports are the harness's: `floor`, `_CIpow`, `_CIasin`, `_CIacos` and the other msvcrt math are the
host's C library (macOS), `qsort` is a stable insertion sort calling the DLL's comparator (D3DX sorts CTAB entries by
register set and index, 0x4F97C4: keys that do not tie), `IsProcessorFeaturePresent` says no (with `--features`,
SSE and SSE2 reported, the interpreter cases and the effect rounds gave the same bytes, **O**), the heap never
frees, the device is a recorder (§6.3). The interpreter in isolation is called exactly as the effect calls it (§1's
table), with the tables of §6.2's layout.

## 6. The checks

### 6.1 Unit tests (`tools/mw/fxpres_test.c`, A)

The number model against values worked out by hand: the three precisions, double rounding (1 + (2^−53 + 2^−65)),
signed zeros, invalid operations, every NaN rule of §3.1, subnormal results and ties, overflow, `_ftol2`, the
double/float/int conversions. Every check of the loader (versions, the comment search's skips, CLIT count 0, every
input, index and output table, the output flag, every truncation of an instruction and the strict end of the output
pair). Every opcode with NaN, −0 and ∞ where they matter, the scalar forms, the no-op opcodes, the addressing (index
from c and from a literal, truncation, a NaN index, table 0, literal wrap and reading on past the literals, the mask,
temporaries modulo 256), aliasing. Where A stops (§3.3, §5.1): `cmp` with two inputs (slot 2 read only for a negative
or NaN input 0), opcodes of no component with missing inputs, `log`'s zero test before its operand, `log` of ±0 and
of NaNs, table 8 after an input and first, table 9 with and without a component, 256 and 257 inputs, noise (its
inputs read first, noise reading 3 components and 3 inputs whatever n and k, the scalar forms), a `pc` of 0 or 2,
temporaries that start unknown (read before written, as an index, at component 1, behind `cmp`'s choice; written
then read; `dot` into one), which operand add, mul, dot, their scalar forms and dotswiz load (a quiet NaN against a
signalling one, both ways), `fxp_pc_from_cw`. With the 31 effects: all 616 load, 739 instructions all `mov`, and
with random parameter bits each preshader state of every pass at each precision equals its input converted (§4.3);
copies whose 150 preshaders read past their float registers evaluate and read 0 there (§2.5); a copy whose
preshader reads r[0] first is stale; the values §4.2 refuses (an array, 5 rows, BOOL, a struct, an object) and a
FLOAT 4 × 4 that runs; the API's refusals (§4.4), and an effect copy whose first preshader has a bad version is
refused whole. Its programs and tables are static: it runs in wasm at wasi-sdk's default 64 KB stack (§6.6).

### 6.2 The interpreter cases (`fxpcase 1` → `fxpres 1`)

`tools/mw/fxpres_cases.py vm` writes one text that A (`tools/mw/fxpresrun.c vm`), B (`tools/fxpres.py vm`) and the
oracle (`tools/mw/fxoracle.c vm`) each run:

```
fxpcase 1
case <name>
pc <24|53|64>
[run 0]                                  (the oracle checks the load only; no generated case has it now)
[temps unknown]                          (the temporaries start unknown, as in an effect: §5.1)
blob <bytes> <dword hex8>...             (the preshader as stored)
c <n> mask <m> <hex16>...                (table 2)
o <n> mask <m> <hex16>...                (tables 4, 5, 6; initial values)
[v <n> mask <m> <hex16>...]              (table 3)
end
```

```
fxpres 1
case <name> load ok | fail <reason> | unsupported scan          (fail: D3DX refuses too; scan: §5.1)
  class exact | approx                                          (approx: an opcode of §3.2's approximate class)
  run ok | range | stale | inputs | noise
  o <hex16>...                                                  (the output table after the run)
  r <i>=<hex16>... | -                                          (the temporaries that are not +0)
end
```

The oracle writes `fxpres 1 oracle`: `load ok | fail | scan <eip> | fault <addr>` and `run ok | fault <addr> |
frame <addr> <eip> | temp <addr> <eip> | inputs | noise | limit`, no class, and `o` and `r` only for `run ok`. It runs the loader's check
on the blob followed by zeros, and stops it as `scan` at the first read past the blob by the comment search
(0x4DA2E5..0x4DA3CC) or of the version or a count (§2.2). It runs the interpreter on copies placed so that the blob,
c, o and v each **end where an unmapped page begins**, its 256 input slots filled with an unmapped address (0x41414141)
and its temporaries with 0, and stops it at the first access outside them: a `fault` past a table, the literals or
the blob (the page after it), at address 0 (table 9, a missing v), or at 0x41414141 + 8j (a read through a slot
nothing set); a `frame` data access (8 bytes, or `mov`'s `rep movsd`) at or above its slots, where temporaries past
255 lie; `inputs` when 0x4E89A8 writes a slot past the 256; `noise` when 0x44318F is called. With `temps unknown`
it also records which of the 256 temporaries' dwords the run writes and stops at the first read of one it has not
(`temp`).

The cases: every opcode of §3.2 in both forms with 0..5 components, inputs from every table directly and through
index registers, at each precision (`syn_*`), the same with signalling NaNs in the pool (`snan_*`; the `dotswiz` that
sets the slots first takes none, so that what differs is the opcode under test); index chains, wraps, index tables
0, 8, 10 and 1000, multi-pair chains (`idx_*`); aliasing (`alias_*`); short input counts after a `dotswiz` of 8
(`slots_*`); `log`'s zero test (`syn_106_*_zt*`, `syn_106_4_v4_own_*`, `syn_106_1_v2_ztonly`), the slots D3DX reads
through and those it does not (`slot_cmp2_*`, `slot_n0_*`, `slot_stale_*`), tables 8 and 9 (`slot_t8_*`,
`slot_t9_*`), temporaries past 255 (`temp_*`), noise in both forms and with its inputs out of reach (`noise_*`), with
only its 4th component or input out of reach (`noise_502_v*_c29`, `noise_711_k*_lastv`: 3 read, so it runs; the k = 3
control stops; review R2-05); temporaries that start unknown (`temp_unknown_*`, review R2-09); signalling NaNs only
where add, mul, dot, their scalar forms and dotswiz take their memory operand (`snanm_*`: there Unicorn and the x87
agree, so the operand roles are compared exactly, §6.4; every other case quiet register NaNs with small payloads
against signalling memory NaNs with larger ones, review R2-04); operands on the number model's rounding corners
(`corner_<op>_<pc>_<kind>_*`, review R2-03): for rcp, rsq, add and mul at each precision, `--corners` (12) of each
kind: just above a tie with the truncated last bit even or just below it with that bit odd (within 2^-16 of a unit:
where only the guard and sticky bits decide), rsq's square root there too where the root a slip would give changes
the double stored, a subtraction at 24 bits of an operand 128 or more binades smaller from one on a tie (only a
sticky bit left of it), and at 64 bits results whose double depends on the register being rounded or truncated to
64 bits; found in integers among structured significands and exponent gaps, and for rcp, rsq's division, rsq's root
at 24 and 53 bits (Hensel's lifting of a square root modulo 2^55) and the subtraction built next to their tie; every
check of §2.2 and the comment walk of both versions (`ld_*`, `ld_walk_*`: `def` takes 5 dwords in `tx_1_0`, a token's
bits 24..27 in 2.0); random programs over every exact opcode (`rand_*`); every preshader of the 31 with random inputs
at each precision, as the effect runs it (`mw_*`: the c table zero past 4 `fend` up to the mask, §2.5; temporaries
unknown). `mutate` writes corrupted copies of every preshader (truncated, bits flipped, small dwords, instruction
tokens, random dwords, bytes appended). Every case runs in the oracle.

### 6.3 The effect rounds (`fxpround 1` → `fxpeff 1`, `fxpdev 1`)

```
fxpround 1
round <name> pc <24|53|64>
set <NAME> <parameter path> <dwords> <hex8>...      (SetValue)
call <Setter> <NAME> <parameter path> <count> <dwords> <hex8>...      (a typed setter)
...
end
```

`rounds` sets, in every round, every parameter a preshader reads in every effect (in byte order of NAME) to edge or
random bits (INT: small values, 0, −1, the limits, 2^24 + 1; FLOAT: signed zeros, infinities, quiet and signalling
NaNs, subnormals, FLT_MAX, whole numbers, random), and in every other round calls on each of them one or two of the
typed setters MW uses (M§1.2: SetInt, SetIntArray, SetFloat, SetVector, SetVectorArray, SetMatrix, SetMatrixArray,
and SetFloatArray, SetMatrixTranspose) with the values where D3DX's conversions show: ±0.5 and its neighbours, 3e9,
−3e9, 2^31, 2^32, NaN, the infinities, colours either side of (k + 0.5)/255, two SetFloats in a row on a FLOAT (−0
after +0, NaN after NaN). After each round A (`fxpresrun effect`) and B write each setter's result, then, for every
pass of every technique, every pass state whose source is a preshader and every state of the sampler blocks the
pass's applied shaders bind (M§3.4), with the device call that carries it and the value:

```
call <Setter> <NAME> <parameter path> = <hresult hex8>
round <name> pc <pc>
effect <NAME>
  tech <t> pass <p>
    state <k> <STATE>[<index>] -> <key> = <hex8>...
    sampler <path> stage <s> state <k> <STATE>[<index>] -> <key> = <hex8>...
```

with key `rs <D3DRS>`, `tss <stage> <D3DTSS>`, `ss <stage> <D3DSAMP>`, `transform <D3DTS> 0`, `material <dword>`,
`light <index> <dword>` or `lightenable <index>`. The oracle creates the effects with `D3DXCreateEffectPool` and
`D3DXCreateEffect` on a recording `IDirect3DDevice9`, applies the same `SetValue`s and setters through the vtable
(the setters at the round's control word: SetVector's colour form rounds at it, M§2.5), and for every pass runs
`SetTechnique`, `Begin(D3DXFX_DONOTSAVESTATE)` (or `--begin-flags 0`), `BeginPass` at the round's control word,
`EndPass`, `End`, writing the setters' results and the device calls BeginPass made (`fxpdev 1`) and the control words
the interpreter ran at.

MW's programs are all `mov`, so through the 31 effects every value is an exact float or integer and the precision
cannot show: a wrong scatter conversion or precision would pass every round (review R2-02). `fxpres_cases.py patch`
writes copies of the 31 (game data, under `out/`) whose preshaders compute, and the same rounds run on each: `op 103`,
`107`, `101`, `104`, `105`, `106 --n1`, `108`, every `mov` made `rcp`, `rsq`, `neg`, `frc`, `exp`, `log` (only
one-component ones: `log`'s zero test reads slot j) or `sin` (the same layout, one input), so that D3DX's own gather,
precision switch and scatter meet rounded, inexact, huge, signed-zero and NaN values; `pad`, in each of the 150
preshaders whose input table is longer than its float registers the first `mov` from c reads `c[4 fend]` (or `c[0]`
where there is no constant), so the zeroed padding of §2.5 reaches the device (review R2-01).

### 6.4 The comparisons

A and B: byte for byte, both texts. A and the oracle (`tools/mw/fxpres_compare.py`):

- **Every verdict**: the load (ok, fail, and A's `scan` against the oracle's `scan`) and the run (A's ok, range,
  stale, inputs, noise against what the oracle saw: a fault past a table or at address 0 or a frame access is range,
  a fault at the poisoned slots is stale). A refusal or a stop is no longer passed over: A stopping where the DLL ran
  on, or running where it stopped, is a split (review R1-01). With `--kinds` (the interpreter cases in
  `fxoracle.sh`) each of the five kinds of stop must occur at least once, so none goes unchecked.
- **Every value** of a run both finish: every output and temporary the same bits (class exact), or for the
  approximate class the same bits or at most 4 units in the last place apart. Counted apart, each only where it can
  arise (§5.2): in `snan_*` cases whose opcode loads through `fld`, A's value being the oracle's signalling NaN with
  the quiet bit set; in `snan_*` cases of an arithmetic opcode, two different quiet NaNs; `fyl2x` of a zero (A −∞,
  the oracle +0) in `log` cases; `fsin`/`fcos` of an infinity; and two NaNs in the cases of the opcodes the oracle
  computes with the host's library (exp, sin, cos, asin, acos, atan, atan2; not `log`, whose NaN is now compared).
  A `mov` of a signalling NaN is compared exactly (review R1-07). "Two different quiet NaNs" is forgiven only in
  `snan_*` cases, whose signalling NaNs can be register operands; in the `snanm_*` cases they are memory operands
  only, where Unicorn and the x87 agree, and only the quiet bit of a NaN the x87 loads is forgiven, so loading the
  wrong operand of add, mul, dot or dotswiz is a split (review R2-04; the scalar forms load input 1, where a
  signalling NaN is Unicorn's blind spot, and the unit tests pin those roles). The oracle's `temp` stop is A's
  `stale`.
- **Effects**: every setter result the same, in order; every state A computes must be in the oracle's calls of that
  pass, with the same dwords at its place (a float state dword that is the oracle's signalling NaN made quiet counted
  apart; with `--fsin-inf`, for the `sin` copies, A's default NaN where Unicorn's `fsin` kept an infinity, §5.2); a
  state A gives as an error is a split; and every device dword that changes between rounds in a pass must be one of
  A's states there, so a preshader state A drops is seen (review R1-02).
- **Hosts** (`host`): A's texts from two builds, native and wasm: every case the same bytes, except outputs and
  temporaries of an approximate-class case, which may be up to 4 units apart or two NaNs (each host's C library).
- `fxpres_compare.py --self-test` feeds the comparison made-up texts for each of these ways A can be wrong (a stop
  where D3DX ran, a scan where it loaded, a stale against a range, a `mov` that made a signalling NaN quiet, a `log`
  NaN with its sign, an error, a dropped state, a setter's result) and checks it splits, and that it passes the
  agreeing ones.

### 6.5 Fuzz

`fxpresrun fuzz N` (ASan and UBSan, any report fatal): N corrupted copies of every preshader and 20 N made-up
programs through `fxp_load` and `fxp_run` with random tables, masks, precisions and temporaries known or not.

### 6.6 Where they run

`tools/mw/fxdump.sh --pres` (unit tests, B's and the comparison's self-tests, both texts A against B, and the rounds
over every patched copy of §6.3 A against B; `--diff N` adds N mutated copies of every preshader; `--fuzz N` the fuzz;
`--wasm` compiles `fxpres.c` for wasm with web/build.sh's flags, then links the unit tests and `fxpresrun` for
`wasm32-wasip1` (the same flags without threads, which node's WASI does not run, at wasi-sdk's default 64 KB stack)
and runs them under node (`tools/mw/wasirun.mjs`): the unit tests on the 31, and with `--pres` the cases (`host`
comparison), the rounds and (with `--diff`) the mutations against the native A (review R2-08); with `NFSMW_D3DX` it
runs `tools/mw/fxoracle.sh`: the cases, the mutations, the rounds with both Begin flags and the rounds over every
patched copy, A against the DLL). `web/test/mw-fxpres.test.mjs` (node): the unit tests and self-tests, A against B
on the cases, mutations, rounds and rounds over the copies, and the oracle (`fxoracle.sh --quick`) when `NFSMW_D3DX`
names the DLL and Unicorn is found. The oracle and Unicorn (GPLv2) are test-only: never linked into the game or the
app; the DLL stays local (GAME §5.1).

## 7. Results (2026-10-28, after review round 2)

| Check | Result |
|---|---|
| `fxpres_test --mw` | 5,059 checks, 0 failed; 616 preshaders load, 739 instructions, all `mov`; 1,476 pass-state evaluations over three rounds, 1,890 dwords equal their input converted; 150 preshaders made to read past their float registers, 150 pass states read 0 there |
| `fxpres.py --self-test` | 48 checks, 0 failed |
| `fxpres_compare.py --self-test` | 39 checks, 0 failed |
| A against B, interpreter cases (default sizes) | 11,900 cases (7,392 of them MW's 616 programs × 4 inputs × 3 precisions; 285 rounding corners; 192 with signalling NaNs in memory operands), the same bytes |
| A against B, mutations (20 per preshader) | 12,320 cases, the same bytes |
| A against B, effect rounds | 18 rounds (6 per precision), 2,790 SetValues, 1,215 setter calls, 15,246 state values, the same bytes; the same on each of the 8 patched copies of §6.3 (15,246 values each) |
| **Oracle, interpreter in isolation** | 11,900 cases agree: 10,840 the same bits, 316 the same bits with signalling NaNs in, 192 with signalling NaNs in memory operands (exact), 186 + 52 approximate within 1 ulp (largest 1), 148 refused by both; both stop: 39 at load (scan), 60 noise, 31 range (27 faults past a table or at address 0, 4 frame accesses), 13 stale (9 through a slot nothing set, 4 unknown temporaries), 2 inputs; counted apart (§5.2): 13 cases with a signalling NaN A makes quiet, 1 an arithmetic NaN pick, 4 `fyl2x` of a zero, 1 `fsin(∞)`, 2 host-library NaNs |
| **Oracle, mutations** | 12,320 cases agree, every one run: 8,453 the same bits, 3 approximate (0 ulp), 1,691 refused by both; both stop: 2,140 at load (scan), 26 range, 7 stale |
| **Oracle, effects** | 1,566 BeginPass calls (87 passes × 18 rounds): all 1,215 setter results the same (1,190 S_OK, 25 D3DERR_INVALIDCALL); every one of A's 15,246 state values on the device with the same dwords (8,406 render, 6,498 sampler, 79 + 47 transform, 54 material, 153 + 9 light; the + are signalling float NaNs A makes quiet, §5.2); all 961 device dwords that change between rounds are A's states; 587 of the 616 preshaders reached (the other 29 are in sampler blocks no applied shader binds, so BeginPass never applies them; they are among §6.2's 616); the interpreter ran at the round's control word in every pass (492 each of 0x007F, 0x027F, 0x037F) |
| Oracle, effects with `Begin(0)` | the same; Begin ran the interpreter at 0x027F in 56 calls, at 0x027F and 0x037F in 16 (§3.4) |
| **Oracle, patched copies** (§6.3) | for each of `pad`, `rcp`, `rsq`, `neg`, `frc`, `exp`, `log`, `sin`: 1,566 BeginPass calls, every one of A's 15,246 state values on the device with the same dwords, every changing device dword one of A's (961, 878 for rcp and rsq, 133 for frc and sin, 817 for pad); 2,700 of the `pad` values read the zeroed padding; counted apart: signalling float NaNs A makes quiet (56 in neg and pad, 9 in log) and 67 dwords of `fsin(±∞)` (A the default NaN, Unicorn the infinity, §5.2) |
| Fuzz | 200 copies: 4,616 programs, 90,147 loads, 37,669 refusals, 180,294 runs (11,256 stopped), no sanitizer report |
| wasm | `fxpres.c` and `fxmodel.c` compile with web/build.sh's target and flags, warnings fatal; linked for `wasm32-wasip1` and run under node at the default 64 KB stack: the unit tests 5,059 checks, 0 failed; the cases 11,882 the same bytes, 18 approximate (atan2 8, acos 5, asin 3, atan 2: 1 ulp, or a NaN's sign from asin and acos outside [−1, 1]); the rounds and the mutations the same bytes |
| Node tests | `mw-fxpres` 4 of 4 pass, the oracle included; with `mw-fx` and `mw-fxmodel`, 10 of 10 (18 s) |

What the oracle cannot check, and what checks it instead: the x87's quieting of signalling NaNs (`neg`, `min`,
`max`, `cmp`, arithmetic, the gather's `fld dword`), the operand roles of the scalar forms with a signalling register
NaN and `fyl2x` of a zero (§5.2) rest on X and S, the unit tests and A against B; the approximate class rests on the
C library (§5.1); a temporary read before written in an effect (§5.1) and the control word mapping rest on the unit
tests. Planting each of review R1's bugs into a copy of A (an off-by-one table bound, a temporary mask of 0x1FF,
refusing v inputs, refusing scalar forms, `cmp` reading slot 2 always, stale by input count, scalar noise refused,
table 8 as c, table 9 read as 0, the 256th input refused, `log`'s zero test through slot 0 or after its operand,
`log` keeping a NaN's sign, `def` taking 4 dwords, `mov` quieting, `rcp` and `rsq` swapped, 24 bits run at 53, states
of a FLOAT type dropped or made errors): the comparison against the oracle splits on every one, except `log` of a
zero giving +0, `neg` keeping a signalling NaN and a signalling memory NaN winning, which the unit tests catch.

Review round 2's plants (each in a copy of A, and the six marked "both" in A and B alike, so only the oracle or the
unit tests can see them; splits against the oracle): an INT scatter that rounds to nearest (1,646..10,482 on the
copies, both too), floors (3,713, 6,150) or saturates at 32 bits (1,646..3,991); `fxp_eval` ignoring the precision
(559 on log, 56 on rsq, both too); a float conversion that truncates (49..206), ties away (6) or saturates (40, 63);
quotients without their extra bits (21 corner cases), a square root without its sticky bit (12), a tie's sticky bit
ignored (47), a subtraction without its borrow (12); add and mul loading the wrong operand (33, both too) and the
scalar dot (15, both too); noise reading n components (3, both too) or k inputs (2); the 4 `fend` input table back
(2,700 on `pad`, both too); unknown temporaries not tracked (4); and reverting the type refusal, the effect's unknown
temporaries or the control word's RC check, which the unit tests catch (3, 1 and 2 checks). Before the round the
scatter and precision plants passed every check, A against B included, and the float conversion's split only 1-2 unit
checks; the quotient, square-root and borrow plants passed everything; the tie's sticky bit split 7 oracle cases;
add's operand order split only A against B (one case), the scalar dot's and noise's nowhere. The only plant that
passes now is a BOOL made 0 or 1, as A refuses BOOL values.

So the done-when "results equal the oracle's for every preshader on recorded inputs" holds for every one of the 616
in isolation and for 587 of them through D3DX's own BeginPass, also with every `mov` made each of seven computing
opcodes, on **generated** inputs written down as `fxpcase` and `fxpround` files (one seed, edge and random values). No
values the game itself sets exist yet (the game does not run to its menus); once M5-07 records them they go through
the same `fxpround` format and the same comparison.

## 8. UG2's `runtime/d3dx_hle.c`: what it teaches, what can be shared

`d3dx_hle.c` has no preshader interpreter of its own: it times, memoizes and verifies the **lifted** x87 code of
UG2's statically linked D3DX (BeginPass 0x66ACD2 → state apply 0x669286 → value 0x664A1F → evaluation 0x663A32 →
the interpreter at 0x688E79, 48 % of a race frame's guest time). What carries over:

- **Its structure is like this DLL's, its behaviour is not**: UG2's 0x663A32 sets the precision on the first
  evaluation inside BeginPass and the end of BeginPass restores it (d3dx_hle.c's verify comment). MW's 0x5036A4 has
  the same kind of switch, but it switches only when its third argument is set (§3.4), and under the oracle no
  BeginPass after `Begin(D3DXFX_DONOTSAVESTATE)` ever did: MW's BeginPass evaluates at the caller's control word. M5-05
  must not carry UG2's "BeginPass runs at 64 bits" over. Expect the same bytecode (UG2's 294 FXLC preshaders, GAME
  §5.3); `fxpres.c` could replace UG2's lifted interpreter behind UG2's own verify switch, if UG2 ever wants that
  (not part of M5).
- **The memo lessons (L-084)**: stage 1 (one state value from a full copy of its inputs, compared with memcmp, no
  hash) and stage 2 (device calls) were right over millions of verified hits; stage 3 (skipping whole passes) was
  wrong in 78 % of verified hits. For MW the memo belongs in M5-05, keyed on the model's versions of a state's own
  inputs (M§2.6, `fxm_version`) instead of memory snapshots, with `MWFXVERIFY=<n>` re-running `fxp_state_value` on
  every n-th hit and comparing the dwords. `fxpres` itself keeps no result, only the decoded programs
  (`fxp_effect_new`), which depend on the bytes alone. Its cost per evaluation is in §4.4.
- **What cannot be shared**: every address, record layout (UG2's 100-byte state records, its dirty flags at
  +0x0/+0x4 of 32-byte input records) and offset is the 2004 static D3DX's; MW's D3DX is a different build in a DLL.
  The lifter's x87 precision emulation (X87PC, `X87R`) rounds lifted code's doubles; `fxpres` rounds its own integer
  model, so the two share the rule (L-041), not code.

## 9. Not pinned down; for M5-05

- **Which D3DX paths take the 64-bit flag** (§3.4): seen in Begin's state-block recording (16 of 72 Begin calls under
  `Begin(0)`); BeginPass after `Begin(D3DXFX_DONOTSAVESTATE)` never. The call graph is known (§3.4: only 0x50622D
  passes a flag, from bit 20 of the state record's flags); what sets that bit, and so which states of which paths
  run at 64 bits, is not. M5-05 traces it under the oracle for CommitChanges and Begin(0) before it picks `pc`. No
  value of MW's depends on it (§3.4).
- **When D3DX evaluates at all**: with `Begin(0)`, 82 passes of the 24-bit rounds that evaluate under
  `D3DXFX_DONOTSAVESTATE` did not run the interpreter in BeginPass (the values came from recording the state block in
  the first round); M5-05's dirty rules (M§2.6) decide.
- **MATRIX_COLUMNS parameters and non-square MATRIX_ROWS in a preshader** (§4.1): X suggests the parameter's class
  decides, but only MW's 4 × 4 MATRIX_ROWS was run; a synthetic effect through the oracle would settle it.
- **Sets b and i in a preshader**: D3DX gathers them (0x502A38, the int part of 0x50269A); the model refuses them
  (M§3.2). Not in MW.
- **A CTAB entry that names no parameter** makes 0x4FB2E7 fail (0x4FB3DB: E_FAIL, the effect is refused), where W
  skips it (M§3.2). MW: every constant matches (**V**). For M5-05's creation path.
- **The x87's tie between two quiet NaNs with equal significands** (§3.1): the SDM is silent; A follows QEMU.
- **0x711** (the second noise form): no name in the DLL's disassembler and no producer known.
- **The scatter beyond MW's shapes** (§4.2): arrays, more than 4 rows, BOOL and structs are read from the code (X)
  and refused by A; a synthetic effect through the oracle would settle them.
- **The game thread's rounding control**: A refuses RC ≠ nearest and the reserved PC field (§4.4). Whether MW ever
  evaluates under another RC is not known; M5-05 sees it as `FXP_E_ARG` from `fxp_pc_from_cw`'s 0, not as a silently
  different value. SetVector's colour form (fx-model §2.5) takes only the precision too.
- **Temporaries in an effect**: D3DX's hold what the stack held (in practice the previous evaluation at the same
  depth); A stops at such a read (§5.1). MW's programs never read one.
