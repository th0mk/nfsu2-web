# Most Wanted's alias save: the file format

Status: **2026-10-30**, the clean-room spec of PLAN D-21 (TRACKER M6-22): what a page needs to read, check, edit and
re-seal a Most Wanted (2005, PC, v1.3) save, as the format, not as the game's code. Facts only: offsets are offsets in
the file, nothing here is game data, and no code of the game was copied. Consumers: the page's save code for Most
Wanted (`web/src/games/mw/`, behind the same interface as Underground 2's `web/src/app/profile.ts`), the vault's size
caps (M6-29).

**Marks.**

| Mark | Source |
|---|---|
| **V** | Verified on nine alias saves the game wrote itself in test runs: two new aliases, prologue, free roam and career saves of blacklist rivals 15 to 13. Every V below held in all nine. |
| **S** | Read from what the game's save and load code does (static reading; not run) |
| **I** | Inferred, not checked |

## 1. Where

- One file per alias: `Documents\NFS Most Wanted\<alias>\<alias>`, with no extension. **S**
- Every save is **63,596 bytes** (0xF86C), whatever the career's state. **V**

All numbers are little-endian. "CRC" is the game's CRC-32 (§4), "MD5" is RFC 1321's.

## 2. The container

The EA memory-card container (the `20CM` family Underground 2 also uses): a header, block A, block B.

| Offset | Size | Field | |
|---|---|---|---|
| 0x00 | 4 | magic, the bytes `20CM` (u32 0x4D433032) | **V S** |
| 0x04 | 4 | the file's size: 0x1C + size of A + size of B (0xF86C) | **V S** |
| 0x08 | 4 | size of block A (8) | **V S** |
| 0x0C | 4 | size of block B (0xF848) | **V S** |
| 0x10 | 4 | CRC of block A | **V S** |
| 0x14 | 4 | CRC of block B | **V S** |
| 0x18 | 4 | CRC of the header's first 0x18 bytes (0x00..0x17) | **V S** |
| 0x1C | 8 | block A | **V** |
| 0x24 | 0xF848 | block B, the game's data | **V** |

- Block A is two u32: 0x10D (269), then block B's size again (0xF848). That 0x10D is a format version is **I**.
- The writer computes the CRC of B, then of A, fills the header with the CRC field at 0, then computes the header's
  CRC over 0x00..0x17 and stores it at 0x18. **S**
- The loader refuses the file (an error code) unless: the size field equals the file's size, the magic is `20CM`, the
  header's CRC matches, and the CRCs of A and B match. **S**
- The header's 0x10..0x1B (two CRCs) differ between nearly any two saves; they are no per-save stamp. **V**

## 3. Block B

Offsets below are in the file (block B starts at 0x24).

| Offset | Size | Field | |
|---|---|---|---|
| 0x24 | 16 | zero | **V** |
| 0x34 | 16 | MD5 of 0x44..0x4033 (0x3FF0 bytes) | **V** |
| 0x44 | 0x3FF0 | the first part of the game's data, starting with the four bytes `Game` | **V** |
| 0x4034 | 0xB828 | the rest of the game's data | **V** |
| 0x4039 | 4 | the cash: 0 in a new alias, 3,000 in the first safehouse's save (its cash in the game's menu), then growing with the career | **V** values, **I** meaning |
| 0x5A31 | ≤ 0x24 | the alias's name, ASCII, NUL-padded (zero up to 0x5A54 in all nine) | **V** |
| 0xF85C | 16 | MD5 of 0x34..0xF85B: from the first MD5 up to this one (block B's bytes 0x10 to its last 16) | **V S** |

- The game writes the MD5 at 0xF85C over block B from 0x10 to its last 16 bytes, and checks it on load. **S** (that the
  bytes are exactly MD5 of that range is **V**).
- Who writes and who checks the MD5 at 0x34 was not traced: that it is the MD5 of 0x44..0x4033 is **V**. It is the same
  in two new aliases of different names (the name lies outside its range), changes as the career goes on, and stays
  the same between two race wins that changed only bytes after 0x4034.
- The name is the only place the alias's name was found in the file (searched in all nine). **V**
- A save is tied to the game's gameplay data files: players report saves "damaged" after replacing them. Which bytes
  depend on them is not known. **I**

## 4. The CRC

CRC-32 with the polynomial 0x04C11DB7, most significant bit first (the table's entries 1..3 are 0x04C11DB7,
0x09823B6E, 0x0D4326D9), in the form that shifts the message into the register without appended zeros:

1. Fewer than four bytes: the CRC is 0.
2. The register starts as the first four bytes read big-endian, inverted.
3. For each byte after them: `r = ((r << 8) | byte) ^ table[r >> 24]` (32-bit).
4. The CRC is the register inverted.

So the CRC of exactly four bytes is those bytes as a big-endian u32, and the common CRC-32/BZIP2 of a message equals
this CRC of the same message followed by four zero bytes. Check values: `123456789` → 0x74527081; `123456789` and four zero bytes →
0xFC891918 (CRC-32/BZIP2's check value); `abcd` → 0x61626364. **V** (the three CRCs of all nine saves)

## 5. Re-sealing an edited save

After a change to block B's game data, in this order (each later field covers the earlier):

1. if anything in 0x44..0x4033 changed, the MD5 at 0x34;
2. the MD5 at 0xF85C (0x34..0xF85B);
3. the CRC of block B at 0x14;
4. the header's CRC at 0x18 (0x00..0x17).

Block A, the sizes and the magic stay. **V**: sealing each of the game's own saves again this way gives it back byte
for byte (13 saves), and a save of the first safehouse with its cash changed and its name changed (0x5A31, NUL-padded),
sealed again so, loaded in the game, which showed the changed cash in the safehouse menu (2026-10-30).

## 6. Size and the vault

63,596 bytes an alias, the same for every alias and career state seen. Four aliases unpacked (254,384 bytes) fit the
vault's 256 KB per version; the page's packed size and the cap are M6-29's to measure and set.
