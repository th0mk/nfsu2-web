#!/usr/bin/env python3
"""The Greek language pack (LANGPACKS/greek): the game's own font files with Greek letters drawn in from a
Greek-capable TTF/OTF (Fira Sans: Regular, Bold, Italic, Bold Italic), and tools/lang/greek.json's texts.

Reads the game's LANGUAGES/English.bin, LANGUAGES/LanguageTextures.bin, GLOBAL/GLOBALA.BUN and memcard/LOCALE_*.loc;
writes a new folder, never over them.  tools/langpacks.py --el puts its output into the game folder.
See README.md and FORMAT_NOTES.md.
"""
from __future__ import annotations
import argparse
from dataclasses import dataclass
import hashlib
import io
import json
import math
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unicodedata
import zipfile

GREEK = ''.join(chr(c) for c in range(0x370, 0x400)
                if c in (0x386, 0x388, 0x389, 0x38a, 0x38c, 0x38e, 0x38f, 0x390)
                or 0x391 <= c <= 0x3a1 or 0x3a3 <= c <= 0x3ce)
PRESERVE = set(map(ord, '\u00a0\u00a9\u00ab\u00ae\u00b7\u00bb\u00bc\u00d1'))
SLOTS = [c for c in range(0xa1, 0x100) if c not in PRESERVE]
GREEK_BYTES = dict(zip(GREEK, SLOTS))
PUNCT = {'\u2019': 0x81, '\u2122': 0x82, '\u2013': 0x83,
         '\u201d': 0x84, '\u2026': 0x86, '\u201c': 0x87, '\u2014': 0x8d}
CODEC = {**GREEK_BYTES, **PUNCT}
REVERSE = {v: k for k, v in CODEC.items()}
FONTS = {'font_arial', 'conduitmditc_tt14i', 'lcd_let48',
         'font_conduitmditctt38bi', 'font_arial12', 'font_impact36', 'conduitmditc_tt21i'}
KEEP_FONTS = {'lcd_let48'}          # the dashboard's digits: no text is drawn in it
GLYPH = struct.Struct('<HBBHHBbbBHh')
MAX_ASSET = 64 * 1024 * 1024

def check(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)

def u32(b: bytes | bytearray, p: int) -> int:
    return struct.unpack_from('<I', b, p)[0]

def chunk(cid: int, body: bytes | bytearray) -> bytes:
    return struct.pack('<II', cid, len(body)) + body

def chunks(b: bytes, start: int = 0, end: int | None = None):
    end = len(b) if end is None else end
    p = start
    while p < end:
        check(p + 8 <= end, f'Truncated chunk header at {p:#x}')
        cid, size = struct.unpack_from('<II', b, p)
        check(size <= end - p - 8, f'Invalid chunk size at {p:#x}')
        yield cid, p + 8, p + 8 + size
        p += 8 + size

def padding(position: int, alignment: int = 128) -> bytes:
    if position % alignment == 0:
        return b''
    return chunk(0, bytes(-(position + 8) % alignment))

def jdlz(b: bytes) -> bytes:
    check(b[:4] == b'JDLZ' and len(b) >= 16, 'Unsupported texture compression (expected JDLZ)')
    n = u32(b, 8)
    check(n <= MAX_ASSET, 'JDLZ allocation too large')
    out = bytearray(n)
    i, o, f1, f2 = 16, 0, 1, 1
    while o < n:
        check(i < len(b), 'Truncated JDLZ data')
        if f1 == 1:
            f1 = b[i] | 0x100; i += 1
        if f2 == 1:
            check(i < len(b), 'Truncated JDLZ flags')
            f2 = b[i] | 0x100; i += 1
        check(i < len(b), 'Truncated JDLZ token')
        if f1 & 1:
            check(i + 2 <= len(b), 'Truncated JDLZ reference')
            if f2 & 1:
                length, distance = (b[i + 1] | (b[i] & 0xf0) << 4) + 3, (b[i] & 15) + 1
            else:
                distance, length = (b[i + 1] | (b[i] & 0xe0) << 3) + 17, (b[i] & 31) + 3
            check(distance <= o and length <= n - o, 'Invalid JDLZ reference')
            i += 2
            for k in range(length): out[o + k] = out[o + k - distance]
            o += length; f2 >>= 1
        else:
            out[o] = b[i]; o += 1; i += 1
        f1 >>= 1
    return bytes(out)

@dataclass
class Texture:
    info: bytearray
    platform: bytearray
    pixels: bytes
    palette: bytes
    changed: bool = False

    @property
    def name(self) -> str:
        return bytes(self.info[12:36]).split(b'\0')[0].decode('ascii')

    @property
    def size(self) -> tuple[int, int]:
        return struct.unpack_from('<HH', self.info, 0x44)

    def rgba(self):
        from PIL import Image
        w, h = self.size
        fmt = bytes(self.platform[20:24])
        if fmt == struct.pack('<I', 21):
            check(len(self.pixels) >= w*h*4, f'{self.name}: short ARGB texture')
            return Image.frombytes('RGBA', (w, h), self.pixels[:w*h*4], 'raw', 'BGRA')
        check(fmt in (b'DXT1', b'DXT3', b'DXT5'), f'{self.name}: unsupported format {fmt!r}')
        header = bytearray(128); header[:4] = b'DDS '
        struct.pack_into('<7I', header, 4, 124, 0x81007, h, w, len(self.pixels), 0, 1)
        struct.pack_into('<II4s', header, 76, 32, 4, fmt)
        struct.pack_into('<I', header, 108, 0x1000)
        return Image.open(io.BytesIO(header + self.pixels)).convert('RGBA')

    def replace_image(self, image, kept: int) -> None:
        """the texture drawn anew, in its own format, one level (no smaller ones): DXT3 or DXT5 as it was (a quarter
        of ARGB's memory), its first `kept` rows the old blocks as they were and only the rows below encoded; else
        ARGB"""
        w, h = image.size
        fmt = bytes(self.platform[20:24])
        if fmt in (b'DXT3', b'DXT5'):
            check(kept % 4 == 0 and len(self.pixels) >= w * kept, f'{self.name}: the old blocks are not whole')
            out = io.BytesIO(); image.crop((0, kept, w, h)).save(out, 'DDS', pixel_format=fmt.decode())
            self.pixels = self.pixels[:w * kept] + out.getvalue()[128:]
            check(len(self.pixels) == w * h, f'{self.name}: {fmt.decode()} of another size')
        else:
            self.pixels = image.tobytes('raw', 'BGRA')
            self.info[0x4a] = 0x20
            struct.pack_into('<I', self.platform, 20, 21)
        self.palette = b''
        struct.pack_into('<III', self.info, 0x38, len(self.pixels), 0, len(self.pixels))
        struct.pack_into('<HH', self.info, 0x44, w, h)
        self.info[0x48] = int(math.log2(w)); self.info[0x49] = int(math.log2(h))
        self.info[0x4e] = 1
        self.changed = True

@dataclass
class TPK:
    header: bytes
    index: bytes
    data_header: bytes
    textures: list[Texture]
    table: bytes = b''              # a compressed pack's 0x33310003, and its blobs (JDLZ), in that order
    blobs: tuple = ()

    @classmethod
    def read(cls, b: bytes, start: int, end: int) -> 'TPK':
        top = {cid: (s, e) for cid, s, e in chunks(b, start, end)}
        check(0xb3310000 in top and 0xb3320000 in top, 'Incomplete texture pack')
        info = {cid: (s, e) for cid, s, e in chunks(b, *top[0xb3310000])}
        data = {cid: (s, e) for cid, s, e in chunks(b, *top[0xb3320000])}
        check(0x33310001 in info and 0x33310002 in info and 0x33320001 in data, 'Unknown TPK layout')
        textures, blobs, table = [], [], b''
        if 0x33310003 in info:
            s, e = info[0x33310003]
            table = b[s:e]
            check((e-s) % 24 == 0, 'Invalid compressed texture table')
            for p in range(s, e, 24):
                nh, off, length, raw_length = struct.unpack_from('<4I', b, p)
                check(off + length <= len(b), 'Texture blob outside file')
                blob = b[off:off+length]; blobs.append(blob)
                raw = blob[16:16+u32(blob, 8)] if blob[:4] == b'RAWW' else jdlz(blob)
                check(len(raw) == raw_length and len(raw) >= 156, 'Unexpected texture size')
                ti, pf = bytearray(raw[-156:-32]), bytearray(raw[-32:])
                check(u32(ti, 36) == nh, 'Texture hash mismatch')
                io_, po, size, psz = struct.unpack_from('<4I', ti, 48)
                check(size <= len(raw)-156, 'Texture image too large')
                pal_start = po - io_
                check(not psz or 0 <= pal_start <= len(raw)-156-psz, 'Invalid palette')
                textures.append(Texture(ti, pf, raw[:size], raw[pal_start:pal_start+psz] if psz else b''))
        else:
            check(0x33310004 in info and 0x33310005 in info and 0x33320002 in data, 'Unsupported TPK')
            s, e = info[0x33310004]; ps, pe = info[0x33310005]
            check((e-s) % 124 == 0 and pe-ps == (e-s)//124*32, 'Invalid texture tables')
            pix = (data[0x33320002][0] + 127) & ~127
            for i, p in enumerate(range(s, e, 124)):
                ti, pf = bytearray(b[p:p+124]), bytearray(b[ps+i*32:ps+(i+1)*32])
                io_, po, size, psz = struct.unpack_from('<4I', ti, 48)
                check(pix+io_+size <= data[0x33320002][1], 'Texture pixels outside pack')
                check(not psz or pix+po+psz <= data[0x33320002][1], 'Texture palette outside pack')
                textures.append(Texture(ti, pf, b[pix+io_:pix+io_+size], b[pix+po:pix+po+psz] if psz else b''))
        return cls(b[slice(*info[0x33310001])], b[slice(*info[0x33310002])],
                   b[slice(*data[0x33320001])], textures, table, tuple(blobs))

    def write_compressed(self) -> bytes:
        """As the game's own compressed packs are (LanguageTextures.bin), at the start of a file: each texture a blob
        listed in 0x33310003 (hash, where in the file, length, unpacked length, 8 bytes kept) that unpacks to its
        pixels, palette, info and platform records.  An unchanged texture keeps its JDLZ blob and its place; a changed
        one is stored (RAWW, as the Polish translation's pack has all of them), placed past the others' end (its info's
        offset; grown, it would reach into the next).  JDLZ of our own, which jdlz() reads back, crashed the game now
        and then; an uncompressed pack (write()) draws nothing in the language's fonts."""
        check(len(self.blobs) == len(self.textures), 'Not a compressed texture pack')
        at = max(u32(t.info, 52) + u32(t.info, 60) for t in self.textures)      # where the last one's palette ends
        packed = []
        for i, t in enumerate(self.textures):
            if not t.changed:
                packed.append((u32(t.info, 36), self.blobs[i], u32(self.table, 24 * i + 12))); continue
            at += -at % 0x2000
            ti = bytearray(t.info)
            struct.pack_into('<II', ti, 48, at, at + len(t.pixels))
            at += len(t.pixels) + len(t.palette)
            raw = t.pixels + t.palette + bytes(ti) + bytes(t.platform)
            packed.append((u32(ti, 36), b'RAWW\x01\x10\x00\x00' + struct.pack('<II', len(raw), len(raw) + 16) + raw, len(raw)))
        def pack(table: bytes) -> tuple[bytes, bytes, int]:
            head = chunk(0, bytes(0x30)) + chunk(0xb3310000, chunk(0x33310001, self.header)
                   + chunk(0x33310002, self.index) + chunk(0x33310003, table))
            head += padding(8 + len(head))
            dhead = chunk(0x33320001, self.data_header)
            dhead += padding(8 + len(head) + 8 + len(dhead))
            return head, dhead, 8 + len(head) + 8 + len(dhead) + 8      # where 0x33320002's data begin
        head, dhead, start = pack(bytes(24 * len(packed)))
        data, table = bytearray(), bytearray()
        for i, (h, blob, raw_len) in enumerate(packed):
            data += bytes(-(start + len(data)) % 0x80)
            table += struct.pack('<4I', h, start + len(data), len(blob), raw_len) + self.table[24*i+16:24*i+24]
            data += blob
        data += bytes(-len(data) % 4)
        head, dhead, start2 = pack(bytes(table))
        check(start2 == start, 'Compressed pack layout moved')
        return chunk(0xb3300000, head + chunk(0xb3320000, dhead + chunk(0x33320002, data)))

    def write(self, position: int = 0) -> bytes:
        pixels, tis, pfs = bytearray(), bytearray(), bytearray()
        for t in self.textures:
            pixels += bytes(-len(pixels) % 128)
            ti = bytearray(t.info)
            struct.pack_into('<II', ti, 48, len(pixels), len(pixels)+len(t.pixels))
            pixels += t.pixels + t.palette
            tis += ti; pfs += t.platform
        info = chunk(0xb3310000, chunk(0x33310001, self.header) + chunk(0x33310002, self.index)
                     + chunk(0x33310004, tis) + chunk(0x33310005, pfs))
        body = chunk(0, bytes(0x30)) + info
        body += padding(position + 8 + len(body))
        data_start = position + 8 + len(body)
        dbody = chunk(0x33320001, self.data_header)
        dbody += padding(data_start + 8 + len(dbody))
        pix_start = data_start + 8 + len(dbody) + 8
        dbody += chunk(0x33320002, bytes(-pix_start % 128) + pixels)
        body += chunk(0xb3320000, dbody)
        return chunk(0xb3300000, body)

@dataclass
class GameFont:
    body: bytes
    name: str
    header: bytearray
    records: dict[int, bytes]
    tail: bytes
    old_kern_offset: int

    @classmethod
    def read(cls, body: bytes) -> 'GameFont':
        check(len(body) >= 0x280 and body[0x200:0x204] == b'FNTF', 'Unknown font record')
        name = body[:256].split(b'\0')[0].decode('ascii')
        h = bytearray(body[512:640]); n = struct.unpack_from('<H', h, 10)[0]
        off, kern, end = struct.unpack_from('<3I', h, 20)
        check(off == 128 and kern == 128+n*16 and end == len(body)-512, f'{name}: unknown font layout')
        recs = [body[512+off+i*16:512+off+(i+1)*16] for i in range(n)]
        codes = [struct.unpack_from('<H', r)[0] for r in recs]
        check(codes == sorted(set(codes)), f'{name}: unsorted/duplicate glyphs')
        return cls(body, name, h, dict(zip(codes, recs)), body[512+kern:], kern)

    def face(self, faces: dict[str, Path]) -> Path:
        """the font file to draw this font's Greek letters with: italic for the game's italic Conduit fonts, bold
        italic for its bold italic one, bold for Impact, each as far as given; else the regular"""
        n = self.name
        if '38bi' in n: return faces.get('bold_italic') or faces.get('bold') or faces['regular']
        if 'impact' in n: return faces.get('bold') or faces['regular']
        if n.startswith('conduit') or 'conduitmditc' in n: return faces.get('italic') or faces['regular']
        return faces['regular']

    def extend(self, tex: Texture, faces: dict[str, Path]):
        from PIL import Image, ImageDraw, ImageFont
        original = tex.rgba()
        ow, oh = original.size
        invw, invh = struct.unpack_from('<ff', self.header, 64)
        check(abs(invw-1/ow)<1e-7 and abs(invh-1/oh)<1e-7, f'{self.name}: font/texture dimensions disagree')
        recs = dict(self.records)
        ref = GLYPH.unpack(recs.get(ord('H'), recs[ord('0')]))
        cap_height, baseline = ref[2], ref[7] + ref[2]
        path = self.face(faces)
        probe = ImageFont.truetype(str(path), 100*4)
        box = probe.getbbox('H', anchor='ls')
        px = max(5, round(cap_height * 100/(box[3]-box[1])*4))
        ft = ImageFont.truetype(str(path), px*4)
        bH = ft.getbbox('H', anchor='ls')
        xscale = min(1.35, max(.55, ref[-1]/(ft.getlength('H')/4)))
        # Both byte-slot glyphs and true Unicode aliases are added. The existing
        # byte conversion table is retained; save-system wide strings also work.
        wanted = {ord(ch): ch for ch in GREEK}
        wanted.update({code: ch for ch, code in GREEK_BYTES.items()})
        extras = set(chr(i) for i in range(32, 127)) | set(map(chr, PRESERVE)) | set(PUNCT)
        for ch in extras:
            if ord(ch) not in recs: wanted[ord(ch)] = ch
        for ch, code in PUNCT.items():
            wanted[code] = ch
        # A scalar may occur at both a legacy slot and a Unicode codepoint.
        rendered = {}
        for ch in sorted(set(wanted.values())):
            left, top, right, bottom = ft.getbbox(ch, anchor='ls')
            width, height = max(0, right-left), max(0, bottom-top)
            advance = max(1, round(ft.getlength(ch)/4*xscale))
            if not width or not height:
                rendered[ch] = (Image.new('RGBA', (1, 1), (255,255,255,0)), 0, 0, 0, baseline, advance)
                continue
            mask = Image.new('L', (width, height), 0)
            ImageDraw.Draw(mask).text((-left, -top), ch, font=ft, anchor='ls', fill=255)
            gw, gh = max(1, round(width/4*xscale)), max(1, round(height/4))
            mask = mask.resize((gw, gh), Image.Resampling.LANCZOS)
            glyph = Image.new('RGBA', (gw, gh), (255,255,255,0)); glyph.putalpha(mask)
            dx, dy = round(left/4*xscale), baseline+round(top/4)
            check(gw <= 255 and gh <= 255 and -128 <= dx <= 127 and -128 <= dy <= 127, 'Glyph metrics overflow')
            rendered[ch] = (glyph, gw, gh, dx, dy, advance)
        placements = {}; x, y, row_height = 1, oh+2, 0
        for ch, (im, gw, gh, dx, dy, adv) in rendered.items():
            check(gw+2 <= ow, f'{self.name}: glyph exceeds atlas width')
            if x+gw+1 > ow: x=1; y += row_height+2; row_height=0
            placements[ch] = (x,y); x += max(gw,1)+2; row_height=max(row_height,gh)
        nh = 1 << (y+row_height+2-1).bit_length()
        check(nh <= 4096, 'Atlas height too large')
        image = Image.new('RGBA', (ow,nh), (255,255,255,0)); image.paste(original,(0,0))
        for ch, (im, *_rest) in rendered.items(): image.paste(im, placements[ch])
        for code, ch in wanted.items():
            _im, gw, gh, dx, dy, advance = rendered[ch]; gx,gy=placements[ch]
            recs[code]=GLYPH.pack(code,gw,gh,gx,gy,0,dx,dy,0,0,advance)
        records=b''.join(recs[k] for k in sorted(recs))
        kern=128+len(records); delta=kern-self.old_kern_offset
        header=bytearray(self.header); tail=bytearray(self.tail)
        struct.pack_into('<H',header,10,len(recs))
        struct.pack_into('<II',header,24,kern,kern+len(tail))
        struct.pack_into('<I',header,4,u32(header,4)+delta+ow*(nh-oh))
        struct.pack_into('<ff',header,64,1/ow,1/nh)
        # Version 0x019e's surface descriptor pointer is an offset. In 0x016e
        # these bytes are float sampling values, so they must NOT be relocated.
        version=struct.unpack_from('<H',header,8)[0]
        if version==0x019e:
            old_ptr=u32(header,48)
            check(self.old_kern_offset <= old_ptr <= len(self.body)-512-32, 'Bad surface descriptor')
            struct.pack_into('<I',header,48,old_ptr+delta)
            pos=old_ptr-self.old_kern_offset
            struct.pack_into('<I',tail,pos+4,u32(tail,pos+4)+ow*(nh-oh))
            struct.pack_into('<I',tail,pos+12,ow*nh)
            struct.pack_into('<II',tail,pos+24,ow,nh)
        else: check(version==0x016e, 'Unsupported FNTF version')
        result=self.body[:512]+header+records+tail
        parsed=GameFont.read(result)
        # Preserve the original ASCII glyph positions, metrics and kerning refs.
        for c,r in self.records.items():
            if 32<=c<127: check(parsed.records[c]==r, f'{self.name}: altered original ASCII glyph')
        for c,r in parsed.records.items():
            _c,gw,gh,gx,gy,*_=GLYPH.unpack(r)
            check(gx+gw<=ow and gy+gh<=nh, f'{self.name}: glyph outside atlas')
        for ch in GREEK:
            check(ord(ch) in parsed.records and GREEK_BYTES[ch] in parsed.records, 'Missing Greek glyph')
            r=GLYPH.unpack(parsed.records[GREEK_BYTES[ch]])
            check(image.getchannel('A').crop((r[3],r[4],r[3]+r[1],r[4]+r[2])).getbbox() is not None, 'Empty Greek glyph')
        check(original.tobytes()==image.crop((0,0,ow,oh)).tobytes(), 'Original atlas pixels changed')
        tex.replace_image(image, oh)
        return result, {'name':self.name,'original_glyphs':len(self.records),'output_glyphs':len(recs),
                        'original_size':[ow,oh],'output_size':[ow,nh], 'original_ascii_unchanged':True,
                        'greek_coverage':len(GREEK),'font_file_used':path.name}

def encode(s: str) -> bytes:
    s=unicodedata.normalize('NFC',s); out=bytearray()
    for ch in s:
        n=ord(ch)
        if ch in CODEC: out.append(CODEC[ch])
        elif 32<=n<127 or n in PRESERVE or n in (9,10,13): out.append(n)
        else: raise ValueError(f'Unsupported character U+{n:04X} {ch!r} in {s[:100]!r}')
    return bytes(out)

def decode(b: bytes) -> str:
    return ''.join(REVERSE.get(n,chr(n)) for n in b)

def label_hash(s: str) -> int:
    h=0xffffffff
    for c in s.encode('ascii'): h=(h*33+c)&0xffffffff
    return h

def read_text_chunk(body: bytes):
    conv,n,tab,sd=struct.unpack_from('<4I',body)
    check(conv==16 and tab+8*n==sd and sd<len(body), 'Unexpected language chunk')
    out={}
    for i in range(n):
        h,off=struct.unpack_from('<II',body,tab+8*i)
        check(sd+off<len(body), 'Text offset outside language chunk')
        out[h]=body[sd+off:body.index(b'\0',sd+off)]
    return conv,tab,out

def translate_chunk(body: bytes, texts: dict[str,str]):
    _conv,tab,old=read_text_chunk(body); mapping={}
    for k,v in texts.items():
        h=int(k,16) if k.startswith('0x') else label_hash(k)
        check(h not in mapping,'Duplicate translation hash')
        mapping[h]=encode(v)
        check(decode(mapping[h])==unicodedata.normalize('NFC',v), 'Encoding round trip failed')
    combined={**old,**mapping}; strings=bytearray(); offsets={}; entries=bytearray()
    for h,v in sorted(combined.items()):
        check(b'\0' not in v,'Embedded NUL in translation')
        if v not in offsets: offsets[v]=len(strings); strings+=v+b'\0'
        entries+=struct.pack('<II',h,offsets[v])
    strings+=bytes(-len(strings)%4)
    conversion=bytearray(body[16:tab])
    # Keep the original punctuation table. Add em dash in an unused byte slot.
    check(u32(conversion,0)==256,'Unexpected character conversion table')
    for ch,slot in GREEK_BYTES.items():
        check(struct.unpack_from('<H',conversion,4+2*slot)[0]==0, 'Greek slot collides with conversion entry')
    struct.pack_into('<H',conversion,4+2*0x8d,0x2014)
    result=struct.pack('<4I',16,len(combined),tab,tab+len(entries))+conversion+entries+strings
    _c,_t,verify=read_text_chunk(result)
    check(verify==combined,'Text pack read-back mismatch')
    unmatched={f'0x{h:08X}':old[h].hex() for h in old if h not in mapping}
    return result,{'source_text_records':len(old),'translated_catalogue_records':len(mapping),
                   'output_text_records':len(verify),'unmatched_preserved_as_hex':unmatched}

def read_loc(b: bytes) -> list[str]:
    q=b.find(b'LOCL'); check(q>=0,'Missing LOCL chunk')
    check(q+u32(b,q+4)==len(b),'Unexpected LOCL size')
    n=u32(b,q+12); check(n<=1000,'Invalid locale count'); out=[]
    for i in range(n):
        p=q+u32(b,q+16+4*i); e=p
        check(p>=q+16+4*n and p<len(b),'Invalid locale offset')
        while e+2<=len(b) and b[e:e+2]!=b'\0\0':e+=2
        check(e+2<=len(b),'Unterminated locale string')
        out.append(b[p:e].decode('utf-16-le'))
    return out

def write_loc(template: bytes, texts: list[str]) -> bytes:
    q=template.find(b'LOCL'); original=read_loc(template); n=len(texts)
    check(len(original)==n,'Save locale count mismatch')
    body=bytearray(); offsets=[]
    for s in texts:
        offsets.append(16+4*n+len(body)); body+=encode(s).decode('latin1').encode('utf-16-le')+bytes(4)
    body+=bytes(-len(body)%4)
    result=template[:q]+b'LOCL'+struct.pack(f'<III{n}I',16+4*n+len(body),0,n,*offsets)+body
    check([decode(s.encode('latin1')) for s in read_loc(result)]==texts,'Locale read-back mismatch')
    return result

def read_assets(path: Path) -> dict[str,bytes]:
    required={'english.bin','languagetextures.bin','globala.bun','locale_english.loc'}
    wanted=required|{'locale_greek.loc'}; found={}
    def accept(name: str,b: bytes):
        key=Path(name.replace('\\','/')).name.lower()
        if key not in wanted:return
        check(key not in found or found[key]==b,f'Ambiguous copies of {key}; use the small input ZIP')
        check(len(b)<=MAX_ASSET,f'{name}: input too large'); found[key]=b
    if path.is_file() and zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            for item in z.infolist():
                key=Path(item.filename).name.lower()
                if key in wanted:
                    check(item.file_size<=MAX_ASSET,'Archive entry too large')
                    accept(item.filename,z.read(item))
    elif path.is_dir():
        for f in path.rglob('*'):
            if any(p.upper()=='LANGPACKS' for p in f.relative_to(path).parts):continue
            if f.is_file() and f.name.lower() in wanted:
                check(f.stat().st_size<=MAX_ASSET,'Input file too large'); accept(f.name,f.read_bytes())
    else:raise ValueError('Input must be Languages.zip or an asset directory')
    check(required<=found.keys(),'Missing input files: '+', '.join(sorted(required-found.keys())))
    return found

def assemble(original: bytes, replacements: dict[int,bytes]) -> bytes:
    out=bytearray()
    for cid,s,e in chunks(original):
        if cid==0:continue
        out+=padding(len(out))
        out+=replacements.get(s-8,chunk(cid,original[s:e]))
    return bytes(out)

def verify_font_file(path: Path) -> None:
    from fontTools.ttLib import TTFont
    check(path.is_file(),f'Font does not exist: {path}')
    with TTFont(str(path),fontNumber=0,lazy=True) as f:
        cmap=f.getBestCmap() or {}
        missing=[f'U+{ord(c):04X}' for c in GREEK if ord(c) not in cmap]
    check(not missing,'Selected font lacks Greek characters: '+', '.join(missing))

def build(source: Path, text_path: Path, faces: dict[str, Path], output: Path,
          locale_source: str='existing') -> dict:
    from PIL import __version__ as pillow_version
    for f in faces.values(): verify_font_file(f)
    inputs=read_assets(source)
    catalogue=json.loads(text_path.read_text(encoding='utf-8-sig'))
    if 'game' in catalogue: catalogue=catalogue['game']
    check(isinstance(catalogue.get('texts'),dict) and isinstance(catalogue.get('memcard'),list),'Invalid Greek JSON')
    output=output.resolve()
    target=output/'LANGPACKS'/'greek'
    check(not target.exists(),'Output LANGPACKS/greek already exists. Choose another output folder; originals are never overwritten.')
    font_chunks=[]; replacements={'english.bin':{},'globala.bun':{},'languagetextures.bin':{}}
    packs=[]
    for key in ['english.bin','globala.bun','languagetextures.bin']:
        b=inputs[key]
        for cid,s,e in chunks(b):
            if cid==0x30201:font_chunks.append((key,s-8,GameFont.read(b[s:e])))
            if cid==0xb3300000:packs.append((key,s-8,TPK.read(b,s,e)))
    check({f.name for _k,_p,f in font_chunks}==FONTS,'Different font set: this builder targets the supplied PC layout')
    textures={t.name.lower():t for _k,_p,pack in packs for t in pack.textures}
    report={'status':'built',
            'pillow':pillow_version,'input_sha256':{k:hashlib.sha256(v).hexdigest() for k,v in inputs.items()},
            'greek_characters':len(GREEK),'fonts':[]}
    for key,pos,f in font_chunks:
        check(f.name in textures,f'Missing texture for {f.name}')
        if f.name in KEEP_FONTS: continue
        print('Building',f.name,flush=True)
        body,info=f.extend(textures[f.name],faces)
        replacements[key][pos]=chunk(0x30201,body);report['fonts'].append(info)
    english=inputs['english.bin']
    text_parts=[(s,e) for cid,s,e in chunks(english) if cid==0x39000]
    check(len(text_parts)==1,'Expected one language text chunk')
    s,e=text_parts[0]; body,stats=translate_chunk(english[s:e],catalogue['texts'])
    replacements['english.bin'][s-8]=chunk(0x39000,body);report.update(stats)
    # These inputs have the TPK first, so rewritten pack offsets start at zero.
    for key,pos,pack in packs:
        check(pos==0,'TPK is not the first chunk; unsupported input variant')
        rebuilt=pack.write_compressed() if pack.blobs else pack.write(0)
        check(next(chunks(rebuilt))[0]==0xb3300000,'Repacked texture root invalid')
        reread=TPK.read(rebuilt,8,len(rebuilt))
        for before,after in zip(pack.textures,reread.textures):
            check(before.pixels==after.pixels and before.palette==after.palette,'Texture reserialization changed pixels')
            check(before.name==after.name and before.size==after.size,'Texture reserialization changed metadata')
        replacements[key][pos]=rebuilt
    locale_key='locale_greek.loc' if locale_source=='existing' and 'locale_greek.loc' in inputs else None
    save_texts=read_loc(inputs[locale_key]) if locale_key else catalogue['memcard']
    report['save_text_source']=locale_key or 'Greek translation JSON'
    built={'LANGUAGES/English.bin':assemble(inputs['english.bin'],replacements['english.bin']),
           'LANGUAGES/LanguageTextures.bin':assemble(inputs['languagetextures.bin'],replacements['languagetextures.bin']),
           'GLOBAL/GLOBALA.BUN':assemble(inputs['globala.bun'],replacements['globala.bun']),
           'memcard/LOCALE_ENGLISH.loc':write_loc(inputs['locale_english.loc'],save_texts)}
    report['generated_files']={k:{'bytes':len(v),'sha256':hashlib.sha256(v).hexdigest()} for k,v in built.items()}
    output.mkdir(parents=True,exist_ok=True)
    temp=Path(tempfile.mkdtemp(prefix='.greek-build-',dir=output))
    try:
        staged=temp/'greek'; staged.mkdir()
        for rel,data in built.items():
            p=staged/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
        target.parent.mkdir(parents=True,exist_ok=True);staged.rename(target)
    finally:shutil.rmtree(temp,ignore_errors=True)
    (output/'greek-build-report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    (output/'greek-character-map.json').write_text(json.dumps(CODEC,ensure_ascii=False,indent=2),encoding='utf-8')
    print(f'\nCreated: {target}')
    return report

def default_font() -> Path | None:
    p=Path(os.environ.get('WINDIR','C:/Windows'))/'Fonts'/'arial.ttf'
    return p if p.is_file() else None

def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',type=Path,help='Languages.zip or the folder containing your source assets')
    parser.add_argument('--text',type=Path,default=Path(__file__).resolve().parents[1]/'lang'/'greek.json')
    parser.add_argument('--font',type=Path,default=default_font(),help='Your Greek-capable TTF/OTF (Windows Arial by default)')
    parser.add_argument('--bold-font',type=Path,help='Optional Greek-capable bold face')
    parser.add_argument('--italic-font',type=Path,help='Optional Greek-capable italic face (the italic Conduit fonts)')
    parser.add_argument('--bold-italic-font',type=Path,help='Optional Greek-capable bold italic face')
    parser.add_argument('--output',type=Path,default=Path.cwd()/'Greek_Build')
    parser.add_argument('--locale-source',choices=['existing','translation'],default='existing')
    parser.add_argument('--gui',action='store_true',help='Choose input/output using Windows file dialogs')
    args=parser.parse_args()
    if args.gui:
        import tkinter as tk
        from tkinter import filedialog
        root=tk.Tk();root.withdraw()
        src=filedialog.askopenfilename(title='Select your Languages.zip',filetypes=[('ZIP archive','*.zip')])
        if not src:return 1
        args.input=Path(src)
        if not args.font:
            face=filedialog.askopenfilename(title='Select a Greek-capable font you have permission to use',filetypes=[('Font','*.ttf *.otf')])
            if not face:return 1
            args.font=Path(face)
        out=filedialog.askdirectory(title='Choose an output folder (a new LANGPACKS/greek will be created)')
        if not out:return 1
        args.output=Path(out)
        root.destroy()
    if not args.input or not args.font:parser.error('--input and --font are required (or use --gui on Windows)')
    if not args.bold_font and args.font.name.lower()=='arial.ttf':
        candidate=args.font.with_name('arialbd.ttf')
        if candidate.is_file():args.bold_font=candidate
    try:
        faces={k:v for k,v in (('regular',args.font),('bold',args.bold_font),('italic',args.italic_font),
                                ('bold_italic',args.bold_italic_font)) if v}
        build(args.input,args.text,faces,args.output,args.locale_source)
    except (OSError,ValueError,struct.error,KeyError,zipfile.BadZipFile) as e:
        print(f'ERROR: {e}',file=sys.stderr);return 1
    return 0

if __name__=='__main__':raise SystemExit(main())
