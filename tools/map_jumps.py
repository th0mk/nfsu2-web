#!/usr/bin/env python3
"""Where the game's World Map jumps a car to in free roam, for the server's cheat watch:
web/server-rs/crates/roam/src/jumps.rs.

  tools/map_jumps.py <game>/TRACKS/ROUTESL4RA/PathsFreeRoam.bin > web/server-rs/crates/roam/src/jumps.rs

The map's jump moves the car within free roam, with no loading screen: onto an event's trigger (marker type 13, a
race's start), or about 15 m beside a shop's, the garage's or the car lot's (type 14).  The markers are the file's
chunk 0x3414A, variable-size records (+0 type, +4 x, +8 y, +0x42 the record's size).  Only their places go into the
table, no game data.
"""
import sys, struct

TYPES = {13: 'event', 14: 'shop'}

def markers(b):
    o = 8
    while o + 8 <= len(b):
        cid, size = struct.unpack_from('<II', b, o)
        if cid == 0x3414A:
            p, end = o + 8, o + 8 + size
            while p + 0x44 <= end:
                t, x, y = struct.unpack_from('<Iff', b, p)
                n, = struct.unpack_from('<h', b, p + 0x42)
                if n <= 0: break
                yield t, x, y
                p += n
            return
        o += 8 + size
    sys.exit('no marker chunk (0x3414A) in this file')

def main():
    b = open(sys.argv[1], 'rb').read()
    rows = sorted({(t, round(x), round(y)) for t, x, y in markers(b) if t in TYPES})
    if not rows: sys.exit('no event or shop triggers in this file')
    out = ['//! Where the game\'s World Map jumps a car to in free roam (watch.rs): the event triggers (a race\'s start, where',
           '//! the jump puts the car) and the shops\' triggers (the garage\'s and the car lot\'s too; it puts the car 15 m beside',
           '//! them), x and y (m).  Made by tools/map_jumps.py from the game\'s TRACKS/ROUTESL4RA/PathsFreeRoam.bin; do not edit.',
           '',
           f'pub(crate) const TRIGGERS: [(f64, f64); {len(rows)}] = [']
    line = '   '
    for t, x, y in rows:
        item = f' ({x}.0, {y}.0),'
        if len(line) + len(item) > 120:
            out.append(line); line = '   '
        line += item
    out += [line, '];']
    print('\n'.join(out))
    counts = {TYPES[t]: sum(r[0] == t for r in rows) for t in TYPES}
    print(f'{len(rows)} triggers: {counts}', file=sys.stderr)

main()
