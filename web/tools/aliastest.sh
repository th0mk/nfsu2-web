#!/bin/bash
# Guest memory type punning on WebAssembly (tests/web/aliastest.c): a vector
# normalize in lifted style that stores a float with MRF32 and copies it out
# with MR32, compiled with exactly web/build.sh's target, features and
# defines against runtime/cpu.h and run in V8 under node:wasi.  Before
# runtime/cpu.h gave the guest accesses may_alias types, clang moved the
# integer load above the float store and the normalized z came back stale
# (the missing car-light glows).  The test shows the hazard only where clang
# happens to schedule it, so the structural check web/tools/aliasguard.sh
# (every guest access macro keeps its may_alias type) runs first.
#
#   web/tools/aliastest.sh            check, build, run; exit 0 = passed
#   OPT=-O2 web/tools/aliastest.sh    another optimisation level (default -O3)
#   GAME=mw web/tools/aliastest.sh    another game (games/<id>/game.conf): its lift's
#                                     include folder and define, as web/build.sh
# Also run by node --test (web/test/alias.test.mjs).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
WASI="${WASI:-$(ls -d "$ROOT"/deps/wasi-sdk-*-arm64-macos "$ROOT"/deps/wasi-sdk-*-x86_64-linux 2>/dev/null | tail -1)}"
[ -x "$WASI/bin/clang" ] || { echo "wasi-sdk not found in deps/ (set WASI=...)"; exit 1; }
WASI="$WASI" web/tools/aliasguard.sh
OUT="$ROOT/out/web/alias"; mkdir -p "$OUT"
# Same target, features and defines as web/build.sh (keep in step with it).
OPT="${OPT:--O3}"
TARGET="--target=wasm32-wasip1-threads -pthread"
FEAT="-msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false"
DEFS="-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS"
# the game's lift folder and define (web/build.sh's -I$G/all and -DNFSU2_GAME_<ID>)
. tools/gameconf.sh
GINC=""; [ ! -d "$GC_GEN_DIR/all" ] || GINC=" -I$GC_GEN_DIR/all"
[ "$GAME" = ug2 ] || GINC="$GINC -DNFSU2_GAME_$(echo "$GAME" | tr a-z A-Z)"
CFLAGS="$TARGET $OPT $FEAT $DEFS -fno-delete-null-pointer-checks -include runtime/web/compat/wasm_plat.h -Iruntime/web/compat -Iruntime$GINC"
# as the runtime is compiled, and as web/build.sh compiles the lifted shards (SHARDFLAGS)
for SF in "" "-DNFSU2_GUEST_OBJ"; do
  "$WASI/bin/clang" $CFLAGS $SF -o "$OUT/aliastest.wasm" tests/web/aliastest.c \
    -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=4294967296 \
    -Wl,--initial-memory=1073741824,--global-base=671088640 -Wl,--no-stack-first \
    -lsetjmp -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks
  echo "aliastest ($GAME) ${SF:-(runtime flags)}:"
  node --no-warnings web/tools/wasi-run.mjs "$OUT/aliastest.wasm"
done
