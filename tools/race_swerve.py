#!/usr/bin/env python3
"""race_swerve.py -- script lines for a LAN race test that drives less smoothly than the game's AI alone.

  tools/race_swerve.py SEED [--swerves N] [--hitch]

Prints one line of ';'-separated script lines (each with its time), for web/tools/lan-pair.mjs's --host-arg / --joinN-arg
--after (tools/gen_lan_race.py appends them after the race start).  Every 5-9 s the AI driver gives the wheel up for a
short tap left or right with the throttle held (a player's swerve or lane change; the AI corrects it after), N times
(default 24, about a two-lap circuit).  --hitch: between the swerves the game's thread is blocked 50-120 ms every
0.7-2.5 s (a phone's or a busy browser's long frames; autotest.c's "stall" with a fraction of a second).  Give each
instance its own SEED, or they all swerve at once.

The race netcode's rig (runtime/racecars.c, tools/race_acc.py), e.g. three players 70 +- 15 ms from the server with
500 ms stalls:
  node web/tools/lan-pair.mjs --out <dir> --players 3 --join-arg --confirm-join \\
     --impair leg=70,legjitter=15,stall=0.03,stallms=500,seed=7 \\
     --host-arg --after --host-arg "$(tools/race_swerve.py 11)" \\
     --join2-arg --after --join2-arg "$(tools/race_swerve.py 22)" --join3-arg --after --join3-arg "$(tools/race_swerve.py 33)"
  tools/race_acc.py <dir>
Compare two versions side by side, at the same time, several runs each: one run against another differs as much as
small changes do (the AI's race, crashes, the machine's load).  NFSU2_RACE_CARS=0 (--env) is the older drawing on the
same build.
"""
import random
import sys


def main(argv):
    if not argv or argv[0].startswith('-'):
        print(__doc__)
        return 2
    seed, n, hitch = int(argv[0]), 24, '--hitch' in argv
    if '--swerves' in argv:
        n = int(argv[argv.index('--swerves') + 1])
    rnd = random.Random(seed)
    out = ['+%dms autopilot on' % rnd.randint(6000, 10000)]
    for _ in range(n):
        gap = rnd.randint(5000, 9000)
        t = 0
        while hitch and t + 2600 < gap:
            d = rnd.randint(700, 2500)
            t += d
            out.append('+%dms stall %.3f' % (d, rnd.randint(50, 120) / 1000))
        side = 'LEFT' if rnd.random() < 0.5 else 'RIGHT'
        out += ['+%dms autopilot off' % max(0, gap - t), '+0 down UP', '+0 down %s' % side,
                '+%dms up %s' % (rnd.randint(80, 180), side), '+%dms up UP' % rnd.randint(50, 250), '+0 autopilot on']
    print(';'.join(out))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
