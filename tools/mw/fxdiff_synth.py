"""Differential test of the two M5-01 dumpers on synthetic fx_2_0 effects.

  .venv/bin/python -I tools/mw/fxdiff_synth.py [--model] <checkout> <count> [seed]

Builds valid containers that use every feature of fx2-format.md (structs,
arrays, nested sampler blocks, unknown types and classes, every usage,
random shader and preshader blobs) and compares A's and B's dumps.

With --model it compares the two effect models instead (docs/mw/fx-model.md
§6.3): out/mw/fx/bin/fxmodeldump (A) and tools/fxmodel.py (B, imported) on
<count> made-up sets of one to four effects created in one pool.  These
effects aim at what the model reads: names from a small vocabulary (so CTAB
names, usage-1 names and pool matches hit, and paths like "a.b" collide),
typedefs that later effects of the set reuse or change slightly (pool
matching), structs, BOOL parameters, string annotations, sampler arrays,
CTAB constants built from the parameters with one field changed now and
then (every reason of §3.2), and shaders, preshaders and parameter
references on pass and sampler states.  Each split is written to
out/mw/fx/diff/model_synth_cases/<n>/ (the inputs, a.txt, b.txt).  The
text compared is the model dump followed by the Set/Get/lookup trace
(fxmodeldump --ops, fxmodel.py's ops_all).  FXMODELDUMP names another A
binary than out/mw/fx/bin/fxmodeldump.
"""
import os, random, shutil, struct, subprocess, sys, importlib.util, collections, copy, signal

ARGS = sys.argv[1:]
MODEL = bool(ARGS) and ARGS[0] == '--model'
if MODEL:
    ARGS = ARGS[1:]
W = ARGS[0]
N = int(ARGS[1])
rnd = random.Random(int(ARGS[2]) if len(ARGS) > 2 else 1)
HERE = os.path.join(W, 'out', 'mw', 'fx', 'diff')
OUTD = os.path.join(HERE, 'model_synth_cases' if MODEL else 'synth_cases')
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


# ---- --model: effect sets for the two effect models (fx-model.md §6.3) ----

VOCAB = [b'a', b'a', b'b', b'c', b'tex', b'smp', b'a.b', b'a[0]', b'a[1]', b'b.c', b'', b'A', b'x y', b'\xe9']
SEMS = [b'COLOR', b'color', b'Color', b'POS', b'pos', b'\xe9T', b'\xc9T', b'']
NUMVALS = [0, 1, 2, 5, 0xFFFFFFFF, 0x3F800000, 0xBF800000, 0x80000000, 0x7FC00000, 0x4F000000]
SAMPLERS = (10, 11, 12, 13, 14)
NONE = 0xFFFFFFFF


class MGen:
    """One made-up effect of a set.  shared is the list of top-level typedef descriptions of the set's earlier
    effects (and this one's), which new parameters reuse as they are or with one field changed."""

    put = Gen.put

    def __init__(self, shared):
        self.data = bytearray(dw(0))
        self.nobj = 1
        self.objtype = {}
        self.max_depth = rnd.choice([1, 2, 3])
        self.shared = shared
        self.tops = []          # top-level typedef descriptions
        self.paths = []         # (path, description) of every parameter, element and member
        self.smp = []           # sampler-block states of top-level parameters: (param, element or None, k, desc)
        self.pst = []           # pass states: (technique, pass, k, desc)

    def nm(self, pool):
        r = rnd.random()
        if r < 0.08:
            return None
        if r < 0.88:
            return rnd.choice(pool)
        return bytes(rnd.choice(b'abcXYZ_09"\\ .[]\x01\x7f\xe9') for _ in range(rnd.randint(0, 6)))

    def put_name(self, s):
        if s is None:
            return 0
        r = rnd.random()
        rec = s if r < 0.04 else s + b'\0tail\0' if r < 0.08 else s + b'\0'
        return self.put(dw(len(rec)) + rec)

    def desc(self, depth=0):
        r = rnd.random()
        if r < 0.45:
            cls = rnd.choice([0, 0, 1, 1, 2, 3])
        elif r < 0.75:
            cls = 4
        elif r < 0.93 and depth < self.max_depth:
            cls = 5
        else:
            cls = rnd.choice([0, 6, 9])
        if cls == 4:
            typ = rnd.choice([4, 4, 5, 5, 7, 9, 10, 12, 12, 14, 15, 16, 0, 19])
        elif cls == 5:
            typ = rnd.choice([0, 0, 0, 3])
        else:
            typ = rnd.choice([1, 1, 2, 3, 3, 3, 0, 4, 21])
        d = {'type': typ, 'cls': cls, 'name': self.nm(VOCAB), 'sem': self.nm(SEMS),
             'elems': rnd.choice([0, 0, 0, 0, 1, 2, 3])}
        if cls in (0, 1, 2, 3):
            d['rows'], d['cols'] = rnd.choice([(1, 1), (1, 1), (1, 2), (1, 3), (1, 4), (1, 4), (3, 3), (4, 3),
                                               (3, 4), (4, 4), (4, 1), (3, 1), (2, 2), (0, 2), (5, 5)])
        if cls == 5:
            d['members'] = [self.desc(depth + 1) for _ in range(rnd.randint(0, 3))]
        return d

    def tweak(self, d):
        k = rnd.choice(['type', 'elems', 'sem', 'shape', 'members', 'mname', 'name', 'cls'])
        if k == 'type':
            d['type'] = rnd.choice([1, 2, 3, 5, 12])
        elif k == 'elems':
            d['elems'] = rnd.choice([0, 1, 2, 4])
        elif k == 'sem':
            d['sem'] = self.nm(SEMS)
        elif k == 'shape' and 'rows' in d:
            d['rows'], d['cols'] = d['cols'], d['rows'] + 1
        elif k == 'members' and d['cls'] == 5:
            if d['members'] and rnd.random() < 0.5:
                d['members'].pop()
            else:
                d['members'].append(self.desc(self.max_depth))
        elif k == 'mname' and d.get('members'):
            rnd.choice(d['members'])['name'] = self.nm(VOCAB)
        elif k == 'name':
            d['name'] = self.nm(VOCAB)
        elif k == 'cls' and d['cls'] in (0, 1, 2, 3):
            d['cls'] = rnd.choice([0, 1, 2, 3])

    def emit(self, d):
        """The typedef's bytes; its names (and its members') are put into the data area now."""
        b = dw(d['type'], d['cls'], self.put_name(d['name']), self.put_name(d['sem']), d['elems'])
        if d['cls'] == 1:
            b += dw(d['cols'], d['rows'])
        elif d['cls'] in (0, 2, 3):
            b += dw(d['rows'], d['cols'])
        elif d['cls'] == 5:
            b += dw(len(d['members']))
            for m in d['members']:
                b += self.emit(m)
        return b

    def value(self, d, depth, top=None):
        out = b''
        for e in range(max(1, d['elems'])):
            out += self.element(d, depth, None if top is None else (top, e if d['elems'] else None))
        return out

    def element(self, d, depth, sink):
        cls, typ = d['cls'], d['type']
        if cls in (0, 1, 2, 3):
            return b''.join(dw(rnd.choice(NUMVALS) if rnd.random() < 0.8 else rnd.getrandbits(32))
                            for _ in range(d['rows'] * d['cols']))
        if cls == 4 and typ in SAMPLERS:
            n = rnd.randint(0, 4) if depth < 4 else 0
            b = dw(n)
            for k in range(n):
                sb, sd = self.state(depth + 1, True)
                b += sb
                if sink is not None:
                    self.smp.append((sink[0], sink[1], k, sd))
            return b
        if cls == 4:
            oid = self.nobj if rnd.random() < 0.93 else rnd.choice([0, 999, self.nobj + 1])
            if oid == self.nobj:
                self.nobj += 1
            self.objtype.setdefault(oid, typ)
            return dw(oid)
        if cls == 5:
            return b''.join(self.value(m, depth + 1) for m in d['members'])
        return b''

    def state_desc(self, op, depth):
        if rnd.random() < 0.12:
            return self.desc(min(depth, self.max_depth))
        d = {'name': None if rnd.random() < 0.8 else self.nm(VOCAB), 'sem': None, 'elems': 0}
        if op in (146, 147):
            if rnd.random() < 0.15:
                return dict(d, type=2, cls=2, rows=1, cols=1)        # a NULL shader: INT literal 0
            return dict(d, type=16 if op == 146 else 15, cls=4)
        if op == 164:
            return dict(d, type=rnd.choice([5, 7, 9]), cls=4)
        if op == 178:
            return dict(d, type=rnd.choice([10, 12]), cls=4)
        if rnd.random() < 0.08:
            return dict(d, type=4, cls=4)
        return dict(d, type=rnd.choice([2, 2, 3, 1]), cls=2, rows=1, cols=rnd.choice([1, 1, 1, 4]))

    def state(self, depth, sampler):
        if sampler:
            op = rnd.choice([164, 164, 164, rnd.randrange(165, 178), rnd.randrange(165, 178), 178,
                             rnd.randrange(182), 182 + rnd.randrange(3)])
        else:
            op = rnd.choice([146, 146, 147, 147, 147, 27, 7, 168, 164, 178, 123, rnd.randrange(182), 200])
        idx = rnd.choice([0, 0, 0, 1, 256, NONE])
        d = self.state_desc(op, depth)
        toff = self.put(self.emit(d))
        voff = self.put(self.value(d, depth))
        return dw(op, idx, toff, voff), d

    def add_paths(self, path, d):
        if len(self.paths) > 300:
            return
        self.paths.append((path, d))
        for e in range(d['elems']):
            self.add_paths(path + b'[%d]' % e, dict(d, elems=0))
        if not d['elems'] and d['cls'] == 5:
            for m in d['members']:
                self.add_paths(path + b'.' + (m['name'] or b''), m)

    def param(self, i):
        if self.shared and rnd.random() < 0.45:
            d = copy.deepcopy(rnd.choice(self.shared))
            if rnd.random() < 0.3:
                self.tweak(d)
        else:
            d = self.desc()
        self.shared.append(d)
        toff = self.put(self.emit(d))
        voff = self.put(self.value(d, 0, i))
        self.tops.append(d)
        self.add_paths(d['name'] or b'', d)
        annos = [self.annot() for _ in range(rnd.choice([0, 0, 0, 1, 2]))]
        return toff, voff, rnd.choice([0, 1, 1, 1, 3, 0x80000000, 0x80000001]), annos

    def annot(self):
        r = rnd.random()
        name = self.nm(VOCAB + [b'shader', b'shader', b'nosuch'])
        if r < 0.3:
            d = {'type': 4, 'cls': 4, 'name': name, 'sem': None, 'elems': rnd.choice([0, 0, 0, 2])}
        elif r < 0.6:
            d = {'type': rnd.choice([1, 2, 3]), 'cls': 0, 'name': name, 'sem': None, 'elems': 0, 'rows': 1,
                 'cols': 1}
        else:
            d = self.desc(rnd.choice([0, self.max_depth]))
        toff = self.put(self.emit(d))
        return toff, self.put(self.value(d, 1))

    def const_spec(self, pres):
        """One CTAB constant: (name, register set, register, count, type info), mostly made from a parameter."""
        r = rnd.random()
        smps = [d for d in self.tops if d['cls'] == 4 and d['type'] in SAMPLERS] if not pres else []
        if smps and r < 0.25:                                  # samplers bind blocks: deps and stages
            d = rnd.choice(smps)
            name = d['name'] if d['name'] is not None else b''
        elif self.tops and r < 0.7:
            d = rnd.choice(self.tops)
            name = d['name'] if d['name'] is not None else b''
        elif self.paths and r < 0.85:
            name, d = rnd.choice(self.paths)
        else:
            name, d = rnd.choice(VOCAB), self.desc(self.max_depth)
        cls, typ = d['cls'], d['type']
        rows, cols = d.get('rows', 1), d.get('cols', 1)
        if cls == 2 and rnd.random() < 0.6:
            cls = 3                                            # MATRIX_COLUMNS against MATRIX_ROWS: the usual pair
        elems, mem = max(1, d['elems']), len(d.get('members', ()))
        if rnd.random() < 0.25:
            f = rnd.choice(['cls', 'rows', 'cols', 'elems', 'mem', 'type'])
            if f == 'cls':
                cls = rnd.choice([0, 1, 2, 3, 4, 5, 7])
            elif f == 'rows':
                rows = rnd.choice([0, 1, 2, 4])
            elif f == 'cols':
                cols = rnd.choice([0, 1, 3, 4])
            elif f == 'elems':
                elems = rnd.choice([0, 1, 2, 5])
            elif f == 'mem':
                mem = rnd.choice([0, 1, 4])
            else:
                typ = rnd.choice([1, 2, 3, 12])
        smp = d['cls'] == 4 and d['type'] in SAMPLERS
        if rnd.random() < 0.88:
            rset = 3 if smp else 2 if pres or rnd.random() < 0.7 else rnd.choice([0, 1])
        else:
            rset = rnd.choice([0, 1, 2, 3, 4, 9])
        per = 1 if rset == 3 else rows * cols if rset == 0 or cls not in (1, 2, 3) else \
            1 if cls == 1 else rows if cls == 2 else cols
        full = elems * per
        count = rnd.choice([full, full, full, max(1, full - 1), 1, full + 2, 0, rnd.randint(0, 9)])
        if smp and d['elems'] > 1 and rnd.random() < 0.4:
            count = rnd.randint(1, d['elems'] - 1)             # a sampler array bound in part
        return name, rset, rnd.randint(0, 20), count, (cls, typ, rows, cols, elems, mem)

    def ref_name(self):
        s = rnd.choice(self.paths)[0] if self.paths and rnd.random() < 0.8 else rnd.choice(VOCAB + [b'nosuch'])
        return s + rnd.choice([b'\0', b'\0', b'\0', b'', b'\0junk'])

    def ctab(self, version, pres):
        specs = [self.const_spec(pres) for _ in range(rnd.randint(0, 4))]
        body = bytearray()

        def s(txt):
            off = 28 + len(body)
            body.extend(txt + b'\0')
            while len(body) % 4:
                body.append(0xAB)
            return off
        creator, target = s(b'Synth compiler'), s(b'xs_2_0')
        tinfos = []
        for sp in specs:
            tinfos.append(28 + len(body))
            body.extend(struct.pack('<6HI', *[x & 0xFFFF for x in sp[4]], 0))
        names = [s(sp[0]) for sp in specs]
        cinfo = 28 + len(body)
        end = cinfo + 20 * len(specs)
        for k, (name, rset, reg, count, _) in enumerate(specs):
            ti = tinfos[k] if rnd.random() < 0.94 else rnd.choice([0x7FFF, end - 8])
            no = names[k] if rnd.random() < 0.95 else rnd.choice([0, 0x8000])
            body.extend(struct.pack('<I4H2I', no, rset, reg, count, 0, ti, 0))
        n = len(specs) + (rnd.choice([1, 2]) if rnd.random() < 0.06 else 0)    # entries past the table: reason 6
        raw = struct.pack('<7I', 28, creator, version, n, cinfo, 0, target) + bytes(body)
        return list(struct.unpack('<%dI' % (len(raw) // 4), raw))

    def shader(self, vs):
        ver = rnd.choice([0x0101, 0x0101, 0x0104, 0x0200, 0x0201, 0x0300, 0x0200, 0x0300, 0x0102]) | \
            (0xFFFE0000 if vs else 0xFFFF0000)
        if rnd.random() < 0.06:
            ver = rnd.choice([0x12340000, 0xFFFF0101 if vs else 0xFFFE0300])
        b = dw(ver)
        if rnd.random() < 0.08:
            b += comment(b'XXXX', [1, 2])
        if rnd.random() < 0.03:
            b += comment(b'CTAB', [1, 2])                      # under 28 bytes: no CTAB
        if rnd.random() < 0.9:
            b += comment(b'CTAB', self.ctab(ver, False))
        if rnd.random() < 0.1:
            b += comment(b'CTAB', self.ctab(ver, False))       # a second one is not read
        major = ver >> 8 & 0xFF
        for _ in range(rnd.randint(0, 3)):
            b += dw(0x01 | (2 << 24 if major >= 2 else 0), 0x80000000 | rnd.getrandbits(16),
                    0x80000000 | rnd.getrandbits(16))
        return b + dw(0x0000FFFF)

    def preshader(self):
        b = dw(0x46580200 if rnd.random() < 0.94 else rnd.choice([0x46580000, 0x12340000]))
        parts = [comment(b'CLIT', [0]), comment(b'FXLC', fxlc_payload())]
        if rnd.random() < 0.9:
            parts.insert(rnd.choice([0, 0, 0, 1, 2]), comment(b'CTAB', self.ctab(0x46580200, True)))
        return b + b''.join(parts) + dw(0x0000FFFF)

    def attach(self, head, d):
        """Resource entries for the state at head (technique or NONE, pass or parameter, element, state)."""
        out = []
        for _ in range(2 if rnd.random() < 0.08 else 1):
            typ, cls, r = d['type'], d['cls'], rnd.random()
            if cls == 4 and typ in (15, 16) and r < 0.8:
                usage, blob = 0, self.shader(typ == 16)
            elif cls == 4 and typ not in SAMPLERS and r < 0.6 or cls == 4 and typ in (15, 16) and r < 0.9:
                usage, blob = 1, self.ref_name()
            elif typ in (1, 2, 3, 4) and cls in (0, 1, 2, 3, 4) and r < 0.5:
                usage, blob = 0, self.preshader()
            elif r < 0.93:
                continue
            else:
                usage, blob = rnd.choice([(2, dw(4) + b'arr\0' + self.preshader()), (5, b'xx'), (0, bytes(3)),
                                          (1, self.ref_name()), (0, self.shader(rnd.random() < 0.5))])
            out.append(dw(*head, usage, len(blob)) + blob + bytes(rnd.randrange(1, 256) for _ in range((-len(blob)) % 4)))
        return out


def m_effect(shared):
    g = MGen(shared)
    params = [g.param(i) for i in range(rnd.randint(0, 6))]
    techs = []
    for ti in range(rnd.randint(0, 3)):
        name = g.put_name(g.nm(VOCAB))
        annos = [g.annot() for _ in range(rnd.choice([0, 1, 2]))]
        passes = []
        for pj in range(rnd.randint(0, 3)):
            pname = g.put_name(g.nm(VOCAB))
            pannos = [g.annot() for _ in range(rnd.choice([0, 0, 1]))]
            states = []
            for k in range(rnd.randint(0, 5)):
                sb, sd = g.state(0, False)
                states.append(sb)
                g.pst.append((ti, pj, k, sd))
            passes.append((pname, pannos, states))
        techs.append((name, annos, passes))
    res = []
    for ti, pj, k, d in g.pst:
        res += g.attach([ti, pj, rnd.choice([NONE, NONE, 0]), k], d)
    for pi, e, k, d in g.smp:
        res += g.attach([NONE, pi, e if e is not None else rnd.choice([0, 0, NONE, 3]), k], d)
    for _ in range(rnd.choice([0, 0, 0, 1])):                  # aimed at nothing
        res.append(dw(rnd.choice([7, NONE]), 50, NONE, 0, 1, 4) + b'a\0\0\0')
    rnd.shuffle(res)
    odata = []
    for oid, typ in sorted(g.objtype.items()):
        if typ == 4 and rnd.random() < 0.85:
            s = rnd.choice([b'hello\0', b'', b'no nul', b'a\0b\0', b'"q"\\\0', b'\xe9\x01\0'])
            odata.append(dw(oid, len(s)) + s + bytes((-len(s)) % 4))
            if rnd.random() < 0.1:
                odata.append(dw(oid, 4) + b'dup\0')
        elif typ != 4 and rnd.random() < 0.3:
            odata.append(dw(oid, 0))
    rnd.shuffle(odata)
    tables = len(g.data)
    t = bytearray()
    nobj = g.nobj if rnd.random() < 0.9 else max(g.nobj + rnd.randint(-1, 2), 0)
    t += dw(len(params), len(techs), rnd.randint(0, 50), nobj)
    for toff, voff, flags, annos in params:
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
            for s in states:
                t += s
    t += dw(len(odata), len(res)) + b''.join(odata) + b''.join(res)
    return dw(0xFEFF0901, tables) + bytes(g.data) + bytes(t)


class Timeout(Exception):
    pass


def on_alarm(*_):
    raise Timeout()


def model_tools(w):
    """A's binary (out/mw/fx/bin/fxmodeldump, or the environment variable FXMODELDUMP) and B's module."""
    spec = importlib.util.spec_from_file_location('fxmodel', os.path.join(w, 'tools', 'fxmodel.py'))
    mb = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mb)
    return os.environ.get('FXMODELDUMP') or os.path.join(w, 'out', 'mw', 'fx', 'bin', 'fxmodeldump'), mb


def model_run(am, mb, case, files):
    """Both models of the set of .fxo files in case: (status, text) for A and for B.  The text is the model
    dump, then the Set/Get/lookup trace (fxmodeldump --ops, fxmodel.py's ops_all)."""
    ra = None
    for args in (['-'], ['--ops', '-']):
        p = subprocess.run([am] + args + files, capture_output=True, timeout=60)
        if p.returncode == 0:
            ra = 'ok', (ra[1] if ra else '') + p.stdout.decode('latin-1')
        elif p.returncode == 1 and b'Sanitizer' not in p.stderr and b'runtime error' not in p.stderr:
            ra = 'refused', p.stderr.decode('latin-1').strip()
            break
        else:
            ra = 'crash', p.stderr.decode('latin-1')[-3000:]
            break
    old = signal.signal(signal.SIGALRM, on_alarm)
    signal.alarm(60)
    try:
        protos = mb.load(case)
        insts, pool = mb.build(protos)
        rb = 'ok', mb.dump_all(insts, pool) + mb.ops_all(protos)
    except SystemExit as e:
        rb = 'refused', str(e)
    except Timeout:
        rb = 'timeout', ''
    except Exception as e:
        rb = 'crash', '%s: %s' % (type(e).__name__, e)
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, old)
    return ra, rb


# what the agreeing dumps exercised (fx-model.md's parts)
MODEL_FEATURES = [('member', '\n    member #'), ('BOOL param', ' BOOL '), ('string annotation', '= string "'),
                  ('pool slot with 2+ holders', None), ('apart', '\n    apart '), ('sampler const', ' sampler stage '),
                  ('vs sampler stage', None), ('param ref', ' param #'), ('unresolved ref', 'param UNKNOWN(0x00000001)'),
                  ('deps', None), ('cut registers', None), ('columns', ' columns per ')] + \
                 [('reason %d' % k, '-> UNKNOWN(0x%08x)' % k) for k in (1, 2, 3, 4, 5, 7)] + \
                 [('reason 6 (past the table)', ' UNKNOWN(0x00000006)\n'), ('two resources', None),
                  ('sampler array partly bound', None), ('unnamed slot', None),
                  ('string id past O', '= string UNKNOWN(')]


def model_features(text, feat):
    import re
    for k, s in MODEL_FEATURES:
        if s is not None and s in text:
            feat[k] += 1
    if re.search(r'holders ([2-9]|\d\d)', text):
        feat['pool slot with 2+ holders'] += 1
    if re.search(r'sampler stage (25[7-9]|2[6-9]\d)', text):
        feat['vs sampler stage'] += 1
    if re.search(r' deps [1-9]', text):
        feat['deps'] += 1
    if re.search(r'^ *state .*(ctab \S+|#\d+|"|\)) UNKNOWN\(0x0000000[2-9]\)$', text, re.M):
        feat['two resources'] += 1
    if re.search(r'sampler stage \d+ elements (\d+)/(?!\1\b)\d+', text):
        feat['sampler array partly bound'] += 1
    if re.search(r'^  slot \d+ name (-|"") ', text, re.M):
        feat['unnamed slot'] += 1
    for m in re.finditer(r'count (\d+) -> #\d+ \S+ \S+ per (\d+) elements (\d+)/(\d+)', text):
        if int(m.group(1)) < int(m.group(2)) * int(m.group(4)):
            feat['cut registers'] += 1
            break


def model_main():
    am, mb = model_tools(W)
    case = os.path.join(HERE, 'model_case')
    shutil.rmtree(OUTD, ignore_errors=True)
    os.makedirs(OUTD)
    stats, feat, n_bad = collections.Counter(), collections.Counter(), 0
    for i in range(N):
        names = rnd.sample(['S', 'S0', 'SA', 'S_a', 'Sb', 'Sz'], rnd.choice([1, 1, 2, 2, 3, 4]))
        shared = []
        sets = [(n, m_effect(shared)) for n in names]
        shutil.rmtree(case, ignore_errors=True)
        os.makedirs(case)
        files = []
        for n, data in sets:
            files.append(os.path.join(case, n + '.fxo'))
            with open(files[-1], 'wb') as f:
                f.write(data)
        rnd.shuffle(files)
        (ra, ta), (rb, tb) = model_run(am, mb, case, files)
        if ra == rb == 'ok' and ta == tb:
            stats['same output'] += 1
            model_features(ta, feat)
            continue
        if ra == rb == 'refused':
            stats['both refused'] += 1
            continue
        stats['DIFFERENT output' if ra == rb == 'ok' else '%s/%s' % (ra, rb)] += 1
        n_bad += 1
        d = os.path.join(OUTD, '%04d' % i)
        shutil.rmtree(d, ignore_errors=True)
        shutil.copytree(case, d)
        with open(os.path.join(d, 'a.txt'), 'w') as f:
            f.write('%s\n%s' % (ra, ta))
        with open(os.path.join(d, 'b.txt'), 'w') as f:
            f.write('%s\n%s' % (rb, tb))
    shutil.rmtree(case, ignore_errors=True)
    if not n_bad:
        shutil.rmtree(OUTD, ignore_errors=True)
        try:
            os.rmdir(HERE)                                     # out/mw/fx/diff, when nothing else is there
        except OSError:
            pass
    for k, v in sorted(stats.items()):
        print('%-20s %d' % (k, v))
    print('model dumps that agree and contain:', ', '.join('%s %d' % (k, feat[k]) for k, _ in MODEL_FEATURES))
    return 1 if n_bad else 0


if MODEL:
    sys.exit(model_main())

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
