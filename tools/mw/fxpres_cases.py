#!/usr/bin/env python3
"""The cases of the preshader evaluator checks (docs/mw/fx-preshader.md 6): one input that implementation A
(tools/mw/fxpresrun.c), implementation B (tools/fxpres.py) and the oracle (tools/mw/fxoracle.c) each run.

  tools/mw/fxpres_cases.py vm <indir> <out.txt> [--seed S] [--mw K] [--synth N] [--random R] [--corners C]
      'fxpcase 1' (6.2): every preshader of <indir>/*.fxo with K input tables at each precision (24, 53, 64), as the
      effect runs it, the synthetic programs (N per opcode and form: every opcode of the interpreter, scalar and
      not, 0..5 components, every operand kind; then signalling NaNs anywhere and only in memory operands), the
      targeted ones (log's zero test, the slots D3DX reads through and the ones it does not, tables 8 and 9,
      temporaries past 255 and unknown ones, noise), R random programs, C operands of each kind of rounding corner
      (rcp, rsq, add, mul) and the loader cases (every check of 2.2, the comment walk of both versions)
  tools/mw/fxpres_cases.py mutate <indir> <out.txt> [--seed S] [--copies N]
      'fxpcase 1': N corrupted copies of every preshader, with random tables (6.5)
  tools/mw/fxpres_cases.py rounds <indir> <out.txt> [--seed S] [--rounds R] [--pc 24,53,64]
      'fxpround 1' (6.3): R rounds per precision of SetValue on every parameter a preshader reads, in every effect,
      and in every other round the typed setters MW uses on them
  tools/mw/fxpres_cases.py patch <indir> <outdir> op <opcode hex> [--n1] | pad
      copies of the effects (6.3) whose preshaders compute something: op: every mov (not scalar; --n1: of one
      component) made that unary opcode, so D3DX's own effect path converts rounded and inexact values (review
      R2-02); pad: in every preshader whose input table is longer than its float registers, the first mov from c
      reads the first double past them, which D3DX zeroes (or c[0] where there is no constant: one double) (R2-01)

The values come from a fixed pool of edge cases (signed zeros, infinities, quiet and signalling NaNs with payloads,
subnormals, the limits of float, int and double, 2^24, 2^31, 2^53, 2^63) and random ones, from one seed, so the
files are the same on every run.  The effects are the game's data: the output stays under out/ (ignored).
"""
import importlib.util, os, random, struct, sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
_spec = importlib.util.spec_from_file_location('fxmodel', os.path.join(ROOT, 'tools', 'fxmodel.py'))
M = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(M)

FXLC, CLIT, END = 0x434C5846, 0x54494C43, 0x0000FFFF


def d2w(x):
    return struct.unpack('<Q', struct.pack('<d', x))[0]


def smear(v):
    for s in (1, 2, 4, 8, 16):
        v |= v >> s
    return v


SNAN = [0x7FF0000000000001, 0xFFF4000000000000, 0x7FF0000DEADBEEF0, 0x7FF4000000000000]
EDGE = [0, 1 << 63, d2w(1.0), d2w(-1.0), d2w(0.5), d2w(-0.5), d2w(2.0), d2w(3.0), d2w(-7.0), d2w(1 / 3), d2w(0.1),
        0x7FF0000000000000, 0xFFF0000000000000,                   # infinities
        0x7FF8000000000000, 0xFFF8000000000000, 0x7FF8000000000123, 0xFFFC000000ABCDEF,     # quiet NaNs
        1, 0x800FFFFFFFFFFFFF, 0x000FFFFFFFFFFFFF, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF,   # subnormals, limits
        d2w(2.0 ** 24), d2w(2.0 ** 24 + 1), d2w(2.0 ** 31), d2w(-2.0 ** 31), d2w(2.0 ** 32 - 1), d2w(2.0 ** 53 + 2),
        d2w(2.0 ** 63), d2w(-2.0 ** 63), d2w(2.0 ** 63 * 0.75), d2w(1e300), d2w(-1e-300),
        d2w(3.4028234663852886e38), d2w(1.401298464324817e-45), d2w(1.1754943508222875e-38), d2w(0.999999999),
        d2w(1.0000000000000002), d2w(4.5), d2w(-2.5), d2w(1.5), d2w(1e-10), d2w(123456789.123)]


def quiet(b):
    """a signalling NaN made quiet, anything else as it is"""
    return b | (1 << 51) if (b >> 52) & 0x7FF == 0x7FF and b & 0xFFFFFFFFFFFFF and not b & (1 << 51) else b


class Vals:
    """doubles for the interpreter's tables; signalling NaNs only with snan (6.2: Unicorn keeps them signalling)"""
    def __init__(self, rng, snan=False):
        self.r, self.snan = rng, snan

    def d(self, kind='any'):
        r = self.r
        if self.snan and r.random() < 0.15:
            return r.choice(SNAN)
        if kind == 'unit':                                  # mostly inside [-1, 1] for asin and acos
            return d2w(r.uniform(-1, 1)) if r.random() < 0.8 else r.choice(EDGE)
        x = r.random()
        if x < 0.3:
            return r.choice(EDGE)
        if x < 0.45:
            return d2w(float(r.randint(-9, 9)))
        if x < 0.85:                                        # moderate exponents, random bits
            return (r.getrandbits(1) << 63) | ((1023 + r.randint(-40, 40)) << 52) | r.getrandbits(52)
        v = r.getrandbits(64)                               # anything, quiet NaNs included
        if (v >> 52) & 0x7FF == 0x7FF and v & 0xFFFFFFFFFFFFF and not self.snan:
            v |= 1 << 51
        return v

    def word(self, typ):
        """a parameter dword for SetValue: INT, BOOL or FLOAT"""
        r = self.r
        if typ == M.FLOAT:
            x = r.random()
            if x < 0.3:
                return r.choice([0, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000,
                                 0xFFC00001, 0x7F800001, 0xFFA00000, 1, 0x807FFFFF, 0x00800000, 0x7F7FFFFF,
                                 0x4B800001, 0x3EAAAAAB, 0x40000000, 0x00000003])
            if x < 0.5:
                return struct.unpack('<I', struct.pack('<f', float(r.randint(-16, 16))))[0]
            if x < 0.9:
                return (r.getrandbits(1) << 31) | ((127 + r.randint(-20, 20)) << 23) | r.getrandbits(23)
            return r.getrandbits(32)
        x = r.random()
        if x < 0.4:
            return r.randint(0, 15)
        if x < 0.7:
            return r.choice([0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x01000001, 0x00FFFFFF, 0xFFFF0000, 0x80000001])
        return r.getrandbits(32)


# ---- programs ----

def operand(pairs):
    """an input: [index pairs] then the register pair; pairs = [(table, offset), ...]"""
    w = [len(pairs) - 1]
    for t, o in pairs:
        w += [t, o]
    return w


def ins(op, ncomp, ins_, out, scalar=False, nin=None):
    w = [(op << 20) | ncomp | (0x80000000 if scalar else 0), len(ins_) if nin is None else nin]
    for p in ins_:
        w += operand(p)
    return w + [0, out[0], out[1]]


def blob(lits, prog, ctab=True, clit=True, fxlc=True, trailer=True, version=0x46580200, end=True, lead=()):
    w = [version] + list(lead)
    if ctab:                                                # an empty constant table, as fxc writes one
        c = [0x42415443, 28, 0, 0x46580200, 0, 28, 0x100, 0]
        w += [0xFFFE | (len(c) << 16)] + c
    if clit:
        c = [CLIT, len(lits)]
        for v in lits:
            c += [v & 0xFFFFFFFF, v >> 32]
        w += [0xFFFE | (len(c) << 16)] + c
    if fxlc:
        c = [FXLC, len(prog)]
        for p in prog:
            c += p
        if trailer:
            c += [0xF0F0F0F0, 0x0F0F0F0F]
        w += [0xFFFE | (len(c) << 16)] + c
    if end:
        w.append(END)
    return w


class Out:
    def __init__(self, f):
        self.f, self.n = f, 0

    def case(self, name, pc, words, c=None, o=None, v=None, nbytes=None, run=1, unknown=False):
        f = self.f
        f.write('case %s\npc %d\n' % (name, pc))
        if not run:
            f.write('run 0\n')
        if unknown:                                         # the temporaries hold D3DX's stack, as in an effect
            f.write('temps unknown\n')
        nb = 4 * len(words) if nbytes is None else nbytes
        f.write('blob %d %s\n' % (nb, ' '.join('%08x' % (x & 0xFFFFFFFF) for x in words[:(nb + 3) // 4])))
        for k, t in (('c', c), ('o', o), ('v', v)):
            if t is not None:
                vals, mask = t
                f.write('%s %d mask %d %s\n' % (k, len(vals), mask, ' '.join('%016x' % x for x in vals)))
        f.write('end\n')
        self.n += 1


# The interpreter's opcodes (2.3, 3.2): name, inputs; 0x502 and 0x711 are D3DX's noise (A refuses them, 5).
OPS = [(0x000, 0), (0x100, 1), (0x101, 1), (0x102, 1), (0x103, 1), (0x104, 1), (0x105, 1), (0x106, 1), (0x107, 1),
       (0x108, 1), (0x109, 1), (0x10A, 1), (0x10B, 1), (0x10C, 1), (0x200, 2), (0x201, 2), (0x202, 2), (0x203, 2),
       (0x204, 2), (0x205, 2), (0x206, 2), (0x207, 2), (0x208, 2), (0x300, 3), (0x301, 3), (0x500, 2), (0x502, 1),
       (0x70E, 6), (0x70E, 8), (0x70E, 5), (0x711, 3), (0x7FF, 1), (0x400, 2)]


def synth(out, rng, per, vals, tag='syn'):
    """every opcode, scalar and not, 0..5 components, at each precision, its inputs from c, literals, temporaries,
    the output table and v, directly and through an index register; a dotswiz with 8 inputs first, so that the
    input slots an opcode reads past its own inputs are set (3.3)"""
    k = 0
    for op, nin in OPS:
        for scalar in (False, True):
            for ncomp in (0, 1, 2, 3, 4, 5):
                for rep in range(per):
                    pc = (24, 53, 64)[(k + rep) % 3]
                    unit = op in (0x10A, 0x10B)
                    c = [vals.d('unit' if unit else 'any') for _ in range(64)]
                    for j in range(48, 56):                 # the dotswiz before: the x87 on it is not under test
                        c[j] = quiet(c[j])
                    lits = [vals.d('unit' if unit else 'any') for _ in range(rng.randint(1, 6))]
                    v = [vals.d() for _ in range(16)]
                    o0 = [vals.d() for _ in range(32)]
                    prog = [ins(0x70E, 1, [[(2, 48 + j)] for j in range(8)], (7, 30))]
                    # temporaries 8.. from c, so a temporary input reads something set
                    prog.append(ins(0x100, 8, [[(2, 40)]], (7, 8)))
                    srcs = []
                    for j in range(nin):
                        kind = rng.randrange(8)
                        if kind == 0:
                            srcs.append([(1, rng.randrange(len(lits) * 2))])           # literal, may wrap
                        elif kind == 1:
                            srcs.append([(7, 8 + rng.randrange(4))])
                        elif kind == 2:
                            srcs.append([(4, rng.randrange(16))])                     # the output table as input
                        elif kind == 3:
                            srcs.append([(3, rng.randrange(8))])                      # v
                        elif kind == 4:                                               # c through an index register
                            srcs.append([(rng.choice((1, 2, 7)), rng.randrange(4)), (2, rng.randrange(8))])
                        else:
                            srcs.append([(2, rng.randrange(32) if rng.random() < 0.8 else 64 + rng.randrange(16))])
                    dst = rng.choice([(4, rng.randrange(16)), (7, 20 + rng.randrange(8)), (5, rng.randrange(8)),
                                      (6, rng.randrange(8))])
                    prog.append(ins(op, ncomp, srcs, dst, scalar))
                    prog.append(ins(0x100, 4, [[(7, 20)]], (4, 24)))                # the temporaries it may have set
                    out.case('%s_%03x_%d_%s%d_%d' % (tag, op, nin, 's' if scalar else 'v', ncomp, rep), pc,
                             blob(lits, prog), (c, 63), (o0, 31), (v, 15))
                    k += 1
    if tag != 'syn':
        return
    # opcodes given fewer inputs than they read: the slots of the instruction before (3.3)
    for rep in range(per * 4):
        pc = (24, 53, 64)[rep % 3]
        c = [vals.d() for _ in range(32)]
        prog = [ins(0x70E, 1, [[(2, 8 + j)] for j in range(8)], (7, 30))]
        for op, nin in ((0x204, 1), (0x205, 0), (0x300, 1), (0x200, 1), (0x100, 0), (0x101, 0), (0x106, 1),
                        (0x500, 1), (0x70E, 2), (0x103, 0)):
            prog.append(ins(op, rng.randint(1, 4), [[(2, rng.randrange(8))] for _ in range(nin)],
                            (7, 40 + 4 * len(prog))))
        prog.append(ins(0x100, 16, [[(7, 40)]], (4, 0)))
        prog.append(ins(0x100, 16, [[(7, 56)]], (4, 16)))
        out.case('slots_%d' % rep, pc, blob([], prog), (c, 31), ([0] * 32, 31))
    # index chains, wraps and aliasing
    for rep in range(per * 8):
        pc = (24, 53, 64)[rep % 3]
        c = [vals.d() for _ in range(32)]
        # index registers holding small integers, halves, negatives, NaN, huge values
        idxv = [d2w(x) for x in (0, 1, 2, 3, -1, 2.9, -0.5, 7.5, 1e10, 2.0 ** 40 + 3)] + [0x7FF8000000000000]
        for j in range(8):
            c[24 + j] = rng.choice(idxv)
        lits = [rng.choice(idxv) for _ in range(rng.randint(1, 4))] + [vals.d() for _ in range(3)]
        prog = [ins(0x100, 4, [[(2, 24)]], (7, 0)), ins(0x100, 4, [[(2, 28)]], (4, 0))]
        for _ in range(rng.randint(1, 5)):
            op, nin = rng.choice([(0x100, 1), (0x101, 1), (0x204, 2), (0x205, 2), (0x200, 2), (0x300, 3), (0x103, 1),
                                  (0x107, 1), (0x500, 2), (0x70E, 6), (0x202, 2), (0x104, 1)])
            srcs = []
            for j in range(nin):
                chain = [(rng.choice((0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 1000)), rng.randrange(40))
                         for _ in range(rng.choice((0, 0, 1, 1, 2, 3)))]
                last = rng.choice([(2, rng.randrange(40)), (1, rng.randrange(12)), (7, rng.randrange(300)),
                                   (4, rng.randrange(20)), (5, rng.randrange(20))])
                srcs.append(chain + [last])
            dst = rng.choice([(7, rng.randrange(300)), (4, rng.randrange(40)), (6, rng.randrange(40))])
            prog.append(ins(op, rng.randint(1, 4), srcs, dst, rng.random() < 0.2))
        prog.append(ins(0x100, 4, [[(7, rng.randrange(8))]], (4, 12)))
        out.case('idx_%d' % rep, pc, blob(lits, prog), (c, rng.choice((31, 15, 0xFFFF))), ([0] * 16, 15),
                 ([vals.d() for _ in range(8)], 7))
    # aliasing: overlapping temporaries, a dot that writes its own input, mov forward
    for rep in range(per * 2):
        pc = (24, 53, 64)[rep % 3]
        c = [vals.d() for _ in range(16)]
        prog = [ins(0x100, 8, [[(2, 0)]], (7, 0)),
                ins(0x100, 6, [[(7, 0)]], (7, 1)),                   # forward copy into itself
                ins(0x500, 3, [[(7, 2)], [(7, 4)]], (7, 3)),          # dot writes 0 into its own input first
                ins(0x70E, 1, [[(7, 0)], [(7, 1)], [(7, 6)], [(7, 6)], [(7, 7)], [(7, 0)]], (7, 7)),
                ins(0x204, 4, [[(7, 0)], [(7, 1)]], (7, 1)),
                ins(0x205, 3, [[(7, 2)], [(7, 0)]], (7, 2), True),
                ins(0x100, 8, [[(7, 0)]], (4, 0))]
        out.case('alias_%d' % rep, pc, blob([], prog), (c, 15), ([0] * 8, 7))


def snan_mem(out, rng, per):
    """signalling NaNs only where an arithmetic opcode takes its memory operand (review R2-04): there Unicorn and the
    x87 agree (a quiet register NaN wins over a signalling memory one, 5.2), so the operand roles of 3.2 are
    compared exactly.  The register operand (fld) is a number or a quiet NaN: input 0 of add, mul and dot, input 1
    (b[j]) of their scalar forms, the first half of dotswiz's inputs; the memory operand is the other one.  Every
    other case has only quiet register NaNs with small payloads against signalling memory NaNs with larger ones,
    where loading the wrong operand gives another NaN in every form (dot's sum included)"""
    def qnan():
        return rng.choice((0, 1 << 63)) | 0x7FF8000000000000 | rng.choice((1, 5, 0x10, rng.getrandbits(50)))

    def snan():
        return rng.choice((0, 1 << 63)) | 0x7FF0000000000000 | rng.choice((2, 9, 0x40, rng.getrandbits(50) | 1))

    def reg0():
        x = rng.random()
        return qnan() if x < 0.6 else d2w(rng.choice((1.0, -2.5, 0.0, 3.0))) if x < 0.9 else 0x7FF0000000000000

    def mem0():
        x = rng.random()
        return snan() if x < 0.7 else qnan() if x < 0.85 else d2w(rng.choice((1.5, -0.0, 7.0)))
    def reg_q():
        return rng.choice((0, 1 << 63)) | 0x7FF8000000000000 | rng.choice((1, 5, 0x10))

    def mem_s():
        return rng.choice((0, 1 << 63)) | 0x7FF0000000000000 | rng.choice((0x40, 0x123, (1 << 50) | rng.getrandbits(40)))
    k = 0
    for op, nin, sc in ((0x204, 2, False), (0x205, 2, False), (0x500, 2, False), (0x204, 2, True), (0x205, 2, True),
                        (0x500, 2, True), (0x70E, 6, False), (0x70E, 8, False)):
        for n in (1, 2, 3, 4):
            for rep in range(per * 2):
                pc = (24, 53, 64)[k % 3]
                k += 1
                reg, memv = (reg_q, mem_s) if rep % 2 else (reg0, mem0)
                c = [0] * 32
                if op == 0x70E:                             # input i at c[4i], component 0 only
                    for i in range(nin):
                        c[4 * i] = reg() if i < nin // 2 else memv()
                    srcs = [[(2, 4 * i)] for i in range(nin)]
                elif sc:                                    # fld b[j]; fop a[0]
                    c[0] = memv()
                    for j in range(4):
                        c[8 + j] = reg()
                    srcs = [[(2, 0)], [(2, 8)]]
                else:                                       # fld a[j]; fop b[j]
                    for j in range(4):
                        c[j], c[8 + j] = reg(), memv()
                    srcs = [[(2, 0)], [(2, 8)]]
                out.case('snanm_%03x_%d_%s%d_%d' % (op, nin, 's' if sc else 'v', n, rep), pc,
                         blob([], [ins(op, n, srcs, (4, 0), sc)]), (c, 31), ([0] * 8, 7))


def targeted(out):
    """the cases the generated ones do not pin down (review R1-06, R1-11): log's zero test reads slot j's
    component 0 (3.2); the reads D3DX makes through its slots, and only those (3.3); tables 8 and 9 (2.4); the
    temporaries past 255 (2.4)"""
    def cvals(m, n=32):
        c = [0] * n
        for k, v in m.items():
            c[k] = d2w(v)
        return c
    # log with one input and 4 components after a dotswiz sets slots 0..3 from c[8], c[12], c[16], c[20]
    for i, cv in enumerate(({0: 2.0, 1: 4.0, 2: 8.0, 3: 16.0, 8: 5.0, 12: 0.0, 13: 5.0, 16: 7.0, 18: 0.0, 20: 0.0, 23: 9.0},
                            {0: 2.0, 1: 0.0, 2: 8.0, 3: -0.0, 8: 5.0, 12: 3.0, 13: 0.0, 16: 7.0, 18: 0.0, 20: 6.0},
                            {0: 0.0, 1: 4.0, 2: 8.0, 3: 16.0, 8: 0.0, 12: 1.0, 16: 1.0, 20: 1.0},
                            {0: 2.0, 1: 4.0, 2: 8.0, 3: 16.0, 8: 5.0, 12: -0.0, 16: 0.5, 20: -0.0})):
        for pc in (24, 53, 64):
            prog = [ins(0x70E, 1, [[(2, 8)], [(2, 12)], [(2, 16)], [(2, 20)]], (7, 100)),
                    ins(0x106, 4, [[(2, 0)]], (4, 0))]
            out.case('syn_106_1_v4_zt%d_%d' % (i, pc), pc, blob([], prog), (cvals(cv), 31), ([0] * 8, 7))
    # log with 4 inputs of its own: slot j is input j
    for pc in (24, 53, 64):
        prog = [ins(0x106, 4, [[(2, 0)], [(2, 4)], [(2, 8)], [(2, 12)]], (4, 0))]
        out.case('syn_106_4_v4_own_%d' % pc, pc, blob([], prog),
                 (cvals({0: 2.0, 1: 4.0, 2: 8.0, 3: 16.0, 4: 0.0, 8: 1.0, 9: 0.0, 12: 0.0}), 31), ([0] * 8, 7))
    # log whose zero test is 0 where its operand is outside the table: D3DX does not read the operand
    out.case('syn_106_1_v2_ztonly', 64, blob([], [ins(0x70E, 1, [[(2, 1)], [(2, 2)]], (7, 9)),
                                                   ins(0x106, 2, [[(2, 31)]], (4, 0))]),
             (cvals({1: 1.0, 2: 0.0, 31: 8.0}), 31), ([0] * 8, 7))
    # cmp with two inputs: slot 2 is read only where input 0 is negative or NaN (X 0x4E90E5)
    for i, a0 in enumerate((1.0, 0.0, -0.0, -1.0, float('nan'))):
        out.case('slot_cmp2_%d' % i, 64, blob([], [ins(0x300, 2, [[(2, 0)], [(2, 4)]], (4, 0))]),
                 (cvals({0: 1.0, 1: a0, 4: 7.0, 5: 8.0}), 31), ([0] * 8, 7))
    # no component: no read through a slot nothing set
    for op, nin in ((0x100, 0), (0x101, 0), (0x204, 1), (0x200, 1), (0x500, 1), (0x106, 0), (0x300, 1), (0x103, 0)):
        for sc in (False, True):
            out.case('slot_n0_%03x_%s' % (op, 's' if sc else 'v'), 64,
                     blob([], [ins(op, 0, [[(2, 1)]] * nin, (4, 2), sc)]), (cvals({1: 3.0}), 31), ([0] * 8, 7))
    # and with a component: a read through a slot nothing set
    for op, nin in ((0x204, 1), (0x205, 0), (0x500, 1), (0x106, 1), (0x70E, 1), (0x100, 0)):
        out.case('slot_stale_%03x' % op, 64, blob([], [ins(op, 2, [[(2, 1)]] * nin, (4, 2))]),
                 (cvals({1: 3.0, 2: 1.0}), 31), ([0] * 8, 7))
    # table 8 keeps the slot the instruction before set, or nothing; table 9 is address 0
    out.case('slot_t8_after', 64, blob([], [ins(0x100, 1, [[(2, 7)]], (4, 0)), ins(0x100, 2, [[(8, 0)]], (4, 1))]),
             (cvals({7: 7.0, 8: 8.0}), 31), ([0] * 8, 7))
    out.case('slot_t8_first', 64, blob([], [ins(0x100, 1, [[(8, 0)]], (4, 0))]), (cvals({}), 31), ([0] * 8, 7))
    out.case('slot_t8_index', 64, blob([], [ins(0x100, 1, [[(8, 3), (2, 1)]], (4, 0))]), (cvals({13: 5.0}), 31),
             ([0] * 8, 7))
    out.case('slot_t9_n0', 64, blob([], [ins(0x100, 0, [[(9, 0)]], (4, 0))]), (cvals({}), 31), ([0] * 8, 7))
    out.case('slot_t9_n1', 64, blob([], [ins(0x100, 1, [[(9, 0)]], (4, 0))]), (cvals({}), 31), ([0] * 8, 7))
    # temporaries past 255: written and read at 255 + 1, and far into the frame
    out.case('temp_past_w', 64, blob([], [ins(0x100, 2, [[(2, 0)]], (7, 255))]), (cvals({0: 1.0, 1: 2.0}), 31),
             ([0] * 8, 7))
    out.case('temp_past_r', 64, blob([], [ins(0x100, 2, [[(7, 255)]], (4, 0))]), (cvals({}), 31), ([0] * 8, 7))
    out.case('temp_past_add', 64, blob([], [ins(0x204, 3, [[(7, 254)], [(2, 0)]], (4, 0))]),
             (cvals({0: 1.0}), 31), ([0] * 8, 7))
    out.case('temp_far', 64, blob([], [ins(0x100, 160, [[(7, 200)]], (7, 0))]), (cvals({}), 31), ([0] * 8, 7))
    # noise reads its inputs before it runs: past the table, and through a missing v
    out.case('noise_502_v4_edge', 64, blob([], [ins(0x502, 4, [[(2, 30)]], (4, 0))]), (cvals({30: 0.5, 31: 0.25}), 31),
             ([0] * 8, 7))
    out.case('noise_711_v1_nullv', 64, blob([], [ins(0x711, 1, [[(2, 1)], [(2, 2)], [(3, 0)]], (4, 0))]),
             (cvals({1: 0.5, 2: 0.25}), 31), ([0] * 8, 7))
    # noise in both forms, with and without components
    for op in (0x502, 0x711):
        for sc in (False, True):
            for n in (0, 1, 4):
                out.case('noise_%03x_%s%d' % (op, 's' if sc else 'v', n), 64,
                         blob([], [ins(op, n, [[(2, 1)], [(2, 2)]], (4, 0), sc)]), (cvals({1: 0.5, 2: 0.25}), 31),
                         ([0] * 8, 7))
    # noise reads min(n, 3) components and min(k, 3) inputs (review R2-05): the 4th is out of reach (c[32] of 32; v,
    # which is NULL), so reading it would stop as range; k = 3 with v third is the control that does read it
    for n in (3, 4, 5, 8):
        out.case('noise_502_v%d_c29' % n, 64, blob([], [ins(0x502, n, [[(2, 29)]], (4, 0))]),
                 (cvals({29: 0.5, 30: 0.25, 31: 0.125}), 31), ([0] * 8, 7))
    for k in (3, 4, 5):
        srcs = [[(2, 1 + j)] for j in range(k - 1)] + [[(3, 0)]]
        out.case('noise_711_k%d_lastv' % k, 64, blob([], [ins(0x711, 1, srcs, (4, 0))]),
                 (cvals({1: 0.5, 2: 0.25, 3: 0.125, 4: 2.0}), 31), ([0] * 8, 7))
    # temporaries that start unknown, as in an effect (review R2-09): a read of one no instruction of the run wrote
    # stops, directly, through an index register, at the second component, behind cmp's choice; a written one runs
    # (dot writes its 0 before it reads, a forward copy reads what it wrote)
    cu = cvals({0: 1.0, 1: 2.0, 4: 3.0, 5: -1.0, 8: 4.0})
    for name, prog in (('read', [ins(0x100, 1, [[(7, 5)]], (4, 0))]),
                       ('wrote', [ins(0x100, 2, [[(2, 0)]], (7, 5)), ins(0x100, 2, [[(7, 5)]], (4, 0))]),
                       ('half', [ins(0x100, 1, [[(2, 0)]], (7, 5)), ins(0x100, 2, [[(7, 5)]], (4, 0))]),
                       ('index', [ins(0x100, 1, [[(7, 3), (2, 1)]], (4, 0))]),
                       ('index_wrote', [ins(0x100, 1, [[(2, 0)]], (7, 3)), ins(0x100, 1, [[(7, 3), (2, 1)]], (4, 0))]),
                       ('dot', [ins(0x500, 2, [[(2, 0)], [(2, 4)]], (7, 10)), ins(0x100, 1, [[(7, 10)]], (4, 0))]),
                       ('forward', [ins(0x100, 1, [[(2, 0)]], (7, 0)), ins(0x100, 6, [[(7, 0)]], (7, 1)),
                                    ins(0x100, 7, [[(7, 0)]], (4, 0))]),
                       ('cmp_pos', [ins(0x300, 2, [[(2, 0)], [(2, 4)], [(7, 50)]], (4, 0))]),
                       ('cmp_neg', [ins(0x300, 2, [[(2, 4)], [(2, 0)], [(7, 50)]], (4, 0))])):
        out.case('temp_unknown_%s' % name, 64, blob([], prog), (cu, 31), ([0] * 8, 7), unknown=True)


# ---- the rounding corners of the number model (3.1; review R2-03) ----
# In integers: a value is an integer n (times a power of two that does not matter here), and the tests look at n's
# bits below the precision.  Nothing here computes a result: A, B and the oracle do; this only picks operands.

def _isqrt(n):
    x = 1 << ((n.bit_length() + 1) // 2)
    while True:
        y = (x + n // x) // 2
        if y >= x:
            return x
        x = y


def _sig(b):
    """a finite non-zero double as (m, e): its magnitude is m * 2^e"""
    ex, fr = (b >> 52) & 0x7FF, b & ((1 << 52) - 1)
    return (fr | (1 << 52), ex - 1075) if ex else (fr, -1074)


def _rnd(n, bits, inexact=False):
    """the integer n > 0 rounded to bits significant bits, ties to even (inexact: the value is a little more than n),
    at n's scale"""
    sh = n.bit_length() - bits
    if sh <= 0:
        return n
    top, low, half = n >> sh, n & ((1 << sh) - 1), 1 << (sh - 1)
    if low > half or (low == half and (inexact or top & 1)):
        top += 1
    return top << sh


def _side(n, bits):
    """for the integer n > 0 (or a value a little more), 'above' or 'below' where only the guard and sticky bits tell
    it from a tie at bits significant bits: within 2^-16 of a unit of the tie, just above it (or on it) with the
    truncated last bit even, where a tie would go down to even and the value goes up; just below with that bit odd,
    where a tie would go up and the value goes down; else None"""
    sh = n.bit_length() - bits
    if sh < 24:
        return None
    top, low, half = n >> sh, n & ((1 << sh) - 1), 1 << (sh - 1)
    if abs(low - half) > half >> 15:
        return None
    if low >= half:
        return None if top & 1 else 'above'
    return 'below' if top & 1 else None


def _double(n, inexact):
    """at 64 bits: whether the double the store makes of the register depends on the register being rounded (the bits
    past 64) or truncated"""
    sh = n.bit_length() - 64
    return sh > 0 and _rnd(_rnd(n, 64, inexact), 53) != _rnd(n >> sh << sh, 53)


def _recip(s, bits):
    """2^L / s for L with 80 bits more than bits, and whether it is inexact"""
    big = 1 << (s.bit_length() + bits + 80)
    return big // s, big % s != 0


def corner_kinds(op, a, b, pc):
    """the corners op lands on at pc for the doubles a (and b): rcp, add, mul: 'above' or 'below' a tie at pc
    (_side), 'far' for a subtraction from a tie of an operand 128 or more binades smaller (nothing of it left in A's
    128-bit window but a sticky bit), at 64 bits 'double' (_double); rsq: 'sqrt_above' or 'sqrt_below' when its
    square root lies next to a tie and the root a slip there would give changes the double stored, 'div_above',
    'div_below' for the reciprocal of the rounded root, and 'double'"""
    out = []
    if op == 0x107:
        m, e = _sig(a)
        r = _isqrt(m << (2 * (pc + 70) + (e & 1)))     # the root, 70 bits past pc and more
        inexact = r * r != m << (2 * (pc + 70) + (e & 1))
        root = _rnd(r, pc, inexact)

        def stored(s):
            q, ix = _recip(s, pc)
            v = _rnd(q, pc, ix)
            return _rnd(v, 53) if pc == 64 else v
        side = _side(r, pc)
        if side:
            sh = r.bit_length() - pc
            slip = (r >> sh << sh) if side == 'above' else ((r >> sh) + 1) << sh
            if stored(root) != stored(slip):
                out.append('sqrt_' + side)
        q, inexact = _recip(root, pc)
    elif op == 0x103:
        q, inexact = _recip(_sig(a)[0], pc)
    else:
        (m1, e1), (m2, e2) = _sig(a), _sig(b)
        if op == 0x205:
            q, inexact = m1 * m2, False
        else:
            lo = min(e1, e2)
            q = abs((m1 << (e1 - lo)) * (-1 if a >> 63 else 1) + (m2 << (e2 - lo)) * (-1 if b >> 63 else 1))
            inexact = False
            if not q:
                return out
    side = _side(q, pc)
    if side:
        out.append(('div_' if op == 0x107 else '') + side)
        if op == 0x204 and side == 'below' and (a >> 63) != (b >> 63) and abs(_sig(a)[1] - _sig(b)[1]) >= 128:
            out.append('far')
    if pc == 64 and _double(q, inexact):
        out.append('double')
    return out


def corners(out, rng, per, tries):
    """operands where rcp, rsq, add (subtraction too) and mul land on each corner of corner_kinds at each precision, per
    of each kind (at 64 bits only those the store's rounding lets through: 'double', and rsq's root), found among
    structured significands (runs of ones and zeros) and exponent gaps of 0..89, and for rcp, rsq's division, rsq's
    root at 24 and 53 bits and the far subtraction built next to the tie they are to land by (review R2-03): random
    operands almost never land there, and a slip of the extra quotient bits, the square root's sticky bit or a tie's
    sticky bit shows only there (A against B, integers against rationals, and the oracle)"""
    def mant():
        k = rng.randrange(4)
        if k == 0:
            a, b = rng.randrange(52), rng.randrange(52)
            return sum(1 << i for i in range(52) if (i >= a) != (i >= b))
        if k == 1:
            return rng.getrandbits(52) & ~((1 << rng.randrange(40)) - 1)
        if k == 2:
            return rng.getrandbits(52) | ((1 << rng.randrange(40)) - 1)
        return rng.getrandbits(52)

    def dbl(e, sign):
        return (sign << 63) | ((1023 + e) << 52) | mant()

    def scaled(n, e):
        """the double n * 2^e (n exact in 53 bits)"""
        return d2w(float(Fraction(n) * Fraction(2) ** e))

    def recip_tie(nbits, p, above):
        """an nbits-bit integer r whose reciprocal at p bits lies within 2^-16 of a unit of a tie, above with the last
        bit even or below with it odd: 2^(p - 1 + nbits) / r is a p-bit quotient q and a fraction m / r"""
        while True:
            r = (1 << (nbits - 1)) | rng.getrandbits(nbits - 1)
            q, m = divmod(1 << (p - 1 + nbits), r)
            if abs(2 * m - r) << 15 <= r and (2 * m >= r) == above and (q & 1) != above and q < 1 << p:
                return r

    def built(op, pc, kind):
        """an operand of rcp or rsq next to the tie of kind, or None: rcp of a 53-bit significand found as above;
        rsq's division: a 26-bit root squared (exactly); rsq's root at 24 bits: t^2 rounded to a double for t next to
        a tie (that moves the root by 2^-29 of a unit at most); at 53 bits: M^2 + t for an odd 54-bit M whose square
        lies t < 2^39 from a multiple of 2^55 (M from a square root of -t or t modulo 2^55, by Hensel's lifting), a
        double whose root sqrt(M^2 + t) / 2 lies t / 4M < 2^-16 of a unit from the tie M / 2"""
        above = kind.endswith('above')
        e = rng.randrange(40) - 20
        if op == 0x103 and pc == 24:                    # 1/t rounded to a double moves 1/a by 2^-29 of a unit at most
            n = (1 << 23) | (rng.getrandbits(22) << 1) | (not above)
            t = (Fraction(2 * n + 1, 2) + Fraction(1 if above else -1, 1 << 20)) * Fraction(2) ** (e - 23)
            return d2w(float(1 / t)) | (rng.getrandbits(1) << 63)
        if op == 0x103:
            return scaled(recip_tie(53, pc, above), e - 52) | (rng.getrandbits(1) << 63)
        if kind.startswith('div'):
            return scaled(recip_tie(26, pc, above) ** 2, 2 * e - 50)
        if pc == 24:
            n = (1 << 23) | (rng.getrandbits(22) << 1) | (not above)
            t = (Fraction(2 * n + 1, 2) + Fraction(1 if above else -1, 1 << 20)) * Fraction(2) ** (e - 23)
            return d2w(float(t * t))
        if pc == 53:
            while True:
                t = rng.randrange(1, 1 << 36) * 8 + (7 if above else 1)     # M^2 = -t (above) or t (below) mod 8
                r = (-t if above else t) % (1 << 55)
                x = 1
                for j in range(3, 55):
                    if (x * x - r) % (1 << (j + 1)):
                        x += 1 << (j - 1)
                x %= 1 << 54
                m = x if x >> 53 else (1 << 54) - x
                if m * m >= 1 << 107 and ((m - 1) // 2 & 1) != above:  # the root's last bit: even above, odd below
                    return scaled((m * m + (t if above else -t)) >> 55, 2 * e - 53)
        return None
    for op in (0x103, 0x107, 0x204, 0x205):
        for pc in (24, 53, 64):
            kinds = {'above': 0, 'below': 0}
            if op == 0x107:
                kinds = {'sqrt_above': 0, 'sqrt_below': 0, 'div_above': 0, 'div_below': 0}
            if op == 0x204 and pc == 24:                # a double's 53 bits are never on a tie at 53 or 64
                kinds['far'] = 0
            if pc == 64:                                # the store rounds again: a corner at 64 bits shows only
                kinds = {'double': 0}                   # where it moves the double (and rsq's root where it does)
                if op == 0x107:
                    kinds.update({'sqrt_above': 0, 'sqrt_below': 0})
            k = 0
            for t in range(tries):
                if all(v == per for v in kinds.values()):
                    break
                need = [g for g, v in kinds.items() if v < per and g != 'double']
                a = b = None
                if op in (0x103, 0x107) and t % 2 == 0:
                    for g in need:
                        a = built(op, pc, g)
                        if a is not None:
                            break
                elif 'far' in need and t % 2 == 0:      # on a tie at 24 bits with the last bit odd, minus a speck
                    a = d2w(float(Fraction(2 * ((1 << 23) | (rng.getrandbits(22) << 1) | 1) + 1, 2) *
                                  Fraction(2) ** (rng.randrange(40) - 43))) | (rng.getrandbits(1) << 63)
                    b = dbl(((a >> 52) & 0x7FF) - 1023 - 128 - rng.randrange(60), 1 - (a >> 63))
                if a is None:
                    a = dbl(rng.randrange(40) - 20, rng.getrandbits(1) if op != 0x107 else 0)
                if b is None:
                    b = dbl(((a >> 52) & 0x7FF) - 1023 - rng.randrange(90), rng.getrandbits(1)) if op in (0x204, 0x205) \
                        else 0
                want = [g for g in corner_kinds(op, a, b, pc) if kinds.get(g, per) < per]
                if not want:
                    continue
                for g in want:
                    kinds[g] += 1
                srcs = [[(2, 0)]] + ([[(2, 1)]] if op in (0x204, 0x205) else [])
                out.case('corner_%03x_%d_%s_%d' % (op, pc, want[0], k), pc, blob([], [ins(op, 1, srcs, (4, 0))]),
                         ([a, b, 0, 0], 3), ([0] * 4, 3))
                k += 1


def randoms(out, rng, count, vals):
    """random programs of 1..12 instructions over every exact opcode and form, inputs from every table and through
    index registers that hold small integers, all in range, at a random precision"""
    ops = [(0x100, 1), (0x101, 1), (0x103, 1), (0x104, 1), (0x107, 1), (0x200, 2), (0x201, 2), (0x202, 2), (0x203, 2),
           (0x204, 2), (0x205, 2), (0x300, 3), (0x500, 2), (0x70E, 6), (0x70E, 8), (0x208, 2), (0x301, 3), (0x102, 1)]
    for k in range(count):
        pc = rng.choice((24, 53, 64))
        c = [vals.d() for _ in range(64)]
        for j in range(56, 64):                               # index values: small integers, halves, negatives
            c[j] = d2w(rng.choice((0.0, 1.0, 2.0, 1.5, 2.9, -0.0, 0.25)))
        lits = [vals.d() for _ in range(rng.randint(1, 5))] + [d2w(float(rng.randint(0, 3)))]
        prog = [ins(0x100, 8, [[(2, 48)]], (7, 0)), ins(0x100, 8, [[(2, 40)]], (4, 16))]
        for _ in range(rng.randint(1, 12)):
            op, nin = rng.choice(ops)
            n = rng.randint(1, 4)
            srcs = []
            for _j in range(nin):
                kind = rng.randrange(7)
                if kind == 0:
                    srcs.append([(1, rng.randrange(len(lits)))])
                elif kind == 1:
                    srcs.append([(7, rng.randrange(12))])
                elif kind == 2:
                    srcs.append([(rng.choice((4, 5, 6)), rng.randrange(24))])
                elif kind == 3:
                    srcs.append([(3, rng.randrange(8))])
                elif kind == 4:
                    srcs.append([(rng.choice((2, 1)), 56 + rng.randrange(8) if rng.random() < 0.7 else len(lits) - 1),
                                 (2, rng.randrange(40))])
                else:
                    srcs.append([(2, rng.randrange(56))])
            dst = rng.choice([(7, rng.randrange(12)), (4, rng.randrange(28)), (5, rng.randrange(28)),
                              (6, rng.randrange(28))])
            prog.append(ins(op, n, srcs, dst, rng.random() < 0.15))
        prog.append(ins(0x100, 12, [[(7, 0)]], (4, 0)))
        out.case('rand_%d' % k, pc, blob(lits, prog), (c, 63), ([0] * 32, 31), ([vals.d() for _ in range(16)], 15))


def loader(out, rng, vals):
    """every check of the loader (2.2): versions, the comment search, counts, operand flags and tables, bounds"""
    good = [ins(0x204, 4, [[(2, 0)], [(1, 1)]], (4, 0)), ins(0x100, 1, [[(1, 0), (2, 4)]], (7, 3))]
    lits = [d2w(1.0), d2w(2.0)]
    c, o = ([d2w(float(i)) for i in range(16)], 15), ([0] * 8, 7)
    for v in (0x46580100, 0x465801FF, 0x46580200, 0x46580201, 0x46580202, 0x54580100, 0x54580101, 0x545800FF,
              0xFFFF0200, 0xFFFE0300, 0x46590200, 0):
        out.case('ld_version_%08x' % v, 64, blob(lits, good, version=v), c, o)
    out.case('ld_noclit', 64, blob(lits, good, clit=False), c, o)
    out.case('ld_nofxlc', 64, blob(lits, good, fxlc=False), c, o)
    out.case('ld_noend', 64, blob(lits, good, end=False), c, o)                    # found before the end
    out.case('ld_noend_noclit', 64, blob(lits, good, clit=False, end=False), c, o)  # the CLIT search runs off
    out.case('ld_clit0', 64, blob([], [ins(0x100, 2, [[(1, 0)]], (4, 0))]), c, o)      # count 0: D3DX makes it 1
    out.case('ld_clit0_wrap', 64, blob([], [ins(0x100, 3, [[(1, 5)]], (4, 0))]), c, o)
    out.case('ld_count0', 64, blob(lits, []), c, o)
    out.case('ld_notrailer', 64, blob(lits, good, trailer=False), c, o)
    # comments the search walks over: bit 31 tokens, short comments, instruction tokens with a length
    for name, lead in (('bit31', [0x80000000, 0xFFFFFFFE]), ('len1', [0x0001FFFE, CLIT]), ('len0', [0x0000FFFE]),
                       ('instr', [0x03000001, 1, 2, 3]), ('def', [0x00000051, 1, 2, 3, 4, 5]),
                       ('other', [0x0002FFFE, 0x12345678, 0])):
        out.case('ld_lead_%s' % name, 64, blob(lits, good, lead=lead), c, o)
    out.case('ld_tx', 64, blob(lits, good, version=0x54580100), c, o, ([d2w(0.25)] * 4, 3))
    for name, lead in (('def', [0x00000051, 1, 2, 3, 4, 0x0000FFFF]), ('def_fe', [0x00000051, 1, 2, 3, 0x0000FFFF, 7]),
                       ('def_short', [0x00000051, 1, 2, 3, 4]), ('instr_len', [0x03000001, 0x80000000, 0x80000000,
                                                                               0x80000000]),
                       ('instr_plain', [0x00000002])):
        for v in (0x54580100, 0x46580200):              # tx_1_0 skips 5 dwords after def; 2.0 reads bits 24..27
            out.case('ld_walk_%s_%08x' % (name, v), 64, blob(lits, good, version=v, lead=lead), c, o)
    # operands: flags, tables, bounds
    for t in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0xFFFFFFFF):
        out.case('ld_in_table_%d' % (t & 0xFFFF), 64, blob(lits, [ins(0x100, 1, [[(t, 0)]], (4, 0))]), c, o)
        out.case('ld_out_table_%d' % (t & 0xFFFF), 64, blob(lits, [ins(0x100, 1, [[(2, 0)]], (t, 0))]), c, o)
        out.case('ld_idx_table_%d' % (t & 0xFFFF), 64, blob(lits, [ins(0x100, 1, [[(t, 1), (2, 0)]], (4, 0))]), c, o)
    p = ins(0x100, 1, [[(2, 0)]], (4, 0))
    p[-3] = 1                                               # an output with an index pair: the flag is not 0
    out.case('ld_out_flag1', 64, blob(lits, [p]), c, o)
    out.case('ld_nin0', 64, blob(lits, [ins(0x000, 1, [], (4, 0))]), c, o)
    out.case('ld_nin300', 64, blob(lits, [ins(0x100, 1, [[(2, 0)]] * 300, (4, 0))]), c, o)
    out.case('ld_nin257', 64, blob(lits, [ins(0x100, 1, [[(2, 0)]], (4, 1)),
                                          ins(0x70E, 1, [[(2, k % 16)] for k in range(257)], (4, 0))]), c, o)
    out.case('ld_nin256', 64, blob(lits, [ins(0x70E, 1, [[(2, k % 16)] for k in range(256)], (4, 0))]), c, o)
    # truncated at every dword of a small program, and the bounds around the end (2.2)
    full = blob(lits, good, trailer=False, end=False)
    for n in range(1, len(full) + 1):
        out.case('ld_trunc_%d' % n, 64, full[:n], c, o)
    for extra in range(0, 4):
        out.case('ld_bytes_%d' % extra, 64, full + [0x12345678], c, o, nbytes=4 * len(full) + extra)
    # random corruption of the good program
    base = blob(lits, good)
    for k in range(200):
        w = list(base)
        for _ in range(rng.randint(1, 3)):
            i = rng.randrange(len(w))
            w[i] = rng.choice([0, 1, 2, 7, 9, 10, w[i] ^ (1 << rng.randrange(32)), rng.getrandbits(32)])
        out.case('ld_corrupt_%d' % k, 64, w, c, o)


def mw(out, protos, rng, per, vals):
    """every preshader of the effects with per input tables at each precision, as the effect runs it: the c table
    D3DX gives it (mask + 1 doubles, zero past 4 fend, 2.5) and temporaries it reads from the stack (5.1)"""
    for p in protos:
        for r in sorted(p.progs):
            pr = p.progs[r]
            if pr.kind != 'preshader':
                continue
            data = p.fx.resources[r][5]
            w = list(struct.unpack_from('<%dI' % (len(data) // 4), data))
            fend = bend = iend = 0
            for cst in pr.consts:
                if cst.why == 6:
                    continue
                end = cst.reg + cst.count
                if cst.rset == 2:
                    fend = max(fend, end)
                elif cst.rset == 0:
                    bend = max(bend, end)
                elif cst.rset == 1:
                    iend = max(iend, end)
            tot = bend + 4 * (fend + iend)
            cmask = smear(tot - 1) if tot else 0
            tg = p.fx.targets[r][1]
            nreg = max(1, (tg.rows + 3) // 4 * tg.cols * max(1, tg.count))
            omask = smear(4 * nreg - 1)
            for k in range(per):
                for pc in (24, 53, 64):
                    c = [vals.d() for _ in range(4 * fend)] + [0] * (cmask + 1 - 4 * fend)
                    out.case('mw_%s_r%d_%d_%d' % (p.name, r, k, pc), pc, w, (c, cmask), ([0] * (omask + 1), omask),
                             nbytes=len(data), unknown=True)


def main_vm(indir, outp, seed, per_mw, per_syn, nrand, per_corner):
    rng = random.Random(seed)
    vals = Vals(rng)
    protos = M.load(indir) if indir != '-' else []
    with open(outp, 'w') as f:
        f.write('fxpcase 1\n')
        out = Out(f)
        synth(out, rng, per_syn, vals)
        synth(out, rng, 1, Vals(rng, snan=True), 'snan')
        snan_mem(out, rng, per_syn)
        targeted(out)
        randoms(out, rng, nrand, vals)
        corners(out, rng, per_corner, 40000)
        loader(out, rng, vals)
        mw(out, protos, rng, per_mw, vals)
    print('fxpres_cases vm: %d cases in %s' % (out.n, outp))


def main_mutate(indir, outp, seed, copies):
    """copies corrupted copies of every preshader of <indir>/*.fxo (truncated, bits flipped, small dwords, tokens,
    bytes appended), each with random tables at a random precision: for A against B, and the loader's verdicts
    against the oracle"""
    rng = random.Random(seed)
    vals = Vals(rng)
    protos = M.load(indir)
    with open(outp, 'w') as f:
        f.write('fxpcase 1\n')
        out = Out(f)
        for p in protos:
            for r in sorted(p.progs):
                if p.progs[r].kind != 'preshader':
                    continue
                data = bytearray(p.fx.resources[r][5])
                for k in range(copies):
                    b = bytearray(data)
                    how = rng.randrange(6)
                    if how == 0:
                        b = b[:rng.randrange(len(b) + 1)]
                    elif how == 1:
                        for _ in range(rng.randint(1, 4)):
                            b[rng.randrange(len(b))] ^= 1 << rng.randrange(8)
                    elif how == 2:
                        at = rng.randrange(len(b) // 4) * 4
                        b[at:at + 4] = struct.pack('<I', rng.randrange(12))
                    elif how == 3:
                        at = rng.randrange(len(b) // 4) * 4
                        b[at:at + 4] = struct.pack('<I', (rng.randrange(0x800) << 20) | rng.randrange(6) |
                                                   (0x80000000 if rng.random() < 0.2 else 0))
                    elif how == 4:
                        at = rng.randrange(len(b) // 4) * 4
                        b[at:at + 4] = struct.pack('<I', rng.getrandbits(32))
                    else:
                        b += bytes(rng.getrandbits(8) for _ in range(rng.randrange(1, 64)))
                    nb = len(b)
                    w = list(struct.unpack_from('<%dI' % ((nb + 3) // 4), bytes(b) + b'\0\0\0'))
                    out.case('mut_%s_r%d_%d' % (p.name, r, k), rng.choice((24, 53, 64)), w,
                             ([vals.d() for _ in range(64)], 63), ([0] * 64, 63), ([vals.d() for _ in range(16)], 15),
                             nbytes=nb)
    print('fxpres_cases mutate: %d cases in %s' % (out.n, outp))


# ---- patched copies of the effects ----

def fxlc_movs(data):
    """the instructions of a preshader as (byte offset of the token, token, [(byte offset of the register pair, table,
    offset) of each input]) (F 4.3)"""
    at = 4
    while True:                                         # the comments after the version, up to FXLC
        tok = struct.unpack_from('<I', data, at)[0]
        n = (tok >> 16) & 0x7FFF
        if struct.unpack_from('<I', data, at + 4)[0] == FXLC:
            break
        at += 4 + 4 * n
    q, out = at + 12, []
    for _ in range(struct.unpack_from('<I', data, at + 8)[0]):
        tok, nin = struct.unpack_from('<II', data, q)
        start, q = q, q + 8
        srcs = []
        for _k in range(nin):
            npairs = struct.unpack_from('<I', data, q)[0]
            q += 4 + 8 * npairs
            srcs.append((q,) + struct.unpack_from('<II', data, q))
            q += 8
        q += 12
        out.append((start, tok, srcs))
    return out


def main_patch(indir, outdir, how, opcode=None, n1=False):
    """write the copies of patch (the effects are game data: outdir is under out/)"""
    os.makedirs(outdir, exist_ok=True)
    nfiles = npatch = 0
    for name in sorted(f for f in os.listdir(indir) if f.endswith('.fxo')):
        with open(os.path.join(indir, name), 'rb') as f:
            orig = f.read()
        data = bytearray(orig)
        p = M.Proto(name[:-4], orig)
        for r in sorted(p.progs):
            pr = p.progs[r]
            if pr.kind != 'preshader':
                continue
            blob = p.fx.resources[r][5]
            edits = []                                  # (byte offset in the blob, new dword)
            if how == 'op':
                for at, tok, srcs in fxlc_movs(blob):
                    if (tok >> 20) & 0x7FF == 0x100 and not tok >> 31 and (not n1 or tok & 0xFFFFF == 1):
                        edits.append((at, (tok & ~(0x7FF << 20)) | (opcode << 20)))
            else:
                fend = bend = iend = 0
                for cst in pr.consts:
                    if cst.why != 6:
                        end = cst.reg + cst.count
                        if cst.rset == 2:
                            fend = max(fend, end)
                        elif cst.rset == 0:
                            bend = max(bend, end)
                        elif cst.rset == 1:
                            iend = max(iend, end)
                tot = bend + 4 * (fend + iend)
                cmask = smear(tot - 1) if tot else 0
                for at, tok, srcs in fxlc_movs(blob):
                    if (tok >> 20) & 0x7FF != 0x100 or tok >> 31 or not srcs or 4 * fend + (tok & 0xFFFFF) > cmask + 1:
                        continue
                    q, t, off = srcs[0]
                    if fend and t == 2:
                        edits.append((q + 4, 4 * fend))
                    elif not fend and t == 1:
                        edits += [(q, 2), (q + 4, 0)]
                    else:
                        continue
                    break
            if not edits:
                continue
            npatch += 1
            s = orig.find(blob)
            while s >= 0:                               # every copy of these bytes is this program
                for at, w in edits:
                    struct.pack_into('<I', data, s + at, w)
                s = orig.find(blob, s + 1)
        with open(os.path.join(outdir, name), 'wb') as f:
            f.write(data)
        nfiles += 1
    print('fxpres_cases patch %s: %d files, %d preshaders patched in %s' % (how, nfiles, npatch, outdir))


# ---- rounds ----

def inputs(p):
    """the top-level parameters a preshader of p reads, in parameter order"""
    ids = set()
    for pr in p.progs.values():
        if pr.kind == 'preshader':
            for cst in pr.consts:
                if cst.why == 0 and not cst.sampler:
                    ids.add(cst.id)
    return sorted(ids)


def fw(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


# SetFloat and friends on INT and BOOL parameters: _ftol2's truncation to 64 bits (3e9, -3e9), NaN and the
# infinities, halves either side of zero, signed zeros, a subnormal; colours either side of (k + 0.5) / 255
F_EDGE = [fw(x) for x in (0.0, 1.0, -1.0, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 0.49999997, 3e9, -3e9, 2147483648.0,
                          -2147483648.0, 2147483520.0, 4294967296.0, 9.3e18, 1e20, -1e20, 1e-40, 16.0, 7.0, 255.0)] + \
    [0x80000000, 0x7FC00000, 0xFFC00001, 0x7F800001, 0x7F800000, 0xFF800000]
C_EDGE = [fw(x) for x in (0.0, 1.0, 0.5, 0.25, 0.999, 0.0039, 1.2, -0.3, 0.5 / 255, 1.5 / 255, 127.5 / 255, 254.5 / 255,
                          0.49999 / 255, 0.50001 / 255, 2.0, -0.0)] + [0x7FC00000, 0x7F800000, 0xFF800000]


def calls(rng, vals, name, path, n):
    """setter calls on parameter n (fx-preshader 6.3): the setters MW uses (fx-model 1.2) on every shape and type,
    with the values where D3DX's conversions show"""
    out = []

    def call(setter, ws, count=0):
        out.append('call %s %s %s %d %d %s' % (setter, name, path, count, len(ws), ' '.join('%08x' % w for w in ws)))
    one = n.rows == 1 and n.cols == 1 and not n.elements
    k = rng.randrange(4)
    if one:
        if n.type == M.FLOAT and k == 0:            # D3DX compares floats before it stores: -0 after +0, NaN after NaN
            a = rng.choice([0, 0x80000000, 0x7FC00000, fw(1.0)])
            call('SetFloat', [a])
            call('SetFloat', [rng.choice([0, 0x80000000, 0x7FC00000, 0xFFC00000, a])])
        elif k == 0:
            call('SetFloat', [rng.choice(F_EDGE)])
        elif k == 1:
            call('SetVector', [rng.choice(C_EDGE) for _ in range(4)])
        elif k == 2:
            call(rng.choice(['SetInt', 'SetIntArray']), [vals.word(M.INT)], 1)
        else:
            call('SetFloatArray', [rng.choice(F_EDGE)], 1)
    elif n.cls == M.VECTOR and not n.elements:
        if k == 0:
            call('SetVector', [rng.choice(F_EDGE + C_EDGE) for _ in range(4)])
        elif k == 1:
            call('SetInt', [vals.word(M.INT)])
        elif k == 2:
            call('SetFloatArray', [rng.choice(F_EDGE) for _ in range(n.cols)], n.cols)
        else:
            call(rng.choice(['SetFloat', 'SetVectorArray']), [rng.choice(F_EDGE) for _ in range(4)], 1)
    elif n.cls == M.MATRIX_ROWS and not n.elements:
        m = [vals.word(M.FLOAT) if rng.random() < 0.5 else rng.choice(F_EDGE) for _ in range(16)]
        call(['SetMatrix', 'SetMatrixTranspose', 'SetMatrixArray', 'SetFloatArray'][k], m,
             [0, 0, rng.randrange(2), 16][k])
    return out


def main_rounds(indir, outp, seed, rounds, pcs):
    rng = random.Random(seed)
    vals = Vals(rng)
    protos = M.load(indir)
    nset = ncall = 0
    with open(outp, 'w') as f:
        f.write('fxpround 1\n')
        for pc in pcs:
            for k in range(rounds):
                f.write('round %d_%d pc %d\n' % (pc, k, pc))
                for p in protos:
                    for nid in inputs(p):
                        n = p.nodes[nid]
                        ws = [vals.word(n.type) for _ in range(n.bytes // 4)]
                        f.write('set %s %s %d %s\n' % (p.name, n.path.decode('latin-1'), len(ws),
                                                       ' '.join('%08x' % x for x in ws)))
                        nset += 1
                if k % 2:                                   # every other round: the typed setters on top
                    for p in protos:
                        for nid in inputs(p):
                            for line in calls(rng, vals, p.name, p.nodes[nid].path.decode('latin-1'), p.nodes[nid]):
                                f.write(line + '\n')
                                ncall += 1
        f.write('end\n')
    print('fxpres_cases rounds: %d rounds, %d sets, %d setter calls in %s' % (rounds * len(pcs), nset, ncall, outp))


def main(argv):
    args, opt = [], {'--seed': '1', '--mw': '4', '--synth': '3', '--random': '2000', '--rounds': '6', '--pc': '24,53,64',
                     '--copies': '20', '--corners': '12'}
    i = 0
    while i < len(argv):
        if argv[i] in opt and i + 1 < len(argv):
            opt[argv[i]] = argv[i + 1]
            i += 2
            continue
        args.append(argv[i])
        i += 1
    if len(args) == 3 and args[0] == 'vm':
        return main_vm(args[1], args[2], int(opt['--seed']), int(opt['--mw']), int(opt['--synth']), int(opt['--random']),
                       int(opt['--corners']))
    if len(args) == 3 and args[0] == 'mutate':
        return main_mutate(args[1], args[2], int(opt['--seed']), int(opt['--copies']))
    if len(args) >= 4 and args[0] == 'patch' and args[3] in ('op', 'pad'):
        return main_patch(args[1], args[2], args[3], int(args[4], 16) if args[3] == 'op' else None, '--n1' in args)
    if len(args) == 3 and args[0] == 'rounds':
        return main_rounds(args[1], args[2], int(opt['--seed']), int(opt['--rounds']),
                           [int(x) for x in opt['--pc'].split(',')])
    sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
