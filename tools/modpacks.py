#!/usr/bin/env python3
"""The mod packs: MODPACKS/ in the game folder, what community mods add to the
game (new parts and their files), for every player alike.  Everyone's game
loads all of it, so a friend's car with a mod's part looks the same on every
screen; each mod's switch (a settings.ini key) only decides whether its parts
are offered in the shops (runtime/modpacks.c).

    tools/modpacks.py <game folder> --mod <mod folder> [--brands BLITZ,...] [--out <folder>]

<mod folder> is the PC mod with the rims, extracted: the folder with
"1 - Mod Files" in it, or that folder itself.  Of it, only the rim brands named (default: BLITZ)
are taken, each with all of its rims: their parts in the mod's car part
database, the brand's model pack, the rims' textures and the brand's icon.
Nothing the mod changes in the game's own parts is taken: the mod also takes
out some of the game's rims and brands, which stay here.

What it writes under <out> (default: the game folder):
  MODPACKS/PARTS.BIN        the parts, for runtime/modpacks.c (below)
  MODPACKS/CARS/WHEELS/GEOMETRY_<BRAND>.BIN   a new brand's model pack, the mod's file as it is
  MODPACKS/CARS/WHEELS/TEXTURES.BIN           what the game's CARS/WHEELS/TEXTURES.BIN gains: the
                            rims' textures and the brands' icons, a pack as tools/vinylpacks.py makes
                            them ('VPAK', read by runtime/fileio.c)
  MODPACKS/GLOBAL/GLOBALTEXTURES{NA,ROW}.BIN  the brands' icons (the rim shop's brand list), in the texture
                            packs the game keeps loaded whole (a VPAK each)
  MODPACKS/GLOBAL/GlobalMemoryFile.bin        the game's memory file without that file's header (a
                            VPAK too): the game keeps the header of CARS/WHEELS/TEXTURES.BIN in it, and
                            would read the textures' list from there, without the new ones

PARTS.BIN (little-endian):
  header   'MPRT', version 1, mods, parts, texts, strings' size, 0, 0
  mods     32 bytes each: the settings.ini key's environment name (NFSU2_...) that switches the mod's
           parts on in the shops
  parts    PART_SZ bytes each (the last word: the stand-in, a game part's key, 0 the slot's stock part):
           key, kind, category and level (the game's byte), its +6 byte, mod (index),
           owner (bStringHash of it), name, the model record (its first two bytes, 1 if the part has one, 0,
           base name, 8 names),
           attributes (count, then 8 of {hash, value})
  texts    {hash, string}: texts the game's language table gains (a brand's name)
  strings  NUL-terminated; a part's strings are offsets into them, 0xFFFFFFFF none
Every string a part names is an offset into strings; runtime/modpacks.c puts
them into the part database's own string table.
"""
import argparse, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from texpacks import bstring_hash, chunks, find, jdlz, tpks  # noqa: E402
from vinylpacks import read_tpk, splice, parse_tpk, jdlz_pack, jdlz_game, MAGIC, VERSION as VPAK_VERSION  # noqa: E402

PART_MAGIC, PART_VERSION = b'MPRT', 1
PART_SZ = 4 + 4 + 4 + 4 + 4 + 4 + 32 + 4 + 64 + 4
NONE = 0xFFFFFFFF
CHUNK_PARTS = 0x80034602
SUB = {0x34603: 'hdr', 0x34606: 'str', 0x3460C: 'list', 0x34605: 'attr', 0x3460A: 'model', 0x3460B: 'owner', 0x34604: 'part'}
KIND_RIM, KIND_BRAND = 28, 67
A_TEXTURE, A_OUTER, A_INNER, A_BRAND = (bstring_hash(n) for n in ('TEXTURE_NAME', 'OUTER_RADIUS', 'INNER_RADIUS', 'BRAND_NAME'))
RIM_ATTRS = {A_TEXTURE, A_OUTER, A_INNER, A_BRAND}     # a rim's attributes: all plain values (no string indices)
OWNER_WHEELS = bstring_hash('WHEELS')
RIMS_SWITCH = ('NFSU2_MOD_RIMS',)                      # the rims' switch (web/src/app/settings.ts)
# A mod rim's look-alike among the game's (picked from the rim shop's renders): its stand-in, in the same size, where
# the game shows no mod parts (a profile, so the original game and pages of before; the Original view of other
# players' cars; a game without MODPACKS).  A mod part without one stands in with the slot's stock part.
LOOKALIKES = {
    'BLITZ_STYLE01': ('AVUS', 'AF-502'),                # Blitz BRW03: five split spokes
    'BLITZ_STYLE02': ('RACINGHART', 'CP8'),             # Blitz Technospeed Z1: ten thin spokes
}
IMG_BASE = 0x30000000                                  # the new textures' TextureInfo image offsets (each its own)


def load(path):
    d = open(path, 'rb').read()
    return jdlz(d) if d[:4] == b'JDLZ' else d


def find_chunk(d, cid, off=0, end=None, depth=0):
    end = len(d) if end is None else end
    for c, b, e in chunks(d, off, end):
        if c == cid: return b, e
        if c & 0x80000000 and depth < 4:
            r = find_chunk(d, cid, b, e, depth + 1)
            if r: return r
    return None


class PartDB:
    """the car part database of a GLOBALB (chunk 0x80034602), as runtime/vinylpacks.c reads it"""
    def __init__(self, path):
        d = load(path)
        r = find_chunk(d, CHUNK_PARTS)
        if not r: sys.exit(f'{path}: no car part database')
        s = {SUB.get(c, c): d[b:e] for c, b, e in chunks(d, *r)}
        if not all(k in s for k in SUB.values()): sys.exit(f'{path}: a car part database laid out unlike the game\'s')
        self.s = s
        nattr, = struct.unpack_from('<I', s['hdr'], 0x20)
        nparts, = struct.unpack_from('<I', s['hdr'], 0x38)
        self.attrs = [struct.unpack_from('<II', s['attr'], 8 * i) for i in range(nattr)]
        self.owners = [struct.unpack_from('<I', s['owner'], 4 * i)[0] for i in range(len(s['owner']) // 4)]
        self.parts = []
        for i in range(nparts):
            key, kind, cl, b6, own, nm, li, mo = struct.unpack_from('<IBBBBHHH', s['part'], 14 * i)
            self.parts.append(dict(key=key, kind=kind, cl=cl, b6=b6, owner=self.owners[own], name=nm, list=li, model=mo))

    def str(self, i):
        s = self.s['str']
        return s[4 * i:s.index(b'\0', 4 * i)].decode('latin1')

    def attr_list(self, li):
        if li == 0xFFFF: return []
        L = self.s['list']
        n, = struct.unpack_from('<h', L, 2 * li)
        return [self.attrs[struct.unpack_from('<h', L, 2 * li + 2 + 2 * j)[0]] for j in range(n)]

    def model(self, mo):
        """(first two bytes, base name, the 8 names), or None; a record the game takes for none (its first byte 0:
        the loader and 0x60FF80 skip it) with no names (what it has there the game never reads)"""
        if mo == 0xFFFF: return None
        m = self.s['model'][0x24 * mo:0x24 * mo + 0x24]
        if not m[0]: return m[:2], None, [None] * 8
        base, = struct.unpack_from('<H', m, 2)
        names = struct.unpack_from('<8I', m, 4)
        return m[:2], (self.str(base) if base != 0xFFFF else None), [self.str(x) if x != NONE else None for x in names]


def tpk_textures(d, want):
    """the textures of the uncompressed TPKs in d that want(hash, name) takes (name: TextureInfo's, at most 23
    letters): hash -> (name, TextureInfo, platform info, pixels)"""
    out = {}
    for b, e in tpks(d, 0, len(d)):
        info = data = None
        for c, cb, ce in chunks(d, b, e):
            if c == 0xB3310000: info = (cb, ce)
            elif c == 0xB3320000: data = (cb, ce)
        if not info or not data: continue
        part = {c: (cb, ce) for c, cb, ce in chunks(d, *info)}
        pix = next(((cb + 0x7F) & ~0x7F for c, cb, ce in chunks(d, *data) if c == 0x33320002), None)
        if 0x33310004 not in part or 0x33310005 not in part or pix is None: continue
        (tb, te), (pb, _) = part[0x33310004], part[0x33310005]
        for i in range((te - tb) // 0x7C):
            ti = d[tb + i * 0x7C:tb + (i + 1) * 0x7C]
            h, = struct.unpack_from('<I', ti, 0x24)
            name = ti[0x0C:0x24].split(b'\0')[0].decode('latin1')
            if h in out or not want(h, name): continue
            img, pal, isz, psz = struct.unpack_from('<4I', ti, 0x30)
            if psz: sys.exit(f'texture {h:08x}: a palette texture, which this does not take')
            out[h] = (name, ti, d[pb + i * 0x20:pb + (i + 1) * 0x20], d[pix + img:pix + img + isz])
    return out


def texture_blob(ti, pf, pix, img_off):
    """a texture as a compressed TPK keeps one unpacked: its pixels, TextureInfo, platform info (as the game's
    CARS/WHEELS/TEXTURES.BIN: the palette's offset just past the image, no palette)"""
    ti = bytearray(ti)
    struct.pack_into('<II', ti, 0x30, img_off, img_off + len(pix))
    return pix + bytes(ti) + pf


def splice_plain(base, new):
    """(cut, head, tail): an uncompressed TPK (GLOBAL/GLOBALTEXTURESNA.BIN: every texture kept loaded) read as
    head + base[cut:] + tail has the textures new too (hash -> (TextureInfo, platform info, pixels)).  Its info chunk's
    hash list (sorted), TextureInfos and platform infos (in the list's order) each gain theirs; the pixels go after
    the game's, each at a 0x80 boundary, its TextureInfo's image offset from the pixels' start (a boundary too)."""
    S = len(base)
    top = list(chunks(base, 0, S))
    if len(top) != 1 or top[0][0] != 0xB3300000 or top[0][2] != S: sys.exit('a texture pack laid out unlike the game\'s')
    kids = list(chunks(base, top[0][1], S))
    ids = [c for c, _, _ in kids]
    if ids != [0, 0xB3310000, 0, 0xB3320000]: sys.exit('a texture pack laid out unlike the game\'s')
    (_, info_b, info_e), (_, pad_b, pad_e), (_, data_b, data_e) = kids[1], kids[2], kids[3]
    part = {c: (b, e) for c, b, e in chunks(base, info_b, info_e)}
    dpart = list(chunks(base, data_b, data_e))
    if list(part) != [0x33310001, 0x33310002, 0x33310004, 0x33310005] or [c for c, _, _ in dpart][-1] != 0x33320002:
        sys.exit('a texture pack laid out unlike the game\'s')
    (hb, he), (tb, te), (fb, fe) = part[0x33310002], part[0x33310004], part[0x33310005]
    pb, pe = dpart[-1][1], dpart[-1][2]
    n = (he - hb) // 8
    if te - tb != 0x7C * n or fe - fb != 0x20 * n or pe != S: sys.exit('a texture pack laid out unlike the game\'s')
    pix0 = (pb + 0x7F) & ~0x7F                         # the pixels' start (TextureInfo's image offsets from it)
    rows = {}
    for k in range(n):
        h, z = struct.unpack_from('<II', base, hb + 8 * k)
        if z or struct.unpack_from('<I', base, tb + 0x7C * k + 0x24)[0] != h: sys.exit('a texture pack whose lists do not agree')
        rows[h] = (base[tb + 0x7C * k:tb + 0x7C * (k + 1)], base[fb + 0x20 * k:fb + 0x20 * (k + 1)])
    grow = len(new) * (8 + 0x7C + 0x20)
    delta = (grow + 0x7F) & ~0x7F
    tail = bytearray()
    for h in sorted(new):
        if h in rows: sys.exit(f'texture {h:08x}: the game\'s pack has it already')
        ti, pf, px = new[h]
        tail += bytes((-(S + delta + len(tail))) % 0x80)
        ti = bytearray(ti)
        img = S + delta + len(tail) - (pix0 + delta)
        struct.pack_into('<II', ti, 0x30, img, 0)
        rows[h] = (bytes(ti), pf)
        tail += px
    tail += bytes((-(S + delta + len(tail))) % 0x80)
    keys = sorted(rows)
    T = len(tail)
    head = bytearray()
    head += struct.pack('<II', 0xB3300000, S + delta + T - 8)
    head += base[8:info_b - 8]
    head += struct.pack('<II', 0xB3310000, info_e - info_b + grow)
    head += base[info_b:hb - 8]
    head += struct.pack('<II', 0x33310002, 8 * len(keys)) + b''.join(struct.pack('<II', h, 0) for h in keys)
    head += struct.pack('<II', 0x33310004, 0x7C * len(keys)) + b''.join(rows[h][0] for h in keys)
    head += struct.pack('<II', 0x33310005, 0x20 * len(keys)) + b''.join(rows[h][1] for h in keys)
    head += struct.pack('<II', 0, pad_e - pad_b + delta - grow) + base[pad_b:pad_e] + bytes(delta - grow)
    head += struct.pack('<II', 0xB3320000, data_e - data_b + T)
    head += base[data_b:pb - 8]
    head += struct.pack('<II', 0x33320002, pe - pb + T)
    cut = pb
    assert len(head) == cut + delta, (hex(len(head)), hex(cut), hex(delta))
    return cut, bytes(head), bytes(tail)


def plain_textures(d):
    """an uncompressed TPK's textures as the game reads them: hash -> (TextureInfo, pixels from its image offset)"""
    out = {}
    for b, e in tpks(d, 0, len(d)):
        info = data = None
        for c, cb, ce in chunks(d, b, e):
            if c == 0xB3310000: info = (cb, ce)
            elif c == 0xB3320000: data = (cb, ce)
        part = {c: (cb, ce) for c, cb, ce in chunks(d, *info)}
        pix = next(((cb + 0x7F) & ~0x7F for c, cb, ce in chunks(d, *data) if c == 0x33320002))
        tb, te = part[0x33310004]
        for i in range((te - tb) // 0x7C):
            ti = d[tb + i * 0x7C:tb + (i + 1) * 0x7C]
            h, = struct.unpack_from('<I', ti, 0x24)
            img, _, isz = struct.unpack_from('<3I', ti, 0x30)
            out[h] = (ti, d[pix + img:pix + img + isz])
    return out


def lang_texts(path):
    """a LANGUAGES/*.bin's texts: hash -> text (Latin-1)"""
    d = open(path, 'rb').read()
    r = find_chunk(d, 0x39000)
    if not r: return {}
    b, _ = r
    _, n, toff, soff = struct.unpack_from('<4I', d, b)
    out = {}
    for k in range(n):
        h, o = struct.unpack_from('<II', d, b + toff + 8 * k)
        s = b + soff + o
        out[h] = d[s:d.index(b'\0', s)].decode('latin1')
    return out


def unlisted(path, rels):
    """a memory file (GLOBAL/*MemoryFile.bin: the headers of packs the game reads at its start, found by the hash of
    their path, 0x579870) without those of rels, so that the game reads them from the files (and their packs): the
    header as the file has it (0, 0, 0x53219999, count, then count entries {path hash, offset, the file's size,
    header size, flags}) with each one's hash another one's no file has.  (cut, head)"""
    d = open(path, 'rb').read()
    magic, n = struct.unpack_from('<II', d, 8)
    if magic != 0x53219999 or 16 + 20 * n > len(d): sys.exit(f'{path}: not a memory file as the game\'s')
    head, hit = bytearray(d[:16 + 20 * n]), set()
    for k in range(n):
        h, = struct.unpack_from('<I', d, 16 + 20 * k)
        for rel, size in rels:
            win = rel.upper().replace('/', '\\')
            if h != bstring_hash(win): continue
            if struct.unpack_from('<I', d, 16 + 20 * k + 8)[0] != size: sys.exit(f'{path}: its {win} is not the game folder\'s')
            struct.pack_into('<I', head, 16 + 20 * k, bstring_hash('MODPACKS:' + win))
            hit.add(rel)
    return len(head), bytes(head), hit


def mod_root(path):
    if find(path, 'CARS') and find(path, 'GLOBAL'): return path
    sub = next((os.path.join(path, n) for n in sorted(os.listdir(path)) if find(os.path.join(path, n), '1 - Mod Files/GLOBAL')), None)
    mod = find(sub, '1 - Mod Files') if sub else find(path, '1 - Mod Files')
    if not mod or not find(mod, 'GLOBAL'): sys.exit(f'{path}: no "1 - Mod Files/GLOBAL" in it')
    return mod


class Strings:
    def __init__(self): self.b, self.at = bytearray(), {}
    def __call__(self, s):
        if s is None: return NONE
        if s not in self.at:
            self.at[s] = len(self.b); self.b += s.encode('latin1') + b'\0'
        return self.at[s]


def main():
    ap = argparse.ArgumentParser(usage='tools/modpacks.py <game folder> --mod <mod folder> [--brands BLITZ,...] [--out <folder>]')
    ap.add_argument('game')
    ap.add_argument('--mod', metavar='MOD', help='the PC mod with the rims, extracted')
    ap.add_argument('--brands', default='BLITZ', help='its new rim brands to take (comma-separated)')
    ap.add_argument('--out', metavar='DIR', help='where MODPACKS/ goes (default: the game folder)')
    a = ap.parse_args()
    if not a.mod: sys.exit(__doc__)
    if not find(a.game, 'SPEED2.EXE'): sys.exit(f'{a.game}: no SPEED2.EXE, not the game folder')
    mod = mod_root(a.mod)
    game_db = PartDB(find(a.game, 'GLOBAL/GLOBALB.LZC') or find(a.game, 'GLOBAL/GLOBALB.BUN'))
    mod_db = PartDB(find(mod, 'GLOBAL/GLOBALB.LZC'))
    game_keys = {p['key'] for p in game_db.parts}
    gbrands = {p['key']: game_db.str(p['name']) for p in game_db.parts if p['kind'] == KIND_BRAND}
    game_rims = {}                                     # (brand, name, inner, outer) -> key
    for p in game_db.parts:
        if p['kind'] != KIND_RIM or p['owner'] != OWNER_WHEELS: continue
        ga = dict(game_db.attr_list(p['list']))
        game_rims[(gbrands.get(ga.get(A_BRAND)), game_db.str(p['name']), ga.get(A_INNER), ga.get(A_OUTER))] = p['key']
    mods = list(RIMS_SWITCH)
    S = Strings()
    parts, texts, want_tex, geo = [], {}, {}, []
    mod_lang = lang_texts(find(mod, 'LANGUAGES/English.bin'))
    for brand in [b.strip().upper() for b in a.brands.split(',') if b.strip()]:
        bp = [p for p in mod_db.parts if p['kind'] == KIND_BRAND and mod_db.str(p['name']) == brand]
        if len(bp) != 1: sys.exit(f'{brand}: the mod has {len(bp)} rim brands of that name')
        bp = bp[0]
        if bp['key'] in game_keys or any(p['kind'] == KIND_BRAND and game_db.str(p['name']) == brand for p in game_db.parts):
            sys.exit(f'{brand}: the game has this brand already (only new brands are taken)')
        rims = [p for p in mod_db.parts if p['kind'] == KIND_RIM and p['owner'] == OWNER_WHEELS
                and dict(mod_db.attr_list(p['list'])).get(A_BRAND) == bp['key']]
        if not rims: sys.exit(f'{brand}: no rims')
        path = find(mod, f'CARS/WHEELS/GEOMETRY_{brand}.BIN')
        if not path: sys.exit(f'{brand}: no CARS/WHEELS/GEOMETRY_{brand}.BIN in the mod')
        geo.append((brand, path))
        for p in [bp] + rims:
            if p['key'] in game_keys: sys.exit(f'{brand}: a rim\'s key {p["key"]:08x} is one of the game\'s parts')
            at = mod_db.attr_list(p['list'])
            if p['kind'] == KIND_RIM and ({h for h, _ in at} - RIM_ATTRS or len(at) > 8):
                sys.exit(f'{brand}: a rim with attributes this does not know: {[hex(h) for h, _ in at]}')
            if p['kind'] == KIND_BRAND and at: sys.exit(f'{brand}: a brand with attributes')
            if p['kind'] == KIND_RIM: want_tex[dict(at)[A_TEXTURE]] = brand
            m = mod_db.model(p['model'])
            name = mod_db.str(p['name'])
            if p['kind'] == KIND_RIM and name.upper().startswith(brand + ' '):
                name = name[len(brand) + 1:]            # the shop shows "<brand> <name>", as the game's own ("CH")
            stand = 0
            if p['kind'] == KIND_RIM and m and m[1] and f'{brand}{m[1]}' in LOOKALIKES:
                lb, ln = LOOKALIKES[f'{brand}{m[1]}']
                ra = dict(at)
                stand = game_rims.get((lb, ln, ra.get(A_INNER), ra.get(A_OUTER)), 0)
                if not stand: sys.exit(f'{brand}{m[1]} {ra.get(A_INNER)}/{ra.get(A_OUTER)}: no {lb} {ln} of that size in the game')
            parts.append(dict(key=p['key'], kind=p['kind'], cl=p['cl'], b6=p['b6'], owner=p['owner'],
                              name=S(name), model=m and (m[0], S(m[1]), [S(x) for x in m[2]]), attrs=at, stand=stand))
        name = mod_lang.get(bstring_hash(f'RIMS_BRAND_{brand}')) or brand
        texts[bstring_hash(f'RIMS_BRAND_{brand}')] = S(name)
    # the textures: each rim style's "<brand>_STYLEnn_..." (TEXTURE_NAME's value is the hash of "<brand>_STYLEnn";
    # its models draw "_WHEEL", the game's own rims have an "_WHEEL_INNER" too), the brands' icons
    # "VISUAL_RIMS_BRAND_<brand>" (the brand menu's, runtime/modpacks.c)
    styles = set()
    for p in parts:
        if p['kind'] != KIND_RIM: continue
        m = p['model']
        base = S.b[m[1]:S.b.index(b'\0', m[1])].decode('latin1') if m and m[1] != NONE else None
        brand = want_tex[dict(p['attrs'])[A_TEXTURE]]
        if not base: sys.exit(f'{brand}: a rim without a model name')
        st = f'{brand}{base}'
        if bstring_hash(st) != dict(p['attrs'])[A_TEXTURE]: sys.exit(f'{st}: not the rim\'s TEXTURE_NAME')
        styles.add(st)
    icons = {bstring_hash(f'VISUAL_RIMS_BRAND_{b}') for b, _ in geo}
    got = tpk_textures(load(find(mod, 'GLOBAL/GLOBALB.LZC')), lambda h, n: any(n.startswith(st + '_') for st in styles))
    got.update(tpk_textures(load(find(mod, 'FRONTEND/FrontB.lzc')), lambda h, n: h in icons))
    for st in sorted(styles):
        if bstring_hash(st + '_WHEEL') not in got: sys.exit(f'{st}_WHEEL: not in the mod\'s GLOBALB')
    for b, _ in geo:
        if bstring_hash(f'VISUAL_RIMS_BRAND_{b}') not in got: sys.exit(f'VISUAL_RIMS_BRAND_{b}: not in the mod\'s FrontB')
    base_path = find(a.game, 'CARS/WHEELS/TEXTURES.BIN')
    base, lay, ents = read_tpk(base_path)
    if ents is None: sys.exit(f'{base_path}: not a compressed TPK as the game\'s')
    icon_tex = {h: got.pop(h) for h in list(got) if h in icons}
    new = {}
    for k, h in enumerate(sorted(got)):
        if h in ents: sys.exit(f'{got[h][0]}: the game\'s CARS/WHEELS/TEXTURES.BIN has it already')
        raw = texture_blob(*got[h][1:], IMG_BASE + k * 0x100000)
        blob = jdlz_pack(raw)
        if jdlz_game(blob) != raw: sys.exit(f'{got[h][0]}: JDLZ does not read back as the game reads it')
        new[h] = (blob, len(raw))
    cut, head, tail = splice(base, lay, ents, new)
    virt = head + base[cut:] + tail
    _, _, vents = parse_tpk(virt)
    if vents is None or len(vents) != len(ents) + len(new): sys.exit('the textures pack does not read back')
    for h, e in ents.items():
        v = vents[h]
        if virt[v[1]:v[1] + v[2]] != base[e[1]:e[1] + e[2]] or v[3] != e[3]: sys.exit('a texture of the game\'s moved wrong')
    out = os.path.join(a.out or a.game, 'MODPACKS')
    os.makedirs(os.path.join(out, 'CARS', 'WHEELS'), exist_ok=True)

    def put(rel, data):
        dst = os.path.join(out, *rel.split('/'))
        tmp = dst + '.new'
        with open(tmp, 'wb') as f: f.write(data)
        os.replace(tmp, dst)

    os.makedirs(os.path.join(out, 'GLOBAL'), exist_ok=True)
    # (its 7th word 1: read only with the memory file's pack, which makes the game read this one's header from it)
    put('CARS/WHEELS/TEXTURES.BIN', struct.pack('<4s7I', MAGIC, VPAK_VERSION, len(base), cut, len(head), len(tail), 1, 0) + head + tail)
    # the brands' icons: in the packs the game keeps loaded whole (a compressed one's textures are unpacked only when
    # a car's load asks for them; the rim shop's brand list asks for none), both regions'
    for rel in ('GLOBAL/GLOBALTEXTURESNA.BIN', 'GLOBAL/GLOBALTEXTURESROW.BIN'):
        gp = find(a.game, rel)
        if not gp: sys.exit(f'{rel}: not in the game folder')
        gb = open(gp, 'rb').read()
        icut, ihead, itail = splice_plain(gb, {h: v[1:] for h, v in icon_tex.items()})
        iv = plain_textures(ihead + gb[icut:] + itail)
        og = plain_textures(gb)
        if any(iv.get(h) != og[h] for h in og) or any(iv.get(h, (0, 0))[1] != v[3] for h, v in icon_tex.items()):
            sys.exit(f'{rel}: the pack does not read back')
        put(rel, struct.pack('<4s7I', MAGIC, VPAK_VERSION, len(gb), icut, len(ihead), len(itail), 0, 0) + ihead + itail)
    # the game reads that pack's header from its memory file, as it was: no more
    mem = find(a.game, 'GLOBAL/GlobalMemoryFile.bin')
    mcut, mhead, hit = unlisted(mem, [('CARS/WHEELS/TEXTURES.BIN', len(base))])
    if not hit: sys.exit(f'{mem}: CARS/WHEELS/TEXTURES.BIN is not in it (the game\'s is)')
    os.makedirs(os.path.join(out, 'GLOBAL'), exist_ok=True)
    put('GLOBAL/GlobalMemoryFile.bin', struct.pack('<4s7I', MAGIC, VPAK_VERSION, os.path.getsize(mem), mcut, len(mhead), 0, 0, 0) + mhead)
    for brand, path in geo: put(f'CARS/WHEELS/GEOMETRY_{brand}.BIN', open(path, 'rb').read())
    body = bytearray(struct.pack('<4s7I', PART_MAGIC, PART_VERSION, len(mods), len(parts), len(texts), len(S.b), 0, 0))
    for m in mods: body += m.encode().ljust(32, b'\0')
    for p in parts:
        m = p['model']
        body += struct.pack('<IBBBBII', p['key'], p['kind'], p['cl'], p['b6'], 0, p['owner'], p['name'])
        body += (m[0] + b'\1\0' if m else b'\0\0\0\0') + struct.pack('<I', m[1] if m else NONE)
        body += struct.pack('<8I', *(m[2] if m else [NONE] * 8))
        at = p['attrs'] + [(0, 0)] * (8 - len(p['attrs']))
        body += struct.pack('<I', len(p['attrs'])) + b''.join(struct.pack('<II', h, v) for h, v in at)
        body += struct.pack('<I', p.get('stand', 0))
    for h in sorted(texts): body += struct.pack('<II', h, texts[h])
    body += S.b
    assert len(body) == 32 + 32 * len(mods) + PART_SZ * len(parts) + 8 * len(texts) + len(S.b)
    put('PARTS.BIN', bytes(body))
    print(f'{len(geo)} rim brand{"s" if len(geo) != 1 else ""} ({", ".join(b for b, _ in geo)}): {len(parts)} parts '
          f'({sum(1 for p in parts if p.get("stand"))} with a look-alike), '
          f'{len(new)} textures ({len(tail) / 1e6:.2f} MB: {", ".join(sorted(g[0] for g in got.values()))}), '
          f'{len(icon_tex)} icon{"s" if len(icon_tex) != 1 else ""}, '
          f'model packs {sum(os.path.getsize(p) for _, p in geo) / 1e6:.2f} MB: {out}')


if __name__ == '__main__':
    main()
