#!/bin/bash
# Profile the Linux Switch harness during Quick Race. Writes report under ~/nfsu2-linux-build/perf/
set -e
OUT="${OUT:-$HOME/nfsu2-linux-build}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
EXE="${1:-${NFSU2_GAME_DIR:?set NFSU2_GAME_DIR to the folder with SPEED2.EXE}/SPEED2.EXE}"
SCRIPT="${2:-$ROOT/tests/scripts/race_long.txt}"
SECS="${3:-100}"
mkdir -p "$OUT/perf"
cd "$OUT"
# Kill any leftover bench so we own the run.
pkill -x nfsu2_bench 2>/dev/null || true
sleep 1
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
# Software GPU path: we want guest CPU hotspots, not GL driver noise.
unset NFSU2_NX_GL
echo "[perf-race] recording $SECS s of $SCRIPT"
perf record -g -o perf/race.perf.data -- ./nfsu2_bench "$EXE" "$SCRIPT" "$SECS" >perf/race.out 2>perf/race.err
echo "[perf-race] record done, exit $?"
# Top symbols (flat, no children): overall and by thread name if available.
perf report -i perf/race.perf.data --stdio --no-children -n --percent-limit 0.3 >perf/race.report.txt 2>/dev/null || true
perf report -i perf/race.perf.data --stdio --children -n --percent-limit 0.5 >perf/race.report-children.txt 2>/dev/null || true
# Race-window only hint: grep status lines with flow=6 (driving).
echo "=== race window gcpu (flow=6) ===" | tee perf/race.summary.txt
grep 'flow=6' perf/race.err | tail -20 | tee -a perf/race.summary.txt
echo "=== top 40 (flat) ===" | tee -a perf/race.summary.txt
head -60 perf/race.report.txt | tee -a perf/race.summary.txt
echo "[perf-race] wrote $OUT/perf/race.summary.txt"
