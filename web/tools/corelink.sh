#!/bin/bash
# The runtime's core links without any game's files: the objects of a
# web/build.sh build (out/web), minus every runtime file runtime/core.list does
# not name (UG2's game_ug2.c and feature files), with runtime/web/*.c (its
# null_game.c stand-ins in place of the missing features and game table) and
# the lifted code: the objects web/build.sh links (out/web/lift.objs), never
# one an older lift left in out/web/obj.  The lift names each override's
# replacement (sub_X, which the game's files define): a generated pass-through
# to the lifted sub_X_orig stands in for each one of `tools/genstamp.sh
# overrides`.  Exit 0 = the link has no undefined symbol; 1 = it failed (the
# undefined symbols and the link's other errors are listed); 2 = no build, or
# one older than the lift (a gen folder's stamp.txt is not the one
# lift.objs names) or than the runtime sources: run web/build.sh first.
#
#   web/tools/corelink.sh       after web/build.sh; writes out/web/corelink/
set -e -o pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
WASI="${WASI:-$( (ls -d "$ROOT"/deps/wasi-sdk-*-arm64-macos "$ROOT"/deps/wasi-sdk-*-x86_64-linux 2>/dev/null || true) | tail -1)}"
[ -x "$WASI/bin/clang" ] || { echo "corelink: wasi-sdk not found in deps/ (set WASI=...)"; exit 2; }
OUT="$ROOT/out/web"; C="$OUT/corelink"; LIST="$OUT/lift.objs"
[ -f "$OUT/rt/mem.o" ] && [ -f "$LIST" ] || { echo "corelink: no build in out/web (run web/build.sh)"; exit 2; }
# the build of the lift and runtime there are now, not of the ones before them
stampsum(){ if [ -f "$1/stamp.txt" ]; then shasum -a 256 < "$1/stamp.txt" | cut -c1-16; else echo none; fi; }
while read -r d s; do
  [ "$(stampsum "$d")" = "$s" ] || { echo "corelink: out/web's objects are older than the lift in $d (its stamp.txt changed since web/build.sh ran): run web/build.sh"; exit 2; }
done < <(sed -n 's/^#stamp //p' "$LIST")
NEWER="$(find runtime runtime/web runtime/web/compat -maxdepth 1 -newer "$LIST" -type f \( -name '*.c' -o -name '*.h' -o -name core.list \) -print -quit)"
[ -z "$NEWER" ] || { echo "corelink: out/web's objects are older than $NEWER (changed since web/build.sh ran): run web/build.sh"; exit 2; }
while read -r o; do
  [ -f "$OUT/$o" ] || { echo "corelink: out/web/$o missing (out/web/lift.objs names it; run web/build.sh)"; exit 2; }
done < <(grep -v '^#' "$LIST")
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
OBJS="$(grep -v '^#' "$LIST" | sed "s|^|$OUT/|") $CORE $WEB $C/overrides_stub.o"
set +e
"$WASI/bin/clang" $TARGET -O3 $FEAT -o "$C/core.wasm" $OBJS \
  -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=4294967296 \
  -Wl,--initial-memory=1073741824,--global-base=671088640 \
  -Wl,-z,stack-size=8388608 -Wl,--no-stack-first,--error-limit=0 \
  -lsetjmp -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks > "$C/link.log" 2>&1
r=$?
if [ $r -ne 0 ] || [ ! -f "$C/core.wasm" ]; then
  echo "corelink: FAILED, $(grep -c 'undefined symbol' "$C/link.log") undefined symbols (out/web/corelink/link.log):"
  grep -o 'undefined symbol: .*' "$C/link.log" | sort | uniq -c | sort -rn | head -60
  # the link's other errors (a duplicate symbol names its objects on the >>> lines)
  awk '/error/ { e = !/undefined symbol/ } e && (/error/ || /^>>>/) && n++ < 60' "$C/link.log"
  exit 1
fi
set -e
echo "corelink: OK, no undefined symbol (out/web/corelink/core.wasm, $(( $(wc -c < "$C/core.wasm") / 1048576 )) MB)"
