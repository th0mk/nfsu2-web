#!/usr/bin/env python3
"""The FXLC preshader evaluator of docs/mw/fx-preshader.md, implementation B.

  tools/fxpres.py vm <cases.txt> <out.txt|->                 'fxpcase 1' -> 'fxpres 1' (fx-preshader.md 6.2)
  tools/fxpres.py effect <rounds.txt> <out.txt|-> <NAME>.fxo...  'fxpround 1' -> 'fxpeff 1' (6.3): SetValue and the
                                                             typed setters on the model, then every pass's
                                                             preshader states
  tools/fxpres.py --self-test                                the number model on its own (3.1)

Written from fx-preshader.md apart from implementation A (runtime/mw/fxpres.c, tools/mw/fxpresrun.c): the two must
write the same texts byte for byte (6.4).  Where A computes in integers with a 64-bit significand, B computes every
exact operation with Python's rationals and rounds the exact value, first to the precision control's bits with no
exponent limit (the x87's registers), then to a double; the square root is an integer square root of a scaled
rational.  The approximate class (3.2) calls the platform's C library through ctypes, as A does, so the two agree on
one host.  The effect path (4) uses tools/fxmodel.py, the B side of the effect model.
"""
import ctypes, importlib.util, os, struct, sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('fxmodel', os.path.join(HERE, 'fxmodel.py'))
M = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(M)
F = M.F

# ---- the C library, for the approximate class and floor (3.2) ----
_libm = ctypes.CDLL(None)


def _f1(name):
    f = getattr(_libm, name)
    f.restype, f.argtypes = ctypes.c_double, [ctypes.c_double]
    return f


C_EXP2, C_LOG, C_SIN, C_COS, C_ASIN, C_ACOS, C_ATAN, C_FLOOR = map(
    _f1, ('exp2', 'log', 'sin', 'cos', 'asin', 'acos', 'atan', 'floor'))
C_ATAN2 = _libm.atan2
C_ATAN2.restype, C_ATAN2.argtypes = ctypes.c_double, [ctypes.c_double, ctypes.c_double]


def bits(x):
    return struct.unpack('<Q', struct.pack('<d', x))[0]


def dbl(b):
    return struct.unpack('<d', struct.pack('<Q', b))[0]


# ---- the number model (3.1) ----
# A value is ('z', sign), ('f', sign, magnitude as a Fraction), ('i', sign) or ('n', sign, significand, snan): the
# significand of a NaN is the x87's 64 bits, always quiet; snan says a memory operand was signalling.

HALF = Fraction(1, 2)
DEFAULT_NAN = ('n', 1, 0xC000000000000000, False)
ONE = ('f', 0, Fraction(1))
ZERO_BITS, ONE_BITS = 0, 0x3FF0000000000000
LOG2E = 0x3FF71547652B82FE


def floor_log2(q):
    """the e with 2^e <= q < 2^(e+1), q > 0"""
    n, d = q.numerator, q.denominator
    e = n.bit_length() - d.bit_length()
    if (n << -e if e < 0 else n) < (d << e if e > 0 else d):
        e -= 1
    return e


def pow2(k):
    return Fraction(1 << k) if k >= 0 else Fraction(1, 1 << -k)


def rne(q):
    """q rounded to an integer, ties to even"""
    n = q.numerator // q.denominator
    r = q - n
    return n + 1 if r > HALF or (r == HALF and n % 2) else n


def round_p(q, p):
    """q > 0 rounded to p significant bits, no exponent limit (the x87 register)"""
    e = floor_log2(q)
    return rne(q / pow2(e - p + 1)) * pow2(e - p + 1)


def mem(b):
    """a double as an operand from memory"""
    s, ex, fr = b >> 63, (b >> 52) & 0x7FF, b & ((1 << 52) - 1)
    if ex == 0x7FF:
        if fr == 0:
            return ('i', s)
        return ('n', s, (1 << 63) | (1 << 62) | ((fr & ((1 << 51) - 1)) << 11), not (fr >> 51))
    if ex == 0:
        return ('z', s) if fr == 0 else ('f', s, Fraction(fr, 1 << 1074))
    return ('f', s, Fraction((1 << 52) | fr) * pow2(ex - 1075))


def ld(b):
    """fld qword: a signalling NaN is loaded quiet"""
    v = mem(b)
    return v[:3] + (False,) if v[0] == 'n' else v


def to_d(v):
    """fstp qword"""
    k, s = v[0], v[1]
    if k == 'z':
        return s << 63
    if k == 'i':
        return (s << 63) | (0x7FF << 52)
    if k == 'n':
        return (s << 63) | (0x7FF << 52) | (1 << 51) | ((v[2] >> 11) & ((1 << 52) - 1))
    q = v[2]
    if floor_log2(q) < -1022:                                   # subnormal: whole units of 2^-1074
        return (s << 63) | rne(q * pow2(1074))                   # 2^52 units is the smallest normal
    r = round_p(q, 53)
    e = floor_log2(r)
    if e > 1023:
        return (s << 63) | (0x7FF << 52)
    return (s << 63) | ((e + 1023) << 52) | (int(r / pow2(e - 52)) - (1 << 52))


def d2f(b):
    """fld qword; fstp dword"""
    v = ld(b)
    k, s = v[0], v[1]
    if k == 'z':
        return s << 31
    if k == 'i':
        return (s << 31) | 0x7F800000
    if k == 'n':
        return (s << 31) | 0x7FC00000 | ((v[2] >> 40) & 0x7FFFFF)
    q = v[2]
    if floor_log2(q) < -126:
        return (s << 31) | rne(q * pow2(149))
    r = round_p(q, 24)
    e = floor_log2(r)
    if e > 127:
        return (s << 31) | 0x7F800000
    return (s << 31) | ((e + 127) << 23) | (int(r / pow2(e - 23)) - (1 << 23))


def f2d(w):
    """fld dword; fstp qword: exact, a signalling NaN quiet"""
    s, ex, fr = w >> 31, (w >> 23) & 0xFF, w & 0x7FFFFF
    if ex == 0xFF:
        return (s << 63) | (0x7FF << 52) | (fr << 29) | (1 << 51 if fr else 0)
    q = Fraction(fr, 1 << 149) if ex == 0 else Fraction((1 << 23) | fr) * pow2(ex - 150)
    return to_d(('z', s) if q == 0 else ('f', s, q))


def i2d(w):
    """fild dword: exact"""
    v = w - (1 << 32) if w >> 31 else w
    return to_d(('z', 0) if v == 0 else ('f', 1 if v < 0 else 0, Fraction(abs(v))))


def ftol(b):
    """_ftol2's low dword: truncation; NaN, infinities and |x| >= 2^63 give 0x8000000000000000"""
    v = ld(b)
    if v[0] != 'f' or v[2] >= 1 << 63:
        return 0
    t = int(v[2])
    return (-t if v[1] else t) & 0xFFFFFFFF


def pick(reg, m):
    """the NaN of an operation on a register and a memory operand (Intel's table, 3.1)"""
    if reg[0] != 'n':
        return m[:3] + (False,)
    if m[0] != 'n' or m[3]:
        return reg[:3] + (False,)
    if reg[2] != m[2]:
        return reg if reg[2] > m[2] else m[:3] + (False,)
    return reg if reg[1] <= m[1] else m[:3] + (False,)


def signed(v):
    return -v[2] if v[1] else v[2]


def fin(q, p):
    """an exact non-zero rational result, rounded to p bits"""
    return ('f', 1 if q < 0 else 0, round_p(abs(q), p))


def add(reg, m, p):
    if reg[0] == 'n' or m[0] == 'n':
        return pick(reg, m)
    if reg[0] == 'i' or m[0] == 'i':
        if reg[0] == 'i' and m[0] == 'i' and reg[1] != m[1]:
            return DEFAULT_NAN
        return reg if reg[0] == 'i' else m
    if reg[0] == 'z' and m[0] == 'z':
        return ('z', reg[1] & m[1])
    q = (signed(reg) if reg[0] == 'f' else 0) + (signed(m) if m[0] == 'f' else 0)
    return ('z', 0) if q == 0 else fin(q, p)


def mul(reg, m, p):
    if reg[0] == 'n' or m[0] == 'n':
        return pick(reg, m)
    s = reg[1] ^ m[1]
    kinds = {reg[0], m[0]}
    if kinds == {'i', 'z'}:
        return DEFAULT_NAN
    if 'i' in kinds:
        return ('i', s)
    if 'z' in kinds:
        return ('z', s)
    return ('f', s, round_p(reg[2] * m[2], p))


def div(a, b, p):
    if a[0] == 'n' or b[0] == 'n':
        return pick(a, b)
    s = a[1] ^ b[1]
    if (a[0], b[0]) in (('i', 'i'), ('z', 'z')):
        return DEFAULT_NAN
    if a[0] == 'i' or b[0] == 'z':
        return ('i', s)
    if a[0] == 'z' or b[0] == 'i':
        return ('z', s)
    return ('f', s, round_p(a[2] / b[2], p))


def isqrt(n):
    x = 1 << ((n.bit_length() + 1) // 2)
    while True:
        y = (x + n // x) // 2
        if y >= x:
            return x
        x = y


def sqrt(a, p):
    """of a value >= 0: +-0, +inf and NaN stay"""
    if a[0] != 'f':
        return a
    q = a[2]
    k = 0
    while True:                                                 # enough bits: sqrt(q * 4^k) >= 2^(p + 2)
        sc = q * pow2(2 * k)
        if sc.numerator >= sc.denominator << (2 * p + 4):
            break
        k += 8
    n, d = sc.numerator, sc.denominator
    r = isqrt(n // d)
    exact = n % d == 0 and r * r == n // d
    # sqrt(q) = (r + delta) / 2^k with 0 <= delta < 1, delta = 0 only when exact: round r with delta as sticky
    e = r.bit_length() - p
    top, rem = r >> e, r & ((1 << e) - 1)
    half = 1 << (e - 1)
    if rem > half or (rem == half and (not exact or top & 1)):
        top += 1
    return ('f', 0, Fraction(top) * pow2(e - k))


def cmp(a, b):
    """-1, 0, 1, or 2 unordered"""
    if a[0] == 'n' or b[0] == 'n':
        return 2

    def order(v):                                   # infinities outside every finite value
        if v[0] == 'i':
            return (-1 if v[1] else 1, 0)
        return (0, 0 if v[0] == 'z' else signed(v))
    x, y = order(a), order(b)
    return -1 if x < y else 1 if x > y else 0


def ldst(b):
    return to_d(ld(b))


def neg(v):
    return (v[0], v[1] ^ 1) + v[2:]


def x87_rsq(b, p):
    v = ld(b)
    return to_d(div(ONE, sqrt((v[0], 0) + v[2:], p), p))


TWO63 = Fraction(1 << 63)


def approx1(op, b, p):
    v = ld(b)
    if op == 0x106:                             # fabs, then fyl2x with ln 2, then fmul log2e (3.2)
        v = (v[0], 0) + v[2:]
        if v[0] == 'n':
            return to_d(v)
        if v[0] == 'z':
            return to_d(('i', 1))               # fyl2x(+0): -inf, the zero divide masked
        return to_d(mul(ld(bits(C_LOG(dbl(to_d(v))))), mem(LOG2E), p))
    if v[0] == 'n':
        return to_d(v)
    x = dbl(b)
    if op == 0x105:
        return bits(C_EXP2(x))
    if op in (0x108, 0x109):
        if v[0] == 'i':
            return to_d(DEFAULT_NAN)
        if v[0] == 'f' and v[2] > TWO63:
            return b
        return bits(C_SIN(x) if op == 0x108 else C_COS(x))
    if op == 0x10A:
        return bits(C_ASIN(x))
    if op == 0x10B:
        return bits(C_ACOS(x))
    return bits(C_ATAN(x))


def atan2(y, x):
    a, b = ld(y), ld(x)
    if a[0] == 'n' or b[0] == 'n':
        return to_d(pick(a, b))
    return bits(C_ATAN2(dbl(y), dbl(x)))


# ---- the program (2) ----
FXLC, CLIT = 0x434C5846, 0x54494C43
ERRS = ['ok', 'version', 'nofxlc', 'noclit', 'operand', 'scan', 'noise', 'inputs', 'range', 'stale', 'type',
        'arg', 'nomem']
D3DX_REFUSES = ('version', 'nofxlc', 'noclit', 'operand')


class Fail(Exception):
    pass


def u32(b, i):
    return struct.unpack_from('<I', b, i)[0]


def search(b, fourcc):
    """D3DXFindShaderComment bounded to the blob (2.1): the data's byte offset, None at the end token"""
    ver, i = u32(b, 0), 4
    while True:
        if i + 4 > len(b):
            raise Fail('scan')
        t = u32(b, i)
        if t >> 31 == 0:
            op = t & 0xFFFF
            if op == 0xFFFF:
                return None
            if op == 0xFFFE:
                L = (t >> 16) & 0x7FFF
                if L > 1:
                    if i + 8 > len(b):
                        raise Fail('scan')
                    if u32(b, i + 4) == fourcc:
                        return i + 8
                i += 4 * L
            elif ver & 0xFFFF >= 0x200:
                i += 4 * ((t >> 24) & 0xF)
            elif op == 0x51:
                i += 20
        i += 4


class Prog:
    pass


def load(b):
    """the loader's checks (2.2) and the comment search bounded to the blob (5.1): a Prog, or raises Fail(reason)"""
    if len(b) < 4:
        raise Fail('scan')
    v = u32(b, 0)
    if not (0x46580200 <= v <= 0x46580201 or v == 0x54580100):
        raise Fail('version')
    fx = search(b, FXLC)
    if fx is None:
        raise Fail('nofxlc')
    cl = search(b, CLIT)
    if cl is None:
        raise Fail('noclit')
    if cl + 4 > len(b) or fx + 4 > len(b):
        raise Fail('scan')
    end = len(b)
    pr = Prog()
    pr.blob, pr.lit, pr.nlit = b, cl + 4, u32(b, cl) or 1
    pr.ins = []

    def word(at):
        if at + 4 > end:
            raise Fail('operand')
        return u32(b, at)
    q = fx + 4
    for _ in range(u32(b, fx)):
        if q >= end:
            raise Fail('operand')
        tok, nin = word(q), word(q + 4)
        q += 8
        opds = []
        for _k in range(nin):
            npairs = word(q)
            q += 4
            if q >= end:
                raise Fail('operand')
            pairs = []
            for _j in range(npairs):
                t = word(q)
                if q + 8 >= end:
                    raise Fail('operand')
                pairs.append((t, u32(b, q + 4)))
                q += 8
                if t == 9:
                    raise Fail('operand')
            t, off = word(q), word(q + 4)
            q += 8
            if t == 0 or t > 9:
                raise Fail('operand')
            opds.append(pairs + [(t, off)])
        if word(q) != 0:
            raise Fail('operand')
        q += 4
        t, off = word(q), word(q + 4)
        q += 8
        if q >= end or t not in (4, 5, 6, 7):
            raise Fail('operand')
        pr.ins.append((tok >> 20 & 0x7FF, tok & 0xFFFFF, tok >> 31, nin, opds, (t, off)))
    pr.approx = any((op == 0x206) if sc else (op in (0x105, 0x106, 0x206) or 0x108 <= op <= 0x10C)
                    for op, n, sc, nin, opds, out in pr.ins)
    return pr


class Tables:
    def __init__(self, c, cmask, o, omask, v=None, vmask=0, pc=64, temps_unknown=False):
        self.c, self.cmask, self.o, self.omask, self.v, self.vmask, self.pc = c, cmask, o, omask, v, vmask, pc
        self.r = [0] * 256
        # temps_unknown: the temporaries hold what D3DX's stack held, so reading one this run has not written stops
        # (5.1); written: the ones this run wrote
        self.temps_unknown, self.written = temps_unknown, set()


class Range(Exception):
    pass


class Stale(Exception):
    pass


class Inputs(Exception):
    pass


class Noise(Exception):
    pass


def place(pr, x, t, e):
    """table t, element e (masked, modulo, wrapped) as (table name, base); None for table 8, which D3DX's decode
    passes over (2.4); 'nil' for table 9, at address 0 in both of D3DX's paths"""
    if t == 1:
        return ('lit', (pr.lit + 8 * (e % pr.nlit)) & 0xFFFFFFFF)
    if t == 2:
        return ('c', e & x.cmask)
    if t == 3:
        return ('v', e & x.vmask)
    if t == 7:
        return ('r', e & 0xFF)
    if t == 8:
        return None
    if t == 9:
        return ('nil', 0)
    return ('o', e & x.omask)


def get(pr, x, ref, k):
    t, base = ref
    if t == 'lit':
        at = (base + 8 * k) & 0xFFFFFFFF
        if at + 8 > len(pr.blob):
            raise Range()
        return struct.unpack_from('<Q', pr.blob, at)[0]
    tab = {'c': x.c, 'v': x.v, 'o': x.o, 'r': x.r}.get(t)
    if tab is None or base + k >= len(tab):
        raise Range()
    if t == 'r' and x.temps_unknown and base + k not in x.written:
        raise Stale()
    return tab[base + k]


def put(x, ref, k, v):
    t, base = ref
    tab = x.r if t == 'r' else x.o
    if base + k >= len(tab):
        raise Range()
    tab[base + k] = v
    if t == 'r':
        x.written.add(base + k)


def resolve(pr, x, pairs):
    e = 0
    for j, (t, off) in enumerate(pairs):
        e = (off + 4 * e) & 0xFFFFFFFF
        if j + 1 < len(pairs):
            if 1 <= t <= 7:
                e = ftol(get(pr, x, place(pr, x, t, e), 0))
            continue
        return place(pr, x, t, e)


def select(op, u, w):
    c = cmp(ld(u), ld(w))
    if op == 0x200:
        return ldst(u) if c == -1 else ldst(w)
    if op == 0x201:
        return ldst(u) if c == 1 else ldst(w)
    if op == 0x202:
        return ONE_BITS if c == -1 else ZERO_BITS
    return ONE_BITS if c in (0, 1) else ZERO_BITS


def run(pr, x):
    """the interpreter (3): 'ok', or where D3DX goes on with memory B does not have: 'range', 'stale', 'inputs',
    'noise' (5.1)"""
    if x.pc not in (24, 53, 64):
        return 'arg'
    try:
        run1(pr, x)
    except Range:
        return 'range'
    except Stale:
        return 'stale'
    except Inputs:
        return 'inputs'
    except Noise:
        return 'noise'
    return 'ok'


def run1(pr, x):
    p = x.pc
    slots = {}                                  # slot -> where it points, for the slots this run has written

    def inp(i, k):                              # input slot i at component k (3.3)
        if i >= 256:
            raise Range()                       # D3DX's frame, past its 256 pointers
        if i not in slots:
            raise Stale()
        return get(pr, x, slots[i], k)
    for op, n, sc, nin, opds, out in pr.ins:
        for k in range(nin):
            ref = resolve(pr, x, opds[k])
            if ref is None:                     # table 8: the slot keeps what it held
                continue
            if k >= 256:
                raise Inputs()
            slots[k] = ref
        dst = resolve(pr, x, [out])
        if sc:
            if 0x200 <= op <= 0x203:
                for k in range(n):
                    put(x, dst, k, select(op, inp(0, 0), inp(1, k)))
            elif op in (0x204, 0x205):
                for k in range(n):
                    w = inp(1, k)
                    u = inp(0, 0)
                    put(x, dst, k, to_d((add if op == 0x204 else mul)(ld(w), mem(u), p)))
            elif op == 0x206:
                for k in range(n):
                    u = inp(0, 0)
                    put(x, dst, k, atan2(u, inp(1, k)))
            elif op == 0x500:
                put(x, dst, 0, ZERO_BITS)
                for k in range(n):
                    w = inp(1, k)
                    prod = mul(ld(w), mem(inp(0, 0)), p)
                    put(x, dst, 0, to_d(add(prod, mem(get(pr, x, dst, 0)), p)))
            continue
        if op == 0x100:
            for k in range(n):
                put(x, dst, k, inp(0, k))
        elif op == 0x101:
            for k in range(n):
                put(x, dst, k, to_d(neg(ld(inp(0, k)))))
        elif op == 0x103:
            for k in range(n):
                put(x, dst, k, to_d(div(ONE, mem(inp(0, k)), p)))
        elif op == 0x104:
            for k in range(n):
                u = inp(0, k)
                v = ld(u)
                if v[0] == 'n':
                    put(x, dst, k, to_d(v))
                    continue
                f = ld(bits(C_FLOOR(dbl(u))))
                put(x, dst, k, to_d(add(neg(f), mem(inp(0, k)), p)))
        elif op == 0x106:
            for k in range(n):
                if cmp(ld(inp(k, 0)), ('z', 0)) == 0:
                    put(x, dst, k, ZERO_BITS)
                else:
                    put(x, dst, k, approx1(op, inp(0, k), p))
        elif op == 0x107:
            for k in range(n):
                put(x, dst, k, x87_rsq(inp(0, k), p))
        elif op in (0x105, 0x108, 0x109, 0x10A, 0x10B, 0x10C):
            for k in range(n):
                put(x, dst, k, approx1(op, inp(0, k), p))
        elif 0x200 <= op <= 0x203:
            for k in range(n):
                u = inp(0, k)
                put(x, dst, k, select(op, u, inp(1, k)))
        elif op in (0x204, 0x205):
            for k in range(n):
                u = inp(0, k)
                w = inp(1, k)
                put(x, dst, k, to_d((add if op == 0x204 else mul)(ld(u), mem(w), p)))
        elif op == 0x206:
            for k in range(n):
                u = inp(0, k)
                put(x, dst, k, atan2(u, inp(1, k)))
        elif op == 0x300:
            for k in range(n):
                c = cmp(ld(inp(0, k)), ('z', 0))
                put(x, dst, k, ldst(inp(1 if c in (0, 1) else 2, k)))
        elif op == 0x500:
            put(x, dst, 0, ZERO_BITS)
            for k in range(n):
                u = inp(0, k)
                prod = mul(ld(u), mem(inp(1, k)), p)
                put(x, dst, 0, to_d(add(prod, mem(get(pr, x, dst, 0)), p)))
        elif op == 0x502:
            for k in range(min(n, 3)):
                inp(0, k)
            raise Noise()
        elif op == 0x70E:
            h = nin // 2
            put(x, dst, 0, ZERO_BITS)
            for k in range(h):
                u = inp(k, 0)
                prod = mul(ld(u), mem(inp(h + k, 0)), p)
                put(x, dst, 0, to_d(add(prod, mem(get(pr, x, dst, 0)), p)))
        elif op == 0x711:
            for k in range(min(nin, 3)):
                inp(k, 0)
            raise Noise()


# ---- the effect path (4) ----
def smear(v):
    for s in (1, 2, 4, 8, 16):
        v |= v >> s
    return v


def evaluate(proto, inst, pr, t, pc):
    """the value dwords of a state of typedef t whose preshader is pr (4): gather, run, scatter"""
    blob = proto.fx.resources[pr.r][5]
    try:
        prog = load(blob)
    except Fail as e:
        return str(e), None
    if t.cls not in M.NUMERIC or t.type not in (M.INT, M.FLOAT) or t.count or t.rows > 4:
        return 'type', None                             # what A scatters (4.2): no BOOL, array or rows past 4
    fend = bend = iend = 0
    for c in pr.consts:
        if c.why == 6:
            continue
        end = c.reg + c.count
        if c.rset == 2:
            fend = max(fend, end)
        elif c.rset == 0:
            bend = max(bend, end)
        elif c.rset == 1:
            iend = max(iend, end)
    tot = bend + 4 * (fend + iend)
    cmask = smear(tot - 1) if tot else 0
    ctab = [0] * (cmask + 1)                            # D3DX zeroes mask + 1 doubles (2.5)
    for c in pr.consts:
        if c.why or c.sampler or c.rset != 2:
            continue
        for lid, reg, count in proto.leaves(c):
            n = proto.nodes[lid]
            data = inst.data(lid)
            rows, cols, ncl = n.rows, n.cols, n.cls
            for r in range(count):
                if reg + r >= fend:
                    break
                for cc in range(4):
                    if ncl == M.MATRIX_ROWS:            # D3DX gathers row r into register r (4.1)
                        if r >= rows or cc >= cols:
                            continue
                        at = r * cols + cc
                    elif c.layout == 'scalar':
                        if cc or r:
                            continue
                        at = 0
                    elif c.layout == 'vector':
                        if cc >= cols:
                            continue
                        at = r * cols + cc
                    elif c.layout == 'rows':
                        if cc >= cols or r >= rows:
                            continue
                        at = cc * rows + r if ncl == M.MATRIX_COLUMNS else r * cols + cc
                    else:
                        if cc >= rows or r >= cols:
                            continue
                        at = r * rows + cc if ncl == M.MATRIX_COLUMNS else cc * cols + r
                    if at < len(data):
                        ctab[4 * (reg + r) + cc] = f2d(data[at]) if n.type == M.FLOAT else i2d(data[at])
    nreg = (t.cols if t.rows else 0) or 1               # one register per column
    if nreg > 0x10000:
        return 'range', None
    omask = smear(4 * nreg - 1)
    x = Tables(ctab, cmask, [0] * (omask + 1), omask, pc=pc, temps_unknown=True)
    res = run(prog, x)
    if res != 'ok':
        return res, None
    out = []
    for r in range(t.cols):
        for cc in range(t.rows):
            at = 4 * r + cc
            v = x.o[at] if at < len(x.o) else 0
            out.append(d2f(v) if t.type == M.FLOAT else ftol(v))
    return 'ok', out


LIGHT_OFF = (0, 1, 5, 9, 13, 16, 19, 20, 21, 22, 23, 24, 25)


def key(st, stage):
    op, index = st[0], st[1]
    if op >= len(M.STATE_ROWS):
        return 'other ?'
    cls, d3d = M.STATE_ROWS[op]
    name = M.SCLASSES[cls]
    if name == 'RENDER':
        return 'rs %d' % d3d
    if name == 'STAGE':
        return 'tss %d %d' % (index, d3d)
    if name == 'SAMPLERSTATE':
        return 'ss %d %d' % (stage, d3d)
    if name == 'TRANSFORM':
        return 'transform %d 0' % (d3d + index)
    if name == 'MATERIAL':
        return 'material %d' % (4 * d3d if d3d < 4 else 16)
    if name == 'LIGHT':
        return 'lightenable %d' % index if d3d == 13 else 'light %d %d' % (index, LIGHT_OFF[d3d] if d3d < 13 else 0)
    return 'other ' + name


def state_line(lines, proto, inst, st, k, rkey, stage, pc, lead):
    rs = proto.res_on.get(rkey, []) if rkey else []
    if not rs:
        return
    pr = proto.progs.get(rs[-1])
    if pr is None or pr.kind != 'preshader':
        return
    op, index, td = st[0], st[1], st[2]
    name = F.STATES[op] if op < len(F.STATES) else '?'
    res, vals = evaluate(proto, inst, pr, proto.fx.typedef(td)[0], pc)
    s = '%sstate %d %s[%d] -> %s = ' % (lead, k, name, index, key(st, stage))
    lines.append(s + ('error ' + res if res != 'ok' else ' '.join('%08x' % v for v in vals)))


def pass_lines(lines, proto, inst, ti, pi, pc):
    states = proto.fx.techs[ti][2][pi][2]
    for k, st in enumerate(states):
        state_line(lines, proto, inst, st, k, ('pass', ti, pi, k), 0, pc, '    ')
    for k, st in enumerate(states):
        rs = proto.res_on.get(('pass', ti, pi, k), [])
        if not rs:
            continue
        pr = proto.progs.get(rs[-1])
        op = st[0]
        if pr is None or pr.kind != 'shader' or not pr.applied or op >= len(M.STATE_ROWS) or \
                M.SCLASSES[M.STATE_ROWS[op][0]] not in ('VSHADER', 'PSHADER'):
            continue
        for c in pr.consts:
            if c.why or not c.sampler:
                continue
            for j, leaf in enumerate(proto.sampler_leaves(c.id, c.used)):
                for b, bst in enumerate(leaf.block):
                    lead = '    sampler %s stage %d ' % (leaf.path.decode('latin-1'), c.stage + j)
                    state_line(lines, proto, inst, bst, b, leaf.skey + (b,) if leaf.skey else None, c.stage + j,
                               pc, lead)


# ---- the canonical texts ----
def fields(path):
    with open(path) as f:
        for line in f:
            w = line.split()
            if w:
                yield w


def main_vm(inp, outp):
    out = ['fxpres 1']
    it = fields(inp)
    if next(it) != ['fxpcase', '1']:
        sys.exit('fxpres: %s is not an fxpcase 1 file' % inp)
    n = 0
    for w in it:
        if w[0] != 'case':
            sys.exit('fxpres: %s where case was expected' % w[0])
        name, pc, blob, tabs, unknown = w[1], 0, b'', {}, False
        for w in it:
            if w[0] == 'end':
                break
            if w[0] == 'pc':
                pc = int(w[1])
            elif w[0] == 'temps':
                unknown = w[1:] == ['unknown']
            elif w[0] == 'blob':
                nb = int(w[1])
                blob = b''.join(struct.pack('<I', int(x, 16)) for x in w[2:])[:nb]
            elif w[0] in ('c', 'o', 'v'):
                tabs[w[0]] = ([int(x, 16) for x in w[4:4 + int(w[1])]], int(w[3]))
        n += 1
        try:
            prog = load(blob)
        except Fail as e:
            r = str(e)
            out.append('case %s load %s %s' % (name, 'fail' if r in D3DX_REFUSES else 'unsupported', r))
            continue
        c, cm = tabs.get('c', ([], 0))
        o, om = tabs.get('o', ([], 0))
        v, vm = tabs.get('v', (None, 0))
        x = Tables(list(c), cm, list(o), om, list(v) if v is not None else None, vm, pc, unknown)
        res = run(prog, x)
        out.append('case %s load ok' % name)
        out.append('  class %s' % ('approx' if prog.approx else 'exact'))
        out.append('  run %s' % res)
        out.append('  o' + ''.join(' %016x' % y for y in x.o))
        rr = ''.join(' %d=%016x' % (i, y) for i, y in enumerate(x.r) if y)
        out.append('  r' + (rr or ' -'))
    out.append('end')
    text = '\n'.join(out) + '\n'
    if outp == '-':
        sys.stdout.write(text)
    else:
        with open(outp, 'w') as f:
            f.write(text)
    print('fxpres vm: %d cases' % n, file=sys.stderr)


def main_effect(inp, outp, files):
    protos = []
    for path in files:
        with open(path, 'rb') as f:
            data = f.read()
        name = os.path.basename(path)
        if name.endswith('.fxo'):
            name = name[:-4]
        protos.append((os.fsencode(name), name, data))
    protos.sort()
    protos = [M.Proto(name, data) for _, name, data in protos]
    insts, pool = M.build(protos)
    lines = ['fxpeff 1']
    it = fields(inp)
    if next(it) != ['fxpround', '1']:
        sys.exit('fxpres: %s is not an fxpround 1 file' % inp)
    pc, rnd, nround = 24, None, 0
    for w in it:
        if w[0] == 'set':
            p = next((i for i, q in enumerate(protos) if q.name == w[1]), None)
            if p is None:
                sys.exit('fxpres: no effect %s' % w[1])
            nid = protos[p].by_name(0, w[2].encode('latin-1'))
            ws = [int(x, 16) for x in w[4:4 + int(w[3])]]
            hr = insts[p].set_value(nid, ws, 4 * len(ws))[0]
            if hr:
                sys.exit('fxpres: SetValue %s.%s: 0x%08x' % (w[1], w[2], hr))
            continue
        if w[0] == 'call':                          # call <Setter> <NAME> <path> <count> <n> <dwords>
            setter, count, n = w[1], int(w[4]), int(w[5])
            p = next((i for i, q in enumerate(protos) if q.name == w[2]), None)
            if p is None:
                sys.exit('fxpres: no effect %s' % w[2])
            inst, nid = insts[p], protos[p].by_name(0, w[3].encode('latin-1'))
            ws = [int(x, 16) for x in w[6:6 + n]]
            inst.set_x87_pc(pc)                         # the round's precision (fx-model 2.5)
            arrays = {'SetBoolArray': M.BOOL, 'SetIntArray': M.INT, 'SetFloatArray': M.FLOAT}
            if setter == 'SetBool':
                hr = inst.set_bool(nid, ws[0])[0]
            elif setter == 'SetInt':
                hr = inst.set_int(nid, ws[0])[0]
            elif setter == 'SetFloat':
                hr = inst.set_float(nid, ws[0])[0]
            elif setter in arrays:
                hr = inst.set_array(nid, arrays[setter], ws, count)[0]
            elif setter == 'SetVector':
                hr = inst.set_vector(nid, ws[:4])[0]
            elif setter == 'SetVectorArray':
                hr = inst.set_vector_array(nid, [ws[4 * e:4 * e + 4] for e in range(count)], count)[0]
            elif setter in ('SetMatrix', 'SetMatrixTranspose'):
                hr = inst.set_matrix(nid, ws[:16], setter == 'SetMatrixTranspose')[0]
            elif setter in ('SetMatrixArray', 'SetMatrixTransposeArray'):
                hr = inst.set_matrix_array(nid, [ws[16 * e:16 * e + 16] for e in range(count)], count,
                                           setter == 'SetMatrixTransposeArray')[0]
            else:
                sys.exit('fxpres: no setter %s' % setter)
            lines.append('call %s %s %s = %08x' % (setter, w[2], w[3], hr))
            continue
        if w[0] in ('round', 'end'):
            if rnd is not None:
                lines.append('round %s pc %d' % (rnd, pc))
                for proto, inst in zip(protos, insts):
                    lines.append('effect %s' % proto.name)
                    for ti, (_, _, passes) in enumerate(proto.fx.techs):
                        for pi in range(len(passes)):
                            lines.append('  tech %d pass %d' % (ti, pi))
                            pass_lines(lines, proto, inst, ti, pi, pc)
                nround += 1
            rnd = w[1] if w[0] == 'round' else None
            if w[0] == 'end':
                break
            if w[2:3] != ['pc']:
                sys.exit('fxpres: a round without its pc')
            pc = int(w[3])
            continue
        sys.exit('fxpres: unknown line %s' % w[0])
    lines.append('end')
    text = '\n'.join(lines) + '\n'
    if outp == '-':
        sys.stdout.write(text)
    else:
        with open(outp, 'w') as f:
            f.write(text)
    print('fxpres effect: %d rounds' % nround, file=sys.stderr)


# ---- the self-test: the number model against exact arithmetic written out by hand (3.1) ----
def self_test():
    fails, checks = [], 0

    def check(cond, what):
        nonlocal checks
        checks += 1
        if not cond:
            fails.append(what)
    for p, want in ((24, 0x3FD5555560000000), (53, 0x3FD5555555555555), (64, 0x3FD5555555555555)):
        check(to_d(div(ONE, ld(bits(3.0)), p)) == want, '1/3 at %d' % p)
    check(x87_rsq(bits(7.0), 64) == 0x3FD83091E6A7F7E7, '1/sqrt(7) at 64: double rounding')
    check(x87_rsq(bits(7.0), 53) == 0x3FD83091E6A7F7E6, '1/sqrt(7) at 53')
    check(to_d(add(ld(bits(1.0)), mem(bits(-1.0)), 64)) == 0, '1 - 1 = +0')
    check(to_d(add(ld(1 << 63), mem(1 << 63), 64)) == 1 << 63, '-0 + -0 = -0')
    check(to_d(add(ld(0x7FF0000000000000), mem(0xFFF0000000000000), 64)) == 0xFFF8000000000000, 'inf - inf')
    check(to_d(mul(ld(0), mem(0x7FF0000000000000), 24)) == 0xFFF8000000000000, '0 * inf')
    check(to_d(add(ld(0x7FF8000000000001), mem(0x7FF0000000000002), 64)) == 0x7FF8000000000001,
          'quiet NaN register, signalling NaN memory: the quiet one')
    check(to_d(add(ld(0x7FF8000000000001), mem(0x7FF8000000000002), 64)) == 0x7FF8000000000002,
          'two quiet NaNs: the larger significand')
    check(to_d(add(ld(0xFFF8000000000002), mem(0x7FF8000000000002), 64)) == 0x7FF8000000000002,
          'two quiet NaNs, same significand: the positive one')
    check(to_d(add(ld(bits(1.0)), mem(1), 64)) == bits(1.0), '1 + the smallest subnormal')
    check(to_d(mul(ld(0x0010000000000000), mem(bits(0.5)), 64)) == 0x0008000000000000, 'a product turns subnormal')
    check(to_d(mul(ld(0x0010000000000001), mem(bits(0.5)), 64)) == 0x0008000000000000, 'subnormal: ties to even')
    check(to_d(mul(ld(0x0010000000000003), mem(bits(0.5)), 64)) == 0x0008000000000002, 'subnormal: ties to even up')
    check(to_d(mul(ld(0x7FEFFFFFFFFFFFFF), mem(bits(2.0)), 64)) == 0x7FF0000000000000, 'overflow on the store')
    for v, want in ((2.5, 2), (-2.5, 0xFFFFFFFE), (0.7, 0), (-0.7, 0), (2.0 ** 32 + 5, 5), (2.0 ** 63, 0),
                    (float('nan'), 0), (-2.0 ** 31, 0x80000000)):
        check(ftol(bits(v)) == want, 'ftol %r' % v)
    check(d2f(bits(1.0000001192092896)) == 0x3F800001, 'd2f')
    check(d2f(0x7FF0000000000001) == 0x7FC00000, 'd2f of a signalling NaN: quiet')
    check(d2f(bits(1e-46)) == 0, 'd2f below the smallest subnormal')
    check(d2f(bits(1.401298464324817e-45)) == 1, 'd2f: the smallest subnormal')
    check(f2d(0x7F800001) == 0x7FF8000020000000, 'f2d of a signalling NaN: quiet')
    check(f2d(1) == bits(1.401298464324817e-45), 'f2d of a subnormal')
    check(i2d(0x80000000) == bits(-2.0 ** 31) and i2d(5) == bits(5.0), 'i2d')

    # programs: which operand an opcode loads (3.2), noise's reads, unknown temporaries (5.1); worked out by hand
    def prog(*ins_):
        """a preshader of the instructions (op, n, scalar, [(table, offset) inputs], (table, offset) output)"""
        f = [FXLC, len(ins_)]
        for op, n, sc, srcs, out in ins_:
            f += [(op << 20) | n | (sc << 31), len(srcs)]
            for t, o in srcs:
                f += [0, t, o]
            f += [0, out[0], out[1]]
        w = [0x46580200, 0xFFFE | (2 << 16), CLIT, 0, 0xFFFE | (len(f) << 16)] + f + [0xFFFF]
        return load(struct.pack('<%dI' % len(w), *w))

    def run_c(pr, c, unknown=False):
        x = Tables(c + [0] * (64 - len(c)), 63, [0] * 16, 15, pc=64, temps_unknown=unknown)
        return run(pr, x), x.o
    q5, s9 = 0x7FF8000000000005, 0x7FF0000000000009
    c = [0] * 8 + [q5, s9]
    for name, op, sc, a, b, want in (('add', 0x204, 0, 8, 9, q5), ('mul', 0x205, 0, 8, 9, q5),
                                     ('dot', 0x500, 0, 8, 9, q5), ('add.s', 0x204, 1, 8, 9, 0x7FF8000000000009),
                                     ('dot.s', 0x500, 1, 8, 9, 0x7FF8000000000009), ('add.s', 0x204, 1, 9, 8, q5),
                                     ('dot.s', 0x500, 1, 9, 8, q5), ('dotswiz', 0x70E, 0, 8, 9, q5),
                                     ('dotswiz', 0x70E, 0, 9, 8, 0x7FF8000000000009)):
        res, o = run_c(prog((op, 1, sc, [(2, a), (2, b)], (4, 0))), c)
        check(res == 'ok' and o[0] == want, '%s of c[%d], c[%d]: %s %016x' % (name, a, b, res, o[0]))
    c = [bits(float(i)) for i in range(64)]
    check(run_c(prog((0x502, 4, 0, [(2, 61)], (4, 0))), c)[0] == 'noise', 'noise of 4 components reads 3')
    check(run_c(prog((0x711, 1, 0, [(2, 1), (2, 2), (2, 3), (9, 0)], (4, 0))), c)[0] == 'noise', '0x711 reads 3 inputs')
    check(run_c(prog((0x711, 1, 0, [(2, 1), (2, 2), (9, 0)], (4, 0))), c)[0] == 'range', '0x711: the 3rd input')
    check(run_c(prog((0x100, 1, 0, [(7, 5)], (4, 0))), c, True)[0] == 'stale', 'an unknown temporary read')
    check(run_c(prog((0x100, 1, 0, [(7, 5)], (4, 0))), c)[0] == 'ok', 'a known temporary read')
    res, o = run_c(prog((0x100, 2, 0, [(2, 1)], (7, 5)), (0x100, 2, 0, [(7, 5)], (4, 0))), c, True)
    check(res == 'ok' and o[1] == bits(2.0), 'temporaries written, then read')
    res, o = run_c(prog((0x100, 1, 0, [(2, 1)], (7, 5)), (0x100, 2, 0, [(7, 5)], (4, 0))), c, True)
    check(res == 'stale' and o[0] == bits(1.0), 'component 1 of a temporary not written')
    print('fxpres --self-test: %d checks, %d failed' % (checks, len(fails)))
    for f in fails:
        print('  FAIL ' + f)
    return 1 if fails else 0


def main(argv):
    if argv[:1] == ['--self-test']:
        return self_test()
    if len(argv) == 3 and argv[0] == 'vm':
        return main_vm(argv[1], argv[2])
    if len(argv) >= 4 and argv[0] == 'effect':
        return main_effect(argv[1], argv[2], argv[3:])
    sys.exit(__doc__)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]) or 0)
