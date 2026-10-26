#!/usr/bin/env python3
"""The effect model of docs/mw/fx-model.md (what D3DX 9 builds from a parsed fx_2_0 effect), implementation B.

  tools/fxmodel.py [<indir> [<outdir>]]        model every <indir>/*.fxo (default out/mw/fx/res), all with one
                                               pool, and write the canonical dump 'fxmodel 1' (fx-model.md 5) to
                                               <outdir>/all.txt (default out/mw/fx/mb); print a count table
  tools/fxmodel.py --check [<indir> [<bdir>]]  check the model against tools/fx2dump.py's dumps in <bdir> (default
                                               out/mw/fx/b: parameters, values, annotations, techniques, passes,
                                               states and the CTAB constants of every program) and, on Most
                                               Wanted's 31 effects, against the numbers of fx-model.md 3.1, 3.6, 7
  tools/fxmodel.py --self-test [<indir>]       the Set/Get rules of 2.5 with their versions and changed flags,
                                               the lookups of 2.3 and the CTAB reasons 1..7 of 3.2 on made-up
                                               effects; the lookups also over every <indir>/*.fxo
  tools/fxmodel.py --pool-test [<indir>]       6.2 over every <indir>/*.fxo with one pool: each slot written
                                               through each of its holders and read back through all of them,
                                               apart parameters untouched, ColorWriteMode and BlendState,
                                               lifetime and the drop callback; slot matching on made-up effects
  tools/fxmodel.py --synth <outdir>            write the made-up effects of the self-test as <outdir>/*.fxo
  tools/fxmodel.py --ops [<indir> [<outdir>]]  the Set/Get/lookup trace 'fxmodel ops 1' of every <indir>/*.fxo in
                                               one pool into <outdir>/ops.txt (default out/mw/fx/mb), the same
                                               calls as tools/mw/fxmodeldump.c --ops: the two must agree

The model is built here from fx-model.md, fx2-format.md and the bytes, on top of tools/fx2dump.py's reader (the
B side of M5-01); it is not derived from implementation A (runtime/mw/fxmodel.c), and the two dumps are compared
byte for byte (fx-model.md 6.3).  Node ids, paths and storage follow 2.2..2.4, the pool 2.7, versions 2.6, the CTAB
match 3.2..3.5.  The effects and every dump are the game's data: they stay under out/ (ignored).  The only names
of the game's this file states are the two pool parameters of GAME.md 5.5 that the pool test names.
"""
import hashlib, importlib.util, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
_spec = importlib.util.spec_from_file_location('fx2dump', os.path.join(HERE, 'fx2dump.py'))
F = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(F)

IN = os.path.join(ROOT, 'out', 'mw', 'fx', 'res')
OUT = os.path.join(ROOT, 'out', 'mw', 'fx', 'mb')
BDIR = os.path.join(ROOT, 'out', 'mw', 'fx', 'b')

OK, INVALIDCALL, FAIL = 0x00000000, 0x8876086C, 0x80004005
SHARED, ANNOTATION = 1, 4
K_PARAM, K_ELEMENT, K_MEMBER, K_ANNOT, K_TECH, K_PASS = range(1, 7)
BOOL, INT, FLOAT, STRING = 1, 2, 3, 4
NUMTYPES = (BOOL, INT, FLOAT)
TEXTURES = (5, 6, 7, 8, 9)
SAMPLERS = F.SAMPLERS
PIXELSHADER, VERTEXSHADER = 15, 16
POINTERS = (STRING,) + TEXTURES + (PIXELSHADER, VERTEXSHADER)      # the object types with storage (fx-model.md 2.4)
SCALAR, VECTOR, MATRIX_ROWS, MATRIX_COLUMNS, OBJECT, STRUCT = range(6)
NUMERIC = (SCALAR, VECTOR, MATRIX_ROWS, MATRIX_COLUMNS)
NONE = F.NONE
TNAME = {BOOL: 'bool', INT: 'int', FLOAT: 'float'}
SETTYPE = {0: BOOL, 1: INT, 2: FLOAT}
LAYOUTS = {SCALAR: 'scalar', VECTOR: 'vector', MATRIX_ROWS: 'rows', MATRIX_COLUMNS: 'columns'}

# The state table as an API (fx-model.md 4.1, 4.2): per operation its class and d3d number.  The D3D9 numbers are
# the SDK's (d3d9types.h), as fx2-format.md 3.1 lists them; rows 89 and 102 hold the SDK's 186 and 209 (4.3).
SCLASSES = ['RENDER', 'STAGE', 'NPATCH', 'FVF', 'TRANSFORM', 'MATERIAL', 'LIGHT', 'VSHADER', 'PSHADER', 'VSCONST',
            'PSCONST', 'TEXTURE', 'SAMPLERSTATE', 'SAMPLER', 'STATEBLOCK', 'PRESHADER_VS', 'PRESHADER_PS']
_RS = ([7, 8, 9, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 34, 35, 36, 37, 38, 48, 52, 53, 54, 55, 56,
        57, 58, 59, 60] + list(range(128, 136)) + list(range(198, 206)) +
       [136, 137, 139, 140, 141, 142, 143, 145, 146, 147, 148, 151, 152, 154, 155, 166, 156, 157, 158, 159, 160,
        161, 162, 163, 165, 167, 168, 170, 171, 172, 173, 174, 175, 176] + list(range(178, 196)) +
       [206, 207, 208, 209])
_TSS = [1, 26, 2, 3, 4, 27, 5, 6, 28, 7, 8, 9, 10, 11, 22, 23, 24, 32]
STATE_ROWS = ([(0, d) for d in _RS] + [(1, d) for d in _TSS] + [(2, 0), (3, 0)] +
              [(4, d) for d in (3, 2, 256, 16)] + [(5, d) for d in range(5)] + [(6, d) for d in range(14)] +
              [(7, 0), (8, 0)] + [(9, d) for d in (0, 0, 0, 0, 1, 2, 3, 4)] +
              [(10, d) for d in (0, 0, 0, 0, 1, 2, 3, 4)] + [(11, 0)] + [(12, d) for d in range(1, 14)] +
              [(13, 0), (14, 0), (15, 0), (16, 0)])
assert len(_RS) == 103 and len(STATE_ROWS) == 182 == len(F.STATES)


def nm(fx, off):
    """The name record at off up to its first NUL; None for offset 0."""
    r = fx.name_rec(off)
    return None if r is None else F.cut(r)


def q(b):
    """QSTR, '-' for a name at offset 0."""
    return '-' if b is None else F.qstr(b)


def tt(t, elem=False):
    """TT (fx2-format.md 5.4); an element prints its array's typedef without the [n] (fx-model.md 5.1)."""
    s = F.type_name(t.type) + ' ' + F.class_name(t.cls)
    if t.cls in NUMERIC:
        s += ' %dx%d' % (t.rows, t.cols)
    elif t.cls == STRUCT:
        s += ' %d' % len(t.members)
    if t.count and not elem:
        s += ' [%d]' % t.count
    return s


def val(typ, ws):
    """VAL(type, dwords)."""
    if not ws:
        return '-'
    if typ == FLOAT:
        return ' '.join(F.f32(w) for w in ws)
    if typ in (BOOL, INT):
        return ' '.join(F.i32(w) for w in ws)
    return ' '.join(F.hex8(w) for w in ws)


def simple(t):
    return t.cls in NUMERIC or (t.cls == OBJECT and t.type not in SAMPLERS)


def vn(v):
    """VN of a shader version token."""
    major, minor = (v >> 8) & 0xFF, v & 0xFF
    return ('vs' if v >> 16 == 0xFFFE else 'ps') + ('_2_x' if (major, minor) == (2, 1) else '_%d_%d' % (major, minor))


# Numbers: dwords in, dwords out (fx-model.md 2.1), with D3DX's conversion conv(to, from, w) of 2.5.

def f2w(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def w2f(w):
    return struct.unpack('<f', struct.pack('<I', w & 0xFFFFFFFF))[0]


INV255 = w2f(f2w(1.0 / 255.0))      # 1/255 as a single


def fmul(a, b):
    """a * b in single precision (both are singles here, so the double product is exact and rounds once)."""
    return f2w(a * b)


def conv(to, frm, w):
    w &= 0xFFFFFFFF
    if to == frm:
        return w                                    # the same type: copied as it is (W set_number)
    if to not in NUMTYPES:
        return 0
    if frm not in NUMTYPES:
        return w if frm == 0 else 0                 # a VOID source is its raw bits, any other type 0
    if frm == BOOL:
        return w if to == BOOL else (1 if w else 0) if to == INT else (0x3F800000 if w else 0)
    if frm == INT:
        return (1 if w else 0) if to == BOOL else w if to == INT else f2w(float(int(F.i32(w))))
    if frm == FLOAT:
        if to == BOOL:
            return 1 if w else 0                    # a bit test: -0.0 is TRUE
        if to == INT:
            x = w2f(w)
            return 0x80000000 if x != x or abs(x) >= 2.0 ** 31 else int(x) & 0xFFFFFFFF
        return w


def getbool(typ, w):
    """GetValue's BOOL dwords: 0 or 1 (GetBool and GetBoolArray use conv, raw from BOOL)."""
    return 1 if conv(BOOL, typ, w) else 0


def to255(w):
    """A float component clamped to [0, 1], times 255 in single precision, truncated."""
    x = w2f(w)
    x = 0.0 if not x > 0.0 else 1.0 if x > 1.0 else x
    return int(w2f(f2w(x * 255.0)))


class Node:
    """A parameter, element, member, annotation, technique or pass (fx-model.md 2.2)."""
    __slots__ = ('id', 'kind', 'parent', 'index', 'top', 't', 'name', 'sem', 'path', 'cls', 'type', 'rows', 'cols',
                 'elements', 'members', 'bytes', 'flags', 'off', 'object', 'block', 'skey', 'children', 'annots',
                 'store', 'types', 'objpos', 'oids', 'states', 'deps', 'vs', 'ps')

    def __init__(self, nid, kind, parent, index):
        self.id, self.kind, self.parent, self.index = nid, kind, parent, index
        self.top = 0
        self.t = self.name = self.sem = self.object = self.block = self.skey = None
        self.path = b''
        self.cls = self.type = self.rows = self.cols = self.elements = self.members = self.bytes = self.flags = 0
        self.off = 0
        self.children, self.annots = [], []
        self.store = self.types = self.objpos = self.oids = self.states = self.deps = None
        self.vs = self.ps = 0


class Acc:
    """The storage of one top-level parameter as it is read: dwords, the type of each, object dword positions."""
    def __init__(self):
        self.store, self.types, self.objpos, self.oids = [], [], [], []


class Prog:
    """A shader or preshader resource with its matched CTAB (fx-model.md 3.1)."""
    __slots__ = ('kind', 'r', 'version', 'known', 'vs', 'row', 'applied', 'ctab', 'nconsts', 'consts', 'outputs')


class Const:
    """One CTAB constant and its match (fx-model.md 3.2, 3.3)."""
    __slots__ = ('k', 'nameq', 'rset', 'reg', 'count', 'why', 'id', 'conv', 'layout', 'per', 'nelem', 'used',
                 'stage', 'sampler', 'cut', 'ccls', 'ctyp')

    def __init__(self, k):
        self.k, self.why, self.id, self.sampler, self.cut = k, 0, 0, False, False
        self.nameq, self.rset, self.reg, self.count = '-', 0, 0, 0
        self.conv = self.layout = None
        self.per = self.nelem = self.used = self.stage = self.ccls = self.ctyp = 0

    def line(self):
        if self.why == 6:
            return 'const %d %s' % (self.k, F.unk(6))
        s = 'const %d %s %s%d count %d -> ' % (self.k, self.nameq, F.SETS[self.rset] if self.rset < 4
                                               else F.unk(self.rset), self.reg, self.count)
        if self.why:
            return s + F.unk(self.why)
        if self.sampler:
            return s + '#%d sampler stage %d elements %d/%d' % (self.id, self.stage, self.used, self.nelem)
        return s + '#%d %s %s per %d elements %d/%d' % (self.id, self.conv, self.layout, self.per, self.used,
                                                        self.nelem)


def objdata(fx):
    """[(object id, bytes)] of the object-data entries: the table area walked again to their position."""
    p = fx.tables + 16
    for _, _, _, annos in fx.params:
        p += 16 + 8 * len(annos)
    for _, annos, passes in fx.techs:
        p += 12 + 8 * len(annos)
        for _, pannos, states in passes:
            p += 12 + 8 * len(pannos) + 16 * len(states)
    p += 8
    out = []
    for oid, size in fx.strings:
        out.append((oid, fx.bytes(p + 8, size)))
        p += 8 + (size + 3 & ~3)
    return out


def shader_comments(w, v):
    """[(dword, L)] of the comments in a shader's token walk (fx2-format.md 4.1, 5.9)."""
    n, major, i, found = len(w), (v >> 8) & 0xFF, 1, []
    while i < n:
        t = w[i]
        if t == F.END:
            break
        if t & 0xFFFF == 0xFFFE:
            L = (t >> 16) & 0x7FFF
            found.append((i, L))
            i += 1 + L
            continue
        if major >= 2:
            i += 1 + ((t >> 24) & 0xF)
        elif t & 0xFFFF in (0x51, 0x30):
            i += 6
        elif t & 0xFFFF == 0x2F:
            i += 3
        else:
            i += 1
            while i < n and w[i] & 0x80000000:
                i += 1
    return found


def lower(b):
    return bytes(c + 32 if 65 <= c <= 90 else c for c in b)


class Proto:
    """Everything the model fixes from the file alone: nodes, lookups, programs and their CTAB match, state sources,
    pass dependencies, technique versions.  Storage and the pool are an Inst's."""

    def __init__(self, name, data):
        fx = F.Effect(name, data)
        fx.dump()                       # refuses what fx2-format.md 5.11 refuses (raises F.Bad)
        self.fx, self.name, self.size = fx, name, len(data)
        self.sha = hashlib.sha256(data).hexdigest()
        self.nodes = [None]
        self.res_on = {}                # state key -> [resource numbers] (fx2-format.md 5.6)
        for r, tg in enumerate(fx.targets):
            if tg is not None:
                self.res_on.setdefault(tg[0], []).append(r)
        self.objdata = objdata(fx)
        self.params, self.techs = [], []
        for i, (td, v, flags, annos) in enumerate(fx.params):
            t = fx.typedef(td)[0]
            acc = Acc()
            n = self.make(K_PARAM, t, False, 0, i, 0, nm(fx, t.name) or b'', flags, v, acc, ('param', i))[0]
            n.store, n.types, n.objpos = acc.store, acc.types, acc.objpos
            n.annots = [self.annot(a, n.id, k) for k, a in enumerate(annos)]
            self.params.append(n.id)
        for ti, (tname, annos, passes) in enumerate(fx.techs):
            tn = self.new(K_TECH, 0, ti)
            tn.name = nm(fx, tname)
            tn.path = tn.name or b''
            tn.annots = [self.annot(a, tn.id, k) for k, a in enumerate(annos)]
            for j, (pname, pannos, states) in enumerate(passes):
                pn = self.new(K_PASS, tn.id, j)
                pn.name = nm(fx, pname)
                pn.path = pn.name or b''
                pn.annots = [self.annot(a, pn.id, k) for k, a in enumerate(pannos)]
                pn.states, pn.skey = states, ('pass', ti, j)
                tn.children.append(pn.id)
            self.techs.append(tn.id)
        self.index = {}                 # path -> the first parameter, element or member with it
        for n in self.nodes[1:]:
            if n.kind in (K_PARAM, K_ELEMENT, K_MEMBER):
                self.index.setdefault(n.path, n.id)
        self.progs = {}
        for r, tg in enumerate(fx.targets):
            if tg is not None and fx.resources[r][4] == 0:
                if tg[1].type in (VERTEXSHADER, PIXELSHADER):
                    self.progs[r] = self.program(r, 'shader', tg[1], self.state_op(tg[0]))
                elif tg[1].type in (BOOL, INT, FLOAT, STRING):
                    self.progs[r] = self.program(r, 'preshader', tg[1], self.state_op(tg[0]))
        for tid in self.techs:
            tn = self.nodes[tid]
            for pid in tn.children:
                self.pass_info(tn, self.nodes[pid])

    def state_op(self, key):
        """The state table row of the state a resource targets (key as fx2dump's target() gives it)."""
        fx = self.fx
        if key[0] == 'pass':
            return fx.techs[key[1]][2][key[2]][2][key[3]][0]
        return fx.du32(fx.sampler_at(fx.params[key[1]][1], key[2]) + 4 + 16 * key[3])

    def new(self, kind, parent, index):
        n = Node(len(self.nodes), kind, parent, index)
        self.nodes.append(n)
        return n

    def node(self, nid):
        return self.nodes[nid] if 0 < nid < len(self.nodes) else None

    # Nodes (fx-model.md 2.2) and storage (2.4), read as the value is laid out in the file (fx2-format.md 2.4).

    def make(self, kind, t, elem, parent, index, top, path, flags, fpos, acc, pkey):
        n = self.new(kind, parent, index)
        n.top = top or n.id
        n.t, n.path, n.flags = t, path, flags
        n.name, n.sem = nm(self.fx, t.name), nm(self.fx, t.sem)
        n.cls, n.type, n.rows, n.cols = t.cls, t.type, t.rows, t.cols
        n.elements = 0 if elem else t.count
        n.members = len(t.members)
        n.off = len(acc.store)
        if n.elements:
            for e in range(t.count):
                c, fpos = self.make(K_ELEMENT, t, True, n.id, e, n.top, path + b'[%d]' % e, flags, fpos, acc,
                                    pkey and pkey[:2] + (e,))
                n.children.append(c.id)
        else:
            fpos = self.one(n, t, fpos, acc, pkey and (pkey if len(pkey) == 3 else pkey + (0,)))
        n.bytes = 4 * (len(acc.store) - n.off)
        return n, fpos

    def one(self, n, t, fpos, acc, leaf):
        """One element's value at fpos into the node n (and its members); returns the file offset after it."""
        fx = self.fx
        if t.cls in NUMERIC:
            k = t.rows * t.cols
            acc.store.extend(fx.dwords(fpos, k) if k else ())
            acc.types.extend([t.type] * k)
            return fpos + 4 * k
        if t.cls == OBJECT and t.type not in SAMPLERS:
            n.object = fx.du32(fpos)
            if t.type in POINTERS:                  # W: any other object type has no storage
                acc.objpos.append(len(acc.store))
                acc.store.append(0)                 # a guest pointer, 0 at creation
                acc.types.append(t.type)
            return fpos + 4
        if t.cls == OBJECT:
            cnt = fx.du32(fpos)
            n.block = [fx.dwords(fpos + 4 + 16 * k, 4) for k in range(cnt)]
            n.skey = leaf                           # resources reach the blocks of top-level samplers only
            return fpos + 4 + 16 * cnt
        if t.cls == STRUCT:
            for m, mt in enumerate(t.members):
                c, fpos = self.make(K_MEMBER, mt, False, n.id, m, n.top, n.path + b'.' + (nm(fx, mt.name) or b''),
                                    n.flags, fpos, acc, None)
                n.children.append(c.id)
            return fpos
        return fpos                                 # an unknown class reads nothing

    def readval(self, t, fpos, acc):
        """A whole value (every element) into acc, without nodes: annotations."""
        fx = self.fx
        for _ in range(max(1, t.count)):
            if t.cls in NUMERIC:
                k = t.rows * t.cols
                acc.store.extend(fx.dwords(fpos, k) if k else ())
                acc.types.extend([t.type] * k)
                fpos += 4 * k
            elif t.cls == OBJECT and t.type not in SAMPLERS:
                acc.oids.append(fx.du32(fpos))
                if t.type in POINTERS:
                    acc.store.append(0)
                    acc.types.append(t.type)
                fpos += 4
            elif t.cls == OBJECT:
                fpos += 4 + 16 * fx.du32(fpos)
            elif t.cls == STRUCT:
                for mt in t.members:
                    fpos = self.readval(mt, fpos, acc)
        return fpos

    def annot(self, a, owner, k):
        td, v = a
        t = self.fx.typedef(td)[0]
        n = self.new(K_ANNOT, owner, k)
        n.t, n.flags = t, ANNOTATION
        n.name, n.sem = nm(self.fx, t.name), nm(self.fx, t.sem)
        n.path = n.name or b''
        n.cls, n.type, n.rows, n.cols = t.cls, t.type, t.rows, t.cols
        n.elements, n.members = t.count, len(t.members)
        acc = Acc()
        self.readval(t, v, acc)
        n.store, n.types, n.oids = acc.store, acc.types, acc.oids
        n.bytes = 4 * len(acc.store)
        return n.id

    # Lookups (fx-model.md 2.3).

    def by_name(self, parent, s):
        if s is None:                               # W: NULL gives the parent's own handle
            p = self.node(parent)
            return parent if p is not None and p.kind in (K_PARAM, K_ELEMENT, K_MEMBER, K_ANNOT) else 0
        if not s:                                   # W: "" is none
            return 0
        if parent == 0:
            return self.index.get(bytes(s), 0)
        p = self.node(parent)
        if p is None or p.kind not in (K_PARAM, K_ELEMENT, K_MEMBER):
            return 0
        return self.index.get(p.path + b'.' + bytes(s), 0)

    def by_semantic(self, parent, s):
        if parent == 0:
            cand = self.params
        else:
            p = self.node(parent)
            if p is None or p.kind not in (K_PARAM, K_ELEMENT, K_MEMBER) or p.cls != STRUCT:
                return 0
            cand = p.children[:p.members]         # W: the member count over members[], an array's elements there
        for i in cand:
            sem = self.nodes[i].sem
            if (s is None and sem is None) or (s is not None and sem is not None and lower(sem) == lower(bytes(s))):
                return i
        return 0

    def param(self, parent, i):
        if parent == 0:
            return self.params[i] if i < len(self.params) else 0
        p = self.node(parent)
        if p is None or p.kind not in (K_PARAM, K_ELEMENT, K_MEMBER) or p.elements or p.cls != STRUCT:
            return 0
        return p.children[i] if i < len(p.children) else 0

    def element(self, parent, i):
        if parent == 0:
            return self.params[i] if i < len(self.params) else 0
        p = self.node(parent)
        if p is None or p.kind not in (K_PARAM, K_ELEMENT, K_MEMBER) or not p.elements:
            return 0
        return p.children[i] if i < p.elements else 0

    def annot_at(self, owner, i):
        o = self.node(owner)
        if o is None or o.kind not in (K_PARAM, K_TECH, K_PASS):
            return 0
        return o.annots[i] if i < len(o.annots) else 0

    def annot_by_name(self, owner, s):
        o = self.node(owner)
        if o is None or o.kind not in (K_PARAM, K_TECH, K_PASS) or not s:   # W: NULL or "" is none
            return 0
        return next((a for a in o.annots if (self.nodes[a].name or b'') == bytes(s)), 0)

    def tech(self, i):
        return self.techs[i] if i < len(self.techs) else 0

    def tech_by_name(self, s):
        if s is None:                               # W: NULL is none ("" matches a name at offset 0)
            return 0
        return next((t for t in self.techs if (self.nodes[t].name or b'') == bytes(s)), 0)

    def pass_at(self, tech, i):
        t = self.node(tech)
        if t is None or t.kind != K_TECH:
            return 0
        return t.children[i] if i < len(t.children) else 0

    def pass_by_name(self, tech, s):
        t = self.node(tech)
        if t is None or t.kind != K_TECH or s is None:     # W: NULL is none
            return 0
        return next((p for p in t.children if (self.nodes[p].name or b'') == bytes(s)), 0)

    # Programs and the CTAB match (fx-model.md 3.1..3.4).

    def program(self, r, kind, target, op):
        blob = self.fx.resources[r][5]
        pr = Prog()
        pr.kind, pr.r, pr.ctab, pr.nconsts, pr.consts = kind, r, None, 0, []
        pr.outputs = 1 if target.type == STRING else target.rows * target.cols
        n = len(blob) // 4
        w = struct.unpack_from('<%dI' % n, blob)
        v = w[0] if n else 0
        pr.version = v
        # the shader row it sits on: W creates the code by the state's type and uploads its constants and sampler
        # states from the VERTEXSHADER and PIXELSHADER rows only, as the row says, so it takes effect only where
        # the row, the type and the version token agree (3.1, 3.4, 3.5)
        sc = SCLASSES[STATE_ROWS[op][0]] if op < len(STATE_ROWS) else None
        pr.row = 'vs' if sc == 'VSHADER' else 'ps' if sc == 'PSHADER' else None
        if kind == 'shader':
            pr.known = v >> 16 in (0xFFFE, 0xFFFF)
            pr.vs = v >> 16 == 0xFFFE
            pr.applied = (pr.row, target.type, v >> 16) in (('vs', VERTEXSHADER, 0xFFFE), ('ps', PIXELSHADER, 0xFFFF))
            found = shader_comments(w, v) if pr.known else []
        else:
            pr.known = v >> 16 == 0x4658
            pr.vs = False
            pr.applied = True
            found = F.comments(w, 1, n)[0] if pr.known else []
        first = next(((i, L) for i, L in found if F.fourcc(blob, i, L) == F.CTAB), None)
        if first is not None:
            reg = F.payload(blob, first[0], first[1])
            if len(reg) >= 28 and struct.unpack_from('<I', reg, 0)[0] == 28:     # W: the header's Size must be 28
                pr.ctab = reg
        if pr.ctab is not None:
            reg = pr.ctab
            pr.nconsts, info = struct.unpack_from('<2I', reg, 12)
            for k in range(pr.nconsts):
                o = info + 20 * k
                if o + 20 > len(reg):
                    c = Const(k)
                    c.why = 6
                    pr.consts.append(c)
                    break
                pr.consts.append(self.match(pr, reg, k, o))
        return pr

    def match(self, pr, reg, k, o):
        name, rset, rindex, rcount, _, tinfo, _ = struct.unpack_from('<I4H2I', reg, o)
        c = Const(k)
        c.nameq, c.rset, c.reg, c.count = F.ctab_str(reg, name), rset, rindex, rcount
        if name == 0 or name >= len(reg):
            c.why = 1
            return c
        pid = self.by_name(0, F.cut(reg[name:]))
        if not pid or self.nodes[pid].kind != K_PARAM:
            c.why = 1
            return c
        if tinfo + 16 > len(reg):
            c.why = 5
            return c
        ccls, ctyp, crows, ccols, celem, cmem = struct.unpack_from('<6H', reg, tinfo)
        c.ccls, c.ctyp = ccls, ctyp
        p = self.nodes[pid]
        nelem = max(1, p.elements)
        if rset == 3:                       # W's object branch: a sampler parameter under class OBJECT, its
            rcount = min(rcount, max(1, celem))     # RegisterCount cut to the constant's own elements (3.4)
            ok = pr.kind == 'shader' and p.cls == OBJECT and p.type in SAMPLERS and ccls == OBJECT and rcount <= nelem
        elif rset < 3:
            ok = (pr.kind == 'shader' or rset == 2) and (p.cls in NUMERIC and p.type in NUMTYPES and ccls in NUMERIC or
                                                         p.cls == STRUCT)
        else:
            ok = False
        if not ok:
            c.why = 2
            return c
        if rset != 3 and ((celem != p.elements if p.elements else celem > 1) or   # W: no count check on samplers;
                          cmem != p.members):       # a non-array takes 0 or 1; StructMembers = members (0: numeric)
            c.why = 4
            return c
        if p.cls == STRUCT:
            c.why = 7
            return c
        if rset < 3 and (crows, ccols) != (p.rows, p.cols):
            c.why = 3
            return c
        c.id, c.nelem = pid, nelem
        if rset == 3:
            c.sampler = True
            c.used = min(rcount, nelem)
            c.stage = rindex + (257 if pr.row == 'vs' else 0)        # W: by the row, not the version token
            return c
        if rset == 0:
            per = crows * ccols
        else:
            per = {SCALAR: crows * ccols, VECTOR: 1, MATRIX_ROWS: crows, MATRIX_COLUMNS: ccols}.get(ccls, crows * ccols)
        c.per = per
        c.layout = LAYOUTS.get(ccls) or F.unk(ccls)
        frm, to = TNAME[p.type], TNAME[SETTYPE[rset]]
        c.conv = frm if frm == to else frm + '>' + to
        c.used = sum(1 for e in range(nelem) if min(per, max(0, rcount - e * per)) > 0)
        c.cut = rcount < nelem * per
        return c

    def leaves(self, c):
        """[(leaf id, first register, count)] of a matched numeric constant, in register order (3.3)."""
        p = self.nodes[c.id]
        ids = p.children if p.elements else [p.id]
        return [(ids[e], c.reg + e * c.per, min(c.per, max(0, c.count - e * c.per))) for e in range(c.nelem)]

    # State sources, pass dependencies, technique versions (fx-model.md 3.5).

    def source(self, rs):
        """SOURCE of a state with resources rs (fx-model.md 5.6), without the literal case: the last one decides (W)."""
        r = rs[-1]
        usage, blob = self.fx.resources[r][4], self.fx.resources[r][5]
        pr = self.progs.get(r)
        if pr is not None and pr.kind == 'shader':
            if not pr.known:
                s = 'shader res %d %s' % (r, F.unk(pr.version))
            else:
                s = 'shader res %d %s ctab %s' % (r, vn(pr.version), pr.nconsts if pr.ctab is not None else '-')
        elif pr is not None:
            s = 'preshader res %d outputs %d ctab %s' % (r, pr.outputs, pr.nconsts if pr.ctab is not None else '-')
        elif usage == 1:
            i = self.by_name(0, F.cut(blob))
            s = 'param #%d' % i if i else 'param %s name %s' % (F.unk(1), F.qstr(F.cut(blob)))
        else:
            s = F.unk(usage)
        if len(rs) > 1:
            s += ' ' + F.unk(len(rs))
        return s

    def sampler_leaves(self, pid, used):
        """The sampler leaves a sampler constant binds: elements j < used (3.4; W loops to the RegisterCount).  A
        usage-1 reference to a sampler passes used 1: the node's own block, element 0's for an array (3.5: the model's
        choice, W reads the array's NULL data pointer)."""
        p = self.nodes[pid]
        return [self.nodes[i] for i in (p.children if p.elements else [pid])[:used] if self.nodes[i].block is not None]

    def scan(self, states, key, ids, samplers):
        """The parameters the program or usage-1 resource that decides each of these states reads (3.5)."""
        for k in range(len(states)):
            for r in (self.res_on.get(key + (k,), []) if key else [])[-1:]:
                pr = self.progs.get(r)
                if pr is not None:
                    for c in (pr.consts if pr.applied else []):     # a shader W never applies reads nothing
                        if c.why == 0:
                            if c.sampler:
                                if samplers is not None:
                                    samplers.append((c.id, c.used))
                            else:
                                ids.add(c.id)
                elif self.fx.resources[r][4] == 1:
                    i = self.by_name(0, F.cut(self.fx.resources[r][5]))
                    if i:
                        ids.add(self.nodes[i].top)
                        op = states[k][0]
                        if samplers is not None and op < len(STATE_ROWS) and SCLASSES[STATE_ROWS[op][0]] == 'SAMPLER' \
                                and self.nodes[i].type in SAMPLERS:
                            samplers.append((i, 1))         # SAMPLER[i] = <smp> (row 178) only: W applies its states

    def pass_info(self, tn, pn):
        ids, samplers = set(), []
        self.scan(pn.states, pn.skey, ids, samplers)
        for sid, used in dict.fromkeys(samplers):
            for leaf in self.sampler_leaves(sid, used):
                self.scan(leaf.block, leaf.skey, ids, None)
        pn.deps = sorted(i for i in ids if self.nodes[i].bytes > 0)
        for k in range(len(pn.states)):
            for r in self.res_on.get(pn.skey + (k,), [])[-1:]:          # the resource that decides
                pr = self.progs.get(r)
                if pr is None or pr.kind != 'shader' or not pr.applied:     # on its own shader row only
                    continue
                key = ((pr.version >> 8) & 0xFF) * 256 + (pr.version & 0xFF)
                if pr.row == 'vs':
                    if not tn.vs or key > ((tn.vs >> 8) & 0xFF) * 256 + (tn.vs & 0xFF):
                        tn.vs = pr.version
                elif not tn.ps or key > ((tn.ps >> 8) & 0xFF) * 256 + (tn.ps & 0xFF):
                    tn.ps = pr.version


def same(pa, ta, pb, tb):
    """The pool's match (fx-model.md 2.7): name, class, type, rows, columns, elements, members, recursively."""
    if (nm(pa.fx, ta.name) or b'') != (nm(pb.fx, tb.name) or b''):     # a name at offset 0 is "" (as its path)
        return False
    if (ta.cls, ta.type, ta.rows, ta.cols, ta.count, len(ta.members)) != \
            (tb.cls, tb.type, tb.rows, tb.cols, tb.count, len(tb.members)):
        return False
    return all(same(pa, ma, pb, mb) for ma, mb in zip(ta.members, tb.members))


class Clock:
    def __init__(self):
        self.t = 0

    def tick(self):
        self.t += 1
        return self.t


class Block:
    __slots__ = ('data', 'version')

    def __init__(self, data, version):
        self.data, self.version = data, version


class Slot:
    __slots__ = ('no', 'proto', 'nid', 'block', 'holders')


class Pool:
    """An ID3DXEffectPool's shared storage: slots by number, one version counter (fx-model.md 2.6, 2.7)."""
    def __init__(self):
        self.clock = Clock()
        self.slots = {}

    def live(self):
        return [self.slots[k] for k in sorted(self.slots)]


class Inst:
    """An effect: a Proto with storage, versions and its pool slots, and the Set/Get rules of fx-model.md 2.5."""

    def __init__(self, proto, pool=None):
        self.p, self.pool = proto, pool
        self.clock = pool.clock if pool is not None else Clock()
        self.blocks, self.slot = {}, {}
        for tid in proto.params:
            n = proto.nodes[tid]
            if pool is not None and n.flags & SHARED and n.type not in SAMPLERS:
                s = next((s for s in pool.live() if same(s.proto, s.proto.nodes[s.nid].t, proto, n.t)), None)
                if s is None:
                    s = Slot()
                    s.no = 0
                    while s.no in pool.slots:
                        s.no += 1
                    s.proto, s.nid, s.block, s.holders = proto, tid, Block(list(n.store), self.clock.tick()), []
                    pool.slots[s.no] = s
                else:
                    s.block.version = self.clock.tick()     # W: joining sets the slot dirty too (++counter)
                s.holders.append((self, tid))
                self.blocks[tid], self.slot[tid] = s.block, s.no
            else:
                self.blocks[tid] = Block(list(n.store), self.clock.tick())

    def free(self, drop=None):
        """Leave the pool; drop(ptr) for every non-zero object pointer whose storage dies here."""
        for tid in self.p.params:
            if tid in self.slot:
                s = self.pool.slots[self.slot[tid]]
                s.holders = [h for h in s.holders if not (h[0] is self and h[1] == tid)]
                if s.holders:
                    continue
                del self.pool.slots[s.no]
            blk = self.blocks[tid]
            for j in self.p.nodes[tid].objpos:
                if blk.data[j] and drop is not None:
                    drop(blk.data[j])
        self.blocks, self.slot = {}, {}

    # helpers

    def _pn(self, nid):
        """A node with storage that Set* may write: a parameter, element or member."""
        n = self.p.node(nid)
        return n if n is not None and n.kind in (K_PARAM, K_ELEMENT, K_MEMBER) else None

    def _rn(self, nid):
        """A node Get* may read: also an annotation (its file value)."""
        n = self.p.node(nid)
        return n if n is not None and n.kind in (K_PARAM, K_ELEMENT, K_MEMBER, K_ANNOT) else None

    def _get(self, n):
        if n.kind == K_ANNOT:
            return n.store[:], n.types
        top = self.p.nodes[n.top]
        k = n.bytes // 4
        return self.blocks[n.top].data[n.off:n.off + k], top.types[n.off:n.off + k]

    def _put(self, n, cells, always):
        """Write {relative dword: value}; the version rises when always, or when a byte changed."""
        blk = self.blocks[n.top]
        changed = 0
        for rel, v in cells.items():
            v &= 0xFFFFFFFF
            if blk.data[n.off + rel] != v:
                changed = 1
            blk.data[n.off + rel] = v
        if always or changed:
            blk.version = self.clock.tick()
        return OK, changed

    @staticmethod
    def _one(n):
        """1x1: W checks the shape only (any type; only the numeric classes have rows and columns)."""
        return n.rows == 1 and n.cols == 1 and n.elements == 0

    @classmethod
    def _colour(cls, n):
        """SetInt/GetInt's colour form after 1x1 (W): FLOAT VECTOR with columns != 2, FLOAT MATRIX_ROWS with one
        column and rows != 2.  Components: min(rows x cols, 4), so never past the storage."""
        return n.type == FLOAT and n.elements == 0 and not cls._one(n) and (
            (n.cls == VECTOR and n.cols != 2) or (n.cls == MATRIX_ROWS and n.cols == 1 and n.rows != 2))

    @staticmethod
    def _num(n, classes=NUMERIC):
        """The class check of the numeric calls: W looks at the class only, not the type."""
        return n is not None and n.cls in classes

    def version(self, nid):
        n = self._pn(nid)
        return self.blocks[n.top].version if n is not None else 0

    def data(self, nid):
        n = self._rn(nid)
        return None if n is None else self._get(n)[0]

    # Set and Get (fx-model.md 2.5): (result, value or None, changed)

    def set_value(self, nid, src, nbytes):
        n = self._pn(nid)
        if n is None:
            return INVALIDCALL, 0
        if n.cls == OBJECT and n.type in SAMPLERS:      # W: a sampler object (class and type)
            return FAIL, 0
        if nbytes < n.bytes:
            return INVALIDCALL, 0
        if n.type not in (0, BOOL, INT, FLOAT, STRING) + TEXTURES:    # W copies by the node's own type: any other
            return self._put(n, {}, True)                              # writes nothing, the version still rises
        return self._put(n, {i: src[i] for i in range(n.bytes // 4)}, True)

    def get_value(self, nid, nbytes):
        n = self._rn(nid)
        if n is None:
            return INVALIDCALL, None
        if n.cls == OBJECT and n.type in SAMPLERS:      # W: a sampler object (class and type)
            return FAIL, None
        if nbytes < n.bytes:
            return INVALIDCALL, None
        d = self._get(n)[0]
        return OK, [getbool(BOOL, w) for w in d] if n.type == BOOL else d    # W switches on the node's own type

    def set_bool(self, nid, b):
        n = self._pn(nid)
        if n is None or not self._one(n):
            return INVALIDCALL, 0
        return self._put(n, {0: conv(n.type, BOOL, b)}, True)

    def get_bool(self, nid):
        n = self._rn(nid)
        if n is None or not self._one(n):
            return INVALIDCALL, None
        return OK, conv(BOOL, n.type, self._get(n)[0][0])     # BOOL to BOOL is copied raw (W set_number)

    def set_int(self, nid, v):
        n = self._pn(nid)
        if n is not None and self._one(n):
            return self._put(n, {0: conv(n.type, INT, v)}, False)
        if n is not None and self._colour(n):
            comps = [(v >> 16) & 255, (v >> 8) & 255, v & 255, (v >> 24) & 255][:min(n.bytes // 4, 4)]
            return self._put(n, {i: fmul(b, INV255) for i, b in enumerate(comps)}, True)
        return INVALIDCALL, 0

    def get_int(self, nid):
        n = self._rn(nid)
        if n is not None and self._one(n):
            return OK, conv(INT, n.type, self._get(n)[0][0])
        if n is not None and self._colour(n):
            d = self._get(n)[0][:4]
            b = [to255(w) for w in d] + [0] * (4 - len(d))      # a component past the storage reads as 0
            return OK, (b[0] << 16) | (b[1] << 8) | b[2] | (b[3] << 24)
        return INVALIDCALL, None

    def set_float(self, nid, f):
        n = self._pn(nid)
        if n is None or not self._one(n):
            return INVALIDCALL, 0
        return self._put(n, {0: conv(n.type, FLOAT, f)}, False)

    def get_float(self, nid):
        n = self._rn(nid)
        if n is None or not self._one(n):
            return INVALIDCALL, None
        return OK, conv(FLOAT, n.type, self._get(n)[0][0])

    def set_array(self, nid, typ, src, count):
        """SetBoolArray, SetIntArray, SetFloatArray (typ BOOL, INT, FLOAT)."""
        n = self._pn(nid)
        if not self._num(n, (SCALAR, VECTOR, MATRIX_ROWS)) or typ not in NUMTYPES:
            return INVALIDCALL, 0
        frm = INT if typ == BOOL else typ           # D3DX does not crop BOOL input
        return self._put(n, {i: conv(n.type, frm, src[i]) for i in range(min(count, n.bytes // 4))}, True)

    def get_array(self, nid, typ, count):
        n = self._rn(nid)
        if not self._num(n) or typ not in NUMTYPES:
            return INVALIDCALL, None
        d = self._get(n)[0][:min(count, n.bytes // 4)]
        return OK, [conv(typ, n.type, w) for w in d]

    def set_vector(self, nid, v):
        n = self._pn(nid)
        if not self._num(n, (SCALAR, VECTOR)) or n.elements:
            return INVALIDCALL, 0
        if self._one(n) and n.type == INT:
            b = [to255(x) for x in v[:4]]
            return self._put(n, {0: (b[0] << 16) | (b[1] << 8) | b[2] | (b[3] << 24)}, True)
        cols = min(n.cols, 4, n.bytes // 4)        # never past the storage (W writes `columns` floats)
        return self._put(n, {c: v[c] if n.type == FLOAT else conv(n.type, FLOAT, v[c]) for c in range(cols)}, True)

    def get_vector(self, nid):
        n = self._rn(nid)
        if not self._num(n, (SCALAR, VECTOR)) or n.elements:
            return INVALIDCALL, None
        d = self._get(n)[0]
        if self._one(n) and n.type == INT:
            w = d[0]
            return OK, [fmul((w >> s) & 255, INV255) for s in (16, 8, 0, 24)]
        cols = min(n.cols, 4, len(d))
        return OK, [conv(FLOAT, n.type, d[c]) for c in range(cols)] + [0] * (4 - cols)

    def set_vector_array(self, nid, vs, count):
        n = self._pn(nid)
        if not self._num(n, (VECTOR,)) or not n.elements or n.elements < count:
            return INVALIDCALL, 0
        stride = n.rows * n.cols
        cols = min(n.cols, 4, stride)
        cells = {}
        for e in range(count):
            for c in range(cols):
                cells[e * stride + c] = vs[e][c] if n.type == FLOAT else conv(n.type, FLOAT, vs[e][c])
        return self._put(n, cells, True)

    def get_vector_array(self, nid, count):
        if not count:
            return OK, []                           # W returns before it looks at the handle
        n = self._rn(nid)
        if not self._num(n, (VECTOR,)) or n.elements < count:
            return INVALIDCALL, None
        d = self._get(n)[0]
        stride = n.rows * n.cols
        cols = min(n.cols, 4, stride)
        return OK, [[conv(FLOAT, n.type, d[e * stride + c]) for c in range(cols)] + [0] * (4 - cols)
                    for e in range(count)]

    def _mcells(self, n, m, base, transpose):
        cells = {}
        for i in range(min(n.rows, 4)):
            for k in range(min(n.cols, 4)):
                w = m[4 * k + i] if transpose else m[4 * i + k]
                cells[base + i * n.cols + k] = w if n.type == FLOAT else conv(n.type, FLOAT, w)
        return cells

    def _mget(self, n, d, base, transpose):
        out = [0] * 16
        for i in range(min(n.rows, 4)):
            for k in range(min(n.cols, 4)):
                out[4 * k + i if transpose else 4 * i + k] = conv(FLOAT, n.type, d[base + i * n.cols + k])
        return out

    def set_matrix(self, nid, m, transpose=False):
        n = self._pn(nid)
        if not self._num(n, (MATRIX_ROWS,)) or n.elements:
            return INVALIDCALL, 0
        return self._put(n, self._mcells(n, m, 0, transpose), True)

    def get_matrix(self, nid, transpose=False):
        n = self._rn(nid)
        if not self._num(n, (SCALAR, VECTOR, MATRIX_ROWS) if transpose else (MATRIX_ROWS,)) or n.elements:
            return INVALIDCALL, None
        return OK, self._mget(n, self._get(n)[0], 0, transpose and n.cls == MATRIX_ROWS)   # W: a vector in row 0

    def set_matrix_array(self, nid, ms, count, transpose=False):
        n = self._pn(nid)
        if not self._num(n, (MATRIX_ROWS,)) or n.elements < count:
            return INVALIDCALL, 0
        cells = {}
        for e in range(count):
            cells.update(self._mcells(n, ms[e], e * n.rows * n.cols, transpose))
        return self._put(n, cells, True)

    def get_matrix_array(self, nid, count, transpose=False):
        if not count:
            return OK, []                           # W returns before it looks at the handle
        n = self._rn(nid)
        if not self._num(n, (MATRIX_ROWS,)) or n.elements < count:
            return INVALIDCALL, None
        d = self._get(n)[0]
        return OK, [self._mget(n, d, e * n.rows * n.cols, transpose) for e in range(count)]

    @staticmethod
    def _obj(n, setting):
        """SetTexture/GetTexture: a texture type with elements 0; SetString any STRING node (W checks only the
        type: an array takes it in its first dword); GetString: STRING with elements 0.  No class check (W);
        a node without storage is refused."""
        if n.bytes < 4:
            return False
        if n.type in TEXTURES:
            return n.elements == 0
        return n.type == STRING and (setting or n.elements == 0)

    def set_object(self, nid, ptr):
        """SetTexture (version only when it changes) and SetString (always); returns (result, old, changed)."""
        n = self._pn(nid)
        if n is None or not self._obj(n, True):
            return INVALIDCALL, None, 0
        old = self.blocks[n.top].data[n.off]
        hr, ch = self._put(n, {0: ptr}, n.type == STRING)
        return hr, old, ch

    def get_object(self, nid):
        n = self._rn(nid)
        if n is None or not self._obj(n, False):
            return INVALIDCALL, None
        return OK, self._get(n)[0][0]


# The dump 'fxmodel 1' (fx-model.md 5).

class Dump:
    def __init__(self):
        self.lines = []

    def put(self, depth, s):
        self.lines.append('  ' * depth + s + '\n')

    def text(self):
        return ''.join(self.lines)


def typed(p, a):
    """TYPED of an annotation."""
    t = a.t
    if t.cls in NUMERIC:
        kind = TNAME.get(t.type, 'raw')
        if t.count == 0 and t.rows == 1 and t.cols == 1:
            w = a.store[0]
            return kind + ' ' + (F.f32(w) if t.type == FLOAT else F.i32(w) if t.type in (BOOL, INT) else F.hex8(w))
        return kind + ' ' + val(t.type, a.store)
    if t.cls == OBJECT and t.type not in SAMPLERS:
        if t.type == STRING:
            out = []
            for oid in a.oids:
                d = next((b for o, b in p.objdata if o == oid), None) if oid < p.fx.nobj else None
                out.append('string ' + (F.unk(oid) if d is None else F.qstr(F.cut(d))))
            return ' '.join(out)
        return ' '.join('obj ' + (str(oid) if oid < p.fx.nobj else F.unk(oid)) for oid in a.oids)
    return F.unk(t.cls)


def frag(p, t, pos):
    """FRAG of a literal state value at body offset pos."""
    if t.cls == OBJECT:
        oid = p.fx.du32(pos)
        return 'obj ' + (str(oid) if oid < p.fx.nobj else F.unk(oid))
    k = t.rows * t.cols
    return val(t.type, p.fx.dwords(pos, k) if k else ())


def dump_state(d, p, k, st, depth, rs):
    op, index, td, v = st
    if op < len(STATE_ROWS):
        cls, d3d = STATE_ROWS[op]
        head = 'state %d %s[%d] %s %d' % (k, F.STATES[op], index, SCLASSES[cls], d3d)
    else:
        head = 'state %d %s[%d] - -' % (k, F.unk(op), index)
    if not rs:
        t = p.fx.typedef(td)[0]
        d.put(depth, head + (' literal = ' + frag(p, t, v) if simple(t) and t.count == 0 else
                             ' literal ' + F.unk(t.cls)))
        return
    d.put(depth, head + ' ' + p.source(rs))
    pr = p.progs.get(rs[-1])
    if pr is not None and (pr.kind == 'preshader' or pr.known):
        for c in pr.consts:
            d.put(depth + 1, c.line())


def dump_node(d, e, n, depth):
    p = e.p
    data = e.blocks[n.top].data
    tail = ''
    if n.elements == 0:
        if n.cls in NUMERIC:
            tail = ' = ' + val(n.type, data[n.off:n.off + n.rows * n.cols])
        elif n.cls == OBJECT and n.type in POINTERS:
            tail = ' = obj %s data %s' % (str(n.object) if n.object < p.fx.nobj else F.unk(n.object),
                                          F.hex8(data[n.off]))
        elif n.cls not in (OBJECT, STRUCT):
            tail = ' = ' + F.unk(n.cls)
    if n.kind == K_PARAM:
        d.put(depth, 'param #%d %d path %s %s sem %s flags %s bytes %d shared %s annotations %d%s' % (
            n.id, n.index, F.qstr(n.path), tt(n.t), q(n.sem), F.hex8(n.flags), n.bytes,
            e.slot.get(n.id, '-'), len(n.annots), tail))
    elif n.kind == K_ELEMENT:
        d.put(depth, 'element #%d %d path %s %s flags %s bytes %d%s' % (
            n.id, n.index, F.qstr(n.path), tt(n.t, True), F.hex8(n.flags), n.bytes, tail))
    else:
        d.put(depth, 'member #%d %d path %s %s name %s sem %s flags %s bytes %d%s' % (
            n.id, n.index, F.qstr(n.path), tt(n.t), q(n.name), q(n.sem), F.hex8(n.flags), n.bytes, tail))
    if n.block is not None:
        d.put(depth + 1, 'sampler states %d' % len(n.block))
        for k, st in enumerate(n.block):
            dump_state(d, p, k, st, depth + 2, p.res_on.get(n.skey + (k,), []) if n.skey else [])
    for c in n.children:
        dump_node(d, e, p.nodes[c], depth + 1)
    for a in n.annots:
        dump_annot(d, p, p.nodes[a], depth + 1)


def dump_annot(d, p, a, depth):
    d.put(depth, 'annotation #%d %d name %s %s flags %s = %s' % (a.id, a.index, q(a.name), tt(a.t),
                                                               F.hex8(a.flags), typed(p, a)))


def dump_all(insts, pool):
    d = Dump()
    d.put(0, 'fxmodel 1')
    d.put(0, 'effects %d' % len(insts))
    for i, e in enumerate(insts):
        p = e.p
        d.put(0, 'effect %d %s size %d sha256 %s' % (i, F.qstr(p.name.encode()), p.size, p.sha))
        d.put(0, 'desc parameters %d techniques %d nodes %d shared %d' % (p.fx.nparams, p.fx.ntech,
                                                                         len(p.nodes) - 1, len(e.slot)))
        for tid in p.params:
            dump_node(d, e, p.nodes[tid], 1)
        for tid in p.techs:
            tn = p.nodes[tid]
            d.put(1, 'technique #%d %d name %s passes %d annotations %d vs %s ps %s' % (
                tn.id, tn.index, q(tn.name), len(tn.children), len(tn.annots), vn(tn.vs) if tn.vs else '-',
                vn(tn.ps) if tn.ps else '-'))
            for a in tn.annots:
                dump_annot(d, p, p.nodes[a], 2)
            for pid in tn.children:
                pn = p.nodes[pid]
                d.put(2, 'pass #%d %d name %s states %d annotations %d deps %d%s' % (
                    pn.id, pn.index, q(pn.name), len(pn.states), len(pn.annots), len(pn.deps),
                    ''.join(' #%d' % i for i in pn.deps)))
                for a in pn.annots:
                    dump_annot(d, p, p.nodes[a], 3)
                for k, st in enumerate(pn.states):
                    dump_state(d, p, k, st, 3, p.res_on.get(pn.skey + (k,), []))
    slots = pool.live()
    d.put(0, 'pool slots %d' % len(slots))
    order = {id(e): i for i, e in enumerate(insts)}
    for s in slots:
        first = s.proto.nodes[s.nid]
        if first.cls == OBJECT:
            v = 'data ' + (' '.join(F.hex8(w) for w in s.block.data) or '-')
        else:
            v = val(first.type, s.block.data)
        d.put(1, 'slot %d name %s %s bytes %d holders %d = %s' % (s.no, q(first.name), tt(first.t), first.bytes,
                                                                  len(s.holders), v))
        held = set()
        for e, tid in sorted(s.holders, key=lambda h: (order[id(h[0])], h[1])):
            d.put(2, 'holder %d %s #%d' % (order[id(e)], F.qstr(e.p.name.encode()), tid))
            held.add((id(e), tid))
        for i, e in enumerate(insts):
            for tid in e.p.params:
                n = e.p.nodes[tid]
                if (n.name or b'') == (first.name or b'') and (id(e), tid) not in held:
                    d.put(2, 'apart %d %s #%d flags %s' % (i, F.qstr(e.p.name.encode()), tid, F.hex8(n.flags)))
    d.put(0, 'end')
    return d.text()


# Loading.

def load(indir):
    """The Protos of every <indir>/*.fxo in byte order of NAME (fx-model.md 5.1); exits on a refused file."""
    names = sorted((os.fsencode(f[:-4]), f) for f in os.listdir(indir) if f.endswith('.fxo'))
    if not names:
        sys.exit('fxmodel: no .fxo files in %s' % indir)
    out = []
    for _, f in names:
        try:
            with open(os.path.join(indir, f), 'rb') as fh:
                out.append(Proto(f[:-4], fh.read()))
        except F.Bad as err:
            raise SystemExit('fxmodel: %s: %s' % (f, err))
    return out


def build(protos):
    pool = Pool()
    return [Inst(p, pool) for p in protos], pool


# Totals for the count table and the checks.

def totals(protos, insts=None, pool=None):
    c = {k: 0 for k in ('effects', 'nodes', 'params', 'elements', 'members', 'annots', 'techs', 'passes',
                        'programs', 'shaders', 'preshaders', 'shader_ctabs', 'preshader_ctabs', 'consts',
                        'set_c', 'set_s', 'unmatched', 'deps', 'deps_max')}
    c['cats'], c['versions'], c['samplers'] = {}, {}, {}
    for p in protos:
        c['effects'] += 1
        c['nodes'] += len(p.nodes) - 1
        for n in p.nodes[1:]:
            c[{K_PARAM: 'params', K_ELEMENT: 'elements', K_MEMBER: 'members', K_ANNOT: 'annots', K_TECH: 'techs',
               K_PASS: 'passes'}[n.kind]] += 1
            if n.kind == K_PASS:
                c['deps'] += len(n.deps)
                c['deps_max'] = max(c['deps_max'], len(n.deps))
            if n.kind == K_TECH:
                key = (vn(n.vs) if n.vs else '-', vn(n.ps) if n.ps else '-')
                c['versions'][key] = c['versions'].get(key, 0) + 1
        for pr in p.progs.values():
            c['programs'] += 1
            c[pr.kind + 's'] += 1
            if pr.ctab is not None:
                c[pr.kind + '_ctabs'] += 1
            for k in pr.consts:
                c['consts'] += 1
                if k.why:
                    c['unmatched'] += 1
                    continue
                c['set_' + F.SETS[k.rset]] = c.get('set_' + F.SETS[k.rset], 0) + 1
                cat = (pr.kind, F.SETS[k.rset], 'sampler') if k.sampler else (pr.kind, F.SETS[k.rset], k.conv,
                                                                            k.layout)
                n, cut = c['cats'].get(cat, (0, 0))
                c['cats'][cat] = (n + 1, cut + k.cut)
                if k.sampler:
                    sk = ('vs' if pr.vs else 'ps', F.type_name(k.ctyp), k.count)
                    c['samplers'][sk] = c['samplers'].get(sk, 0) + 1
    if pool is not None:
        slots = pool.live()
        c['slots'] = len(slots)
        c['holders'] = sum(len(s.holders) for s in slots)
        c['dist'] = sorted((len(s.holders) for s in slots), reverse=True)
    return c


def main_dump(indir, outdir):
    target = os.path.join(outdir, 'all.txt')
    try:
        protos = load(indir)
    except SystemExit:
        if os.path.exists(target):
            os.remove(target)
        raise
    insts, pool = build(protos)
    text = dump_all(insts, pool)
    os.makedirs(outdir, exist_ok=True)
    with open(target, 'w', newline='\n') as out:
        out.write(text)
    print('%-28s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s' % ('effect', 'nodes', 'params', 'elems', 'membs',
                                                            'annots', 'techs', 'passes', 'progs', 'consts',
                                                            'shared'))
    for e in insts:
        c = totals([e.p])
        print('%-28s %6d %6d %6d %6d %6d %6d %6d %6d %6d %6d' % (e.p.name, c['nodes'], c['params'], c['elements'],
                                                                c['members'], c['annots'], c['techs'],
                                                                c['passes'], c['programs'], c['consts'],
                                                                len(e.slot)))
    c = totals(protos, insts, pool)
    print('%-28s %6d %6d %6d %6d %6d %6d %6d %6d %6d %6d' % ('total', c['nodes'], c['params'], c['elements'],
                                                            c['members'], c['annots'], c['techs'], c['passes'],
                                                            c['programs'], c['consts'], c['holders']))
    apart = text.count('\n    apart ')
    print('pool: %d slots, %d holders, %d apart; %d lines, %d UNKNOWN' % (c['slots'], c['holders'], apart,
                                                                         text.count('\n'), text.count('UNKNOWN(')))
    print('fxmodel: %d effects into %s' % (len(insts), target))


# --check: the model against tools/fx2dump.py's dumps and fx-model.md's numbers.

QS = r'("(?:[^"\\]|\\.)*"|-)'


def parse_b(text):
    """The parts of an 'fx2dump 1' dump the model can be checked against."""
    import re
    out = {'params': [], 'techs': [], 'res': {}}
    lines = text.split('\n')
    cur = None
    for ln in lines:
        m = re.match(r'header .* params (\d+) techniques (\d+) ', ln)
        if m:
            out['P'], out['T'] = int(m.group(1)), int(m.group(2))
            continue
        m = re.match(r'param (\d+) (.+?) name %s sem %s flags (0x[0-9a-f]{8}) annotations (\d+)(?: = (.*))?$' %
                     (QS, QS), ln)
        if m:
            cur = {'tt': m.group(2), 'name': m.group(3), 'sem': m.group(4), 'flags': m.group(5),
                   'nannots': int(m.group(6)), 'values': [] if m.group(7) is None else [m.group(7)], 'annots': [],
                   'samplers': [], 'kind': 'param'}
            out['params'].append(cur)
            continue
        m = re.match(r'technique (\d+) name %s annotations (\d+) passes (\d+)$' % QS, ln)
        if m:
            cur = {'name': m.group(2), 'nannots': int(m.group(3)), 'npasses': int(m.group(4)), 'annots': [],
                   'passes': [], 'kind': 'tech'}
            out['techs'].append(cur)
            continue
        m = re.match(r'  pass (\d+) name %s annotations (\d+) states (\d+)$' % QS, ln)
        if m:
            tech = out['techs'][-1]
            cur = {'name': m.group(2), 'nannots': int(m.group(3)), 'nstates': int(m.group(4)), 'annots': [],
                   'states': [], 'kind': 'pass'}
            tech['passes'].append(cur)
            continue
        m = re.match(r' *annotation (\d+) (.+?) name %s sem %s(?: = (.*))?$' % (QS, QS), ln)
        if m:
            cur['annots'].append((m.group(2), m.group(3), m.group(5)))
            continue
        m = re.match(r'  value(?: \[\d+\])? = (.*)$', ln)
        if m and cur and cur['kind'] == 'param':
            cur['values'].append(m.group(1))
            continue
        m = re.match(r'  sampler(?: \[\d+\])? states (\d+)$', ln)
        if m and cur and cur['kind'] == 'param':
            cur['samplers'].append([])
            continue
        m = re.match(r'( *)state (\d+) (\S+) op \d+ (.+?)(?: = (.+?))?((?: res \d+)*)$', ln)
        if m:
            st = (int(m.group(2)), m.group(3), m.group(5), [int(x) for x in m.group(6).split()[1::2]])
            if cur['kind'] == 'pass':
                cur['states'].append(st)
            else:
                cur['samplers'][-1].append(st)
            continue
        m = re.match(r'resource (\d+) ', ln)
        if m:
            cur = {'kind': 'res', 'consts': [], 'nconsts': None, 'vn': None}
            out['res'][int(m.group(1))] = cur
            continue
        m = re.match(r'  ctab .* constants (\d+)$', ln)
        if m and cur and cur['kind'] == 'res':
            cur['nconsts'] = int(m.group(1))
            continue
        m = re.match(r'  shader (\S+) 0x', ln)
        if m and cur and cur['kind'] == 'res':
            cur['vn'] = m.group(1)
            continue
        m = re.match(r'  const (\d+) (.*)$', ln)
        if m and cur and cur['kind'] == 'res':
            parts = re.match(r'%s (\S+) count (\d+) ' % QS, m.group(2))
            cur['consts'].append((int(m.group(1)),) + ((parts.group(1), parts.group(2), int(parts.group(3)))
                                                       if parts else (m.group(2),)))
    return out


def check_b(protos, bdir):
    """Compare each Proto with tools/fx2dump.py's dump of the same file; returns a list of failures."""
    bad, seen = [], {'params': 0, 'values': 0, 'annotations': 0, 'states': 0, 'programs': 0, 'constants': 0}
    pool = Pool()
    for p in protos:
        e = Inst(p, pool)
        path = os.path.join(bdir, p.name + '.txt')
        if not os.path.exists(path):
            bad.append('%s: no dump in %s' % (p.name, bdir))
            continue
        b = parse_b(open(path).read())

        def want(cond, what):
            if not cond:
                bad.append('%s: %s' % (p.name, what))
        want(b['P'] == p.fx.nparams == len(p.params) == len(b['params']), 'parameter count')
        want(b['T'] == p.fx.ntech == len(p.techs) == len(b['techs']), 'technique count')
        seen['annotations'] += sum(len(x['annots']) for x in b['params'] + b['techs'] +
                                   [y for t in b['techs'] for y in t['passes']])
        seen['states'] += sum(len(y['states']) for t in b['techs'] for y in t['passes']) + \
            sum(len(z) for x in b['params'] for z in x['samplers'])
        for i, (tid, bp) in enumerate(zip(p.params, b['params'])):
            n = p.nodes[tid]
            seen['params'] += 1
            seen['values'] += len(bp['values'])
            want(bp['tt'] == tt(n.t) and bp['name'] == q(n.name) and bp['sem'] == q(n.sem) and
                 bp['flags'] == F.hex8(n.flags) and bp['nannots'] == len(n.annots), 'param %d header' % i)
            leaves = [p.nodes[c] for c in n.children] if n.elements else [n]
            if n.cls == OBJECT and n.type in SAMPLERS:      # only sampler objects have blocks
                want(len(bp['samplers']) == len(leaves), 'param %d sampler blocks' % i)
                for leaf, bs in zip(leaves, bp['samplers']):
                    want(len(bs) == len(leaf.block), 'param %d sampler states' % i)
                    for k, st in enumerate(leaf.block):
                        if k < len(bs):
                            check_state(p, st, leaf.skey + (k,) if leaf.skey else None, bs[k], want,
                                        'param %d sampler state %d' % (i, k))
            elif n.cls in NUMERIC or n.cls == OBJECT:
                mine = []
                for leaf in leaves:
                    if leaf.cls == OBJECT:
                        mine.append('obj %d' % leaf.object)
                    else:
                        mine.append(val(leaf.type, e.blocks[tid].data[leaf.off:leaf.off + leaf.rows * leaf.cols]))
                want(mine == bp['values'], 'param %d values' % i)
            check_annots(p, n, bp, want, 'param %d' % i)
        for ti, (tid, bt) in enumerate(zip(p.techs, b['techs'])):
            tn = p.nodes[tid]
            want(bt['name'] == q(tn.name) and bt['nannots'] == len(tn.annots) and
                 bt['npasses'] == len(tn.children), 'technique %d header' % ti)
            check_annots(p, tn, bt, want, 'technique %d' % ti)
            for j, (pid, bpass) in enumerate(zip(tn.children, bt['passes'])):
                pn = p.nodes[pid]
                want(bpass['name'] == q(pn.name) and bpass['nannots'] == len(pn.annots) and
                     bpass['nstates'] == len(pn.states) == len(bpass['states']), 'pass %d/%d header' % (ti, j))
                check_annots(p, pn, bpass, want, 'pass %d/%d' % (ti, j))
                for k, st in enumerate(pn.states):
                    if k < len(bpass['states']):
                        check_state(p, st, pn.skey + (k,), bpass['states'][k], want, 'pass %d/%d state %d' %
                                    (ti, j, k))
        for r, pr in p.progs.items():
            br = b['res'].get(r)
            if br is None:
                want(False, 'resource %d missing in the dump' % r)
                continue
            if pr.kind == 'shader' and pr.known:
                want(br['vn'] == vn(pr.version), 'resource %d version' % r)
            want(br['nconsts'] == (pr.nconsts if pr.ctab is not None else None), 'resource %d ctab count' % r)
            mine = [(c.k, c.nameq, (F.SETS[c.rset] if c.rset < 4 else F.unk(c.rset)) + str(c.reg), c.count)
                    for c in pr.consts if c.why != 6]
            theirs = [c for c in br['consts'] if len(c) == 4]
            want(mine == theirs, 'resource %d constants (%d against %d)' % (r, len(mine), len(theirs)))
            seen['programs'] += 1
            seen['constants'] += len(theirs)
    return bad, seen


def check_state(p, st, key, bst, want, what):
    k, namei, bfrag, bres = bst
    op, index = st[0], st[1]
    want(namei == '%s[%d]' % (F.STATES[op] if op < 182 else F.unk(op), index), what + ' name')
    rs = p.res_on.get(key, []) if key else []
    want(rs == bres, what + ' resources')
    if not rs:
        t = p.fx.typedef(st[2])[0]
        if simple(t) and t.count == 0:
            want(frag(p, t, st[3]) == bfrag, what + ' literal')


def check_annots(p, owner, bo, want, what):
    want(len(bo['annots']) == len(owner.annots), what + ' annotation count')
    for k, (aid, (btt, bname, bfrag)) in enumerate(zip(owner.annots, bo['annots'])):
        a = p.nodes[aid]
        want(btt == tt(a.t) and bname == q(a.name), '%s annotation %d header' % (what, k))
        mine = typed(p, a)
        if a.t.cls in NUMERIC and a.t.count == 0:
            want(mine.split(' ', 1)[1] == bfrag, '%s annotation %d value' % (what, k))


# fx-model.md 3.1, 3.6 and 7 (counts on Most Wanted's 31 effects; the A/B equality is the contract, these are
# the design's first-run checks).
MW_EXPECT = {
    'effects': 31, 'nodes': 951, 'params': 533, 'elements': 154, 'members': 0, 'annots': 101, 'techs': 76,
    'passes': 87, 'programs': 758, 'shaders': 142, 'preshaders': 616, 'shader_ctabs': 140, 'preshader_ctabs': 616,
    'consts': 1355, 'set_c': 1184, 'set_s': 171, 'unmatched': 0, 'deps': 1145, 'deps_max': 29, 'slots': 20,
    'holders': 156,
}
MW_CATS = {('preshader', 'c', 'int>float', 'scalar'): (597, 288), ('preshader', 'c', 'float', 'columns'): (7, 0),
           ('preshader', 'c', 'float', 'vector'): (6, 0), ('shader', 'c', 'float', 'vector'): (365, 3),
           ('shader', 'c', 'float', 'columns'): (110, 26), ('shader', 'c', 'float', 'scalar'): (99, 0),
           ('shader', 's', 'sampler'): (171, 0)}
MW_VERSIONS = {('-', '-'): 16, ('vs_1_1', 'ps_1_1'): 21, ('vs_1_1', 'ps_1_4'): 1, ('vs_1_1', 'ps_2_0'): 18,
               ('vs_1_1', 'ps_2_x'): 18, ('vs_3_0', 'ps_3_0'): 2}
MW_SAMPLERS = {('ps', 'SAMPLER2D', 1): 165, ('ps', 'SAMPLERCUBE', 1): 6}
MW_DIST = [27, 23, 19] + [7] * 7 + [6] * 5 + [3, 2, 1, 1, 1]
POOL_NAMES = {b'ColorWriteMode': (19, 1), b'BlendState': (23, 3)}      # GAME.md 5.5, fx-model.md 2.7


def main_check(indir, bdir):
    protos = load(indir)
    bad, seen = check_b(protos, bdir)
    print('against %s: %d effects, %s' % (bdir, len(protos), 'all agree' if not bad else '%d failures' % len(bad)))
    print('  compared: %s' % ', '.join('%d %s' % (v, k) for k, v in seen.items()))
    for x in bad[:40]:
        print('  ' + x)
    insts, pool = build(protos)
    text = dump_all(insts, pool)
    c = totals(protos, insts, pool)
    c['apart'] = text.count('\n    apart ')
    c['unknown'] = text.count('UNKNOWN(')
    fails = len(bad)
    if c['effects'] == 31:
        exp = dict(MW_EXPECT, apart=9, unknown=0)
        for k, v in exp.items():
            ok = c[k] == v
            fails += not ok
            print('  %-16s %6d %s' % (k, c[k], 'ok' if ok else 'expected %d' % v))
        for name, d in (('categories', MW_CATS), ('versions', MW_VERSIONS), ('samplers', MW_SAMPLERS)):
            got = c['cats' if name == 'categories' else name]
            ok = got == d
            fails += not ok
            print('  %-16s %s' % (name, 'ok (%s)' % '; '.join('%s %s' % ('/'.join(map(str, k)), v)
                                                              for k, v in sorted(d.items()))
                                  if ok else 'got %r' % sorted(got.items())))
        ok = c['dist'] == MW_DIST
        fails += not ok
        print('  %-16s %s' % ('holders/slot', ' '.join(map(str, c['dist'])) + ('' if ok else ' expected %s' % MW_DIST)))
        for name, (h, a) in POOL_NAMES.items():
            s = [s for s in pool.live() if s.proto.nodes[s.nid].name == name]
            got = (len(s), len(s[0].holders) if s else 0, sum(1 for e in insts for t in e.p.params
                                                               if e.p.nodes[t].name == name and t not in e.slot))
            ok = got == (1, h, a)
            fails += not ok
            print('  %-16s 1 slot, %d holders, %d apart%s' % (name.decode(), got[1], got[2], '' if ok else
                                                               ' expected 1 slot, %d holders, %d apart' % (h, a)))
        shared = [s.proto.nodes[s.nid] for s in pool.live()]
        print('  shared types     %s' % ', '.join('%d %s' % (v, k) for k, v in sorted(
            {tt(n.t): sum(1 for m in shared if tt(m.t) == tt(n.t)) for n in shared}.items())))
    print('fxmodel --check: %s' % ('ok' if not fails else '%d failures' % fails))
    return 1 if fails else 0


# Made-up effects (fx2-format.md 2) for the self-tests: a small writer.

class T:
    def __init__(self, type, cls, name=None, sem=None, count=0, rows=1, cols=1, members=()):
        self.type, self.cls, self.name, self.sem, self.count = type, cls, name, sem, count
        self.rows, self.cols, self.members = rows, cols, list(members)


def fl(x):
    return f2w(x)


class Syn:
    def __init__(self):
        self.body = bytearray(4)
        self.params, self.techs, self.objd, self.res, self.nobj = [], [], [], [], 1

    def put(self, b):
        off = len(self.body)
        self.body += b + b'\x5a' * (-len(b) % 4)       # pad bytes are not zero (fx2-format.md 1)
        return off

    def name(self, s):
        return 0 if s is None else self.put(struct.pack('<I', len(s) + 1) + s + b'\0')

    def tdbytes(self, t):
        na, se = self.name(t.name), self.name(t.sem)
        mem = [self.tdbytes(m) for m in t.members]
        w = struct.pack('<5I', t.type, t.cls, na, se, t.count)
        if t.cls in NUMERIC:
            w += struct.pack('<2I', *((t.cols, t.rows) if t.cls == VECTOR else (t.rows, t.cols)))
        elif t.cls == STRUCT:
            w += struct.pack('<I', len(mem)) + b''.join(mem)
        return w

    def td(self, t):
        b = self.tdbytes(t)
        return self.put(b)

    def val(self, ws):
        return self.put(struct.pack('<%dI' % len(ws), *[w & 0xFFFFFFFF for w in ws]))

    def obj(self, data=None):
        oid = self.nobj
        self.nobj += 1
        if data is not None:
            self.objd.append((oid, data))
        return oid

    def state(self, op, idx, t, ws):
        return (op, idx, self.td(t), self.val(ws))

    def param(self, t, ws, flags=0, annos=()):
        self.params.append((self.td(t), self.val(ws), flags, [(self.td(a), self.val(v)) for a, v in annos]))
        return len(self.params) - 1

    def sampler(self, states):
        """The value of one sampler element: its count, then the state entries."""
        return [len(states)] + [w for st in states for w in st]

    def tech(self, name, annos, passes):
        self.techs.append((self.name(name), [(self.td(a), self.val(v)) for a, v in annos],
                           [(self.name(pn), [(self.td(a), self.val(v)) for a, v in pa], sts) for pn, pa, sts in passes]))
        return len(self.techs) - 1

    def resource(self, tech, ps, elem, state, usage, blob):
        self.res.append((tech, ps, elem, state, usage, blob))

    def build(self):
        tables = len(self.body)
        t = struct.pack('<4I', len(self.params), len(self.techs), 0, self.nobj)
        for td, v, flags, annos in self.params:
            t += struct.pack('<4I', td, v, flags, len(annos)) + b''.join(struct.pack('<2I', *a) for a in annos)
        for name, annos, passes in self.techs:
            t += struct.pack('<3I', name, len(annos), len(passes)) + b''.join(struct.pack('<2I', *a) for a in annos)
            for pn, pa, sts in passes:
                t += struct.pack('<3I', pn, len(pa), len(sts)) + b''.join(struct.pack('<2I', *a) for a in pa)
                t += b''.join(struct.pack('<4I', *s) for s in sts)
        t += struct.pack('<2I', len(self.objd), len(self.res))
        for oid, data in self.objd:
            t += struct.pack('<2I', oid, len(data)) + data + b'\0' * (-len(data) % 4)
        for tech, ps, elem, state, usage, blob in self.res:
            t += struct.pack('<6I', tech, ps, elem, state, usage, len(blob)) + blob + b'\0' * (-len(blob) % 4)
        return struct.pack('<2I', 0xFEFF0901, tables) + bytes(self.body) + t


def ctab_comment(version, consts, extra=0):
    """A CTAB comment: header, type infos, strings, then the constant entries last, so that a count past them
    (extra) puts an entry outside the table.  consts: (name or None, set, reg, count, typeinfo 6-tuple or None)."""
    pay = bytearray(28)
    tinfo = []
    for c in consts:
        if c[4] is None:
            tinfo.append(0xFFF0)
        else:
            tinfo.append(len(pay))
            pay += struct.pack('<6HI', *c[4], 0)
    def s(b):
        off = len(pay)
        pay.extend(b + b'\0')
        return off
    creator, target = s(b'made up'), s(b'vs_1_1' if version >> 16 == 0xFFFE else b'ps_2_0' if version >> 16 ==
                                     0xFFFF else b'tx')
    names = [0 if c[0] is None else s(c[0]) for c in consts]
    pay.extend(b'\0' * (-len(pay) % 4))
    info = len(pay)
    for c, na, ti in zip(consts, names, tinfo):
        pay += struct.pack('<I4H2I', na, c[1], c[2], c[3], 0, ti, 0)
    struct.pack_into('<7I', pay, 0, 28, creator, version, len(consts) + extra, info, 0x100, target)
    ws = [0xFFFE | ((1 + len(pay) // 4) << 16), 0x42415443] + list(struct.unpack('<%dI' % (len(pay) // 4), pay))
    return ws


def shader_blob(version, consts, extra=0):
    ws = [version] + ctab_comment(version, consts, extra) + [F.END]
    return struct.pack('<%dI' % len(ws), *ws)


def preshader_blob(consts):
    ws = [0x46580200] + ctab_comment(0x46580200, consts) + [0xFFFE | (2 << 16), 0x54494C43, 0]
    ws += [0xFFFE | (4 << 16), 0x434C5846, 0, 0xF0F0F0F0, 0x0F0F0F0F, F.END]
    return struct.pack('<%dI' % len(ws), *ws)


I11 = T(INT, MATRIX_ROWS)                      # a state value: INT 1x1


def syn_main():
    """SYN_MAIN: every shape the Set/Get rules name, a struct, arrays, textures, a sampler with a usage-1 texture,
    annotations of three kinds, and programs whose CTABs give every reason of fx-model.md 3.2."""
    s = Syn()
    P = {}
    sid = s.obj(b'hello')
    P['b'] = s.param(T(BOOL, SCALAR, b'b'), [0], annos=[(T(STRING, OBJECT, b'label'), [sid])])
    P['i'] = s.param(T(INT, SCALAR, b'i', b'Count'), [7])
    P['f'] = s.param(T(FLOAT, SCALAR, b'f'), [fl(0.5)])
    P['v2'] = s.param(T(FLOAT, VECTOR, b'v2', cols=2), [fl(1), fl(2)])
    P['v3'] = s.param(T(FLOAT, VECTOR, b'v3', b'Colour', cols=3), [0, 0, 0])
    P['v4'] = s.param(T(FLOAT, VECTOR, b'v4', b'Tint', cols=4), [fl(1), fl(0.5), 0, fl(-0.1)], flags=1)
    P['m33'] = s.param(T(FLOAT, MATRIX_ROWS, b'm33', rows=3, cols=3), [0] * 9)
    P['m43'] = s.param(T(FLOAT, MATRIX_ROWS, b'm43', rows=4, cols=3), [0] * 12)
    P['m44'] = s.param(T(FLOAT, MATRIX_ROWS, b'm44', rows=4, cols=4), [fl(i) for i in range(16)])
    P['mc'] = s.param(T(FLOAT, MATRIX_COLUMNS, b'mc', rows=4, cols=4), [0] * 16)
    P['ia'] = s.param(T(INT, SCALAR, b'ia', b'BLEND', count=5), [1, 2, 3, 4, 5], flags=1)
    P['va'] = s.param(T(FLOAT, VECTOR, b'va', count=4, cols=4), [0] * 16)
    P['ma'] = s.param(T(FLOAT, MATRIX_ROWS, b'ma', count=2, rows=4, cols=4), [0] * 32)
    P['st'] = s.param(T(0, STRUCT, b'st', members=[T(FLOAT, VECTOR, b'pos', cols=3), T(INT, SCALAR, b'n', b'N')]),
                      [fl(1), fl(2), fl(3), 9])
    t1, t2, t3, t4 = s.obj(b''), s.obj(b''), s.obj(b''), s.obj(b'')
    P['sa'] = s.param(T(0, STRUCT, b'sa', count=2, members=[T(FLOAT, SCALAR, b'x'), T(5, OBJECT, b't')]),
                      [fl(1), t1, fl(2), t2])
    P['tex'] = s.param(T(5, OBJECT, b'tex'), [t3])
    t5 = s.obj(b'')
    P['texa'] = s.param(T(7, OBJECT, b'texa', count=2), [t4, t5])
    tobj = s.obj()
    P['smp'] = s.param(T(10, OBJECT, b'smp'), s.sampler([s.state(164, 256, T(5, OBJECT), [tobj]),
                                                        s.state(170, 256, I11, [2])]))
    P['bb'] = s.param(T(BOOL, SCALAR, b'bb'), [0])
    P['iv'] = s.param(T(INT, VECTOR, b'iv', cols=3), [0, 0, 0])
    P['ff'] = s.param(T(FLOAT, MATRIX_ROWS, b'ff', rows=4, cols=1), [0] * 4)
    s.resource(NONE, P['smp'], 0, 0, 1, b'tex\0')
    vs, ps, vs2, ptex, ptex2 = s.obj(), s.obj(), s.obj(), s.obj(), s.obj()
    sv = lambda c, r: (c, FLOAT, r[0], r[1], 1, 0)
    s.tech(b't0', [(T(INT, SCALAR, b'level'), [3]), (T(FLOAT, SCALAR, b'gain'), [fl(1.5)])], [
        (b'p0', [(T(INT, SCALAR, b'n'), [1])], [s.state(13, 0, I11, [0]), s.state(146, 0, T(VERTEXSHADER, OBJECT), [vs]),
                                                s.state(147, 0, T(PIXELSHADER, OBJECT), [ps]),
                                                s.state(127, 0, T(FLOAT, MATRIX_ROWS, rows=4, cols=1), [0] * 4)]),
        (b'p1', [], [s.state(146, 0, T(VERTEXSHADER, OBJECT), [vs2]), s.state(147, 0, I11, [0]),
                     s.state(200, 0, I11, [0]), s.state(9, 0, I11, [4]), s.state(164, 0, T(5, OBJECT), [ptex]),
                     s.state(10, 0, I11, [0]), s.state(164, 1, T(5, OBJECT), [ptex2]),
                     s.state(13, 0, T(INT, MATRIX_ROWS, count=2), [0, 1])])])
    s.resource(0, 0, NONE, 0, 0, preshader_blob([(b'ia', 2, 0, 3, (SCALAR, INT, 1, 1, 5, 0)),
                                                 (b'b', 0, 0, 1, (SCALAR, BOOL, 1, 1, 1, 0))]))
    s.resource(0, 0, NONE, 1, 0, shader_blob(0xFFFE0101, [(b'v4', 2, 4, 1, sv(VECTOR, (1, 4))),
                                                          (b'm44', 2, 0, 4, (MATRIX_COLUMNS, FLOAT, 4, 4, 1, 0)),
                                                          (b'va', 2, 8, 3, (VECTOR, FLOAT, 1, 4, 4, 0))]))
    s.resource(0, 0, NONE, 2, 0, shader_blob(0xFFFF0200, [(b'smp', 3, 2, 1, (OBJECT, 12, 1, 1, 1, 0)),
                                                          (b'f', 2, 0, 1, (SCALAR, FLOAT, 1, 1, 1, 0)),
                                                          (b'f', 2, 1, 1, (SCALAR, FLOAT, 1, 1, 1, 2)),   # 4 members
                                                          (b'va', 2, 2, 4, (VECTOR, FLOAT, 1, 4, 4, 1))]))  # 4 the same
    s.resource(0, 0, NONE, 3, 0, preshader_blob([(b'v4', 2, 0, 1, sv(VECTOR, (1, 4)))]))
    s.resource(0, 1, NONE, 0, 0, shader_blob(0xFFFE0101, [
        (b'v4', 2, 0, 1, sv(VECTOR, (1, 4))),                       # 0 matched
        (b'nosuch', 2, 1, 1, sv(VECTOR, (1, 4))),                   # 1 no such name
        (b'va[1]', 2, 2, 1, sv(VECTOR, (1, 4))),                    # 1 an element, not top level
        (None, 2, 3, 1, sv(VECTOR, (1, 4))),                        # 1 no name
        (b'tex', 2, 4, 1, (OBJECT, 5, 1, 1, 1, 0)),                 # 2 a texture in set c
        (b'v4', 3, 0, 1, (OBJECT, 12, 1, 1, 1, 0)),                 # 2 a vector in set s
        (b'v3', 2, 5, 1, sv(VECTOR, (1, 4))),                       # 3 size
        (b'va', 2, 6, 4, (VECTOR, FLOAT, 1, 4, 2, 0)),              # 4 elements
        (b'v4', 2, 7, 1, None),                                     # 5 type info outside
        (b'st', 2, 10, 2, (STRUCT, 0, 1, 4, 1, 2)),                 # 7 a struct
        (b'm44', 2, 12, 3, (MATRIX_COLUMNS, FLOAT, 4, 4, 1, 0)),    # 0 matched, registers cut
        (b'smp', 3, 0, 1, (OBJECT, 12, 1, 1, 1, 0)),                # 0 a sampler in a vertex shader
        (b'i', 1, 0, 1, (SCALAR, INT, 1, 1, 1, 0)),                 # 0 set i
        (b'bb', 0, 0, 1, (SCALAR, BOOL, 1, 1, 1, 0)),               # 0 set b
        (b'st', 9, 0, 1, (STRUCT, 0, 1, 4, 1, 2)),                  # 2 an unknown set
    ], extra=1))                                                    # 6 the entry past the table
    s.resource(0, 1, NONE, 4, 0, b'\0\0\0\0')                       # 6: usage 0 on a texture: UNK(usage)
    s.resource(0, 1, NONE, 5, 0, b'\0\0\0\0')                       # 7: a preshader of no known version
    s.resource(0, 1, NONE, 3, 1, b'iv\0')                           # 8, 9: two resources on one state; the
    s.resource(0, 1, NONE, 3, 1, b'f\0')                            # last decides, so iv is no dep (3.5)
    s.resource(0, 1, NONE, 6, 1, b'missing\0')                      # 10: a name that does not resolve
    return s.build(), P


def syn_pool():
    """SYN_POOL_A/B/C: shared parameters that match and that do not (fx-model.md 6.2, matching)."""
    a = Syn()
    a.param(T(INT, SCALAR, b'x'), [1], flags=1)
    a.param(T(FLOAT, VECTOR, b'y', cols=4), [fl(1), fl(2), fl(3), fl(4)], flags=1)
    a.param(T(INT, SCALAR, b'z', count=3), [1, 2, 3], flags=1)
    a.param(T(0, STRUCT, b'st', members=[T(INT, SCALAR, b'a')]), [5], flags=1)
    a.param(T(10, OBJECT, b'smp'), a.sampler([]), flags=1)
    a.param(T(5, OBJECT, b't'), [a.obj(b'')], flags=1)
    b = Syn()
    b.param(T(FLOAT, SCALAR, b'x'), [fl(2)], flags=1)                # another type
    b.param(T(FLOAT, VECTOR, b'y', cols=4), [0, 0, 0, 0], flags=1)    # the same: joins, its default is dropped
    b.param(T(INT, SCALAR, b'z', count=4), [9, 9, 9, 9], flags=1)    # another element count
    b.param(T(0, STRUCT, b'st', members=[T(INT, SCALAR, b'a'), T(INT, SCALAR, b'b')]), [6, 7], flags=1)
    b.param(T(10, OBJECT, b'smp'), b.sampler([]), flags=1)
    b.param(T(5, OBJECT, b't'), [b.obj(b'')], flags=1)
    c = Syn()
    c.param(T(FLOAT, VECTOR, b'y', cols=4), [0, 0, 0, 0])             # same name, not shared: apart
    c.param(T(0, STRUCT, b'st', members=[T(INT, SCALAR, b'b')]), [8], flags=1)   # member name differs
    return a.build(), b.build(), c.build()


def syn_unnamed():
    """SYN_UNNAMED: fx-model.md 2.3's empty names: an unnamed FLOAT4 with an unnamed annotation and one named k, a
    struct st with an unnamed member and a member y, a CTAB constant named "", a usage-1 reference to "", and an
    unnamed technique with an unnamed pass."""
    s = Syn()
    s.param(T(FLOAT, VECTOR, cols=4), [0] * 4, annos=[(T(INT, SCALAR), [5]), (T(INT, SCALAR, b'k'), [6])])
    s.param(T(0, STRUCT, b'st', members=[T(FLOAT, SCALAR), T(FLOAT, SCALAR, b'y')]), [fl(1), fl(2)])
    s.param(T(5, OBJECT, b'tex'), [s.obj(b'')])
    vs, tx = s.obj(), s.obj()
    s.tech(b't', [], [(b'p', [], [s.state(146, 0, T(VERTEXSHADER, OBJECT), [vs]),
                                  s.state(164, 0, T(5, OBJECT), [tx])])])
    s.tech(None, [], [(None, [], [])])
    s.resource(0, 0, NONE, 0, 0, shader_blob(0xFFFE0101, [(b'', 2, 0, 1, (VECTOR, FLOAT, 1, 4, 1, 0))]))
    s.resource(0, 0, NONE, 1, 1, b'\0')
    return s.build()


def syn_sampler_ref():
    """SYN_SAMPLER_REF: fx-model.md 3.5's row 178: SAMPLER[0] = <smp> and SAMPLER[1] = <smpa> as usage-1 references,
    smp's block taking <ta>, the blocks of the array smpa's two elements <tb> and <tc>; and TEXTURE[2] = <smpb>, a
    usage-1 reference on another row, smpb's block taking <td>."""
    s = Syn()
    for n in (b'ta', b'tb', b'tc'):
        s.param(T(5, OBJECT, n), [s.obj(b'')])
    block = lambda: s.sampler([s.state(164, 256, T(5, OBJECT), [s.obj()])])
    smp = s.param(T(10, OBJECT, b'smp'), block())
    smpa = s.param(T(10, OBJECT, b'smpa', count=2), block() + block())
    s.param(T(5, OBJECT, b'td'), [s.obj(b'')])
    smpb = s.param(T(10, OBJECT, b'smpb'), block())
    s.tech(b't', [], [(b'p', [], [s.state(178, 0, T(10, OBJECT), [0]), s.state(178, 1, T(10, OBJECT), [0]),
                                  s.state(164, 2, T(5, OBJECT), [s.obj()])])])
    s.resource(NONE, smp, 0, 0, 1, b'ta\0')
    s.resource(NONE, smpa, 0, 0, 1, b'tb\0')
    s.resource(NONE, smpa, 1, 0, 1, b'tc\0')
    s.resource(NONE, smpb, 0, 0, 1, b'td\0')
    s.resource(0, 0, NONE, 0, 1, b'smp\0')
    s.resource(0, 0, NONE, 1, 1, b'smpa\0')
    s.resource(0, 0, NONE, 2, 1, b'smpb\0')
    return s.build()


def syn_w():
    """SYN_W: where W goes by the type or the row (fx-model.md 2.4, 2.5, 3.1, 3.4): an INT and a VOID in class
    OBJECT (no storage), a VERTEXSHADER parameter (SetValue writes nothing), a ps_2_0 shader with a sampler and a
    vector constant on the VERTEXSHADER row and a vs_3_0 one with a sampler on the PIXELSHADER row (neither applied:
    stages by the row, no deps, no versions), and a vs_1_1 shader whose CTAB header has Size 0 (no table)."""
    s = Syn()
    s.param(T(INT, OBJECT, b'oi'), [s.obj()])
    s.param(T(0, OBJECT, b'ov'), [s.obj()])
    s.param(T(VERTEXSHADER, OBJECT, b'vsp'), [s.obj()])
    s.param(T(5, OBJECT, b't'), [s.obj()])
    smp = s.param(T(10, OBJECT, b'smp'), s.sampler([s.state(164, 256, T(5, OBJECT), [s.obj()])]))
    s.param(T(FLOAT, VECTOR, b'v4', cols=4), [0] * 4)
    s.tech(b'swapped', [], [(b'p', [], [s.state(146, 0, T(VERTEXSHADER, OBJECT), [s.obj()]),
                                        s.state(147, 0, T(PIXELSHADER, OBJECT), [s.obj()])])])
    s.tech(b'noctab', [], [(b'p', [], [s.state(146, 0, T(VERTEXSHADER, OBJECT), [s.obj()])])])
    s.resource(NONE, smp, 0, 0, 1, b't\0')
    s.resource(0, 0, NONE, 0, 0, shader_blob(0xFFFF0200, [(b'smp', 3, 0, 1, (OBJECT, 12, 1, 1, 1, 0)),
                                                          (b'v4', 2, 0, 1, (VECTOR, FLOAT, 1, 4, 1, 0))]))
    s.resource(0, 0, NONE, 1, 0, shader_blob(0xFFFE0300, [(b'smp', 3, 1, 1, (OBJECT, 12, 1, 1, 1, 0))]))
    blob = bytearray(shader_blob(0xFFFE0101, [(b'v4', 2, 0, 1, (VECTOR, FLOAT, 1, 4, 1, 0))]))
    struct.pack_into('<I', blob, 12, 0)         # the token, the comment token, 'CTAB', then the header's Size
    s.resource(1, 0, NONE, 0, 0, bytes(blob))
    return s.build()


def synth_files():
    m = syn_main()[0]
    a, b, c = syn_pool()
    return {'SYN_MAIN': m, 'SYN_POOL_A': a, 'SYN_POOL_B': b, 'SYN_POOL_C': c, 'SYN_UNNAMED': syn_unnamed(),
            'SYN_SAMPLER_REF': syn_sampler_ref(), 'SYN_W': syn_w()}


# --self-test

class Checker:
    def __init__(self):
        self.n, self.bad = 0, []

    def eq(self, what, got, want):
        self.n += 1
        if got != want:
            self.bad.append('%s: got %r, want %r' % (what, got, want))

    def ok(self, what, cond):
        self.eq(what, bool(cond), True)


def self_test(indir):
    ck = Checker()
    data, P = syn_main()
    p = Proto('SYN_MAIN', data)
    e = Inst(p)
    pid = {k: p.params[v] for k, v in P.items()}
    eq = ck.eq

    # conv() and the rows of fx-model.md 2.5
    eq('float 1.9 -> int', conv(INT, FLOAT, fl(1.9)), 1)
    eq('float -1.9 -> int', conv(INT, FLOAT, fl(-1.9)), 0xFFFFFFFF)
    eq('float NaN -> int', conv(INT, FLOAT, 0x7FC00000), 0x80000000)
    eq('float 3e9 -> int', conv(INT, FLOAT, fl(3e9)), 0x80000000)
    eq('float -3e9 -> int', conv(INT, FLOAT, fl(-3e9)), 0x80000000)
    eq('float -0.0 -> bool', conv(BOOL, FLOAT, 0x80000000), 1)
    eq('int -> float', conv(FLOAT, INT, 0xFFFFFFFF), fl(-1.0))
    eq('int 2^24+1 -> float rounds to even', conv(FLOAT, INT, (1 << 24) + 1), fl(16777216.0))
    eq('bool 5 -> bool', conv(BOOL, BOOL, 5), 5)
    eq('bool 5 -> int', conv(INT, BOOL, 5), 1)
    eq('bool 5 -> float', conv(FLOAT, BOOL, 5), fl(1.0))
    eq('int 5 -> bool', conv(BOOL, INT, 5), 1)
    eq('any -> string', conv(STRING, INT, 5), 0)

    i, f, b, bb = pid['i'], pid['f'], pid['b'], pid['bb']
    v0 = e.version(i)
    eq('SetFloat 1.9 on INT', e.set_float(i, fl(1.9)), (OK, 1))
    eq('INT after SetFloat 1.9', e.get_int(i), (OK, 1))
    v1 = e.version(i)
    ck.ok('SetFloat with a change raises the version', v1 > v0)
    eq('SetFloat -1.9 on INT', e.set_float(i, fl(-1.9)), (OK, 1))
    eq('INT after SetFloat -1.9', e.data(i), [0xFFFFFFFF])
    eq('SetFloat NaN on INT', (e.set_float(i, 0x7FC00000), e.data(i)), ((OK, 1), [0x80000000]))
    eq('SetFloat 3e9 on INT', (e.set_float(i, fl(3e9)), e.data(i)), ((OK, 0), [0x80000000]))
    v2 = e.version(i)
    eq('SetInt same value', e.set_int(i, 0x80000000), (OK, 0))
    eq('SetInt same value keeps the version', e.version(i), v2)
    eq('SetFloat same bytes keeps the version', (e.set_float(i, fl(3e9)), e.version(i)), ((OK, 0), v2))
    eq('SetInt new value', e.set_int(i, 42), (OK, 1))
    ck.ok('SetInt with a change raises the version', e.version(i) > v2)
    eq('SetFloat -0.0 on BOOL', (e.set_float(b, 0x80000000), e.data(b)), ((OK, 1), [1]))
    eq('SetBool 5 on BOOL stored raw', (e.set_bool(bb, 5), e.data(bb)), ((OK, 1), [5]))
    eq('GetBool after SetBool 5: raw (W set_number with equal types)', e.get_bool(bb), (OK, 5))
    eq('GetBoolArray after SetBool 5: raw', e.get_array(bb, BOOL, 1), (OK, [5]))
    eq('GetValue after SetBool 5', e.get_value(bb, 4), (OK, [1]))
    vb = e.version(bb)
    eq('SetBool same value', e.set_bool(bb, 5), (OK, 0))
    ck.ok('SetBool raises the version always', e.version(bb) > vb)
    eq('SetBool on FLOAT', (e.set_bool(f, 7), e.data(f)), ((OK, 1), [fl(1.0)]))
    eq('GetInt on FLOAT 1.0', e.get_int(f), (OK, 1))
    eq('SetBoolArray 5 on FLOAT', (e.set_array(f, BOOL, [5], 1), e.data(f)), ((OK, 1), [fl(5.0)]))
    eq('SetBoolArray 5 on BOOL', (e.set_array(bb, BOOL, [5], 1), e.data(bb)), ((OK, 1), [1]))
    eq('GetBoolArray on FLOAT', e.get_array(f, BOOL, 1), (OK, [1]))
    eq('GetFloatArray on INT', e.get_array(i, FLOAT, 4), (OK, [fl(42.0)]))

    v3, v4 = pid['v3'], pid['v4']
    eq('SetInt colour form on float3', e.set_int(v3, 0x80FF4000), (OK, 1))
    eq('float3 after SetInt', e.data(v3), [fmul(255, INV255), fmul(64, INV255), 0])
    eq('float3 SetInt 0xFF0000 gives 1.0', (e.set_int(v3, 0xFF0000), e.data(v3)[0]), ((OK, 1), fl(1.0)))
    eq('GetInt colour form on float3', (e.set_int(v3, 0x80FF4000), e.get_int(v3)), ((OK, 1), (OK, 0x00FF4000)))
    eq('SetInt colour form, same value: changed 0', e.set_int(v3, 0x80FF4000), (OK, 0))
    vv = e.version(v3)
    e.set_int(v3, 0x80FF4000)
    ck.ok('SetInt colour form raises the version always', e.version(v3) > vv)
    eq('SetInt colour form on float4', e.set_int(v4, 0x80FF4000), (OK, 1))
    eq('float4 after SetInt', e.data(v4), [fmul(255, INV255), fmul(64, INV255), 0, fmul(128, INV255)])
    eq('GetInt colour form on float4', e.get_int(v4), (OK, 0x80FF4000))
    eq('SetInt colour form on float4x1', (e.set_int(pid['ff'], 0x01020304), e.get_int(pid['ff'])),
       ((OK, 1), (OK, 0x01020304)))
    eq('GetInt colour form clamps', (e.set_vector(v4, [fl(2.0), fl(-1.0), fl(0.5), fl(1.0)]), e.get_int(v4)),
       ((OK, 1), (OK, 0xFFFF007F)))
    eq('SetInt colour form refused on float2', e.set_int(pid['v2'], 1), (INVALIDCALL, 0))
    # x 2.0 -> 255 in bits 16..23, y 0.5 -> 127 in 8..15, z -3.0 -> 0 in 0..7, w 1.0 -> 255 in 24..31
    eq('SetVector into INT 1x1 clamps', (e.set_vector(i, [fl(2.0), fl(0.5), fl(-3.0), fl(1.0)]), e.data(i)),
       ((OK, 1), [0xFFFF7F00]))
    eq('GetVector on INT 1x1', e.get_vector(i), (OK, [fl(1.0), fmul(127, INV255), 0, fl(1.0)]))
    vv = e.version(pid['v2'])
    eq('SetVector into float2', (e.set_vector(pid['v2'], [fl(7), fl(8), fl(9), fl(10)]), e.data(pid['v2'])),
       ((OK, 1), [fl(7), fl(8)]))
    eq('GetVector on float2', e.get_vector(pid['v2']), (OK, [fl(7), fl(8), 0, 0]))
    e.set_vector(pid['v2'], [fl(7), fl(8), fl(9), fl(10)])
    ck.ok('SetVector with the same values raises the version', e.version(pid['v2']) > vv + 1)
    eq('SetVector into INT vector converts', (e.set_vector(pid['iv'], [fl(1.9), fl(-2.5), fl(3), 0]),
                                              e.data(pid['iv'])), ((OK, 1), [1, 0xFFFFFFFE, 3]))
    eq('SetVector refused on an array', e.set_vector(pid['va'], [0] * 4), (INVALIDCALL, 0))
    eq('SetVector refused on a matrix', e.set_vector(pid['m44'], [0] * 4), (INVALIDCALL, 0))

    m = [fl(10 * r + c) for r in range(4) for c in range(4)]
    eq('SetMatrix 3x3', (e.set_matrix(pid['m33'], m), e.data(pid['m33'])),
       ((OK, 1), [fl(10 * r + c) for r in range(3) for c in range(3)]))
    eq('SetMatrixTranspose 3x3', (e.set_matrix(pid['m33'], m, True), e.data(pid['m33'])),
       ((OK, 1), [fl(10 * c + r) for r in range(3) for c in range(3)]))
    eq('SetMatrix 4x3', (e.set_matrix(pid['m43'], m), e.data(pid['m43'])),
       ((OK, 1), [fl(10 * r + c) for r in range(4) for c in range(3)]))
    eq('SetMatrixTranspose 4x3', (e.set_matrix(pid['m43'], m, True), e.data(pid['m43'])),
       ((OK, 1), [fl(10 * c + r) for r in range(4) for c in range(3)]))
    eq('GetMatrix 4x3 fills 0', e.get_matrix(pid['m43']),
       (OK, [fl(10 * c + r) if c < 3 else 0 for r in range(4) for c in range(4)]))
    eq('GetMatrixTranspose on a vector: row 0 (W get_matrix(FALSE))', e.get_matrix(pid['v2'], True)[1][:5],
       [fl(7), fl(8), 0, 0, 0])
    vm = e.version(pid['m44'])
    e.set_matrix(pid['m44'], m)
    e.set_matrix(pid['m44'], m)
    ck.ok('SetMatrix with the same values raises the version', e.version(pid['m44']) == e.clock.t > vm)
    eq('SetMatrix refused on MATRIX_COLUMNS', e.set_matrix(pid['mc'], m), (INVALIDCALL, 0))
    eq('GetFloatArray on MATRIX_COLUMNS', e.get_array(pid['mc'], FLOAT, 2), (OK, [0, 0]))
    eq('SetFloatArray refused on MATRIX_COLUMNS', e.set_array(pid['mc'], FLOAT, [0], 1), (INVALIDCALL, 0))
    eq('SetMatrixArray count > elements', e.set_matrix_array(pid['ma'], [m] * 3, 3), (INVALIDCALL, 0))
    eq('SetMatrixArray 2', (e.set_matrix_array(pid['ma'], [m, m], 2), e.data(pid['ma'])[16:18]),
       ((OK, 1), [fl(0), fl(1)]))
    eq('GetMatrixArray transposed', e.get_matrix_array(pid['ma'], 1, True)[1][0][:2], [fl(0), fl(10)])
    eq('SetVectorArray count > elements', e.set_vector_array(pid['va'], [[0] * 4] * 5, 5), (INVALIDCALL, 0))
    eq('SetVectorArray 2', (e.set_vector_array(pid['va'], [[fl(1)] * 4, [fl(2)] * 4], 2), e.data(pid['va'])[:9]),
       ((OK, 1), [fl(1)] * 4 + [fl(2)] * 4 + [0]))
    eq('GetVectorArray', e.get_vector_array(pid['va'], 2)[1][1], [fl(2)] * 4)
    eq('SetVectorArray refused on a non-vector', e.set_vector_array(pid['ma'], [], 0), (INVALIDCALL, 0))

    ia = pid['ia']
    va = e.version(ia)
    eq('SetIntArray 3 on INT[5]', (e.set_array(ia, INT, [7, 8, 9], 3), e.data(ia)), ((OK, 1), [7, 8, 9, 4, 5]))
    eq('SetIntArray 9 on INT[5]', (e.set_array(ia, INT, list(range(10, 19)), 9), e.data(ia)),
       ((OK, 1), [10, 11, 12, 13, 14]))
    vs_ = e.version(ia)
    eq('SetIntArray same values', e.set_array(ia, INT, list(range(10, 15)), 5), (OK, 0))
    ck.ok('SetIntArray raises the version always', e.version(ia) > vs_ > va)
    eq('SetFloatArray on INT[5]', (e.set_array(ia, FLOAT, [fl(2.7)], 1), e.data(ia)[0]), ((OK, 1), 2))
    eq('GetIntArray on an element', e.get_array(p.by_name(0, b'ia[2]'), INT, 9), (OK, [12]))
    eq('SetInt on an element', (e.set_int(p.by_name(0, b'ia[4]'), 99), e.data(ia)[4]), ((OK, 1), 99))
    eq('SetInt refused on the array', e.set_int(ia, 1), (INVALIDCALL, 0))
    eq('SetFloat refused on a vector', e.set_float(v4, 0), (INVALIDCALL, 0))
    eq('SetBool refused on a texture', e.set_bool(pid['tex'], 1), (INVALIDCALL, 0))

    tex = pid['tex']
    vt = e.version(tex)
    eq('SetTexture', e.set_object(tex, 0x1234), (OK, 0, 1))
    vt2 = e.version(tex)
    ck.ok('SetTexture with a change raises the version', vt2 > vt)
    eq('SetTexture same pointer', e.set_object(tex, 0x1234), (OK, 0x1234, 0))
    eq('SetTexture same pointer keeps the version', e.version(tex), vt2)
    eq('GetTexture', e.get_object(tex), (OK, 0x1234))
    eq('SetTexture refused on a texture array', e.set_object(pid['texa'], 1), (INVALIDCALL, None, 0))
    eq('SetTexture on a texture element', e.set_object(p.by_name(0, b'texa[1]'), 0x55), (OK, 0, 1))
    eq('SetTexture on a struct member', e.set_object(p.by_name(0, b'sa[1].t'), 0x66), (OK, 0, 1))
    eq('the struct array holds it', e.data(pid['sa']), [fl(1), 0, fl(2), 0x66])
    eq('SetTexture refused on a float', e.set_object(f, 1), (INVALIDCALL, None, 0))

    smp = pid['smp']
    eq('SetValue on a sampler', e.set_value(smp, [], 0), (FAIL, 0))
    eq('GetValue on a sampler', e.get_value(smp, 0), (FAIL, None))
    eq('SetValue too few bytes', e.set_value(v4, [0] * 4, 12), (INVALIDCALL, 0))
    eq('SetValue raw BOOL', (e.set_value(bb, [9], 4), e.data(bb)), ((OK, 1), [9]))
    eq('GetValue normalises BOOL', e.get_value(bb, 4), (OK, [1]))
    eq('SetValue on a struct', (e.set_value(pid['st'], [1, 2, 3, 4], 16), e.get_value(pid['st'], 16)),
       ((OK, 1), (OK, [1, 2, 3, 4])))
    eq('SetValue on a member', (e.set_value(p.by_name(0, b'st.n'), [77], 4), e.data(pid['st'])[3]), ((OK, 1), 77))
    clock = e.clock.t
    snapshot = {n.id: e.data(n.id) for n in p.nodes[1:] if n.kind in (K_PARAM, K_ELEMENT, K_MEMBER)}
    for what, call in (('id 0', lambda: e.set_int(0, 1)), ('id past N', lambda: e.set_int(len(p.nodes), 1)),
                       ('a technique', lambda: e.set_int(p.techs[0], 1)),
                       ('a pass', lambda: e.set_value(p.nodes[p.techs[0]].children[0], [0], 4)),
                       ('an annotation', lambda: e.set_int(p.nodes[p.techs[0]].annots[0], 1)),
                       ('a param annotation', lambda: e.set_value(p.nodes[pid['b']].annots[0], [0], 4))):
        eq('Set* on %s' % what, call()[0], INVALIDCALL)
    eq('nothing changed after refused calls', ({n.id: e.data(n.id) for n in p.nodes[1:]
                                                if n.kind in (K_PARAM, K_ELEMENT, K_MEMBER)}, e.clock.t),
       (snapshot, clock))
    eq('GetInt on id 0', e.get_int(0), (INVALIDCALL, None))
    level = p.annot_by_name(p.techs[0], b'level')
    eq('GetInt on a technique annotation', e.get_int(level), (OK, 3))
    eq('GetFloat on a technique annotation', e.get_float(p.annot_by_name(p.techs[0], b'gain')), (OK, fl(1.5)))
    eq('GetInt on a FLOAT annotation', e.get_int(p.annot_by_name(p.techs[0], b'gain')), (OK, 1))
    eq('GetInt on a missing annotation', p.annot_by_name(p.techs[0], b'nosuch'), 0)
    stamps = []
    for k in range(5):
        e.set_array(ia, INT, [k], 1)
        stamps.append(e.version(ia))
    ck.ok('versions only rise', all(x < y for x, y in zip(stamps, stamps[1:])))

    # lookups (fx-model.md 2.3)
    eq('by_name element', p.by_name(0, b'va[3]'), p.nodes[pid['va']].children[3])
    for bad in (b'va[03]', b'va[ 3]', b'va[]', b'va[4]', b'V4', b'v4 ', b'', b'st.', b'st.pos.x'):
        eq('by_name %r' % bad, p.by_name(0, bad), 0)
    eq('by_name member', p.by_name(0, b'st.pos'), p.nodes[pid['st']].children[0])
    eq('by_name with a struct parent', p.by_name(pid['st'], b'n'), p.nodes[pid['st']].children[1])
    eq('by_name with an array parent', p.by_name(pid['sa'], b'x'), 0)
    eq('by_name with an element parent', p.by_name(p.by_name(0, b'sa[1]'), b't'), p.by_name(0, b'sa[1].t'))
    eq('by_name with a technique parent', p.by_name(p.techs[0], b'v4'), 0)
    eq('by_semantic as stored', p.by_semantic(0, b'Tint'), pid['v4'])
    eq('by_semantic upper', p.by_semantic(0, b'TINT'), pid['v4'])
    eq('by_semantic lower', p.by_semantic(0, b'blend'), pid['ia'])
    eq('by_semantic NULL', p.by_semantic(0, None), pid['b'])
    eq('by_semantic struct member', p.by_semantic(pid['st'], b'n'), p.nodes[pid['st']].children[1])
    eq('by_semantic unknown', p.by_semantic(0, b'Tin'), 0)
    eq('by_semantic under an array of structs: element 0 (W), under a numeric array: none',
       (p.by_semantic(pid['sa'], None), p.by_semantic(pid['ia'], b'BLEND')), (p.by_name(0, b'sa[0]'), 0))
    eq('param(0, i)', [p.param(0, k) for k in range(len(p.params) + 1)], p.params + [0])
    eq('element(0, i)', [p.element(0, k) for k in range(len(p.params) + 1)], p.params + [0])
    eq('param on a struct', p.param(pid['st'], 1), p.nodes[pid['st']].children[1])
    eq('param on an array', p.param(pid['va'], 0), 0)
    eq('element on an array', [p.element(pid['va'], k) for k in range(5)], p.nodes[pid['va']].children + [0])
    eq('element on a scalar', p.element(pid['i'], 0), 0)
    eq('tech by index and name', (p.tech(0), p.tech_by_name(b't0'), p.tech(1), p.tech_by_name(b't9')),
       (p.techs[0], p.techs[0], 0, 0))
    eq('pass by index and name', (p.pass_by_name(p.techs[0], b'p1'), p.pass_at(p.techs[0], 2)),
       (p.nodes[p.techs[0]].children[1], 0))
    eq('annotation by index', p.annot_at(pid['b'], 0), p.nodes[pid['b']].annots[0])
    eq('annotation on an element', p.annot_at(p.by_name(0, b'va[0]'), 0), 0)

    # the model's numbers on SYN_MAIN: 21 parameters, 15 elements (ia 5, va 4, ma 2, sa 2, texa 2), 6 members
    # (st 2, sa 2 x 2), 4 annotations, 1 technique, 2 passes
    eq('nodes', len(p.nodes) - 1, 21 + 15 + 6 + 4 + 1 + 2)
    eq('bytes', [p.nodes[pid[k]].bytes for k in ('b', 'v3', 'm43', 'ia', 'st', 'sa', 'tex', 'texa', 'smp')],
       [4, 12, 48, 20, 16, 16, 4, 8, 0])
    eq('element flags follow the parameter', p.nodes[p.by_name(0, b'ia[0]')].flags, 1)
    eq('annotation flags', p.nodes[level].flags, ANNOTATION)
    prs = {(r, pr.kind): [c.why for c in pr.consts] for r, pr in p.progs.items()}
    eq('CTAB reasons', prs, {(1, 'preshader'): [0, 2], (2, 'shader'): [0, 0, 0], (3, 'shader'): [0, 0, 4, 4],
                             (4, 'preshader'): [0],
                             (5, 'shader'): [0, 1, 1, 1, 2, 2, 3, 4, 5, 7, 0, 0, 0, 0, 2, 6],
                             (7, 'preshader'): []})
    lines = [c.line() for c in p.progs[5].consts]
    eq('const matched in set c', lines[0], 'const 0 "v4" c0 count 1 -> #%d float vector per 1 elements 1/1' % pid['v4'])
    eq('const no name', lines[3], 'const 3 - c3 count 1 -> UNKNOWN(0x00000001)')
    eq('const columns cut', lines[10], 'const 10 "m44" c12 count 3 -> #%d float columns per 4 elements 1/1' %
       pid['m44'])
    eq('const sampler in a vertex shader', lines[11], 'const 11 "smp" s0 count 1 -> #%d sampler stage 257 '
       'elements 1/1' % pid['smp'])
    eq('const set i', lines[12], 'const 12 "i" i0 count 1 -> #%d int scalar per 1 elements 1/1' % pid['i'])
    eq('const set b', lines[13], 'const 13 "bb" b0 count 1 -> #%d bool scalar per 1 elements 1/1' % pid['bb'])
    eq('const unknown set', lines[14], 'const 14 "st" UNKNOWN(0x00000009)0 count 1 -> UNKNOWN(0x00000002)')
    eq('const past the table', lines[15], 'const 15 UNKNOWN(0x00000006)')
    pre = [c.line() for c in p.progs[1].consts]
    eq('preshader int[5] cut', pre[0], 'const 0 "ia" c0 count 3 -> #%d int>float scalar per 1 elements 3/5' % pid['ia'])
    eq('preshader set b', pre[1], 'const 1 "b" b0 count 1 -> UNKNOWN(0x00000002)')
    eq('leaves of ia', p.leaves(p.progs[1].consts[0]), [(p.by_name(0, b'ia[%d]' % k), k, 1 if k < 3 else 0)
                                                       for k in range(5)])
    eq('leaves of a cut matrix', p.leaves(p.progs[5].consts[10]), [(pid['m44'], 12, 3)])
    eq('vertex va cut', p.progs[2].consts[2].line(), 'const 2 "va" c8 count 3 -> #%d float vector per 1 '
       'elements 3/4' % pid['va'])
    eq('pixel sampler stage', p.progs[3].consts[0].line(), 'const 0 "smp" s2 count 1 -> #%d sampler stage 2 '
       'elements 1/1' % pid['smp'])
    eq('StructMembers on a numeric parameter (W init_set_constants_param)', p.progs[3].consts[2].line(),
       'const 2 "f" c1 count 1 -> UNKNOWN(0x00000004)')
    t0 = p.nodes[p.techs[0]]
    p0, p1 = p.nodes[t0.children[0]], p.nodes[t0.children[1]]
    eq('pass 0 deps', p0.deps, sorted([pid['ia'], pid['v4'], pid['m44'], pid['va'], pid['f'], pid['tex']]))
    eq('pass 1 deps', p1.deps, sorted([pid['v4'], pid['m44'], pid['tex'], pid['i'], pid['bb'], pid['f']]))
    eq('technique versions', (vn(t0.vs), vn(t0.ps)), ('vs_1_1', 'ps_2_0'))
    text = dump_all([e], Pool())
    for what, line in (
            ('a usage-1 source', 'state 0 TEXTURE[256] TEXTURE 0 param #%d\n' % pid['tex']),
            ('an unknown operation', 'state 2 UNKNOWN(0x000000c8)[0] - - literal = 0\n'),
            ('two resources on a state', 'state 3 ZFUNC[0] RENDER 23 param #%d UNKNOWN(0x00000002)\n' % pid['f']),
            ('usage 0 on a texture state', 'state 4 TEXTURE[0] TEXTURE 0 UNKNOWN(0x00000000)\n'),
            ('a preshader of no known version', 'state 5 ALPHAREF[0] RENDER 24 preshader res 7 outputs 1 ctab -\n'),
            ('a name that does not resolve', 'state 6 TEXTURE[1] TEXTURE 0 param UNKNOWN(0x00000001) name '
             '"missing"\n'),
            ('an array literal', 'state 7 ALPHABLENDENABLE[0] RENDER 27 literal UNKNOWN(0x00000002)\n'),
            ('a material state with a preshader', 'state 3 MATERIALDIFFUSE[0] MATERIAL 0 preshader res 4 outputs 4 '
             'ctab 1\n'),
            ('the string annotation', 'annotation #%d 0 name "label" STRING OBJECT flags 0x00000004 = '
             'string "hello"\n' % p.nodes[pid['b']].annots[0]),
            ('a member', 'member #%d 1 path "st.n" INT SCALAR 1x1 name "n" sem "N" flags 0x00000000 bytes 4 = 77\n'
             % p.by_name(0, b'st.n')),
            ('a struct element', 'element #%d 1 path "sa[1]" VOID STRUCT 2 flags 0x00000000 bytes 8\n'
             % p.by_name(0, b'sa[1]')),
            ('a texture member', 'member #%d 1 path "sa[1].t" TEXTURE OBJECT name "t" sem - flags 0x00000000 bytes 4 '
             '= obj %d data 0x00000066\n' % (p.by_name(0, b'sa[1].t'), p.nodes[p.by_name(0, b'sa[1].t')].object)),
            ('a float annotation', '= float 0x3fc00000:1.5\n')):
        ck.ok('the dump prints ' + what, line in text)

    # pool matching (fx-model.md 6.2): SYN_POOL_A, B, C
    a, bpr, c = (Proto(n, d) for n, d in zip(('SYN_POOL_A', 'SYN_POOL_B', 'SYN_POOL_C'), syn_pool()))
    pool = Pool()
    ea, eb, ec = Inst(a, pool), Inst(bpr, pool), Inst(c, pool)
    eq('slots made', [(s.no, s.proto.name, s.proto.nodes[s.nid].name, len(s.holders)) for s in pool.live()],
       [(0, 'SYN_POOL_A', b'x', 1), (1, 'SYN_POOL_A', b'y', 2), (2, 'SYN_POOL_A', b'z', 1),
        (3, 'SYN_POOL_A', b'st', 1), (4, 'SYN_POOL_A', b't', 2), (5, 'SYN_POOL_B', b'x', 1),
        (6, 'SYN_POOL_B', b'z', 1), (7, 'SYN_POOL_B', b'st', 1), (8, 'SYN_POOL_C', b'st', 1)])
    eq('a shared sampler gets no slot', (a.params[4] in ea.slot, bpr.params[4] in eb.slot), (False, False))
    eq('a joiner takes the slot value', eb.data(bpr.params[1]), [fl(1), fl(2), fl(3), fl(4)])
    eb.set_vector(bpr.params[1], [fl(9)] * 4)
    eq('a write through B reaches A', ea.data(a.params[1]), [fl(9)] * 4)
    eq('versions agree across holders', ea.version(a.params[1]), eb.version(bpr.params[1]))
    eq('the apart parameter keeps its value', ec.data(c.params[0]), [0, 0, 0, 0])
    lone = Inst(bpr)
    lone.set_vector(bpr.params[1], [fl(5)] * 4)
    eq('an effect without a pool shares nothing', (lone.slot, ea.data(a.params[1])), ({}, [fl(9)] * 4))
    text = dump_all([ea, eb, ec], pool)
    ck.ok('the pool section lists the apart parameter', '  slot 1 name "y" FLOAT VECTOR 1x4 bytes 16 holders 2 = '
          '0x41100000:9 0x41100000:9 0x41100000:9 0x41100000:9\n    holder 0 "SYN_POOL_A" #2\n    holder 1 '
          '"SYN_POOL_B" #2\n    apart 2 "SYN_POOL_C" #1 flags 0x00000000\n' in text)
    ck.ok('a typedef mismatch is apart of the other slot', '  slot 0 name "x" INT SCALAR 1x1 bytes 4 holders 1 = 1\n'
          '    holder 0 "SYN_POOL_A" #1\n    apart 1 "SYN_POOL_B" #1 flags 0x00000001\n' in text)
    ea.set_object(a.params[5], 0x77)
    drops = []
    ea.free(drops.append)
    eq('freeing a holder drops nothing shared', drops, [])
    eb.free(drops.append)
    eq('the last holder drops the slot pointer', drops, [0x77])
    eq('slots after both frees', [s.no for s in pool.live()], [8])
    ea2 = Inst(a, pool)
    eq('freed slot numbers are reused, lowest first', [ea2.slot[t] for t in a.params if t in ea2.slot], [0, 1, 2, 3, 4])
    eq('a re-created slot starts from the default', ea2.data(a.params[1]), [fl(1), fl(2), fl(3), fl(4)])
    va = ea2.version(a.params[1])
    eb2 = Inst(bpr, pool)                           # y (parameter 1 of 6) joins slot 1
    ck.ok('joining a live slot raises its version to ++counter, in parameter order (W: set_dirty)',
          ea2.version(a.params[1]) == eb2.version(bpr.params[1]) == eb2.clock.t - 4 > va)

    # empty names (fx-model.md 2.3): "" names no parameter and no annotation (W), so neither a CTAB constant nor a
    # usage-1 reference named "" matches
    u = Proto('SYN_UNNAMED', syn_unnamed())
    up, ust = u.params[0], u.params[1]
    um = u.nodes[ust].children[0]
    eq('unnamed: the paths "" and "st."', (u.nodes[up].path, u.nodes[um].path), (b'', b'st.'))
    eq('by_name "": none, also under a parent', (u.by_name(0, b''), u.by_name(ust, b'')), (0, 0))
    ua = u.nodes[up].annots[0]
    eq('by_name NULL: the parent itself (W), none without one or under a technique',
       (u.by_name(ust, None), u.by_name(um, None), u.by_name(ua, None), u.by_name(0, None), u.by_name(u.techs[0], None)),
       (ust, um, ua, 0, 0))
    ut1 = u.techs[1]
    eq('technique and pass by NULL: none; by "": the unnamed ones',
       (u.tech_by_name(None), u.tech_by_name(b''), u.pass_by_name(ut1, None), u.pass_by_name(ut1, b'')),
       (0, ut1, 0, u.nodes[ut1].children[0]))
    eq('the unnamed member by its path, y under st', (u.by_name(0, b'st.'), u.by_name(ust, b'y')), (um, um + 1))
    eq('annot_by_name "": none', (u.annot_by_name(up, b''), u.annot_by_name(up, b'k')), (0, u.nodes[up].annots[1]))
    eq('a CTAB constant named ""', [c.line() for c in u.progs[0].consts],
       ['const 0 "" c0 count 1 -> UNKNOWN(0x00000001)'])
    up0 = u.nodes[u.nodes[u.techs[0]].children[0]]
    eq('a usage-1 name "": none, no deps', (u.source(u.res_on[up0.skey + (1,)]), up0.deps),
       ('param UNKNOWN(0x00000001) name ""', []))

    # SAMPLER[i] = <sampler> (fx-model.md 3.5, row 178): the sampler's block feeds the deps, element 0's of an array
    # (the model's choice); another row (TEXTURE[2] = <smpb>) applies no sampler states, so smpb's block does not
    sr = Proto('SYN_SAMPLER_REF', syn_sampler_ref())
    eq('SAMPLER[i] = <sampler>: deps ta (smp) and tb (smpa[0]), not tc, not td (TEXTURE[2] = <smpb>)',
       sr.nodes[sr.nodes[sr.techs[0]].children[0]].deps, [sr.by_name(0, b'ta'), sr.by_name(0, b'tb')])

    # where W goes by the type or the row (fx-model.md 2.4, 2.5, 3.1, 3.4)
    w = Proto('SYN_W', syn_w())
    we = Inst(w)
    oi, ov, vsp = w.params[:3]
    eq('class OBJECT, type INT or VOID: no storage (W reads no object id for it)',
       (w.nodes[oi].bytes, w.nodes[ov].bytes, we.data(oi), we.get_value(ov, 0)), (0, 0, [], (OK, [])))
    v0 = we.version(vsp)
    eq('SetValue on a VERTEXSHADER parameter: nothing written, the version rises',
       (we.set_value(vsp, [0xDEAD0000], 4), we.data(vsp), we.version(vsp) > v0), ((OK, 0), [0], True))
    sw, nc = w.nodes[w.techs[0]], w.nodes[w.techs[1]]
    eq('shaders on the other shader row: sampler stages by the row (257 on VERTEXSHADER), not applied',
       ([c.stage for c in w.progs[1].consts[:1] + w.progs[2].consts], w.progs[1].applied, w.progs[2].applied),
       ([257, 1], False, False))
    eq('... so no deps and no versions', (w.nodes[sw.children[0]].deps, sw.vs, sw.ps), ([], 0, 0))
    eq('a CTAB header whose Size is not 28: no table, the shader still counts', (w.progs[3].ctab, nc.vs),
       (None, 0xFFFE0101))

    # a set-s count cut to the constant's own elements (3.4, W's RegisterCount), and the last of two shaders on a
    # state decides (3.5, W: each resource overwrites the state's shader and evaluator)
    s = Syn()
    tx = [s.param(T(5, OBJECT, n), [s.obj(b'')]) for n in (b'ta', b'tb', b'tc', b'td', b'te')]
    block = lambda: s.sampler([s.state(164, 256, T(5, OBJECT), [s.obj()])])
    smp = s.param(T(10, OBJECT, b'smp'), block())
    smpa = s.param(T(10, OBJECT, b'smpa', count=4), block() + block() + block() + block())
    s.param(T(FLOAT, VECTOR, b'a', cols=4), [0] * 4)
    b4 = s.param(T(FLOAT, VECTOR, b'b', cols=4), [0] * 4)
    for (pi, el), n in zip(((smp, 0), (smpa, 0), (smpa, 1), (smpa, 2), (smpa, 3)), (b'ta', b'tb', b'tc', b'td', b'te')):
        s.resource(NONE, pi, el, 0, 1, n + b'\0')
    s.tech(b't', [], [(b'p', [], [s.state(146, 0, T(VERTEXSHADER, OBJECT), [s.obj()]),
                                  s.state(147, 0, T(PIXELSHADER, OBJECT), [s.obj()])])])
    s.resource(0, 0, NONE, 0, 0, shader_blob(0xFFFE0300, [(b'a', 2, 0, 1, (VECTOR, FLOAT, 1, 4, 1, 0))]))
    s.resource(0, 0, NONE, 0, 0, shader_blob(0xFFFE0101, [(b'b', 2, 0, 1, (VECTOR, FLOAT, 1, 4, 1, 0))]))
    s.resource(0, 0, NONE, 1, 0, shader_blob(0xFFFF0200, [(b'smp', 3, 0, 2, (OBJECT, 12, 1, 1, 1, 0)),
                                                          (b'smpa', 3, 4, 4, (OBJECT, 12, 1, 1, 2, 0))]))
    c = Proto('SYN_CUT', s.build())
    tn = c.nodes[c.techs[0]]
    eq('set s: smp count 2 under Elements 1 binds one stage, smpa count 4 under Elements 2 binds two',
       [(k.why, k.stage, k.used) for k in c.progs[7].consts], [(0, 0, 1), (0, 4, 2)])
    eq('two shaders on a state: the last (vs_1_1) decides, so deps b, ta, tb, tc and not a, td, te',
       (vn(tn.vs), c.nodes[tn.children[0]].deps), ('vs_1_1', sorted(c.params[i] for i in (b4,) + tuple(tx[:3]))))

    # lookups over the effects (fx-model.md 6.1)
    nfx = 0
    if indir and os.path.isdir(indir) and any(x.endswith('.fxo') for x in os.listdir(indir)):
        for p in load(indir):
            nfx += 1
            e = Inst(p)
            first = {}                  # path -> the first parameter, element or member with it (names may repeat)
            for n in p.nodes[1:]:
                if n.kind in (K_PARAM, K_ELEMENT, K_MEMBER):
                    first.setdefault(n.path, n.id)

            def none_or_exact(s):
                """A query that names no node: 0, or a node whose path is exactly s."""
                g = p.by_name(0, s)
                return g == 0 or p.nodes[g].path == s

            for n in p.nodes[1:]:
                if n.kind in (K_PARAM, K_ELEMENT, K_MEMBER):
                    ck.eq('%s: by_name %r' % (p.name, n.path), p.by_name(0, n.path), first[n.path] if n.path else 0)
                    if n.kind != K_PARAM:
                        par = p.nodes[n.parent]
                        if n.kind == K_MEMBER:
                            ck.eq('%s: member by parent' % p.name, p.by_name(par.id, n.name or b''),
                                  first[n.path] if n.name else 0)
                        else:
                            ck.eq('%s: element' % p.name, p.element(par.id, n.index), n.id)
                    if n.kind == K_PARAM and n.elements:
                        ck.ok('%s: %r[n]' % (p.name, n.path), none_or_exact(n.path + b'[%d]' % n.elements))
                        ck.ok('%s: %r[00]' % (p.name, n.path), none_or_exact(n.path + b'[00]'))
                        ck.ok('%s: %r[]' % (p.name, n.path), none_or_exact(n.path + b'[]'))
                        ck.eq('%s: param on an array' % p.name, p.param(n.id, 0), 0)
                        ck.eq('%s: element n' % p.name, p.element(n.id, n.elements), 0)
                    if n.kind == K_PARAM and n.path != lower(n.path):
                        ck.ok('%s: case-changed name' % p.name, none_or_exact(lower(n.path)))
                    if n.kind == K_PARAM and n.sem is not None:
                        want = next(t for t in p.params if p.nodes[t].sem is not None and
                                    lower(p.nodes[t].sem) == lower(n.sem))      # the first, without case (stricmp)
                        ck.eq('%s: semantic %r' % (p.name, n.sem), p.by_semantic(0, n.sem), want)
                        ck.eq('%s: semantic upper' % p.name, p.by_semantic(0, n.sem.upper()), want)
                        ck.eq('%s: semantic lower' % p.name, p.by_semantic(0, lower(n.sem)), want)
                elif n.kind == K_ANNOT:
                    o = p.nodes[n.parent]
                    want = next(a for a in o.annots if p.nodes[a].name == n.name) if n.name else 0
                    ck.eq('%s: annotation by name' % p.name, p.annot_by_name(o.id, n.name), want)
                    if n.cls in NUMERIC and n.type == INT and n.elements == 0 and n.rows * n.cols == 1:
                        ck.eq('%s: GetInt on annotation' % p.name, e.get_int(n.id), (OK, n.store[0]))
                elif n.kind == K_TECH:
                    ck.eq('%s: technique by index' % p.name, p.tech(n.index), n.id)
                    want = next(t for t in p.techs if (p.nodes[t].name or b'') == (n.name or b''))
                    ck.eq('%s: technique by name' % p.name, p.tech_by_name(n.name or b''), want)  # offset 0: by ""
                elif n.kind == K_PASS:
                    ck.eq('%s: pass by index' % p.name, p.pass_at(n.parent, n.index), n.id)
                    want = next(q for q in p.nodes[n.parent].children if (p.nodes[q].name or b'') == (n.name or b''))
                    ck.eq('%s: pass by name' % p.name, p.pass_by_name(n.parent, n.name or b''), want)
                    ck.eq('%s: pass by NULL' % p.name, p.pass_by_name(n.parent, None), 0)
            ck.eq('%s: technique by NULL' % p.name, p.tech_by_name(None), 0)
            ck.eq('%s: param(0, P)' % p.name, p.param(0, len(p.params)), 0)
            ck.eq('%s: params by index' % p.name, [p.param(0, k) for k in range(len(p.params))], p.params)
            nosem = next((t for t in p.params if p.nodes[t].sem is None), 0)
            ck.eq('%s: semantic NULL' % p.name, p.by_semantic(0, None), nosem)
    for x in ck.bad[:60]:
        print('  FAIL ' + x)
    print('fxmodel --self-test: %d checks, %d failed%s' % (ck.n, len(ck.bad), ' (lookups over %d effects)' % nfx
                                                          if nfx else ''))
    return 1 if ck.bad else 0


# --pool-test (fx-model.md 6.2)

def pool_value(n, slot, writer, second):
    """A value of the slot's type that cannot occur by chance: dwords for GetValue, and the call MW would use."""
    k = n.bytes // 4
    if n.cls == OBJECT:
        return [0x13570000 + 16 * slot + writer + (8 if second else 0)]
    if n.type == FLOAT:
        return [fl(slot + writer / 64 + 0.125 + j + (0.5 if second else 0)) for j in range(k)]
    return [(0x5A000000 + 256 * slot + writer + j + (0x100000 if second else 0)) & 0xFFFFFFFF for j in range(k)]


def mw_set(e, tid, v):
    """The setter MW uses for the shape (fx-model.md 1.2): SetIntArray, SetInt, SetFloat, SetVector, SetTexture."""
    n = e.p.nodes[tid]
    if n.cls == OBJECT and n.type in TEXTURES:
        return e.set_object(tid, v[0])[0], 'SetTexture'
    if n.type == INT and (n.elements or n.cls != SCALAR):
        return e.set_array(tid, INT, v, len(v))[0], 'SetIntArray'
    if n.type == INT:
        return e.set_int(tid, v[0])[0], 'SetInt'
    if n.type == FLOAT and n.cls == SCALAR and not n.elements:
        return e.set_float(tid, v[0])[0], 'SetFloat'
    if n.type == FLOAT and n.cls == VECTOR and not n.elements:
        return e.set_vector(tid, v + [0] * (4 - len(v)))[0], 'SetVector'
    return e.set_value(tid, v, 4 * len(v))[0], 'SetValue'


def pool_test(indir):
    ck = Checker()
    protos = load(indir)
    insts, pool = build(protos)
    stamps, calls = [0], {}

    def stamp(e, tid):
        ck.ok('versions never go back or repeat', e.version(tid) > stamps[-1])
        stamps.append(e.version(tid))

    aparts = {}
    for s in pool.live():
        name = s.proto.nodes[s.nid].name
        held = {(id(e), t) for e, t in s.holders}
        aparts[s.no] = [(e, t) for e in insts for t in e.p.params
                        if e.p.nodes[t].name == name and (id(e), t) not in held]
    for s in pool.live():
        before_all = [(e, t, e.data(t), e.version(t)) for e, t in aparts[s.no]]   # through this slot's writes (a
        for w, (we, wid) in enumerate(list(s.holders)):                          # holder of a twin slot has its own)
            n = we.p.nodes[wid]
            for second in (False, True):
                v = pool_value(n, s.no, w, second)
                before = [e.version(t) for e, t in s.holders]
                if second:
                    hr, call = we.set_value(wid, v, 4 * len(v))[0], 'SetValue'
                else:
                    hr, call = mw_set(we, wid, v)
                calls[call] = calls.get(call, 0) + 1
                what = 'slot %d %r writer %d %s' % (s.no, n.name, w, call)
                ck.eq(what + ' result', hr, OK)
                stamp(we, wid)
                for (e, t), vb in zip(s.holders, before):
                    ck.eq(what + ' read back in %s' % e.p.name, e.get_value(t, n.bytes), (OK, v))
                    ck.eq(what + ' version in %s' % e.p.name, e.version(t), we.version(wid))
                    ck.ok(what + ' version rose in %s' % e.p.name, e.version(t) > vb)
        for e, t, d, ver in before_all:
            ck.eq('apart #%d of %s keeps its value' % (t, e.p.name), e.data(t), d)
            ck.eq('apart #%d of %s keeps its version' % (t, e.p.name), e.version(t), ver)
            if t not in e.slot:
                ck.eq('apart #%d of %s keeps its default' % (t, e.p.name), e.data(t), e.p.nodes[t].store)
    mw = len(protos) == 31                  # MW's names only on the 31 effects (main_check's rule)
    named = {}
    for name, (h, a) in (POOL_NAMES.items() if mw else ()):
        ss = [s for s in pool.live() if s.proto.nodes[s.nid].name == name]
        ck.eq('%s: one slot' % name.decode(), len(ss), 1)
        if not ss:
            continue
        s = ss[0]
        ck.eq('%s: holders' % name.decode(), len(s.holders), h)
        ck.eq('%s: apart' % name.decode(), len(aparts[s.no]), a)
        e0, t0 = s.holders[len(s.holders) // 2]
        before_all = [(e, t, e.data(t), e.version(t)) for e, t in aparts[s.no]]
        if name == b'ColorWriteMode':
            ck.eq('SetInt(0xF) on ColorWriteMode', e0.set_int(t0, 0xF), (OK, 1))
            want = [0xF]
            for e, t in s.holders:
                ck.eq('ColorWriteMode GetInt in %s' % e.p.name, e.get_int(t), (OK, 0xF))
        else:
            ck.eq('SetIntArray({0,0,0,5,6}) on BlendState', e0.set_array(t0, INT, [0, 0, 0, 5, 6], 5)[0], OK)
            want = [0, 0, 0, 5, 6]
            for e, t in s.holders:
                ck.eq('BlendState GetIntArray in %s' % e.p.name, e.get_array(t, INT, 5), (OK, want))
        for e, t in s.holders:
            ck.eq('%s GetValue in %s' % (name.decode(), e.p.name), e.get_value(t, 4 * len(want)), (OK, want))
        for e, t, d, ver in before_all:
            ck.eq('%s apart in %s untouched' % (name.decode(), e.p.name), (e.data(t), e.version(t)), (d, ver))
        named[name] = (len(s.holders), len(aparts[s.no]))

    # lifetime: free the first holder; the rest keep the value; re-created, it reads the slot's value (BlendState
    # on MW, on any other set the first slot with the most holders, as fxmodel_test.c picks it)
    two = [s for s in pool.live() if len(s.holders) > 1 and (not mw or s.proto.nodes[s.nid].name == b'BlendState')]
    s = max(two, key=lambda x: len(x.holders)) if two else None
    if mw:
        ck.ok('lifetime: BlendState has two holders', s is not None)
    if s is not None:
        name = s.proto.nodes[s.nid].name
        fe, ft = s.holders[0]
        cur = s.block.data[:]
        ck.ok('the current %r differs from its default' % name, cur != fe.p.nodes[ft].store)
        kept = {t: no for t, no in fe.slot.items() if len(pool.slots[no].holders) > 1}   # slots that outlive fe
        ck.ok('the first %r holder shares slots' % name, ft in kept)
        fe.free()
        insts[insts.index(fe)] = None
        for e, t in s.holders:
            ck.eq('after the free, %s still reads it' % e.p.name, e.data(t), cur)
        ver = s.block.version
        ne = Inst(fe.p, pool)
        insts[[i for i, x in enumerate(insts) if x is None][0]] = ne
        ck.eq('re-created %s reads the slot value, not its default' % ne.p.name, ne.data(ft), cur)
        ck.ok('re-created %s raises the slot version (W: set_dirty on joining)' % ne.p.name, ne.version(ft) > ver)
        ck.eq('re-created %s joined the slots that outlived it' % ne.p.name, {t: ne.slot.get(t) for t in kept}, kept)
        if mw:
            want = [1, 2, 3, 4, 5]
            ne.set_array(ft, INT, want, 5)
        else:
            want = pool_value(ne.p.nodes[ft], s.no, 98, False)
            ne.set_value(ft, want, 4 * len(want))
        for e, t in s.holders:
            ck.eq('a write through the re-created effect reaches %s' % e.p.name, e.data(t), want)
    # free every holder of a slot: the next effect with it starts from its default (each slot on a fresh pool)
    recreated = 0
    for k in range(len(build(protos)[1].live())):
        fi, fp = build(protos)
        s = fp.live()[k]
        name = s.proto.nodes[s.nid].name
        e0, t0 = s.holders[0]
        hr = e0.set_value(t0, pool_value(e0.p.nodes[t0], s.no, 99, True), e0.p.nodes[t0].bytes)[0]
        ck.ok('slot %r: the value written differs from the default' % name,
              hr == OK and s.block.data != e0.p.nodes[t0].store)
        victims = list(dict.fromkeys(e for e, _ in s.holders))
        others = [(x, x.block.data[:]) for x in fp.live() if x is not s and
                  any(e not in victims for e, _ in x.holders)]
        for e in victims:
            e.free()
        ck.ok('slot %r is gone with its last holder' % name, s.no not in fp.slots)
        for x, d in others:
            ck.eq('slot %r: slot %d with other holders keeps its value' % (name, x.no), x.block.data, d)
        ne = Inst(victims[0].p, fp)
        t = next(t for t in ne.p.params if ne.p.nodes[t].name == name and t in ne.slot)
        ck.eq('a new holder of %r starts from its default' % name, ne.data(t), ne.p.nodes[t].store)
        recreated += 1
    # drop: every object pointer that dies with an effect, exactly once
    insts2, pool2 = build(protos)
    ptr, live_ptrs = 0x20000000, {}
    for e in insts2:
        for tid in e.p.params:
            n = e.p.nodes[tid]
            for leaf in ([e.p.nodes[c] for c in n.children] if n.elements else [n]):
                if leaf.cls == OBJECT and leaf.type in TEXTURES:
                    ptr += 4
                    e.set_object(leaf.id, ptr)
    for e in insts2:
        expected = []
        for tid in e.p.params:
            blk = e.blocks[tid]
            last = tid not in e.slot or len(pool2.slots[e.slot[tid]].holders) == 1
            if last:
                expected += [blk.data[j] for j in e.p.nodes[tid].objpos if blk.data[j]]
        got = []
        e.free(got.append)
        ck.eq('drop of %s' % e.p.name, sorted(got), sorted(expected))
        for x in got:
            live_ptrs[x] = live_ptrs.get(x, 0) + 1
    ck.eq('every pointer dropped exactly once', sorted(set(live_ptrs.values())), [1] if ptr > 0x20000000 else [])
    ck.eq('no slot left', pool2.slots, {})
    nslots = len(build(protos)[1].live())
    for x in ck.bad[:60]:
        print('  FAIL ' + x)
    print('pool: %d slots, %d holders written through each holder (%s), %d apart parameters checked, %s; '
          'lifetime %d re-created, drop over %d effects' % (
              nslots, sum(len(s.holders) for s in build(protos)[1].live()),
              ', '.join('%s %d' % kv for kv in sorted(calls.items())), sum(len(v) for v in aparts.values()),
              ', '.join('%s %d holders %d apart' % (k.decode(), h, a) for k, (h, a) in sorted(named.items())),
              recreated, len(insts2)))
    print('fxmodel --pool-test: %d checks, %d failed' % (ck.n, len(ck.bad)))
    return 1 if ck.bad else 0


# --ops: the Set/Get/lookup trace (tools/mw/fxmodeldump.c --ops makes the same calls and prints the same text).

OPS_PAT = [0x3FF33333, 0xBFF33333, 0x7FC00000, 0x4F32D05E, 0x80000000, 5, 0xFFFFFFFF, 0x3F800000, 0, 0x40490FDB,
           0x437F0000, 0x3F000000]


def ops_pat(k):
    return OPS_PAT[k % 12] ^ (k // 12)


def ops_node(out, e, nid):
    p = e.p
    n = p.node(nid)
    W = n.bytes // 4 if n is not None and n.kind != K_TECH and n.kind != K_PASS else 0
    E = min(n.elements, 64) if n is not None and n.kind not in (K_TECH, K_PASS) else 0
    A = min(W + 1, 64)
    room = max(W, 16 * (E + 1)) + 64
    src = [ops_pat(k) for k in range(room)]
    v1 = [0x7FC00000, 0x40000000, 0xBF000000, 0x3F400000]
    v2 = [0x3E800000, 0x3FC00000, 0xBF800000, 0x3F000000]
    v3 = [0x7F800001, 0xFF800001, 0x7FBFFFFF, 0x3F000000]      # signalling NaNs
    put = out.append

    def sset(txt, r):
        hr, ch = r[0], r[1]
        put('  %s -> 0x%08x ch %d v %d\n' % (txt, hr, 0 if hr else ch, e.version(nid)))

    def sget(txt, r, k=None):
        hr, w = r
        if hr:
            put('  %s -> 0x%08x\n' % (txt, hr))
            return
        if not isinstance(w, list):
            w = [w]
        elif w and isinstance(w[0], list):
            w = [x for v in w for x in v]
        if k is not None:
            w = w[:k]
        put('  %s -> 0x%08x =%s\n' % (txt, hr, ''.join(' %08x' % (x & 0xFFFFFFFF) for x in w)))

    put(' #%d\n' % nid)
    sget('getvalue', e.get_value(nid, W * 4))
    if W:
        sget('getvalue short', e.get_value(nid, W * 4 - 4), 0)
    sset('setbool 5', e.set_bool(nid, 5))
    sget('getbool', e.get_bool(nid))
    sset('setint', e.set_int(nid, 0x80FF4020))
    sset('setint again', e.set_int(nid, 0x80FF4020))
    sget('getint', e.get_int(nid))
    sset('setfloat', e.set_float(nid, 0xC0490FDB))
    sset('setfloat again', e.set_float(nid, 0xC0490FDB))
    sget('getfloat', e.get_float(nid))
    sget('getint', e.get_int(nid))
    for t in (BOOL, INT, FLOAT):
        sset('setarray %d %d' % (t, A), e.set_array(nid, t, src[t:], A))
        sget('getarray %d %d' % (t, A), e.get_array(nid, t, A), min(A, W))
        sset('setarray %d 3' % t, e.set_array(nid, t, src[7:], 3))
    sset('setvector 1', e.set_vector(nid, v1))
    sget('getvector', e.get_vector(nid))
    sset('setvector 2', e.set_vector(nid, v2))
    sget('getint', e.get_int(nid))
    sset('setvector 3', e.set_vector(nid, v3))
    sget('getint', e.get_int(nid))
    q4 = lambda base, cnt: [src[base + 4 * i:base + 4 * i + 4] for i in range(cnt)]
    q16 = lambda base, cnt: [src[base + 16 * i:base + 16 * i + 16] for i in range(cnt)]
    sset('setvectorarray %d' % E, e.set_vector_array(nid, q4(3, E), E))
    sset('setvectorarray 0', e.set_vector_array(nid, [], 0))
    sset('setvectorarray %d' % (E + 1), e.set_vector_array(nid, q4(0, E + 1), E + 1))
    sget('getvectorarray %d' % E, e.get_vector_array(nid, E))
    sget('getvectorarray 0', e.get_vector_array(nid, 0))
    sset('setmatrix', e.set_matrix(nid, src[5:21], False))
    sget('getmatrix', e.get_matrix(nid, False))
    sset('setmatrix transposed', e.set_matrix(nid, src[1:17], True))
    sget('getmatrix transposed', e.get_matrix(nid, True))
    sset('setmatrixarray %d' % E, e.set_matrix_array(nid, q16(2, E), E, False))
    sset('setmatrixarray 0', e.set_matrix_array(nid, [], 0, True))
    sget('getmatrixarray %d transposed' % E, e.get_matrix_array(nid, E, True))
    sget('getmatrixarray 0', e.get_matrix_array(nid, 0, False))
    hr, old, ch = e.set_object(nid, 0x13570000 + nid)
    put('  setobject -> 0x%08x ch %d v %d%s\n' % (hr, 0 if hr else ch, e.version(nid), '' if hr else ' old %08x' % old))
    hr, old, ch = e.set_object(nid, 0x13570000 + nid)
    sset('setobject again', (hr, ch))
    sget('getobject', e.get_object(nid))
    sset('setvalue', e.set_value(nid, src[4:], W * 4))
    if W:
        sset('setvalue short', e.set_value(nid, src, W * 4 - 4))
    sget('getvalue', e.get_value(nid, W * 4))
    oid = lambda what, r: put('  %s -> #%d\n' % (what, r))
    oid('param 0', p.param(nid, 0))
    oid('param 1', p.param(nid, 1))
    oid('element 0', p.element(nid, 0))
    oid('element last+1', p.element(nid, n.elements if n is not None else 0))
    oid('annot 0', p.annot_at(nid, 0))
    oid('pass 0', p.pass_at(nid, 0))
    if n is not None:
        name = n.name or b''
        oid('by_name path', p.by_name(0, n.path))
        oid('by_name under parent', p.by_name(n.parent, name) if n.parent else 0)
        oid('by_name NULL under it', p.by_name(nid, None))
        oid('by_semantic', p.by_semantic(0, n.sem) if n.sem is not None else 0)
        oid('by_semantic lower', p.by_semantic(0, lower(n.sem)) if n.sem is not None and len(n.sem) <= 256 else 0)
        oid('by_semantic NULL under it', p.by_semantic(nid, None))
        oid('annot_by_name shader', p.annot_by_name(nid, b'shader'))
        oid('tech_by_name', p.tech_by_name(name))
        oid('pass_by_name', p.pass_by_name(nid, name))
        oid('pass_by_name NULL', p.pass_by_name(nid, None))


def ops_all(protos):
    out = ['fxmodel ops 1\n', 'effects %d\n' % len(protos)]
    insts, pool = build(protos)
    for i, e in enumerate(insts):
        out.append('effect %d %s clock %d\n' % (i, F.qstr(e.p.name.encode()), e.clock.t))
        for nid in range(len(e.p.nodes) + 1):
            ops_node(out, e, nid)

    def free(i):
        drops = []
        insts[i].free(drops.append)
        drops.sort()
        out.append('free %d drops %d%s\n' % (i, len(drops), ''.join(' %08x' % d for d in drops)))
    if insts:
        free(0)
        insts[0] = Inst(protos[0], pool)
        e = insts[0]
        out.append('again 0 clock %d\n' % e.clock.t)
        for tid in e.p.params:
            n = e.p.nodes[tid]
            out.append(' #%d shared %d v %d getvalue' % (tid, e.slot[tid] + 1 if tid in e.slot else 0, e.version(tid)))
            hr, w = e.get_value(tid, n.bytes)
            out.append(' -> 0x%08x =%s\n' % (hr, ''.join(' %08x' % x for x in w)) if not hr else ' -> 0x%08x\n' % hr)
        for i in range(len(insts)):
            free(i)
    out.append('end\n')
    return ''.join(out)


def main():
    a = sys.argv[1:]
    if a and a[0] == '--ops' and len(a) <= 3:
        protos = load(a[1] if len(a) > 1 else IN)
        outdir = a[2] if len(a) > 2 else OUT
        os.makedirs(outdir, exist_ok=True)
        with open(os.path.join(outdir, 'ops.txt'), 'w', newline='\n') as fh:
            fh.write(ops_all(protos))
        return
    if a and a[0] == '--check' and len(a) <= 3:
        sys.exit(main_check(a[1] if len(a) > 1 else IN, a[2] if len(a) > 2 else BDIR))
    if a and a[0] == '--self-test' and len(a) <= 2:
        sys.exit(self_test(a[1] if len(a) > 1 else IN))
    if a and a[0] == '--pool-test' and len(a) <= 2:
        sys.exit(pool_test(a[1] if len(a) > 1 else IN))
    if a and a[0] == '--synth' and len(a) == 2:
        os.makedirs(a[1], exist_ok=True)
        for name, data in synth_files().items():
            with open(os.path.join(a[1], name + '.fxo'), 'wb') as out:
                out.write(data)
        print('fxmodel: %d made-up effects into %s' % (len(synth_files()), a[1]))
        return
    if len(a) > 2 or any(x.startswith('-') for x in a):
        sys.exit(__doc__)
    main_dump(a[0] if a else IN, a[1] if len(a) > 1 else OUT)


if __name__ == '__main__':
    main()
