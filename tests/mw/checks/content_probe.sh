#!/bin/bash
# Suite check for "content_probe" (tests/mw/scripts/content_probe.txt, TRACKER M4-37): the module under test is named
# (L-020); the five Custom Race track lists were each seen whole (every "N / M" of the list's drawn counter from 1 to
# M, read off the "[fng] ... Track_Select.fng shows" lines as tools/mw/census.py reads them), every wait in time;
# no stall report; the game left by WM_CLOSE.  Prints each list's tracks and how many it offers with no profile
# loaded (its Accept button, label 2b65a216, drawn).
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG content_probe: done$' "$LOG" || fail "the probe did not end"
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
python3 - "$LOG" <<'PY' || exit 1
import re, sys
TITLES = {'3de80a85': 'circuit', 'c2d85652': 'sprint', '136c5c90': 'drag', 'd6d65640': 'lap knockout',
          '3070453a': 'speedtrap'}
seen = {}
for line in open(sys.argv[1], encoding='latin1'):
    if 'Track_Select.fng shows' not in line: continue
    toks = re.findall(r":([0-9a-f]{8})'(\S*)", line)
    title = trk = None; nums = []
    for l, t in toks:            # as tools/mw/census.py: the title, the track's name label, the "N / M" counter
        if t.endswith('track_select') and title is None: title = l
        elif title and trk is None and l != title and t.startswith('Track'): trk = l
        if l == '00000018': nums.append(t)
    if not (title and trk and len(nums) >= 2): continue
    e = seen.setdefault(title, {'n': int(nums[-1]), 'at': {}})
    e['at'].setdefault(int(nums[-2]), set()).add(any(l == '2b65a216' for l, _ in toks))
bad = [t for t in TITLES if t not in seen or set(seen[t]['at']) != set(range(1, seen[t]['n'] + 1))]
if bad or len(seen) != len(TITLES):
    print('lists not seen whole: %s; seen: %s' % (', '.join(TITLES.get(t, t) for t in bad) or '-',
          ', '.join('%s %d of %d' % (TITLES.get(t, t), len(e['at']), e['n']) for t, e in seen.items())))
    sys.exit(1)
tot = sum(seen[t]['n'] for t in TITLES)
print('the 5 track lists seen whole: %s (%d tracks); offered with no profile: %s; ' % (
    ', '.join('%s %d' % (TITLES[t], seen[t]['n']) for t in TITLES), tot,
    ', '.join('%s %d' % (TITLES[t], sum(1 for a in seen[t]['at'].values() if a == {True})) for t in TITLES)), end='')
PY
echo "the game left by WM_CLOSE"
