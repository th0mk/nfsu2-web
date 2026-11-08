#!/usr/bin/env python3
"""tools/packcheck.py's own test: a small game folder and packs made the way the pack tools make them, each one then
broken one way (cut short, an offset past the end, two textures in one place, one among the game's, JDLZ the native
unpacker refuses, a memory file that still lists a spliced file ...), and the check must name it; the good ones pass.
web/test/packs.test.mjs runs it:

    python3 -I tools/test_packcheck.py
"""
import gc, os, re, shutil, struct, sys, tempfile, unittest, warnings, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import packcheck as P  # noqa: E402
from vinylpacks import splice, parse_tpk, jdlz_pack  # noqa: E402
from modpacks import splice_plain, unlisted  # noqa: E402

ARGB = 21


def chunk(cid, body): return struct.pack('<II', cid, len(body)) + body


def ti(name, img, pal, isz, psz, w=8, h=8, levels=1):
    t = bytearray(0x7C)
    t[0x0C:0x0C + len(name)] = name.encode()
    struct.pack_into('<I', t, 0x24, P.bstring_hash(name))
    struct.pack_into('<4I', t, 0x30, img, pal, isz, psz)
    struct.pack_into('<HH', t, 0x44, w, h)
    t[0x4E] = levels
    return bytes(t)


def pf(fmt=ARGB):
    p = bytearray(0x20)
    struct.pack_into('<I', p, 0x14, fmt)
    return bytes(p)


def pixels(name, n=256): return bytes((i * 7 + len(name) * 13) & 0xFF for i in range(n))


def block(name, img, n=256):
    """an 8x8 A8R8G8B8 texture as a compressed pack keeps it unpacked: pixels, TextureInfo (its palette offset just
    past the image, no palette, as the game's), platform info"""
    return pixels(name, n) + ti(name, img, img + n, n, 0) + pf()


def huff(n):
    """a block of the game's own HUFF kind (this test's stand-in: the check never unpacks one)"""
    return b'HUFF\x01\x10\x00\x00' + struct.pack('<II', n, 36) + bytes(20)


def packed_tpk(rows):
    """a compressed TPK laid out as the game's VINYLS.BIN (as tools/vinylpacks.py splice() takes it): rows
    [(hash, blob, unpacked size)]"""
    rows = sorted(rows)
    def build(offs):
        info = chunk(0xB3310000, chunk(0x33310001, bytes(0x7C)) +
                     chunk(0x33310002, b''.join(struct.pack('<II', h, 0) for h, _, _ in rows)) +
                     chunk(0x33310003, b''.join(struct.pack('<6I', h, o, len(b), u, 0x100, 0) for (h, b, u), o in zip(rows, offs))))
        pre = info + chunk(0, bytes(0x40))
        dhead = chunk(0x33320001, bytes(0x18))
        start = 8 + len(pre) + 8 + len(dhead) + 8              # where the blobs' chunk's data begins
        data, offs2 = bytearray(), []
        for h, b, u in rows:
            data += bytes(-(start + len(data)) % 0x80)
            offs2.append(start + len(data)); data += b
        return chunk(0xB3300000, pre + chunk(0xB3320000, dhead + chunk(0x33320002, bytes(data)))), offs2
    d, offs = build([0] * len(rows))
    d, offs2 = build(offs)
    assert offs == offs2
    return d


def plain_tpk(texs):
    """an uncompressed TPK laid out as the game's GLOBALTEXTURESNA.BIN (as tools/modpacks.py splice_plain() takes it):
    texs [(name, pixels)], each at its own place"""
    texs = sorted(texs, key=lambda t: P.bstring_hash(t[0]))
    pix, tis = bytearray(), []
    for name, px in texs:
        pix += bytes(-len(pix) % 0x80)
        tis.append(ti(name, len(pix), 0, len(px), 0)); pix += px
    info = chunk(0xB3310000, chunk(0x33310001, bytes(0x7C)) +
                 chunk(0x33310002, b''.join(struct.pack('<II', P.bstring_hash(n), 0) for n, _ in texs)) +
                 chunk(0x33310004, b''.join(tis)) + chunk(0x33310005, pf() * len(texs)))
    pre = chunk(0, bytes(0x30)) + info + chunk(0, bytes(0x40))
    dhead = chunk(0x33320001, bytes(0x18))
    lead = -(8 + len(pre) + 8 + len(dhead) + 8) % 0x80             # the pixels at a 0x80 boundary of the file
    return chunk(0xB3300000, pre + chunk(0xB3320000, dhead + chunk(0x33320002, bytes(lead) + bytes(pix))))


def memfile(entries):
    """a memory file: [(game path, the file's bytes, header size)]"""
    head = struct.pack('<4I', 0, 0, P.MEMFILE_MAGIC, len(entries))
    at, blobs, rows = 16 + 20 * len(entries), b'', b''
    for rel, d, hs in entries:
        rows += struct.pack('<5I', P.bstring_hash(rel.upper().replace('/', '\\')), at + len(blobs), len(d), hs, 0x80)
        blobs += d[:hs]
    return head + rows + blobs


def vpak(base, cut, head, tail, flag=0):
    return struct.pack('<4s7I', b'VPAK', 1, len(base), cut, len(head), len(tail), flag, 0) + head + tail


# the game's own: six textures, each at its place in the table's order (0x100 each), two of them HUFF
GAME_NAMES = [f'TEST_G{k}' for k in range(6)]


def game_vinyls():
    hs = sorted(P.bstring_hash(n) for n in GAME_NAMES)
    name = {P.bstring_hash(n): n for n in GAME_NAMES}
    rows = []
    for k, h in enumerate(hs):
        raw = block(name[h], k * 0x100)
        rows.append((h, huff(len(raw)) if k in (2, 3) else jdlz_pack(raw), len(raw)))
    return packed_tpk(rows), {name[h]: k * 0x100 for k, h in enumerate(hs)}


def vinyl_pack(base, new):
    """the splice of base with the textures new [(name, raw block, its blob or None: JDLZ of it)]"""
    _, lay, ents = parse_tpk(base)
    cut, head, tail = splice(base, lay, ents, {P.bstring_hash(n): (b if b is not None else jdlz_pack(r), len(r)) for n, r, b in new})
    return cut, head, tail


def literal_jdlz(raw, last_flag=True):
    """raw in JDLZ as literals alone; last_flag False: as an encoder that writes a flag byte only before tokens that
    use it, so a stream of a multiple of 8 tokens lacks the one the game's loop reads after the last (past the count:
    the game's loop then runs on, past its output)"""
    out = bytearray(b'JDLZ\x02\x10\x00\x00' + struct.pack('<II', len(raw), 0) + b'\0\0')
    for k, c in enumerate(raw):
        out.append(c)
        if k % 8 == 7 and (last_flag or k + 1 < len(raw)): out.append(0)
    struct.pack_into('<I', out, 12, len(out))
    return bytes(out)


class PackCheck(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='packcheck-')
        cls.game = os.path.join(cls.tmp, 'game')
        cls.base, cls.places = game_vinyls()
        cls.plain = plain_tpk([(f'GLOBAL_T{k}', pixels(f'GLOBAL_T{k}')) for k in range(3)])
        files = {'SPEED2.EXE': b'', 'CARS/TEST/VINYLS.BIN': cls.base, 'LANGUAGES/LanguageTextures.bin': cls.base,
                 'GLOBAL/GLOBALTEXTURESNA.BIN': cls.plain, 'GLOBAL/LISTED.BIN': cls.plain}
        files['GLOBAL/GlobalMemoryFile.bin'] = memfile([('GLOBAL/LISTED.BIN', cls.plain, 64)])
        for rel, d in files.items():
            p = os.path.join(cls.game, *rel.split('/'))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'wb') as f: f.write(d)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def check(self, files):
        return [f'{k}: {why}' for k, why in P.check(self.game, files)]

    def refused(self, files, pattern):
        probs = self.check(files)
        self.assertTrue(any(re.search(pattern, p) for p in probs), f'{pattern!r} not in {probs}')
        return probs

    # --- VINYLPACKS: a compressed pack, spliced ----------------------------------------------------------------------
    def good_new(self):
        return [('TEST_N0', block('TEST_N0', 0x30000000), None), ('TEST_N1', block('TEST_N1', 0x30000100), None)]

    def test_good_vinyl_pack(self):
        cut, head, tail = vinyl_pack(self.base, self.good_new())
        self.assertEqual(self.check({'VINYLPACKS/CARS/TEST/VINYLS.BIN': vpak(self.base, cut, head, tail)}), [])

    def test_cut_short(self):
        cut, head, tail = vinyl_pack(self.base, self.good_new())
        self.refused({'VINYLPACKS/CARS/TEST/VINYLS.BIN': vpak(self.base, cut, head, tail)[:-100]}, r'cut short or grown')

    def test_another_game_file(self):
        cut, head, tail = vinyl_pack(self.base, self.good_new())
        p = bytearray(vpak(self.base, cut, head, tail)); struct.pack_into('<I', p, 8, len(self.base) + 4)
        self.refused({'VINYLPACKS/CARS/TEST/VINYLS.BIN': bytes(p)}, r'made for a game file of')

    def test_block_past_the_end(self):
        cut, head, tail = vinyl_pack(self.base, self.good_new())
        v = head + self.base[cut:] + tail
        _, lay, ents = parse_tpk(v)
        tb, _ = lay[0x33310003]
        h = bytearray(head)
        k = sorted(ents).index(P.bstring_hash('TEST_N1'))
        struct.pack_into('<I', h, tb + 24 * k + 4, len(v) + 0x1000)     # its block's offset past the file's end
        self.refused({'VINYLPACKS/CARS/TEST/VINYLS.BIN': vpak(self.base, cut, bytes(h), tail)}, r'outside the pack\'s data')

    def test_image_past_its_block(self):
        raw = bytearray(block('TEST_N0', 0x30000000))
        struct.pack_into('<I', raw, 256 + 0x38, 0x1000)                   # an image bigger than its block
        cut, head, tail = vinyl_pack(self.base, [('TEST_N0', bytes(raw), None)])
        self.refused({'VINYLPACKS/CARS/TEST/VINYLS.BIN': vpak(self.base, cut, head, tail)}, r'an image of 4096 bytes in a block of 256')

    def test_two_in_one_place(self):
        new = [('TEST_N0', block('TEST_N0', 0x30000000), None), ('TEST_N1', block('TEST_N1', 0x30000080), None)]
        cut, head, tail = vinyl_pack(self.base, new)
        self.refused({'VINYLPACKS/CARS/TEST/VINYLS.BIN': vpak(self.base, cut, head, tail)}, r"TEST_N0: its place .* holds TEST_N1's offset")

    def test_in_a_game_textures_place(self):
        jd = [n for n, at in self.places.items() if at in (0, 0x100, 0x400, 0x500)][0]
        new = [('TEST_N0', block('TEST_N0', self.places[jd] + 0x40), None)]
        cut, head, tail = vinyl_pack(self.base, new)
        self.refused({'VINYLPACKS/CARS/TEST/VINYLS.BIN': vpak(self.base, cut, head, tail)}, rf"lies in the game's {jd}'s place")

    def test_among_the_game_huff_ones(self):
        new = [('TEST_N0', block('TEST_N0', 0x280), None)]               # where the two HUFF ones are (0x200 .. 0x400)
        cut, head, tail = vinyl_pack(self.base, new)
        self.refused({'VINYLPACKS/CARS/TEST/VINYLS.BIN': vpak(self.base, cut, head, tail)}, r"lies among the game's own textures' \(0x200 \.\. 0x400")

    def test_first_vinyl_places(self):
        """tools/vinylpacks.py before 8473add3: the Sha_Do vinyls from 0x20000000, among a VINYLS.BIN's own"""
        new = [('TEST_ART_F_28', block('TEST_ART_F_28', 0x20000000), None)]
        cut, head, tail = vinyl_pack(self.base, new)
        self.refused({'VINYLPACKS/CARS/TEST/VINYLS.BIN': vpak(self.base, cut, head, tail)}, r"at the first vinyl packs' places")

    def test_jdlz_refused(self):
        raw = block('TEST_N0', 0x30000000, 260)                          # 416 bytes: 52 groups of 8 literals
        self.assertEqual(P.jdlz_native(literal_jdlz(raw))[0], raw, 'literals with the last flag byte: as the game reads them')
        # the encoder that leaves the last flag byte out: the game's loop reads past its count and runs on
        cut, head, tail = vinyl_pack(self.base, [('TEST_N0', raw, literal_jdlz(raw, last_flag=False))])
        self.refused({'VINYLPACKS/CARS/TEST/VINYLS.BIN': vpak(self.base, cut, head, tail)}, r"a flag byte past the block's end")
        # a match before the output's start
        bad = b'JDLZ\x02\x10\x00\x00' + struct.pack('<II', len(raw), 0x14) + b'\x01\x01' + b'\x10\x00'
        cut, head, tail = vinyl_pack(self.base, [('TEST_N0', raw, bad)])
        self.refused({'VINYLPACKS/CARS/TEST/VINYLS.BIN': vpak(self.base, cut, head, tail)}, r'before its start')
        # unpacked size other than its entry's: smaller, and bigger (the streamer allocates the entry's: written past)
        cut, head, tail = vinyl_pack(self.base, [('TEST_N0', raw + b'\0' * 4, jdlz_pack(raw))])
        self.refused({'VINYLPACKS/CARS/TEST/VINYLS.BIN': vpak(self.base, cut, head, tail)}, r'says it unpacks to 416 bytes, its entry 420')
        big = bytearray(jdlz_pack(raw)); struct.pack_into('<I', big, 8, len(raw) + 65536)
        cut, head, tail = vinyl_pack(self.base, [('TEST_N0', raw, bytes(big))])
        self.refused({'VINYLPACKS/CARS/TEST/VINYLS.BIN': vpak(self.base, cut, head, tail)}, rf'says it unpacks to {len(raw) + 65536} bytes, its entry {len(raw)}')

    # --- LANGPACKS: a file in place of the game's -------------------------------------------------------------------
    def test_language_pack(self):
        """a texture redrawn bigger at its old place (as the Greek fonts would have been): it reaches into the next"""
        _, lay, ents = parse_tpk(self.base)
        name = min(self.places, key=lambda n: self.places[n])          # at 0: a JDLZ one, the next one JDLZ too
        raw = block(name, 0, 0x200)
        rows = []
        for h, e in ents.items():
            b = self.base[e[1]:e[1] + e[2]]
            if h == P.bstring_hash(name): b = b'RAWW\x01\x10\x00\x00' + struct.pack('<II', len(raw), len(raw) + 16) + raw
            rows.append((h, b, len(raw) if h == P.bstring_hash(name) else e[3]))
        self.refused({'LANGPACKS/test/LANGUAGES/LanguageTextures.bin': packed_tpk(rows)}, r"holds the game's TEST_G\d's offset")
        # past the others' end, as tools/greekfont stores it
        raw = block(name, 0x2000, 0x200)
        rows = [(h, b'RAWW\x01\x10\x00\x00' + struct.pack('<II', len(raw), len(raw) + 16) + raw if h == P.bstring_hash(name) else b,
                 len(raw) if h == P.bstring_hash(name) else u) for h, b, u in rows]
        self.assertEqual(self.check({'LANGPACKS/test/LANGUAGES/LanguageTextures.bin': packed_tpk(rows)}), [])

    # --- MODPACKS: uncompressed packs, the memory file, the crash damage's and the whine's -------------------------------
    def plain_pack(self, rel='GLOBAL/GLOBALTEXTURESNA.BIN'):
        cut, head, tail = splice_plain(self.plain, {P.bstring_hash('MOD_ICON'): (ti('MOD_ICON', 0, 0, 256, 0), pf(), pixels('MOD_ICON'))})
        return cut, head, tail

    def test_plain_pack(self):
        cut, head, tail = self.plain_pack()
        self.assertEqual(self.check({'MODPACKS/GLOBAL/GLOBALTEXTURESNA.BIN': vpak(self.plain, cut, head, tail)}), [])
        # the new texture's pixels on a game texture's
        v = head + self.plain[cut:] + tail
        info = P.TPKView(v, *P.chunk_tree(v, 0, len(v))[1][0])
        k = [t.name for t in info.texs].index('MOD_ICON')
        tb = v.find(info.texs[k].ti)
        h = bytearray(head); struct.pack_into('<I', h, tb + 0x30, info.texs[(k + 1) % len(info.texs)].img)
        self.refused({'MODPACKS/GLOBAL/GLOBALTEXTURESNA.BIN': vpak(self.plain, cut, bytes(h), tail)}, r'their pixels overlap')

    def test_memory_file(self):
        """a spliced file the memory file lists: the game reads its old header from there, unless the memory file's
        splice takes it off the list (as tools/modpacks.py's for CARS/WHEELS/TEXTURES.BIN)"""
        cut, head, tail = self.plain_pack()
        files = {'MODPACKS/GLOBAL/LISTED.BIN': vpak(self.plain, cut, head, tail, P.MP_NEEDS_MEMFILE)}
        self.refused(files, r'needs MODPACKS/GLOBAL/GlobalMemoryFile.bin')
        files['MODPACKS/GLOBAL/LISTED.BIN'] = vpak(self.plain, cut, head, tail)
        self.refused(files, r"GlobalMemoryFile.bin holds this file's header")
        mem = os.path.join(self.game, 'GLOBAL', 'GlobalMemoryFile.bin')
        with warnings.catch_warnings():                                  # (modpacks.py's read leaves its file to the GC)
            warnings.simplefilter('ignore', ResourceWarning)
            mcut, mhead, hit = unlisted(mem, [('GLOBAL/LISTED.BIN', len(self.plain))])
            gc.collect()
        self.assertTrue(hit)
        files['MODPACKS/GLOBAL/GlobalMemoryFile.bin'] = vpak(P.read(mem), mcut, mhead, b'')
        files['MODPACKS/GLOBAL/LISTED.BIN'] = vpak(self.plain, cut, head, tail, P.MP_NEEDS_MEMFILE)
        self.assertEqual(self.check(files), [])

    def test_damage_pack(self):
        self.assertEqual(self.check({'MODPACKS/DAMAGE/SCRATCHES.TPK': self.plain}), [])
        d = bytearray(self.plain)
        at = d.find(ti('GLOBAL_T0', 0, 0, 256, 0)[:0x30])
        struct.pack_into('<I', d, at + 0x30, len(d))                    # its image past the pack's data
        self.refused({'MODPACKS/DAMAGE/SCRATCHES.TPK': bytes(d)}, r"outside the pack's data")
        self.refused({'MODPACKS/DAMAGE/SCRATCHES.TPK': self.plain + b'\0' * 8}, r'not one texture pack chunk')

    def test_whine(self):
        def wav(ch):
            data = bytes(2000 * ch)
            return (b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, ch, 44100, 88200 * ch, 2 * ch, 16)
                    + b'data' + struct.pack('<I', len(data)) + data)
        self.assertEqual(self.check({'MODPACKS/WHINE/car_whine.wav': wav(1)}), [])
        self.refused({'MODPACKS/WHINE/car_whine.wav': wav(2)}, r'not 16-bit mono PCM')

    def test_parts(self):
        self.refused({'MODPACKS/PARTS.BIN': b'MPRT' + struct.pack('<7I', 1, 1, 1, 0, 0, 0, 0)}, r'not its 32 bytes')

    # --- TEXPACKS ----------------------------------------------------------------------------------------------------
    def texpack(self, version, size_fix=0, bad_z=False):
        lv = pixels('PAK', 256)
        z = zlib.compressobj(9, zlib.DEFLATED, -15); body = z.compress(lv) + z.flush()
        if bad_z: body = body[:len(body) // 2]
        data = lv if version == 1 else body
        names_at = 16 + (40 if version == 1 else 48)
        base = (names_at + 8 + 15) & ~15
        out = struct.pack('<4sIII', b'NTXP', version, 1, 0)
        out += struct.pack('<IIHHIHHIIIII', 1, 2, 8, 8, ARGB, 8, 8, ARGB, 1, base, len(data) + size_fix, names_at)
        if version == 2: out += bytes(8)
        out += b'PAK\0'
        return out + bytes(base - len(out)) + data

    def test_texpacks(self):
        self.assertEqual(self.check({'TEXPACKS/a.pak': self.texpack(1)}), [])
        self.assertEqual(self.check({'TEXPACKS/b.pak': self.texpack(2)}), [])
        self.refused({'TEXPACKS/a.pak': self.texpack(1, size_fix=-56)}, r'200 bytes of levels')
        self.refused({'TEXPACKS/b.pak': self.texpack(2, bad_z=True)}, r'do not inflate')

    # --- HQVINYLS ----------------------------------------------------------------------------------------------------
    def hqv(self, rows=((5, 1), (9, 3)), unsorted=False, overlap=False, past=False, not_avif=False):
        """a HQVINYLS/CARS/<car>.hqv (tools/hqvinyls.py): rows (hash, channels), each picture a stand-in AVIF box"""
        pic = lambda: struct.pack('>I', 20) + (b'ftypmif1' if not_avif else b'ftypavif') + bytes(8)
        n = len(rows); start = (16 + 16 * n + 15) & ~15
        head, data, at = bytearray(struct.pack('<4sIII', b'HQV1', 1, n, start)), b'', start
        order = list(rows)[::-1] if unsorted else sorted(rows)
        for i, (h, ch) in enumerate(order):
            b = pic(); off = at - (8 if overlap and i else 0)
            head += struct.pack('<IIIHBB', h, off, len(b) + (64 if past and i == n - 1 else 0), 1024, ch, 0)
            data += b; at += len(b)
        return bytes(head) + bytes(start - len(head)) + data

    def test_hqvinyls(self):
        self.assertEqual(self.check({'HQVINYLS/CARS/350Z.hqv': self.hqv()}), [])
        self.refused({'HQVINYLS/CARS/350Z.hqv': self.hqv(unsorted=True)}, r'not sorted by hash')
        self.refused({'HQVINYLS/CARS/350Z.hqv': self.hqv(overlap=True)}, r'overlap')
        self.refused({'HQVINYLS/CARS/350Z.hqv': self.hqv(past=True)}, r'outside the pictures')
        self.refused({'HQVINYLS/CARS/350Z.hqv': self.hqv(not_avif=True)}, r'not an AVIF')
        self.refused({'HQVINYLS/CARS/350Z.hqv': self.hqv(rows=((5, 4),))}, r'channel')
        self.refused({'HQVINYLS/CARS/350Z.hqv': b'HQV2' + bytes(12)}, r'not a HQV1 file')

    def test_cli(self):
        """the command line: a problem per line, exit 1; all good, exit 0"""
        import subprocess
        out = os.path.join(self.tmp, 'out')
        cut, head, tail = vinyl_pack(self.base, self.good_new())
        dst = os.path.join(out, 'VINYLPACKS', 'CARS', 'TEST', 'VINYLS.BIN')
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f: f.write(vpak(self.base, cut, head, tail))
        tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'packcheck.py')
        r = subprocess.run([sys.executable, '-I', tool, self.game, out], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('all good', r.stdout)
        with open(dst, 'wb') as f: f.write(vpak(self.base, cut, head, tail)[:-1])
        r = subprocess.run([sys.executable, '-I', tool, self.game, out, '--jobs', '1'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn('VINYLPACKS/CARS/TEST/VINYLS.BIN: ', r.stdout)


if __name__ == '__main__':
    unittest.main()
