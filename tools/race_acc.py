#!/usr/bin/env python3
"""race_acc.py -- how accurately each game of a LAN race shows the other players' cars, close up.

  tools/race_acc.py <run dir> [--near M] [--label TEXT] [--json FILE]

Reads web/tools/lan-pair.mjs's <run dir>/i*/cars.csv (NFSU2_CAR_TRACE, runtime/autotest.c: per frame the drawn place,
velocity and heading of every race car, the world step, WorldTimer and RealTimer), i*/clock.csv and i*/log.txt.

Time: every instance's trace clock is its page's own (a worker's time origin + now(): pages started across a WSL clock
step read that far apart, 1-9 s seen), so the instances are lined up by clock.csv: each "[ctclock]" line's trace time
against when lan-pair saw it by its own monotonic clock; the least difference per instance is its offset (the console
way's least delay, the same for all).  Never by the race clock (WorldTimer): that is measured here.

For every (owner, viewer) pair: where the viewer draws the owner's car against where the owner had it at the same
instant (the owner's own trace, interpolated), while the owner moves (> 2 m/s), split by the distance between the
viewer's own car and the drawn one (near: within --near m, default 50; far):
  err     |drawn - true| (m): median, p95, max
  along   the error along the owner's way (horizontal, + = drawn ahead): mean (m, and as ms of the owner's speed) and
          its standard deviation (the rubber band: how far the car swings back and forth along its way)
  across  the error across the way (horizontal): p95 of its size
  head    the drawn heading against the owner's (degrees): p95
  pops    frames whose drawn move differs from the frame before's (scaled by the viewer's world steps) by over
          0.5 m / 2 m, a minute of near (far) time: the visible snaps
Per instance: its race clock against the host's (WorldTimer - host WorldTimer at the same instant, each smoothed over
+-0.15 s for the frame pacer's steps: mean, p95 of the size, p95 of its change over 0.5 s; jumps: frames where the
race clock moved over 50 ms off the wall clock at once), and its frames whose world step was over 5 % off the real
time the frame took (the frame pacer's step): a joiner's clock slew speeding its whole world up or down for the frame.
"""
import csv, glob, json, math, os, sys
from bisect import bisect_left

WT_HZ = 4000.0
MIN_V = 2.0


def results_frame(d):
    try:
        with open(os.path.join(d, 'log.txt'), errors='replace') as f:
            for line in f:
                if 'screen UI_PostRaceResults.fng' in line and line.startswith('[fng] f='):
                    return int(line.split('f=')[1].split()[0])
    except OSError:
        pass
    return None


def fnum(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def load(d):
    rows = []
    stop = results_frame(d)
    with open(os.path.join(d, 'cars.csv')) as f:
        for r in csv.DictReader(f):
            t = fnum(r.get('t')); n = fnum(r.get('n'))
            if t is None or n is None:
                continue
            if stop is not None and int(r['frame']) >= stop:
                break
            if r.get('hud') == '0':
                continue
            cars = []
            for i in range(int(n)):
                x, y, z, v = (fnum(r.get(f'c{i}{k}')) for k in 'xyzv')
                if x is None:
                    cars.append(None); continue
                vx, vy, vz, fx, fy = (fnum(r.get(f'c{i}{k}')) for k in ('vx', 'vy', 'vz', 'fx', 'fy'))
                cars.append({'local': r.get(f'c{i}local') == '1', 'p': (x, y, z), 'v': v,
                             'vel': (vx, vy, vz) if vx is not None else None, 'f': (fx, fy) if fx is not None else None})
            rows.append({'t': t, 'frame': int(r['frame']), 'wt': int(r['worldtimer'] or 0), 'rt': int(r['realtimer'] or 0),
                         'wdt': fnum(r.get('wdt')), 'cap': fnum(r.get('cap')), 'cars': cars})
    return rows


def clock_offset(d):
    """this tool's monotonic seconds minus the trace clock, the least over the instance's [ctclock] lines"""
    best, n = None, 0
    try:
        with open(os.path.join(d, 'clock.csv')) as f:
            for r in csv.DictReader(f):
                a, t = fnum(r.get('arrived_ms')), fnum(r.get('trace_s'))
                if a is None or t is None:
                    continue
                o = a / 1000.0 - t; n += 1
                if best is None or o < best:
                    best = o
    except OSError:
        pass
    return best, n


def pct(v, p):
    if not v:
        return float('nan')
    v = sorted(v)
    return v[min(len(v) - 1, int(p * (len(v) - 1) + 0.5))]


def mean(v):
    return sum(v) / len(v) if v else float('nan')


def sd(v):
    if len(v) < 2:
        return float('nan')
    m = mean(v)
    return math.sqrt(sum((x - m) ** 2 for x in v) / (len(v) - 1))


def own_track(rows):
    tr = []
    for r in rows:
        for c in r['cars']:
            if c and c['local']:
                tr.append((r['t'], c)); break
    return tr


def lerp(a, b, k):
    return tuple(a[i] + k * (b[i] - a[i]) for i in range(len(a)))


def at(tr, ts, t):
    """the owner's own car at time t (its clock): place, velocity, forward"""
    i = bisect_left(ts, t)
    if i <= 0 or i >= len(tr):
        return None
    (ta, a), (tb, b) = tr[i - 1], tr[i]
    if tb - ta > 0.25 or tb <= ta:
        return None
    k = (t - ta) / (tb - ta)
    p = lerp(a['p'], b['p'], k)
    vel = lerp(a['vel'], b['vel'], k) if a['vel'] and b['vel'] else None
    f = lerp(a['f'], b['f'], k) if a['f'] and b['f'] else None
    return p, vel, f


def heading(f):
    return math.atan2(f[1], f[0]) if f and (f[0] or f[1]) else None


def angdiff(a, b):
    d = (a - b + math.pi) % (2 * math.pi) - math.pi
    return abs(d)


def main(argv):
    near_m, label, jout = 50.0, '', None
    args = []
    i = 0
    while i < len(argv):
        if argv[i] == '--near': near_m = float(argv[i + 1]); i += 2
        elif argv[i] == '--label': label = argv[i + 1]; i += 2
        elif argv[i] == '--json': jout = argv[i + 1]; i += 2
        else: args.append(argv[i]); i += 1
    if len(args) != 1:
        print(__doc__); return 2
    run = args[0]
    dirs = sorted(d for d in glob.glob(os.path.join(run, 'i*')) if os.path.exists(os.path.join(d, 'cars.csv')))
    if len(dirs) < 2:
        print('need two or more instances with cars.csv'); return 2
    names = [os.path.basename(d) for d in dirs]
    data, off = {}, {}
    out = {'label': label, 'run': run, 'instances': {}, 'pairs': [], 'pooled': {}}
    for d, k in zip(dirs, names):
        data[k] = load(d)
        o, n = clock_offset(d)
        off[k] = o
        out['instances'][k] = {'rows': len(data[k]), 'clock_lines': n}
    if any(o is None for o in off.values()):
        print('note: an instance has no clock.csv lines: lined up by its own wall clock (may be off)')
        for k in off:
            off[k] = 0.0 if off[k] is None else off[k]
    # common time: trace clock + offset (offsets relative to the first instance, the host)
    h = names[0]
    for k in names:
        for r in data[k]:
            r['T'] = r['t'] + off[k] - off[h]
    print(f'== {label or run}')
    print('instance  rows   racing s   fps    clock vs i1 (ms, wall-clock difference the traces had)')
    for k in names:
        rows = data[k]
        if not rows:
            print(f'{k:8s}  no rows'); continue
        span = rows[-1]['T'] - rows[0]['T']
        rel = (off[k] - off[h]) * 1000.0
        out['instances'][k].update({'span_s': span, 'clock_vs_i1_ms': rel})
        print(f'{k:8s}  {len(rows):5d}  {span:8.1f}  {len(rows) / span if span > 0 else 0:6.1f}   {rel:+9.1f}')
    tracks = {k: own_track(v) for k, v in data.items()}
    trs ={k: [(x[0] + off[k] - off[h], x[1]) for x in tracks[k]] for k in names}

    # race clock and warp per instance.  A frame's WorldTimer moves in the frame pacer's world steps (16.7 / 33 ms on
    # a busy box) against the wall clock, so per frame it is that noisy: each instance's race clock minus the common
    # time is smoothed by a running median over +-SMOOTH_S (a slew or a jump still shows, a frame's step does not)
    SMOOTH_S = 0.15
    def clock_track(rows):
        raw = [(r['T'], r['wt'] / WT_HZ - r['T']) for r in rows]
        sm, lo, hi = [], 0, 0
        for i, (t, _) in enumerate(raw):
            while raw[lo][0] < t - SMOOTH_S: lo += 1
            while hi + 1 < len(raw) and raw[hi + 1][0] <= t + SMOOTH_S: hi += 1
            w = sorted(x[1] for x in raw[lo:hi + 1])
            sm.append((t, w[len(w) // 2]))
        return sm, raw
    hsm, hraw = clock_track(data[h])
    hT = [x[0] for x in hsm]
    print('instance  race clock - host (ms): mean   p95|e|  p95 0.5s change  jumps>50ms   frames warped >5 % (world step vs real)')
    for k in names:
        rows = data[k]
        sm, raw = clock_track(rows)
        e, et = [], []
        if k != h:
            for t, o in sm:
                i = bisect_left(hT, t)
                if 0 < i < len(hsm) and hsm[i][0] - hsm[i - 1][0] < 0.25:
                    (ta, oa), (tb, ob) = hsm[i - 1], hsm[i]
                    e.append((o - (oa + (ob - oa) * (t - ta) / (tb - ta))) * 1000.0); et.append(t)
        ch, j = [], 0
        for i in range(len(e)):
            while et[j] < et[i] - 0.5: j += 1
            if et[i] - et[j] > 0.4: ch.append(abs(e[i] - e[j]))
        jumps = sum(1 for (ta, a), (tb, b) in zip(raw, raw[1:]) if abs(b - a) > 0.05)
        # the world's rate: each frame's world step against the frame pacer's step it was made of.  A frame's step is
        # the frame cap [0x865558] or a multiple of it (1/60 s: 16.67, 33.33 ms; the host's within ~3 %), so a step
        # more than 5 % off that is a joiner's clock slew (0x601A60: the step x 0.85 .. x 1.2, e.g. 14.17 / 20.0 ms):
        # its whole world sped up or slowed down for the frame
        warped, nr, worst = 0, 0, 0.0
        for b in rows:
            w, cap = b['wdt'], b['cap']
            if w is None or not w > 0 or not cap or not cap > 0: continue
            nom = cap * max(1, round(w / cap))
            r = w / nom - 1.0; nr += 1
            if abs(r) > 0.05: warped += 1; worst = max(worst, abs(r))
        span = rows[-1]['T'] - rows[0]['T'] if rows else 0
        wpct = warped / nr * 100 if nr else float('nan')
        wmin = warped / (span / 60.0) if span > 0 else float('nan')
        out['instances'][k].update({'clock_mean_ms': mean(e) if e else None, 'clock_p95_ms': pct([abs(x) for x in e], .95) if e else None,
                                    'clock_change05_p95_ms': pct(ch, .95) if ch else None, 'clock_jumps': jumps,
                                    'warped_pct': wpct, 'warped_per_min': wmin, 'warp_max': worst})
        if k == h:
            print(f'{k:8s}  (host)                                       {jumps:5d}          {wpct:5.1f} % ({wmin:4.0f}/min, max {worst * 100:3.0f} %)')
        else:
            print(f'{k:8s}  {mean(e):+8.1f} {pct([abs(x) for x in e], .95):8.1f} {pct(ch, .95):10.2f}    {jumps:5d}'
                  f'          {wpct:5.1f} % ({wmin:4.0f}/min, max {worst * 100:3.0f} %)')

    pooled = {True: {'e': [], 'al': [], 'alms': [], 'ac': [], 'hd': [], 'p05': 0, 'p2': 0, 'dur': 0.0},
              False: {'e': [], 'al': [], 'alms': [], 'ac': [], 'hd': [], 'p05': 0, 'p2': 0, 'dur': 0.0}}
    hdr = (f'owner->viewer  part  n      err med/p95/max (m)     along mean (ms) sd   across p95  head p95   pops/min >0.5 >2')
    print(hdr)
    for a in names:
        tra = trs[a]
        if len(tra) < 10:
            continue
        ts = [x[0] for x in tra]
        for b in names:
            if b == a or not data[b]:
                continue
            rows = data[b]
            ncar = max(len(r['cars']) for r in rows)
            best = None
            for j in range(ncar):
                d = []
                for r in rows[::10]:
                    c = r['cars'][j] if j < len(r['cars']) else None
                    if not c or c['local']:
                        continue
                    s = at(tra, ts, r['T'])
                    if s:
                        d.append(math.dist(s[0], c['p']))
                if len(d) > 5:
                    m = pct(d, .5)
                    if best is None or m < best[0]:
                        best = (m, j)
            if not best:
                print(f'{a}->{b}: car not found'); continue
            j = best[1]
            part = {True: {'e': [], 'al': [], 'alms': [], 'ac': [], 'hd': [], 'p05': 0, 'p2': 0, 'dur': 0.0},
                    False: {'e': [], 'al': [], 'alms': [], 'ac': [], 'hd': [], 'p05': 0, 'p2': 0, 'dur': 0.0}}
            hist = []          # the drawn car's last frames: (T, p)
            for r in rows:
                c = r['cars'][j] if j < len(r['cars']) else None
                if not c or c['local']:
                    hist = []; continue
                me = next((x for x in r['cars'] if x and x['local']), None)
                near = me is not None and math.dist(me['p'], c['p']) <= near_m
                s = at(tra, ts, r['T'])
                hist.append((r['T'], c['p'], r['wdt']))
                if len(hist) > 3:
                    hist.pop(0)
                if not s or not s[1]:
                    continue
                p, vel, f = s
                sp = math.hypot(vel[0], vel[1])
                if sp < MIN_V:
                    continue
                P = part[near]
                e = tuple(c['p'][i] - p[i] for i in range(3))
                ux, uy = vel[0] / sp, vel[1] / sp
                al = e[0] * ux + e[1] * uy
                ac = -e[0] * uy + e[1] * ux
                P['e'].append(math.sqrt(sum(x * x for x in e)))
                P['al'].append(al); P['alms'].append(al / sp * 1000.0); P['ac'].append(abs(ac))
                hs, ho = heading(c['f']), heading(f)
                if hs is not None and ho is not None:
                    P['hd'].append(math.degrees(angdiff(hs, ho)))
                if len(hist) == 3:
                    # a frame's move against the one before, scaled by the viewer's world steps (the cars move by
                    # those; scaled by the wall clock, the frame pacer's 16.7 / 33 ms steps would read as pops)
                    (t0, p0, w0), (t1, p1, w1), (t2, p2, w2) = hist
                    if t1 > t0 and t2 > t1 and t2 - t0 < 0.2 and w1 and w2 and w1 > 0 and w2 > 0:
                        P['dur'] += t2 - t1
                        k = w2 / w1
                        dev = math.sqrt(sum(((p2[i] - p1[i]) - (p1[i] - p0[i]) * k) ** 2 for i in range(3)))
                        if dev > 0.5: P['p05'] += 1
                        if dev > 2.0: P['p2'] += 1
            for nr in (True, False):
                P, Q = part[nr], pooled[nr]
                for key in ('e', 'al', 'alms', 'ac', 'hd'):
                    Q[key] += P[key]
                for key in ('p05', 'p2', 'dur'):
                    Q[key] += P[key]
                if not P['e']:
                    continue
                mins = P['dur'] / 60.0 if P['dur'] > 0 else float('nan')
                rec = {'owner': a, 'viewer': b, 'near': nr, 'n': len(P['e']), 'err_med': pct(P['e'], .5), 'err_p95': pct(P['e'], .95),
                       'err_max': max(P['e']), 'along_mean': mean(P['al']), 'along_mean_ms': mean(P['alms']), 'along_sd': sd(P['al']),
                       'across_p95': pct(P['ac'], .95), 'head_p95': pct(P['hd'], .95) if P['hd'] else None,
                       'pops05_min': P['p05'] / mins if mins == mins else None, 'pops2_min': P['p2'] / mins if mins == mins else None}
                out['pairs'].append(rec)
                print(f'{a}->{b:6s}  {"near" if nr else "far ":4s}  {len(P["e"]):5d}  {pct(P["e"], .5):6.2f} {pct(P["e"], .95):6.2f} {max(P["e"]):7.2f}'
                      f'   {mean(P["al"]):+6.2f} ({mean(P["alms"]):+5.0f}) {sd(P["al"]):5.2f}   {pct(P["ac"], .95):6.2f}'
                      f'   {pct(P["hd"], .95) if P["hd"] else float("nan"):6.2f}   {rec["pops05_min"] or 0:6.1f} {rec["pops2_min"] or 0:5.1f}')
    for nr in (True, False):
        Q = pooled[nr]
        if not Q['e']:
            continue
        mins = Q['dur'] / 60.0 if Q['dur'] > 0 else float('nan')
        rec = {'n': len(Q['e']), 'err_med': pct(Q['e'], .5), 'err_p95': pct(Q['e'], .95), 'err_max': max(Q['e']),
               'along_mean': mean(Q['al']), 'along_mean_ms': mean(Q['alms']), 'along_sd': sd(Q['al']),
               'across_p95': pct(Q['ac'], .95), 'head_p95': pct(Q['hd'], .95) if Q['hd'] else None,
               'pops05_min': Q['p05'] / mins if mins == mins else None, 'pops2_min': Q['p2'] / mins if mins == mins else None}
        out['pooled']['near' if nr else 'far'] = rec
        print(f'ALL      {"near" if nr else "far ":4s}  {rec["n"]:5d}  {rec["err_med"]:6.2f} {rec["err_p95"]:6.2f} {rec["err_max"]:7.2f}'
              f'   {rec["along_mean"]:+6.2f} ({rec["along_mean_ms"]:+5.0f}) {rec["along_sd"]:5.2f}   {rec["across_p95"]:6.2f}'
              f'   {rec["head_p95"] if rec["head_p95"] is not None else float("nan"):6.2f}   {rec["pops05_min"] or 0:6.1f} {rec["pops2_min"] or 0:5.1f}')
    if jout:
        with open(jout, 'w') as f:
            json.dump(out, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
