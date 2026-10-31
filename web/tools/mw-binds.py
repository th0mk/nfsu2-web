#!/usr/bin/env python3
# Most Wanted's input bindings as the game holds them (tests/mw/scripts/pad_binds.txt peeks the table 0x91F418 at the
# main menu): one line per action, its name read from speed.exe, its four bindings (type, device, code) decoded as
# GAME.md 13.1 has them: type 1 a keyboard DIK (in the second field), 2 a joystick button, 3 a POV-0 direction (0 up, 1 right, 2 down,
# 3 left), 4..9 axes.  The first two bindings are the keyboard's, the last two the joystick's.
#   python3 web/tools/mw-binds.py <run log> <speed.exe>
import struct
import sys

BASE, ROWS, ROW = 0x91F418, 76, 0x34


def pe_reader(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    opt = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    secs = []
    for i in range(nsec):
        vs, va, rs, raw = struct.unpack_from('<IIII', d, pe + 24 + opt + 40 * i + 8)
        secs.append((base + va, max(vs, rs), raw))

    def cstr(v):
        for a, n, raw in secs:
            if a <= v < a + n:
                o = raw + v - a
                return d[o:d.index(b'\0', o)].decode('latin1')
        return '?%08x' % v
    return cstr


DIK = {0x01: 'Esc', 0x0E: 'Backspace', 0x0F: 'Tab', 0x1C: 'Enter', 0x1D: 'LCtrl', 0x2A: 'LShift', 0x36: 'RShift',
       0x38: 'LAlt', 0x39: 'Space', 0x9C: 'NumEnter', 0x9D: 'RCtrl', 0xB8: 'RAlt', 0xC8: 'Up', 0xD0: 'Down',
       0xCB: 'Left', 0xCD: 'Right', 0xC9: 'PgUp', 0xD1: 'PgDn', 0xD2: 'Insert', 0x3B: 'F1', 0x3C: 'F2'}
ROWS_KEYS = '1234567890-='
for i, c in enumerate('QWERTYUIOP'):
    DIK[0x10 + i] = c
for i, c in enumerate('ASDFGHJKL'):
    DIK[0x1E + i] = c
for i, c in enumerate('ZXCVBNM'):
    DIK[0x2C + i] = c
for i, c in enumerate(ROWS_KEYS):
    DIK[0x02 + i] = c


def binding(t, dev, code):
    if t == 0:
        return '-'
    if t == 1:                          # a key's DIK is the binding's second field
        return 'key ' + DIK.get(dev, '0x%02x' % dev)
    if t == 2:
        return 'button %d' % code
    if t == 3:
        return 'pov %s' % ('up', 'right', 'down', 'left')[code] if code < 4 else 'pov %d' % code
    return 'axis type %d code %d' % (t, code)


def main():
    log, exe = sys.argv[1], sys.argv[2]
    cstr = pe_reader(exe)
    mem = {}
    for line in open(log, encoding='utf-8', errors='replace'):
        if ' peek [0x' not in line:
            continue
        a = int(line.split('peek [')[1][:10], 16)
        mem[a] = int(line.split('] = ')[1][:10], 16)
    for r in range(ROWS):
        at = BASE + r * ROW
        w = [mem.get(at + 4 * i) for i in range(13)]
        if None in w:
            print('%2d (not peeked)' % r)
            continue
        b = [binding(w[1 + 3 * k], w[2 + 3 * k], w[3 + 3 * k]) for k in range(4)]
        print('%2d %-28s keys: %-18s %-18s pad: %-18s %s' % (r, cstr(w[0]), b[0], b[1], b[2], b[3]))


main()
