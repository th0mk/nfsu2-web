#!/usr/bin/env python3
"""The start page's Full Scene Anti-Aliasing question's picture (web/src/app/shell.ts askFsaa): the player's car at a
Quick Race's start, its roof, rear window, wiper and mirror, zoomed 4x without smoothing, once with the game's FSAA
off and once on.  Made from two frame dumps of the game, so the pictures are the game's and live in the server's game
folder beside the texture packs (TEXPACKS/fsaa-off.webp, TEXPACKS/fsaa-on.webp), never in this repository.

The two frames: tests/web/scripts/fsaa_pics.txt on /test/play.html at w=1280&h=720&dumpscale=1, once with
reg=/tests/web/seed/registry_perf4_native.txt (FSAA off) and once with reg=/tests/web/seed/registry_perf4_fsaa.txt
(on); its dump fsaa_a (fsaa_b: the same moment 3 frames later, should a dump be dropped).

    python3 tools/fsaapics.py <off.png> <on.png> <out folder>   (writes <out folder>/TEXPACKS/fsaa-{off,on}.webp)
    [--box X,Y,W,H] the part of the 1280x720 frame (default 525,400,200,100: 2:1, as the question shows it)
    [--scale N]     its zoom (default 4: 800x400, the question's 400x200 at two device pixels a pixel)

Needs Pillow (the repo's .venv).  Lossless WebP: the zoomed pixels' edges are the point.
"""
import argparse
import os
import sys

from PIL import Image


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('off'); ap.add_argument('on'); ap.add_argument('out')
    ap.add_argument('--box', default='525,400,200,100')
    ap.add_argument('--scale', type=int, default=4)
    a = ap.parse_args()
    x, y, w, h = (int(v) for v in a.box.split(','))
    dest = os.path.join(a.out, 'TEXPACKS')
    os.makedirs(dest, exist_ok=True)
    for tag, src in (('off', a.off), ('on', a.on)):
        im = Image.open(src).convert('RGB')
        if im.size != (1280, 720):
            print(f'{src}: {im.size[0]}x{im.size[1]}, not the 1280x720 dump the box is for', file=sys.stderr)
            return 1
        part = im.crop((x, y, x + w, y + h)).resize((w * a.scale, h * a.scale), Image.NEAREST)
        path = os.path.join(dest, f'fsaa-{tag}.webp')
        part.save(path, 'WEBP', lossless=True, method=6)
        print(f'{path}: {part.width}x{part.height}, {os.path.getsize(path)} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
