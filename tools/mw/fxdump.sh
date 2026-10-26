#!/bin/bash
# Most Wanted's compiled effects as text: builds tools/mw/fxdump.c with
# runtime/mw/fx2.c (the parser the runtime uses) and writes the canonical
# dump (docs/mw/fx2-format.md §5) of every effect tools/fxres.py extracted.
#
#   tools/mw/fxdump.sh [--fuzz <copies per file>] [--diff <count>] [--wasm]
#
# out/mw/fx/res/<NAME>.fxo -> out/mw/fx/a/<NAME>.txt.  The tools are built
# with the system clang (-Wall -Wextra -Werror, ASan and UBSan, any report
# fatal) into out/mw/fx/bin/.  It stops on a parse error or a sanitizer
# report and prints the number of UNKNOWN( in each dump that has any (the
# goal is none).
#   --fuzz N  also runs tools/mw/fxfuzz.c: truncated and corrupted copies of
#             every effect through the parser and the dump, N random ones each
#   --diff N  also compares this dump with tools/fx2dump.py's (implementation B)
#             on N mutated copies of each effect (tools/mw/fxdiff_mutate.py)
#             and N made-up effects (tools/mw/fxdiff_synth.py); any split fails
#   --wasm    also compiles runtime/mw/fx2.c with web/build.sh's wasm target
#             and runtime flags (GAME=mw), warnings fatal
# Everything goes under out/ (ignored): the effects and their dumps are game
# data and never go into the repo.
set -e -o pipefail
cd "$(dirname "$0")/../.."
FUZZ=""; DIFF=""; WASM=""
while [ $# -gt 0 ]; do
  case "$1" in
    --fuzz) FUZZ="${2:?--fuzz <copies per file>}"; shift 2 ;;
    --diff) DIFF="${2:?--diff <count>}"; shift 2 ;;
    --wasm) WASM=1; shift ;;
    *) echo "usage: tools/mw/fxdump.sh [--fuzz <copies per file>] [--diff <count>] [--wasm]"; exit 2 ;;
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

if [ -n "$FUZZ" ]; then
  clang $CFLAGS -DFXDUMP_NO_MAIN -o "$BIN/fxfuzz" tools/mw/fxfuzz.c tools/mw/fxdump.c runtime/mw/fx2.c
  "$BIN/fxfuzz" "$FUZZ" "$RES"/*.fxo
fi

if [ -n "$DIFF" ]; then
  rm -rf out/mw/fx/diff
  .venv/bin/python -I tools/mw/fxdiff_mutate.py "$PWD" "$DIFF" || { echo "A and B split: see out/mw/fx/diff/cases"; exit 1; }
  .venv/bin/python -I tools/mw/fxdiff_synth.py "$PWD" "$DIFF" || { echo "A and B split: see out/mw/fx/diff/synth_cases"; exit 1; }
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
fi
