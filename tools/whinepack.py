#!/usr/bin/env python3
"""The gearbox whine's sounds: MODPACKS/WHINE/ in the game folder, the three
looped sounds of ropi's Underground 2 Transmission Whine (UG2WhineGen 1.0.2:
NFS Carbon's BMW M3 GTR gearbox whine), which runtime/whine.c plays with the
Mods tab's Transmission Whine on.  They are not in the game build: the server
serves them with the rest of the game folder, and a game reads them (so the
page fetches them) only with the switch on.

    tools/whinepack.py <folder> --mod <the mod's release zip> [--any]

<the mod's release zip> is the mod's 1.0.2 release as it comes
(ug2WhineGen-1-0-2.zip, the snd folder in it); a folder with its files
extracted works too.  Of it only the three sounds are taken, as they are
(16-bit mono PCM WAVs): its plugin, its settings file and its two on/off
chimes are not.  <folder> is the game folder, or any folder to copy MODPACKS/
from to the game folder.  Writes:
  MODPACKS/WHINE/car_whine.wav           the whine
  MODPACKS/WHINE/car_whine_2.wav         its second layer
  MODPACKS/WHINE/car_whine_reverse.wav   the whine in reverse
Each sound is checked: a 16-bit mono PCM WAV of at most 4 MB (tools/packcheck.py,
as runtime/whine.c reads it); and the 1.0.2 release's own (SHA-256), unless
--any (another release's, or a sound of your own).
"""
import argparse, hashlib, os, struct, sys, zipfile

NAMES = ('car_whine.wav', 'car_whine_2.wav', 'car_whine_reverse.wav')
# the 1.0.2 release's sounds (runtime/whine.c's gains are the mod's for these)
SHA256 = {
    'car_whine.wav': 'c03885fa49022e18973767d1140afd9ef8a2c447c0096aa576c4e94390e9b5ab',
    'car_whine_2.wav': 'a85d8782a92ed1aa0b0a9caac90a98e8c387b1d6776b75d9059868197e7cb7c3',
    'car_whine_reverse.wav': '4fe209772b86aeb45410fd93f974ca43b3d635ea4125d1c0758218795e05dc93',
}


def wav_check(name, b):
    """why b is not a WAV runtime/whine.c reads (16-bit mono PCM), or None"""
    if len(b) < 12 or b[:4] != b'RIFF' or b[8:12] != b'WAVE':
        return 'not a WAV file'
    o, fmt = 12, None
    while o + 8 <= len(b):
        cid, sz = b[o:o + 4], struct.unpack_from('<I', b, o + 4)[0]
        if cid == b'fmt ' and sz >= 16:
            fmt = struct.unpack_from('<HHIIHH', b, o + 8)
        elif cid == b'data':
            if not fmt:
                return 'no format before its samples'
            tag, ch, rate, _, _, bits = fmt
            if (tag, ch, bits) != (1, 1, 16) or not rate:
                return f'{bits}-bit, {ch} channel(s), format {tag}: not 16-bit mono PCM'
            if min(sz, len(b) - o - 8) < 16:
                return 'no samples'
            return None
        o += 8 + sz + (sz & 1)
    return 'no samples'


def read_mod(src):
    """the three sounds of the mod's zip (or extracted folder): {name: bytes}"""
    got = {}
    if os.path.isdir(src):
        for root, _, files in os.walk(src):
            for f in files:
                if f.lower() in NAMES and os.path.basename(root).lower() == 'snd':
                    got.setdefault(f.lower(), open(os.path.join(root, f), 'rb').read())
    else:
        with zipfile.ZipFile(src) as z:
            for i in z.infolist():
                parts = i.filename.replace('\\', '/').lower().split('/')
                if len(parts) >= 2 and parts[-2] == 'snd' and parts[-1] in NAMES and not i.is_dir():
                    got.setdefault(parts[-1], z.read(i))
    return got


def main():
    ap = argparse.ArgumentParser(usage='tools/whinepack.py <folder> --mod <the mod\'s release zip> [--any]')
    ap.add_argument('folder', help='where MODPACKS/ goes: the game folder, or one to copy it from')
    ap.add_argument('--mod', required=True, metavar='ZIP', help='the mod\'s release (ug2WhineGen-1-0-2.zip), or its files extracted')
    ap.add_argument('--any', action='store_true', help='take sounds that are not the 1.0.2 release\'s')
    a = ap.parse_args()
    got = read_mod(a.mod)
    missing = [n for n in NAMES if n not in got]
    if missing:
        sys.exit(f'{a.mod}: no snd/{", snd/".join(missing)}: not the mod\'s release')
    for n in NAMES:
        why = wav_check(n, got[n])
        if why:
            sys.exit(f'{a.mod}: snd/{n}: {why}')
        if hashlib.sha256(got[n]).hexdigest() != SHA256[n] and not a.any:
            sys.exit(f'{a.mod}: snd/{n} is not the 1.0.2 release\'s (--any takes it all the same)')
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import packcheck                                   # (here: it reads wav_check from this file)
    bad = packcheck.check(None, {f'MODPACKS/WHINE/{n}': got[n] for n in NAMES})
    if bad:
        sys.exit('not written, tools/packcheck.py says: ' + '; '.join(f'{k}: {why}' for k, why in bad))
    out = os.path.join(a.folder, 'MODPACKS', 'WHINE')
    os.makedirs(out, exist_ok=True)
    for n in NAMES:
        with open(os.path.join(out, n), 'wb') as f:
            f.write(got[n])
    total = sum(len(got[n]) for n in NAMES)
    print(f'{out}: {len(NAMES)} sounds, {total / 1e6:.2f} MB')


if __name__ == '__main__':
    main()
