#!/usr/bin/env python3
"""The Lithuanian language pack (LANGPACKS/lithuanian): the game's own font files with the Lithuanian letters
(Ą Č Ę Ė Į Š Ų Ū Ž and their small ones) and the quotes, dashes and ellipsis Lithuanian text needs added, and
tools/lang/lithuanian.json's texts.

The letters are the fonts' own: each is the font's base letter (A, c, e ...) with its mark (ogonek, caron, dot,
macron) taken from the same font where it has one (its ˇ ˙ ˛ ˉ, its â's circumflex turned over, its i's dot, its ¯),
else drawn from the TTF given for that font (Windows Arial for the Arial fonts, Impact for Impact).  „ “ ” ’ … – —
are put together from the font's own comma, period and hyphen.  The text writes the letters as bytes 0x8E-0x9F
(LT_BYTES) and the game's character table maps those bytes to the letters' Unicode; each font has a glyph at both.
Latin-1 (0xA0-0xFF) and the game's own punctuation bytes stay as they are.

Reads the game's LANGUAGES/English.bin, LANGUAGES/LanguageTextures.bin, GLOBAL/GLOBALA.BUN and
memcard/LOCALE_ENGLISH.loc; writes a new folder, never over them.  The font and texture formats are
tools/greekfont's (build_greek.py, FORMAT_NOTES.md), whose readers and writers this uses.
tools/langpacks.py --lt puts its output into the game folder.
"""
from __future__ import annotations
import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unicodedata

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'greekfont'))
import build_greek as gk                                    # noqa: E402  the formats: chunks, fonts, texture packs
from build_greek import check, chunk, chunks, u32, GLYPH, GameFont, TPK, assemble, read_text_chunk, read_loc, label_hash  # noqa: E402

LETTERS = 'ĄČĘĖĮŠŲŪŽąčęėįšųūž'
LT_BYTES = {ch: 0x8e + i for i, ch in enumerate(LETTERS)}            # 0x8E-0x9F: free in the game's character table
MARKS = {'Ą': ('A', 'ogonek'), 'Č': ('C', 'caron'), 'Ę': ('E', 'ogonek'), 'Ė': ('E', 'dot'), 'Į': ('I', 'ogonek'),
         'Š': ('S', 'caron'), 'Ų': ('U', 'ogonek'), 'Ū': ('U', 'macron'), 'Ž': ('Z', 'caron')}
MARKS.update({k.lower(): (b.lower(), m) for k, (b, m) in list(MARKS.items())})
# the game's own punctuation bytes (its character table maps them so), and the em dash at 0x8D as Greek's has it
PUNCT = {'’': 0x81, '™': 0x82, '–': 0x83, '”': 0x84, '…': 0x86, '“': 0x87,
         '„': 0x88, '—': 0x8d}
CODEC = {**LT_BYTES, **PUNCT}
REVERSE = {v: k for k, v in CODEC.items()}
NEW_TABLE = {0x8d: 0x2014, **{b: ord(ch) for ch, b in LT_BYTES.items()}}   # the character table's new entries
SPACING_MARK = {'caron': 0x2c7, 'dot': 0x2d9, 'ogonek': 0x2db, 'macron': 0x2c9}
ALPHA = 24                          # what counts as ink: the DXT3 atlases' alpha has some noise around the glyphs


def encode(s: str) -> bytes:
    s = unicodedata.normalize('NFC', s); out = bytearray()
    for ch in s:
        n = ord(ch)
        if ch in CODEC: out.append(CODEC[ch])
        elif 32 <= n < 127 or 0xa0 <= n < 0x100 or n in (9, 10, 13, 0x85): out.append(n)
        else: raise ValueError(f'Unsupported character U+{n:04X} {ch!r} in {s[:100]!r}')
    return bytes(out)


def decode(b: bytes) -> str:
    return ''.join(REVERSE.get(n, chr(n)) for n in b)


@dataclass
class Glyph:
    """a glyph's image and where it goes: dx, dy from the pen and the line's top, advance the pen's step"""
    im: object
    dx: int
    dy: int
    adv: int

    def ink(self):
        """the inked box in line coordinates (x0, y0, x1, y1), or None"""
        box = self.im.getchannel('A').point(lambda a: 255 if a >= ALPHA else 0).getbbox()
        return box and (self.dx + box[0], self.dy + box[1], self.dx + box[2], self.dy + box[3])

    def cropped(self) -> 'Glyph':
        x0, y0, x1, y1 = self.ink()
        return Glyph(self.im.crop((x0 - self.dx, y0 - self.dy, x1 - self.dx, y1 - self.dy)), x0, y0, self.adv)


def compose(parts: list[Glyph], adv: int) -> Glyph:
    from PIL import Image
    x0 = min(p.dx for p in parts); y0 = min(p.dy for p in parts)
    x1 = max(p.dx + p.im.size[0] for p in parts); y1 = max(p.dy + p.im.size[1] for p in parts)
    im = Image.new('RGBA', (x1 - x0, y1 - y0), (255, 255, 255, 0))
    for p in parts:
        layer = Image.new('RGBA', im.size, (255, 255, 255, 0)); layer.paste(p.im, (p.dx - x0, p.dy - y0))
        im = Image.alpha_composite(im, layer)
    return Glyph(im, x0, y0, adv)


def moved(g: Glyph, x: int, y: int) -> Glyph:
    """g's ink placed with its top left at (x, y)"""
    c = g.cropped()
    return Glyph(c.im, x, y, g.adv)


class Font:
    """one of the game's fonts and its atlas, as glyphs to put new ones together from"""

    def __init__(self, font: GameFont, atlas, faces: dict[str, Path]):
        self.font, self.atlas, self.faces = font, atlas, faces

    def has(self, ch: str) -> bool:
        return ord(ch) in self.font.records

    def glyph(self, ch: str) -> Glyph:
        _c, w, h, x, y, _page, dx, dy, _nk, _ki, adv = GLYPH.unpack(self.font.records[ord(ch)])
        return Glyph(self.atlas.crop((x, y, x + w, y + h)), dx, dy, adv)

    def face(self) -> Path:
        n = self.font.name
        if 'impact' in n: return self.faces.get('impact') or self.faces.get('bold') or self.faces['regular']
        if '38bi' in n: return self.faces.get('bold_italic') or self.faces.get('bold') or self.faces['regular']
        if 'conduit' in n: return self.faces.get('italic') or self.faces['regular']
        return self.faces['regular']

    def drawn(self, ch: str) -> Glyph:
        """ch from the TTF for this font, at the size whose H is the font's H; its ink only, on the font's baseline"""
        from PIL import Image, ImageDraw, ImageFont
        H = self.glyph('H').ink(); cap = H[3] - H[1]; baseline = H[3]
        path = str(self.face())
        probe = ImageFont.truetype(path, 400).getbbox('H', anchor='ls')
        ft = ImageFont.truetype(path, max(8, round(cap * 400 / (probe[3] - probe[1]))) * 4)
        left, top, right, bottom = ft.getbbox(ch, anchor='ls')
        check(right > left and bottom > top, f'{path}: no {ch!r} to draw')
        mask = Image.new('L', (right - left, bottom - top), 0)
        ImageDraw.Draw(mask).text((-left, -top), ch, font=ft, anchor='ls', fill=255)
        w, h = max(1, round((right - left) / 4)), max(1, round((bottom - top) / 4))
        im = Image.new('RGBA', (w, h), (255, 255, 255, 0)); im.putalpha(mask.resize((w, h), Image.Resampling.LANCZOS))
        return Glyph(im, round(left / 4), baseline + round(top / 4), max(1, round(ft.getlength(ch) / 4)))

    # ---- the marks, and where above a letter they go -------------------------------------------------------------
    def above_part(self, accented: str, base: str) -> Glyph | None:
        """the accented letter's ink above its base letter's: the accent alone (â's circumflex), or None"""
        if not (self.has(accented) and self.has(base)): return None
        a, b = self.glyph(accented), self.glyph(base)
        top = b.ink()[1]
        rows = top - a.dy - 1                                     # the accented glyph's rows above the base's top
        if rows <= 0: return None
        part = Glyph(a.im.crop((0, 0, a.im.size[0], rows)), a.dx, a.dy, a.adv)
        return part.cropped() if part.ink() else None

    def mark(self, kind: str, upper: bool) -> Glyph:
        if self.has(chr(SPACING_MARK[kind])): return self.glyph(chr(SPACING_MARK[kind])).cropped()
        if kind == 'caron':
            circ = self.above_part('Â', 'A') if upper else self.above_part('â', 'a')
            if circ:
                from PIL import ImageOps
                return Glyph(ImageOps.flip(circ.im), circ.dx, circ.dy, circ.adv)
        if kind == 'macron' and self.has('¯'): return self.glyph('¯').cropped()
        if kind == 'dot':
            i = self.glyph('i'); alpha = i.im.getchannel('A')
            w, h = i.im.size; ink = [any(alpha.getpixel((x, y)) >= ALPHA for x in range(w)) for y in range(h)]
            first = ink.index(True); gap = next((y for y in range(first, h) if not ink[y]), None)
            if gap is not None and any(ink[gap:]):
                return Glyph(i.im.crop((0, 0, w, gap)), i.dx, i.dy, i.adv).cropped()
        return self.drawn(chr(SPACING_MARK[kind]))

    def above_place(self, base: str, upper: bool) -> tuple[int, float]:
        """where a mark above base sits: (its ink's bottom row, the x its ink's centre is at, from base's ink centre)
        as the font's own â (Â) has its circumflex; without one, the mark a little above the letter, centred"""
        ref = self.above_part('Â', 'A') if upper else self.above_part('â', 'a')
        b = self.glyph('A' if upper else 'a').ink()
        if ref:
            r = ref.ink()
            return r[3], (r[0] + r[2]) / 2 - (b[0] + b[2]) / 2
        H = self.glyph('H').ink(); gap = max(1, round((H[3] - H[1]) * .1))
        return self.glyph(base).ink()[1] - gap, 0.0

    # ---- the new glyphs -----------------------------------------------------------------------------------------
    def letter(self, ch: str) -> Glyph:
        if self.has(ch): return self.glyph(ch)                   # conduitmditc_tt21i has Š š Ž ž of its own
        base_ch, kind = MARKS[ch]; upper = ch.isupper()
        base = self.glyph(base_ch); bx0, by0, bx1, by1 = base.ink()
        m = self.mark(kind, upper).cropped(); mw, mh = m.im.size    # its ink only: a TTF's box is taller
        if kind == 'ogonek':
            # under the letter's right foot: the right edge of its lowest rows' ink
            alpha = base.im.getchannel('A'); w, h = base.im.size
            low = [x for y in range(max(0, h - max(2, h // 4)), h) for x in range(w) if alpha.getpixel((x, y)) >= ALPHA]
            right = base.dx + max(low) + 1
            overlap = max(1, round(mh * .2))
            return compose([base, moved(m, right - mw, by1 - overlap)], base.adv)
        bottom, shift = self.above_place(base_ch, upper)
        centre = (bx0 + bx1) / 2 + shift
        return compose([base, moved(m, round(centre - mw / 2), bottom - mh)], base.adv)

    def punct(self, ch: str) -> Glyph:
        if self.has(ch): return self.glyph(ch)
        cap_top = self.glyph('H').ink()[1]
        if ch == '™': return self.drawn(ch)
        if ch == '…':
            p = self.glyph('.'); return compose([Glyph(p.im, p.dx + i * p.adv, p.dy, 0) for i in range(3)], 3 * p.adv)
        if ch in '–—':
            hy = self.glyph('-').cropped(); n = self.glyph('n').adv
            adv = n if ch == '–' else 2 * n
            from PIL import Image
            w = max(hy.im.size[0], round(adv * .9))
            return Glyph(hy.im.resize((w, hy.im.size[1]), Image.Resampling.BILINEAR), round((adv - w) / 2), hy.dy, adv)
        comma = self.glyph(',')
        c = comma.cropped(); cw = c.im.size[0]
        gap = max(1, round((self.glyph('H').ink()[3] - cap_top) * .08))
        if ch == '„':                                       # „ two commas on the line
            return compose([c, Glyph(c.im, c.dx + cw + gap, c.dy, 0)], comma.adv + cw + gap)
        if ch == '“': c = Glyph(c.im.rotate(180), c.dx, c.dy, 0)   # “ the comma turned over
        high = Glyph(c.im, c.dx, cap_top, 0)                     # ’ ” “ at the capitals' top
        if ch == '’': return Glyph(high.im, high.dx, high.dy, comma.adv)
        return compose([high, Glyph(high.im, high.dx + cw + gap, high.dy, 0)], comma.adv + cw + gap)


def glyphs_for(f: Font) -> tuple[dict, dict]:
    """the rendered glyphs {char: (image, w, h, dx, dy, advance)} and the records wanted {code: char}: each letter
    and punctuation mark at its byte and its Unicode, but no record the font already has"""
    rendered, wanted = {}, {}
    for ch in LETTERS + ''.join(PUNCT):
        codes = [c for c in (ord(ch), CODEC[ch]) if c not in f.font.records]
        if not codes: continue
        g = f.letter(ch) if ch in LT_BYTES else f.punct(ch)
        w, h = g.im.size
        check(w <= 255 and h <= 255 and -128 <= g.dx <= 127 and -128 <= g.dy <= 127 and 0 < g.adv < 32768,
              f'{f.font.name}: {ch!r}: glyph metrics overflow')
        rendered[ch] = (g.im, w, h, g.dx, g.dy, g.adv)
        for c in codes: wanted[c] = ch
    return rendered, wanted


def extend(font: GameFont, tex, faces: dict[str, Path]):
    original = tex.rgba()
    f = Font(font, original, faces)
    rendered, wanted = glyphs_for(f)
    result, parsed, image = font.add_glyphs(tex, original, rendered, wanted)
    for ch in LETTERS + ''.join(PUNCT):
        for code in (ord(ch), CODEC[ch]):
            check(code in parsed.records, f'{font.name}: no glyph for {ch!r} at {code:#x}')
        r = GLYPH.unpack(parsed.records[CODEC[ch]])
        if r[0] in wanted:
            check(image.getchannel('A').crop((r[3], r[4], r[3] + r[1], r[4] + r[2])).getbbox() is not None,
                  f'{font.name}: {ch!r} drew nothing')
    tex.replace_image(image, original.size[1])
    return result, image, {'name': font.name, 'original_glyphs': len(font.records), 'output_glyphs': len(parsed.records),
                           'original_size': list(original.size), 'output_size': list(image.size),
                           'drawn_from': f.face().name}


def translate_chunk(body: bytes, texts: dict[str, str]):
    """the language chunk with the translation's texts (and the others as they were), and the character table's
    entries for the new bytes"""
    _conv, tab, old = read_text_chunk(body); mapping = {}
    for k, v in texts.items():
        h = int(k, 16) if k.startswith('0x') else label_hash(k)
        check(h not in mapping, 'Duplicate translation hash')
        mapping[h] = encode(v)
        check(decode(mapping[h]) == unicodedata.normalize('NFC', v), f'Encoding round trip failed: {k}')
    combined = {**old, **mapping}; strings = bytearray(); offsets = {}; entries = bytearray()
    for h, v in sorted(combined.items()):
        check(b'\0' not in v, 'Embedded NUL in translation')
        if v not in offsets: offsets[v] = len(strings); strings += v + b'\0'
        entries += struct.pack('<II', h, offsets[v])
    strings += bytes(-len(strings) % 4)
    conversion = bytearray(body[16:tab])
    check(u32(conversion, 0) == 256, 'Unexpected character conversion table')
    for b, u in NEW_TABLE.items():
        check(struct.unpack_from('<H', conversion, 4 + 2 * b)[0] == 0, f'Byte {b:#x} is already in the character table')
        struct.pack_into('<H', conversion, 4 + 2 * b, u)
    result = struct.pack('<4I', 16, len(combined), tab, tab + len(entries)) + conversion + entries + strings
    _c, _t, verify = read_text_chunk(result)
    check(verify == combined, 'Text pack read-back mismatch')
    return result, {'source_text_records': len(old), 'translated_records': len(mapping), 'output_text_records': len(verify),
                    'kept_as_they_were': len([h for h in old if h not in mapping])}


def write_loc(template: bytes, texts: list[str]) -> bytes:
    """the memory card's texts in Unicode, as the game's own LOCALE_POLISH.loc and LOCALE_CZECH.loc have theirs (ą
    U+0105, č U+010D), which the fonts draw at their Unicode records.  Not the text's bytes, as Greek's and Turkish's
    are: the save screens read 0x80-0x9F as Windows-1252 (0x9C œ, 0x99 ™), where LT_BYTES are."""
    q = template.find(b'LOCL'); n = len(texts)
    check(len(read_loc(template)) == n, 'Save locale count mismatch')
    body = bytearray(); offsets = []
    for s in texts:
        s = unicodedata.normalize('NFC', s); encode(s)               # only what the fonts draw
        offsets.append(16 + 4 * n + len(body)); body += s.encode('utf-16-le') + bytes(4)
    body += bytes(-len(body) % 4)
    result = template[:q] + b'LOCL' + struct.pack(f'<III{n}I', 16 + 4 * n + len(body), 0, n, *offsets) + body
    check(read_loc(result) == [unicodedata.normalize('NFC', s) for s in texts], 'Locale read-back mismatch')
    return result


def read_assets(path: Path) -> dict[str, bytes]:
    files = {'english.bin': 'LANGUAGES/English.bin', 'languagetextures.bin': 'LANGUAGES/LanguageTextures.bin',
             'globala.bun': 'GLOBAL/GLOBALA.BUN', 'locale_english.loc': 'memcard/LOCALE_ENGLISH.loc'}
    found = {}
    for key, rel in files.items():
        hits = [p for p in path.rglob('*') if p.is_file() and p.name.lower() == key
                and not any(x.upper() == 'LANGPACKS' for x in p.relative_to(path).parts)]
        check(hits, f'{path}: no {rel} below it')
        exact = [p for p in hits if p.relative_to(path).as_posix().lower() == rel.lower()]
        found[key] = (exact or hits)[0].read_bytes()
    return found


def build(source: Path, text_path: Path, faces: dict[str, Path], output: Path) -> dict:
    from PIL import __version__ as pillow_version
    inputs = read_assets(source)
    catalogue = json.loads(text_path.read_text(encoding='utf-8'))
    check(isinstance(catalogue.get('texts'), dict) and isinstance(catalogue.get('memcard'), list), 'Invalid Lithuanian JSON')
    output = output.resolve(); target = output / 'LANGPACKS' / 'lithuanian'
    check(not target.exists(), f'{target} already exists; choose another output folder')
    font_chunks, packs = [], []
    replacements = {'english.bin': {}, 'globala.bun': {}, 'languagetextures.bin': {}}
    for key in replacements:
        b = inputs[key]
        for cid, s, e in chunks(b):
            if cid == 0x30201: font_chunks.append((key, s - 8, GameFont.read(b[s:e])))
            if cid == 0xb3300000: packs.append((key, s - 8, TPK.read(b, s, e)))
    check({f.name for _k, _p, f in font_chunks} == gk.FONTS, 'Different font set: this builder targets the PC game\'s')
    textures = {t.name.lower(): t for _k, _p, pack in packs for t in pack.textures}
    report = {'status': 'built', 'pillow': pillow_version,
              'input_sha256': {k: hashlib.sha256(v).hexdigest() for k, v in inputs.items()}, 'fonts': []}
    previews = {}
    for key, pos, f in font_chunks:
        check(f.name in textures, f'Missing texture for {f.name}')
        if f.name in gk.KEEP_FONTS: continue
        print('Building', f.name, flush=True)
        body, image, info = extend(f, textures[f.name], faces)
        replacements[key][pos] = chunk(0x30201, body); report['fonts'].append(info); previews[f.name] = (GameFont.read(body), image)
    english = inputs['english.bin']
    parts = [(s, e) for cid, s, e in chunks(english) if cid == 0x39000]
    check(len(parts) == 1, 'Expected one language text chunk')
    s, e = parts[0]; body, stats = translate_chunk(english[s:e], catalogue['texts'])
    replacements['english.bin'][s - 8] = chunk(0x39000, body); report.update(stats)
    for key, pos, pack in packs:
        check(pos == 0, 'TPK is not the first chunk; unsupported input variant')
        rebuilt = pack.write_compressed() if pack.blobs else pack.write(0)
        reread = TPK.read(rebuilt, 8, len(rebuilt))
        for before, after in zip(pack.textures, reread.textures):
            check(before.pixels == after.pixels and before.palette == after.palette, 'Texture reserialization changed pixels')
        replacements[key][pos] = rebuilt
    built = {'LANGUAGES/English.bin': assemble(inputs['english.bin'], replacements['english.bin']),
             'LANGUAGES/LanguageTextures.bin': assemble(inputs['languagetextures.bin'], replacements['languagetextures.bin']),
             'GLOBAL/GLOBALA.BUN': assemble(inputs['globala.bun'], replacements['globala.bun']),
             'memcard/LOCALE_ENGLISH.loc': write_loc(inputs['locale_english.loc'], catalogue['memcard'])}
    report['generated_files'] = {k: {'bytes': len(v), 'sha256': hashlib.sha256(v).hexdigest()} for k, v in built.items()}
    output.mkdir(parents=True, exist_ok=True)
    temp = Path(tempfile.mkdtemp(prefix='.lithuanian-build-', dir=output))
    try:
        staged = temp / 'lithuanian'; staged.mkdir()
        for rel, data in built.items():
            p = staged / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(data)
        target.parent.mkdir(parents=True, exist_ok=True); staged.rename(target)
    finally: shutil.rmtree(temp, ignore_errors=True)
    for name, (font, image) in previews.items(): preview(font, image).save(output / f'preview-{name}.png')
    (output / 'lithuanian-build-report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(f'\nCreated: {target}\nPreviews: {output}/preview-*.png')
    return report


def preview(font: GameFont, atlas, text: str = 'ĄČĘĖĮŠŲŪŽ ąčęėįšųūž „Žiedas“ – lenktynės… ’ — ™ AaCcEeIiSsUuZz'):
    """the sample line drawn as the game would from the built font's records, four times its size"""
    from PIL import Image
    recs = {GLYPH.unpack(r)[0]: GLYPH.unpack(r) for r in font.records.values()}
    line = max(r[6] + r[2] for r in recs.values()) + 8
    width = sum(recs[ord(c)][10] if ord(c) in recs else 8 for c in text) + 8
    im = Image.new('RGBA', (width, line), (30, 30, 46, 255)); x = 4
    for c in text:
        r = recs.get(ord(c))
        if not r: x += 8; continue
        _c, w, h, gx, gy, _p, dx, dy, *_rest, adv = r
        g = atlas.crop((gx, gy, gx + w, gy + h))
        layer = Image.new('RGBA', im.size, (0, 0, 0, 0)); layer.paste(g, (x + dx, 4 + dy)); im = Image.alpha_composite(im, layer)
        x += adv
    return im.resize((im.size[0] * 4, im.size[1] * 4), Image.Resampling.NEAREST)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--input', type=Path, required=True, help='the game folder (or a folder with the four files)')
    ap.add_argument('--text', type=Path, default=Path(__file__).resolve().parents[1] / 'lang' / 'lithuanian.json')
    ap.add_argument('--font', type=Path, required=True, help='Arial (arial.ttf): the marks the Arial fonts lack')
    ap.add_argument('--bold-font', type=Path, help='Arial Bold (arialbd.ttf)')
    ap.add_argument('--italic-font', type=Path, help='Arial Italic (ariali.ttf): for the italic Conduit fonts')
    ap.add_argument('--bold-italic-font', type=Path, help='Arial Bold Italic (arialbi.ttf)')
    ap.add_argument('--impact-font', type=Path, help='Impact (impact.ttf): for font_impact36')
    ap.add_argument('--output', type=Path, default=Path.cwd() / 'Lithuanian_Build')
    a = ap.parse_args()
    faces = {k: v for k, v in (('regular', a.font), ('bold', a.bold_font), ('italic', a.italic_font),
                               ('bold_italic', a.bold_italic_font), ('impact', a.impact_font)) if v}
    try:
        for p in faces.values(): check(p.is_file(), f'Font does not exist: {p}')
        build(a.input, a.text, faces, a.output)
    except (OSError, ValueError, struct.error, KeyError) as e:
        print(f'ERROR: {e}', file=sys.stderr); return 1
    return 0


if __name__ == '__main__': raise SystemExit(main())
