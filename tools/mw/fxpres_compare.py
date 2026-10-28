#!/usr/bin/env python3
"""Implementation A's preshader results against the oracle's (docs/mw/fx-preshader.md 6.4).

  tools/mw/fxpres_compare.py vm [--kinds] <a.txt> <oracle.txt>  'fxpres 1' from tools/mw/fxpresrun.c and fxoracle.c;
                                                              --kinds: every kind of stop must occur (the generated
                                                              cases have each, so none of them goes unchecked)
  tools/mw/fxpres_compare.py effect [--fsin-inf] <a.txt> <oracle.txt>
                                                              'fxpeff 1' from fxpresrun.c and 'fxpdev 1' from fxoracle.c;
                                                              --fsin-inf: effects whose preshaders take sin or cos
  tools/mw/fxpres_compare.py host <a.txt> <a2.txt>            A's 'fxpres 1' on two hosts (native and wasm): the same
                                                              bytes but for the approximate class, which calls each
                                                              host's C library
  tools/mw/fxpres_compare.py --self-test                      the comparison on made-up texts: each way A can be
                                                              wrong that it must catch

vm: every verdict is compared, A's own refusals and stops included.  The load: ok, fail (D3DX's E_FAIL) or A's
'unsupported scan', which needs the oracle's 'load scan' (D3DX's loader read past the blob).  The run: A's ok,
range, stale, inputs or noise against what the oracle saw the interpreter do (a fault past a table or at address 0,
or a data access in its frame: range; a fault at the poisoned slots, or a read of a temporary the run did not
write when they start unknown: stale; inputs; noise).  A program both run to the end must give every output and
temporary the same bits (class exact) or, for the approximate class (3.2), the same bits or at most MAXULP units in
the last place apart.  Counted apart, each as narrowly as it is (5.2): a signalling NaN the x87 loads quiet and
Unicorn does not (only where the opcode under test loads through the x87, only as the quiet bit); an x87 operation
on a register Unicorn left signalling (only in snan_ cases, whose signalling NaNs can be register operands: the
snanm_ cases put them only in memory operands, where the two agree, review R2-04); fsin and fcos of an infinity;
fyl2x of a zero; NaN results the oracle takes from the host's C library.
effect: every preshader state A computes must be on the device with the same dwords in the same pass, none may be
an error, every device value that changes between rounds must be one A computes, and the setters' results ('call')
must be the same in the same order (6.3).  With --fsin-inf a float dword that is A's default NaN where the device has
an infinity is counted apart: fsin and fcos of an infinity (5.2).
Exit status 1 on any split; the first ones are printed.
"""
import sys
from collections import Counter, defaultdict

MAXULP = 4
POISON = 0x41414141                 # tools/mw/fxoracle.c: the address the oracle's input slots start with
QUIET = 1 << 51


def parse_vm(path):
    cases, cur = {}, None
    with open(path) as f:
        head = f.readline().split()
        if head[:2] != ['fxpres', '1']:
            sys.exit('%s: not an fxpres 1 file' % path)
        for line in f:
            w = line.split()
            if not w or w[0] == 'end':
                continue
            if w[0] == 'case':
                cur = {'load': w[3], 'why': ' '.join(w[4:])}
                cases[w[1]] = cur
            elif w[0] == 'class':
                cur['class'] = w[1]
            elif w[0] == 'run':
                cur['run'] = w[1:]
            elif w[0] == 'o':
                cur['o'] = [int(x, 16) for x in w[1:]]
            elif w[0] == 'r':
                cur['r'] = {} if w[1:] == ['-'] else {int(a): int(b, 16) for a, b in (x.split('=') for x in w[1:])}
    return cases


def isnan(b):
    return (b >> 52) & 0x7FF == 0x7FF and b & 0xFFFFFFFFFFFFF


def isinf(b):
    return (b >> 52) & 0x7FF == 0x7FF and not b & 0xFFFFFFFFFFFFF


def issnan(b):
    return isnan(b) and not b & QUIET


def ulps(a, b):
    """units in the last place between two doubles of the same sign (an ordering of the bits)"""
    def key(x):
        return -(x & ~(1 << 63)) if x >> 63 else x
    return abs(key(a) - key(b))


def oracle_run(w):
    """the oracle's run line as one of A's run results, and what the oracle saw"""
    if w[0] == 'fault':
        a = int(w[1], 16)
        if POISON <= a < POISON + 0x1000000:
            return 'stale', 'a read through an input slot nothing set (a fault at the poisoned slot)'
        return 'range', 'a fault past a table, the literals or the blob, or at address 0'
    if w[0] == 'frame':
        return 'range', 'a data access in the interpreter\'s frame (temporaries past 255)'
    if w[0] == 'temp':
        return 'stale', 'a read of a temporary the run did not write (they start unknown)'
    return w[0], ''


def case_op(name):
    """the opcode under test and its form, from a generated case's name (syn_/snan_/snanm_<op>_<nin>_<s|v><n>_<rep>)"""
    w = name.split('_')
    if w[0] in ('syn', 'snan', 'snanm') and len(w) >= 4:
        try:
            return int(w[1], 16), w[3][:1] == 's'
        except ValueError:
            pass
    return None, None


def x87_loads(op, scalar):
    """the opcode in this form loads its inputs with fld, where a signalling NaN is loaded quiet (5.2); not mov's
    copy nor the forms that do nothing, where the oracle's bits are the hardware's"""
    if scalar:
        return 0x200 <= op <= 0x206 or op == 0x500
    return op in (0x101, 0x103, 0x104, 0x105, 0x106, 0x107, 0x300, 0x500, 0x70E) or 0x108 <= op <= 0x10C or \
        0x200 <= op <= 0x206


ARITH = (0x103, 0x104, 0x204, 0x205, 0x500, 0x70E)          # an x87 operation with a memory operand
HOSTLIBM = (0x105, 0x108, 0x109, 0x10A, 0x10B, 0x10C, 0x206)  # msvcrt's helpers; QEMU's fsin, fcos, fpatan


STOPS = ('scan', 'range', 'stale', 'inputs', 'noise')


def main_vm(pa, po, out=print, kinds=False):
    A, O = parse_vm(pa), parse_vm(po)
    seen = set()
    c = Counter()
    bad, maxu = [], 0
    for name, a in A.items():
        o = O.get(name)
        if o is None:
            bad.append('%s: not in the oracle' % name)
            continue
        if o['load'] == 'fault':
            bad.append('%s: the oracle faulted in the loader, A says %s' % (name, a['load']))
            continue
        if a['load'] == 'unsupported' or o['load'] == 'scan':
            if (a['load'], a['why'], o['load']) == ('unsupported', 'scan', 'scan'):
                c['both stop at load: the loader reads past the blob (scan)'] += 1
                seen.add('scan')
            else:
                bad.append('%s: load: A %s %s, D3DX %s' % (name, a['load'], a['why'], o['load']))
            continue
        if a['load'] != o['load']:
            bad.append('%s: load: A %s %s, D3DX %s' % (name, a['load'], a['why'], o['load']))
            continue
        if a['load'] == 'fail':
            c['both refuse'] += 1
            continue
        if 'run' not in o:
            c['not run by the oracle (run 0)'] += 1
            continue
        orun, detail = oracle_run(o['run'])
        arun = a['run'][0]
        if arun != orun:
            bad.append('%s: run: A %s, D3DX %s (%s)' % (name, arun, orun, ' '.join(o['run'])))
            continue
        if arun != 'ok':
            c['both stop: %s%s' % (arun, ', ' + detail if detail else '')] += 1
            seen.add(arun)
            continue
        approx = a.get('class') == 'approx'
        op, scalar = case_op(name)
        snan = name.startswith('snan_')                 # signalling NaNs anywhere
        snanm = name.startswith('snanm_')               # only in memory operands
        diffs = []
        pairs = [('o[%d]' % i, x, y) for i, (x, y) in enumerate(zip(a['o'], o['o']))]
        if len(a['o']) != len(o['o']):
            diffs.append('o has %d and %d entries' % (len(a['o']), len(o['o'])))
        for k in sorted(set(a['r']) | set(o['r'])):
            pairs.append(('r%d' % k, a['r'].get(k, 0), o['r'].get(k, 0)))
        apart = Counter()
        for where, x, y in pairs:
            if x == y:
                continue
            if (snan or snanm) and op is not None and x87_loads(op, scalar) and issnan(y) and x == y | QUIET:
                apart['a signalling NaN the x87 loads quiet, Unicorn keeps (5.2)'] += 1
                continue
            if snan and op in ARITH and not scalar and isnan(x) and isnan(y) and not issnan(x):
                apart['an x87 operation on a register Unicorn left signalling: another NaN (5.2)'] += 1
                continue
            if approx:
                if op == 0x106 and not scalar and x == 0xFFF0000000000000 and y == 0:
                    apart['fyl2x of a zero: the x87 gives -inf, Unicorn leaves +0 (5.2)'] += 1
                    continue
                if isnan(x) and isinf(y) and op in (0x108, 0x109):
                    apart['fsin/fcos of an infinity: A NaN, Unicorn keeps it (5.2)'] += 1
                    continue
                if isnan(x) and isnan(y) and (op in HOSTLIBM or op is None):
                    apart['a NaN from the host C library: another NaN (5.2)'] += 1
                    continue
                if not isnan(x) and not isnan(y) and (x >> 63) == (y >> 63):
                    u = ulps(x, y)
                    if u <= MAXULP:
                        maxu = max(maxu, u)
                        continue
            diffs.append('%s A %016x D3DX %016x' % (where, x, y))
        if diffs:
            bad.append('%s (%s): %s' % (name, a.get('class'), '; '.join(diffs[:4])))
        elif apart:
            for k in apart:
                c['counted apart: ' + k] += 1
        else:
            c[('same bits' if not approx else 'approx: within %d ulp' % MAXULP) +
              (' (signalling NaNs in)' if snan else ' (signalling NaNs in memory operands)' if snanm else '')] += 1
    for name in O:
        if name not in A:
            bad.append('%s: only in the oracle' % name)
    if kinds:
        for k in STOPS:
            if k not in seen:
                bad.append('no case where both stop with %s: that check went unused' % k)
    for k in sorted(c):
        out('  %-78s %d' % (k, c[k]))
    out('  approximate class: largest distance %d ulp' % maxu)
    if bad:
        out('%d splits:' % len(bad))
        for b in bad[:30]:
            out('  ' + b)
        return 1
    out('A and the oracle agree on %d cases' % len(A))
    return 0


def parse_passes(path, head):
    """{(round, effect, tech, pass): [line words]}, the pass lines' extras (the oracle's control words), and the
    setter calls in order"""
    out, extra, calls, cur = {}, {}, [], None
    rnd = eff = None
    with open(path) as f:
        if f.readline().split()[:2] != head:
            sys.exit('%s: not an %s file' % (path, ' '.join(head)))
        for line in f:
            w = line.split()
            if not w or w[0] == 'end':
                continue
            if w[0] == 'call':
                calls.append(w[1:])
            elif w[0] == 'round':
                rnd = w[1] + ' ' + w[3]
            elif w[0] == 'effect':
                eff = w[1]
            elif w[0] == 'tech' and w[2] == 'begin':          # the interpreter ran inside Begin (state saving)
                extra.setdefault('begin', []).append(' '.join(w[3:]))
            elif w[0] == 'tech':
                cur = (rnd, eff, int(w[1]), int(w[3]))
                out[cur] = []
                extra[cur] = w[4:]
            else:
                out[cur].append(w)
    return out, extra, calls


CW = {24: '007f', 53: '027f', 64: '037f'}


def fsnan(w):
    return (w >> 23) & 0xFF == 0xFF and w & 0x7FFFFF and not w & 0x400000


def device_values(call):
    """an oracle device call as {(key..., dword index): value}"""
    k = call[0]
    if k in ('rs', 'lightenable'):
        return {(k, call[1], 0): call[2]}
    if k in ('tss', 'ss'):
        return {(k, call[1], call[2], 0): call[3]}
    if k in ('transform', 'light'):
        return {(k, call[1], i): v for i, v in enumerate(call[2:])}
    if k == 'material':
        return {(k, i): v for i, v in enumerate(call[1:])}
    return {}


def claims(k, n):
    """the device dwords a state of A with key k and n dwords stands for"""
    if k[0] in ('rs', 'lightenable'):
        return [(k[0], k[1], 0)]
    if k[0] in ('tss', 'ss'):
        return [(k[0], k[1], k[2], 0)]
    if k[0] in ('transform', 'light'):
        return [(k[0], k[1], int(k[2]) + i) for i in range(n)]
    if k[0] == 'material':
        return [('material', int(k[1]) + i) for i in range(n)]
    return []


def main_effect(pa, po, out=print, fsin_inf=False):
    A, _, acalls = parse_passes(pa, ['fxpeff', '1'])
    O, oextra, ocalls = parse_passes(po, ['fxpdev', '1'])
    c = Counter()
    bad = []
    if len(acalls) != len(ocalls):
        bad.append('%d setter calls in A, %d in the oracle' % (len(acalls), len(ocalls)))
    for x, y in zip(acalls, ocalls):
        if x != y:
            bad.append('call: A %s, D3DX %s' % (' '.join(x), ' '.join(y)))
        else:
            c['call %s: the same result (%s)' % (x[0], 'S_OK' if x[-1] == '00000000' else x[-1])] += 1
    for b in oextra.get('begin', []):
        c['Begin ran the interpreter at %s' % b] += 1
    claimed = defaultdict(set)
    for key, states in A.items():
        if key not in O:
            bad.append('%s: no such pass in the oracle' % (key,))
            continue
        calls = O[key]
        pc = int(key[0].split()[1])
        ex = oextra[key]
        if 'BeginPass' in ex:
            bad.append('%s: BeginPass failed in D3DX: %s' % (key, ' '.join(ex)))
            continue
        if 'cw' in ex:
            cws = ex[ex.index('cw') + 1:]
            c['interpreter ran at %s' % ' '.join(cws)] += 1
            if cws != [CW[pc]]:
                bad.append('%s: the interpreter ran at %s, the round is pc %d' % (key, cws, pc))
        for w in states:
            arrow = w.index('->')
            eq = w.index('=')
            k, vals = w[arrow + 1:eq], w[eq + 1:]
            where = '%s %s' % (key, ' '.join(w[:arrow]))
            if vals and vals[0] == 'error':
                bad.append('%s: %s A error %s' % (where, ' '.join(k), ' '.join(vals[1:])))
                continue
            claimed[key[1:]].update(claims(k, len(vals)))
            kind = k[0]
            if kind in ('rs', 'tss', 'ss', 'lightenable'):
                want = [x for x in calls if x[:len(k)] == k]
                got = [want[-1][len(k)]] if want else None
            elif kind in ('transform', 'light'):
                want = [x for x in calls if x[0] == kind and x[1] == k[1]]
                off = int(k[2])
                got = want[-1][2 + off:2 + off + len(vals)] if want else None
            elif kind == 'material':
                want = [x for x in calls if x[0] == 'material']
                off = int(k[1])
                got = want[-1][1 + off:1 + off + len(vals)] if want else None
            else:
                bad.append('%s: %s: a state of a class the device recorder does not show' % (where, ' '.join(k)))
                continue
            if got is None:
                bad.append('%s: %s is not on the device' % (where, ' '.join(k)))
                continue
            quieted = sininf = 0
            ok = len(got) == len(vals)
            for x, y in zip(vals, got):
                if x == y:
                    continue
                xi, yi = int(x, 16), int(y, 16)
                if fsnan(yi) and xi == yi | 0x00400000:
                    quieted += 1                     # Unicorn keeps a signalling float NaN signalling (5.2)
                elif fsin_inf and xi == 0xFFC00000 and yi in (0x7F800000, 0xFF800000):
                    sininf += 1                      # QEMU's fsin and fcos keep an infinity (5.2)
                else:
                    ok = False
            if not ok:
                bad.append('%s: %s A %s D3DX %s' % (where, ' '.join(k), ' '.join(vals), ' '.join(got)))
            elif quieted:
                c['%s: same dwords but a signalling float NaN A makes quiet (5.2)' % kind] += 1
            elif sininf:
                c['%s: same dwords but fsin/fcos of an infinity: A NaN, Unicorn keeps it (5.2)' % kind] += 1
            else:
                c['%s: same dwords' % kind] += 1
    # the other way: a device value that changes between rounds comes from a parameter the rounds set, so from a
    # preshader A must have computed in that pass (no state of MW's takes such a parameter directly)
    seen = defaultdict(lambda: defaultdict(set))
    for key, calls in O.items():
        for call in calls:
            for dk, v in device_values(call).items():
                seen[key[1:]][dk].add(v)
    nvar = 0
    for ps in sorted(seen):
        for dk, vs in seen[ps].items():
            if len(vs) < 2:
                continue
            nvar += 1
            if dk not in claimed[ps]:
                bad.append('%s: the device value %s changes between rounds (%d values), and A has no state for it'
                           % (ps, ' '.join(map(str, dk)), len(vs)))
    c['device dwords that change between rounds (each one a state of A)'] = nvar
    for k in sorted(c):
        out('  %-78s %d' % (k, c[k]))
    if bad:
        out('%d splits:' % len(bad))
        for b in bad[:30]:
            out('  ' + b)
        return 1
    out('A and the oracle agree on every preshader state of %d passes' % len(A))
    return 0


def main_host(pa, pb, out=print):
    """A on two hosts: every case the same, but the outputs and temporaries of an approximate-class case may differ
    by at most MAXULP units or be two NaNs (exp, log, sin, ... come from each host's C library, 5.1)"""
    def cases(path):
        with open(path) as f:
            text = f.read()
        if not text.startswith('fxpres 1\n'):
            sys.exit('%s: not an fxpres 1 file' % path)
        return [blk.split('\n') for blk in text[len('fxpres 1\n'):].split('\ncase ')]
    A, B = cases(pa), cases(pb)
    c, bad, maxu = Counter(), [], 0
    if len(A) != len(B):
        bad.append('%d cases and %d' % (len(A), len(B)))
    for x, y in zip(A, B):
        name = x[0].split()[0].replace('case ', '')
        if x == y:
            c['the same bytes'] += 1
            continue
        if '  class approx' not in x or len(x) != len(y) or any(
                p != q for p, q in zip(x, y) if not (p.startswith('  o ') or p.startswith('  r '))):
            bad.append('%s: %s' % (name, next('%s | %s' % (p, q) for p, q in zip(x, y) if p != q)))
            continue
        ok = True
        for p, q in zip(x, y):
            if p == q:
                continue
            u = [int(v.split('=')[-1], 16) for v in p.split()[1:]] if p.split()[1:] != ['-'] else []
            w = [int(v.split('=')[-1], 16) for v in q.split()[1:]] if q.split()[1:] != ['-'] else []
            if len(u) != len(w) or (p.startswith('  r ') and [v.split('=')[0] for v in p.split()[1:]] !=
                                    [v.split('=')[0] for v in q.split()[1:]]):
                ok = False
                break
            for g, h in zip(u, w):
                if g == h or (isnan(g) and isnan(h)):
                    continue
                if isnan(g) or isnan(h) or g >> 63 != h >> 63 or ulps(g, h) > MAXULP:
                    ok = False
                    break
                maxu = max(maxu, ulps(g, h))
        if ok:
            c['approximate class: within %d ulp or both NaN' % MAXULP] += 1
        else:
            bad.append('%s (approx): %s' % (name, next('%s | %s' % (p, q) for p, q in zip(x, y) if p != q)))
    for k in sorted(c):
        out('  %-78s %d' % (k, c[k]))
    out('  approximate class: largest distance %d ulp' % maxu)
    if bad:
        out('%d splits:' % len(bad))
        for b in bad[:30]:
            out('  ' + b)
        return 1
    out('the two hosts agree on %d cases' % len(A))
    return 0


# ---- the self-test: each way A can be wrong that the comparison must catch, and what it must let through ----

def self_test():
    import os, tempfile
    fails, checks = [], 0
    d = tempfile.mkdtemp(prefix='fxpres_compare_')

    def text(path, lines):
        with open(path, 'w') as f:
            f.write('\n'.join(lines) + '\nend\n')
        return path

    def vm(alines, olines):
        a = text(os.path.join(d, 'a.txt'), ['fxpres 1'] + alines)
        o = text(os.path.join(d, 'o.txt'), ['fxpres 1 oracle'] + olines)
        return main_vm(a, o, out=lambda s: None)

    def eff(alines, olines):
        a = text(os.path.join(d, 'a.txt'), ['fxpeff 1'] + alines)
        o = text(os.path.join(d, 'o.txt'), ['fxpdev 1'] + olines)
        return main_effect(a, o, out=lambda s: None)

    def check(what, got, want):
        nonlocal checks
        checks += 1
        if got != want:
            fails.append('%s: exit %d, want %d' % (what, got, want))
    out0 = ['  o 0000000000000000', '  r -']
    acase = ['case t load ok', '  class exact']
    oran = ['case t load ok', '  run ok'] + out0
    # A refuses or stops where D3DX runs on: a split
    for kind in ('range', 'stale', 'noise', 'inputs'):
        check('A %s, D3DX ran' % kind, vm(acase + ['  run ' + kind] + out0, oran), 1)
    check('A scan, D3DX loaded', vm(['case t load unsupported scan'], oran), 1)
    check('A scan, D3DX failed', vm(['case t load unsupported scan'], ['case t load fail']), 1)
    check('A loaded, D3DX read past the blob', vm(acase + ['  run ok'] + out0, ['case t load scan 0x004da353']), 1)
    check('A range, D3DX stale', vm(acase + ['  run range'] + out0, ['case t load ok', '  run fault 0x41414141']), 1)
    check('A stale, D3DX past a table', vm(acase + ['  run stale'] + out0, ['case t load ok', '  run fault 0x21140000']), 1)
    check('A ran, D3DX in its frame', vm(acase + ['  run ok'] + out0, ['case t load ok', '  run frame 0x280ff000 0x004e8d5f']), 1)
    check('A ran, D3DX noise', vm(acase + ['  run ok'] + out0, ['case t load ok', '  run noise']), 1)
    # where they agree, it passes
    check('both range (frame)', vm(acase + ['  run range'] + out0, ['case t load ok', '  run frame 0x280ff000 0x004e8d5f']), 0)
    check('both range (guard)', vm(acase + ['  run range'] + out0, ['case t load ok', '  run fault 0x21140000']), 0)
    check('both stale', vm(acase + ['  run stale'] + out0, ['case t load ok', '  run fault 0x41414149']), 0)
    check('both scan', vm(['case t load unsupported scan'], ['case t load scan 0x004da353']), 0)
    check('both inputs', vm(acase + ['  run inputs'] + out0, ['case t load ok', '  run inputs']), 0)
    a = text(os.path.join(d, 'a.txt'), ['fxpres 1'] + acase + ['  run inputs'] + out0)
    o = text(os.path.join(d, 'o.txt'), ['fxpres 1 oracle', 'case t load ok', '  run inputs'])
    check('--kinds: only inputs seen', main_vm(a, o, out=lambda s: None, kinds=True), 1)
    # signalling NaNs: mov copies bits, so a NaN made quiet after mov is a split; after neg it is the x87's rule
    snan_o = ['case snan_100_1_v1_0 load ok', '  run ok', '  o 7ff0000000000001', '  r -']
    check('mov made a signalling NaN quiet', vm(['case snan_100_1_v1_0 load ok', '  class exact', '  run ok',
                                                 '  o 7ff8000000000001', '  r -'], snan_o), 1)
    neg_o = ['case snan_101_1_v1_0 load ok', '  run ok', '  o fff0000000000001', '  r -']
    check('neg: quiet where Unicorn keeps it signalling', vm(['case snan_101_1_v1_0 load ok', '  class exact', '  run ok',
                                                              '  o fff8000000000001', '  r -'], neg_o), 0)
    check('neg: another NaN', vm(['case snan_101_1_v1_0 load ok', '  class exact', '  run ok',
                                  '  o fff8000000000002', '  r -'], neg_o), 1)
    # log: -inf against Unicorn's +0 is the x87's (5.2); a NaN with another sign is not
    log_a = ['case syn_106_1_v2_0 load ok', '  class approx', '  run ok']
    log_o = ['case syn_106_1_v2_0 load ok', '  run ok']
    check('log(0)', vm(log_a + ['  o fff0000000000000', '  r -'], log_o + ['  o 0000000000000000', '  r -']), 0)
    check('log(-NaN) keeps its sign', vm(log_a + ['  o fffc000000abcdef', '  r -'], log_o + ['  o 7ffc000000abcdef', '  r -']), 1)
    # arithmetic on signalling NaNs: another quiet NaN is Unicorn's register rule only where a signalling NaN can be
    # the register operand (snan_); where they are memory operands only (snanm_) the two agree, so it is a split
    # (review R2-04: the operand roles of add and mul)
    ar_o = ['  run ok', '  o 7ff8000000000005', '  r -']
    ar_a = ['  class exact', '  run ok', '  o 7ff8000000000009', '  r -']
    check('snan_ add: another quiet NaN', vm(['case snan_204_2_v1_0 load ok'] + ar_a, ['case snan_204_2_v1_0 load ok'] + ar_o), 0)
    check('snanm_ add: another quiet NaN', vm(['case snanm_204_2_v1_0 load ok'] + ar_a, ['case snanm_204_2_v1_0 load ok'] + ar_o), 1)
    check('snanm_ add: the memory NaN made quiet', vm(['case snanm_204_2_v1_0 load ok', '  class exact', '  run ok',
                                                      '  o 7ff8000000000009', '  r -'],
                                                     ['case snanm_204_2_v1_0 load ok', '  run ok', '  o 7ff0000000000009', '  r -']), 0)
    # temporaries that start unknown: the oracle's read of one the run did not write is stale
    check('temp read: both stale', vm(acase + ['  run stale'] + out0, ['case t load ok', '  run temp 0x280fe3e4 0x004e8d5f']), 0)
    check('temp read: A ran on', vm(acase + ['  run ok'] + out0, ['case t load ok', '  run temp 0x280fe3e4 0x004e8d5f']), 1)
    # A on two hosts: exact cases byte for byte, approximate ones within MAXULP
    def host(alines, blines):
        a = text(os.path.join(d, 'a.txt'), ['fxpres 1'] + alines)
        b = text(os.path.join(d, 'b.txt'), ['fxpres 1'] + blines)
        return main_host(a, b, out=lambda s: None)
    ex = ['case t load ok', '  class exact', '  run ok']
    ap = ['case t load ok', '  class approx', '  run ok']
    check('host: an exact case 1 ulp apart', host(ex + ['  o 3ff0000000000000', '  r -'], ex + ['  o 3ff0000000000001', '  r -']), 1)
    check('host: an approximate case 1 ulp apart', host(ap + ['  o 3ff0000000000000', '  r -'], ap + ['  o 3ff0000000000001', '  r -']), 0)
    check('host: an approximate case, NaN signs', host(ap + ['  o 7ff8000000000000', '  r -'], ap + ['  o fff8000000000000', '  r -']), 0)
    check('host: an approximate case far apart', host(ap + ['  o 3ff0000000000000', '  r -'], ap + ['  o 3ff0000000001000', '  r -']), 1)
    check('host: another verdict', host(ap + ['  o 3ff0000000000000', '  r -'], ['case t load ok', '  class approx', '  run range',
                                                                                 '  o 3ff0000000000000', '  r -']), 1)
    # effects: an error, a value A does not compute, a setter's result
    r1, r2 = ['round a pc 24', 'effect E', '  tech 0 pass 0'], ['round b pc 24', 'effect E', '  tech 0 pass 0']
    st1, st2 = '    state 0 AMBIENT[0] -> rs 139 = 00000001', '    state 0 AMBIENT[0] -> rs 139 = 00000002'
    o1 = ['round a pc 24', 'effect E', '  tech 0 pass 0 cw 007f', '    rs 139 00000001']
    o2 = ['round b pc 24', 'effect E', '  tech 0 pass 0 cw 007f', '    rs 139 00000002']
    check('the same states', eff(r1 + [st1] + r2 + [st2], o1 + o2), 0)
    check('an error', eff(r1 + ['    state 0 AMBIENT[0] -> rs 139 = error type'], o1), 1)
    check('a value that changes, no state of A', eff(r1 + r2, o1 + o2), 1)
    check('a setter\'s result', eff(['call SetFloat E P = 00000000'] + r1 + [st1], ['call SetFloat E P = 8876086c'] + o1), 1)
    check('the same setter results', eff(['call SetFloat E P = 00000000'] + r1 + [st1], ['call SetFloat E P = 00000000'] + o1), 0)
    # fsin of an infinity: A's default NaN against Unicorn's infinity, only with --fsin-inf
    sa = r1 + ['    state 0 LIGHTDIFFUSE[0] -> light 0 1 = ffc00000']
    so = ['round a pc 24', 'effect E', '  tech 0 pass 0 cw 007f', '    light 0 00000000 ff800000']
    check('fsin(inf) without --fsin-inf', eff(sa, so), 1)
    a = text(os.path.join(d, 'a.txt'), ['fxpeff 1'] + sa)
    o = text(os.path.join(d, 'o.txt'), ['fxpdev 1'] + so)
    check('fsin(inf) with --fsin-inf', main_effect(a, o, out=lambda s: None, fsin_inf=True), 0)
    for f in os.listdir(d):
        os.remove(os.path.join(d, f))
    os.rmdir(d)
    print('fxpres_compare --self-test: %d checks, %d failed' % (checks, len(fails)))
    for f in fails:
        print('  FAIL ' + f)
    return 1 if fails else 0


def main(argv):
    if argv[:1] == ['--self-test']:
        return self_test()
    if len(argv) == 4 and argv[:2] == ['vm', '--kinds']:
        return main_vm(argv[2], argv[3], kinds=True)
    if len(argv) == 3 and argv[0] == 'vm':
        return main_vm(argv[1], argv[2])
    if len(argv) == 3 and argv[0] == 'effect':
        return main_effect(argv[1], argv[2])
    if len(argv) == 4 and argv[:2] == ['effect', '--fsin-inf']:
        return main_effect(argv[2], argv[3], fsin_inf=True)
    if len(argv) == 3 and argv[0] == 'host':
        return main_host(argv[1], argv[2])
    sys.exit(__doc__)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
