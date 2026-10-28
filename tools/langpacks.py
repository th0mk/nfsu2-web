#!/usr/bin/env python3
"""The language packs: LANGPACKS/<language> in the game folder, from the
"Multi 13 languages" mod for NFSU2 (KyoriAsh, nfsmods.xyz/mod/5749), from
TraduMix's and MixMods' Brazilian Portuguese translation (and ZANIT's dubbing),
from PLProjekt's Polish one, from VileStuff's English subtitles and from
muntazam's Turkish translation.

    tools/langpacks.py <game folder> [--multi13 <mod folder>] [--ptbr <translation folder>]
                       [--ptbr-dub <dub folder>] [--pl <installer>] [--subs <subtitles folder>]
                       [--tr <translation folder>]

Each source builds its own languages' packs; the others are left as they are.

<mod folder> holds every archive of the mod extracted into it, Core and the
dubs (nfsug2Multi13Core.rar, nfsug2Multi13{Fr,De,It,Es,Ja,Ru}Dub.rar; the
English dub is the game's own).  The dubs need RAR 7's unrar (`unrar x`):
libarchive (bsdtar, Windows' tar) cannot read their dictionary size.

The mod's own installer copies its files over the game's and sets the
registry; the port reads them from a folder per language instead
(runtime/language.c, fileio.c's language pack), so every player picks one
for themselves and the game folder stays as it was.  A pack holds only what
differs from the game folder: a file there is read in place of the game's file
of the same path.

  - the dubbed speech banks, from each dub's sdat*.viv: speech/UG2_Speech_FR,
    _GR, _IT, _SP and _JP are the game's own names for French, German, Italian,
    Spanish and Japanese; Russian's stands in for the English bank.  The rest of
    the archive is the game's (the cutscene sound, nisaudio) or its music,
    which stays the game's.
  - the dubbed movies (Lang/Movies/<Xx>), and the Danish, Dutch and Swedish EA
    logos
  - the full Chinese, Japanese and Thai texts (the game folder's are short
    early versions) with the fonts drawn for them (LanguageTexturesGl), and
    Japanese's in-game textures (GlobalB_ja)
  - Russian, which takes English's place: its text, fonts and credits

<translation folder> is the Brazilian Portuguese translation's archive
extracted ("Tradução PT-BR, V1.1 (TraduMix e MixMods)", joao13traducoes.com;
its password is "João13"), or any folder below which it has
LANGUAGES/English.bin.  Optionally includes the Brazilian Portuguese dubbing
by ZANIT (speech/UG2_Speech.big, MOVIES/*.vp6), or pass it via --ptbr-dub.
Portuguese takes English's place as Russian does: its text (accented in the
game's own fonts) and credits, and the game folder's own Brazilian Portuguese
memory-card texts ("Carregado." for "Load complete."), along with dubbed
voices and movies.  The rest of the archive is the Extra Options mod, for its
ShowSubs: the port shows the subtitles itself (runtime/language.c).

<installer> is either installer of the Polish translation (v1.1, 2009,
NFSU2PLv1.1US.exe or NFSU2PLv1.1UK.exe, from its archive at portal24h.pl), or
the folder they are in.  It is read, never run: an Astrum InstallWizard
package, whose files are gzip streams.  Polish takes English's place as Russian
does: its text spells the Polish letters with accented ones the game's fonts
have (ę is ¿, ł is î, ś is â ...), and its fonts' textures
(LanguageTextures.bin, and one in GLOBALA.BUN) draw those as the Polish ones;
also its memory-card texts and credits.  The rest of the
installer is a Polish SPEED2.EXE, the game folder's own GLOBALB.BUN, the
British game's Corsa and 106 (this game folder has them) and a script that
hides the movies.

<subtitles folder> is VileStuff's "English Subtitles Fixed" mod (v1.0,
nfsmods.xyz/mod/5286) extracted.  Its Binary endscript (script/main.end)
gives 105 of English's texts: the movies' lines (the PSA, the tutorials, the
story) whole and with who speaks them, where the game folder's stop at their
first apostrophe ("But if you").  English's pack holds the game's English.bin
with those texts, and with it the port shows the movies' subtitles in English
too (runtime/language.c).  The mod is installed with Binary and needs the
Extra Options mod's ShowSubs; neither is used.

<translation folder> (--tr) is muntazam's Turkish translation (v1.1, 2024,
"Need for Speed Underground 2 Turkish Translation Patch" at moddb.com)
extracted: its archive holds an Inno Setup installer, nfsu2tr11.exe, which
`innoextract nfsu2tr11.exe` unpacks (into app/) without running it; or any
folder below which it has LANGUAGES/English.bin.  Turkish takes English's
place as Polish does: its text spells the Turkish letters with accented ones
the game's fonts have (ş is ÿ, ı is í, ğ is ý, İ is Í, Ş is þ, Ğ is Ý), and
its fonts' textures (LanguageTextures.bin, and one in GLOBALA.BUN) draw those
as the Turkish ones; also its memory-card texts and credits.  The port shows
the movies' subtitles itself (runtime/language.c), so the Extra Options mod
the translation asks for is not needed.
Rerunning it copies only what changed.  Prints what each pack holds.
"""
import argparse, filecmp, gzip, os, re, shutil, struct, sys

LANGS = ['french', 'german', 'italian', 'spanish', 'dutch', 'swedish', 'danish',
         'korean', 'chinese', 'japanese', 'russian', 'thai']
# the dubs: Lang/Movies/<dir> and Lang/Sdata/sdat<dir lower-cased>.viv
DUBS = {'french': 'Fr', 'german': 'De', 'italian': 'It', 'spanish': 'Es', 'japanese': 'Ja', 'russian': 'Ru'}
# (language, file in the mod folder, the game's file it stands in for)
FILES = [
    ('chinese', 'Languages/Chinese.bin', 'LANGUAGES/Chinese.bin'),
    ('chinese', 'Lang/Languages/LanguageTexturesGl.bin', 'LANGUAGES/LanguageTextures.bin'),
    ('chinese', 'Credits/CHINESE.TXT', 'CREDITS/CHINESE.TXT'),
    ('chinese', 'Memcard/LOCALE_CHINESE.loc', 'memcard/LOCALE_CHINESE.loc'),
    ('japanese', 'Languages/Japanese.bin', 'LANGUAGES/Japanese.bin'),
    ('japanese', 'Lang/Languages/LanguageTexturesGl.bin', 'LANGUAGES/LanguageTextures.bin'),
    ('japanese', 'Lang/Global/GlobalB_ja.lzc', 'GLOBAL/GlobalB.lzc'),
    ('japanese', 'Credits/JAPANESE.TXT', 'CREDITS/JAPANESE.TXT'),
    ('japanese', 'Memcard/LOCALE_JAPANESE.loc', 'memcard/LOCALE_JAPANESE.loc'),
    ('thai', 'Languages/Thailand.bin', 'LANGUAGES/Thailand.bin'),
    ('thai', 'Lang/Languages/LanguageTexturesGl.bin', 'LANGUAGES/LanguageTextures.bin'),
    ('thai', 'Credits/THAILAND.TXT', 'CREDITS/THAILAND.TXT'),
    ('thai', 'Memcard/LOCALE_THAILAND.loc', 'memcard/LOCALE_THAILAND.loc'),
    ('russian', 'Lang/Languages/English_ru.bin', 'LANGUAGES/English.bin'),
    ('russian', 'Lang/Languages/LanguageTexturesRu.bin', 'LANGUAGES/LanguageTextures.bin'),
    ('russian', 'Lang/Global/GLOBALA_ru.BUN', 'GLOBAL/GLOBALA.BUN'),
    ('russian', 'Lang/Credits/UK_ENGLISH_ru.txt', 'CREDITS/NA_ENGLISH.TXT'),
    ('russian', 'Lang/Credits/UK_ENGLISH_ru.txt', 'CREDITS/UK_ENGLISH.TXT'),
    ('russian', 'Lang/Memcard/LOCALE_ENGLISH_Ru.loc', 'memcard/LOCALE_ENGLISH.loc'),
    ('danish', 'Movies/ealogo_dn.vp6', 'MOVIES/ealogo.vp6'),
    ('dutch', 'Movies/ealogo_du.vp6', 'MOVIES/ealogo.vp6'),
    ('swedish', 'Movies/ealogo_sw.vp6', 'MOVIES/ealogo.vp6'),
]
# Brazilian Portuguese: (file in the translation, the game's file it stands in for)
PTBR = [
    ('LANGUAGES/English.bin', 'LANGUAGES/English.bin'),
    ('CREDITS/NA_ENGLISH.TXT', 'CREDITS/NA_ENGLISH.TXT'),
    ('CREDITS/NA_ENGLISH.TXT', 'CREDITS/UK_ENGLISH.TXT'),
]
# Polish: (file in the installer, the game's file it stands in for)
PL = [
    ('LANGUAGES/Polish.bin', 'LANGUAGES/English.bin'),
    ('LANGUAGES/LanguageTextures.bin', 'LANGUAGES/LanguageTextures.bin'),
    ('GLOBAL/GLOBALA.BUN', 'GLOBAL/GLOBALA.BUN'),
    ('MEMCARD/LOCALE_POLISH.loc', 'memcard/LOCALE_ENGLISH.loc'),
    ('CREDITS/POLISH.TXT', 'CREDITS/NA_ENGLISH.TXT'),
    ('CREDITS/POLISH.TXT', 'CREDITS/UK_ENGLISH.TXT'),
]
# Turkish: (file in the translation, the game's file it stands in for)
TR = [
    ('LANGUAGES/English.bin', 'LANGUAGES/English.bin'),
    ('LANGUAGES/LanguageTextures.bin', 'LANGUAGES/LanguageTextures.bin'),
    ('GLOBAL/GLOBALA.BUN', 'GLOBAL/GLOBALA.BUN'),
    ('memcard/LOCALE_ENGLISH.loc', 'memcard/LOCALE_ENGLISH.loc'),
    ('CREDITS/NA_ENGLISH.TXT', 'CREDITS/NA_ENGLISH.TXT'),
    ('CREDITS/NA_ENGLISH.TXT', 'CREDITS/UK_ENGLISH.TXT'),
]


def find(root, rel):
    """rel under root, each part matched case-insensitively; None if missing"""
    cur = root
    for part in rel.split('/'):
        if not os.path.isdir(cur): return None
        hit = next((n for n in os.listdir(cur) if n.lower() == part.lower()), None)
        if hit is None: return None
        cur = os.path.join(cur, hit)
    return cur


def game_name(game, rel):
    """rel spelled as the game folder spells it, as far as it has it"""
    out, cur = [], game
    for part in rel.split('/'):
        hit = next((n for n in os.listdir(cur) if n.lower() == part.lower()), None) if os.path.isdir(cur) else None
        out.append(hit or part); cur = os.path.join(cur, hit or part)
    return '/'.join(out)


def viv_entries(path):
    """(name, offset, size) of a BIG4/BIGF archive (EA's .viv)"""
    with open(path, 'rb') as f:
        head = f.read(16)
        if head[:3] != b'BIG': sys.exit(f'{path}: not a .viv archive')
        n, hdr = struct.unpack('>II', head[8:16]); d = f.read(hdr - 16)
    out, q = [], 0
    for _ in range(n):
        off, size = struct.unpack('>II', d[q:q + 8]); q += 8
        e = d.index(b'\0', q); out.append((d[q:e].decode('latin1'), off, size)); q = e + 1
    return out


class Pack:
    def __init__(self, game, lang):
        self.game, self.root, self.files, self.bytes = game, os.path.join(game, 'LANGPACKS', lang), 0, 0
        self.made = set()

    def target(self, rel):
        return os.path.join(self.root, game_name(self.game, rel))

    def same_as_game(self, rel, data=None, src=None):
        g = find(self.game, rel)
        if not g or os.path.getsize(g) != (len(data) if data is not None else os.path.getsize(src)): return False
        if src: return filecmp.cmp(src, g, shallow=False)
        with open(g, 'rb') as f: return f.read() == data

    def add_file(self, src, rel):
        if self.same_as_game(rel, src=src): return
        dst = self.target(rel)
        if not (os.path.exists(dst) and filecmp.cmp(src, dst, shallow=False)):
            os.makedirs(os.path.dirname(dst), exist_ok=True); shutil.copyfile(src, dst)
        self.files += 1; self.bytes += os.path.getsize(dst); self.made.add(dst)

    def add_data(self, data, rel):
        if self.same_as_game(rel, data=data): return
        dst = self.target(rel)
        if not (os.path.exists(dst) and os.path.getsize(dst) == len(data) and open(dst, 'rb').read() == data):
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as f: f.write(data)
        self.files += 1; self.bytes += len(data); self.made.add(dst)

    def drop_stale(self):
        """what an earlier run put here and this one did not"""
        for d, dirs, names in os.walk(self.root, topdown=False):
            for n in names:
                if os.path.join(d, n) not in self.made: os.remove(os.path.join(d, n))
            if not os.listdir(d): os.rmdir(d)


def done(p, lang):
    p.drop_stale()
    print(f'{lang:10} {p.files:3} files {p.bytes / 1048576:8.1f} MB  {p.root if p.files else "(none needed)"}')


def multi13(mod, game):
    missing = [s for _, s, _ in FILES if not find(mod, s)]
    missing += [f'Lang/Sdata/sdat{x.lower()}.viv' for x in DUBS.values() if not find(mod, f'Lang/Sdata/sdat{x.lower()}.viv')]
    missing += [f'Lang/Movies/{x}' for x in DUBS.values() if not find(mod, f'Lang/Movies/{x}')]
    if missing: sys.exit(f'{mod}: the mod is not all there (extract every archive into it), missing:\n  ' + '\n  '.join(missing))

    for lang in LANGS:
        p = Pack(game, lang)
        for l, src, rel in FILES:
            if l == lang: p.add_file(find(mod, src), rel)
        if lang in DUBS:
            viv = find(mod, f'Lang/Sdata/sdat{DUBS[lang].lower()}.viv')
            with open(viv, 'rb') as f:
                for name, off, size in viv_entries(viv):
                    if not name.lower().startswith('speech/ug2_speech'): continue
                    f.seek(off); p.add_data(f.read(size), name)
            mdir = find(mod, f'Lang/Movies/{DUBS[lang]}')
            for n in sorted(os.listdir(mdir)): p.add_file(os.path.join(mdir, n), f'MOVIES/{n}')
        done(p, lang)


def ptbr(src, game, dub=None):
    p = Pack(game, 'portuguese')
    if src:
        root = next((d for d, _, _ in os.walk(src) if find(d, 'LANGUAGES/English.bin')), None)
        if not root: sys.exit(f'{src}: no LANGUAGES/English.bin below it, not the translation')
        missing = [s for s, _ in PTBR if not find(root, s)]
        if missing: sys.exit(f'{root}: the translation is not all there, missing:\n  ' + '\n  '.join(missing))
        if p.same_as_game('LANGUAGES/English.bin', src=find(root, 'LANGUAGES/English.bin')):
            sys.exit(f'{root}: its LANGUAGES/English.bin is the game\'s own, not the translation')
        for s, rel in PTBR: p.add_file(find(root, s), rel)
        loc = find(game, 'memcard/LOCALE_BRAZILIAN_PORTUGUESE.loc')
        if loc: p.add_file(loc, 'memcard/LOCALE_ENGLISH.loc')

    dub_dir = dub or (src if (src and (find(src, 'speech') or find(src, 'Dublagem/speech'))) else None)
    if dub_dir:
        sdir = find(dub_dir, 'speech') or find(dub_dir, 'Dublagem/speech')
        if sdir:
            for n in sorted(os.listdir(sdir)):
                if n.lower().startswith('ug2_speech'):
                    p.add_file(os.path.join(sdir, n), f'speech/{n}')
        mdir = find(dub_dir, 'MOVIES') or find(dub_dir, 'Dublagem/MOVIES')
        if mdir:
            for n in sorted(os.listdir(mdir)):
                if n.lower().endswith('.vp6'):
                    p.add_file(os.path.join(mdir, n), f'MOVIES/{n}')
    done(p, 'portuguese')


def astrum_files(path):
    """{path in the game folder, lower-cased: data} of an Astrum InstallWizard 2
    installer.  It ends with the package's header offset and the magic; the
    header (big-endian words) gives where the files start, and they end where
    it begins.  Each file: a 0x40-byte record whose word 0x2c is its name's
    length, the name ("<InstallDir>\\...", each byte's bit i stored as bit i^6),
    the offset its data ends at, the data: a gzip stream, or the file as it is
    (a few bytes)."""
    with open(path, 'rb') as f: d = f.read()
    if d[-8:] != struct.pack('<II', 0x0b1c2d3e, 0x12345678): sys.exit(f'{path}: not an Astrum InstallWizard installer')
    n = struct.unpack('<I', d[-12:-8])[0]
    head = struct.unpack('<I', d[-16 - n:-12 - n])[0]
    hdr = struct.unpack('>59I', d[head:head + 236])
    pos = hdr[48]
    if pos + hdr[49] != head: sys.exit(f'{path}: an Astrum package of another layout')
    out = {}
    while pos < head:
        size = struct.unpack('<I', d[pos + 0x2c:pos + 0x30])[0]
        name = bytes(sum((b >> i & 1) << (i ^ 6) for i in range(8)) for b in d[pos + 0x40:pos + 0x40 + size])
        end = struct.unpack('<I', d[pos + 0x40 + size:pos + 0x44 + size])[0]
        rel = name.decode('latin1').split('\\', 1)[1].replace('\\', '/')
        data = d[pos + 0x44 + size:end]
        out[rel.lower()] = gzip.decompress(data) if data[:3] == b'\x1f\x8b\x08' else data
        pos = end
    return out


def pl(src, game):
    exe = src if os.path.isfile(src) else next(
        (os.path.join(src, n) for n in sorted(os.listdir(src), reverse=True) if n.lower().startswith('nfsu2pl') and n.lower().endswith('.exe')), None)
    if not exe: sys.exit(f'{src}: no NFSU2PLv1.1US.exe or NFSU2PLv1.1UK.exe, not the translation')
    files = astrum_files(exe)
    missing = [s for s, _ in PL if s.lower() not in files]
    if missing: sys.exit(f'{exe}: not the translation, missing:\n  ' + '\n  '.join(missing))
    p = Pack(game, 'polish')
    for s, rel in PL: p.add_data(files[s.lower()], rel)
    done(p, 'polish')


def tr(src, game):
    root = next((d for d, _, _ in os.walk(src) if find(d, 'LANGUAGES/English.bin')), None)
    if not root: sys.exit(f'{src}: no LANGUAGES/English.bin below it, not the translation (innoextract its nfsu2tr11.exe)')
    missing = [s for s, _ in TR if not find(root, s)]
    if missing: sys.exit(f'{root}: the translation is not all there, missing:\n  ' + '\n  '.join(missing))
    p = Pack(game, 'turkish')
    if p.same_as_game('LANGUAGES/English.bin', src=find(root, 'LANGUAGES/English.bin')):
        sys.exit(f'{root}: its LANGUAGES/English.bin is the game\'s own, not the translation')
    for s, rel in TR: p.add_file(find(root, s), rel)
    done(p, 'turkish')


STRINGS = 0x39000           # a language file's text: its chunk's id


def end_texts(path):
    """{hash: text} of a Binary endscript's "update_string LANGUAGES\\ENGLISH.BIN STRBlocks <block> <hash> Text "..."
    lines (the last one for a hash wins).  Its Label lines name the texts for Binary only; a Key line may not move a
    text to another hash."""
    out = {}
    with open(path, encoding='utf-8-sig') as f:
        for n, line in enumerate(f, 1):
            m = re.match(r'update_string\s+(\S+)\s+STRBlocks\s+\S+\s+(\S+)\s+(Text|Label|Key)\s+(.*?)\s*$', line)
            if not m or m[1].upper() != 'LANGUAGES\\ENGLISH.BIN': continue
            h = int(m[2], 0)
            if m[3] == 'Key' and int(m[4], 0) != h: sys.exit(f'{path}:{n}: moves a text to another hash')
            if m[3] != 'Text': continue
            t = m[4]
            if len(t) < 2 or t[0] != '"' or t[-1] != '"': sys.exit(f'{path}:{n}: a text not in quotes')
            if not all(' ' <= ch <= '~' for ch in t[1:-1]):
                sys.exit(f'{path}:{n}: a text with letters beyond ASCII, which the game spells in its own way')
            out[h] = t[1:-1]
    return out


def with_texts(data, texts):
    """A language file (chunks: id, size, data) with these texts, and how many of them differ from its own.  Its
    text chunk: { 16, count, table, strings } and a character table, then the table (hash, offset into the strings;
    in hash order) and the 0-terminated strings.  A changed text goes after the others; the chunks after the text
    (the fonts) stay 0x80-aligned behind a pad chunk (id 0), as the game's own file has them."""
    q = 0
    while q < len(data) and struct.unpack_from('<I', data, q)[0] != STRINGS: q += 8 + struct.unpack_from('<I', data, q + 4)[0]
    if q >= len(data): sys.exit('English.bin: no text in it')
    size = struct.unpack_from('<I', data, q + 4)[0]
    c = data[q + 8:q + 8 + size]
    head, n, tab, sd = struct.unpack_from('<IIII', c)
    if head != 16 or tab + 8 * n != sd or sd > size: sys.exit('English.bin: a text chunk of another layout')
    at = dict(struct.unpack_from('<II', c, tab + 8 * i) for i in range(n))
    old = lambda h: c[sd + at[h]:c.index(b'\0', sd + at[h])] if h in at else None
    strs, changed = bytearray(c[sd:]), 0
    for h, t in sorted(texts.items()):
        if old(h) == t.encode('ascii'): continue
        at[h] = len(strs); strs += t.encode('ascii') + b'\0'; changed += 1
    strs += bytes(-len(strs) % 4)
    body = (struct.pack('<IIII', 16, len(at), tab, tab + 8 * len(at)) + c[16:tab]
            + b''.join(struct.pack('<II', h, at[h]) for h in sorted(at)) + strs)
    r = q + 8 + size
    while r < len(data) and struct.unpack_from('<I', data, r)[0] == 0: r += 8 + struct.unpack_from('<I', data, r + 4)[0]
    out = bytearray(data[:q]) + struct.pack('<II', STRINGS, len(body)) + body
    if r < len(data):
        if r % 0x80: sys.exit('English.bin: its fonts are not 0x80-aligned, a file of another layout')
        pad = -(len(out) + 8) % 0x80
        out += struct.pack('<II', 0, pad) + bytes(pad)
    return bytes(out + data[r:]), changed


def subs(src, game):
    texts = {}
    for d, _, names in sorted(os.walk(src)):
        for name in sorted(names):
            if name.lower().endswith('.end'): texts.update(end_texts(os.path.join(d, name)))
    if not texts: sys.exit(f'{src}: no endscript below it that changes English\'s text, not the mod')
    eng = find(game, 'LANGUAGES/English.bin')
    if not eng: sys.exit(f'{game}: no LANGUAGES/English.bin')
    with open(eng, 'rb') as f: data, changed = with_texts(f.read(), texts)
    print(f'english: {changed} of the mod\'s {len(texts)} texts differ from the game folder\'s')
    p = Pack(game, 'english')
    p.add_data(data, 'LANGUAGES/English.bin')
    done(p, 'english')


def main():
    ap = argparse.ArgumentParser(usage='tools/langpacks.py <game folder> [--multi13 <mod folder>] [--ptbr <translation folder>] [--ptbr-dub <dub folder>] [--pl <installer>] [--subs <subtitles folder>] [--tr <translation folder>]')
    ap.add_argument('game')
    ap.add_argument('--multi13', metavar='MOD', help='the Multi 13 languages mod, every archive extracted')
    ap.add_argument('--ptbr', metavar='FOLDER', help='the Brazilian Portuguese translation, extracted')
    ap.add_argument('--ptbr-dub', metavar='FOLDER', help='the Brazilian Portuguese dubbing (ZANIT), extracted')
    ap.add_argument('--pl', metavar='INSTALLER', help='the Polish translation\'s installer (or its folder), never run')
    ap.add_argument('--subs', metavar='FOLDER', help='the English Subtitles Fixed mod, extracted')
    ap.add_argument('--tr', metavar='FOLDER', help='the Turkish translation, its installer extracted (innoextract), never run')
    a = ap.parse_args()
    if not (a.multi13 or a.ptbr or a.ptbr_dub or a.pl or a.subs or a.tr): sys.exit(__doc__)
    if not find(a.game, 'SPEED2.EXE'): sys.exit(f'{a.game}: no SPEED2.EXE, not the game folder')
    if a.multi13: multi13(a.multi13, a.game)
    if a.ptbr or a.ptbr_dub: ptbr(a.ptbr, a.game, dub=a.ptbr_dub)
    if a.pl: pl(a.pl, a.game)
    if a.subs: subs(a.subs, a.game)
    if a.tr: tr(a.tr, a.game)


if __name__ == '__main__':
    main()
