#!/usr/bin/env python3
"""The packs' check: every file the packs add to the game folder or change in it, read as the game reads it, so that
a broken pack is never written or copied to a server (the game trusts its files: an offset a pack gets wrong is a
read past a block, as on 2026-11-06).

    tools/packcheck.py <game folder> [<folder> ...]

Each <folder> holds VINYLPACKS/, MODPACKS/, LANGPACKS/, TEXPACKS/ or HQVINYLS/ (or is one of them); without one, the game
folder's own.  A problem is a line "<file>: <what is wrong>", and the exit status is 1; 0 when all is well.  The
pack tools (vinylpacks.py, modpacks.py, damagepack.py, whinepack.py, langpacks.py, texpacks.py, hqvinyls.py) run
the same check
(check() below) on what they made before they write it.

What it checks, by folder:
  VINYLPACKS/<game file>, MODPACKS/<game file>   a splice ('VPAK', runtime/fileio.c vp_open): its size is 32 + head +
        tail, its base size the game file's; the game reads head + the game file from cut + tail, and that file is
        checked as the game's own kind of file (below), the game's own textures in it as they were
  MODPACKS/<new file>       a file the game folder lacks, read as the game's (a rim brand's model pack): its chunks
  MODPACKS/PARTS.BIN        as runtime/modpacks.c reads it, and what it names: each rim's texture in
        CARS/WHEELS/TEXTURES.BIN, each brand's icon in GLOBAL/GLOBALTEXTURES{NA,ROW}.BIN and its model pack
  MODPACKS/DAMAGE/*.TPK     one texture pack, as runtime/damage.c hands it to the game's loader
  MODPACKS/WHINE/*.wav      16-bit mono PCM, as runtime/whine.c reads it
  LANGPACKS/<language>/<game file>   read in place of the game's: its chunks, texture packs and texts
  TEXPACKS/*.pak            as runtime/web/texrep_web.c reads it: its entries, their levels' sizes, version 2's
        deflated levels
  HQVINYLS/CARS/<car>.hqv   as the runtime and the page read it (tools/hqvinyls.py): its rows sorted by hash, each
        picture inside the file after the rows, none overlapping, each an AVIF of 1 or 3 channels
and in every file the game reads (a spliced or replaced one): its chunks lie inside the file, inside the chunk that
holds them; and in each texture pack (TPK, chunk 0xB3300000) in it:
  - its tables: the hash list sorted, its count the TextureInfos' (an uncompressed pack's) or the blocks' (a
    compressed one's); every image and palette inside the pack's data, every block inside the file
  - a block that is not the game's (a pack's, or one a language pack changed): JDLZ that runtime/overrides.c's
    jdlz_native unpacks to exactly its entry's size (the game's own loop has no bounds), or RAWW of that size; its
    TextureInfo (its last 0x9C bytes: the TextureInfo and the platform info) its entry's, its image as big as its
    size, format and levels need, image and palette inside the block
  - the textures' places: as a compressed texture comes in, the game points every texture of the pack whose image
    or palette offset (TextureInfo +0x30, +0x34) lies in the new block's place [image offset, palette offset + its
    size) (without a palette: [image offset, + its size)) into that block and makes it from there (0x494C10 ->
    0x494A50, signed compares): a texture whose offset lies in another's place is read from the wrong block, past
    its end when it is bigger (tools/vinylpacks.py's first packs, at 0x20000000: up to 230 KB, a trap in 0x5CE170;
    runtime/vinylpacks.c).  So a pack's texture's place holds no other texture's offsets and lies in no place of the
    game's textures still in the file.  The game's places are their places in an unpacked copy of the pack: in the
    table's order, each the size of its image and palette, at a 0x80 boundary (the 2623 JDLZ blocks of the game
    folder's texture packs all have them so).  Its HUFF blocks are not unpacked here: the place of one lies between
    its neighbours' in the table (or the copy's end), all of which counts as theirs.  The vinyl packs made before
    8473add3 (from 0x20000000, among a VINYLS.BIN's own) are named as such, and checked as runtime/vinylpacks.c
    moves them.
  - an uncompressed pack's images do not overlap where a pack's texture is one of them
Every memory file (GLOBAL/*MemoryFile.bin: headers the game reads in place of the file's own) that lists a spliced or
replaced file must hold that file's header as it is served, at its size.
"""
import argparse, os, struct, sys, zlib
from concurrent.futures import ProcessPoolExecutor

KINDS = ('VINYLPACKS', 'MODPACKS', 'LANGPACKS', 'TEXPACKS', 'HQVINYLS')
TPK, TPK_INFO, TPK_DATA = 0xB3300000, 0xB3310000, 0xB3320000
T_HASHES, T_PACKED, T_INFOS, T_PLAT = 0x33310002, 0x33310003, 0x33310004, 0x33310005
T_PIXELS = 0x33320002
TI_SIZE, PF_SIZE, TAIL = 0x7C, 0x20, 0x9C          # TextureInfo, platform info, both at a block's end
NONE = 0xFFFFFFFF
FOURCC = {0x31545844: 8, 0x33545844: 16, 0x35545844: 16}   # DXT1, DXT3, DXT5: bytes a 4x4 block
FMT = {21: 4, 22: 4, 28: 1, 41: 1}                 # A8R8G8B8, X8R8G8B8, A8, P8: bytes a pixel
FMT_NAME = {21: 'A8R8G8B8', 22: 'X8R8G8B8', 28: 'A8', 41: 'P8', 0x31545844: 'DXT1', 0x33545844: 'DXT3', 0x35545844: 'DXT5'}
VPAK, MP_NEEDS_MEMFILE = b'VPAK', 1
MEMFILE_MAGIC = 0x53219999
MAX_UNPACKED = 64 << 20                            # a block bigger than this unpacked is no texture's
CHUNKED = ('.bin', '.bun', '.lzc', '.tpk')         # the game's chunk files (the rest: sound banks, movies, texts)


def u32(d, o): return struct.unpack_from('<I', d, o)[0]


def s32(v): return v - (1 << 32) if v & 0x80000000 else v


def read(path):
    with open(path, 'rb') as f: return f.read()


def bstring_hash(s):
    """the game's name hash (bStringHash)"""
    h = 0xFFFFFFFF
    for c in s.encode('latin1'): h = (h * 33 + c) & 0xFFFFFFFF
    return h


def find(root, rel):
    """rel under root, each part matched case-insensitively; None if missing"""
    cur = root
    for part in rel.replace('\\', '/').split('/'):
        if not part: continue
        if not os.path.isdir(cur): return None
        hit = next((n for n in os.listdir(cur) if n.lower() == part.lower()), None)
        if hit is None: return None
        cur = os.path.join(cur, hit)
    return cur


# --- chunks -------------------------------------------------------------------
def chunks(d, b, e):
    """the chunks [b, e) holds, each (id, data start, data end); and what is wrong with them, or None.  The game's
    files have chunks one after another to their container's end, each inside it."""
    out, o = [], b
    while o < e:
        if o + 8 > e: return out, f'{e - o} bytes at 0x{o:x} are no chunk'
        cid, sz = struct.unpack_from('<II', d, o)
        if sz > e - o - 8: return out, f'chunk 0x{cid:08x} at 0x{o:x} says {sz} bytes, {e - o - 8} are left'
        out.append((cid, o + 8, o + 8 + sz)); o += 8 + sz
    return out, None


def chunk_tree(d, b, e, depth=0):
    """what is wrong with the chunks in [b, e) and in the containers among them (id with 0x80000000); the TPKs
    found, as (start, end) of their data"""
    kids, why = chunks(d, b, e)
    probs, found = ([why] if why else []), []
    for cid, cb, ce in kids:
        if cid == TPK: found.append((cb, ce))
        elif cid & 0x80000000 and depth < 8:
            p, f = chunk_tree(d, cb, ce, depth + 1)
            probs += p; found += f
    return probs, found


# --- the unpackers --------------------------------------------------------------
def jdlz_native(b):
    """(the bytes, None) as runtime/overrides.c jdlz_native unpacks the JDLZ block b (as the game read it: its entry's
    size), or (None, why) where it would not (and the game's own loop, which has no bounds, would run) or where the
    output would not be whole (the rest of it as the memory had it)"""
    if len(b) < 0x12 or b[:4] != b'JDLZ': return None, 'not a JDLZ block'
    if b[4] != 2: return None, f'JDLZ version {b[4]}, not 2: the game unpacks nothing'
    size, csize = struct.unpack_from('<II', b, 8)
    if csize < 0x12: return None, f'its size says {csize} bytes, less than its header'
    if csize > len(b): return None, f'its size says {csize} bytes, its entry {len(b)}: read past the block'
    if size > MAX_UNPACKED: return None, f'{size} bytes unpacked: no texture is that big'
    out = bytearray(size)
    s, o, left = 0x12, 0, csize - 0x12
    f1, f2 = b[0x10] | 0x100, b[0x11] | 0x100
    while left:
        if f1 & 1:
            if s + 2 > csize: return None, f'a match past the block\'s end (byte {s})'
            b0, b1 = b[s], b[s + 1]
            if f2 & 1: ln, dist = ((b0 & 0xF0) << 4 | b1) + 3, (b0 & 0x0F) + 1
            else: dist, ln = ((b0 & 0xE0) << 3 | b1) + 17, (b0 & 0x1F) + 3
            if o < dist: return None, f'a match {dist} bytes back at byte {o} of the output: before its start'
            if ln > size - o: return None, f'a match of {ln} bytes at byte {o}: past the output\'s end ({size})'
            st = o - dist
            if dist >= ln: out[o:o + ln] = out[st:st + ln]
            else: out[o:o + ln] = (out[st:o] * (ln // dist + 1))[:ln]
            o += ln; s += 2; left -= 2; f2 >>= 1
            if left < 0: return None, 'its byte count runs past 0 (the game\'s loop would run on)'
        else:
            if o < size:
                if s >= csize: return None, 'a byte past the block\'s end'
                out[o] = b[s]; o += 1; s += 1
            left -= 1
        f1 >>= 1
        if f1 == 1:
            if not left or s >= csize: return None, 'a flag byte past the block\'s end'
            f1 = b[s] | 0x100; s += 1; left -= 1
        if f2 == 1:
            if not left or s >= csize: return None, 'a flag byte past the block\'s end'
            f2 = b[s] | 0x100; s += 1; left -= 1
    if o != size: return None, f'{o} of its {size} bytes: the rest of the output as the memory had it'
    return bytes(out), None


def unpack(blob, size):
    """(the block's bytes as the game unpacks it into its entry's size, None) or (None, why): JDLZ under the native
    unpacker's rules, RAWW; another kind (HUFF, COMP: the game's own code) is not unpacked here"""
    tag = blob[:4]
    if tag == b'JDLZ':
        if len(blob) >= 12 and u32(blob, 8) != size:
            return None, f'says it unpacks to {u32(blob, 8)} bytes, its entry {size} (the streamer allocates the entry\'s)'
        u, why = jdlz_native(blob)
        if u is not None and len(u) != size: return None, f'JDLZ of {len(u)} bytes, its entry says {size}'
        return u, why
    if tag == b'RAWW':                              # 0x5744F0: version 1, [+8] bytes from +0x10, no bounds
        if len(blob) < 16 or blob[4] != 1: return None, 'RAWW not of version 1: the game unpacks nothing'
        n = u32(blob, 8)
        if n != size: return None, f'RAWW of {n} bytes, its entry says {size}'
        if 16 + n > len(blob): return None, f'RAWW of {n} bytes in a block of {len(blob)}: read past it'
        return blob[16:16 + n], None
    if tag in (b'HUFF', b'COMP'): return None, f'{tag.decode()}: the game\'s own unpacker, no bounds (a pack\'s blocks are JDLZ or RAWW)'
    return None, f'a block of kind {tag!r}: the game unpacks nothing'


# --- textures -----------------------------------------------------------------
class Tex:
    """a TextureInfo (0x7C bytes) and its platform info (0x20)"""
    def __init__(self, ti, pf):
        self.name = ti[0x0C:0x24].split(b'\0')[0].decode('latin1')
        self.hash = u32(ti, 0x24)
        self.img, self.pal, self.isz, self.psz = struct.unpack_from('<4I', ti, 0x30)
        self.w, self.h = struct.unpack_from('<HH', ti, 0x44)
        self.levels = ti[0x4E]
        self.fmt = u32(pf, 0x14) if len(pf) >= 0x18 else 0
        self.ti, self.pf = bytes(ti), bytes(pf)

    def label(self): return self.name or f'texture 0x{self.hash:08x}'

    def need(self):
        """the bytes its levels take, as the game makes it (0x5CE170) and uploads it; None for a format this does not
        know"""
        n = 0
        for lv in range(max(self.levels, 1)):
            w, h = max(1, self.w >> lv), max(1, self.h >> lv)
            if self.fmt in FOURCC: n += ((w + 3) // 4) * ((h + 3) // 4) * FOURCC[self.fmt]
            elif self.fmt in FMT: n += w * h * FMT[self.fmt]
            else: return None
        return n

    def place(self):
        """the place 0x494C10 gives 0x494A50 when this one's block comes in: (start, end), signed as it compares"""
        n = (self.psz + self.pal - self.img) & 0xFFFFFFFF if self.pal != NONE else self.isz
        return s32(self.img), s32(self.img) + s32(n)

    def offsets(self):
        """the offsets 0x494A50 looks for in a place: the image's (with an image), the palette's (with a palette)"""
        return ([s32(self.img)] if self.isz else []) + ([s32(self.pal)] if self.psz else [])

    def own_problems(self, room=None):
        """what is wrong with it as a pack's texture: its image's size, its palette; room: its block's bytes before
        the TextureInfo (a compressed pack's), the image and palette inside them"""
        p = []
        need = self.need()
        if not self.w or not self.h: p.append(f'{self.w}x{self.h}')
        if need is None: p.append(f'format 0x{self.fmt:x}, which this does not check')
        elif self.isz < need: p.append(f'an image of {self.isz} bytes, {self.w}x{self.h} {FMT_NAME[self.fmt]} in {max(self.levels, 1)} levels takes {need}')
        if self.fmt == 41 and (self.pal == NONE or self.psz < 1024): p.append('P8 without a palette of 256 colours')
        if room is not None:
            if self.isz > room: p.append(f'an image of {self.isz} bytes in a block of {room}')
            if self.psz and (self.pal == NONE or s32(self.pal - self.img) < 0 or s32(self.pal - self.img) + self.psz > room):
                p.append(f'its palette at {s32(self.pal - self.img)} (+{self.psz}) outside its block of {room} bytes')
            a, b = self.place()
            if a < 0 or b > 0x7FFFFFFF: p.append(f'its place 0x{self.img:x} .. 0x{(b & 0xFFFFFFFF):x} is past the game\'s signed compares')
        return p


def tex_of_block(u):
    """the TextureInfo at an unpacked block's end"""
    return Tex(u[-TAIL:-PF_SIZE], u[-PF_SIZE:])


# --- texture packs (TPK) --------------------------------------------------------
class TPKView:
    """a TPK chunk's tables: hashes, and either rows (a compressed pack's: hash, offset, size, unpacked, flags, 0) or
    textures (an uncompressed one's: Tex, with pix the pixels' start); probs: what is wrong with them"""
    def __init__(self, d, b, e):
        self.d, self.b, self.e = d, b, e
        self.probs, self.hashes, self.rows, self.texs, self.pix, self.data = [], [], None, None, None, None
        kids, why = chunks(d, b, e)
        if why: self.probs.append(f'the texture pack at 0x{b - 8:x}: {why}')
        info = [k for k in kids if k[0] == TPK_INFO]
        data = [k for k in kids if k[0] == TPK_DATA]
        if len(info) != 1 or len(data) != 1:
            self.probs.append(f'the texture pack at 0x{b - 8:x}: {len(info)} info and {len(data)} data chunks, not one each'); return
        part, why = chunks(d, info[0][1], info[0][2])
        if why: self.probs.append(f'the texture pack at 0x{b - 8:x}\'s info: {why}')
        part = {c: (cb, ce) for c, cb, ce in part}
        dpart, why = chunks(d, data[0][1], data[0][2])
        if why: self.probs.append(f'the texture pack at 0x{b - 8:x}\'s data: {why}')
        dpart = {c: (cb, ce) for c, cb, ce in dpart}
        if T_HASHES not in part or T_PIXELS not in dpart:
            self.probs.append(f'the texture pack at 0x{b - 8:x}: no hash list or no data'); return
        hb, he = part[T_HASHES]
        if (he - hb) % 8: self.probs.append(f'the texture pack at 0x{b - 8:x}: a hash list of {he - hb} bytes')
        self.hashes = [u32(d, hb + 8 * k) for k in range((he - hb) // 8)]
        if self.hashes != sorted(self.hashes) or len(set(self.hashes)) != len(self.hashes):
            self.probs.append(f'the texture pack at 0x{b - 8:x}: its hash list is not sorted (the game looks textures up in it)')
        self.data = dpart[T_PIXELS]
        n = len(self.hashes)
        if T_PACKED in part:
            rb, re_ = part[T_PACKED]
            if re_ - rb != 24 * n:
                self.probs.append(f'the texture pack at 0x{b - 8:x}: {(re_ - rb) / 24:g} blocks, {n} hashes')
            self.rows = [struct.unpack_from('<6I', d, rb + 24 * k) for k in range(min(n, (re_ - rb) // 24))]
            for k, r in enumerate(self.rows):
                if r[0] != self.hashes[k]: self.probs.append(f'block {k}\'s hash 0x{r[0]:08x}, the list\'s 0x{self.hashes[k]:08x}'); break
            ends = sorted((r[1], r[1] + r[2], r[0]) for r in self.rows)
            for a, z, h in ends:
                if a < self.data[0] or z > self.data[1]:
                    self.probs.append(f'block 0x{h:08x} at 0x{a:x} (+{z - a}) outside the pack\'s data 0x{self.data[0]:x} .. 0x{self.data[1]:x}')
            for (a, z, h), (a2, z2, h2) in zip(ends, ends[1:]):
                if a2 < z: self.probs.append(f'blocks 0x{h:08x} and 0x{h2:08x} overlap in the file')
            for r in self.rows:
                if r[3] < TAIL: self.probs.append(f'block 0x{r[0]:08x}: {r[3]} bytes unpacked, less than a TextureInfo')
        elif T_INFOS in part and T_PLAT in part:
            (tb, te), (fb, fe) = part[T_INFOS], part[T_PLAT]
            if te - tb != TI_SIZE * n or fe - fb != PF_SIZE * n:
                self.probs.append(f'the texture pack at 0x{b - 8:x}: {(te - tb) / TI_SIZE:g} TextureInfos and {(fe - fb) / PF_SIZE:g} platform infos, {n} hashes')
            self.pix = (self.data[0] + 0x7F) & ~0x7F
            self.texs = []
            for k in range(min(n, (te - tb) // TI_SIZE, (fe - fb) // PF_SIZE)):
                t = Tex(d[tb + TI_SIZE * k:tb + TI_SIZE * (k + 1)], d[fb + PF_SIZE * k:fb + PF_SIZE * (k + 1)])
                if t.hash != self.hashes[k]: self.probs.append(f'{t.label()}: its hash 0x{t.hash:08x}, the list\'s 0x{self.hashes[k]:08x}')
                for what, o, z in (('image', t.img, t.isz), ('palette', t.pal, t.psz)):
                    if z and (o == NONE or self.pix + o + z > self.data[1] or self.pix + o < self.data[0]):
                        self.probs.append(f'{t.label()}: its {what} at 0x{o:x} (+{z}) outside the pack\'s data')
                self.texs.append(t)
        else:
            self.probs.append(f'the texture pack at 0x{b - 8:x}: neither a block table nor TextureInfos')

    def blob(self, r): return self.d[r[1]:r[1] + r[2]]


def layout(view):
    """a game file's compressed pack: each row's texture (Tex) where this unpacks its block, None for the rest; and the
    end of their places (the unpacked copy's size, each place at a 0x80 boundary)"""
    texs, total = [], 0
    for r in view.rows:
        total += (r[3] - TAIL + 0x7F) & ~0x7F
        b = view.blob(r)
        u, _ = unpack(b, r[3]) if b[:4] in (b'JDLZ', b'RAWW') else (None, None)
        texs.append(tex_of_block(u) if u is not None and len(u) >= TAIL else None)
    end = max([total] + [t.place()[1] for t in texs if t])
    return texs, end


def unknown_spans(texs, end, kept):
    """where the blocks this does not unpack (and still in the file: kept[k]) have their places: each between the
    known places before and after it in the table (the game's are in its order), merged; all of [0, end) where the
    known ones are not in order"""
    known = [t.place() for t in texs if t]
    if any(b[0] < a[0] for a, b in zip(known, known[1:])):
        return [(0, end)] if any(t is None and kept[k] for k, t in enumerate(texs)) else []
    spans, lo = [], 0
    for k, t in enumerate(texs):
        if t: lo = max(lo, t.place()[1]); continue
        if not kept[k]: continue
        hi = next((q.place()[0] for q in texs[k + 1:] if q), end)
        if spans and spans[-1][1] >= lo: spans[-1] = (spans[-1][0], max(spans[-1][1], hi))
        else: spans.append((lo, hi))
    return spans


# the vinyl packs' first places (tools/vinylpacks.py until 8473add3): from OLD_IMG, OLD_STEP each, among a VINYLS.BIN's
# own; runtime/vinylpacks.c tex_in moves a texture of theirs (named <CAR>_ART_F_28 .. 34) to NEW_IMG + the same
OLD_IMG, OLD_STEP, NEW_IMG, OLD_N = 0x20000000, 0x56000, 0x30000000, 14


def first_place(t):
    """t at the vinyl packs' first places, as runtime/vinylpacks.c pack_texture() knows them: moved, or None"""
    at = t.name.find('_ART_F_')
    k = t.name[at + 7:at + 9] if at >= 0 else ''
    if not (k.isdigit() and 28 <= int(k) < 35 and t.name[at + 9:at + 10] in ('', '_')): return None
    if not OLD_IMG <= t.img < OLD_IMG + OLD_N * OLD_STEP: return None
    m = Tex(bytearray(t.ti), t.pf)
    m.img += NEW_IMG - OLD_IMG
    if m.pal != NONE: m.pal += NEW_IMG - OLD_IMG
    return m


def check_tpk(d, b, e, game=None, splice=False, vinyls=False):
    """what is wrong with the TPK chunk data [b, e) of d (a file as the game reads it); game: the game file's own
    bytes (the textures in both are the game's); splice: every texture of the game file must be in it, as it was;
    vinyls: a vinyl pack's (its textures at the first packs' places moved as the runtime moves them)"""
    v = TPKView(d, b, e)
    probs = list(v.probs)
    gviews = []
    if game is not None:
        _, gt = chunk_tree(game, 0, len(game))
        gviews = [TPKView(game, gb, ge) for gb, ge in gt]
    if v.rows is not None:
        grows = {}                                      # the game's blocks: hash -> (view, row index)
        for gv in gviews:
            if gv.rows is not None:
                for k, r in enumerate(gv.rows): grows.setdefault(r[0], (gv, k))
        mine = {r[0]: r for r in v.rows}
        theirs, ours = [], []
        for k, r in enumerate(v.rows):
            g = grows.get(r[0])
            if g and g[0].blob(g[0].rows[g[1]]) == v.blob(r) and g[0].rows[g[1]][3] == r[3]: theirs.append(k)
            else: ours.append(k)
        if splice:
            for gv in gviews:
                if gv.rows is None: continue
                for r in gv.rows:
                    m = mine.get(r[0])
                    if not m or v.blob(m) != gv.blob(r) or m[3] != r[3]:
                        probs.append(f'the game\'s texture 0x{r[0]:08x} is not as the game file has it'); break
        # the pack's textures (and the game's this unpacks, still in the file): their places
        texs, known, spans, moved = {}, [], [], []
        for k in ours:
            r = v.rows[k]
            u, why = unpack(v.blob(r), r[3])
            if u is None: probs.append(f'block 0x{r[0]:08x} at 0x{r[1]:x} (+{r[2]}): {why}'); continue
            t = tex_of_block(u)
            if t.hash != r[0]: probs.append(f'block 0x{r[0]:08x} holds the TextureInfo of 0x{t.hash:08x} ({t.label()})'); continue
            probs += [f'{t.label()}: {p}' for p in t.own_problems(r[3] - TAIL)]
            m = first_place(t) if vinyls else None
            if m: moved.append(t.label()); t = m
            texs[k] = t
        if moved:
            probs.append(f'{len(moved)} textures ({moved[0]} ...) at the first vinyl packs\' places (from 0x{OLD_IMG:x}, among '
                         f'the game\'s own): runtime/vinylpacks.c moves them as they come in, a module without that reads '
                         f'past blocks; rebuild with tools/vinylpacks.py')
        kept_hashes = {v.rows[k][0] for k in theirs}
        for gv in {id(g[0]): g[0] for g in grows.values()}.values():
            gtexs, end = layout(gv)
            kept = [r[0] in kept_hashes for r in gv.rows]
            spans += unknown_spans(gtexs, end, kept)
            known += [(t, True) for t, keep in zip(gtexs, kept) if t and keep]
        known += [(t, False) for t in texs.values()]
        for t in texs.values():
            a, z = t.place()
            for u, game_s in known:
                if u is t: continue
                for o in u.offsets():
                    if a <= o < z:
                        probs.append(f'{t.label()}: its place 0x{a:x} .. 0x{z:x} holds {"the game\'s " if game_s else ""}{u.label()}\'s '
                                     f'offset 0x{o & 0xFFFFFFFF:x}: as its block comes in the game makes {u.label()} from it, '
                                     f'{max(0, o - a + u.isz - (z - a))} bytes past its end')
                ua, uz = u.place()
                for o in t.offsets():
                    if ua <= o < uz:
                        probs.append(f'{t.label()}: its offset 0x{o & 0xFFFFFFFF:x} lies in {"the game\'s " if game_s else ""}{u.label()}\'s place '
                                     f'0x{ua:x} .. 0x{uz:x}: the game makes it from that block')
            for sa, sz in spans:
                if a < sz and sa < z:
                    probs.append(f'{t.label()}: its place 0x{a:x} .. 0x{z:x} lies among the game\'s own textures\' '
                                 f'(0x{sa:x} .. 0x{sz:x}, blocks this does not unpack): the game makes theirs from its block or its from theirs')
                    break
    elif v.texs is not None:
        gtex = {}
        for gv in gviews:
            for t in gv.texs or []: gtex.setdefault(t.hash, (gv, t))
        ours, ranges = [], []
        for t in v.texs:
            g = gtex.get(t.hash)
            same = g and g[1].ti == t.ti and g[1].pf == t.pf and \
                game[g[0].pix + g[1].img:g[0].pix + g[1].img + g[1].isz] == d[v.pix + t.img:v.pix + t.img + t.isz]
            if not same: ours.append(t); probs += [f'{t.label()}: {p}' for p in t.own_problems()]
            for o, z in ((t.img, t.isz), (t.pal, t.psz)):
                if z and o != NONE: ranges.append((o, o + z, t, not same))
        if splice:
            have = {t.hash: t for t in v.texs}
            for gv in gviews:
                for g in gv.texs or []:
                    t = have.get(g.hash)
                    if not t or t.ti != g.ti or d[v.pix + t.img:v.pix + t.img + t.isz] != game[gv.pix + g.img:gv.pix + g.img + g.isz]:
                        probs.append(f'the game\'s texture {g.label()} is not as the game file has it'); break
        ranges.sort(key=lambda x: (x[0], x[1]))
        for i, (a, z, t, mine_) in enumerate(ranges):
            for a2, z2, t2, mine2 in ranges[i + 1:]:
                if a2 >= z: break
                if (mine_ or mine2) and t2 is not t:
                    probs.append(f'{t.label()} and {t2.label()}: their pixels overlap (0x{a:x} .. 0x{z:x}, 0x{a2:x} .. 0x{z2:x})')
    return probs


# --- the kinds of file ------------------------------------------------------------
def check_chunked(rel, d, game=None, splice=False, vinyls=False):
    """a chunk file the game reads (rel: its game path), as served (d) and as the game folder has it (game)"""
    if d[:4] == b'JDLZ':                            # a whole compressed bundle (.lzc): the bundle loader's unpack
        u, why = jdlz_native(d)
        if u is None: return [f'the file is JDLZ: {why}']
        d = u
        if game is not None and game[:4] == b'JDLZ': game, _ = jdlz_native(game)
    probs, tpks = chunk_tree(d, 0, len(d))
    for b, e in tpks:
        probs += check_tpk(d, b, e, game, splice, vinyls)
    if rel.lower().startswith('languages/') and not rel.lower().endswith('textures.bin'):
        probs += check_texts(d)
    return probs


def check_game_file(rel, d, game=None, splice=False, vinyls=False):
    """a file of the game's (rel: its game path) as served (d) and as the game folder has it (game), by its kind"""
    if rel.lower().endswith('memoryfile.bin'): return check_memfile(d)[0]
    if rel.lower().endswith(CHUNKED): return check_chunked(rel, d, game, splice, vinyls)
    return []


def check_texts(d):
    """a LANGUAGES/*.bin's texts (chunk 0x39000): its table and its strings inside the chunk, sorted by hash"""
    probs = []
    for cid, b, e in chunks(d, 0, len(d))[0]:
        if cid != 0x39000: continue
        if e - b < 16: return ['its texts\' chunk has no header']
        _, n, toff, soff = struct.unpack_from('<4I', d, b)
        if toff + 8 * n > e - b or soff > e - b: return [f'its {n} texts\' table (at {toff}) or strings (at {soff}) past their chunk']
        last = -1
        for k in range(n):
            h, o = struct.unpack_from('<II', d, b + toff + 8 * k)
            if h < last: probs.append(f'text {k} (0x{h:08x}) out of the hash order'); break
            last = h
            s = b + soff + o
            if s >= e or d.find(b'\0', s, e) < 0: probs.append(f'text 0x{h:08x}\'s string at {o} runs past its chunk'); break
    return probs


def vpak(pack, base):
    """(the file as the game reads it: head + base from cut + tail, the header's words) or (None, why)"""
    if len(pack) < 32 or pack[:4] != VPAK: return None, 'not a splice (\'VPAK\'): the runtime reads the game\'s file alone'
    w = struct.unpack_from('<8I', pack, 0)
    if w[1] != 1: return None, f'a splice of version {w[1]}: the runtime reads the game\'s file alone'
    if len(pack) != 32 + w[4] + w[5]:
        return None, f'{len(pack)} bytes, its header says 32 + {w[4]} + {w[5]} = {32 + w[4] + w[5]}: cut short or grown'
    if base is None: return None, 'the game folder has no such file to add to'
    if w[2] != len(base): return None, f'made for a game file of {w[2]} bytes, the game folder\'s has {len(base)}: the runtime reads the game\'s alone'
    if w[3] > w[2]: return None, f'its cut {w[3]} is past the game file\'s end'
    if w[6] not in (0, MP_NEEDS_MEMFILE) or w[7]: return None, f'header words {w[6]}, {w[7]}: not ones the runtime knows'
    return pack[32:32 + w[4]] + base[w[3]:] + pack[32 + w[4]:], w


def check_memfile(d):
    """a memory file (GLOBAL/*MemoryFile.bin): its list and the headers it holds inside it; (problems, entries)"""
    if len(d) < 16 or u32(d, 8) != MEMFILE_MAGIC: return ['not a memory file as the game\'s'], []
    n = u32(d, 12)
    if 16 + 20 * n > len(d): return [f'{n} entries do not fit its {len(d)} bytes'], []
    ents = [struct.unpack_from('<5I', d, 16 + 20 * k) for k in range(n)]
    probs = [f'entry 0x{h:08x}\'s header at 0x{o:x} (+{hs}) past its end' for h, o, sz, hs, fl in ents if o + hs > len(d)]
    return probs, ents


def wav_problems(b):
    """why b is not a sound runtime/whine.c plays (16-bit mono PCM, at most 4 MB), or []"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from whinepack import wav_check
    why = wav_check('', b)
    return ([why] if why else []) + ([f'{len(b)} bytes, past the 4 MB the runtime reads'] if len(b) > 4 << 20 else [])


def check_parts(d, served):
    """MODPACKS/PARTS.BIN as runtime/modpacks.c load() reads it; served(rel): a file as the game reads it, for what the
    parts name"""
    if len(d) < 32 or d[:4] != b'MPRT' or u32(d, 4) != 1: return ['not a parts list (\'MPRT\', version 1)']
    nm, np_, nt, ns = struct.unpack_from('<4I', d, 8)
    want = 32 + 32 * nm + 128 * np_ + 8 * nt + ns
    if nm > 8 or np_ > 4096 or want != len(d) or (ns and d[-1]): return [f'{nm} mods, {np_} parts, {nt} texts, {ns} bytes of strings: not its {len(d)} bytes']
    strs = d[want - ns:]
    def s(o): return strs[o:strs.index(b'\0', o)].decode('latin1') if o < ns else None
    probs, parts = [], []
    for i in range(np_):
        q = 32 + 32 * nm + 128 * i
        key, kind, cl, b6, mod, owner, name = struct.unpack_from('<IBBBBII', d, q)
        has_model, mbase = d[q + 18] == 1, u32(d, q + 20)
        mstr = struct.unpack_from('<8I', d, q + 24)
        nattr = u32(d, q + 56)
        attrs = [struct.unpack_from('<II', d, q + 60 + 8 * k) for k in range(min(nattr, 8))]
        bad = mod >= nm or nattr > 8 or s(name) is None or kind in (0x30, 0x32) or (has_model and mbase != NONE and s(mbase) is None)
        bad = bad or any(x != NONE and s(x) is None for x in mstr)
        if bad: probs.append(f'part {i} (0x{key:08x}) is not one runtime/modpacks.c takes'); continue
        parts.append((key, kind, s(name), s(mbase) if has_model and mbase != NONE else None, dict(attrs)))
    # what they name: a rim's texture, a brand's icon and model pack
    brands = {p[0]: p[2] for p in parts if p[1] == 67}
    wheels = served('CARS/WHEELS/TEXTURES.BIN')
    hashes = set()
    if wheels:
        for b, e in chunk_tree(wheels, 0, len(wheels))[1]: hashes |= set(TPKView(wheels, b, e).hashes)
    for key, kind, name, base, attrs in parts:
        if kind == 28:
            brand = brands.get(attrs.get(bstring_hash('BRAND_NAME')))
            if brand is None or base is None: continue
            tex = f'{brand}{base}_WHEEL'
            if attrs.get(bstring_hash('TEXTURE_NAME')) != bstring_hash(f'{brand}{base}'): probs.append(f'rim {name}: its TEXTURE_NAME is not {brand}{base}\'s')
            elif wheels and bstring_hash(tex) not in hashes: probs.append(f'rim {name}: no {tex} in CARS/WHEELS/TEXTURES.BIN as the game reads it')
        elif kind == 67:
            if not served(f'CARS/WHEELS/GEOMETRY_{name}.BIN'): probs.append(f'brand {name}: no CARS/WHEELS/GEOMETRY_{name}.BIN')
            for rel in ('GLOBAL/GLOBALTEXTURESNA.BIN', 'GLOBAL/GLOBALTEXTURESROW.BIN'):
                g = served(rel)
                if g and bstring_hash(f'VISUAL_RIMS_BRAND_{name}') not in {h for b, e in chunk_tree(g, 0, len(g))[1] for h in TPKView(g, b, e).hashes}:
                    probs.append(f'brand {name}: no VISUAL_RIMS_BRAND_{name} in {rel} as the game reads it')
    return list(dict.fromkeys(probs))                  # (a rim in each of its sizes: once)


def level_bytes(w, h, fmt, levels):
    """the bytes of an entry's levels as runtime/web/gpu_web.c uploads them (texrep_web.c ent_bytes); 0: none"""
    if not w or not h or w > 4096 or h > 4096 or not levels or levels > 13: return 0
    if fmt in FOURCC: blk, bpb = 4, FOURCC[fmt]
    elif fmt in FMT: blk, bpb = 1, FMT[fmt]
    else: return 0
    n = 0
    for lv in range(levels):
        lw, lh = max(1, w >> lv), max(1, h >> lv)
        n += ((lw + blk - 1) // blk) * bpb * ((lh + blk - 1) // blk)
    return n


def check_texpack(d):
    """a TEXPACKS/*.pak as runtime/web/texrep_web.c reads it"""
    if len(d) < 16 or d[:4] != b'NTXP': return ['not a texture pack (\'NTXP\')']
    ver, n = u32(d, 4), u32(d, 8)
    if ver not in (1, 2) or not n or n > 65536: return [f'version {ver}, {n} entries: not a pack the runtime reads']
    esz = 40 if ver == 1 else 48
    if 16 + esz * n > len(d): return [f'{n} entries past its end']
    ents = [struct.unpack_from('<IIHHIHHIIIII', d, 16 + 40 * k) for k in range(n)]
    bases = [struct.unpack_from('<II', d, 16 + 40 * n + 8 * k) for k in range(n)] if ver == 2 else [(0, 0)] * n
    names_at = 16 + esz * n
    first = min(e[9] for e in ents)
    probs, places = [], {(e[9], e[10]) for e in ents}
    for k, (crc, adler, ow, oh, ofmt, w, h, fmt, levels, off, size, name) in enumerate(ents):
        nm = d[name:d.find(b'\0', name, first)].decode('latin1') if names_at <= name < first and d.find(b'\0', name, first) >= 0 else None
        label = nm or f'entry {k}'
        if nm is None: probs.append(f'entry {k}: its name at {name} is not in the names')
        want = level_bytes(w, h, fmt, levels)
        if not want: probs.append(f'{label}: {w}x{h} format 0x{fmt:x} in {levels} levels: none the runtime makes'); continue
        if off + size > len(d) or off < names_at: probs.append(f'{label}: its levels at {off} (+{size}) outside the pack'); continue
        if ver == 1:
            if size != want: probs.append(f'{label}: {size} bytes of levels, {w}x{h} {FMT_NAME.get(fmt)} in {levels} takes {want}')
            continue
        bo, bs = bases[k]
        if bool(bo) != bool(bs): probs.append(f'{label}: a base at {bo} of {bs} bytes'); continue
        if bs and (bo, bs) not in places: probs.append(f'{label}: its base at {bo} (+{bs}) is no entry\'s levels')
        for what, o, z in (('levels', off, size), ('base', bo, bs)):
            if not z: continue
            try:
                zo = zlib.decompressobj(-15)
                got = zo.decompress(d[o:o + z], want + 1)
                ok = len(got) == want and zo.eof
            except zlib.error as x:
                ok, got = False, str(x)
            if not ok: probs.append(f'{label}: its {what} do not inflate to {want} bytes')
    return probs


# --- the packs, as the game reads them ------------------------------------------------
HQV_MAGIC, HQV_ROW, HQV_MAX = b'HQV1', 16, 4096


def check_hqv(d):
    """a HQVINYLS/CARS/<car>.hqv (tools/hqvinyls.py) as the runtime and the page read it: [why]"""
    if len(d) < 16 or d[:4] != HQV_MAGIC: return ['not a HQV1 file']
    ver, n, start = struct.unpack_from('<III', d, 4)
    if ver != 1: return [f'version {ver}: this reads version 1']
    if not 0 < n <= HQV_MAX: return [f'{n} pictures: 1 to {HQV_MAX}']
    if start < 16 + HQV_ROW * n or start > len(d): return [f'its pictures start at {start}, inside its {n} rows or past the file']
    probs, last, spans = [], -1, []
    for i in range(n):
        nh, off, size, side, ch, z = struct.unpack_from('<IIIHBB', d, 16 + HQV_ROW * i)
        if nh <= last: probs.append(f'row {i} (0x{nh:08x}): not sorted by hash'); break
        last = nh
        if side != 1024 or ch not in (1, 3) or z: probs.append(f'row {i} (0x{nh:08x}): side {side}, {ch} channel(s)')
        if size < 12 or off < start or off + size > len(d): probs.append(f'row {i} (0x{nh:08x}): {size} bytes at {off}, outside the pictures'); continue
        if d[off + 4:off + 12] not in (b'ftypavif', b'ftypavis'): probs.append(f'row {i} (0x{nh:08x}): not an AVIF')
        spans.append((off, off + size, nh))
    spans.sort()
    for (a0, a1, ah), (b0, b1, bh) in zip(spans, spans[1:]):
        if b0 < a1: probs.append(f'0x{ah:08x} and 0x{bh:08x} overlap'); break
    return probs[:20]


class Packs:
    """the pack files to check ({'VINYLPACKS/CARS/350Z/VINYLS.BIN': bytes, or the path of the file}) and the game
    folder (None: without it)"""
    def __init__(self, game, files):
        self.game, self.files = game, {k.replace('\\', '/'): v for k, v in files.items()}
        self.low = {k.lower(): k for k in self.files}
        self._game, self._served = {}, {}

    def data(self, key):
        v = self.files[key]
        return bytes(v) if isinstance(v, (bytes, bytearray)) else read(v)

    def game_file(self, rel):
        """the game folder's own file rel, or None"""
        k = rel.lower()
        if k not in self._game:
            p = find(self.game, rel) if self.game else None
            self._game[k] = read(p) if p and os.path.isfile(p) else None
        return self._game[k]

    def pack_of(self, rel):
        """(key, folder) of the splice of the game file rel the runtime reads: VINYLPACKS' first, then MODPACKS'"""
        for kind in ('VINYLPACKS', 'MODPACKS'):
            k = self.low.get(f'{kind}/{rel}'.lower())
            if k: return k, kind
        return None, None

    def served(self, rel, lang=None):
        """the file rel as the game reads it (a language's pack, the game's file with its splice, a mod pack's own
        file), or None"""
        key = (rel.lower(), lang)
        if key not in self._served:
            out = None
            lk = self.low.get(f'LANGPACKS/{lang}/{rel}'.lower()) if lang else None
            if lk: out = self.data(lk)
            else:
                base = self.game_file(rel)
                pk, _ = self.pack_of(rel)
                if base is not None:
                    out = base
                    if pk:
                        v, w = vpak(self.data(pk), base)
                        if v is not None and not (w[6] == MP_NEEDS_MEMFILE and not self.memfile_spliced()): out = v
                elif pk and pk.startswith('MODPACKS/'): out = self.data(pk)
            self._served[key] = out
        return self._served[key]

    def memfile_spliced(self):
        k = self.low.get('modpacks/global/globalmemoryfile.bin')
        return bool(k and self.game_file('GLOBAL/GlobalMemoryFile.bin') is not None and
                    vpak(self.data(k), self.game_file('GLOBAL/GlobalMemoryFile.bin'))[0] is not None)

    def memfiles(self, lang=None):
        """the memory files the game reads: {rel: bytes}"""
        g = find(self.game, 'GLOBAL') if self.game else None
        names = [n for n in os.listdir(g) if n.lower().endswith('memoryfile.bin')] if g else []
        return {f'GLOBAL/{n}': self.served(f'GLOBAL/{n}', lang) for n in names}

    def listed(self, rel, served, lang=None):
        """what is wrong with how the memory files list the game file rel (served: its bytes as the game reads it)"""
        h, probs = bstring_hash(rel.upper().replace('/', '\\')), []
        for mrel, m in self.memfiles(lang).items():
            if m is None or mrel.lower() == rel.lower(): continue
            _, ents = check_memfile(m)
            for eh, o, size, hs, fl in ents:
                if eh != h: continue
                if size != len(served) or m[o:o + hs] != served[:hs]:
                    probs.append(f'{mrel} holds this file\'s header as the game folder has it ({size} bytes): the game '
                                 f'reads that one, not the one served ({len(served)} bytes)')
        return probs

    def check_one(self, key):
        """what is wrong with the pack file key: [(key, why)]"""
        kind, _, rest = key.partition('/')
        try:
            if kind == 'HQVINYLS':
                return [(key, p) for p in (check_hqv(self.data(key)) if key.lower().endswith('.hqv') else [])]
            if kind == 'TEXPACKS':
                return [(key, p) for p in (check_texpack(self.data(key)) if key.lower().endswith('.pak') else [])]
            if kind == 'LANGPACKS':
                lang, _, rel = rest.partition('/')
                if not rel.lower().endswith(CHUNKED): return []
                d, probs = self.data(key), []
                game = self.game_file(rel)
                pk, pkind = self.pack_of(rel)
                if pk: probs.append(f'in place of {rel}, which {pkind} adds to: in this language the game reads it without that')
                probs += check_game_file(rel, d, game)
                probs += self.listed(rel, d, lang)
                return [(key, p) for p in probs]
            if kind == 'MODPACKS':
                low = rest.lower()
                if low == 'parts.bin': return [(key, p) for p in check_parts(self.data(key), self.served)]
                if low.startswith('whine/'): return [(key, p) for p in (wav_problems(self.data(key)) if low.endswith('.wav') else [])]
                if low.startswith('damage/') and low.endswith('.tpk'):
                    d = self.data(key)
                    if len(d) < 8 or u32(d, 0) != TPK or u32(d, 4) + 8 != len(d):
                        return [(key, 'not one texture pack chunk the size of the file (runtime/damage.c reads only that)')]
                    return [(key, p) for p in check_chunked(rest, d)]
            if kind not in ('VINYLPACKS', 'MODPACKS'): return []
            rel = rest
            base = self.game_file(rel) if self.game else None
            d = self.data(key)
            if base is None and kind == 'MODPACKS' and self.game:     # a file the game folder lacks: served as the game's
                if d[:4] == VPAK: return [(key, 'a splice of a file the game folder does not have')]
                return [(key, p) for p in check_game_file(rel, d) + self.listed(rel, d)]
            if not self.game:                                          # (without it: what the file alone says)
                if d[:4] != VPAK: return [(key, p) for p in check_game_file(rel, d)]
                ok = len(d) >= 32 and len(d) == 32 + u32(d, 16) + u32(d, 20)
                return [] if ok else [(key, f'{len(d)} bytes, its header says 32 + head + tail: cut short or grown')]
            v, w = vpak(d, base)
            if v is None: return [(key, w)]
            probs = []
            if kind == 'MODPACKS' and self.pack_of(rel)[1] == 'VINYLPACKS':
                probs.append('VINYLPACKS has a splice of this file too: the runtime reads only that one')
            if w[6] == MP_NEEDS_MEMFILE and not self.memfile_spliced():
                probs.append('needs MODPACKS/GLOBAL/GlobalMemoryFile.bin\'s splice, which is not there or does not fit: the runtime reads the game\'s file alone')
            probs += check_game_file(rel, v, base, splice=True, vinyls=kind == 'VINYLPACKS')
            probs += self.listed(rel, v)
            return [(key, p) for p in probs]
        except Exception as x:                                         # (a file too broken to read is one too)
            return [(key, f'cannot be read: {type(x).__name__}: {x}')]


def check(game, files, jobs=1):
    """the problems of the pack files `files` ({'VINYLPACKS/CARS/350Z/VINYLS.BIN': bytes or a path}) as the game reads
    them with the game folder `game` (None: the checks that need it are left out): [(file, why)]"""
    P = Packs(game, files)
    keys = sorted(P.files)
    if jobs > 1 and len(keys) > 1:
        with ProcessPoolExecutor(jobs) as ex:
            res = list(ex.map(_one, [(game, files, k) for k in keys]))
    else:
        res = [P.check_one(k) for k in keys]
    return [p for r in res for p in r]


def _one(job):
    game, files, key = job
    return Packs(game, files).check_one(key)


def walk(folders):
    """the pack files under the folders (each holding VINYLPACKS/ ... or being one): {key: path}"""
    out = {}
    for f in folders:
        tops = [os.path.join(f, n) for n in os.listdir(f) if n.upper() in KINDS and os.path.isdir(os.path.join(f, n))]
        if not tops and os.path.basename(os.path.normpath(f)).upper() in KINDS: tops = [f]
        for top in tops:
            kind = os.path.basename(os.path.normpath(top)).upper()
            for d, _, names in os.walk(top):
                for n in names:
                    if n.endswith(('.new', '.part', '.tmp')): continue
                    p = os.path.join(d, n)
                    out[f'{kind}/' + os.path.relpath(p, top).replace(os.sep, '/')] = p
    return out


def main():
    ap = argparse.ArgumentParser(usage='tools/packcheck.py <game folder> [<folder> ...]')
    ap.add_argument('game')
    ap.add_argument('folders', nargs='*', help='folders with VINYLPACKS/, MODPACKS/, LANGPACKS/, TEXPACKS/ or HQVINYLS/ (default: the game folder)')
    ap.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 1))
    a = ap.parse_args()
    if not find(a.game, 'SPEED2.EXE'): sys.exit(f'{a.game}: no SPEED2.EXE, not the game folder')
    files = walk(a.folders or [a.game])
    probs = check(a.game, files, a.jobs)
    for k, why in probs: print(f'{k}: {why}')
    kinds = sorted({k.split('/')[0] for k in files})
    print(f'packs: {len(files)} files ({", ".join(kinds) or "none"}), '
          f'{"all good" if not probs else f"{len(probs)} problem" + ("s" if len(probs) != 1 else "")}')
    sys.exit(1 if probs else 0)


if __name__ == '__main__':
    main()
