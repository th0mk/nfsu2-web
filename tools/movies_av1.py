#!/usr/bin/env python3
"""The movies in AV1: MOVIES/AV1/<name>.vp6 (and LANGPACKS/<language>/MOVIES/AV1/<name>.vp6) next to the game's
VP6 movies, for the browsers that decode AV1 (WebCodecs).  The page plays those instead of the VP6 ones
(web/src/host/movie.ts, runtime/movie_av1.c); every other browser, and the Mac, keeps the VP6 files.

    tools/movies_av1.py <game folder> <out folder> [--crf 26] [--cpu-used 4] [--jobs 6] [--threads 4]
        [--only Scene01] [--only ^LANGPACKS/] [--force]
    tools/movies_av1.py --check <game folder> <out folder> [--ffmpeg <one with libvmaf>] [--only ...]
        VMAF (or PSNR) of each AV1 movie's pictures against its VP6 one's, frame for frame

<out folder> gets the AV1 files at their place in the game folder (MOVIES/AV1/..., LANGPACKS/x/MOVIES/AV1/...):
copy its MOVIES and LANGPACKS over the game folder's and the server's manifest lists them.  A movie whose AV1 file
is already there and newer than its VP6 file is left alone, so a second run only makes what is new.  At crf 26
every movie keeps a VMAF of 95 or more against its VP6 pictures (the game folder's 95.8 to 99.6, the language
packs' dubbed ones, 512x512 at 25 fps, 95.4 to 99.7).

The AV1 file is the VP6 file with each video frame swapped: EA's chunks stay as they were, in the same order
(MVhd; MV0K for a key frame, MV0F for the others; the sound's SCHl/SCCl/SCDl/SCEl chunks byte for byte), so
the game's own movie player still reads the file, keeps the time, plays the sound and skips.  Only a video
chunk's payload is new: 'AV01', a flags byte (1: an AV1 key frame), three zero bytes, then one AV1 temporal
unit, one shown frame.  Every MV0K chunk holds an AV1 key frame, because the player skips to the next MV0K
when it falls more than 16 frames behind.  The header's largest-frame field (MVhd +0x14) is kept at least as
large as before.  No VP6 frame starts with 'AV01' (its first byte would make it a key frame of VP6 version
10, which does not exist), and this tool checks that for each file it reads.

The pictures are the VP6 frames as ffmpeg decodes them (the game's own decoder gives the same within a step of
rounding: 47 to 66 dB PSNR, some frames identical), upside down as that decoder leaves them in memory, so that
the page copies them in as they come.  ffmpeg reads the frames from the chunks this tool takes out (its EA
demuxer loses some of the dubbed movies' frames), and libaom encodes them at a constant quality (crf) a stretch
at a time, from each MV0K to the next (ffmpeg's libaom makes a forced key frame and the frame after it both key
frames).  Each language's dubbed movies share their pictures, so each picture stream is encoded once (a cache in
<out>/.av1cache).  Needs ffmpeg with libaom-av1 (apt install ffmpeg); --check also wants libvmaf (else PSNR).
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

MAGIC = b'AV01'
VIDEO = (b'MV0K', b'MV0F')
MIN_SIZE = 64 << 10     # blank.vp6 (2 KB, a black frame) stays VP6
_locks, _locks_lock = {}, threading.Lock()


def lock(key):
    """one encode of a picture stream at a time (the dubbed copies wait for it)"""
    with _locks_lock:
        return _locks.setdefault(key, threading.Lock())


def chunks(data):
    """EA's movie chunks: (id, offset, size with the 8-byte header)"""
    out, o = [], 0
    while o + 8 <= len(data):
        cid, size = data[o:o + 4], struct.unpack_from('<I', data, o + 4)[0]
        if size < 8 or o + size > len(data):
            raise ValueError(f'bad chunk {cid!r} at {o}')
        out.append((cid, o, size))
        o += size
    if o != len(data):
        raise ValueError(f'{len(data) - o} bytes after the last chunk')
    if not out or out[0][0] != b'MVhd':
        raise ValueError('no MVhd chunk first')
    return out


def movies(game):
    """every .vp6 the game can open: MOVIES/*.vp6 and LANGPACKS/<language>/MOVIES/*.vp6 (relative, with /)"""
    found = []
    for top in os.listdir(game):
        if top.lower() == 'movies':
            found += [f'{top}/{f}' for f in sorted(os.listdir(os.path.join(game, top))) if f.lower().endswith('.vp6')]
        elif top.lower() == 'langpacks':
            for lang in sorted(os.listdir(os.path.join(game, top))):
                ld = os.path.join(game, top, lang)
                for m in os.listdir(ld) if os.path.isdir(ld) else []:
                    if m.lower() == 'movies' and os.path.isdir(os.path.join(ld, m)):
                        found += [f'{top}/{lang}/{m}/{f}' for f in sorted(os.listdir(os.path.join(ld, m)))
                                  if f.lower().endswith('.vp6')]
    return found


def av1_path(rel):
    """MOVIES/x.vp6 -> MOVIES/AV1/x.vp6 (the page maps it the same way: web/src/host/datacache.ts av1Path)"""
    d, f = rel.rsplit('/', 1)
    return f'{d}/AV1/{f}'


def has_key(tu):
    """whether a temporal unit holds an AV1 key frame (its OBUs, each with its size: OBU_FRAME or OBU_FRAME_HEADER
    whose header starts show_existing_frame = 0, frame_type = KEY_FRAME)"""
    o = 0
    while o < len(tu):
        h = tu[o]
        t, p = (h >> 3) & 15, o + 1 + ((h >> 2) & 1)
        size, shift = 0, 0
        while True:
            b = tu[p]
            p += 1
            size |= (b & 127) << shift
            shift += 7
            if not b & 128:
                break
        if t in (3, 6) and size and tu[p] >> 5 == 0:
            return True
        o = p + size
    return False


def ivf(path, fourcc, w, h, num, den, frames):
    """frames into an IVF file (one per packet: ffmpeg then decodes VP6 frame for frame, where its EA demuxer loses
    some of the language packs' frames)"""
    with open(path, 'wb') as f:
        f.write(b'DKIF' + struct.pack('<HH4sHHIIII', 0, 32, fourcc, w, h, num, den, len(frames), 0))
        for i, fr in enumerate(frames):
            f.write(struct.pack('<IQ', len(fr), i) + fr)


def ivf_frames(path):
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'DKIF':
        raise ValueError('not an IVF file')
    o, out = struct.unpack_from('<H', d, 6)[0], []
    while o + 12 <= len(d):
        n = struct.unpack_from('<I', d, o)[0]
        out.append(d[o + 12:o + 12 + n])
        o += 12 + n
    return out


def encode(vp6, w, h, num, den, a):
    """VP6 frames from a key frame on (ffmpeg's decode) -> AV1 temporal units, the first a key frame.  A movie is
    encoded a stretch at a time, from each of its key frames to the next: ffmpeg's libaom makes a forced key frame
    and the frame after it both key frames.  libaom 3.8 stops now and then on an assertion in its two-pass
    statistics (pass2_strategy.c, 1 stretch of 300): such a stretch is tried again, the last time without its
    look-ahead."""
    with tempfile.TemporaryDirectory() as td:
        src, dst = os.path.join(td, 'vp6.ivf'), os.path.join(td, 'av1.ivf')
        ivf(src, b'VP60', w, h, num, den, vp6)
        for lag in ([], [], ['-lag-in-frames', '0']):
            dec = subprocess.Popen(['ffmpeg', '-v', 'error', '-i', src, '-fps_mode', 'passthrough', '-vf', 'vflip',
                                    '-f', 'rawvideo', '-pix_fmt', 'yuv420p', 'pipe:'], stdout=subprocess.PIPE)
            enc = subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'yuv420p', '-s', f'{w}x{h}',
                                  '-r', f'{num}/{den}', '-i', 'pipe:', '-c:v', 'libaom-av1', '-crf', str(a.crf), '-b:v', '0',
                                  '-cpu-used', str(a.cpu_used), '-row-mt', '1', '-threads', str(a.threads), '-g', '9999',
                                  *lag, '-color_primaries', 'bt709', '-color_trc', 'bt709', '-colorspace', 'bt709',
                                  '-color_range', 'tv', '-f', 'ivf', dst], stdin=dec.stdout)
            dec.stdout.close()
            if not dec.wait() and not enc.returncode:
                break
        else:
            raise RuntimeError(f'ffmpeg failed ({dec.returncode}, {enc.returncode})')
        tus = ivf_frames(dst)
    if len(tus) != len(vp6) or not has_key(tus[0]):
        raise ValueError(f'{len(tus)} AV1 frames for {len(vp6)} VP6 frames, first a key frame: {has_key(tus[0])}')
    return tus


def movie(data):
    """a movie's header fields and video chunks: (w, h, frames, largest, num, den, chunks, video chunks)"""
    ch = chunks(data)
    hd = data[ch[0][1]:ch[0][1] + ch[0][2]]
    w, h = struct.unpack_from('<HH', hd, 12)
    frames, largest, num, den = struct.unpack_from('<IIII', hd, 16)
    return w, h, frames, largest, num, den, ch, [(cid, o, s) for cid, o, s in ch if cid in VIDEO]


def convert(game, out, rel, a, pool):
    src, dst = os.path.join(game, rel), os.path.join(out, av1_path(rel))
    if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src) and not a.force:
        return {'file': rel, 'skipped': 'up to date', 'vp6': os.path.getsize(src), 'av1': os.path.getsize(dst)}
    with open(src, 'rb') as f:
        data = f.read()
    if len(data) < MIN_SIZE:
        return {'file': rel, 'skipped': 'small', 'vp6': len(data)}
    w, h, frames, largest, num, den, ch, video = movie(data)
    if frames != len(video):
        raise ValueError(f'{rel}: header says {frames} frames, {len(video)} video chunks')
    if any(data[o + 8:o + 12] == MAGIC for _, o, _ in video):
        raise ValueError(f'{rel}: a VP6 frame starts with {MAGIC!r}')
    keys = [i for i, (cid, _, _) in enumerate(video) if cid == b'MV0K']
    if not keys or keys[0]:
        raise ValueError(f'{rel}: the first frame is no key frame')
    vp6 = [data[o + 8:o + s] for _, o, s in video]
    sha = hashlib.sha256()
    for fr in vp6:
        sha.update(fr)
    key = f'{sha.hexdigest()[:24]}-{w}x{h}-crf{a.crf}-cpu{a.cpu_used}-up'
    cache = os.path.join(out, '.av1cache')
    os.makedirs(cache, exist_ok=True)
    path, t0 = os.path.join(cache, key + '.ivf'), time.time()
    with lock(key):
        shared = os.path.exists(path)
        if not shared:
            ends = keys[1:] + [len(vp6)]
            segs = [pool.submit(encode, vp6[k:e], w, h, num, den, a) for k, e in zip(keys, ends)]
            tus = [tu for f in segs for tu in f.result()]
            ivf(path + '.tmp', b'AV01', w, h, num, den, tus)
            os.replace(path + '.tmp', path)
    tus = ivf_frames(path)
    if len(tus) != len(video):
        raise ValueError(f'{rel}: {len(tus)} AV1 frames for {len(video)} video chunks')
    iskey = [has_key(t) for t in tus]
    if not all(iskey[k] for k in keys):
        raise ValueError(f'{rel}: an MV0K frame is no AV1 key frame')
    parts, vi, top = [], 0, 0
    for cid, o, s in ch:
        if cid in VIDEO:
            pay = MAGIC + bytes([1 if iskey[vi] else 0, 0, 0, 0]) + tus[vi]
            parts.append(cid + struct.pack('<I', 8 + len(pay)) + pay)
            top = max(top, len(pay))
            vi += 1
        else:
            parts.append(data[o:o + s])
    head = bytearray(parts[0])
    struct.pack_into('<I', head, 0x14, max(largest, top))
    parts[0] = bytes(head)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst + '.tmp', 'wb') as f:
        f.write(b''.join(parts))
    os.replace(dst + '.tmp', dst)
    return {'file': rel, 'vp6': len(data), 'av1': os.path.getsize(dst), 'frames': len(video), 'size': f'{w}x{h}',
            'keys': len(keys), 'av1keys': sum(iskey), 'shared': shared, 'seconds': round(time.time() - t0, 1)}


def check(game, out, rel, a, pool=None):
    """VMAF (libvmaf) or PSNR of the AV1 file's pictures against the VP6 file's (decoded as the AV1 ones were made)"""
    src, dst = os.path.join(game, rel), os.path.join(out, av1_path(rel))
    if not os.path.exists(dst):
        return {'file': rel, 'skipped': 'no AV1 file'}
    res = {'file': rel}
    with tempfile.TemporaryDirectory() as td:
        for name, path, fourcc in (('ref', src, b'VP60'), ('av1', dst, b'AV01')):
            with open(path, 'rb') as f:
                data = f.read()
            w, h, _, _, num, den, _, video = movie(data)
            ivf(os.path.join(td, name + '.ivf'), fourcc, w, h, num, den,
                [data[o + (16 if fourcc == b'AV01' else 8):o + s] for _, o, s in video])
        vmaf = 'libvmaf' in subprocess.run([a.ffmpeg, '-hide_banner', '-filters'], capture_output=True, text=True).stdout
        # frame for frame (the two files' timestamps round apart), the AV1 pictures upright again, both with the AV1
        # file's colour tags (else ffmpeg 7 converts one, as the browser does not: 37 dB where it is 47)
        tags = 'setparams=range=tv:colorspace=bt709:color_primaries=bt709:color_trc=bt709'
        pair = f'[0:v]vflip,setpts=N,{tags}[a];[1:v]setpts=N,{tags}[b];[a][b]'
        lav = pair + ('libvmaf=n_threads=4:feature=name=psnr' if vmaf else 'psnr')
        r = subprocess.run([a.ffmpeg, '-hide_banner', '-i', os.path.join(td, 'av1.ivf'), '-i', os.path.join(td, 'ref.ivf'), '-lavfi', lav,
                            '-f', 'null', '-'], capture_output=True, text=True)
        for line in r.stderr.splitlines():
            if 'VMAF score' in line:
                res['vmaf'] = round(float(line.split(':')[-1]), 2)
            if 'PSNR y:' in line:
                res['psnr_y'] = line.split('PSNR y:')[1].split()[0]
    return res


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('game')
    p.add_argument('out')
    p.add_argument('--crf', type=int, default=26)
    p.add_argument('--cpu-used', type=int, default=4)
    p.add_argument('--jobs', type=int, default=6, help='stretches encoded at once (--check: movies at once)')
    p.add_argument('--threads', type=int, default=4, help="libaom's threads for each")
    p.add_argument('--only', action='append', help='only files whose path has this (^: begins with it; any of them)')
    p.add_argument('--force', action='store_true', help='make them again')
    p.add_argument('--check', action='store_true', help='measure the AV1 files instead of making them')
    p.add_argument('--ffmpeg', default='ffmpeg', help='the ffmpeg for --check (one with libvmaf)')
    a = p.parse_args()
    if not shutil.which(a.ffmpeg if a.check else 'ffmpeg'):
        sys.exit('ffmpeg is missing (apt install ffmpeg)')
    pick = lambda m, s: m.lower().startswith(s[1:].lower()) if s.startswith('^') else s.lower() in m.lower()
    todo = [m for m in movies(a.game) if not a.only or any(pick(m, s) for s in a.only)]
    # the biggest first, and one of each picture stream before its copies (they wait for its cache file)
    todo.sort(key=lambda m: -os.path.getsize(os.path.join(a.game, m)))
    job = check if a.check else convert
    tot = {'vp6': 0, 'av1': 0}
    with cf.ThreadPoolExecutor(a.jobs) as pool, cf.ThreadPoolExecutor(a.jobs if a.check else 4) as ex:
        for r in ex.map(lambda m: job(a.game, a.out, m, a, pool), todo):
            print(json.dumps(r), flush=True)
            if 'av1' in r:
                tot['vp6'] += r['vp6']
                tot['av1'] += r['av1']
    if tot['vp6']:
        print(f"VP6 {tot['vp6'] / 1e6:.1f} MB -> AV1 {tot['av1'] / 1e6:.1f} MB ({tot['av1'] / tot['vp6']:.1%})")


if __name__ == '__main__':
    main()
