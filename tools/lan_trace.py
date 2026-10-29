#!/usr/bin/env python3
"""lan_trace.py -- compare how LAN race instances see each other's cars.

  tools/lan_trace.py <run dir>            (reads <run dir>/i*/cars.csv)
  tools/lan_trace.py a.csv b.csv [...]

Each instance writes NFSU2_CAR_TRACE (runtime/autotest.c): per frame the
wall-clock time (CLOCK_REALTIME, the same clock for every instance on this
Mac; in the browser a worker's time origin plus its monotonic clock, so
instances started on either side of a correction of the system clock -- WSL
makes them -- are that far apart: a WorldTimer offset of a second or more
in the last lines, and ~50 m errors for every pair with that instance, while
the game's own [racenet] "their race clock ... off ours" says tens of ms),
the frame cap, WorldTimer and every race car with a flag for the local
player's car.  Frames whose top package is not the race HUD (pre-race
screen, results) are left out when the trace says so.  For every pair (owner A, viewer B) this tool finds A's car in
B's car list (the index whose positions stay closest to A's own trace) and
reports, over the time both are racing and A's car moves (> 2 m/s):

  err     distance between where B shows A's car and where A had it at the
          same instant (median / 95th percentile / max, metres)
  lag     err / speed: the display delay that would explain it (ms)
  jumps   frames in which B's view of A's car moved more than
          max(2 m, 3 x speed x frame time) -- a teleport / rubber band;
          compare with the jumps in A's own trace (the game resetting a
          crashed car onto the track is a real teleport, seen by everyone).
          The frame time is the longer of the wall-clock step and the game
          time the frame simulated (B's WorldTimer: the next row's minus
          this row's, as a row's cars are logged after the frame's
          simulation and its WorldTimer before it).  After a stall of an
          instance (for example 106 and 208 ms) a network race's
          world clock catches up in one frame and every car moves that far,
          which is no jump.  A jump within BACK_S seconds after a frame in
          which B's WorldTimer went back by more than 50 ms (the client's world
          clock set back to the host's after stalls of both instances:
          -217.8 ms, then the shown car 5.5 m back) is
          listed apart ("after a clock correction") and not counted; so is a
          step of the game's far-car mode (B shows A's car with a speed under
          0.5 m/s while A moves: from about 145 m between the cars the game
          updates a remote car in steps every 0.1 s; seen at 143-149 m), and a jump within STALL_S
          seconds after a frame of A or B that took longer than STALL_MS (a
          stalled instance: its car stops for the others, or it stops showing
          them, and the game corrects afterwards; on a loaded Mac, load 40-70,
          115-270 ms, then a 2.3 m snap).  Both are
          printed, so a run on a busy machine still shows them
  fps     frames per second of each instance while racing, and its cap

and the same split by the distance between B's own car and A's car: within
NEAR metres (racing together) and farther.  The game treats a car far from
the player differently: from about 150-200 m its shown speed is 0 and it
moves in steps of several metres every 0.1 s (netfix/session6 race 1, at
180-207 m), and beyond 300 m it is 1-2.5 m off (netfix/lanrace-final; E2's
150-300 m corrections) -- seen in the traces, not traced in the code.
tests/net/lan_race.sh judges the near part.

It also prints each instance's own racing time span and frame rate, and how
far each instance's WorldTimer is from the first instance's (the race host
in the LAN scripts: clients synchronise their world clock to it).
"""
import csv, glob, math, os, sys
from bisect import bisect_left

NEAR = 150.0                                  # metres between the viewer's car and the shown car
BACK = -0.050                                 # s: a frame whose world step is below this set the clock back
BACK_S = 1.0                                  # s after it: a jump there is the correction showing
FAR_V = 0.5                                   # m/s: a shown speed below this while the owner moves = the far-car mode
STALL_MS = 100.0                              # a frame longer than this: the instance stalled
STALL_S = 1.0                                 # s after the stall's end: a jump there is its correction

def stall_ends(times):
    """wall times at which a frame of an instance ended after more than STALL_MS"""
    return [times[i] for i in range(1, len(times)) if (times[i] - times[i - 1]) * 1000.0 > STALL_MS]

def after(ends, t):
    i = bisect_left(ends, t + 1e-9)
    return i > 0 and t - ends[i - 1] <= STALL_S

def results_frame(path):
    """first frame of the results screen in the instance's log, if any"""
    log = os.path.join(os.path.dirname(path), 'log.txt')
    try:
        with open(log, errors='replace') as f:
            for line in f:
                if line.startswith('[fng] f=') and ('screen UI_PostRaceResults.fng' in line
                                                    or 'screen PostRace_Results.fng' in line):   # UG2, MW
                    return int(line.split('f=')[1].split()[0])
    except OSError:
        pass
    return None

def load(path):
    rows = []
    stop = results_frame(path)
    with open(path) as f:
        for r in csv.DictReader(f):
            try:
                t = float(r['t']); n = int(r['n'])
            except (ValueError, KeyError, TypeError):
                continue
            if stop is not None and int(r['frame']) >= stop:
                break                         # the race is over (results screen)
            cars = []
            for i in range(n):
                try:
                    cars.append((r[f'c{i}local'] == '1', float(r[f'c{i}x']), float(r[f'c{i}y']),
                                 float(r[f'c{i}z']), float(r[f'c{i}v'])))
                except (ValueError, KeyError, TypeError):
                    cars.append(None)
            if r.get('hud') == '0':
                continue                      # results / post-race scene, not racing
            rows.append((t, float(r['cap'] or 0), int(r['worldtimer'] or 0), cars))
    return rows

WT_HZ = 4000.0                                # WorldTimer ticks per second

def world_steps(rows):
    """per row: the game time (s) its frame simulated"""
    return [(rows[i + 1][2] - rows[i][2]) / WT_HZ if i + 1 < len(rows) else 0.0 for i in range(len(rows))]

def own_track(rows):
    tr = []
    ws = world_steps(rows)
    for i, (t, cap, wt, cars) in enumerate(rows):
        for c in cars:
            if c and c[0]:
                tr.append((t, c[1], c[2], c[3], c[4], ws[i])); break
    return tr

def interp(tr, ts, t):
    i = bisect_left(ts, t)
    if i <= 0 or i >= len(tr): return None
    a, b = tr[i - 1], tr[i]
    if b[0] - a[0] > 0.5: return None
    k = (t - a[0]) / (b[0] - a[0]) if b[0] > a[0] else 0
    return tuple(a[j] + k * (b[j] - a[j]) for j in (1, 2, 3, 4))

def pct(v, p):
    if not v: return float('nan')
    v = sorted(v); return v[min(len(v) - 1, int(p * (len(v) - 1) + 0.5))]

def main(argv):
    if len(argv) == 1 and os.path.isdir(argv[0]):
        files = sorted(glob.glob(os.path.join(argv[0], 'i*', 'cars.csv')))
    else:
        files = argv
    if len(files) < 2:
        print('need two or more traces'); return 2
    data = {os.path.basename(os.path.dirname(f)) or f: load(f) for f in files}
    tracks = {k: own_track(v) for k, v in data.items()}
    print('instance  rows  racing span (s)  fps   cap (fps)  own car samples')
    for k, rows in data.items():
        if not rows: print(f'{k:8s}  no rows'); continue
        span = rows[-1][0] - rows[0][0]
        caps = sorted(set(round(1 / r[1]) if r[1] > 0 else 0 for r in rows))
        print(f'{k:8s}  {len(rows):5d}  {span:8.1f}  {len(rows) / span if span > 0 else 0:6.1f}  {caps}  {len(tracks[k])}')
    worst = 0.0
    def own_jumps(tra):
        n = 0
        for p, q in zip(tra, tra[1:]):
            dt = max(q[0] - p[0], q[5]); mv = math.dist(p[1:4], q[1:4])
            if dt > 0 and q[4] > 2.0 and mv > max(2.0, 3 * q[4] * dt): n += 1
        return n
    for a, tra in tracks.items():
        if len(tra) < 10: continue
        print(f'{a} own car: {own_jumps(tra)} jumps in its owner\'s own trace (resets onto the track)')
        ts = [p[0] for p in tra]
        for b, rows in data.items():
            if b == a or not rows: continue
            ncar = max(len(r[3]) for r in rows)
            best = None
            for j in range(ncar):
                d = []
                for t, cap, wt, cars in rows[::10]:
                    if j >= len(cars) or not cars[j] or cars[j][0]: continue
                    p = interp(tra, ts, t)
                    if p: d.append(math.dist(p[:3], cars[j][1:4]))
                if len(d) > 5:
                    m = sorted(d)[len(d) // 2]
                    if best is None or m < best[0]: best = (m, j)
            if not best:
                print(f'{a} -> {b}: car not found'); continue
            j = best[1]
            err, lag, jumps, n = [], [], 0, 0
            part = {True: [[], 0, 0, 0, 0], False: [[], 0, 0, 0, 0]}   # near / far: errors, jumps, after a correction, far-car steps, after a stall
            stalls = sorted(stall_ends(ts) + stall_ends([r[0] for r in rows]))
            prev = None
            ws = world_steps(rows)
            back_t = None
            for i, (t, cap, wt, cars) in enumerate(rows):
                if ws[i] < BACK: back_t = t
                if j >= len(cars) or not cars[j]: prev = None; continue
                c = cars[j]
                me = next((x for x in cars if x and x[0]), None)
                near = me is not None and math.dist(me[1:4], c[1:4]) <= NEAR
                p = interp(tra, ts, t)
                if p and p[3] > 2.0:
                    e = math.dist(p[:3], c[1:4]); err.append(e); lag.append(e / p[3] * 1000); n += 1
                    part[near][0].append(e)
                if prev and p and p[3] > 2.0:
                    dt = max(t - prev[0], ws[i]); mv = math.dist(prev[1], c[1:4])
                    if dt > 0 and mv > max(2.0, 3 * p[3] * dt):
                        if back_t is not None and t - back_t <= BACK_S: part[near][2] += 1
                        elif len(c) > 4 and abs(c[4]) < FAR_V: part[near][3] += 1
                        elif after(stalls, t): part[near][4] += 1
                        else: jumps += 1; part[near][1] += 1
                prev = (t, c[1:4])
            worst = max(worst, pct(err, 0.95) if err else 0)
            print(f'{a} car as seen by {b} (index {j}): {n} samples, err median {pct(err, .5):.2f} m, '
                  f'p95 {pct(err, .95):.2f} m, max {max(err) if err else float("nan"):.2f} m; '
                  f'lag median {pct(lag, .5):.0f} ms, p95 {pct(lag, .95):.0f} ms; jumps {jumps}')
            for k, label in ((True, f'within {NEAR:.0f} m of the viewer'), (False, 'farther')):
                e, jn, jb, jf, js = part[k]
                print(f'    {label}: {len(e)} samples, err median {pct(e, .5):.2f} m, p95 {pct(e, .95):.2f} m; '
                      f'jumps {jn}' + (f' (+{jb} after a clock correction, not counted)' if jb else '')
                      + (f' (+{jf} far-car steps, shown speed 0, not counted)' if jf else '')
                      + (f' (+{js} after a stall over {STALL_MS:.0f} ms, not counted)' if js else ''))
    for k, rows in data.items():
        g = [(rows[i][0] - rows[i - 1][0]) * 1000.0 for i in range(1, len(rows))]
        g = [x for x in g if x > STALL_MS]
        if g: print(f'{k} frames longer than {STALL_MS:.0f} ms while racing: {len(g)}, longest {max(g):.0f} ms')
        b = [x for x in world_steps(rows) if x < BACK]
        if b: print(f'{k} WorldTimer set back by more than {-BACK * 1000:.0f} ms in {len(b)} frame(s), '
                    f'largest {min(b) * 1000:.1f} ms')
    # world clocks: every instance against the first one (the race host in
    # the LAN scripts; clients slave their WorldTimer to it, 0x5891B0)
    keys = list(data)
    if keys and data[keys[0]]:
        h = [(r[0], r[2]) for r in data[keys[0]]]
        ht = [x[0] for x in h]
        for k in keys[1:]:
            d = []
            for t, cap, wt, cars in data[k]:
                i = bisect_left(ht, t)
                if 0 < i < len(h) and h[i][0] - h[i - 1][0] < 0.5:
                    a, b = h[i - 1], h[i]
                    d.append((wt - (a[1] + (b[1] - a[1]) * (t - a[0]) / (b[0] - a[0]))) / 4000.0)
            if d:
                print(f'{k} WorldTimer - {keys[0]} WorldTimer at the same instant: median {pct(d, .5) * 1000:+.1f} ms, '
                      f'p5 {pct(d, .05) * 1000:+.1f} ms, p95 {pct(d, .95) * 1000:+.1f} ms')
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
