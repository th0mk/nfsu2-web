#!/bin/bash
set -e
ROOT="${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
cd ~/nfsu2-linux-build
./nfsu2_bench "$NFSU2_GAME_DIR/SPEED2.EXE" "$ROOT/tests/scripts/race_long.txt" 95 >perf/glocal.out 2>perf/glocal.err
python3 - <<'PY'
import re
from pathlib import Path
def avg(path):
    xs=[]
    for line in Path(path).read_text(errors='replace').splitlines():
        if 'flow=6' not in line: continue
        m=re.search(r'gcpu=([0-9.]+)ms', line)
        if m: xs.append(float(m.group(1)))
    if not xs: return None,0
    return sum(xs)/len(xs), len(xs)
for tag,p in [('base','perf/race.err'),('glocal','perf/glocal.err')]:
    a,n=avg(p)
    print(f'{tag}: gcpu_avg={a:.3f} ms over {n}' if a else f'{tag}: no samples')
PY
