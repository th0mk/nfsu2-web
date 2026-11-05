#!/usr/bin/env python3
"""The vinyl packs: VINYLPACKS/CARS/<car>/VINYLS.BIN in the game folder, the
vinyls the Japanese release (Need for Speed Underground 2 Sha_Do, PlayStation 2,
2005) added and the other releases never had: Art Factory 28 to 34, Sha_Do's
designs for Art Factory Graphics.  The runtime (runtime/fileio.c) reads a car's
VINYLS.BIN as the game's own file with these textures added, and
runtime/vinylpacks.c adds their seven parts to the car part database.

    tools/vinylpacks.py <game folder> --shado <underground2.net folder> [--out <folder>]

<underground2.net folder> is the underground2.net mod (nfsmods.xyz/mod/2004, by
the underground2.net team) extracted: the folder with "1 - Mod Files" in it, or
that folder itself.  Its cars' VINYLS.BIN have
the Sha_Do vinyls ported to the PC; only those 14 textures a car (7 vinyls, each
with its mask) are taken, nothing else of the mod.  The packs are written under
<out> (default: the game folder): VINYLPACKS/...

The mod keeps them as 512x512 A8R8G8B8 pictures (1 MB each); the game's own
vinyls are 512x512 P8 (a 256-colour palette) with 7 mipmap levels (342 KB).
Here they become what the game's own are: P8 with the palette of the picture's
own colours (a picture of more than 256 is cut down to 256 by median cut), alpha
255 as the game's Art Factory vinyls have, the mipmaps every other pixel, the
TextureInfo the game's own Art Factory vinyls and masks have, and packed with
JDLZ.  The JDLZ written here is checked against a copy of the game's own unpack
loop (runtime/overrides.c jdlz_native): its byte count must run out exactly
when the last token and the flag bytes the loop reads after it are used up.

A pack file (little-endian):
  header  'VPAK', version 1, base size, cut, head size, tail size, 0, 0
  head    head size bytes
  tail    tail size bytes
The game reads it as: head, then the game file from byte `cut` to its end (base
size), then tail.  The head is the game file's TPK header with the new
textures' entries in its two tables (sorted by hash, as the game's) and every
size and offset moved to fit; the tail holds the new textures' blobs.  The
runtime uses a pack only when the game file is base size bytes long.
"""
import argparse, os, struct, sys
from collections import Counter
from multiprocessing import Pool

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from texpacks import bstring_hash, chunks, find, jdlz  # noqa: E402

VINYLS = [f'ART_F_{k}' for k in range(28, 35)]       # the Sha_Do vinyls, as the part database names them
MAGIC, VERSION = b'VPAK', 1
P8_PIX = 262144 + 65536 + 16384 + 4096 + 1024 + 256 + 64   # 512x512, 7 levels
P8_PAL = 0x55580                                      # the palette's place in a P8 vinyl's blob (the game's)
P8_SIZE = P8_PAL + 1024 + 0x9C                        # = 350748, every vinyl of the game's
TEMPLATES = ('350Z', '350Z_ART_F_12', '350Z_AEM_HKS_MASK')   # a car whose TPK has a main and a mask in JDLZ
IMG_BASE = 0x20000000                                 # TextureInfo image offsets past the game's own (the game uses
                                                      # only palette - image; each texture gets its own)


def read_tpk(path):
    """(file bytes, its chunk places, entries: hash -> (hash, offset, size, unpacked, flags, 0))"""
    return parse_tpk(open(path, 'rb').read())


def parse_tpk(d):
    top = list(chunks(d, 0, len(d)))
    if len(top) != 1 or top[0][0] != 0xB3300000: return d, None, None
    lay = {'tpk': top[0], 'pads': []}                   # the TPK's own children by id; their children's places
    for cid, b, e in chunks(d, top[0][1], top[0][2]):
        if cid == 0: lay['pads'].append((b, e))
        else: lay[cid] = [(b, e)]
        if cid in (0xB3310000, 0xB3320000):
            for c2, b2, e2 in chunks(d, b, e):
                if c2: lay[c2] = (b2, e2)
    if 0x33310003 not in lay or 0x33310002 not in lay or 0x33320002 not in lay: return d, None, None
    tb, te = lay[0x33310003]
    ents = {}
    for i in range((te - tb) // 24):
        t = struct.unpack_from('<6I', d, tb + 24 * i)
        ents[t[0]] = t
    return d, lay, ents


def unpack(d, e):
    blob = d[e[1]:e[1] + e[2]]
    if blob[:4] != b'JDLZ': return None
    return jdlz(blob)


# --- JDLZ, as the game unpacks it -------------------------------------------
def jdlz_game(b):
    """the game's unpack loop (0x573700, runtime/overrides.c jdlz_native): the bytes, or None where it would fail"""
    size, csize = struct.unpack_from('<II', b, 8)
    if csize != len(b) or csize < 0x12: return None
    out = bytearray(size)
    s, o, left = 0x12, 0, csize - 0x12
    f1, f2 = b[0x10] | 0x100, b[0x11] | 0x100
    while left:
        if f1 & 1:
            if s + 2 > csize: return None
            if f2 & 1: ln, dist = ((b[s] & 0xF0) << 4 | b[s + 1]) + 3, (b[s] & 0x0F) + 1
            else: dist, ln = ((b[s] & 0xE0) << 3 | b[s + 1]) + 17, (b[s] & 0x1F) + 3
            if o < dist or ln > size - o or left < 2: return None
            for k in range(ln): out[o + k] = out[o + k - dist]
            o += ln; s += 2; left -= 2; f2 >>= 1
        else:
            if o < size:
                if s >= csize: return None
                out[o] = b[s]; o += 1; s += 1
            left -= 1
        f1 >>= 1
        if f1 == 1:
            if not left or s >= csize: return None
            f1 = b[s] | 0x100; s += 1; left -= 1
        if f2 == 1:
            if not left or s >= csize: return None
            f2 = b[s] | 0x100; s += 1; left -= 1
    return bytes(out) if o == size else None


def match_len(d, i, j, cap):
    """how many bytes from i equal those from j (j < i; may overlap, as the copy does), at most cap"""
    n = 0
    step = 16
    while n < cap:
        k = min(step, cap - n)
        if d[i + n:i + n + k] == d[j + n:j + n + k]:
            n += k; step = min(step * 2, 4096)
        else:
            if k == 1: break
            step = max(1, k // 2)
    return n


def jdlz_pack(d):
    """d packed in JDLZ: greedy matches (distance 1-16 up to 4098 bytes, 17-2064 up to 34)"""
    n = len(d)
    out = bytearray(b'JDLZ\x02\x10\x00\x00' + struct.pack('<II', n, 0))
    fl = [len(out), 0, 0, len(out) + 1, 0, 0]            # f1: place, bits, count; f2: place, bits, count
    out += b'\0\0'
    heads = {}
    i = 0
    while i < n:
        best, bd = 0, 0
        for dist in range(1, min(16, i) + 1):
            if d[i] != d[i - dist]: continue
            L = match_len(d, i, i - dist, min(4098, n - i))
            if L > best: best, bd = L, dist
            if best >= 64: break
        if best < 34 and i + 3 <= n:
            for j in reversed(heads.get(d[i:i + 3], ())):
                if i - j > 2064: break
                if i - j <= 16: continue
                L = match_len(d, i, j, min(34, n - i))
                if L > best: best, bd = L, i - j
                if best == 34: break
        if best >= 3:
            short = bd <= 16
            if short: out += bytes(((((best - 3) >> 4) & 0xF0) | (bd - 1), (best - 3) & 0xFF))
            else: out += bytes((((((bd - 17) >> 8) & 7) << 5) | (best - 3), (bd - 17) & 0xFF))
            fl[1] |= 1 << fl[2]
            if short: fl[4] |= 1 << fl[5]
            fl[5] += 1
            step = best
        else:
            out.append(d[i]); step = 1
        fl[2] += 1
        for k in range(i, min(i + step, n - 2)):
            heads.setdefault(d[k:k + 3], []).append(k)
            if len(heads[d[k:k + 3]]) > 64: del heads[d[k:k + 3]][:32]
        i += step
        # the loop reads the next flag byte right after the token that used up the last bit: f1 first, then f2
        if fl[2] == 8:
            out[fl[0]] = fl[1]; fl[0:3] = [len(out), 0, 0]; out.append(0)
        if fl[5] == 8:
            out[fl[3]] = fl[4]; fl[3:6] = [len(out), 0, 0]; out.append(0)
    out[fl[0]] = fl[1]; out[fl[3]] = fl[4]
    struct.pack_into('<I', out, 12, len(out))
    return bytes(out)


# --- the pictures -------------------------------------------------------------
def median_cut(cols, k):
    """at most k colours standing for cols (Counter of 3-byte colours, weighted), and each colour's index"""
    boxes = [list(cols.items())]
    while len(boxes) < k:
        best = None
        for bi, bx in enumerate(boxes):
            if len(bx) < 2: continue
            for ch in range(3):
                lo, hi = min(c[0][ch] for c in bx), max(c[0][ch] for c in bx)
                if best is None or hi - lo > best[0]: best = (hi - lo, bi, ch)
        if best is None or best[0] == 0: break
        _, bi, ch = best
        bx = sorted(boxes[bi], key=lambda c: c[0][ch])
        half, acc, cut = sum(w for _, w in bx) / 2, 0, 1
        for m, (_, w) in enumerate(bx):
            acc += w
            if acc >= half: cut = max(1, min(len(bx) - 1, m + 1)); break
        boxes[bi:bi + 1] = [bx[:cut], bx[cut:]]
    pal, index = [], {}
    for bx in boxes:
        tw = sum(w for _, w in bx)
        pal.append(bytes(round(sum(c[ch] * w for c, w in bx) / tw) for ch in range(3)))
        for c, _ in bx: index[c] = len(pal) - 1
    return pal, index


def to_p8(argb):
    """512x512 BGRA bytes -> (indices of the 7 levels, 1024-byte BGRA palette), alpha 255; how many colours it had"""
    px = [argb[i:i + 3] for i in range(0, 512 * 512 * 4, 4)]
    cols = Counter(px)
    if len(cols) <= 256:
        pal = list(cols); index = {c: k for k, c in enumerate(pal)}
    else:
        pal, index = median_cut(cols, 256)
    lv0 = bytes(index[c] for c in px)
    levels, w = [lv0], 512
    while len(levels) < 7:
        prev = levels[-1]
        rows = [prev[y * w:(y + 1) * w:2] for y in range(0, w, 2)]
        levels.append(b''.join(rows)); w //= 2
    palb = b''.join(c + b'\xff' for c in pal) + bytes(4 * (256 - len(pal)))
    return b''.join(levels), palb, len(cols)


def vinyl_blob(argb, ti, pf, name, h, img_off):
    """a P8 vinyl as the game keeps one unpacked: pixels (7 levels), palette, TextureInfo, platform info"""
    pix, pal, ncol = to_p8(argb)
    ti = bytearray(ti)
    nm = name.encode('latin1')[:23]
    ti[0x0C:0x24] = nm + bytes(24 - len(nm))
    struct.pack_into('<I', ti, 0x24, h)
    struct.pack_into('<II', ti, 0x30, img_off, img_off + P8_PAL)
    raw = pix + bytes(P8_PAL - len(pix)) + pal + bytes(ti) + pf
    assert len(raw) == P8_SIZE
    return raw, ncol


def templates(game):
    car, main, mask = TEMPLATES
    d, lay, ents = read_tpk(find(game, f'CARS/{car}/VINYLS.BIN'))
    out = []
    for nm in (main, mask):
        e = ents.get(bstring_hash(nm))
        u = unpack(d, e) if e else None
        if not u or len(u) != P8_SIZE or struct.unpack_from('<I', u, len(u) - 0x20 + 0x14)[0] != 41:
            sys.exit(f'{car}/VINYLS.BIN: no P8 {nm} in JDLZ to copy the TextureInfo of')
        out.append((u[-0x9C:-0x20], u[-0x20:]))
    return out


def splice(base, lay, ents, new):
    """(cut, head, tail): the game file `base` read as head + base[cut:] + tail has the entries `new` too
    (hash -> (blob, unpacked size)) """
    S = len(base)
    tpk_b = lay['tpk'][1]
    info_b, info_e = lay[0xB3310000][0]
    data_b, data_e = lay[0xB3320000][0]
    pads = [p for p in lay['pads'] if info_e <= p[0] - 8 and p[1] <= data_b - 8]
    t2b, t2e = lay[0x33310002]; t3b, t3e = lay[0x33310003]; pb, pe = lay[0x33320002]
    if len(pads) != 1 or pads[0][1] != data_b - 8 or t3e != info_e or pe != S or lay['tpk'][2] != S:
        sys.exit('a VINYLS.BIN laid out unlike the game\'s')
    pad_b, pad_e = pads[0]
    pairs = [struct.unpack_from('<II', base, t2b + 8 * k) for k in range((t2e - t2b) // 8)]
    if [p[0] for p in pairs] != sorted(ents) or any(p[1] for p in pairs):
        sys.exit('a VINYLS.BIN whose hash list is not its entries sorted')
    grow = 32 * len(new)
    delta = (grow + 0x7F) & ~0x7F
    cut = pb                                            # the textures' data: from here on the game's bytes, moved by delta
    tail, at = bytearray(), S + delta
    rows = {}
    for h in sorted(new):
        blob, usize = new[h]
        padn = (-(at + len(tail))) % 0x80
        tail += bytes(padn)
        rows[h] = (h, at + len(tail), len(blob), usize, 0x100, 0)
        tail += blob
    tail += bytes((-(at + len(tail))) % 0x80)
    for h, e in ents.items(): rows[h] = (h, e[1] + delta, e[2], e[3], e[4], e[5])
    keys = sorted(rows)
    t2 = b''.join(struct.pack('<II', h, 0) for h in keys)
    t3 = b''.join(struct.pack('<6I', *rows[h]) for h in keys)
    T = len(tail)
    head = bytearray()
    head += struct.pack('<II', 0xB3300000, S + delta + T - 8)
    head += base[8:info_b - 8]
    head += struct.pack('<II', 0xB3310000, info_e - info_b + grow)
    head += base[info_b:t2b - 8]
    head += struct.pack('<II', 0x33310002, len(t2)) + t2
    head += base[t2e:t3b - 8]
    head += struct.pack('<II', 0x33310003, len(t3)) + t3
    head += struct.pack('<II', 0, pad_e - pad_b + delta - grow) + base[pad_b:pad_e] + bytes(delta - grow)
    head += struct.pack('<II', 0xB3320000, data_e - data_b + T)
    head += base[data_b:pb - 8]
    head += struct.pack('<II', 0x33320002, pe - pb + T)
    assert len(head) == cut + delta, (hex(len(head)), hex(cut), hex(delta))
    return cut, bytes(head), bytes(tail)


def one_car(job):
    game, mod, out, car, tpl = job
    gp, mp = find(game, f'CARS/{car}/VINYLS.BIN'), find(mod, f'CARS/{car}/VINYLS.BIN')
    base, lay, ents = read_tpk(gp)
    if ents is None or bstring_hash(f'{car}_ART_F_27') not in ents: return car, None, 'no Art Factory vinyls'
    if not mp: return car, None, 'not in the mod'
    md, mlay, ments = read_tpk(mp)
    new, notes = {}, []
    for k, v in enumerate(VINYLS):
        for m, sfx in enumerate(('', '_MASK')):
            name = f'{car}_{v}{sfx}'
            h = bstring_hash(name)
            if h in ents: return car, None, f'the game already has {name}'
            e = ments.get(h) if ments else None
            u = unpack(md, e) if e else None
            if u is None: return car, None, f'the mod has no {name} in JDLZ'
            ti, pf = u[-0x9C:-0x20], u[-0x20:]
            w, hh = struct.unpack_from('<HH', ti, 0x44)
            if (w, hh) != (512, 512) or struct.unpack_from('<I', pf, 0x14)[0] != 21:
                return car, None, f'{name}: not a 512x512 A8R8G8B8'
            raw, ncol = vinyl_blob(u[:512 * 512 * 4], *tpl[m], name, h, IMG_BASE + (2 * k + m) * 0x56000)
            blob = jdlz_pack(raw)
            if jdlz_game(blob) != raw: return car, None, f'{name}: JDLZ does not read back as the game reads it'
            new[h] = (blob, len(raw))
            if ncol > 256: notes.append(f'{v}{sfx} {ncol} colours -> 256')
    cut, head, tail = splice(base, lay, ents, new)
    # read back as the game will: the stock textures where they were, moved; the new ones there
    virt = head + base[cut:] + tail
    _, vlay, vents = parse_tpk(virt)
    if vents is None or len(vents) != len(ents) + len(new): return car, None, 'the result does not read back'
    for h, e in ents.items():
        v = vents[h]
        if virt[v[1]:v[1] + v[2]] != base[e[1]:e[1] + e[2]] or v[3] != e[3]: return car, None, 'a texture of the game\'s moved wrong'
    for h, (blob, usize) in new.items():
        v = vents[h]
        if virt[v[1]:v[1] + v[2]] != blob or v[3] != usize: return car, None, 'a new texture is not where its entry says'
    dst = os.path.join(out, 'VINYLPACKS', 'CARS', os.path.basename(os.path.dirname(gp)), os.path.basename(gp))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + '.new'
    with open(tmp, 'wb') as f:
        f.write(struct.pack('<4s7I', MAGIC, VERSION, len(base), cut, len(head), len(tail), 0, 0))
        f.write(head); f.write(tail)
    os.replace(tmp, dst)
    return car, (len(base), len(head), len(tail), os.path.getsize(dst)), '; '.join(notes)


def main():
    ap = argparse.ArgumentParser(usage='tools/vinylpacks.py <game folder> --shado <underground2.net folder> [--out <folder>]')
    ap.add_argument('game')
    ap.add_argument('--shado', metavar='MOD', help='the underground2.net mod (nfsmods.xyz/mod/2004), extracted')
    ap.add_argument('--out', metavar='DIR', help='where VINYLPACKS/ goes (default: the game folder)')
    ap.add_argument('--cars', help='only these cars (comma-separated folder names), for a test')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    if not a.shado: sys.exit(__doc__)
    if not find(a.game, 'SPEED2.EXE'): sys.exit(f'{a.game}: no SPEED2.EXE, not the game folder')
    mod = a.shado
    if not find(mod, 'CARS'):
        sub = next((os.path.join(mod, n) for n in sorted(os.listdir(mod)) if find(os.path.join(mod, n), '1 - Mod Files/CARS')), None)
        mod = os.path.join(sub, '1 - Mod Files') if sub else find(mod, '1 - Mod Files')
    if not mod or not find(mod, 'CARS'): sys.exit(f'{a.shado}: no "1 - Mod Files/CARS" in it')
    tpl = templates(a.game)
    cars = sorted(os.listdir(find(a.game, 'CARS')))
    if a.cars: cars = [c for c in cars if c.upper() in a.cars.upper().split(',')]
    cars = [c for c in cars if find(a.game, f'CARS/{c}/VINYLS.BIN')]
    out = a.out or a.game
    made = 0
    with Pool(a.jobs) as p:
        for car, r, note in p.imap_unordered(one_car, [(a.game, mod, out, c, tpl) for c in cars]):
            if r is None:
                if note not in ('no Art Factory vinyls',): print(f'  {car}: skipped, {note}')
                continue
            made += 1
            print(f'  {car}: {r[2] / 1e6:.2f} MB of new textures (pack {r[3] / 1e6:.2f} MB){"; " + note if note else ""}')
    print(f'{made} cars: {os.path.join(out, "VINYLPACKS")}')


if __name__ == '__main__':
    main()
