#!/usr/bin/env python3
"""Most Wanted's compiled effects: take them out of speed.exe, count their blobs.

  tools/fxres.py <speed.exe> [<outdir>]   write the effects (default out/mw/fx/res)
  tools/fxres.py --census [<dir>]         count shader and preshader blobs in them

speed.exe carries its 31 effects as RT_RCDATA resources named IDI_*_FX
(language 0x409), each a compiled fx_2_0 effect (first dword 0xFEFF0901,
bytes 01 09 FF FE); docs/mw/fx2-format.md is the format.  Extraction writes
<outdir>/<NAME>.fxo for each and <outdir>/index.txt (name, size, SHA-256).
It refuses an exe whose SHA-256 is not games/mw/game.conf's EXE_SHA256 and
only reads the exe.  The output is the game's own data: it goes under out/
(ignored), never into the repo.

The census reads the extracted files as bytes and counts every size-prefixed
blob: a dword N, then N bytes whose first dword is a version token (vs_*,
ps_*, or 0x46580200 for an FXLC preshader) and whose last dword is the end
token 0x0000FFFF.  It knows nothing of the container's tables, so it checks a
parser from outside.  It also prints the raw count of version tokens, which
is about twice as high: each constant table (CTAB) repeats its program's
version.
"""
import hashlib, os, struct, sys
import pefile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
GAME_CONF = os.path.join(ROOT, 'games', 'mw', 'game.conf')
OUT = os.path.join(ROOT, 'out', 'mw', 'fx', 'res')

RT_RCDATA = 10
LANG = 0x409
COUNT = 31                      # IDI_*_FX resources in speed.exe (GAME.md 5.2)
MAGIC = 0xFEFF0901              # fx_2_0: D3DX effect binary version 9.1
END = 0x0000FFFF
FXLC = 0x46580200               # 'FX' 2.0: a preshader's version token


def game_conf(key):
    """One KEY=VALUE from games/mw/game.conf."""
    with open(GAME_CONF) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#') and line.split('=', 1)[0] == key:
                return line.split('=', 1)[1]
    sys.exit("fxres: %s has no %s=" % (GAME_CONF, key))


def effects(exe):
    """[(name, bytes)] of the exe's IDI_*_FX RT_RCDATA resources, language 0x409."""
    pe = pefile.PE(exe, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_RESOURCE']])
    out = []
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if t.id != RT_RCDATA:
            continue
        for e in t.directory.entries:
            name = str(e.name) if e.name is not None else None
            if not name or not name.startswith('IDI_') or not name.endswith('_FX'):
                continue
            langs = [l for l in e.directory.entries if l.id == LANG]
            if len(langs) != 1:
                sys.exit("fxres: %s: %d resources of language 0x%x" % (name, len(langs), LANG))
            d = langs[0].data.struct
            out.append((name, pe.get_data(d.OffsetToData, d.Size)))
    return out


def extract(exe, outdir):
    want = game_conf('EXE_SHA256').lower()
    raw = open(exe, 'rb').read()
    got = hashlib.sha256(raw).hexdigest()
    if got != want:
        sys.exit("fxres: %s is not Most Wanted's speed.exe (SHA-256 %s; game.conf wants %s)" % (exe, got, want))
    fx = effects(exe)
    if len(fx) != COUNT:
        sys.exit("fxres: %d IDI_*_FX resources, expected %d" % (len(fx), COUNT))
    for name, data in fx:
        if len(data) < 8 or struct.unpack_from('<I', data)[0] != MAGIC:
            sys.exit("fxres: %s does not start with 01 09 FF FE (fx_2_0)" % name)
    os.makedirs(outdir, exist_ok=True)
    index = []
    for name, data in sorted(fx):
        with open(os.path.join(outdir, name + '.fxo'), 'wb') as f:
            f.write(data)
        index.append('%s\t%d\t%s\n' % (name, len(data), hashlib.sha256(data).hexdigest()))
    with open(os.path.join(outdir, 'index.txt'), 'w') as f:
        f.write('# name\tsize\tsha256 (tools/fxres.py from %s)\n' % os.path.basename(exe))
        f.writelines(index)
    print("fxres: %d effects, %d bytes, into %s" % (len(fx), sum(len(d) for _, d in fx), outdir))


def version_name(v):
    """The version token as fxc names it (ps_2_x for 0xFFFF0201), or None."""
    kind = {0xFFFE: 'vs', 0xFFFF: 'ps'}.get(v >> 16)
    if v == FXLC:
        return 'fxlc'
    if not kind:
        return None
    major, minor = (v >> 8) & 0xFF, v & 0xFF
    if major == 2 and minor == 1:
        return kind + '_2_x'
    if major > 3 or minor > 4:
        return None
    return '%s_%d_%d' % (kind, major, minor)


def blobs(data):
    """{version name: count} of the size-prefixed blobs in one effect file."""
    n = len(data) // 4
    w = struct.unpack_from('<%dI' % n, data)
    found = {}
    for i in range(n - 2):
        size = w[i]
        if size < 8 or size & 3 or i + 1 + size // 4 > n:
            continue
        name = version_name(w[i + 1])
        if name and w[i + size // 4] == END:
            found[name] = found.get(name, 0) + 1
    return found


def tokens(data):
    """{version name: count} of every dword that is a version token."""
    n = len(data) // 4
    found = {}
    for v in struct.unpack_from('<%dI' % n, data):
        name = version_name(v)
        if name:
            found[name] = found.get(name, 0) + 1
    return found


ORDER = ['vs_1_1', 'vs_2_0', 'vs_2_x', 'vs_3_0', 'ps_1_1', 'ps_1_2', 'ps_1_3', 'ps_1_4', 'ps_2_0', 'ps_2_x',
         'ps_3_0', 'fxlc']


def ranked(names):
    """Version names in GAME.md 5.3's column order, any others after them."""
    return sorted(names, key=lambda k: (ORDER.index(k), '') if k in ORDER else (len(ORDER), k))


def census(d):
    names = sorted(f for f in os.listdir(d) if f.endswith('.fxo'))
    if not names:
        sys.exit("fxres: no .fxo files in %s" % d)
    total, raw, size = {}, {}, 0
    for f in names:
        data = open(os.path.join(d, f), 'rb').read()
        size += len(data)
        b = blobs(data)
        for k, v in b.items():
            total[k] = total.get(k, 0) + v
        for k, v in tokens(data).items():
            raw[k] = raw.get(k, 0) + v
        print('  %-32s %7d  %s' % (f[:-4], len(data), ' '.join('%s %d' % (k, b[k]) for k in ranked(b))))
    keys = ranked(set(total) | set(raw))
    print("census: %d files, %d bytes" % (len(names), size))
    print("blobs:  " + ', '.join('%s %d' % (k, total.get(k, 0)) for k in keys))
    print("tokens: " + ', '.join('%s %d' % (k, raw.get(k, 0)) for k in keys))


def main():
    a = sys.argv[1:]
    if a and a[0] == '--census':
        census(a[1] if len(a) > 1 else OUT)
    elif len(a) in (1, 2) and not a[0].startswith('-'):
        extract(a[0], a[1] if len(a) > 1 else OUT)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
