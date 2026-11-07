#!/usr/bin/env python3
"""Crash damage's scratches in the game folder: MODPACKS/DAMAGE/SCRATCHES.TPK, a mod pack's file (runtime/fileio.c).

    tools/damagepack.py <game folder or out folder>

It is Crashing Foreign Cars' own texture pack of DAMAGE_SCRATCH1..4 (FRS Mods' tools/build_pack.py: the bare metal
with the scratches in the alpha, four strengths, DXT5), tools/damage/scratches.tpk, as the game's LoadChunks takes it.
runtime/damage.c reads it only while the crash damage is on, so the browser fetches it only then (the page's preload
too: web/src/host/datacache.ts); without it the dents come without scratches.  The server serves it with the rest of
MODPACKS/ (web/server-rs manifest.rs GAME_ENTRIES).  It is checked as the game reads it (tools/packcheck.py) before it
is written."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import packcheck  # noqa: E402

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'damage', 'scratches.tpk')
REL = os.path.join('MODPACKS', 'DAMAGE', 'SCRATCHES.TPK')
TPK_CHUNK = 0xB3300000


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    data = open(SRC, 'rb').read()
    chunk, size = struct.unpack_from('<II', data)
    if chunk != TPK_CHUNK or size + 8 != len(data):
        sys.exit(f'{SRC}: not one texture pack chunk (0x{chunk:08X}, {size + 8} of {len(data)} bytes)')
    bad = packcheck.check(None, {REL.replace(os.sep, '/'): data})    # as runtime/damage.c and the game's loader read it
    if bad: sys.exit(f'{SRC}: not written, tools/packcheck.py says: ' + '; '.join(why for _, why in bad))
    out = os.path.join(sys.argv[1], REL)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if os.path.exists(out) and open(out, 'rb').read() == data:
        print(f'{out}: as it is ({len(data)} bytes)')
        return
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, out)
    print(f'{out}: {len(data)} bytes')


if __name__ == '__main__':
    main()
