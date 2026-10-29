#!/usr/bin/env python3
"""Most Wanted lockstep A/B at two frame rates (TRACKER M7-21, M7-22).

    tools/mw/hfr_ab.py REF.log RUN.log [RUN.log ...]

Each log is a run of tests/mw/scripts/hfr_race.txt with NFSMW_HFR_TRACE=1: runtime/mw/career_mw.c writes an
[hfrtr] line every 0.25 s of simulation time (steps x step), so runs at different rates line up on the same
game times after GO (the player's race timer starting).  Compared, per run against the reference, over the game times both reached:
  - the player's car (the game's AI drives it): position apart (m) and speed (m/s) at 10, 30, 60, 90 s and the
    largest gap;
  - the racers' share of the race done (GRacerInfo +0x18) at the end;
  - the other cars (the vehicles of the list at 0x92D0DC after the racers, i.e. traffic): how many were seen,
    how many stopped (under 0.5 m/s), and how many of those stayed stopped for 10 s or more, and for how long the
    longest stop lasted: the traffic freeze above about 40 Hz (WidescreenFixesPack #1218) shows as stops that never
    end.
"""
import re, sys

TR = re.compile(r'^\[hfrtr\] f=(\d+) t=([\d.]+) st=(\d+)(.*)$')


BAD = re.compile(r'\[expect\] FAIL|waitexpr TIMEOUT')


def invalid(path):
    """the first line that makes a run no evidence: a failed expect, or (older runs, before the script's
    expects) a timed-out waitexpr, after which the script went on as if GO had come"""
    for line in open(path, errors='replace'):
        if BAD.search(line):
            return line.strip()
    return None


def load(path):
    rows = {}
    for line in open(path, errors='replace'):
        m = TR.match(line.strip())
        if not m:
            continue
        t = round(float(m.group(2)) * 4) / 4
        rest, _, vehs = m.group(4).partition(' |')
        racers = [float(x) for x in re.findall(r' r\d+=([-\d.]+)', rest)]
        cars = {}
        for sid, x, y, z, v in re.findall(r' ([0-9a-f]+):([-\d.]+),([-\d.]+),([-\d.]+),([-\d.]+)', rest):
            cars[sid] = (float(x), float(y), float(z), float(v))
        veh = {vid: float(v) for vid, v in re.findall(r' ([0-9a-f]+)=([-\d.]+)', vehs)}
        rows[t] = {'racers': racers, 'cars': cars, 'veh': veh}
    return rows


def player(row):
    c = list(row['cars'].values())
    return c[0] if c else None


def traffic(rows):
    """per non-racer car: its stops (start, length) under 0.5 m/s; a car is a racer if it is among the first n
    entries of the list, n = the racers"""
    ts = sorted(rows)
    if not ts:
        return {}
    nr = max(len(r['racers']) for r in rows.values())
    seen, stop_at, stops = set(), {}, {}
    for t in ts:
        ids = list(rows[t]['veh'])
        for sid in ids[nr:]:
            v = rows[t]['veh'][sid]
            seen.add(sid)
            if abs(v) < 0.5:
                stop_at.setdefault(sid, t)
            elif sid in stop_at:
                st = stop_at.pop(sid)
                stops.setdefault(sid, []).append((st, t - st))
        for sid in list(stop_at):
            if sid not in rows[t]['veh']:       # the car went (despawned) while stopped
                st = stop_at.pop(sid)
                stops.setdefault(sid, []).append((st, t - st))
    end = ts[-1]
    for sid, st in stop_at.items():
        stops.setdefault(sid, []).append((st, end - st, 'open'))
    return {'seen': len(seen), 'stops': stops, 'end': end}


def tsum(tr):
    stops = [s for v in tr['stops'].values() for s in v]
    long_ = [s for s in stops if s[1] >= 10.0]
    opened = [s for s in stops if len(s) > 2 and s[1] >= 10.0]
    longest = max((s[1] for s in stops), default=0.0)
    return (f"traffic: {tr['seen']} cars, {len(tr['stops'])} stopped, {len(stops)} stops, {len(long_)} of 10 s or more "
            f"({len(opened)} still stopped at the end), longest {longest:.1f} s")


def main():
    bad = invalid(sys.argv[1])
    if bad:
        sys.exit(f"ref {sys.argv[1]}: not a valid run: {bad}")
    ref = load(sys.argv[1])
    print(f"ref {sys.argv[1]}: {len(ref)} samples to t={max(ref) if ref else 0:.2f}")
    if ref:
        print('  ' + tsum(traffic(ref)))
    for path in sys.argv[2:]:
        bad = invalid(path)
        if bad:
            print(f"run {path}: left out, not a valid run: {bad}")
            continue
        run = load(path)
        common = sorted(set(ref) & set(run))
        print(f"run {path}: {len(run)} samples, {len(common)} in common")
        if not common:
            continue
        gaps = []
        for t in common:
            a, b = player(ref[t]), player(run[t])
            if a and b:
                d = ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2) ** 0.5
                gaps.append((t, d, b[3] - a[3], a[3]))
        for want in (10, 30, 60, 90):
            g = [x for x in gaps if abs(x[0] - want) < 0.01]
            if g:
                t, d, dv, v = g[0]
                print(f"  t={t:5.1f} s: player {d:7.2f} m apart, speed {v:6.2f} m/s, run - ref {dv:+.3f} m/s")
        if gaps:
            w = max(gaps, key=lambda x: x[1])
            print(f"  largest gap {w[1]:.2f} m at t={w[0]:.2f}")
        t = common[-1]
        ra, rb = ref[t]['racers'], run[t]['racers']
        print(f"  racers at t={t:.2f}: ref {' '.join('%.2f' % x for x in ra)} | run {' '.join('%.2f' % x for x in rb)}")
        print('  ' + tsum(traffic(run)))


if __name__ == '__main__':
    main()
