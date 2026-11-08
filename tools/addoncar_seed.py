#!/usr/bin/env python3
"""A save tree for tests/scripts/career_addon_car.txt: a copy of a career seed (one car in slot 1) whose profile has
cars of car types the game does not have, as a modded game's profile can (runtime/garage.c addon_cars):

  tools/addoncar_seed.py <seed tree> <out tree> [--af28] [--name CLAUDE]

  - the career car (slot 1): car type bStringHash("ADDONCAR1")
  - Customize car 1: a copy of the career car's parts as car type "ADDONCAR2" (handle TUNED_CAR_0, as 0x52A710 makes one)
  - a GARAGE.DAT beside the profile (slots 6-10, runtime/garage.c): slot 6 a copy of the career car as "ADDONCAR3"
    (handle CAREER_SLOT_5, owned, open), slots 7-10 empty and locked, its fingerprint the poked profile's
  - --af28: the career car (before it is poked) with Art Factory 28 (ART_F_28) in its vinyl slot (0x40), as a profile
    saved before the vinyls were kept beside it: a raw key, for the conversion at the next save
  - --af28-only: only that (no car types poked, no GARAGE.DAT)
  - --badstock: stock car 1 of car type 60, past the game's 46 (a modded game's car lot sells its added cars):
    runtime/overrides.c cars_stock_fix makes the stock cars again as the profile loads
  - --newdir: a folder named MODPARTS.NEW beside the profile (a file of it), so the game cannot write that file
    (as on a full disk, where a new file fails and overwriting works)
  tools/addoncar_seed.py --compare <seed tree> <saved tree>
    which car records (Customize, slots 1-10, presets) a save made of the seed has byte for byte as the seed has them
  tools/addoncar_seed.py --two <seed tree> <out tree> [--full]
    slot 1 car type ADDA, slot 2 ADDB, each a copy of the career car with keys of its own in slots 0x60-0x6F, no
    GARAGE.DAT: two kept cars of the same stand-in and parts (Trade Car, tests/scripts/career_addon_trade.txt);
    --full: slots 3-5 locked and a GARAGE.DAT with 6-10 locked, so the car lot trades a car in for one it sells
  tools/addoncar_seed.py --check <saved tree> [--name CLAUDE]
    what a save made of it holds: the profile's checksums, the car types of slot 1, Customize car 1 and GARAGE.DAT's
    slot 6, slot 1's vinyl, and MODPARTS.DAT's entries (its head, kind 0 parts, entries of other kinds)
The profile is sealed again (web/src/app/profile.ts checksum).  The seed itself is never written: the tree is copied
first.  Prints the poked records' handles and car types.
"""
import shutil
import struct
import sys

PAYLOAD_AT = 36
CARS_PART = 39230
CUSTOM_AT, CUSTOM_SZ = 1156, 1072
CAREER_AT, CAREER_SZ = 22596, 2034
SLOT_VINYL = 0x40


def bhash(s):
    h = 0xFFFFFFFF
    for ch in s.encode():
        h = (h * 33 + ch) & 0xFFFFFFFF
    return h


def crc_table():
    t = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) if r & 0x80000000 else (r << 1)
        t.append(r & 0xFFFFFFFF)
    return t


T = crc_table()


def checksum(d):
    if len(d) < 4:
        return 0
    r = ~((d[0] << 24) | (d[1] << 16) | (d[2] << 8) | d[3]) & 0xFFFFFFFF
    for b in d[4:]:
        r = (((r << 8) | b) ^ T[r >> 24]) & 0xFFFFFFFF
    return ~r & 0xFFFFFFFF


def seal(f):
    struct.pack_into('<I', f, 16, checksum(f[28:36]))
    struct.pack_into('<I', f, 20, checksum(f[36:]))
    struct.pack_into('<I', f, 24, checksum(f[0:24]))


def fnv64(d):
    h = 0xcbf29ce484222325
    for b in d:
        h ^= b
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


NAMES = {bhash(n): n for n in ('PEUGOT', 'FOCUS', 'ADDONCAR1', 'ADDONCAR2', 'ADDONCAR3', 'ART_F_28', 'TUNED_CAR_0', 'ADDA', 'ADDB')}


def named(h):
    return f'{h:08x} ({NAMES[h]})' if h in NAMES else f'{h:08x}'


def check(tree, name):
    f = open(f'{tree}/NFS Underground 2/{name}/{name}', 'rb').read()
    sealed = struct.unpack_from('<3I', f, 16) == (checksum(f[28:36]), checksum(f[36:]), checksum(f[0:24]))
    pl = f[PAYLOAD_AT:]
    u = lambda o: struct.unpack_from('<I', pl, o)[0]
    print(f'profile: {len(f)} bytes, 20CM {f[:4] == b"20CM"}, checksums hold {sealed}, stock cars of a type past 46: '
          f'{sum(1 for i in range(48) if 46 <= u(i * 24 + 20) != 0xFFFFFFFF)}')
    print(f'slot 1: handle {u(CAREER_AT + 4):08x} car type {named(u(CAREER_AT + 28))}, vinyl {named(u(CAREER_AT + 32 + 4 * SLOT_VINYL))}')
    print('slots 1-5:', ', '.join(f'{named(u(CAREER_AT + i * CAREER_SZ + 28))} owned {pl[CAREER_AT + i * CAREER_SZ + 1073]} key60 '
                                  f'{u(CAREER_AT + i * CAREER_SZ + 32 + 4 * 0x60):08x}' for i in range(5)))
    print(f'Customize car 1: handle {named(u(CUSTOM_AT + 4))} car type {named(u(CUSTOM_AT + 28))}')
    try:
        g = open(f'{tree}/NFS Underground 2/{name}/GARAGE.DAT', 'rb').read()
        fp = fnv64(pl[:CARS_PART])
        hd = struct.unpack_from('<8I', g, 0)
        s6 = g[32:32 + CAREER_SZ]
        print(f'GARAGE.DAT: fingerprint matches {hd[2] == fp & 0xFFFFFFFF and hd[3] == fp >> 32}, slot 6 handle '
              f'{struct.unpack_from("<I", s6, 4)[0]:08x} car type {named(struct.unpack_from("<I", s6, 28)[0])} owned {s6[1073]}')
    except FileNotFoundError:
        print('GARAGE.DAT: none')
    try:
        m = open(f'{tree}/NFS Underground 2/{name}/MODPARTS.DAT', 'rb').read()
        hd = struct.unpack_from('<4I', m, 0)
        print(f'MODPARTS.DAT: magic {hd[0]:08x} version {hd[1]} count {hd[2]} (size {len(m)} = {16 + 32 * hd[2]}), write {hd[3]}')
        for i in range(hd[2]):
            w = struct.unpack_from('<8I', m, 16 + 32 * i)
            if w[7] == 0:
                print(f'  part: car {w[0]:08x} type {named(w[1])} slot {w[2]:#x} key {named(w[3])} stand-in {w[4]:08x}')
            elif w[7] == 1:
                ks = {}
                for j in range(hd[2]):
                    x = struct.unpack_from('<8I', m, 16 + 32 * j)
                    if x[7] == 2 and x[0] == w[0]:
                        for q in range(5): ks[x[1] + q] = x[2 + q]
                print(f'  car: {w[0]:08x} shown as {named(w[1])}, its own car type {named(w[3])}, {w[4]} key entries, '
                      f'key60 {ks.get(0x60, 0):08x} vinyl {named(ks.get(SLOT_VINYL, 0))}, fp {w[6]:08x}{w[5]:08x}')
            elif w[7] == 3 and w[0] == 0x4448504d:
                print(f'  head: the profile {w[1]:08x} (CLAUDE {bhash("CLAUDE"):08x}), write {w[2]}')
            elif w[7] != 2:
                print(f'  kind {w[7]}: {w}')
        print(f'  keys entries: {sum(1 for i in range(hd[2]) if struct.unpack_from("<I", m, 16 + 32 * i + 28)[0] == 2)}')
    except FileNotFoundError:
        print('MODPARTS.DAT: none')


def two(seed, out, full, name='CLAUDE'):
    shutil.copytree(seed, out)
    path = f'{out}/NFS Underground 2/{name}/{name}'
    f = bytearray(open(path, 'rb').read())
    rec = bytes(f[PAYLOAD_AT + CAREER_AT: PAYLOAD_AT + CAREER_AT + CAREER_SZ])
    for i, nm in enumerate(('ADDA', 'ADDB')):
        r = bytearray(rec)
        struct.pack_into('<I', r, 4, 0x319a8ca1 + i)
        struct.pack_into('<I', r, 28, bhash(nm))
        for s in range(0x60, 0x70):
            struct.pack_into('<I', r, 32 + 4 * s, 0xb0000000 | (i << 16) | s)
        r[1072], r[1073] = 0, 1
        f[PAYLOAD_AT + CAREER_AT + i * CAREER_SZ: PAYLOAD_AT + CAREER_AT + (i + 1) * CAREER_SZ] = r
    if full:
        for i in range(2, 5):
            f[PAYLOAD_AT + CAREER_AT + i * CAREER_SZ + 1072] = 1
    seal(f)
    open(path, 'wb').write(f)
    if full:
        pl = f[PAYLOAD_AT:]
        fp = fnv64(pl[:CARS_PART])
        empty = bytes(pl[CAREER_AT + 2 * CAREER_SZ: CAREER_AT + 3 * CAREER_SZ])
        slots = []
        for i in range(5):
            r = bytearray(empty)
            struct.pack_into('<I', r, 4, 0x319a8ca6 + i)
            r[1072], r[1073] = 1, 0
            slots.append(bytes(r))
        hd = struct.pack('<8I', 0x4147324e, 1, fp & 0xFFFFFFFF, fp >> 32, 0, 0, 0, 0)
        open(f'{out}/NFS Underground 2/{name}/GARAGE.DAT', 'wb').write(hd + b''.join(slots))
    print(f'slot 1 ADDA {bhash("ADDA"):08x}, slot 2 ADDB {bhash("ADDB"):08x}' + (' (2 of 10 slots open, both full)' if full else ''))


def records(tree, name='CLAUDE'):
    """every car record of the profile (Customize 20, slots 1-5, presets 6) and GARAGE.DAT's 5, by name: its bytes"""
    f = open(f'{tree}/NFS Underground 2/{name}/{name}', 'rb').read()[PAYLOAD_AT:]
    out = {}
    for i in range(20):
        out[f'Customize car {i + 1}'] = f[CUSTOM_AT + i * CUSTOM_SZ: CUSTOM_AT + (i + 1) * CUSTOM_SZ]
    for i in range(5):
        out[f'slot {i + 1}'] = f[CAREER_AT + i * CAREER_SZ: CAREER_AT + (i + 1) * CAREER_SZ]
    for i in range(6):
        out[f'preset {i + 1}'] = f[32770 + i * 1076: 32770 + (i + 1) * 1076]
    try:
        g = open(f'{tree}/NFS Underground 2/{name}/GARAGE.DAT', 'rb').read()
        for i in range(5):
            out[f'slot {6 + i}'] = g[32 + i * CAREER_SZ: 32 + (i + 1) * CAREER_SZ]
    except FileNotFoundError:
        pass
    return out


def compare(seed, saved, name='CLAUDE'):
    a, b = records(seed, name), records(saved, name)
    same = [k for k in a if k in b and a[k] == b[k]]
    diff = [k for k in a if k in b and a[k] != b[k]]
    print(f'{len(same)} records byte for byte as in the seed; {len(diff)} not:')
    for k in diff:
        t0, t1 = struct.unpack_from('<I', a[k], 28)[0], struct.unpack_from('<I', b[k], 28)[0]
        print(f'  {k}: car type {named(t0)} -> {named(t1)}')


def main():
    if sys.argv[1:2] == ['--compare']:
        return compare(sys.argv[2], sys.argv[3])
    if sys.argv[1:2] == ['--check']:
        nm = sys.argv[sys.argv.index('--name') + 1] if '--name' in sys.argv else 'CLAUDE'
        return check(sys.argv[2], nm)
    if sys.argv[1:2] == ['--two']:
        a = [x for x in sys.argv[2:] if not x.startswith('--')]
        return two(a[0], a[1], '--full' in sys.argv)
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    flags = [a for a in sys.argv[1:] if a.startswith('--')]
    name = 'CLAUDE'
    for i, a in enumerate(sys.argv):
        if a == '--name':
            name = sys.argv[i + 1]
            args.remove(name)
    if len(args) != 2:
        sys.exit(__doc__)
    seed, out = args
    shutil.copytree(seed, out)
    path = f'{out}/NFS Underground 2/{name}/{name}'
    f = bytearray(open(path, 'rb').read())
    assert f[:4] == b'20CM' and len(f) == 54966, 'not a profile of this version'

    def u32(o):
        return struct.unpack_from('<I', f, PAYLOAD_AT + o)[0]

    def put(o, v):
        struct.pack_into('<I', f, PAYLOAD_AT + o, v)

    car = CAREER_AT
    assert u32(car + 4) == 0x319a8ca1 and f[PAYLOAD_AT + car + 1073], 'slot 1 has no car'
    if '--af28' in flags or '--af28-only' in flags:
        put(car + 32 + 4 * SLOT_VINYL, bhash('ART_F_28'))
        print(f'slot 1: ART_F_28 {bhash("ART_F_28"):08x} in the vinyl slot')
    if '--af28-only' in flags:
        seal(f)
        open(path, 'wb').write(f)
        return
    rec = bytes(f[PAYLOAD_AT + car: PAYLOAD_AT + car + CAREER_SZ])
    # Customize car 1: 5 words (+4 0, handle, +0xC 0, in use, kind 2), the parts block (0x418), +0x430
    cu = CUSTOM_AT
    f[PAYLOAD_AT + cu: PAYLOAD_AT + cu + CUSTOM_SZ] = struct.pack('<5I', 0, bhash('TUNED_CAR_0'), 0, 1, 2) + rec[20:20 + 0x418] + rec[1068:1072]
    put(cu + 28, bhash('ADDONCAR2'))
    put(car + 28, bhash('ADDONCAR1'))
    if '--badstock' in flags:
        put(20, 60)
        print('stock car 1: car type 60')
    seal(f)
    open(path, 'wb').write(f)
    pl = f[PAYLOAD_AT:]
    fp = fnv64(pl[:CARS_PART])
    # GARAGE.DAT: slot 6 the career car as ADDONCAR3, slots 7-10 as slot 2 (empty), locked
    empty = bytes(pl[CAREER_AT + CAREER_SZ: CAREER_AT + 2 * CAREER_SZ])
    slots = []
    for i in range(5):
        r = bytearray(rec if i == 0 else empty)
        struct.pack_into('<I', r, 4, 0x319a8ca6 + i)
        if i == 0:
            struct.pack_into('<I', r, 28, bhash('ADDONCAR3'))
            r[1072], r[1073] = 0, 1
        else:
            r[1072], r[1073] = 1, 0
        slots.append(bytes(r))
    hd = struct.pack('<8I', 0x4147324e, 1, fp & 0xFFFFFFFF, fp >> 32, 0, 0, 0, 0)
    open(f'{out}/NFS Underground 2/{name}/GARAGE.DAT', 'wb').write(hd + b''.join(slots))
    if '--newdir' in flags:
        import os
        os.makedirs(f'{out}/NFS Underground 2/{name}/MODPARTS.NEW', exist_ok=True)
        open(f'{out}/NFS Underground 2/{name}/MODPARTS.NEW/keep', 'w').write('a folder: MODPARTS.NEW cannot be written\n')
    print(f'slot 1 (0x319a8ca1): ADDONCAR1 {bhash("ADDONCAR1"):08x}; Customize car 1 ({bhash("TUNED_CAR_0"):08x}): ADDONCAR2 '
          f'{bhash("ADDONCAR2"):08x}; GARAGE.DAT slot 6 (0x319a8ca6): ADDONCAR3 {bhash("ADDONCAR3"):08x}; fingerprint {fp:016x}')


if __name__ == '__main__':
    main()
