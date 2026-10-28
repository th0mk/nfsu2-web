#!/bin/bash
# The preshader evaluator against the dev oracle (docs/mw/fx-preshader.md
# §6.4): Microsoft's d3dx9_26.dll under Unicorn (tools/mw/fxoracle.c) on the
# same cases as implementation A (runtime/mw/fxpres.c via
# tools/mw/fxpresrun.c), compared by tools/mw/fxpres_compare.py.
#
#   tools/mw/fxoracle.sh [--quick]
#
# NFSMW_D3DX names d3dx9_26.dll (its SHA-256 must be game.conf's
# D3DX_SHA256); Unicorn comes from UNICORN_INC and UNICORN_LIB, or the first
# unicorn-2.1.4 found next to the checkout (../nsfu2/work/deps,
# ../../nsfu2/work/deps, ../../work/deps).  The effects come from
# tools/fxres.py into out/mw/fx/res (tools/mw/fxdump.sh says how).
#   --quick   fewer cases (as the node test runs it)
# FXRES and FXOUT move the effects (default out/mw/fx/res) and the results
# (default out/mw/fx/oracle).  Runs: the interpreter cases, the mutations,
# the effect rounds (Begin with D3DXFX_DONOTSAVESTATE and with 0), and the
# same rounds on patched copies of the effects (fxpres_cases.py patch: the
# input table's padding; mov made rcp, rsq, neg, frc, exp, log, sin).
# Test-only binary: Unicorn is GPLv2 and the DLL a local dev oracle; neither
# is ever linked into the game or the app, and everything this writes stays
# under out/mw/fx/ (ignored).
set -e -o pipefail
cd "$(dirname "$0")/../.."
ROOT="$PWD"
QUICK=""
[ "${1:-}" = "--quick" ] && QUICK=1
DLL="${NFSMW_D3DX:?set NFSMW_D3DX to the path of d3dx9_26.dll}"
RES="${FXRES:-out/mw/fx/res}"; OUT="${FXOUT:-out/mw/fx/oracle}"; BIN="$OUT/bin"
ls "$RES"/*.fxo > /dev/null 2>&1 || { echo "no effects in $RES: run tools/fxres.py <speed.exe> $RES first"; exit 1; }
UDEP=""
for d in "$ROOT/../nsfu2/work/deps/unicorn-2.1.4" "$ROOT/../../nsfu2/work/deps/unicorn-2.1.4" "$ROOT/../../work/deps/unicorn-2.1.4"; do
  [ -d "$d" ] && { UDEP="$(cd "$d" && pwd)"; break; }
done
UNICORN_INC="${UNICORN_INC:-${UDEP:+$UDEP/include}}"; UNICORN_LIB="${UNICORN_LIB:-${UDEP:+$UDEP/lib/libunicorn.a}}"
[ -d "$UNICORN_INC" ] && [ -f "$UNICORN_LIB" ] || { echo "needs Unicorn 2.1.x: UNICORN_INC and UNICORN_LIB"; exit 1; }
mkdir -p "$BIN" "$OUT"
CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Werror -Iruntime/mw"
clang $CFLAGS -I"$UNICORN_INC" -o "$BIN/fxoracle" tools/mw/fxoracle.c runtime/mw/fx2.c "$UNICORN_LIB" -lpthread -lm
clang $CFLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -o "$BIN/fxpresrun" tools/mw/fxpresrun.c \
  runtime/mw/fxpres.c runtime/mw/fxmodel.c runtime/mw/fx2.c -lm
PY=.venv/bin/python; [ -x "$PY" ] || PY=python3
if [ -n "$QUICK" ]; then VMARGS="--mw 1 --synth 1 --random 300 --corners 6"; RARGS="--rounds 2"; COPIES=2
else VMARGS="--mw 4 --synth 3 --random 2000"; RARGS="--rounds 6"; COPIES=20; fi
# the interpreter alone: every opcode, form and operand kind, the loader, every preshader of the 31
$PY -I tools/mw/fxpres_cases.py vm "$RES" "$OUT/cases.txt" $VMARGS
"$BIN/fxpresrun" vm "$OUT/cases.txt" "$OUT/a_vm.txt"
"$BIN/fxoracle" --dll "$DLL" vm "$OUT/cases.txt" "$OUT/o_vm.txt"
echo "vm: A against d3dx9_26's interpreter (0x4E8817) in isolation"
$PY -I tools/mw/fxpres_compare.py vm --kinds "$OUT/a_vm.txt" "$OUT/o_vm.txt"
# the loader on corrupted copies of every preshader
$PY -I tools/mw/fxpres_cases.py mutate "$RES" "$OUT/mut.txt" --copies $COPIES
"$BIN/fxpresrun" vm "$OUT/mut.txt" "$OUT/a_mut.txt"
"$BIN/fxoracle" --dll "$DLL" vm "$OUT/mut.txt" "$OUT/o_mut.txt"
echo "mutated: A against the DLL's loader check (0x4E9146) and interpreter"
$PY -I tools/mw/fxpres_compare.py vm "$OUT/a_mut.txt" "$OUT/o_mut.txt"
# the effect path: every pass of every technique of the 31 after SetValue rounds, at precision 24, 53 and 64
$PY -I tools/mw/fxpres_cases.py rounds "$RES" "$OUT/rounds.txt" $RARGS
"$BIN/fxpresrun" effect "$OUT/rounds.txt" "$OUT/a_eff.txt" "$RES"/*.fxo
"$BIN/fxoracle" --dll "$DLL" effect "$OUT/rounds.txt" "$OUT/o_eff.txt" "$RES"/*.fxo
echo "effect: A against D3DXCreateEffect, SetValue and BeginPass on a recording device"
$PY -I tools/mw/fxpres_compare.py effect "$OUT/a_eff.txt" "$OUT/o_eff.txt"
# the same with Begin(0), which records a state block first (MW passes 0 at 10 of its 41 Begin sites)
"$BIN/fxoracle" --dll "$DLL" --begin-flags 0 effect "$OUT/rounds.txt" "$OUT/o_eff0.txt" "$RES"/*.fxo
echo "effect, Begin(0): the same through D3DX's state saving"
$PY -I tools/mw/fxpres_compare.py effect "$OUT/a_eff.txt" "$OUT/o_eff0.txt"
# the effect path on values MW's own preshaders never give it (review R2-01, R2-02): copies of the 31 whose
# preshaders read the zeroed padding of D3DX's input table, or whose movs are made rcp, rsq, neg, frc, exp, log
# (one component) or sin, so that the gather, the precision and the scatter's conversions (FLOAT rounded to single,
# INT truncated) meet rounded, inexact, huge and NaN values; game data, under $OUT
for spec in "pad" "op 103" "op 107" "op 101" "op 104" "op 105" "op 106 --n1" "op 108"; do
  set -- $spec
  dir="$OUT/fx_$1${2:+_$2}"; flag=""; [ "${2:-}" = 108 ] && flag="--fsin-inf"
  rm -rf "$dir"
  $PY -I tools/mw/fxpres_cases.py patch "$RES" "$dir" $spec
  "$BIN/fxpresrun" effect "$OUT/rounds.txt" "$dir/a_eff.txt" "$dir"/*.fxo
  "$BIN/fxoracle" --dll "$DLL" effect "$OUT/rounds.txt" "$dir/o_eff.txt" "$dir"/*.fxo
  if ! $PY -I tools/mw/fxpres_compare.py effect $flag "$dir/a_eff.txt" "$dir/o_eff.txt" > "$dir/cmp.txt"; then
    echo "effect, $spec: A against D3DX on the copies"; cat "$dir/cmp.txt"; exit 1
  fi
  echo "effect, $spec: $(tail -1 "$dir/cmp.txt") ($(grep -c ' state ' "$dir/a_eff.txt") values)"
done
