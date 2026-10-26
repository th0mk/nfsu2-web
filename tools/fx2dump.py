#!/usr/bin/env python3
"""Dump compiled fx_2_0 effects as text, in the canonical form 'fx2dump 1'.

  tools/fx2dump.py [<indir> [<outdir>]]   dump every <indir>/*.fxo to <outdir>/<NAME>.txt
                                          (default out/mw/fx/res to out/mw/fx/b), print a count table
  tools/fx2dump.py --one <file.fxo>       print one dump to stdout

docs/mw/fx2-format.md is the format and section 5 of it the dump: header,
parameters, techniques, passes, states and annotations with their values,
objects, then every resource (D3D9 shaders with their constant tables, FXLC
preshaders with literals and instructions, parameter names).  Anything the
spec has no name for prints as UNKNOWN(0x........), so a dump can be checked
by counting them.  This is implementation B of M5-01, written from the spec
and the bytes alone; another implementation writes the same text, and the
two are compared.

The count table lists per effect what the dump holds: parameters,
techniques, passes, annotations, states, resources, shaders per version,
preshaders, preshader instructions and UNKNOWNs.  The effects are the
game's own data: dumps go under out/ (ignored), never into the repo.
"""
import hashlib, os, re, struct, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
IN = os.path.join(ROOT, 'out', 'mw', 'fx', 'res')
OUT = os.path.join(ROOT, 'out', 'mw', 'fx', 'b')

NONE = 0xFFFFFFFF
END = 0x0000FFFF

TYPES = ['VOID', 'BOOL', 'INT', 'FLOAT', 'STRING', 'TEXTURE', 'TEXTURE1D', 'TEXTURE2D', 'TEXTURE3D', 'TEXTURECUBE',
         'SAMPLER', 'SAMPLER1D', 'SAMPLER2D', 'SAMPLER3D', 'SAMPLERCUBE', 'PIXELSHADER', 'VERTEXSHADER',
         'PIXELFRAGMENT', 'VERTEXFRAGMENT', 'UNSUPPORTED']
CLASSES = ['SCALAR', 'VECTOR', 'MATRIX_ROWS', 'MATRIX_COLUMNS', 'OBJECT', 'STRUCT']
BOOL, INT, FLOAT, STRING = 1, 2, 3, 4
SAMPLERS = (10, 11, 12, 13, 14)
PIXELSHADER, VERTEXSHADER = 15, 16
VECTOR, OBJECT, STRUCT = 1, 4, 5
NUMERIC = (0, 1, 2, 3)

# d3dx9_26's state table, operation 0..181 (fx2-format.md 3.1); 31..46 are WRAP0..WRAP15
STATES = ('ZENABLE FILLMODE SHADEMODE ZWRITEENABLE ALPHATESTENABLE LASTPIXEL SRCBLEND DESTBLEND CULLMODE ZFUNC '
          'ALPHAREF ALPHAFUNC DITHERENABLE ALPHABLENDENABLE FOGENABLE SPECULARENABLE FOGCOLOR FOGTABLEMODE '
          'FOGSTART FOGEND FOGDENSITY RANGEFOGENABLE STENCILENABLE STENCILFAIL STENCILZFAIL STENCILPASS '
          'STENCILFUNC STENCILREF STENCILMASK STENCILWRITEMASK TEXTUREFACTOR '
          + ' '.join('WRAP%d' % i for i in range(16)) +
          ' CLIPPING LIGHTING AMBIENT FOGVERTEXMODE COLORVERTEX LOCALVIEWER NORMALIZENORMALS '
          'DIFFUSEMATERIALSOURCE SPECULARMATERIALSOURCE AMBIENTMATERIALSOURCE EMISSIVEMATERIALSOURCE VERTEXBLEND '
          'CLIPPLANEENABLE POINTSIZE POINTSIZE_MIN POINTSIZE_MAX POINTSPRITEENABLE POINTSCALEENABLE POINTSCALE_A '
          'POINTSCALE_B POINTSCALE_C MULTISAMPLEANTIALIAS MULTISAMPLEMASK PATCHEDGESTYLE DEBUGMONITORTOKEN '
          'INDEXEDVERTEXBLENDENABLE COLORWRITEENABLE TWEENFACTOR BLENDOP POSITIONDEGREE NORMALDEGREE '
          'SCISSORTESTENABLE SLOPESCALEDEPTHBIAS ANTIALIASEDLINEENABLE MINTESSELLATIONLEVEL MAXTESSELLATIONLEVEL '
          'ADAPTIVETESS_X ADAPTIVETESS_Y ADAPTIVETESS_Z ADAPTIVETESS_W ENABLEADAPTIVETESSELLATION '
          'TWOSIDEDSTENCILMODE CCW_STENCILFAIL CCW_STENCILZFAIL CCW_STENCILPASS CCW_STENCILFUNC COLORWRITEENABLE1 '
          'COLORWRITEENABLE2 COLORWRITEENABLE3 BLENDFACTOR SRGBWRITEENABLE DEPTHBIAS SEPARATEALPHABLENDENABLE '
          'SRCBLENDALPHA DESTBLENDALPHA BLENDOPALPHA '
          'COLOROP COLORARG0 COLORARG1 COLORARG2 ALPHAOP ALPHAARG0 ALPHAARG1 ALPHAARG2 RESULTARG BUMPENVMAT00 '
          'BUMPENVMAT01 BUMPENVMAT10 BUMPENVMAT11 TEXCOORDINDEX BUMPENVLSCALE BUMPENVLOFFSET TEXTURETRANSFORMFLAGS '
          'CONSTANT PATCHSEGMENTS FVF PROJECTIONTRANSFORM VIEWTRANSFORM WORLDTRANSFORM TEXTURETRANSFORM '
          'MATERIALDIFFUSE MATERIALAMBIENT MATERIALSPECULAR MATERIALEMISSIVE MATERIALPOWER LIGHTTYPE LIGHTDIFFUSE '
          'LIGHTSPECULAR LIGHTAMBIENT LIGHTPOSITION LIGHTDIRECTION LIGHTRANGE LIGHTFALLOFF LIGHTATTENUATION0 '
          'LIGHTATTENUATION1 LIGHTATTENUATION2 LIGHTTHETA LIGHTPHI LIGHTENABLE VERTEXSHADER PIXELSHADER '
          'VERTEXSHADERCONSTANTF VERTEXSHADERCONSTANTB VERTEXSHADERCONSTANTI VERTEXSHADERCONSTANT '
          'VERTEXSHADERCONSTANT1 VERTEXSHADERCONSTANT2 VERTEXSHADERCONSTANT3 VERTEXSHADERCONSTANT4 '
          'PIXELSHADERCONSTANTF PIXELSHADERCONSTANTB PIXELSHADERCONSTANTI PIXELSHADERCONSTANT PIXELSHADERCONSTANT1 '
          'PIXELSHADERCONSTANT2 PIXELSHADERCONSTANT3 PIXELSHADERCONSTANT4 TEXTURE ADDRESSU ADDRESSV ADDRESSW '
          'BORDERCOLOR MAGFILTER MINFILTER MIPFILTER MIPMAPLODBIAS MAXMIPLEVEL MAXANISOTROPY SRGBTEXTURE '
          'ELEMENTINDEX DMAPOFFSET SAMPLER STATEBLOCK $PRESHADER_VS $PRESHADER_PS').split()
assert len(STATES) == 182

# FXLC opcodes by (opcode, input count), fx2-format.md 4.3
OPS = {(0x000, 0): 'nop', (0x100, 1): 'mov', (0x101, 1): 'neg', (0x103, 1): 'rcp', (0x104, 1): 'frc',
       (0x105, 1): 'exp', (0x106, 1): 'log', (0x107, 1): 'rsq', (0x108, 1): 'sin', (0x109, 1): 'cos',
       (0x10a, 1): 'asin', (0x10b, 1): 'acos', (0x10c, 1): 'atan', (0x200, 2): 'min', (0x201, 2): 'max',
       (0x202, 2): 'lt', (0x203, 2): 'ge', (0x204, 2): 'add', (0x205, 2): 'mul', (0x206, 2): 'atan2',
       (0x208, 2): 'div', (0x300, 3): 'cmp', (0x301, 3): 'movc', (0x500, 2): 'dot', (0x70e, 6): 'dotswiz6',
       (0x70e, 8): 'dotswiz8'}
TABLES = {1: 'imm', 2: 'c', 3: 'v', 4: 'oc', 5: 'ob', 6: 'oi', 7: 'r'}
SETS = 'bics'                   # CTAB register sets 0..3

CTAB, CLIT, FXLC = b'CTAB', b'CLIT', b'FXLC'


class Bad(Exception):
    pass


def hex8(v):
    return '0x%08x' % v


def unk(v):
    return 'UNKNOWN(%s)' % hex8(v & 0xFFFFFFFF)


def qstr(b):
    out = ['"']
    for c in b:
        if c == 0x22:
            out.append('\\"')
        elif c == 0x5C:
            out.append('\\\\')
        elif 0x20 <= c <= 0x7E:
            out.append(chr(c))
        else:
            out.append('\\x%02x' % c)
    out.append('"')
    return ''.join(out)


def g(x, p):
    """C99 %.<p>g; Python's % operator rounds the same way and spells nan, inf and -0 the same."""
    return '%.*g' % (p, x)


def f32(w):
    return '%s:%s' % (hex8(w), g(struct.unpack('<f', struct.pack('<I', w))[0], 9))


def f64(q):
    return '0x%016x:%s' % (q, g(struct.unpack('<d', struct.pack('<Q', q))[0], 17))


def i32(w):
    return str(w - (1 << 32) if w & 0x80000000 else w)


def type_name(t):
    return TYPES[t] if t < len(TYPES) else unk(t)


def class_name(c):
    return CLASSES[c] if c < len(CLASSES) else unk(c)


def cut(b):
    """Bytes up to the first NUL (all of them when there is none)."""
    z = b.find(b'\0')
    return b if z < 0 else b[:z]


class Typedef:
    __slots__ = ('type', 'cls', 'name', 'sem', 'count', 'rows', 'cols', 'members')

    def tt(self):
        s = type_name(self.type) + ' ' + class_name(self.cls)
        if self.cls in NUMERIC:
            s += ' %dx%d' % (self.rows, self.cols)
        elif self.cls == STRUCT:
            s += ' %d' % len(self.members)
        if self.count:
            s += ' [%d]' % self.count
        return s

    def sampler(self):
        return self.cls == OBJECT and self.type in SAMPLERS

    def simple(self):
        return self.cls in NUMERIC or (self.cls == OBJECT and self.type not in SAMPLERS)


class Effect:
    """One fx_2_0 file: the container's tables, read once; dump() writes the text."""

    def __init__(self, name, data):
        self.name, self.data = name, data
        if len(data) < 8:
            raise Bad('%d bytes, too short for the header' % len(data))
        self.tag, self.tables = struct.unpack_from('<2I', data)
        p = self.tables
        self.data0 = self.u32(0)
        self.nparams, self.ntech, self.shaders, self.nobj = self.words(p, 4)
        p += 16
        if self.nobj > len(data) // 4 + 1:    # every id but 0 is held by a 4-byte value
            raise Bad('object count %d is more than a file of %d bytes holds' % (self.nobj, len(data)))
        self.params = []
        for _ in range(self.nparams):
            td, val, flags, na = self.words(p, 4)
            p += 16
            annos, p = self.annos(p, na)
            self.params.append((td, val, flags, annos))
        self.techs = []
        for _ in range(self.ntech):
            name, na, npass = self.words(p, 3)
            p += 12
            annos, p = self.annos(p, na)
            passes = []
            for _ in range(npass):
                pname, pa, nst = self.words(p, 3)
                p += 12
                pannos, p = self.annos(p, pa)
                states = []
                for _ in range(nst):
                    states.append(self.words(p, 4))
                    p += 16
                passes.append((pname, pannos, states))
            self.techs.append((name, annos, passes))
        nstr, nres = self.words(p, 2)
        p += 8
        self.strings = []
        for _ in range(nstr):
            oid, size = self.words(p, 2)
            self.bytes(p + 8, size)     # the data must be inside the file
            self.strings.append((oid, size))
            p += 8 + (size + 3 & ~3)
        self.resources = []
        for _ in range(nres):
            tech, ps, elem, state, usage, size = self.words(p, 6)
            blob = self.bytes(p + 24, size)
            self.resources.append((tech, ps, elem, state, usage, blob))
            p += 24 + (size + 3 & ~3)
        self.left = len(data) - 8 - p   # bytes after the last table entry (0 in a whole file)
        self.budget = len(data) + 64    # target() spends none of it; dump() starts it again
        self.targets = [self.target(r) for r in self.resources]

    def bytes(self, off, n):
        """n bytes at body offset off."""
        if off < 0 or off + 8 + n > len(self.data):
            raise Bad('read of %d bytes at body +0x%x runs past the end of the file' % (n, off))
        return self.data[off + 8:off + 8 + n]

    def words(self, off, n):
        return struct.unpack('<%dI' % n, self.bytes(off, 4 * n))

    def u32(self, off):
        return self.words(off, 1)[0]

    # Names, typedefs and values must lie in the data area, body [0, tables) (fx2-format.md 5.11).

    def dbytes(self, off, n):
        if off + n > self.tables:
            raise Bad('%d bytes at body +0x%x are outside the data area (0x%x bytes)' % (n, off, self.tables))
        return self.bytes(off, n)

    def dwords(self, off, n):
        return struct.unpack('<%dI' % n, self.dbytes(off, 4 * n))

    def du32(self, off):
        return self.dwords(off, 1)[0]

    def name_rec(self, off):
        """The bytes of the name record at off (None for offset 0)."""
        return self.dbytes(off + 4, self.du32(off)) if off else None

    def node(self, n, level):
        """Spend n of the node budget at nesting level (fx2-format.md 5.11)."""
        if level > 32:
            raise Bad('nested deeper than 32 levels')
        self.budget -= n
        if self.budget < 0:
            raise Bad('more typedefs and value elements than a file of %d bytes holds' % len(self.data))

    def annos(self, p, n):
        out = []
        for _ in range(n):
            out.append(self.words(p, 2))
            p += 8
        return out, p

    def typedef(self, off, level=1, count=False):
        """(Typedef, offset after it): fx2-format.md 2.3.  level is its nesting; with count the read spends
        the node budget, as each of the dump's reads does (5.11)."""
        self.node(1 if count else 0, level)
        t = Typedef()
        t.type, t.cls, t.name, t.sem, t.count = self.dwords(off, 5)
        self.name_rec(t.name)
        self.name_rec(t.sem)
        t.rows = t.cols = 0
        t.members = []
        off += 20
        if t.cls in NUMERIC:
            a, b = self.dwords(off, 2)
            t.cols, t.rows = (a, b) if t.cls == VECTOR else (b, a)
            off += 8
        elif t.cls == STRUCT:
            m = self.du32(off)
            off += 4
            for _ in range(m):
                mt, off = self.typedef(off, level + 1, count)
                t.members.append(mt)
        return t, off

    def str_at(self, off):
        """QSTR of the name record at off, '-' for offset 0."""
        if off == 0:
            return '-'
        return qstr(cut(self.name_rec(off)))

    def sampler_at(self, val, elem):
        """Body offset of element elem's sampler block in a sampler value."""
        pos = val
        for _ in range(elem):
            pos += 4 + 16 * self.du32(pos)
        return pos

    def target(self, r):
        """The state a resource is attached to, as (key, typedef, value offset), or None."""
        tech, ps, elem, state, usage, blob = r
        if tech == NONE:
            if ps >= len(self.params):
                return None
            td, val = self.params[ps][:2]
            t = self.typedef(td)[0]
            if not t.sampler():
                return None
            if t.count:
                if elem >= t.count:
                    return None
                e = elem
            else:
                e = 0
            pos = self.sampler_at(val, e)
            if state >= self.du32(pos):
                return None
            st = self.dwords(pos + 4 + 16 * state, 4)
            key = ('param', ps, e, state)
        else:
            if tech >= len(self.techs):
                return None
            passes = self.techs[tech][2]
            if ps >= len(passes) or state >= len(passes[ps][2]):
                return None
            st = passes[ps][2][state]
            key = ('pass', tech, ps, state)
        return key, self.typedef(st[2])[0], st[3]

    # The dump.

    def dump(self):
        self.lines = []
        self.holder = {}                # object id -> type of the first value holding it
        self.res_on = {}                # state key -> [resource numbers]
        self.obj_res = {}               # object id -> [resource numbers]
        self.budget = len(self.data) + 64   # typedefs and value elements the dump may read (5.11)
        for r, tg in enumerate(self.targets):
            if tg is None:
                continue
            key, t, val = tg
            self.res_on.setdefault(key, []).append(r)
            if t.cls == OBJECT and t.type not in SAMPLERS and t.count == 0:
                self.obj_res.setdefault(self.du32(val), []).append(r)
        data = self.data
        self.put(0, 'fx2dump 1')
        self.put(0, 'effect %s size %d sha256 %s' % (qstr(self.name.encode()), len(data),
                                                     hashlib.sha256(data).hexdigest()))
        self.put(0, 'header tag %s tables %s data0 %s params %d techniques %d shaders %d objects %d strings %d '
                 'resources %d' % (hex8(self.tag), hex8(self.tables), hex8(self.data0), self.nparams, self.ntech,
                                   self.shaders, self.nobj, len(self.strings), len(self.resources)))
        for i, (td, val, flags, annos) in enumerate(self.params):
            t = self.typedef(td, 1, True)[0]
            line = 'param %d %s name %s sem %s flags %s annotations %d' % (
                i, t.tt(), self.str_at(t.name), self.str_at(t.sem), hex8(flags), len(annos))
            self.value(line, t, val, 0, '', lambda e, k, i=i: self.res_on.get(('param', i, e, k), []))
            self.annotations(annos, 1)
        for i, (name, annos, passes) in enumerate(self.techs):
            self.put(0, 'technique %d name %s annotations %d passes %d' % (i, self.str_at(name), len(annos),
                                                                         len(passes)))
            self.annotations(annos, 1)
            for j, (pname, pannos, states) in enumerate(passes):
                self.put(1, 'pass %d name %s annotations %d states %d' % (j, self.str_at(pname), len(pannos),
                                                                         len(states)))
                self.annotations(pannos, 2)
                for k, st in enumerate(states):
                    self.state(k, st, 2, self.res_on.get(('pass', i, j, k), []))
        for oid in range(self.nobj):
            s = 'object %d %s' % (oid, self.holder.get(oid, '-'))
            s += ''.join(' string %d size %d' % (k, size) for k, (o, size) in enumerate(self.strings) if o == oid)
            s += ''.join(' res %d' % r for r in self.obj_res.get(oid, []))
            self.put(0, s)
        for k, (o, size) in enumerate(self.strings):
            if o >= self.nobj:
                self.put(0, 'object %s string %d size %d' % (unk(o), k, size))
        for r, res in enumerate(self.resources):
            self.resource(r, res)
        self.put(0, 'end')
        return ''.join(self.lines)

    def put(self, depth, s):
        self.lines.append('  ' * depth + s + '\n')

    def annotations(self, annos, depth):
        for k, (td, val) in enumerate(annos):
            t = self.typedef(td, 1, True)[0]
            self.value('annotation %d %s name %s sem %s' % (k, t.tt(), self.str_at(t.name), self.str_at(t.sem)),
                       t, val, depth)

    def state(self, k, st, depth, res, level=1):
        op, index, td, val = st
        t = self.typedef(td, level, True)[0]
        name = STATES[op] if op < len(STATES) else unk(op)
        self.value('state %d %s[%d] op %d %s' % (k, name, index, op, t.tt()), t, val, depth,
                   ''.join(' res %d' % r for r in res), level=level)

    def frag(self, t, pos):
        """(FRAG, offset after it) of one simple element at pos."""
        if t.cls == OBJECT:
            oid = self.du32(pos)
            self.holder.setdefault(oid, type_name(t.type))
            return 'obj ' + (str(oid) if oid < self.nobj else unk(oid)), pos + 4
        n = t.rows * t.cols
        if n == 0:
            self.dbytes(pos, 0)         # no bytes, but it still starts inside the data area
            return '-', pos
        w = self.dwords(pos, n)
        if t.type == FLOAT:
            s = ' '.join(f32(x) for x in w)
        elif t.type in (BOOL, INT):
            s = ' '.join(i32(x) for x in w)
        else:
            s = ' '.join(hex8(x) for x in w)
        return s, pos + 4 * n

    def value(self, line, t, pos, depth, suffix='', sampres=None, level=1):
        """The owner line and its value lines (fx2-format.md 5.4); returns the offset after the value.
        sampres(e, k) gives the resources on state k of sampler element e (parameters only); level is the
        value's nesting (5.11)."""
        self.node(max(1, t.count), level)
        if t.simple() and t.count == 0:
            s, pos = self.frag(t, pos)
            self.put(depth, line + ' = ' + s + suffix)
            return pos
        self.put(depth, line + suffix)
        for e in range(max(1, t.count)):
            at = ' [%d]' % e if t.count else ''
            if t.simple():
                s, pos = self.frag(t, pos)
                self.put(depth + 1, 'value%s = %s' % (at, s))
            elif t.sampler():
                n = self.du32(pos)
                pos += 4
                self.put(depth + 1, 'sampler%s states %d' % (at, n))
                for k in range(n):
                    self.state(k, self.dwords(pos, 4), depth + 2, sampres(e, k) if sampres else [], level + 1)
                    pos += 16
            elif t.cls == STRUCT:
                self.put(depth + 1, 'struct' + at)
                for m, mt in enumerate(t.members):
                    pos = self.value('member %d %s name %s sem %s' % (m, mt.tt(), self.str_at(mt.name),
                                                                     self.str_at(mt.sem)), mt, pos, depth + 2,
                                     level=level + 1)
            else:
                self.put(depth + 1, 'value%s = %s' % (at, unk(t.cls)))
        return pos

    def resource(self, r, res):
        tech, ps, elem, state, usage, blob = res
        if tech == NONE:
            self.put(0, 'resource %d param %d element %s state %d usage %d size %d' % (
                r, ps, '-' if elem == NONE else unk(elem) if self.targets[r] and elem and
                not self.typedef(self.params[ps][0])[0].count else str(elem), state, usage, len(blob)))
        else:
            self.put(0, 'resource %d technique %d pass %d state %d usage %d size %d' % (
                r, tech, ps, state, usage, len(blob)))
        tg = self.targets[r]
        kind = tg[1].type if tg else None
        if tg and usage == 0 and kind in (VERTEXSHADER, PIXELSHADER):
            shader(self, blob, 1)
        elif tg and usage == 0 and kind in (BOOL, INT, FLOAT, STRING):
            preshader(self, blob, 1)
        elif tg and usage == 1:
            self.put(1, 'name ' + qstr(cut(blob)))
        elif tg and usage == 2:
            if len(blob) < 4:                   # no room for S2
                self.put(1, 'select ' + unk(len(blob)))
                return
            s2 = struct.unpack_from('<I', blob)[0]
            if 4 + s2 > len(blob):
                self.put(1, 'select ' + unk(s2))
            else:
                self.put(1, 'select ' + qstr(cut(blob[4:4 + s2])))
                preshader(self, blob[4 + s2:], 1)
        else:
            self.put(1, unk(usage))


def comments(w, i, n):
    """[(index, length)] of the run of comment tokens from dword i, and the index after them."""
    out = []
    while i < n and w[i] & 0xFFFF == 0xFFFE:
        L = (w[i] >> 16) & 0x7FFF
        out.append((i, L))
        i += 1 + L
    return out, i


def fourcc(b, i, L):
    """The 4 bytes of the first dword of the comment at dword i ('' when it is empty or that dword is past the blob)."""
    return b[4 * (i + 1):4 * (i + 2)] if L and i + 2 <= len(b) // 4 else b''


def payload(b, i, L):
    """The bytes of the comment at dword i after its FOURCC, up to its end or the blob's last whole dword."""
    return b[4 * (i + 2):4 * min(i + 1 + L, len(b) // 4)]


def comment_lines(fx, b, found, depth):
    for i, L in found:
        fx.put(depth, 'comment %s dwords %d' % (qstr(fourcc(b, i, L)), L))


def ctab_str(reg, off):
    if off == 0:
        return '-'
    if off >= len(reg):
        return unk(off)
    return qstr(cut(reg[off:]))


def ctab(fx, b, i, L, depth):
    """The ctab and const lines of the CTAB comment at dword i; its offsets count from after the FOURCC."""
    reg = payload(b, i, L)
    if len(reg) < 28:                   # the comment runs past the end of the blob
        return
    size, creator, version, count, info, flags, target = struct.unpack_from('<7I', reg)
    fx.put(depth, 'ctab size %d creator %s version %s target %s flags %s constants %d' % (
        size, ctab_str(reg, creator), hex8(version), ctab_str(reg, target), hex8(flags), count))
    for k in range(count):
        o = info + 20 * k
        if o + 20 > len(reg):
            fx.put(depth, 'const %d %s' % (k, unk(o)))
            break
        name, rset, rindex, rcount, _, tinfo, default = struct.unpack_from('<I4H2I', reg, o)
        if tinfo + 16 > len(reg):           # D3DXSHADER_TYPEINFO is 16 bytes
            typeinfo = unk(tinfo)
        else:
            cls, typ, rows, cols, elems, members = struct.unpack_from('<6H', reg, tinfo)
            typeinfo = '%s %s %dx%d elements %d members %d' % (class_name(cls), type_name(typ), rows, cols, elems,
                                                               members)
        fx.put(depth, 'const %d %s %s%d count %d %s default %s' % (
            k, ctab_str(reg, name), SETS[rset] if rset < len(SETS) else unk(rset), rindex, rcount, typeinfo,
            'yes' if default else 'no'))


def shader(fx, b, depth):
    """Shader block (fx2-format.md 5.9)."""
    n = len(b) // 4
    w = struct.unpack_from('<%dI' % n, b)
    v = w[0] if n else 0
    if v >> 16 not in (0xFFFE, 0xFFFF):
        fx.put(depth, 'shader %s dwords %d' % (unk(v), n))
        return
    major, minor = (v >> 8) & 0xFF, v & 0xFF
    vn = ('vs' if v >> 16 == 0xFFFE else 'ps') + ('_2_x' if (major, minor) == (2, 1) else '_%d_%d' % (major, minor))
    i, count, found, stop = 1, 0, [], None
    while i < n:
        t = w[i]
        if t == END:
            stop = i
            break
        if t & 0xFFFF == 0xFFFE:
            L = (t >> 16) & 0x7FFF
            found.append((i, L))
            i += 1 + L
            continue
        count += 1
        if major >= 2:
            i += 1 + ((t >> 24) & 0xF)
        elif t & 0xFFFF in (0x51, 0x30):        # def, defi: register and four values
            i += 6
        elif t & 0xFFFF == 0x2F:                # defb: register and one value
            i += 3
        else:
            i += 1
            while i < n and w[i] & 0x80000000:
                i += 1
    if stop is None:
        stop = i
    fx.put(depth, 'shader %s %s dwords %d instructions %d %s' % (vn, hex8(v), n, count,
                                                                 'end' if stop == n - 1 else unk(stop)))
    comment_lines(fx, b, found, depth)
    for i, L in found:
        if L >= 8 and fourcc(b, i, L) == CTAB:
            ctab(fx, b, i, L, depth)


def operand(x, p):
    """(text, words) of the FXLC operand at x[p], (None, flag) for an unknown flag, None if it runs out."""
    if p >= len(x):
        return None
    flag = x[p]
    if flag == 0:
        if p + 3 > len(x):
            return None
        return '%s[%d]' % (TABLES.get(x[p + 1]) or unk(x[p + 1]), x[p + 2]), 3
    if flag == 1:
        if p + 5 > len(x):
            return None
        return '%s[%d+%s[%d]]' % (TABLES.get(x[p + 3]) or unk(x[p + 3]), x[p + 4],
                                  TABLES.get(x[p + 1]) or unk(x[p + 1]), x[p + 2]), 5
    return None, flag


def preshader(fx, b, depth):
    """Preshader block (fx2-format.md 5.10)."""
    n = len(b) // 4
    w = struct.unpack_from('<%dI' % n, b)
    v = w[0] if n else 0
    if v >> 16 != 0x4658:
        fx.put(depth, 'preshader %s dwords %d' % (unk(v), n))
        return
    found, i = comments(w, 1, n)
    fx.put(depth, 'preshader %s dwords %d %s' % (hex8(v), n, 'end' if i == n - 1 and w[i] == END else unk(i)))
    comment_lines(fx, b, found, depth)
    first = {}
    for i, L in found:
        first.setdefault(fourcc(b, i, L), (i, L))
    if CTAB in first and first[CTAB][1] >= 8:
        ctab(fx, b, first[CTAB][0], first[CTAB][1], depth)
    if CLIT in first:
        i, L = first[CLIT]
        x = payload(b, i, L)
        if len(x) < 4:                          # no count
            fx.put(depth, 'literals ' + unk(L))
        else:
            count = struct.unpack_from('<I', x)[0]
            if 4 + 8 * count > len(x):          # the doubles do not fit
                fx.put(depth, 'literals ' + unk(count))
            else:
                fx.put(depth, 'literals %d' % count)
                for k in range(count):
                    fx.put(depth, 'lit %d %s' % (k, f64(struct.unpack_from('<Q', x, 4 + 8 * k)[0])))
    else:
        fx.put(depth, 'literals -')
    if FXLC not in first:
        fx.put(depth, 'instructions -')
        return
    i, L = first[FXLC]
    x = w[i + 2:min(i + 1 + L, n)]
    if not x:                                   # no count
        fx.put(depth, 'instructions ' + unk(L))
        fx.put(depth, 'trailer')
        return
    m = x[0]
    fx.put(depth, 'instructions %d' % m)
    p = 1
    for k in range(m):
        start = p
        if p + 2 > len(x):
            fx.put(depth, 'ins %d %s' % (k, unk(len(x) - start)))
            p = len(x)
            break
        tok, nin = x[p], x[p + 1]
        p += 2
        op = (tok >> 20) & 0x7FF
        mn = (OPS.get((op, nin)) or unk(op)) + ('.s' if tok >> 31 else '')
        head = 'ins %d %s %d%s' % (k, mn, tok & 0xFFFF, ' ' + unk(tok) if tok & 0xF0000 else '')
        ops, bad = [], None
        for _ in range(nin + 1):
            o = operand(x, p)
            if o is None:
                break
            if o[0] is None:
                bad = o[1]
                break
            ops.append(o[0])
            p += o[1]
        if bad is not None:
            fx.put(depth, '%s %s' % (head, unk(bad)))
            break
        if len(ops) < nin + 1:
            fx.put(depth, 'ins %d %s' % (k, unk(len(x) - start)))
            p = len(x)
            break
        fx.put(depth, '%s %s' % (head, ' '.join(ops[-1:] + ops[:-1])))
    fx.put(depth, 'trailer' + ''.join(' ' + hex8(t) for t in x[p:]))


# The count table.

VERSIONS = ['vs_1_1', 'vs_2_0', 'vs_2_x', 'vs_3_0', 'ps_1_1', 'ps_1_2', 'ps_1_3', 'ps_1_4', 'ps_2_0', 'ps_2_x',
            'ps_3_0']
COLUMNS = [('params', r'param '), ('techs', r'technique '), ('passes', r'  pass '),
           ('annos', r' *annotation '), ('states', r' *state '), ('res', r'resource '),
           ('presh', r'  preshader '), ('ins', r'  ins ')]


def counts(text):
    """{column: count} of one dump, read from its lines."""
    c = {}
    for name, pat in COLUMNS:
        c[name] = len(re.findall('^' + pat, text, re.M))
    for v in re.findall(r'^  shader (\S+) ', text, re.M):
        c[v] = c.get(v, 0) + 1
    c['unknown'] = text.count('UNKNOWN(')
    return c


def table(rows):
    vers = [v for v in VERSIONS if any(v in c for _, c in rows)]
    vers += sorted({k for _, c in rows for k in c} - set(vers) - {n for n, _ in COLUMNS} - {'unknown'})
    cols = [n for n, _ in COLUMNS[:6]] + vers + ['presh', 'ins', 'unknown']
    total = {k: sum(c.get(k, 0) for _, c in rows) for k in cols}
    width = max(len(n) for n, _ in rows + [('total', 0)])
    print('%-*s %s' % (width, 'effect', ' '.join('%7s' % k for k in cols)))
    for name, c in rows + [('total', total)]:
        print('%-*s %s' % (width, name, ' '.join('%7d' % c.get(k, 0) for k in cols)))


def dump_file(path):
    name = os.path.basename(path)[:-4] if path.endswith('.fxo') else os.path.basename(path)
    fx = Effect(name, open(path, 'rb').read())
    return fx, fx.dump()


def main():
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == '--one':
        try:
            sys.stdout.write(dump_file(a[1])[1])
        except Bad as e:
            sys.exit('fx2dump: %s: %s' % (a[1], e))
        return
    if len(a) > 2 or any(x.startswith('-') for x in a):
        sys.exit(__doc__)
    indir = a[0] if a else IN
    outdir = a[1] if len(a) > 1 else OUT
    names = sorted(f for f in os.listdir(indir) if f.endswith('.fxo'))
    if not names:
        sys.exit('fx2dump: no .fxo files in %s' % indir)
    os.makedirs(outdir, exist_ok=True)
    rows, failed = [], 0
    for f in names:
        try:
            fx, text = dump_file(os.path.join(indir, f))
        except Bad as e:
            print('fx2dump: %s: %s' % (f, e), file=sys.stderr)
            failed += 1
            continue
        if fx.left:
            print('fx2dump: %s: %d bytes after the tables' % (f, fx.left), file=sys.stderr)
            failed += 1
        with open(os.path.join(outdir, f[:-4] + '.txt'), 'w', newline='\n') as out:
            out.write(text)
        rows.append((f[:-4], counts(text)))
    table(rows)
    print('fx2dump: %d of %d effects into %s' % (len(rows), len(names), outdir))
    if failed:
        sys.exit(1)


if __name__ == '__main__':
    main()
