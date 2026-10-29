#!/bin/bash
# Suite check for "race_kind_end" (tests/mw/scripts/race_kind_end.txt, TRACKER M4-05, M4-06): the module under test
# is named (L-020); every pass picked its track, loaded, reached GO, had the autopilot on, moved (a "where" speed of
# 30 km/h or more) and came to the race's end screen with no racewin (the kind's own rules ended it), every wait in
# time, the race logged with the player as racer 1 at the end; no stall report; the game left by WM_CLOSE.  Prints
# each pass: the end screen, the player's share of the race, finished or not, and race time (racestatus), the top
# speed logged, and what the trace saw of the other racers before the end screen: a finished byte set, with the
# player's share then, and a share that stopped short of 100 % for the trace's last samples (in a lap knockout of 4
# racers, slot 3 stopped at 31.3 % and slot 1 at 65.8 % while the other two drove on: just short of a third and two
# thirds, the ends of laps 1 and 2 if the race has 3, so knocked out there; their finished bytes stayed 0 until the
# race's end set them all).
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG race_kind_end: done$' "$LOG" || fail "the runs did not end"
grep -q 'racewin' "$LOG" && fail "a shortcut finish: these races are driven to their own end"
n=$(grep -c 'LOG race_kind_end: track ' "$LOG")
[ "$n" -gt 0 ] || fail "no pass picked a track"
for m in load racing GO results; do
  k=$(grep -c "LOG race_kind_end: $m\$" "$LOG")
  [ "$k" = "$n" ] || fail "$k of $n passes reached '$m'"
done
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
python3 - "$LOG" <<'PY' || exit 1
import re, sys
passes, cur, screen = [], None, '?'
for line in open(sys.argv[1], encoding='latin1'):
    m = re.match(r'\[fng\] f=\d+ screen (\S+)', line)
    if m: screen = m.group(1)
    if 'LOG race_kind_end: track ' in line:
        cur = {'track': line.split()[-1], 'ai': False, 'v': [], 'tr': [], 'end': None, 'st': None}; passes.append(cur)
    if cur is None: continue
    if re.match(r'\[mwrace\] f=\d+ autopilot on$', line): cur['ai'] = True
    m = re.match(r'\[mwrace\] f=\d+ where .* speed ([0-9.]+) km/h', line)
    if m: cur['v'].append(float(m.group(1)))
    if line.startswith('[trace] ') and cur['end'] is None:
        cur['tr'].append((int(re.search(r' f=(\d+)', line).group(1)), dict(re.findall(r' (\w+)=([0-9.e+-]+|\?)', line))))
    if line.rstrip().endswith('LOG race_kind_end: results'): cur['end'] = screen
    m = re.match(r'\[mwrace\] f=\d+ racestatus flow \d+ racers (\d+) active \d+ won (\d+) player 1: #0 ([0-9.]+)%( finished)? ([0-9.]+)s', line)
    if m and cur['end'] and cur['st'] is None: cur['st'] = m.groups()
bad, out = [], []
for i, p in enumerate(passes, 1):
    v = max(p['v']) if p['v'] else 0.0
    if not p['ai']: bad.append('pass %d: no "autopilot on"' % i)
    if v < 30: bad.append('pass %d: top speed logged %.1f km/h' % (i, v))
    if not p['st']: bad.append('pass %d: no racestatus with the player as racer 1 at the end' % i); continue
    racers, won, pct, fin, t = p['st']
    tr = p['tr']; seen = {}; stop = []
    for f, d in tr:
        for s in '123':
            if d.get('f' + s) == '1' and s not in seen and int(s) < int(racers): seen[s] = (f, d.get('r' + s), d.get('r0'))
    # a racer whose share stopped short of 100 for the trace's last two samples or more (a lap knockout's racer out)
    for s in '123'[:max(0, int(racers) - 1)]:
        k = len(tr) - 1
        while k > 0 and tr[k - 1][1].get('r' + s) == tr[-1][1].get('r' + s): k -= 1
        x = tr[-1][1].get('r' + s, '?') if tr else '?'
        if tr and k <= len(tr) - 3 and x != '?' and float(x) < 99.9:
            stop.append('slot %s at %.1f%% from f=%d (the player then at %.1f%%)' % (s, float(x), tr[k][0],
                                                                                     float(tr[k][1].get('r0', 0))))
    others = '; '.join('slot %s at %s%% (the player at %s%%)' % (s, a, b) for s, (f, a, b) in sorted(seen.items(), key=lambda kv: kv[1][0]))
    out.append('pass %d on %s: %s, the player %s%%%s, race time %ss, won %s, top speed %.1f km/h; %d racers; finished '
               'bytes set before the end: %s; stopped short of the end: %s' % (
                   i, p['track'], p['end'], pct, ' finished' if fin else ' not finished', t, won, v, int(racers),
                   others or 'none', ', '.join(stop) or 'none'))
if bad: print('; '.join(bad)); sys.exit(1)
print('%d races driven to their own end: %s; ' % (len(passes), ' | '.join(out)), end='')
PY
echo "the game left by WM_CLOSE"
