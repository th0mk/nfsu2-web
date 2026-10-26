"""Differential test of the two M5-01 dumpers on mutated copies of the 31 effects.

  .venv/bin/python -I tools/mw/fxdiff_mutate.py [--model] <checkout> <copies per file> [seed]

A = out/mw/fx/bin/fxdump (subprocess), B = tools/fx2dump.py (imported).
Prints a summary of agreements, refusals and differences; writes each
differing case to out/mw/fx/diff/cases/<n>/ with a.txt/b.txt for inspection
(game bytes: they stay under out/, which is ignored).

With --model the two effect models are compared instead (docs/mw/fx-model.md
§6.3): A = out/mw/fx/bin/fxmodeldump, B = tools/fxmodel.py (imported).  Each
mutated copy is modelled alone or, half the time, in one pool with an
unchanged copy of another effect, so mutated typedefs meet real ones in the
pool.  Splits go to out/mw/fx/diff/model_cases/<n>/.
"""
import os, random, shutil, signal, struct, subprocess, sys, importlib.util, collections

ARGS = sys.argv[1:]
MODEL = bool(ARGS) and ARGS[0] == '--model'
if MODEL:
    ARGS = ARGS[1:]
W = ARGS[0]
N = int(ARGS[1])
seed = int(ARGS[2]) if len(ARGS) > 2 else 1
rnd = random.Random(seed)
HERE = os.path.join(W, 'out', 'mw', 'fx', 'diff')
CASES = os.path.join(HERE, 'model_cases' if MODEL else 'cases')
os.makedirs(CASES, exist_ok=True)
TMP = os.path.join(HERE, 'tmp.fxo')

spec = importlib.util.spec_from_file_location('fx2dump', os.path.join(W, 'tools', 'fx2dump.py'))
B = importlib.util.module_from_spec(spec)
spec.loader.exec_module(B)
A = os.path.join(W, 'out', 'mw', 'fx', 'bin', 'fxdump')
RES = os.path.join(W, 'out', 'mw', 'fx', 'res')


class Timeout(Exception):
    pass


def on_alarm(*_):
    raise Timeout()


signal.signal(signal.SIGALRM, on_alarm)


def run_b(name, data):
    signal.alarm(5)
    try:
        return 'ok', B.Effect(name, data).dump()
    except B.Bad as e:
        return 'refused', str(e)
    except Timeout:
        return 'timeout', ''
    except RecursionError:
        return 'crash', 'RecursionError'
    except Exception as e:
        return 'crash', '%s: %s' % (type(e).__name__, e)
    finally:
        signal.alarm(0)


def run_a(name, data):
    with open(TMP, 'wb') as f:
        f.write(data)
    try:
        p = subprocess.run([A, TMP], capture_output=True, timeout=20)
    except subprocess.TimeoutExpired:
        return 'timeout', ''
    if p.returncode == 0:
        # A names the dump after its file name; put the real name back
        return 'ok', p.stdout.decode('latin-1').replace('effect "tmp"', 'effect "%s"' % name, 1)
    if p.returncode == 1 and b'AddressSanitizer' not in p.stderr and b'runtime error' not in p.stderr:
        return 'refused', p.stderr.decode('latin-1').strip()
    return 'crash', p.stderr.decode('latin-1')[-2000:]


def locate(data):
    """File offsets of interesting fields, from a plain walk of a whole file."""
    u = lambda o: struct.unpack_from('<I', data, 8 + o)[0]
    tables = struct.unpack_from('<I', data, 4)[0]
    p = tables
    np_, nt, _, _ = [u(p + 4 * i) for i in range(4)]
    p += 16
    loc = collections.defaultdict(list)
    loc['counts'] += [8 + tables + 4 * i for i in range(4)]
    for _ in range(np_):
        loc['param'] += [8 + p + 4 * i for i in range(4)]
        loc['typedef'].append(u(p))
        loc['value'].append(u(p + 4))
        na = u(p + 12)
        p += 16
        for _ in range(na):
            loc['typedef'].append(u(p)); loc['value'].append(u(p + 4)); p += 8
    for _ in range(nt):
        loc['tech'] += [8 + p + 4 * i for i in range(3)]
        na, nps = u(p + 4), u(p + 8)
        p += 12
        for _ in range(na):
            loc['typedef'].append(u(p)); loc['value'].append(u(p + 4)); p += 8
        for _ in range(nps):
            loc['pass'] += [8 + p + 4 * i for i in range(3)]
            pa, ns = u(p + 4), u(p + 8)
            p += 12
            for _ in range(pa):
                loc['typedef'].append(u(p)); loc['value'].append(u(p + 4)); p += 8
            for _ in range(ns):
                loc['state'] += [8 + p + 4 * i for i in range(4)]
                loc['typedef'].append(u(p + 8)); loc['value'].append(u(p + 12))
                p += 16
    nstr, nres = u(p), u(p + 4)
    p += 8
    for _ in range(nstr):
        loc['odata'] += [8 + p, 8 + p + 4]
        p += 8 + (u(p + 4) + 3 & ~3)
    for _ in range(nres):
        loc['reshead'] += [8 + p + 4 * i for i in range(6)]
        size = u(p + 20)
        loc['resdata'] += [8 + p + 24 + 4 * i for i in range(size // 4)]
        p += 24 + (size + 3 & ~3)
    loc['typedef'] = [8 + o + 4 * i for o in loc['typedef'] for i in range(7)]
    loc['value'] = [8 + o + 4 * i for o in loc['value'] for i in range(4)]
    return loc


VALUES = [0, 1, 2, 3, 4, 5, 7, 8, 0x10, 0xFF, 0x100, 0xFFFF, 0xFFFE, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
          0xFFFFFFFE, 0x0000FFFF, 0x46580200, 0xFFFE0101, 0xFFFF0201, 0x42415443, 0x54494C43, 0x434C5846, 0x00080FFFE,
          0x10000100, 0x10000101, 0x20000204, 0x70E00004, 0x30100004, 0x80000000 | 0x10000001]


def mutate(data, loc):
    b = bytearray(data)
    kind = rnd.choice(['resdata'] * 6 + ['state'] * 2 + ['typedef'] * 2 + ['value', 'reshead', 'param', 'tech',
                                                                            'pass', 'counts', 'odata', 'flip',
                                                                            'trunc', 'resdata2'])
    if kind == 'flip':
        for _ in range(rnd.randint(1, 4)):
            i = rnd.randrange(len(b))
            b[i] ^= 1 << rnd.randrange(8)
    elif kind == 'trunc':
        b = b[:rnd.randrange(len(b))]
    else:
        k = 'resdata' if kind == 'resdata2' else kind
        if not loc[k]:
            return mutate(data, loc)
        reps = 3 if kind == 'resdata2' else 1
        for _ in range(reps):
            o = rnd.choice(loc[k])
            if o + 4 > len(b):
                continue
            r = rnd.random()
            if r < 0.6:
                v = rnd.choice(VALUES)
            elif r < 0.8:
                v = struct.unpack_from('<I', b, o)[0] ^ (1 << rnd.randrange(32))
            else:
                v = rnd.getrandbits(32)
            struct.pack_into('<I', b, o, v)
    return kind, bytes(b)


def model_main():
    spec = importlib.util.spec_from_file_location('fxmodel', os.path.join(W, 'tools', 'fxmodel.py'))
    mb = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mb)
    am = os.path.join(W, 'out', 'mw', 'fx', 'bin', 'fxmodeldump')
    case = os.path.join(HERE, 'model_case')
    shutil.rmtree(CASES, ignore_errors=True)
    os.makedirs(CASES)
    names = sorted(f[:-4] for f in os.listdir(RES) if f.endswith('.fxo'))
    stats, nbad = collections.Counter(), 0
    for name in names:
        data = open(os.path.join(RES, name + '.fxo'), 'rb').read()
        loc = locate(data)
        for i in range(N):
            kind, m = mutate(data, loc)
            shutil.rmtree(case, ignore_errors=True)
            os.makedirs(case)
            files = [os.path.join(case, name + '.fxo')]
            with open(files[0], 'wb') as f:
                f.write(m)
            if rnd.random() < 0.5:
                other = rnd.choice([n for n in names if n != name])
                files.append(os.path.join(case, other + '.fxo'))
                shutil.copyfile(os.path.join(RES, other + '.fxo'), files[-1])
            rnd.shuffle(files)
            ra, ta = None, ''
            for args in (['-'], ['--ops', '-']):          # the model dump, then the Set/Get/lookup trace
                p = subprocess.run([am] + args + files, capture_output=True, timeout=120)
                if p.returncode == 0:
                    ra, ta = 'ok', ta + p.stdout.decode('latin-1')
                elif p.returncode == 1 and b'Sanitizer' not in p.stderr and b'runtime error' not in p.stderr:
                    ra, ta = 'refused', p.stderr.decode('latin-1').strip()
                    break
                else:
                    ra, ta = 'crash', p.stderr.decode('latin-1')[-3000:]
                    break
            signal.alarm(120)
            try:
                protos = mb.load(case)
                insts, pool = mb.build(protos)
                rb, tb = 'ok', mb.dump_all(insts, pool) + mb.ops_all(protos)
            except SystemExit as e:
                rb, tb = 'refused', str(e)
            except Timeout:
                rb, tb = 'timeout', ''
            except RecursionError:
                rb, tb = 'crash', 'RecursionError'
            except Exception as e:
                rb, tb = 'crash', '%s: %s' % (type(e).__name__, e)
            finally:
                signal.alarm(0)
            if ra == rb == 'ok' and ta == tb:
                stats['same output'] += 1
                continue
            if ra == rb == 'refused':
                stats['both refused'] += 1
                continue
            stats['DIFFERENT output' if ra == rb == 'ok' else '%s/%s' % (ra, rb)] += 1
            if nbad < 400:
                d = os.path.join(CASES, '%03d_%s_%d_%s' % (nbad, name, i, kind))
                shutil.copytree(case, d)
                with open(os.path.join(d, 'a.txt'), 'w') as f:
                    f.write('%s\n%s' % (ra, ta))
                with open(os.path.join(d, 'b.txt'), 'w') as f:
                    f.write('%s\n%s' % (rb, tb))
            nbad += 1
    shutil.rmtree(case, ignore_errors=True)
    if not nbad:
        shutil.rmtree(CASES, ignore_errors=True)
    for k, v in sorted(stats.items()):
        print('%-24s %d' % (k, v))
    print('model cases written:', min(nbad, 400))
    return 1 if nbad else 0


if MODEL:
    sys.exit(model_main())

stats = collections.Counter()
diffs = []
for f in sorted(os.listdir(RES)):
    if not f.endswith('.fxo'):
        continue
    name = f[:-4]
    data = open(os.path.join(RES, f), 'rb').read()
    loc = locate(data)
    for i in range(N):
        kind, m = mutate(data, loc)
        ra, ta = run_a(name, m)
        rb, tb = run_b(name, m)
        key = (ra, rb)
        if ra == rb == 'ok':
            if ta == tb:
                stats['same output'] += 1
            else:
                stats['DIFFERENT output'] += 1
                diffs.append((name, i, kind, m, ta, tb))
        elif ra == rb == 'refused':
            stats['both refused'] += 1
        else:
            stats['%s/%s' % key] += 1
            diffs.append((name, i, kind, m, '%s: %s' % (ra, ta[:3000]), '%s: %s' % (rb, tb[:3000])))
for k, v in sorted(stats.items()):
    print('%-24s %d' % (k, v))
for n, (name, i, kind, m, ta, tb) in enumerate(diffs[:400]):
    d = os.path.join(CASES, '%03d_%s_%d_%s' % (n, name, i, kind))
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, 'in.fxo'), 'wb').write(m)
    open(os.path.join(d, 'a.txt'), 'w').write(ta)
    open(os.path.join(d, 'b.txt'), 'w').write(tb)
print('cases written:', min(len(diffs), 400))
sys.exit(1 if diffs else 0)
