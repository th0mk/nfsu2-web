"""Differential test of the two M5-01 dumpers on synthetic fx_2_0 effects.

  .venv/bin/python -I tools/mw/fxdiff_synth.py <checkout> <count> [seed]

Builds valid containers that use every feature of fx2-format.md (structs,
arrays, nested sampler blocks, unknown types and classes, every usage,
random shader and preshader blobs) and compares A's and B's dumps.
"""
import os, random, struct, subprocess, sys, importlib.util, collections

W = sys.argv[1]
N = int(sys.argv[2])
rnd = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
HERE = os.path.join(W, 'out', 'mw', 'fx', 'diff')
OUTD = os.path.join(HERE, 'synth_cases')
os.makedirs(OUTD, exist_ok=True)
TMP = os.path.join(HERE, 'SYN.fxo')
spec = importlib.util.spec_from_file_location('fx2dump', os.path.join(W, 'tools', 'fx2dump.py'))
B = importlib.util.module_from_spec(spec)
spec.loader.exec_module(B)
A = os.path.join(W, 'out', 'mw', 'fx', 'bin', 'fxdump')


def dw(*v):
    return struct.pack('<%dI' % len(v), *[x & 0xFFFFFFFF for x in v])


class Gen:
    def __init__(self):
        self.data = bytearray(dw(0))   # body +0 is never referenced
        self.nobj = 1
        self.max_depth = rnd.choice([1, 2, 3])

    def put(self, b):
        off = len(self.data)
        self.data += b
        while len(self.data) % 4:
            self.data.append(rnd.randrange(256))   # nonzero pad, as in MW
        return off

    def name(self):
        if rnd.random() < 0.15:
            return 0
        s = bytes(rnd.choice(b'abcXYZ_09"\\ \x01\x7f\xe9') for _ in range(rnd.randint(0, 8)))
        if rnd.random() < 0.9:
            s += b'\0'
        if rnd.random() < 0.1:
            s += b'tail\0'
        return self.put(dw(len(s)) + s)

    def typedef(self, depth=0, allow_struct=True):
        """(bytes of the typedef, its description) -- the typedef is placed by the caller."""
        cls = rnd.choice([0, 1, 2, 3, 4, 4, 4] + ([5] if allow_struct and depth < self.max_depth else []) + [6, 9])
        if cls == 4:
            typ = rnd.choice([4, 5, 7, 9, 10, 12, 14, 15, 16, 0, 19, 23])
        else:
            typ = rnd.choice([1, 2, 3, 3, 0, 5, 21])
        elems = rnd.choice([0, 0, 0, 1, 2, 3])
        b = dw(typ, cls, self.name(), self.name(), elems)
        d = {'type': typ, 'cls': cls, 'elems': elems}
        if cls in (0, 1, 2, 3):
            a, c = rnd.choice([(1, 1), (4, 1), (1, 4), (4, 4), (3, 1), (2, 3), (0, 2), (1, 0)])
            b += dw(a, c)
            d['per'] = a * c
        elif cls == 5:
            m = rnd.randint(0, 3)
            b += dw(m)
            d['members'] = []
            for _ in range(m):
                mb, md = self.typedef(depth + 1)
                b += mb
                d['members'].append(md)
        return b, d

    def value(self, d, depth):
        """bytes of a value for typedef description d"""
        out = b''
        for _ in range(max(1, d['elems'])):
            out += self.element(d, depth)
        return out

    def element(self, d, depth):
        cls, typ = d['cls'], d['type']
        if cls in (0, 1, 2, 3):
            return b''.join(dw(rnd.choice([0, 1, -1, 0x3f800000, 0x7fc00000, 0x80000000, 0xff800000, 0x00000001,
                                           rnd.getrandbits(32)])) for _ in range(d['per']))
        if cls == 4 and typ in (10, 11, 12, 13, 14):
            n = rnd.randint(0, 3) if depth < self.max_depth else 0
            b = dw(n)
            for _ in range(n):
                b += self.state_entry(depth + 1)
            return b
        if cls == 4:
            oid = self.nobj if rnd.random() < 0.9 else rnd.choice([0, 999])
            if oid == self.nobj:
                self.nobj += 1
            self.objtype[oid] = typ
            return dw(oid)
        if cls == 5:
            return b''.join(self.value(md, depth + 1) for md in d['members'])
        return b''

    def typed_value(self, depth, allow_struct=True):
        tb, d = self.typedef(depth, allow_struct)
        toff = self.put(tb)
        voff = self.put(self.value(d, depth)) if (d['cls'] != 6 and d['cls'] != 9) or rnd.random() < 0.5 else len(self.data)
        return toff, voff, d

    def state_entry(self, depth, with_desc=False):
        op = rnd.choice(list(range(0, 182)) + [182, 300, 0xFFFFFFFF])
        idx = rnd.choice([0, 1, 7, 256, 0xFFFFFFFF])
        toff, voff, d = self.typed_value(depth)
        return (dw(op, idx, toff, voff), d) if with_desc else dw(op, idx, toff, voff)


def comment(fourcc, payload_dw):
    return dw(0xFFFE | (len(payload_dw) + 1) << 16) + fourcc + b''.join(dw(x) for x in payload_dw)


def ctab_payload(version):
    # header 28 bytes then strings and entries; offsets from the payload's start
    nconst = rnd.randint(0, 3)
    body = bytearray()
    strings = {}

    def s(txt):
        off = 28 + len(body)
        body.extend(txt + b'\0')
        while len(body) % 4:
            body.append(0xAB)
        return off
    creator = s(b'Synth compiler')
    target = s(rnd.choice([b'vs_1_1', b'ps_2_b', b'tx', b'']))
    tinfos = []
    for _ in range(nconst):
        off = 28 + len(body)
        body.extend(struct.pack('<6HI', rnd.choice([0, 1, 2, 3, 4, 5, 9]), rnd.choice([1, 2, 3, 12, 14, 30]),
                                rnd.randint(0, 4), rnd.randint(0, 4), rnd.randint(1, 5), 0, 0))
        tinfos.append(off)
    names = [s(b'c%d' % k) for k in range(nconst)]
    cinfo = 28 + len(body)
    for k in range(nconst):
        ti = tinfos[k] if rnd.random() < 0.9 else rnd.choice([0x7fff, len(body) + 20])
        nm = names[k] if rnd.random() < 0.9 else rnd.choice([0, 0x8000])
        body.extend(struct.pack('<I4H2I', nm, rnd.choice([0, 1, 2, 3, 3, 7]), rnd.randint(0, 20), rnd.randint(1, 5), 0,
                                ti, rnd.choice([0, 4])))
    if rnd.random() < 0.1:
        nconst += 2         # entries past the table
    hdr = struct.pack('<7I', 28, creator, version, nconst, cinfo, rnd.choice([0x100, 0x120, 0]), target)
    raw = hdr + bytes(body)
    return [x for x in struct.unpack('<%dI' % (len(raw) // 4), raw)]


def shader_blob():
    ver = rnd.choice([0xFFFE0101, 0xFFFF0101, 0xFFFF0104, 0xFFFF0200, 0xFFFF0201, 0xFFFE0300, 0xFFFF0300, 0x12340000])
    major = ver >> 8 & 0xFF
    out = [ver]
    b = dw(ver)
    for _ in range(rnd.randint(0, 2)):
        kind = rnd.choice(['CTAB', 'CTAB', 'PRES', 'XXXX', 'empty'])
        if kind == 'CTAB':
            b += comment(b'CTAB', ctab_payload(ver))
        elif kind == 'empty':
            b += dw(0xFFFE)
        else:
            b += comment(kind.encode(), [rnd.getrandbits(32) for _ in range(rnd.randint(0, 3))])
    for _ in range(rnd.randint(0, 6)):
        op = rnd.choice([0x01, 0x02, 0x42, 0x51, 0x30, 0x2F, 0x1F, 0xFFFD, 0x05])
        nparams = {0x51: 5, 0x30: 5, 0x2F: 2}.get(op, rnd.randint(0, 3))
        if major >= 2:
            tok = op | (nparams << 24)
        else:
            tok = op
        b += dw(tok)
        for k in range(nparams):
            if op in (0x51, 0x30, 0x2F) and k > 0:
                b += dw(rnd.getrandbits(32))
            else:
                b += dw(0x80000000 | rnd.getrandbits(31))
        if rnd.random() < 0.05:
            b += comment(b'CTAB', ctab_payload(ver))
    if rnd.random() < 0.9:
        b += dw(0x0000FFFF)
    if rnd.random() < 0.1:
        b += bytes(rnd.randint(1, 3))
    return b


def operand():
    flag = rnd.choice([0, 0, 0, 1, 2])
    if flag == 0:
        return [0, rnd.choice([1, 2, 3, 4, 5, 6, 7, 0, 9]), rnd.randint(0, 40)]
    if flag == 1:
        return [1, rnd.choice([2, 7]), rnd.randint(0, 8), rnd.choice([1, 2, 7]), rnd.randint(0, 40)]
    return [flag]


def fxlc_payload():
    ins = []
    m = rnd.randint(0, 4)
    count = m if rnd.random() < 0.9 else m + rnd.randint(1, 3)
    for _ in range(m):
        op, nin = rnd.choice([(0x100, 1), (0x103, 1), (0x204, 2), (0x300, 3), (0x301, 3), (0x500, 2), (0x70e, 6),
                              (0x70e, 8), (0x000, 0), (0x999, 1), (0x100, 2)])
        tok = op << 20 | rnd.randint(1, 4) | (0x80000000 if rnd.random() < 0.2 else 0) | \
            (0x30000 if rnd.random() < 0.05 else 0)
        ins += [tok, nin]
        for _ in range(nin + 1):
            ins += operand()
    trailer = rnd.choice([[0xF0F0F0F0, 0x0F0F0F0F], [], [1, 2, 3]])
    return [count] + ins + trailer


def preshader_blob():
    ver = rnd.choice([0x46580200, 0x46580200, 0x46580000, 0x12340000])
    b = dw(ver)
    parts = []
    for kind in rnd.sample(['CTAB', 'CLIT', 'FXLC', 'CTAB', 'ZZZZ'], rnd.randint(0, 4)):
        if kind == 'CTAB':
            parts.append(comment(b'CTAB', ctab_payload(ver)))
        elif kind == 'CLIT':
            n = rnd.randint(0, 3)
            lits = []
            for _ in range(n):
                q = struct.unpack('<2I', struct.pack('<d', rnd.choice([0.0, 1.0, -0.0, 0.5, 1e300, float('inf'),
                                                                      float('nan'), 5e-324, 1 / 3.0])))
                lits += list(q)
            cnt = n if rnd.random() < 0.85 else n + 2
            parts.append(comment(b'CLIT', [cnt] + lits if rnd.random() < 0.95 else []))
        elif kind == 'FXLC':
            parts.append(comment(b'FXLC', fxlc_payload() if rnd.random() < 0.95 else []))
        else:
            parts.append(comment(b'ZZZZ', [7]))
    b += b''.join(parts)
    if rnd.random() < 0.9:
        b += dw(0x0000FFFF)
    if rnd.random() < 0.05:
        b += dw(0)
    return b


def effect():
    g = Gen()
    g.objtype = {}
    params, techs = [], []
    for _ in range(rnd.randint(0, 5)):
        toff, voff, d = g.typed_value(0)
        annos = [g.typed_value(0)[:2] for _ in range(rnd.choice([0, 0, 1, 2]))]
        params.append((toff, voff, rnd.choice([0, 1, 0x80000000]), annos, d))
    for _ in range(rnd.randint(0, 3)):
        name = g.name()
        annos = [g.typed_value(0)[:2] for _ in range(rnd.choice([0, 1, 2]))]
        passes = []
        for _ in range(rnd.randint(0, 3)):
            pname = g.name()
            pannos = [g.typed_value(0)[:2] for _ in range(rnd.choice([0, 0, 1]))]
            states = [g.state_entry(0, True) for _ in range(rnd.randint(0, 4))]
            passes.append((pname, pannos, states))
        techs.append((name, annos, passes))
    tables = len(g.data)
    t = bytearray()
    nobj = g.nobj if rnd.random() < 0.9 else g.nobj + rnd.randint(-1, 2)
    t += dw(len(params), len(techs), rnd.randint(0, 50), max(nobj, 0))
    for toff, voff, flags, annos, d in params:
        t += dw(toff, voff, flags, len(annos))
        for a in annos:
            t += dw(*a)
    for name, annos, passes in techs:
        t += dw(name, len(annos), len(passes))
        for a in annos:
            t += dw(*a)
        for pname, pannos, states in passes:
            t += dw(pname, len(pannos), len(states))
            for a in pannos:
                t += dw(*a)
            for s, _ in states:
                t += s
    odata = []
    for _ in range(rnd.randint(0, 3)):
        size = rnd.choice([0, 0, 3, 8])
        odata.append(dw(rnd.randint(0, g.nobj + 1), size) + bytes(rnd.randrange(256) for _ in range(size)) +
                     bytes((-size) % 4))
    res = []
    for _ in range(rnd.randint(0, 6)):
        if techs and rnd.random() < 0.6:
            ti = rnd.randrange(len(techs))
            passes = techs[ti][2]
            pj = rnd.randrange(len(passes)) if passes else 0
            st = rnd.randrange(len(passes[pj][2])) if passes and passes[pj][2] else 0
            head = [ti if rnd.random() < 0.9 else 7, pj, 0xFFFFFFFF, st]
        else:
            pi = rnd.randrange(len(params)) if params else 0
            head = [0xFFFFFFFF, pi if rnd.random() < 0.9 else 50, rnd.choice([0, 0xFFFFFFFF, 1, 2]), rnd.randint(0, 3)]
        usage = rnd.choice([0, 0, 0, 1, 1, 2, 2, 5])
        kind = rnd.random()
        if head[0] < len(techs) and techs[head[0]][2] and techs[head[0]][2][head[1]][2] and usage == 0:
            d = techs[head[0]][2][head[1]][2][head[3]][1]
            if d['type'] in (15, 16) and d['cls'] == 4:
                kind = 0.1 if rnd.random() < 0.9 else kind
            elif d['type'] in (1, 2, 3, 4):
                kind = 0.6 if rnd.random() < 0.9 else kind
        if usage == 1:
            blob = bytes(rnd.choice(b'abc"\\\x00\x05') for _ in range(rnd.randint(0, 9)))
        elif usage == 2:
            nm = b'arr\0'
            if rnd.random() < 0.5:
                blob = dw(len(nm)) + nm + preshader_blob()
            else:
                blob = rnd.choice([b'', b'\x01\x02', dw(100) + b'abc', dw(0) + preshader_blob()])
        else:
            blob = shader_blob() if kind < 0.45 else preshader_blob() if kind < 0.9 else bytes(rnd.randint(0, 6))
        res.append(dw(*head, usage, len(blob)) + blob + bytes(rnd.randrange(1, 256) for _ in range((-len(blob)) % 4)))
    t += dw(len(odata), len(res))
    t += b''.join(odata) + b''.join(res)
    return dw(0xFEFF0901, tables) + bytes(g.data) + bytes(t)


def run_a(data):
    open(TMP, 'wb').write(data)
    p = subprocess.run([A, TMP], capture_output=True, timeout=20)
    if p.returncode == 0:
        return 'ok', p.stdout.decode('latin-1')
    if p.returncode == 1 and b'Sanitizer' not in p.stderr and b'runtime error' not in p.stderr:
        return 'refused', p.stderr.decode('latin-1')
    return 'crash', p.stderr.decode('latin-1')[-3000:]


def run_b(data):
    try:
        return 'ok', B.Effect('SYN', data).dump()
    except B.Bad as e:
        return 'refused', str(e)
    except Exception as e:
        return 'crash', '%s: %s' % (type(e).__name__, e)


stats = collections.Counter()
feat = collections.Counter()
n_bad = 0
for i in range(N):
    data = effect()
    ra, ta = run_a(data)
    rb, tb = run_b(data)
    if ra == rb == 'ok':
        if ta == tb:
            stats['same output'] += 1
            for k in ('struct', 'sampler', 'value [', 'UNKNOWN(', 'select', 'res ', 'shader ', 'preshader ',
                      'lit ', 'ins ', '.s ', '+', 'string '):
                if k in ta:
                    feat[k] += 1
        else:
            stats['DIFFERENT output'] += 1
            n_bad += 1
    elif ra == rb:
        stats['both ' + ra] += 1
    else:
        stats['%s/%s' % (ra, rb)] += 1
        n_bad += 1
    if (ra, rb) != ('ok', 'ok') or ta != tb:
        if ra != rb or ta != tb:
            d = os.path.join(OUTD, '%04d' % i)
            os.makedirs(d, exist_ok=True)
            open(os.path.join(d, 'in.fxo'), 'wb').write(data)
            open(os.path.join(d, 'a.txt'), 'w').write('%s\n%s' % (ra, ta))
            open(os.path.join(d, 'b.txt'), 'w').write('%s\n%s' % (rb, tb))
for k, v in sorted(stats.items()):
    print('%-20s %d' % (k, v))
print('dumps that agree and contain:', dict(feat))
sys.exit(1 if n_bad else 0)
