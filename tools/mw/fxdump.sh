#!/bin/bash
# Most Wanted's compiled effects as text: builds tools/mw/fxdump.c with
# runtime/mw/fx2.c (the parser the runtime uses) and writes the canonical
# dump (docs/mw/fx2-format.md §5) of every effect tools/fxres.py extracted.
#
#   tools/mw/fxdump.sh [--model] [--pres] [--fuzz <copies per file>] [--diff <count>] [--wasm]
#
# out/mw/fx/res/<NAME>.fxo -> out/mw/fx/a/<NAME>.txt.  The tools are built
# with the system clang (-Wall -Wextra -Werror, ASan and UBSan, any report
# fatal) into out/mw/fx/bin/.  It stops on a parse error or a sanitizer
# report and prints the number of UNKNOWN( in each dump that has any (the
# goal is none).
#   --model   also the effect model (docs/mw/fx-model.md): builds
#             tools/mw/fxmodeldump.c and tools/mw/fxmodel_test.c with
#             runtime/mw/fxmodel.c, writes the model dump of all 31 effects in
#             one pool to out/mw/fx/ma/all.txt (implementation A), checks it
#             for UNKNOWN( and against fx-model §7's line counts, runs the unit
#             tests on the 31 (--mw; the DLL part when NFSMW_D3DX names
#             d3dx9_26.dll), and compares A with tools/fxmodel.py's dump
#             (implementation B, out/mw/fx/mb/all.txt), then A's and B's
#             Set/Get/lookup traces (--ops, ma/ops.txt and mb/ops.txt), then
#             B's own self-test and pool test
#   --pres    also the preshader evaluator (docs/mw/fx-preshader.md): builds
#             tools/mw/fxpres_test.c and tools/mw/fxpresrun.c with
#             runtime/mw/fxpres.c, runs the unit tests on the 31 (--mw),
#             writes the cases and rounds of tools/mw/fxpres_cases.py into
#             out/mw/fx/pres/, compares implementation A's results with
#             tools/fxpres.py's (B) byte for byte on both and on the rounds
#             over patched copies of the effects (fxpres_cases.py patch: the
#             input table's padding, mov made rcp, rsq, neg, frc, exp, log,
#             sin), runs B's self-test and the oracle comparison's own (each
#             way A can be wrong that it must catch), and when NFSMW_D3DX
#             names d3dx9_26.dll, runs tools/mw/fxoracle.sh (A against the
#             DLL under Unicorn)
#   --fuzz N  also runs tools/mw/fxfuzz.c: truncated and corrupted copies of
#             every effect through the parser and the dump, N random ones each;
#             then the same copies through the effect model (fxmodeldump.c's
#             fuzz entry: two models in one pool, the model dump, every Set,
#             Get and lookup on every node, free and make again); with --pres
#             also N corrupted copies of every preshader and 20 x N made-up
#             programs through fxp_load and fxp_run (tools/mw/fxpresrun.c fuzz)
#   --diff N  also compares this dump with tools/fx2dump.py's (implementation B)
#             on N mutated copies of each effect (tools/mw/fxdiff_mutate.py)
#             and N made-up effects (tools/mw/fxdiff_synth.py); any split fails.
#             With --pres also the two evaluators on N corrupted copies of
#             every preshader (tools/mw/fxpres_cases.py mutate).
#             With --model also the two models (dump and trace) on N mutated
#             copies of each effect and 20 x N made-up effect sets
#   --wasm    also compiles runtime/mw/fx2.c, runtime/mw/fxmodel.c and runtime/mw/fxpres.c with
#             web/build.sh's wasm target and runtime flags (GAME=mw), warnings
#             fatal; then links tools/mw/fxpres_test.c and tools/mw/fxpresrun.c
#             for wasm32-wasip1 (the same flags without threads, which node's
#             WASI does not run; wasi-sdk's default 64 KB stack) and runs them
#             under node (tools/mw/wasirun.mjs): the unit tests on the 31, and
#             with --pres the interpreter cases, the effect rounds and (with
#             --diff) the corrupted copies against the native A: the same
#             bytes but for the approximate class, which takes each host's C
#             library (fxpres_compare.py host)
# Everything goes under out/ (ignored): the effects and their dumps are game
# data and never go into the repo.
set -e -o pipefail
cd "$(dirname "$0")/../.."
FUZZ=""; DIFF=""; WASM=""; MODEL=""; PRES=""
while [ $# -gt 0 ]; do
  case "$1" in
    --model) MODEL=1; shift ;;
    --pres) PRES=1; shift ;;
    --fuzz) FUZZ="${2:?--fuzz <copies per file>}"; shift 2 ;;
    --diff) DIFF="${2:?--diff <count>}"; shift 2 ;;
    --wasm) WASM=1; shift ;;
    *) echo "usage: tools/mw/fxdump.sh [--model] [--pres] [--fuzz <copies per file>] [--diff <count>] [--wasm]"; exit 2 ;;
  esac
done
RES=out/mw/fx/res; OUT=out/mw/fx/a; BIN=out/mw/fx/bin
ls "$RES"/*.fxo > /dev/null 2>&1 || { echo "no effects in $RES: run tools/fxres.py <speed.exe> first"; exit 1; }
mkdir -p "$OUT" "$BIN"
CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -Iruntime/mw"
clang $CFLAGS -o "$BIN/fxdump" tools/mw/fxdump.c runtime/mw/fx2.c

n=0; total=0
for f in "$RES"/*.fxo; do
  name=$(basename "$f" .fxo)
  "$BIN/fxdump" "$f" "$OUT/$name.txt"
  u=$( { grep -o 'UNKNOWN(' "$OUT/$name.txt" || true; } | wc -l | tr -d ' ')
  [ "$u" = 0 ] || echo "$name: $u UNKNOWN"
  n=$((n + 1)); total=$((total + u))
done
echo "$n dumps in $OUT, $total UNKNOWN"

if [ -n "$MODEL" ]; then
  MA=out/mw/fx/ma; MB=out/mw/fx/mb
  mkdir -p "$MA"
  clang $CFLAGS -o "$BIN/fxmodeldump" tools/mw/fxmodeldump.c runtime/mw/fxmodel.c runtime/mw/fx2.c
  clang $CFLAGS -o "$BIN/fxmodel_test" tools/mw/fxmodel_test.c runtime/mw/fxmodel.c runtime/mw/fx2.c
  "$BIN/fxmodeldump" "$MA/all.txt" "$RES"/*.fxo
  count() { { grep -E -c "$1" "$MA/all.txt" || true; } | tr -d ' '; }
  u=$( { grep -o 'UNKNOWN(' "$MA/all.txt" || true; } | wc -l | tr -d ' ')
  c=$(count '^ +const '); s=$(count '^  slot '); h=$(count '^    holder '); a=$(count '^    apart ')
  echo "model dump: $MA/all.txt, $(wc -l < "$MA/all.txt" | tr -d ' ') lines, $u UNKNOWN, $c const, $s slots, $h holders, $a apart"
  [ "$u" = 0 ] || { echo "UNKNOWN in the model dump"; exit 1; }
  # fx-model §7's counts for the 31 effects
  [ "$c $s $h $a" = "1355 20 156 9" ] || { echo "the model dump's counts differ from fx-model §7 (1355 const, 20 slots, 156 holders, 9 apart)"; exit 1; }
  "$BIN/fxmodel_test" --mw "$RES"/*.fxo
  if [ -f tools/fxmodel.py ]; then
    rm -rf "$MB"; mkdir -p "$MB"
    .venv/bin/python -I tools/fxmodel.py "$RES" "$MB" > /dev/null
    if ! cmp -s "$MA/all.txt" "$MB/all.txt"; then
      echo "A and B split:"; diff "$MA/all.txt" "$MB/all.txt" | head -5; exit 1
    fi
    echo "A and B agree byte for byte: $MA/all.txt"
    "$BIN/fxmodeldump" --ops "$MA/ops.txt" "$RES"/*.fxo
    .venv/bin/python -I tools/fxmodel.py --ops "$RES" "$MB"
    if ! cmp -s "$MA/ops.txt" "$MB/ops.txt"; then
      echo "A's and B's Set/Get traces split:"; diff "$MA/ops.txt" "$MB/ops.txt" | head -5; exit 1
    fi
    echo "A's and B's Set/Get/lookup traces agree: $MA/ops.txt, $(wc -l < "$MA/ops.txt" | tr -d ' ') lines"
    .venv/bin/python -I tools/fxmodel.py --self-test "$RES" | tail -1
    .venv/bin/python -I tools/fxmodel.py --pool-test "$RES" | tail -1
  else
    echo "tools/fxmodel.py (implementation B) is not there yet: no A/B compare"
  fi
fi

if [ -n "$PRES" ]; then
  PD=out/mw/fx/pres
  mkdir -p "$PD"
  clang $CFLAGS -o "$BIN/fxpres_test" tools/mw/fxpres_test.c runtime/mw/fxpres.c runtime/mw/fxmodel.c runtime/mw/fx2.c -lm
  clang $CFLAGS -o "$BIN/fxpresrun" tools/mw/fxpresrun.c runtime/mw/fxpres.c runtime/mw/fxmodel.c runtime/mw/fx2.c -lm
  "$BIN/fxpres_test" --mw "$RES"/*.fxo | tail -2
  .venv/bin/python -I tools/fxpres.py --self-test | head -1
  .venv/bin/python -I tools/mw/fxpres_compare.py --self-test | head -1
  .venv/bin/python -I tools/mw/fxpres_cases.py vm "$RES" "$PD/cases.txt"
  "$BIN/fxpresrun" vm "$PD/cases.txt" "$PD/a_vm.txt"
  .venv/bin/python -I tools/fxpres.py vm "$PD/cases.txt" "$PD/b_vm.txt"
  if ! cmp -s "$PD/a_vm.txt" "$PD/b_vm.txt"; then
    echo "A and B split on the interpreter cases:"; diff "$PD/a_vm.txt" "$PD/b_vm.txt" | head -5; exit 1
  fi
  echo "A and B agree on $(grep -c '^case ' "$PD/a_vm.txt") interpreter cases: $PD/a_vm.txt"
  .venv/bin/python -I tools/mw/fxpres_cases.py rounds "$RES" "$PD/rounds.txt"
  "$BIN/fxpresrun" effect "$PD/rounds.txt" "$PD/a_eff.txt" "$RES"/*.fxo
  .venv/bin/python -I tools/fxpres.py effect "$PD/rounds.txt" "$PD/b_eff.txt" "$RES"/*.fxo
  if ! cmp -s "$PD/a_eff.txt" "$PD/b_eff.txt"; then
    echo "A and B split on the effect rounds:"; diff "$PD/a_eff.txt" "$PD/b_eff.txt" | head -5; exit 1
  fi
  echo "A and B agree on $(grep -c ' state ' "$PD/a_eff.txt") preshader state values: $PD/a_eff.txt"
  # the same rounds on patched copies of the effects (game data, under out/)
  for spec in "pad" "op 103" "op 107" "op 101" "op 104" "op 105" "op 106 --n1" "op 108"; do
    set -- $spec
    dir="$PD/fx_$1${2:+_$2}"
    rm -rf "$dir"
    .venv/bin/python -I tools/mw/fxpres_cases.py patch "$RES" "$dir" $spec > /dev/null
    "$BIN/fxpresrun" effect "$PD/rounds.txt" "$dir/a_eff.txt" "$dir"/*.fxo 2> /dev/null
    .venv/bin/python -I tools/fxpres.py effect "$PD/rounds.txt" "$dir/b_eff.txt" "$dir"/*.fxo 2> /dev/null
    if ! cmp -s "$dir/a_eff.txt" "$dir/b_eff.txt"; then
      echo "A and B split on the rounds over the copies ($spec):"; diff "$dir/a_eff.txt" "$dir/b_eff.txt" | head -5; exit 1
    fi
    echo "A and B agree on the rounds over the copies ($spec): $(grep -c ' state ' "$dir/a_eff.txt") values"
  done
  if [ -n "$DIFF" ]; then
    .venv/bin/python -I tools/mw/fxpres_cases.py mutate "$RES" "$PD/mut.txt" --copies "$DIFF"
    "$BIN/fxpresrun" vm "$PD/mut.txt" "$PD/a_mut.txt"
    .venv/bin/python -I tools/fxpres.py vm "$PD/mut.txt" "$PD/b_mut.txt"
    if ! cmp -s "$PD/a_mut.txt" "$PD/b_mut.txt"; then
      echo "A and B split on corrupted preshaders:"; diff "$PD/a_mut.txt" "$PD/b_mut.txt" | head -5; exit 1
    fi
    echo "A and B agree on $(grep -c '^case ' "$PD/a_mut.txt") corrupted preshaders"
  fi
  if [ -n "${NFSMW_D3DX:-}" ]; then
    tools/mw/fxoracle.sh
  else
    echo "NFSMW_D3DX is not set: no oracle (tools/mw/fxoracle.sh)"
  fi
fi

if [ -n "$FUZZ" ]; then
  clang $CFLAGS -DFXDUMP_NO_MAIN -o "$BIN/fxfuzz" tools/mw/fxfuzz.c tools/mw/fxdump.c runtime/mw/fx2.c
  "$BIN/fxfuzz" "$FUZZ" "$RES"/*.fxo
  clang $CFLAGS -DFXMODELDUMP_NO_MAIN -DFXMODEL_FUZZ -o "$BIN/fxmodelfuzz" tools/mw/fxfuzz.c tools/mw/fxmodeldump.c runtime/mw/fxmodel.c runtime/mw/fx2.c
  "$BIN/fxmodelfuzz" "$FUZZ" "$RES"/*.fxo | sed 's/^fxfuzz:/fxmodelfuzz (the model):/'
  if [ -n "$PRES" ]; then
    "$BIN/fxpresrun" fuzz "$FUZZ" "$RES"/*.fxo
  fi
fi

if [ -n "$DIFF" ]; then
  rm -rf out/mw/fx/diff
  .venv/bin/python -I tools/mw/fxdiff_mutate.py "$PWD" "$DIFF" || { echo "A and B split: see out/mw/fx/diff/cases"; exit 1; }
  .venv/bin/python -I tools/mw/fxdiff_synth.py "$PWD" "$DIFF" || { echo "A and B split: see out/mw/fx/diff/synth_cases"; exit 1; }
  if [ -n "$MODEL" ]; then
    .venv/bin/python -I tools/mw/fxdiff_mutate.py --model "$PWD" "$DIFF" | sed 's/^/model, mutated: /' ||
      { echo "the models split: see out/mw/fx/diff/model_cases"; exit 1; }
    .venv/bin/python -I tools/mw/fxdiff_synth.py --model "$PWD" "$((DIFF * 20))" | sed 's/^/model, made up: /' ||
      { echo "the models split: see out/mw/fx/diff/model_synth_cases"; exit 1; }
  fi
  rm -rf out/mw/fx/diff
fi

if [ -n "$WASM" ]; then
  WASI="${WASI:-$( (ls -d deps/wasi-sdk-*-arm64-macos deps/wasi-sdk-*-x86_64-linux 2>/dev/null || true) | tail -1)}"
  [ -x "$WASI/bin/clang" ] || { echo "wasi-sdk not found in deps/ (set WASI=...)"; exit 1; }
  GAME=mw; . tools/gameconf.sh
  # web/build.sh's TARGET, OPT, FEAT, DEFS and CFLAGS, and the -DNFS_ENHANCED=0
  # it compiles runtime/<game>/*.c with
  "$WASI/bin/clang" --target=wasm32-wasip1-threads -pthread -O3 \
    -msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false \
    -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS \
    -fno-delete-null-pointer-checks -include runtime/web/compat/wasm_plat.h -Iruntime/web/compat -Iruntime -I"$GC_GEN_DIR/all" -DNFSU2_GAME_MW \
    -DNFS_ENHANCED=0 -Wall -Wextra -Werror -c -o "$BIN/mw_fx2.o" runtime/mw/fx2.c
  echo "runtime/mw/fx2.c compiles for wasm: $BIN/mw_fx2.o"
  "$WASI/bin/clang" --target=wasm32-wasip1-threads -pthread -O3 \
    -msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false \
    -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS \
    -fno-delete-null-pointer-checks -include runtime/web/compat/wasm_plat.h -Iruntime/web/compat -Iruntime -I"$GC_GEN_DIR/all" -DNFSU2_GAME_MW \
    -DNFS_ENHANCED=0 -Wall -Wextra -Werror -c -o "$BIN/mw_fxmodel.o" runtime/mw/fxmodel.c
  echo "runtime/mw/fxmodel.c compiles for wasm: $BIN/mw_fxmodel.o"
  "$WASI/bin/clang" --target=wasm32-wasip1-threads -pthread -O3 \
    -msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false \
    -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS \
    -fno-delete-null-pointer-checks -include runtime/web/compat/wasm_plat.h -Iruntime/web/compat -Iruntime -I"$GC_GEN_DIR/all" -DNFSU2_GAME_MW \
    -DNFS_ENHANCED=0 -Wall -Wextra -Werror -c -o "$BIN/mw_fxpres.o" runtime/mw/fxpres.c
  echo "runtime/mw/fxpres.c compiles for wasm: $BIN/mw_fxpres.o"
  # linked and run: the unit tests, and A's texts against the native ones (node's WASI: no threads)
  WFLAGS="--target=wasm32-wasip1 -O3 -msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -std=c11 -Wall -Wextra
    -Werror -Iruntime/mw"
  "$WASI/bin/clang" $WFLAGS -o "$BIN/fxpres_test.wasm" tools/mw/fxpres_test.c runtime/mw/fxpres.c runtime/mw/fxmodel.c \
    runtime/mw/fx2.c -lm
  "$WASI/bin/clang" $WFLAGS -o "$BIN/fxpresrun.wasm" tools/mw/fxpresrun.c runtime/mw/fxpres.c runtime/mw/fxmodel.c \
    runtime/mw/fx2.c -lm
  W() { node --no-warnings tools/mw/wasirun.mjs "$@"; }       # out/mw/fx is /fx inside
  FX=$(cd "$RES" && ls *.fxo | sed 's|^|/fx/res/|')
  W "$BIN/fxpres_test.wasm" out/mw/fx --mw $FX | tail -1
  if [ -n "$PRES" ]; then
    W "$BIN/fxpresrun.wasm" out/mw/fx vm /fx/pres/cases.txt /fx/pres/w_vm.txt 2> /dev/null
    .venv/bin/python -I tools/mw/fxpres_compare.py host "$PD/a_vm.txt" "$PD/w_vm.txt" | tail -4
    W "$BIN/fxpresrun.wasm" out/mw/fx effect /fx/pres/rounds.txt /fx/pres/w_eff.txt $FX 2> /dev/null
    cmp "$PD/a_eff.txt" "$PD/w_eff.txt" || { echo "the wasm effect rounds differ from the native ones"; exit 1; }
    echo "wasm: the effect rounds are the native A's bytes"
    if [ -n "$DIFF" ]; then
      W "$BIN/fxpresrun.wasm" out/mw/fx vm /fx/pres/mut.txt /fx/pres/w_mut.txt 2> /dev/null
      cmp "$PD/a_mut.txt" "$PD/w_mut.txt" || { echo "the wasm runs of the corrupted copies differ"; exit 1; }
      echo "wasm: the corrupted copies are the native A's bytes"
    fi
  fi
fi
