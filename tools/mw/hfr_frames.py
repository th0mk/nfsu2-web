#!/usr/bin/env python3
"""Most Wanted's frame times in a world (TRACKER M7-32), from runtime/mw/framerate_mw.c's NFSU2_HFR_FT=1 lines.

    tools/mw/hfr_frames.py RUN.log [RUN.log ...] [--skip S] [--csv OUT.csv]

Each [mwft] line holds 100 frames: iv (a frame's start to the next frame's, 0.01 ms), wk (its work before the
next frame's pacer: the game's frame and the present's wait), sl (the slots the next frame stood for) and st (the
steps the simulation ran in it).  Per run: the frames, the content rate, the mean frame rate, the intervals' and
the work's median, 99th and 99.9th percentiles and largest, the late frames (an interval of 1.5 slots or more),
the steps a frame ran (0, 1, 2, 3+), and the CPU share of the frame work (work over interval).  --skip leaves out
the first S seconds of frames (the world's start); --csv writes every run's frames (run, frame, iv, wk, sl, st).
"""
import re, sys

FT = re.compile(r'\[mwft\] n=(\d+) rate=(\d+) iv=([\d,]+) wk=([\d,]+) sl=(\d+) st=(\d+)')


def load(path):
    rate, rows = 0, []
    for line in open(path, errors='replace'):
        m = FT.search(line)
        if not m:
            continue
        rate = int(m.group(2))
        iv = [int(x) / 100.0 for x in m.group(3).split(',')]
        wk = [int(x) / 100.0 for x in m.group(4).split(',')]
        for k in range(len(iv)):
            rows.append((iv[k], wk[k], int(m.group(5)[k]), int(m.group(6)[k])))
    return rate, rows


def pct(xs, p):
    if not xs:
        return 0.0
    s = sorted(xs)
    return s[min(len(s) - 1, int(p / 100.0 * len(s)))]


def main():
    args, skip, csv = [], 0.0, None
    a = sys.argv[1:]
    while a:
        x = a.pop(0)
        if x == '--skip':
            skip = float(a.pop(0))
        elif x == '--csv':
            csv = a.pop(0)
        else:
            args.append(x)
    out = open(csv, 'w') if csv else None
    if out:
        out.write('run,frame,iv_ms,wk_ms,slots,steps\n')
    for path in args:
        rate, rows = load(path)
        t, k0 = 0.0, 0
        while k0 < len(rows) and t < skip:
            t += rows[k0][0] / 1000.0
            k0 += 1
        rows = rows[k0:]
        if not rows:
            print(f"{path}: no [mwft] frames")
            continue
        iv = [r[0] for r in rows]
        wk = [r[1] for r in rows]
        period = 1000.0 / rate if rate else 0.0
        late = sum(1 for r in rows if r[2] >= 2 or (period and r[0] >= 1.5 * period))
        st = [0, 0, 0, 0]
        for r in rows:
            st[min(r[3], 3)] += 1
        total = sum(iv)
        print(f"{path}: rate {rate}, {len(rows)} frames over {total / 1000.0:.1f} s, {1000.0 * len(rows) / total:.2f} fps; "
              f"interval ms median {pct(iv, 50):.2f} p99 {pct(iv, 99):.2f} p99.9 {pct(iv, 99.9):.2f} max {max(iv):.2f}; "
              f"work ms median {pct(wk, 50):.2f} mean {sum(wk) / len(wk):.2f} p99 {pct(wk, 99):.2f} max {max(wk):.2f} "
              f"({100.0 * sum(wk) / total:.0f} % of the time); late {late} ({100.0 * late / len(rows):.2f} %); "
              f"steps a frame 0:{st[0]} 1:{st[1]} 2:{st[2]} 3+:{st[3]}")
        if out:
            for k, r in enumerate(rows):
                out.write(f"{path},{k},{r[0]:.2f},{r[1]:.2f},{r[2]},{r[3]}\n")


if __name__ == '__main__':
    main()
