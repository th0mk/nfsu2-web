#!/bin/bash
# Most Wanted's compiled effects as text: builds tools/mw/fxdump.c with
# runtime/mw/fx2.c (the parser the runtime uses) and writes the canonical
# dump (docs/mw/fx2-format.md §5) of every effect tools/fxres.py extracted.
#
#   tools/mw/fxdump.sh [--model] [--fuzz <copies per file>] [--diff <count>] [--wasm]
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
#   --fuzz N  also runs tools/mw/fxfuzz.c: truncated and corrupted copies of
#             every effect through the parser and the dump, N random ones each;
#             then the same copies through the effect model (fxmodeldump.c's
#             fuzz entry: two models in one pool, the model dump, every Set,
#             Get and lookup on every node, free and make again)
#   --diff N  also compares this dump with tools/fx2dump.py's (implementation B)
#             on N mutated copies of each effect (tools/mw/fxdiff_mutate.py)
#             and N made-up effects (tools/mw/fxdiff_synth.py); any split fails.
#             With --model also the two models (dump and trace) on N mutated
#             copies of each effect and 20 x N made-up effect sets
#   --wasm    also compiles runtime/mw/fx2.c and runtime/mw/fxmodel.c with
#             web/build.sh's wasm target and runtime flags (GAME=mw), warnings
#             fatal
# Everything goes under out/ (ignored): the effects and their dumps are game
# data and never go into the repo.
set -e -o pipefail
cd "$(dirname "$0")/../.."
FUZZ=""; DIFF=""; WASM=""; MODEL=""
while [ $# -gt 0 ]; do
  case "$1" in
    --model) MODEL=1; shift ;;
    --fuzz) FUZZ="${2:?--fuzz <copies per file>}"; shift 2 ;;
    --diff) DIFF="${2:?--diff <count>}"; shift 2 ;;
    --wasm) WASM=1; shift ;;
    *) echo "usage: tools/mw/fxdump.sh [--model] [--fuzz <copies per file>] [--diff <count>] [--wasm]"; exit 2 ;;
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

if [ -n "$FUZZ" ]; then
  clang $CFLAGS -DFXDUMP_NO_MAIN -o "$BIN/fxfuzz" tools/mw/fxfuzz.c tools/mw/fxdump.c runtime/mw/fx2.c
  "$BIN/fxfuzz" "$FUZZ" "$RES"/*.fxo
  clang $CFLAGS -DFXMODELDUMP_NO_MAIN -DFXMODEL_FUZZ -o "$BIN/fxmodelfuzz" tools/mw/fxfuzz.c tools/mw/fxmodeldump.c runtime/mw/fxmodel.c runtime/mw/fx2.c
  "$BIN/fxmodelfuzz" "$FUZZ" "$RES"/*.fxo | sed 's/^fxfuzz:/fxmodelfuzz (the model):/'
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
fi
