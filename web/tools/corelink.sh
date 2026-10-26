#!/bin/bash
# The runtime's core links without any game's files: the objects of a
# web/build.sh build (out/web), minus every runtime file runtime/core.list does
# not name (UG2's game_ug2.c and feature files), with runtime/web/*.c (its
# null_game.c stand-ins in place of the missing features and game table) and
# the lifted code.  The lift names each override's replacement (sub_X, which
# the game's files define): a generated pass-through to the lifted sub_X_orig
# stands in for each one of `tools/genstamp.sh overrides`.  Exit 0 = the link
# has no undefined symbol; 1 = it failed (the undefined symbols are listed).
#
#   web/tools/corelink.sh       after web/build.sh; writes out/web/corelink/
set -e -o pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
WASI="${WASI:-$( (ls -d "$ROOT"/deps/wasi-sdk-*-arm64-macos "$ROOT"/deps/wasi-sdk-*-x86_64-linux 2>/dev/null || true) | tail -1)}"
[ -x "$WASI/bin/clang" ] || { echo "corelink: wasi-sdk not found in deps/ (set WASI=...)"; exit 2; }
OUT="$ROOT/out/web"; C="$OUT/corelink"
[ -f "$OUT/rt/mem.o" ] && ls "$OUT"/obj/*.o > /dev/null 2>&1 || { echo "corelink: no build in out/web (run web/build.sh)"; exit 2; }
rm -rf "$C"; mkdir -p "$C"
# web/build.sh's target and features
TARGET="--target=wasm32-wasip1-threads -pthread"
FEAT="-msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false"
# the overrides' stand-ins (UG2's, as out/web's objects are, whatever GAME the shell has)
{ echo '#include "cpu.h"'
  GAME=ug2 tools/genstamp.sh overrides | tr ',' '\n' | grep . | while read -r a; do
    echo "void sub_00${a}_orig(CPU *c); void sub_00${a}(CPU *c){ sub_00${a}_orig(c); }"; done; } > "$C/overrides_stub.c"
"$WASI/bin/clang" $TARGET -O1 $FEAT -include runtime/web/compat/wasm_plat.h -Iruntime/web/compat -Iruntime \
  -c -o "$C/overrides_stub.o" "$C/overrides_stub.c"
# the core's objects: core.list's .c files and runtime/web/*.c
CORE=""; n=0
for f in $(sed 's/#.*//' runtime/core.list | awk 'NF{print $1}' | grep '\.c$' | sed 's/\.c$//'); do
  [ -f "$OUT/rt/$f.o" ] || { echo "corelink: out/web/rt/$f.o missing (core.list names $f.c; rebuild)"; exit 2; }
  CORE="$CORE $OUT/rt/$f.o"; n=$((n + 1)); done
WEB="$(ls "$OUT"/rt/web_*.o)"
LEFT="$(cd "$OUT/rt" && ls *.o | grep -v '^web_' | while read -r o; do
  sed 's/#.*//' "$ROOT/runtime/core.list" | awk 'NF{print $1}' | grep -qx "${o%.o}.c" || printf '%s ' "${o%.o}"; done)"
echo "corelink: $n core files + $(echo "$WEB" | wc -l | tr -d ' ') runtime/web files + the lift; left out: $LEFT"
OBJS="$(ls "$OUT"/obj/*.o) $(ls "$OUT"/obj/srv/*.o) $CORE $WEB $C/overrides_stub.o"
set +e
"$WASI/bin/clang" $TARGET -O3 $FEAT -o "$C/core.wasm" $OBJS \
  -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=4294967296 \
  -Wl,--initial-memory=1073741824,--global-base=671088640 \
  -Wl,-z,stack-size=8388608 -Wl,--no-stack-first,--error-limit=0 \
  -lsetjmp -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks > "$C/link.log" 2>&1
r=$?
set -e
if [ $r -ne 0 ] || [ ! -f "$C/core.wasm" ]; then
  echo "corelink: FAILED, $(grep -c 'undefined symbol' "$C/link.log") undefined symbols:"
  grep -o 'undefined symbol: .*' "$C/link.log" | sort | uniq -c | sort -rn | head -60
  exit 1
fi
echo "corelink: OK, no undefined symbol (out/web/corelink/core.wasm, $(( $(wc -c < "$C/core.wasm") / 1048576 )) MB)"
