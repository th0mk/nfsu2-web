#!/usr/bin/env python3
"""The game's music in fewer bytes: pfdata/MusicSFx.mus and MusicSFx.mpf
rebuilt so the songs in 4-bit ADPCM become EALayer3, the MP3 variant the game
already plays its other songs in, and without the streams no song uses.  No
game code changes: the game's own decoder plays both (docs/web/VPS.md, "Smaller
music").

    tools/musicpack.py <game folder> <out folder> [--jobs N]

writes <out folder>/MusicSFx.mus and MusicSFx.mpf (510 MB -> 235 MB, a minute
on 12 cores).  Needs LAME 3.100 on the PATH (`apt install lame`); the same
input and LAME give the same bytes.

What is in the original (PC):
  - MusicSFx.mpf, EA's interactive-music map (PathFinder "PFDx" version 4).
    The game's loader (0x73a7c0) finds its tables from the header; the last
    one, the "samples" table, has an entry per song: its offset in the .mus
    / 0x80 and its length in ms.  The event graph names songs by their index
    in that table and nothing else points into the .mus.  Only the table's
    offsets change here.
  - MusicSFx.mus, 73 SCHl streams one after the other, each at a multiple of
    0x80 (zeros in between).  The table names 43: 16 songs in EALayer3 (32 kHz,
    ~130 kbit/s), kept as they are, and 27 in EA-XA ADPCM (44.1 kHz,
    382 kbit/s, 66 % of the file).  The other 30 are never read: an EALayer3
    copy of 27 of the ADPCM songs (some a shorter edit) and three ADPCM
    streams.
  - The music's stream buffer: 4780 ms at the rate of the table's
    second-to-last song (0x73aea0 sizes it from the distance between the last
    two entries; 82173 bytes with the overhead, seen at 0x82ba08).  Those two
    are EALayer3 songs kept next to each other, so it stays the same (checked
    below); the new songs need fewer bytes a second than the ADPCM it served.

The ADPCM songs are decoded (EA-XA v1, as EA's sx.exe and vgmstream decode it:
the history runs on across blocks, the one stored per block is not read) and
encoded with LAME, VBR -V 1 without the bit reservoir (an EALayer3 granule
carries its own bits): 217-254 kbit/s, 231 on average.  -V 1 because it is
transparent to the ADPCM: PEAQ (GstPEAQ, advanced) over 20 s of each song
gives -0.17 to -0.02, -0.07 on average (0 = imperceptible).  On six excerpts
of three songs -V 2 (~190 kbit/s) gives -0.28 to -0.12, -V 0 (~280 kbit/s)
-0.03 to +0.02.
Each MP3 granule becomes an EALayer3 v1 frame, as EA's own encoder wrote them
(vgmstream's mpeg_custom_utils_ealayer3.c reads the same format):
  0x00, 8 bits MPEG version / sample-rate index / channel mode / mode
  extension, 1 bit granule, (granule 1: 4 bits scfsi per channel), per
  channel the granule's 59 bits of side info, then its main data, padded to a
  byte.  A PCM frame is 0xEE, u16 BE offset, u16 BE count, count stereo
  samples (s16 BE): they replace the decoded samples from (end of the granule
  before it - offset) on.
The decoder skips 1105 samples at the start (MP3's 529 + one granule): with
LAME's 576-sample encoder delay, output sample n is the encoder's input sample
n.  As in EA's files the first 47 samples (what is left of frame 0) and the
last 100 come as PCM, so both ends are the original's (pcm_plan; it gives EA's
own 43 EALayer3 streams their PCM frames exactly).  Blocks (SCDl) hold two MP3
frames, one when two would pass the largest block EA wrote (2384 bytes): id,
size, the samples the block adds, 0, then 1 / 0x01000000 for even / odd
blocks, the frames, zeros to a multiple of 4.  The header (SCHl) is the ADPCM
song's with version 2 and codec 0x17 (EALayer3), as EA's EALayer3 songs have
it; sample rate and sample count stay, so does every song's length.

Most Wanted (--game mw; TRACKER M7-19, M7-37):

    tools/musicpack.py --game mw <game folder> <out folder> [--quality V] [--jobs N]

writes MW_Music.mus and MW_Music.mpf (SOUND/PFDATA) for the runtime's own
EALayer3 decoder (runtime/mw/ealayer3_mw.c): Most Wanted's SND has the codec's
slot but no decoder in it, so only a module with that decoder plays the pack.
  - MW_Music.mpf is PathFinder version 5: its header holds its sections'
    offsets (0x14..0x38, loader 0x829D80), the last the samples table (3,257
    entries of offset / 0x80 and ms, every stream of the .mus, all named).
  - MW_Music.mus: a 0x100-byte header (FA CE A5 8C, zeros), then 3,257 SCHl
    streams with GSTR headers (version 3, no codec tag: EA-XA), EA-XA v2 at
    36 kHz (3,243 stereo, 14 of four channels); the blocks' fields are
    big-endian.  EA-XA v2 is v1 without the 128 and the clamp, and with frames
    of 28 samples as they are after 0xEE and two history samples; decoded as
    vgmstream decodes it (the same samples, checked on stereo and
    four-channel streams).
Each stream becomes EALayer3 in the format above, with these differences: the
header is the stream's own with codec 0x17 added; its samples stay at 36 kHz,
which LAME is told are 32 kHz (no MPEG rate is 36 kHz, and the decoder never
looks at it: SND plays the stream at its header's rate); a four-channel stream
is two stereo MP3 streams, channels 0+1 and 2+3; blocks hold up to 6 MP3
frames (EA's ADPCM blocks hold 7,224 samples) and are no larger than the
stream's largest original block; every channel's offset names its pair's
frames.  Resampled instead, the songs either lose their top (32 kHz) or take
more bytes for less (44.1 kHz).  The segments of a song are short (EA Trax
songs are chains of 2.4 to 4.7 s segments) and each is encoded on its own:
encoding a run of them in one piece rates no better (-0.02 to +0.04).  PEAQ
(GstPEAQ, advanced) over 7 runs of 19-34 s of consecutive streams (front end,
two EA Trax songs, two pursuit tracks, two others), the original's EA-XA as the
reference (its ADPCM takes 309 kbit/s in stereo):
    -V 0  257 kbit/s on average, -0.03 to -0.09, -0.06 on average
    -V 1  230 kbit/s, -0.08 to -0.19, -0.13
    -V 2  208 kbit/s, -0.16 to -0.27, -0.22
    -V 3  190 kbit/s, -0.27 to -0.46, -0.37
    -V 4  179 kbit/s, -0.36 to -0.54, -0.46
-V 2 is the default (about 328 MB from 534, the size/quality choice made for the
pack); -V 0 is the one as close to the original as Underground 2's pack (-0.07):
534 MB -> 405 MB (76 %), 7 minutes on 12 cores.  The whole pack at -V 0,
decoded by the runtime's decoder (tools/mw/el3_test.c), every stream to the
sample count of its original, against the original, per track (476 runs of
consecutive stereo streams of 20 s or more, and the 14 four-channel streams'
first 40 s, each of their pairs): -0.06 on average, -0.16 at worst, none
under -0.2.
"""
import argparse, array, os, shutil, struct, subprocess, sys
from multiprocessing import Pool

LAME = ['lame', '--quiet', '-r', '-s', '44.1', '--bitwidth', '16', '--signed', '--little-endian',
        '-m', 'j', '-V', '1', '-q', '0', '--nores', '-t', '--noreplaygain', '-', '-']
DELAY = 529 + 576          # what the decoder skips at the start
HEAD = 1152 - DELAY        # 47: frame 0's samples that are left, sent as PCM
TAIL = 100                 # the last samples, sent as PCM (EA's count)
BLOCK_MAX = 2384           # the largest EALayer3 block in EA's file
ALIGN = 0x80               # streams start at a multiple of this (the table stores offset / 0x80)


# --- the map (.mpf) ---------------------------------------------------------

def samples_table(m):
    """(offset of the samples table, entry count), found the way the game's loader (0x73a7c0) does."""
    u8 = lambda o: m[o]
    s16 = lambda o: struct.unpack_from('<h', m, o)[0]
    u32 = lambda o: struct.unpack_from('<I', m, o)[0]
    if m[:4] != b'xDFP' or m[4] != 4:
        raise SystemExit('MusicSFx.mpf: not a PathFinder map, version 4 (little-endian)')
    nodes, num_nodes = 0x20, struct.unpack_from('<H', m, 0x12)[0]
    last = s16(nodes + (num_nodes - 1) * 2) * 4
    events = (last + ((u32(last + 4) >> 12) & 0x1f) * 4 + 0x13) & ~3
    last = s16(events + (u8(0xf) - 1) * 2) * 4
    routers = (last + (u8(last + 0xe) & 0x3f) * 16 + 0x13) & ~3
    tracks = u32(routers + u8(0x10) * 4) * 4
    first, end = tracks + u8(0xd) * 4 + 4, u32(tracks + u8(0xd) * 4) * 4
    if (end - first) % 8 or end > len(m):
        raise SystemExit('MusicSFx.mpf: the samples table is not where the game would find it')
    return first, (end - first) // 8


# --- the streams (.mus) -----------------------------------------------------

def chunks(d, off):
    """The chunks of the SCHl stream at off: [(id, offset, size)], SCHl to SCEl."""
    out = []
    while True:
        cid, sz = d[off:off + 4], struct.unpack_from('<I', d, off + 4)[0]
        if cid not in (b'SCHl', b'SCCl', b'SCDl', b'SCEl') or sz < 8:
            raise SystemExit(f'MusicSFx.mus: no stream chunk at {off:#x}')
        out.append((cid, off, sz))
        off += sz
        if cid == b'SCEl':
            return out


def header_tags(h):
    """SCHl's PT header: [(tag, bytes)], 0xfd/0xfe/0xfc as (tag, None), up to 0xff."""
    if h[:2] != b'PT':
        raise SystemExit('MusicSFx.mus: a stream header without PT')
    i, out = 4, []
    while True:
        t = h[i]; i += 1
        if t == 0xff:
            return out
        if t in (0xfc, 0xfd, 0xfe):
            out.append((t, None)); continue
        n = h[i]; out.append((t, h[i + 1:i + 1 + n])); i += 1 + n


def header_bytes(tags):
    b = bytearray(b'PT\0\0')
    for t, v in tags:
        b.append(t)
        if v is not None:
            b.append(len(v)); b += v
    b.append(0xff)
    while (len(b) + 8) % 4:
        b.append(0)
    return b'SCHl' + struct.pack('<I', len(b) + 8) + bytes(b)


def tagval(tags, t, default=None):
    for k, v in tags:
        if k == t:
            return int.from_bytes(v, 'big')
    return default


# vgmstream's EA_XA_TABLE: coefficient 1 at [i], 2 at [i + 4] (EA's encoders use i = 0..3)
EA_XA_T = [0, 240, 460, 392, 0, 0, -208, -220, 0, 1, 3, 4, 7, 8, 10, 11, 0, -1, -3, -4]
EA_XA = [(EA_XA_T[i], EA_XA_T[i + 4]) for i in range(16)]


def decode_ea_xa(d, ch):
    """EA-XA v1 (split mono per block) of one stream: interleaved s16 samples."""
    tags = header_tags(d[ch[0][1] + 8:ch[0][1] + ch[0][2]])
    chans, total = tagval(tags, 0x82, 1), tagval(tags, 0x85)
    if chans != 2:
        raise SystemExit('MusicSFx.mus: an ADPCM song that is not stereo')
    pcm = [array.array('h') for _ in range(chans)]
    hist = [[0, 0] for _ in range(chans)]
    for cid, off, sz in ch:
        if cid != b'SCDl':
            continue
        # id, size, samples, an offset per channel; each channel's data starts with a history (s16 x 2) that
        # the decoder does not read
        n = struct.unpack_from('<I', d, off + 8)[0]
        frames = (n + 27) // 28
        for c in range(chans):
            p = off + 0x0c + 4 * chans + struct.unpack_from('<I', d, off + 0x0c + 4 * c)[0] + 4
            out, (h1, h2) = pcm[c], hist[c]
            for _ in range(frames):
                info = d[p]
                c1, c2 = EA_XA[info >> 4]
                shift = (info & 15) + 8
                for byte in d[p + 1:p + 15]:
                    for nib in (byte >> 4, byte & 15):
                        s = ((((nib << 28) - ((nib & 8) << 29)) >> shift) + c1 * h1 + c2 * h2 + 128) >> 8
                        s = -32768 if s < -32768 else 32767 if s > 32767 else s
                        out.append(s); h2, h1 = h1, s
                p += 15
            del out[len(out) - (frames * 28 - n):len(out)]
            hist[c] = [h1, h2]
    if any(len(x) != total for x in pcm):
        raise SystemExit('MusicSFx.mus: an ADPCM song whose blocks do not add up to its sample count')
    inter = array.array('h', bytes(4 * total))
    inter[0::2], inter[1::2] = pcm[0], pcm[1]
    return inter


# --- MP3 to EALayer3 --------------------------------------------------------

def mp3_frames(mp3, srate=0):
    """LAME's MPEG-1 layer III frames: [(header fields, scfsi, side[gr][ch] (59 bits), main[gr][ch] (value, bits))];
    srate: the sample-rate index they must have (0 44.1 kHz, 2 32 kHz)."""
    rates = [32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
    out, p = [], 0
    while p < len(mp3):
        h = int.from_bytes(mp3[p:p + 4], 'big')
        if h >> 21 != 0x7ff or (h >> 19) & 3 != 3 or (h >> 17) & 3 != 1 or not (h >> 16) & 1:
            raise SystemExit(f'LAME output: not an MPEG-1 layer III frame without CRC at {p}')
        br, sr, pad, mode, ext = (h >> 12) & 15, (h >> 10) & 3, (h >> 9) & 1, (h >> 6) & 3, (h >> 4) & 3
        if sr != srate or mode == 3 or not 1 <= br <= 14:
            raise SystemExit(f'LAME output: not {(44100, 48000, 32000)[srate]} Hz stereo')
        size = 144000 * rates[br - 1] // (44100, 48000, 32000)[sr] + pad
        f = int.from_bytes(mp3[p:p + size], 'big'); bits = size * 8
        get = lambda pos, n: (f >> (bits - pos - n)) & ((1 << n) - 1)
        pos = 32
        if get(pos, 9):
            raise SystemExit('LAME output: a frame uses the bit reservoir')
        pos += 9 + 3
        scfsi = [get(pos, 4), get(pos + 4, 4)]; pos += 8
        side = [[0, 0], [0, 0]]
        for g in range(2):
            for c in range(2):
                side[g][c] = get(pos, 59); pos += 59
        main = [[0, 0], [0, 0]]
        for g in range(2):
            for c in range(2):
                n = side[g][c] >> 47
                main[g][c] = (get(pos, n), n); pos += n
        if pos > bits:
            raise SystemExit('LAME output: main data past the frame')
        out.append(((3, sr, mode, ext), scfsi, side, main))
        p += size
    return out


def granule_frame(fr, g):
    """One EALayer3 v1 frame: granule g of MP3 frame fr."""
    (vi, sr, mode, ext), scfsi, side, main = fr
    acc, n = (vi << 6) | (sr << 4) | (mode << 2) | ext, 8
    acc, n = (acc << 1) | g, n + 1
    if g:
        acc, n = (acc << 8) | (scfsi[0] << 4) | scfsi[1], n + 8
    for c in range(2):
        acc, n = (acc << 59) | side[g][c], n + 59
    for c in range(2):
        v, k = main[g][c]
        acc, n = (acc << k) | v, n + k
    pad = -n % 8
    return b'\0' + (acc << pad).to_bytes((n + pad) // 8, 'big')


def pcm_plan(total):
    """The PCM frames of a song of total samples, as EA's encoder placed them: [(after granule k, first sample,
    count, offset)].  The head after granule 1; the tail after the last granule, split over the last two when the
    last holds fewer than TAIL of the song's samples (a PCM frame stays within its granule)."""
    last = (total - 1 + DELAY) // 576                   # the granule that holds the last sample
    r = total + DELAY - 576 * last                      # the song's samples in it
    plan = [(1, 0, HEAD, HEAD)]
    if r >= TAIL:
        return plan + [(last, total - TAIL, TAIL, 576 - r + TAIL)]
    return plan + [(last - 1, total - TAIL, TAIL - r, TAIL - r), (last, total - r, r, 576)]


def pcm_frame(pcm, start, count, offset):
    s = array.array('h', pcm[2 * start:2 * (start + count)])
    if sys.byteorder == 'little':
        s.byteswap()
    return b'\xee' + struct.pack('>HH', offset, count) + s.tobytes()


def ealayer3_stream(tags, pcm):
    """The SCHl stream of an ADPCM song's samples (interleaved s16), in EALayer3."""
    total = len(pcm) // 2
    if total < 4 * 1152:
        raise SystemExit('MusicSFx.mus: a song too short for this layout')
    mp3 = subprocess.run(LAME, input=pcm.tobytes(), capture_output=True, check=True).stdout
    frames = mp3_frames(mp3)
    last = (total - 1 + DELAY) // 576                   # the granule that holds the last sample
    need = last // 2 + 1
    if len(frames) < need:
        raise SystemExit('LAME output: fewer frames than the song needs')
    frames = frames[:need]
    pcms = {}
    for k, start, count, offset in pcm_plan(total):
        pcms.setdefault(k, []).append(pcm_frame(pcm, start, count, offset))
    # (granule, bytes) in decoding order; PCM frames carry the granule they follow
    items = []
    for i, fr in enumerate(frames):
        for g in range(2):
            k = 2 * i + g
            items.append((k, granule_frame(fr, g)))
            items += [(k, b) for b in pcms.get(k, [])]
    # blocks of two MP3 frames (four granules), one when two would be too big; the first holds frames 0 and 1
    blocks, i = [], 0
    while i < len(items):
        k0 = items[i][0]
        j = i
        while j < len(items) and items[j][0] < k0 + 4:
            j += 1
        if sum(len(b) for _, b in items[i:j]) + 0x14 + 3 > BLOCK_MAX and k0 > 0:
            j = i
            while j < len(items) and items[j][0] < k0 + 2:
                j += 1
        k1 = items[j - 1][0]
        lo, hi = max(0, 576 * k0 - DELAY), min(total, 576 * (k1 + 1) - DELAY)
        payload = b''.join(b for _, b in items[i:j])
        payload += bytes(-len(payload) % 4)
        blocks.append(b'SCDl' + struct.pack('<IIII', len(payload) + 0x14, hi - lo, 0,
                                            1 if len(blocks) % 2 == 0 else 0x01000000) + payload)
        i = j
    if sum(struct.unpack_from('<I', b, 8)[0] for b in blocks) != total:
        raise SystemExit('internal: block sample counts do not add up')
    head = [(t, b'\x02') if t == 0x80 else (t, v) for t, v in tags if t != 0xa0] + [(0xa0, b'\x17')]
    return (header_bytes(head) + b'SCCl' + struct.pack('<II', 12, len(blocks)) + b''.join(blocks)
            + b'SCEl' + struct.pack('<I', 8))


def encode(job):
    """One ADPCM song (path, offset, end) as its EALayer3 stream."""
    path, off, end = job
    with open(path, 'rb') as f:
        f.seek(off)
        d = f.read(end - off)
    ch = chunks(d, 0)
    return ealayer3_stream(header_tags(d[8:ch[0][2]]), decode_ea_xa(d, ch))



# --- Most Wanted (--game mw) ------------------------------------------------

MW_FRAMES = 6               # MP3 frames a block at most (6912 samples; EA's ADPCM blocks hold 7224)


def mw_samples_table(m):
    """(offset of the samples table, entry count) of MW_Music.mpf: PathFinder version 5 keeps its sections' offsets in
    the header (0x14..0x38, read by the loader 0x829D80); the samples table is the last, up to the file's end."""
    if m[:4] != b'xDFP' or m[4] != 5 or m[5] != 1:
        raise SystemExit('MW_Music.mpf: not a PathFinder map, version 5 (little-endian)')
    first, end = struct.unpack_from('<II', m, 0x34)
    if first < 0x3c or (end - first) % 8 or end != len(m):
        raise SystemExit('MW_Music.mpf: the samples table is not where the game would find it')
    return first, (end - first) // 8


def gstr_tags(h):
    """SCHl's GSTR header: (its 4 bytes after GSTR, [(tag, bytes)]), 0xfd/0xfe/0xfc as (tag, None), up to 0xff."""
    if h[:4] != b'GSTR':
        raise SystemExit('MW_Music.mus: a stream header without GSTR')
    i, out = 8, []
    while True:
        t = h[i]; i += 1
        if t == 0xff:
            return h[4:8], out
        if t in (0xfc, 0xfd, 0xfe):
            out.append((t, None)); continue
        n = h[i]; out.append((t, h[i + 1:i + 1 + n])); i += 1 + n


def gstr_bytes(word, tags):
    b = bytearray(b'GSTR' + word)
    for t, v in tags:
        b.append(t)
        if v is not None:
            b.append(len(v)); b += v
    b.append(0xff)
    while (len(b) + 8) % 4:
        b.append(0)
    return b'SCHl' + struct.pack('<I', len(b) + 8) + bytes(b)


def decode_ea_xa_v2(d, ch, chans, total):
    """EA-XA v2 of one stream (each channel's data at its offset in a block, the block's fields big-endian; a frame is
    an ADPCM frame or, after 0xEE, two history samples and 28 samples as they are), as vgmstream decodes it: a list of
    s16 sample arrays, one per channel."""
    pcm = [array.array('h') for _ in range(chans)]
    hist = [(0, 0)] * chans
    for cid, off, sz in ch:
        if cid != b'SCDl':
            continue
        n = struct.unpack_from('>I', d, off + 8)[0]
        frames = (n + 27) // 28
        for c in range(chans):
            p = off + 0x0c + 4 * chans + struct.unpack_from('>I', d, off + 0x0c + 4 * c)[0]
            out, (h1, h2) = pcm[c], hist[c]
            for _ in range(frames):
                info = d[p]
                if info == 0xee:
                    h1, h2 = struct.unpack_from('>hh', d, p + 1)
                    out.extend(struct.unpack_from('>28h', d, p + 5))
                    p += 61
                    continue
                c1, c2 = EA_XA[info >> 4]
                shift = (info & 15) + 8
                for byte in d[p + 1:p + 15]:
                    for nib in (byte >> 4, byte & 15):
                        s = ((((nib << 28) - ((nib & 8) << 29)) >> shift) + c1 * h1 + c2 * h2) >> 8
                        s = -32768 if s < -32768 else 32767 if s > 32767 else s
                        out.append(s); h2, h1 = h1, s
                p += 15
            del out[len(out) - (frames * 28 - n):len(out)]
            hist[c] = (h1, h2)
    if any(len(x) != total for x in pcm):
        raise SystemExit('MW_Music.mus: a stream whose blocks do not add up to its sample count')
    return pcm


def mw_pair_frames(left, right, quality):
    """One stereo MP3 stream of a pair of channels (s16 arrays) as granule items: [(granule, bytes)].  LAME is told
    32 kHz for the 36 kHz samples: the decoder never looks at the rate (SND plays the stream at its header's), and
    resampled the songs lose either their top (32 kHz) or more bytes than they save (44.1 kHz) (the docstring)."""
    total = len(left)
    inter = array.array('h', bytes(4 * total))
    inter[0::2], inter[1::2] = left, right
    lame = list(LAME)
    lame[lame.index('-s') + 1] = '32'
    lame[lame.index('-V') + 1] = quality
    frames = mp3_frames(subprocess.run(lame, input=inter.tobytes(), capture_output=True, check=True).stdout, srate=2)
    last = (total - 1 + DELAY) // 576
    need = last // 2 + 1
    if len(frames) < need:
        raise SystemExit('LAME output: fewer frames than the stream needs')
    pcms = {}
    for k, start, count, offset in pcm_plan(total):
        pcms.setdefault(k, []).append(pcm_frame(inter, start, count, offset))
    items = []
    for i, fr in enumerate(frames[:need]):
        for g in range(2):
            k = 2 * i + g
            items.append((k, granule_frame(fr, g)))
            items += [(k, b) for b in pcms.get(k, [])]
    return items


def mw_encode(job):
    """One stream (path, offset, end, LAME's -V) as its EALayer3 stream."""
    path, off, end, quality = job
    with open(path, 'rb') as f:
        f.seek(off)
        d = f.read(end - off)
    ch = chunks(d, 0)
    word, tags = gstr_tags(d[8:ch[0][2]])
    chans, total = tagval(tags, 0x82, 1), tagval(tags, 0x85)
    if tagval(tags, 0xa0, 0x0a) != 0x0a or chans not in (2, 4):
        raise SystemExit(f'MW_Music.mus: a stream at {off:#x} in a codec or layout this does not know')
    if total < 4 * 1152:
        raise SystemExit('MW_Music.mus: a stream too short for this layout')
    pcm = decode_ea_xa_v2(d, ch, chans, total)
    pairs = [mw_pair_frames(pcm[2 * p], pcm[2 * p + 1], quality) for p in range(chans // 2)]
    # each MP3 frame's bytes (granule 0, its PCM frames, granule 1, its PCM frames), per pair
    nframes = max(k for k, _ in pairs[0]) // 2 + 1
    per = [[b''] * nframes for _ in pairs]
    for p, items in enumerate(pairs):
        for k, b in items:
            per[p][k // 2] += b
    # blocks of up to MW_FRAMES frames, none larger than EA's largest block of this stream; both pairs alike; a block's
    # channel offsets name its pair's frames (both channels of a pair the same)
    cap = max(sz for cid, _, sz in ch if cid == b'SCDl')
    size = lambda a, b: 0x0c + 4 * chans + sum((sum(len(x) for x in per[p][a:b]) + 3) & ~3 for p in range(len(per)))
    blocks, f0 = [], 0
    while f0 < nframes:
        f1 = min(nframes, f0 + MW_FRAMES)
        while f1 - f0 > 1 and size(f0, f1) > cap:
            f1 -= 1
        lo, hi = max(0, 1152 * f0 - DELAY), min(total, 1152 * f1 - DELAY)
        datas = [b''.join(per[p][f0:f1]) for p in range(len(per))]
        datas = [x + bytes(-len(x) % 4) for x in datas]
        offs, at = [], 0
        for x in datas:
            offs += [at, at]; at += len(x)
        payload = struct.pack('>I', hi - lo) + struct.pack(f'>{chans}I', *offs) + b''.join(datas)
        blocks.append(b'SCDl' + struct.pack('<I', len(payload) + 8) + payload)
        f0 = f1
    if sum(struct.unpack_from('>I', b, 8)[0] for b in blocks) != total:
        raise SystemExit('internal: block sample counts do not add up')
    head = [(t, v) for t, v in tags if t != 0xa0] + [(0xa0, b'\x17')]
    return (gstr_bytes(word, head) + b'SCCl' + struct.pack('<I', 12) + struct.pack('>I', len(blocks))
            + b''.join(blocks) + b'SCEl' + struct.pack('<I', 8))


def mw_main(a):
    src, names = None, {}
    for root, dirs, files in os.walk(a.game):
        names = {n.lower(): n for n in files}
        if 'mw_music.mus' in names and 'mw_music.mpf' in names:
            src = root
            break
    if not src:
        raise SystemExit('no MW_Music.mus and MW_Music.mpf (SOUND/PFDATA) in the game folder')
    mus_path, mpf_path = (os.path.join(src, names[n]) for n in ('mw_music.mus', 'mw_music.mpf'))
    m = bytearray(open(mpf_path, 'rb').read())
    with open(mus_path, 'rb') as f:
        d = f.read()
    table, count = mw_samples_table(m)
    entries = [struct.unpack_from('<II', m, table + 8 * i) for i in range(count)]
    offs = sorted({o * ALIGN for o, _ in entries})
    if offs != chunks_all(d, offs[0]):
        raise SystemExit('MW_Music.mus: streams the map does not name')
    end = {}
    for o in offs:
        ch = chunks(d, o)
        end[o] = ch[-1][1] + ch[-1][2]
    del d
    print(f'{count} songs and segments in {len(offs)} streams, LAME -V {a.quality}', file=sys.stderr)
    os.makedirs(a.out, exist_ok=True)
    tmp = os.path.join(a.out, 'MW_Music.mus.tmp')
    new = {}
    with open(mus_path, 'rb') as f, open(tmp, 'wb') as out, Pool(a.jobs) as pool:
        out.write(f.read(offs[0]))                  # the file's header (FA CE A5 8C, zeros)
        jobs = [(mus_path, o, end[o], a.quality) for o in offs]
        for o, e in zip(offs, pool.imap(mw_encode, jobs, chunksize=8)):
            new[o] = out.tell() // ALIGN
            out.write(e + bytes(-(out.tell() + len(e)) % ALIGN))
        total = out.tell()
    for i, (o, ms) in enumerate(entries):
        struct.pack_into('<II', m, table + 8 * i, new[o * ALIGN], ms)
    os.replace(tmp, os.path.join(a.out, 'MW_Music.mus'))
    with open(os.path.join(a.out, 'MW_Music.mpf.tmp'), 'wb') as f:
        f.write(m)
    os.replace(os.path.join(a.out, 'MW_Music.mpf.tmp'), os.path.join(a.out, 'MW_Music.mpf'))
    size = os.path.getsize(mus_path)
    print(f'MW_Music.mus: {size} -> {total} bytes ({total / size:.1%})', file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('game', help="the game folder (with pfdata/ in it; Most Wanted's with SOUND/PFDATA/)")
    ap.add_argument('out', help='where MusicSFx.mus and MusicSFx.mpf go')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument('--game', dest='which', choices=('ug2', 'mw'), default='ug2',
                    help="mw: Most Wanted's SOUND/PFDATA/MW_Music.mus and .mpf, for the runtime's decoder")
    ap.add_argument('--quality', default='2', help="mw: LAME's VBR quality, -V (default 2: the docstring's PEAQ)")
    a = ap.parse_args()
    if sys.byteorder != 'little':
        raise SystemExit('a little-endian machine only (the samples go to LAME as they are in memory)')
    if not shutil.which('lame'):
        raise SystemExit('needs LAME 3.100 on the PATH (apt install lame)')
    version = subprocess.run(['lame', '--version'], capture_output=True, text=True).stdout.split('\n')[0]
    if '3.100' not in version:
        print(f'warning: {version.strip()}; the bytes were made with LAME 3.100', file=sys.stderr)
    if a.which == 'mw':
        return mw_main(a)
    src = os.path.join(a.game, 'pfdata')
    names = {n.lower(): n for n in os.listdir(src)}
    mus_path, mpf_path = (os.path.join(src, names[n]) for n in ('musicsfx.mus', 'musicsfx.mpf'))
    m = bytearray(open(mpf_path, 'rb').read())
    with open(mus_path, 'rb') as f:
        d = f.read()
    table, count = samples_table(m)
    entries = [struct.unpack_from('<II', m, table + 8 * i) for i in range(count)]
    offs = sorted({o * ALIGN for o, _ in entries})     # the streams the songs use, in file order
    kind, end = {}, {}
    for o in offs:
        ch = chunks(d, o)
        end[o] = ch[-1][1] + ch[-1][2]
        tags = header_tags(d[o + 8:o + ch[0][2]])
        codec = tagval(tags, 0xa0)
        if codec == 0x17:
            kind[o] = 'copy'
        elif codec is None and tagval(tags, 0x80) == 1 and tagval(tags, 0x83) is None:
            kind[o] = 'encode'
        else:
            raise SystemExit(f'MusicSFx.mus: a song at {o:#x} in a codec this does not know')
    todo = [o for o in offs if kind[o] == 'encode']
    print(f'{count} songs: {len(offs) - len(todo)} in EALayer3 kept, {len(todo)} in ADPCM to encode, '
          f'{len(chunks_all(d)) - len(offs)} unused streams dropped', file=sys.stderr)
    with Pool(a.jobs) as pool:
        encoded = dict(zip(todo, pool.map(encode, [(mus_path, o, end[o]) for o in todo], chunksize=1)))
    out, new = bytearray(), {}
    for o in offs:
        new[o] = len(out) // ALIGN
        out += d[o:end[o]] if kind[o] == 'copy' else encoded[o]
        out += bytes(-len(out) % ALIGN)
    for i, (o, ms) in enumerate(entries):
        struct.pack_into('<II', m, table + 8 * i, new[o * ALIGN], ms)
    # the music's stream buffer (0x73aea0): from the distance between the last two entries, which must not change
    gap = lambda es: ((es[-1][0] - es[-2][0]) * ALIGN) & 0xffffffff
    if gap([(new[o * ALIGN], ms) for o, ms in entries]) != gap(entries):
        raise SystemExit('the last two songs moved apart: the game would size its music buffer differently')
    os.makedirs(a.out, exist_ok=True)
    for name, data in (('MusicSFx.mus', out), ('MusicSFx.mpf', m)):
        tmp = os.path.join(a.out, name + '.tmp')
        with open(tmp, 'wb') as f:
            f.write(data)
        os.replace(tmp, os.path.join(a.out, name))
    print(f'MusicSFx.mus: {len(d)} -> {len(out)} bytes ({len(out) / len(d):.1%})', file=sys.stderr)


def chunks_all(d, off=0):
    """Every SCHl stream's offset from off on (they follow each other, zeros between them)."""
    out = []
    while off < len(d):
        if d[off:off + 4] != b'SCHl':
            raise SystemExit(f'MusicSFx.mus: no stream at {off:#x}')
        out.append(off)
        ch = chunks(d, off)
        off = ch[-1][1] + ch[-1][2]
        off += -off % ALIGN
        while off < len(d) and not any(d[off:off + ALIGN]):
            off += ALIGN
    return out


if __name__ == '__main__':
    main()
