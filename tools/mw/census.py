#!/usr/bin/env python3
"""Most Wanted's content census, read from the game's own data (TRACKER M4-37).

    tools/mw/census.py "<game folder>" [LOG ...]

Prints markdown tables, read at run time from the game folder (nothing of the game is kept here):
  - GLOBAL/gameplay.bin, a VPAK of 272 Attrib vaults: the race bins race_bin_01..15 (the Blacklist: each bin's
    boss collection, its races by kind, its engage list, reward cards and unlocks), the event vaults by kind, the
    Challenge Series bins;
  - GLOBAL/attributes.bin (one vault, "db"): the pvehicle class's collections (cars, cops, traffic) by parent;
  - MOVIES and NIS: the file counts;
  - with LOGs of runs made with NFSU2_FNG_LOG=3 that stepped a Quick Race track list (Track_Select.fng): each
    list's tracks, their names from LANGUAGES/English.bin, their lengths and whether the list offers them (its
    Accept button, label 0x2B65A216, drawn) or shows them locked.

Formats (speed.exe v1.3's data; read here, checked against the game's own screens):
  VPAK: 'VPAK', count, string block offset and size, then per vault {name offset, bin size, vlt size, bin offset,
    vlt offset}.  A vault's vlt and bin parts are chunks {fourcc reversed, size}: DepN, StrN, DatN, ExpN, PtrN
    (vlt) and StrE (bin: the collections' names, "race_bin_15/15_1_1_circuit").
  ExpN: count, then {key, type, 0, size, offset}; a collection (type 0x8E112EB7) at offset in the vlt part:
    key, class, parent, reserve, shift, entry count, type count (low half), layout pointer, the types, then the
    entries {key, value, type index (u16), flags (u16): 0x20 = the value is inline, else PtrN points it into the
    bin part}.  PtrN: {vlt offset, type (u16), u16, bin offset}.  An array in the bin part: u16 count, u16 cap,
    u32 element size, the elements (the Children field 0x916E0E78 is one of collection keys).
  Keys are Attrib::StringHash32 (Jenkins' lookup2, init 0xABCDEF00) of the full collection name; most field names
    are not in the data, so a field without a name is shown by its key.
  English.bin: chunk {id, size}, then {header size, count, table offset, strings offset} and {hash, offset} pairs;
    hashes are the FE labels the screens show (bStringHash).
"""
import collections, os, re, struct, sys

M = 0xffffffff


def _mix(a, b, c):
    a = (a - b - c) & M; a ^= c >> 13
    b = (b - c - a) & M; b ^= (a << 8) & M
    c = (c - a - b) & M; c ^= b >> 13
    a = (a - b - c) & M; a ^= c >> 12
    b = (b - c - a) & M; b ^= (a << 16) & M
    c = (c - a - b) & M; c ^= b >> 5
    a = (a - b - c) & M; a ^= c >> 3
    b = (b - c - a) & M; b ^= (a << 10) & M
    c = (c - a - b) & M; c ^= b >> 15
    return a, b, c


def h32(s, init=0xABCDEF00):
    """Attrib::StringHash32 (lookup2): h32("default") == 0xEEC2271A, h32("pvehicle") == 0x4A97EC8F"""
    k = s.encode('latin1'); n = len(k); a = b = 0x9E3779B9; c = init; i = 0
    while n - i >= 12:
        x, y, z = struct.unpack_from('<3I', k, i)
        a, b, c = _mix((a + x) & M, (b + y) & M, (c + z) & M); i += 12
    t = k[i:] + bytes(12); r = n - i; c = (c + n) & M
    for j in range(r - 1, -1, -1):
        if j >= 8: c = (c + (t[j] << (8 * (j - 7)))) & M
        elif j >= 4: b = (b + (t[j] << (8 * (j - 4)))) & M
        else: a = (a + (t[j] << (8 * j))) & M
    return _mix(a, b, c)[2]


def vpak(path):
    d = open(path, 'rb').read()
    _, n, soff, _ = struct.unpack_from('<4sIII', d, 0)
    out = {}
    for i in range(n):
        no, bs, vs, bo, vo = struct.unpack_from('<5I', d, 16 + 20 * i)
        name = d[soff + no:d.index(b'\0', soff + no)].decode('latin1')
        out[name] = (d[bo:bo + bs], d[vo:vo + vs])
    return out


def chunks(b):
    o, out = 0, {}
    while o + 8 <= len(b):
        t, s = struct.unpack_from('<4sI', b, o)
        if s < 8: break
        out[t[::-1].decode('latin1')] = b[o + 8:o + s]; o += s
    return out


def collections_of(bn, vl):
    """[(key, class, parent, {field key: (value, type, flags, (ptr type, bin offset) or None)})]"""
    cv = chunks(vl); ex = cv['ExpN']; p = cv['PtrN']
    ptr = {}
    for i in range(len(p) // 12):
        a, t, _, b = struct.unpack_from('<IHHI', p, 12 * i); ptr[a] = (t, b)
    out = []
    for i in range(struct.unpack_from('<I', ex, 0)[0]):
        _, typ, _, _, off = struct.unpack_from('<5I', ex, 4 + 20 * i)
        if typ != 0x8E112EB7: continue
        key, cls, par, _, _, ne, nt = struct.unpack_from('<7I', vl, off); nt &= 0xffff
        tys = struct.unpack_from('<%dI' % nt, vl, off + 0x20); eo = off + 0x20 + 4 * nt
        ents = {}
        for j in range(ne):
            ek, ev, eti, efl = struct.unpack_from('<IIHH', vl, eo + 12 * j)
            ents[ek] = (ev, tys[eti] if eti < nt else 0, efl, ptr.get(eo + 12 * j + 4))
        out.append((key, cls, par, ents))
    return out


def names_of(vaults, extra=()):
    H = {}
    for nm, (bn, _) in vaults.items():
        H[h32(nm)] = nm
        for s in chunks(bn).get('StrE', b'').split(b'\0'):
            if s: s = s.decode('latin1'); H[h32(s)] = s
    for s in extra: H.setdefault(h32(s), s)
    return H


def language(path):
    d = open(path, 'rb').read()
    _, n, to, so = struct.unpack_from('<4I', d, 8)
    T = {}
    for i in range(n):
        h, o = struct.unpack_from('<II', d, 8 + to + 8 * i)
        s = d[8 + so + o:]; T[h] = s[:s.index(b'\0')].decode('latin1')
    return T


CHILDREN = 0x916E0E78
KINDS = {'circuit': 'circuit', 'p2p': 'sprint', 'lapknockout': 'lap knockout', 'tollboothrace': 'tollbooth',
         'speedtraprace': 'speedtrap', 'drag': 'drag'}


def array(bn, tgt):
    n = struct.unpack_from('<H', bn, tgt[1])[0]
    return [struct.unpack_from('<I', bn, tgt[1] + 8 + 4 * i)[0] for i in range(n)]


def gameplay(G):
    V = vpak(os.path.join(G, 'GLOBAL', 'gameplay.bin')); H = names_of(V)
    leaf = lambda k: H.get(k, '0x%08X' % k).split('/')[-1]
    f32 = lambda v: struct.unpack('<f', struct.pack('<I', v))[0]
    print('## The Blacklist race bins (GLOBAL/gameplay.bin, vaults race_bin_01..15)\n')
    print('A race is a top-level collection of the bin whose parent is a race kind; the engage list is the bin\'s '
          '"engage" collection\'s Children (the events offered while that rival is next); rewards and unlocks are '
          'the Children of its "rewards" and "unlocks" folders.  The boss collection\'s two fields have no name '
          'in the data: 0x3B798AA2 (an int) and 0xB1ECE070 (a float).\n')
    print('| bin | boss | 0x3B798AA2 | 0xB1ECE070 | races by kind | engage | reward cards | unlocks |')
    print('|---|---|---|---|---|---|---|---|')
    total = collections.Counter()
    for nm in sorted(v for v in V if re.match(r'race_bin_\d\d$', v)):
        bn, vl = V[nm]; kinds = collections.Counter(); boss = bi = bf = ''; eng = 0; rew = unl = []
        for key, _, par, ents in collections_of(bn, vl):
            n = H.get(key, ''); pn = leaf(par)
            if pn in KINDS and n.count('/') == 1: kinds[KINDS[pn]] += 1; total[KINDS[pn]] += 1
            if pn == 'boss':
                boss = n.split('/')[-1]
                if 0x3B798AA2 in ents: bi = str(ents[0x3B798AA2][0])
                if 0xB1ECE070 in ents: bf = '%.2f' % f32(ents[0xB1ECE070][0])
            if CHILDREN in ents and ents[CHILDREN][3]:
                ch = [leaf(k) for k in array(bn, ents[CHILDREN][3])]
                if n.endswith('/engage'): eng = len(ch)
                elif n.endswith('/rewards'): rew = [c.replace('reward_', '') for c in ch]
                elif n.endswith('/unlocks'): unl = [c.replace('upgrade_', '') for c in ch]
        print('| %s | %s | %s | %s | %s | %d | %s | %s |' % (nm[-2:], boss, bi, bf,
              ', '.join('%s %d' % kv for kv in sorted(kinds.items())), eng, ' '.join(rew), ' '.join(unl)))
    print('\nRaces in the 15 bins by kind: %s\n' % ', '.join('%s %d' % kv for kv in sorted(total.items())))
    print('## The other bins\n')
    for nm in ('race_bin_challenge', 'race_bin_challenge_tollbooth', 'race_bin_collectors', 'race_bin_opm'):
        bn, vl = V[nm]; kinds = collections.Counter(); items = []
        for key, _, par, _ in collections_of(bn, vl):
            n = H.get(key, ''); pn = leaf(par)
            if n.count('/') == 1 and (pn.startswith('challengerace') or pn in KINDS or pn == 'boss'):
                kinds[pn] += 1; items.append(n.split('/')[-1])
        print('- %s: %d (%s): %s' % (nm, sum(kinds.values()), ', '.join('%s %d' % kv for kv in sorted(kinds.items())),
                                     ' '.join(sorted(items))))
    print('\n## The vaults by name (VPAK directory: %d vaults)\n' % len(V))
    cat = collections.defaultdict(list)
    kind_digit = {'1': 'circuit', '2': 'sprint', '3': 'lap knockout', '4': 'tollbooth', '5': 'speedtrap', '7': 'drag',
                  '8': 'pursuit race'}
    for nm in V:
        m = re.match(r'(\d+)_(\d)_', nm)
        if m: cat['event, kind digit %s (%s)' % (m.group(2), kind_digit.get(m.group(2), '?'))].append(nm)
        elif nm.lower().startswith('challenge_tollbooth'): cat['Challenge Series tollbooth'].append(nm)
        elif nm.startswith('challenge_'): cat['Challenge Series, other'].append(nm)
        else: cat['other'].append(nm)
    for k, v in sorted(cat.items()): print('- %s: %d' % (k, len(v)))


def cars(G):
    V = vpak(os.path.join(G, 'GLOBAL', 'attributes.bin'))
    (bn, vl), = V.values()
    exe = os.path.join(G, 'speed.exe')
    words = set(re.findall(rb'[A-Za-z0-9_]{2,40}', open(exe, 'rb').read())) if os.path.exists(exe) else set()
    cand = set()
    for w in [x.decode('latin1') for x in words] + [s.decode('latin1') for s in chunks(bn).get('StrE', b'').split(b'\0') if s]:
        p = w.split('_')
        for i in range(1, len(p) + 1): cand.add('_'.join(p[:i]))
    H = names_of(V, cand)
    tree = collections.defaultdict(list)
    for key, cls, par, _ in collections_of(bn, vl):
        if cls == 0x4A97EC8F: tree[H.get(par, '(root)')].append(H.get(key, '0x%08X' % key))
    print('\n## Vehicles (GLOBAL/attributes.bin, class pvehicle: %d collections, by parent)\n'
          % sum(len(v) for v in tree.values()))
    for p, kids in sorted(tree.items()): print('- %s (%d): %s' % (p, len(kids), ' '.join(sorted(kids))))


def quick_race(G, logs):
    T = language(os.path.join(G, 'LANGUAGES', 'English.bin'))
    seen = {}
    for path in logs:
        mode = None
        for line in open(path, encoding='latin1'):
            m = re.search(r'LOG \S+ mode (\w+)', line)
            if m: mode = m.group(1); continue
            if 'Track_Select.fng shows' not in line: continue
            toks = re.findall(r":([0-9a-f]{8})'(\S*)", line)
            title = trk = length = None; nums = []
            for l, t in toks:
                if t.endswith('track_select') and title is None: title = l
                elif title and trk is None and l != title and t.startswith('Track'): trk = l
                if l == '00000018': nums.append(t)
                if t.endswith('Miles'): length = t.replace('_', ' ')
            if not (title and trk and len(nums) >= 2): continue
            ent = seen.setdefault((title, trk), [int(nums[-2]), int(nums[-1]), length, set()])
            ent[3].add(any(l == '2b65a216' for l, _ in toks))
    lists = collections.defaultdict(list)
    for (title, trk), (i, n, length, acc) in seen.items(): lists[title].append((i, n, trk, length, acc))
    for title, rows in lists.items():
        rows.sort(); opened = sum(1 for r in rows if r[4] == {True})
        print('\n## %s (label 0x%s): %d of %d tracks seen, %d offered\n' % (T.get(int(title, 16), '?'), title.upper(),
              len(rows), rows[0][1], opened))
        print('| # | track | label | length | offered |'); print('|---|---|---|---|---|')
        for i, n, trk, length, acc in rows:
            print('| %d | %s | 0x%s | %s | %s |' % (i, T.get(int(trk, 16), '?'), trk.upper(), length,
                                                  'yes' if acc == {True} else 'locked' if acc == {False} else 'both'))


if __name__ == '__main__':
    if len(sys.argv) < 2 or not os.path.isdir(sys.argv[1]):
        sys.exit(__doc__)
    G = sys.argv[1]
    gameplay(G); cars(G)
    print('\n## Files\n')
    for d in ('MOVIES', 'NIS'):
        fs = os.listdir(os.path.join(G, d))
        print('- %s: %d files (%s)' % (d, len(fs), ', '.join('%s %d' % kv for kv in
              sorted(collections.Counter(os.path.splitext(f)[1].lower() for f in fs).items()))))
    if sys.argv[2:]: quick_race(G, sys.argv[2:])
