#!/usr/bin/env python3
"""HQ vinyls: HQVINYLS/CARS/<car>.hqv in the game folder, the vinyls of
AdrienShadow's "High Quality Vinyls & Doorlines (1024x Upscaled)"
(nfsmods.xyz/mod/8064) for the browser's runtime, which paints a car's skin a
second time at 1024x1024 from them when a player has the mod on (Mods tab).

    .venv/bin/python tools/hqvinyls.py <game folder> --mod <the mod's zip or folder> [--quality 50] [--jobs N]

<the mod's zip> is "High Quality Vinyls & Doorlines v1.0.zip" as it comes (5.8 GB), or the folder it unpacks to.
It needs numpy and Pillow with AVIF (Pillow 11.3 or newer), and a C compiler for speed (cc; without one the
textures unpack in Python, hours instead of minutes).

The mod's installer replaces every car's CARS/<car>/VINYLS.BIN with one whose 1,788 textures are 1024x1024
A8R8G8B8, and the paint target DUMMY_SKIN1-6 with 1024 ones, which the game's compositor (0x611F10, one pixel of
the target per pixel of each layer) then fills at 1024.  The port keeps the game's own files: the game paints its
512 skin as always, and the runtime paints the same layers again at 1024 on the GPU from this pack.

A pack holds, per texture of the car's VINYLS.BIN and per doorline of its TEXTURES.BIN (<car>_DOORLINE, _KIT,
_WIDEBODY and their _MASK: the mod's Installation/ModData/CARTEMPLATETEXTURES_), the mod's picture as AVIF:
  - cleaned: the upscaler left faint noise (values 1-4) over 13-21% of the empty black; every pixel more than two
    of the game's texels away from anything in the game's own 512 picture is set to 0 (it blended at most 3% before)
  - masks (<vinyl>_MASK: the compositor reads their G) and grey pictures (the game's has R = G = B) as one channel,
    the G; colour pictures (R, G, B pick the vinyl's three colours) as three, 4:4:4
  - each vinyl followed by its mask (894 pairs a car), each doorline by its mask
  - no alpha: the mod's are all 255 (it took the game's off before upscaling).  The game's alpha is the skin's gloss
    where a picture is drawn as it is (a doorline, a vinyl without REMAP): the runtime sends the game's own 512 one
  - quality 50: mean error 1.6 (grey) and 2.4 (colour) of 255, ~5 and ~8 KB a picture (350Z, measured)

File layout (little-endian; tools/packcheck.py checks it, runtime/hqvinyls.c and the page read it):
  0   'HQV1'
  4   u32 version (1)
  8   u32 count
  12  u32 data start (= 16 + 16 * count, rounded up to 16)
  16  count x { u32 name hash, u32 offset (from the file's start), u32 size, u16 side, u8 channels (1, 3), u8 0 }
      sorted by hash
  ..  the AVIF pictures, each where its row says
"""
import argparse, ctypes, io, os, struct, subprocess, sys, tempfile, zipfile
from multiprocessing import Pool

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from texpacks import chunks, tpks, textures, jdlz as jdlz_py, huff, bstring_hash, find  # noqa: E402

MAGIC, VERSION, ROW = b'HQV1', 1, 16
SIDE = 1024
ARGB, P8 = 21, 41
UNPACKED_HQ = SIDE * SIDE * 4 + 0x9C                # the mod's: level 0 only, then TextureInfo + platform info
UNPACKED_GAME = 350748                              # the game's: P8 512x512, 7 levels, palette, TextureInfo, platform
GAME_PAL = 0x55580
GROW = 2                                            # the game's texels around its picture that keep the mod's pixels
DOORLINES = ('_DOORLINE', '_DOORLINE_MASK', '_DOORLINE_KIT', '_DOORLINE_KIT_MASK', '_DOORLINE_WIDEBODY',
             '_DOORLINE_WIDEBODY_MASK')

# ---- JDLZ in C (the mod's 4 MB pictures: ~0.5 ms each against ~1.5 s in Python) ----------------------------------
JDLZ_C = r'''
#include <stdint.h>
#include <string.h>
long jdlz(const uint8_t *b, long csize_in, uint8_t *out, long cap){
    if (csize_in < 0x12 || memcmp(b, "JDLZ", 4)) return -1;
    uint32_t size, csize; memcpy(&size, b + 8, 4); memcpy(&csize, b + 12, 4);
    if ((long)csize != csize_in || (long)size > cap) return -1;
    long s = 0x12, o = 0, left = (long)csize - 0x12;
    unsigned f1 = b[0x10] | 0x100, f2 = b[0x11] | 0x100;
    while (left){
        if (f1 & 1){
            if (s + 2 > (long)csize) return -1;
            long ln, dist;
            if (f2 & 1){ ln = (((b[s] & 0xF0) << 4) | b[s+1]) + 3; dist = (b[s] & 0x0F) + 1; }
            else { dist = (((b[s] & 0xE0) << 3) | b[s+1]) + 17; ln = (b[s] & 0x1F) + 3; }
            if (o < dist || ln > (long)size - o || left < 2) return -1;
            for (long k = 0; k < ln; k++) out[o + k] = out[o + k - dist];
            o += ln; s += 2; left -= 2; f2 >>= 1;
        } else {
            if (o < (long)size){ if (s >= (long)csize) return -1; out[o++] = b[s++]; }
            left -= 1;
        }
        f1 >>= 1;
        if (f1 == 1){ if (!left || s >= (long)csize) return -1; f1 = b[s++] | 0x100; left--; }
        if (f2 == 1){ if (!left || s >= (long)csize) return -1; f2 = b[s++] | 0x100; left--; }
    }
    return o == (long)size ? o : -1;
}
'''
_lib = None


def jdlz_fast_init():
    global _lib
    d = tempfile.mkdtemp(prefix='hqvinyls-')
    src, lib = os.path.join(d, 'jdlz.c'), os.path.join(d, 'jdlz.so')
    with open(src, 'w') as f: f.write(JDLZ_C)
    try:
        subprocess.run(['cc', '-O2', '-shared', '-fPIC', '-o', lib, src], check=True, capture_output=True)
        _lib = ctypes.CDLL(lib)
        _lib.jdlz.restype = ctypes.c_long
        _lib.jdlz.argtypes = [ctypes.c_char_p, ctypes.c_long, ctypes.c_void_p, ctypes.c_long]
    except (OSError, subprocess.CalledProcessError):
        print('no C compiler: unpacking in Python (slow)', file=sys.stderr)
        _lib = False


def unpack(blob, cap):
    """a texture's block (JDLZ or HUFF), unpacked; None if it is neither or does not unpack within cap bytes"""
    try:
        if blob[:4] == b'JDLZ':
            size = struct.unpack_from('<I', blob, 8)[0]
            if size > cap: return None
            if _lib:
                out = ctypes.create_string_buffer(size)
                return out.raw if _lib.jdlz(blob, len(blob), out, size) == size else None
            return jdlz_py(blob)
        if blob[:4] == b'HUFF':
            return huff(blob, max_output_bytes=cap)
    except (ValueError, SystemExit, IndexError):
        return None
    return None


def packed_rows(d):
    """{name hash: its packed block} of a compressed texture pack (VINYLS.BIN)"""
    rows = {}
    for b, e in tpks(d, 0, len(d)):
        for cid, cb, ce in chunks(d, b, e):
            if cid != 0xB3310000: continue
            part = {c: (x, y) for c, x, y in chunks(d, cb, ce)}
            if 0x33310003 not in part: continue
            tb, te = part[0x33310003]
            for i in range((te - tb) // 24):
                nh, off, size = struct.unpack_from('<3I', d, tb + i * 24)
                if off + size <= len(d): rows[nh] = (off, size)
    return rows


# ---- one car ----------------------------------------------------------------------------------------------------
def game_picture(blob):
    """the game's 512 picture of a vinyl texture: (RGB array 512x512x3, grey?) or None"""
    import numpy as np
    u = unpack(blob, UNPACKED_GAME)
    if not u or len(u) != UNPACKED_GAME or struct.unpack_from('<I', u, len(u) - 0x20 + 0x14)[0] != P8: return None
    w, h = struct.unpack_from('<HH', u, len(u) - 0x9C + 0x44)
    if (w, h) != (512, 512): return None
    ix = np.frombuffer(u, np.uint8, 512 * 512).reshape(512, 512)
    pal = np.frombuffer(u, np.uint8, 1024, GAME_PAL).reshape(256, 4)
    rgb = pal[ix][..., [2, 1, 0]]
    return rgb, bool((rgb[..., 0] == rgb[..., 1]).all() and (rgb[..., 1] == rgb[..., 2]).all())


def mod_picture(blob):
    """the mod's 1024 picture: BGRA array or None"""
    import numpy as np
    u = unpack(blob, UNPACKED_HQ)
    if not u or len(u) != UNPACKED_HQ: return None
    w, h = struct.unpack_from('<HH', u, len(u) - 0x9C + 0x44)
    if (w, h) != (SIDE, SIDE) or struct.unpack_from('<I', u, len(u) - 0x20 + 0x14)[0] != ARGB: return None
    return np.frombuffer(u, np.uint8, SIDE * SIDE * 4).reshape(SIDE, SIDE, 4)


def keep_area(rgb512):
    """where the mod's pixels are kept: the game's picture's pixels, grown by GROW texels, at 1024"""
    import numpy as np
    from PIL import Image, ImageFilter
    g = Image.fromarray(rgb512.any(-1).astype(np.uint8) * 255).filter(ImageFilter.MaxFilter(2 * GROW + 1))
    return np.asarray(g.resize((SIDE, SIDE), Image.Resampling.NEAREST)) > 0


def avif(bgra, keep, grey, quality):
    """the cleaned picture as AVIF bytes, and its channel count"""
    import numpy as np
    from PIL import Image
    if grey:
        a = np.where(keep, bgra[..., 1], 0).astype(np.uint8)
        im, kw, ch = Image.fromarray(a), {}, 1
    else:
        a = np.where(keep[..., None], bgra[..., [2, 1, 0]], 0).astype(np.uint8)
        im, kw, ch = Image.fromarray(np.ascontiguousarray(a)), {'subsampling': '4:4:4'}, 3
    b = io.BytesIO()
    im.save(b, 'AVIF', quality=quality, speed=6, **kw)
    return b.getvalue(), ch


class Mod:
    """the mod's files, from its zip or its unpacked folder"""
    def __init__(self, src):
        self.src, self.zip, self.root = src, None, None
        if os.path.isdir(src):
            self.root = src
        else:
            self.zip = zipfile.ZipFile(src)
            self.names = {n.replace('\\', '/').split('v1.0/', 1)[-1].upper(): n for n in self.zip.namelist()}

    def read(self, rel):
        rel = rel.replace('\\', '/')
        if self.zip:
            n = self.names.get(rel.upper())
            return self.zip.read(n) if n else None
        for top in (self.root, os.path.join(self.root, 'High Quality Vinyls & Doorlines v1.0')):
            p = find(top, rel)
            if p:
                with open(p, 'rb') as f: return f.read()
        return None

    def cars(self):
        if self.zip:
            return sorted({k.split('/')[1] for k in self.names if k.startswith('CARS/') and k.endswith('/VINYLS.BIN')})
        top = self.root if find(self.root, 'CARS') else os.path.join(self.root, 'High Quality Vinyls & Doorlines v1.0')
        cars = find(top, 'CARS')
        return sorted(c.upper() for c in os.listdir(cars) if find(os.path.join(cars, c), 'VINYLS.BIN')) if cars else []


def dds_argb(b):
    """a 1024x1024 A8R8G8B8 DDS (the mod's doorlines): BGRA array or None"""
    import numpy as np
    if not b or len(b) < 128 + SIDE * SIDE * 4 or b[:4] != b'DDS ': return None
    h, w = struct.unpack_from('<II', b, 12)
    flags, four, bits = struct.unpack_from('<III', b, 80)
    if (w, h) != (SIDE, SIDE) or four or bits != 32: return None
    return np.frombuffer(b, np.uint8, SIDE * SIDE * 4, 128).reshape(SIDE, SIDE, 4)


def one_car(job):
    game, mod_src, car, quality, out_dir = job
    if _lib is None: jdlz_fast_init()
    mod = Mod(mod_src)
    gp = find(game, f'CARS/{car}/VINYLS.BIN')
    mb = mod.read(f'CARS/{car}/VINYLS.BIN')
    if not gp or not mb: return car, 0, 0, [f'{car}: no VINYLS.BIN in the game or the mod']
    with open(gp, 'rb') as f: gd = f.read()
    grows, mrows = packed_rows(gd), packed_rows(mb)
    ents, notes = [], []
    both = set(grows) & set(mrows)
    masks = {hash_append(nh, '_MASK') for nh in both} & both
    for nh in pair_order(both, masks):
        g = game_picture(gd[grows[nh][0]:grows[nh][0] + grows[nh][1]])
        m = mod_picture(mb[mrows[nh][0]:mrows[nh][0] + mrows[nh][1]])
        if g is None or m is None:
            notes.append(f'{car} {nh:08x}: {"game" if g is None else "mod"} picture unreadable, left out'); continue
        rgb, grey = g
        if not rgb.any(): continue                               # nothing to paint: the game's stays
        blob, ch = avif(m, keep_area(rgb), grey or nh in masks, quality)
        ents.append((nh, blob, ch))
    if len(set(grows) - set(mrows)): notes.append(f'{car}: {len(set(grows) - set(mrows))} of the game\'s not in the mod')
    # the doorlines (CARS/<car>/TEXTURES.BIN, 512x512 A8R8G8B8): the mod's from its endscript's folder
    tp = find(game, f'CARS/{car}/TEXTURES.BIN')
    own = {}
    if tp:
        with open(tp, 'rb') as f: td = f.read()
        want = {bstring_hash(car + x): x for x in DOORLINES}
        own = {nh: (w, h, lv0) for nh, w, h, fmt, lv0 in textures(td) if nh in want and fmt == ARGB and lv0}
    for nh, x in ((bstring_hash(car + x), x) for x in DOORLINES):
        if nh not in own: continue
        w, h, lv0 = own[nh]
        m = dds_argb(mod.read(f'Installation/ModData/CARTEMPLATETEXTURES_/{car}/{car}{x}.dds'))
        if m is None or (w, h) != (512, 512):
            notes.append(f'{car}{x}: no 1024 picture in the mod'); continue
        import numpy as np
        rgb = np.frombuffer(lv0, np.uint8).reshape(512, 512, 4)[..., [2, 1, 0]]
        if not rgb.any(): continue
        blob, ch = avif(m, keep_area(rgb), x.endswith('_MASK'), quality)
        ents.append((nh, blob, ch))
    ents = [e for k in pair_order({e[0] for e in ents}, {hash_append(e[0], '_MASK') for e in ents} & {e[0] for e in ents})
            for e in ents if e[0] == k]
    path = os.path.join(out_dir, 'HQVINYLS', 'CARS', f'{car}.hqv')
    size = write(path, ents)
    return car, len(ents), size, notes


def hash_append(h, s):
    """bStringHash(name + s) from bStringHash(name)"""
    for c in s.encode('latin1'): h = (h * 33 + c) & 0xFFFFFFFF
    return h


def pair_order(hashes, masks):
    """the pictures in the order they are stored: each vinyl followed by its _MASK (the runtime reads them together,
    and the page fetches the game folder in 1 MiB blocks: one block, not two)"""
    out = []
    for nh in sorted(hashes - masks):
        out.append(nh)
        m = hash_append(nh, '_MASK')
        if m in masks: out.append(m)
    seen = set(out)
    return out + sorted(h for h in hashes if h not in seen)


def write(path, ents):
    """ents in their stored order; the rows sorted by hash"""
    start = (16 + ROW * len(ents) + 15) & ~15
    head = bytearray(struct.pack('<4sIII', MAGIC, VERSION, len(ents), start))
    data, at, rows = bytearray(), start, []
    for nh, blob, ch in ents:
        rows.append((nh, at, len(blob), ch))
        data += blob; at += len(blob)
    for nh, off, size, ch in sorted(rows):
        head += struct.pack('<IIIHBB', nh, off, size, SIDE, ch, 0)
    head += bytes(start - len(head))
    whole = bytes(head) + bytes(data)
    import packcheck
    bad = packcheck.check(None, {f'HQVINYLS/CARS/{os.path.basename(path)}': whole})
    if bad: sys.exit(f'{path}: not written, tools/packcheck.py says: ' + '; '.join(why for _, why in bad[:20]))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + '.part', 'wb') as f: f.write(whole)
    os.replace(path + '.part', path)
    return len(whole)


def main():
    ap = argparse.ArgumentParser(usage=__doc__.split('\n\n')[1].strip())
    ap.add_argument('game')
    ap.add_argument('--mod', required=True, help="the mod's zip (High Quality Vinyls & Doorlines v1.0.zip) or folder")
    ap.add_argument('--quality', type=int, default=50, help='AVIF quality (default 50)')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument('--cars', help='only these cars (comma-separated)')
    ap.add_argument('--out', help='write HQVINYLS/ here (default: the game folder)')
    a = ap.parse_args()
    if not find(a.game, 'SPEED2.EXE'): sys.exit(f'{a.game}: no SPEED2.EXE, not the game folder')
    from PIL import features
    if not features.check('avif'): sys.exit('Pillow without AVIF: pip install "Pillow>=11.3"')
    cars = Mod(a.mod).cars()
    if a.cars: cars = [c for c in cars if c in {x.strip().upper() for x in a.cars.split(',')}]
    if not cars: sys.exit(f'{a.mod}: no CARS/<car>/VINYLS.BIN, not the mod')
    jobs = [(a.game, a.mod, c, a.quality, a.out or a.game) for c in cars]
    total = 0
    with Pool(min(a.jobs, len(jobs))) as pool:
        for car, n, size, notes in pool.imap_unordered(one_car, jobs):
            total += size
            for x in notes: print('  ' + x)
            print(f'{car}: {n} pictures, {size / 1e6:.1f} MB', flush=True)
    print(f'HQVINYLS: {len(cars)} cars, {total / 1e6:.0f} MB')


if __name__ == '__main__':
    main()
