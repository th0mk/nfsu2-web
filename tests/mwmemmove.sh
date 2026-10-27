#!/bin/bash
# Most Wanted's lifted CRT memmove (0x7C4840) and its twin (0x7D6D30)
# against a reference memmove: see tests/mwmemmove.c.
#   tests/mwmemmove.sh           native: the lift compiled by tests/mwshards.sh
#   tests/mwmemmove.sh --wasm    the objects GAME=mw web/build.sh compiled
#                                (out/web/mw/lift.objs), under node:wasi
# Needs gen/mw/all (GAME=mw web/gen.sh) and speed.exe: NFSMW_DATA=<MW's
# folder>, else the ignored mw/ folder of this checkout or, in a worktree, of
# the main clone.  The exe is read, never written; it goes to the test on
# stdin.  Exit 0 = every case passed.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
GAME=mw; . tools/gameconf.sh
MAIN="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$ROOT/.git")")"
MWDIR="${NFSMW_DATA:-}"
if [ -z "$MWDIR" ]; then
  for d in "$ROOT/mw/Need For Speed Most Wanted Black Edition" "$MAIN/mw/Need For Speed Most Wanted Black Edition"; do
    [ -f "$d/$GC_EXE_NAME" ] && { MWDIR="$d"; break; }
  done
fi
EXE="$MWDIR/$GC_EXE_NAME"
[ -f "$EXE" ] || { echo "no $GC_EXE_NAME (NFSMW_DATA=<MW's folder>)"; exit 1; }
gameconf_sha "$EXE" "$GC_EXE_SHA256" "$GC_EXE_NAME"
mkdir -p out/obj/mwtest
if [ "${1:-}" = --wasm ]; then
  WASI="${WASI:-$(ls -d "$ROOT"/deps/wasi-sdk-*-arm64-macos "$ROOT"/deps/wasi-sdk-*-x86_64-linux 2>/dev/null | tail -1)}"
  [ -x "$WASI/bin/clang" ] || { echo "wasi-sdk not found in deps/ (set WASI=...)"; exit 1; }
  OUT="$ROOT/out/web/mw"; LIST="$OUT/lift.objs"
  [ -f "$LIST" ] || { echo "no $LIST: run GAME=mw web/build.sh"; exit 1; }
  stampsum(){ if [ -f "$1/stamp.txt" ]; then shasum -a 256 < "$1/stamp.txt" | cut -c1-16; else echo none; fi; }
  [ "$(sed -n 's|^#stamp gen/mw/all ||p' "$LIST")" = "$(stampsum gen/mw/all)" ] ||
    { echo "out/web/mw's objects are older than the lift in gen/mw/all: run GAME=mw web/build.sh"; exit 1; }
  # Same target, features and defines as web/build.sh (keep in step with it).
  TARGET="--target=wasm32-wasip1-threads -pthread"
  FEAT="-msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false"
  DEFS="-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS"
  CFLAGS="$TARGET -O2 $FEAT $DEFS -fno-delete-null-pointer-checks -include runtime/web/compat/wasm_plat.h -Iruntime/web/compat -Iruntime -Igen/mw/all -DNFSU2_GAME_MW"
  "$WASI/bin/clang" $CFLAGS -c -o out/obj/mwtest/mwmemmove.wasm.o tests/mwmemmove.c
  # the exe's lifted objects only (not server.dll's, not the D3DX oracle's)
  LIFTED="$(grep -E '^obj/[^/]+\.o$' "$LIST" | sed "s|^|$OUT/|")"
  "$WASI/bin/clang" $TARGET -O2 -o out/obj/mwtest/mwmemmove.wasm $LIFTED out/obj/mwtest/mwmemmove.wasm.o \
    -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=4294967296 \
    -Wl,--initial-memory=1073741824,--global-base=671088640 \
    -Wl,-z,stack-size=$(( 8 * 1048576 )) -Wl,--no-stack-first \
    -lsetjmp -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks
  node --no-warnings web/tools/wasi-run.mjs out/obj/mwtest/mwmemmove.wasm < "$EXE"
else
  tests/mwshards.sh
  clang -O2 -Iruntime -Igen/mw/all -DNFSU2_GAME_MW -c -o out/obj/mwtest/mwmemmove.o tests/mwmemmove.c
  clang -O2 -Iruntime -c -o out/obj/mwtest/mem.o runtime/mem.c
  clang -o out/obj/mwtest/mwmemmove out/obj/mwtest/mwmemmove.o out/obj/mwtest/mem.o $(cat out/obj/mw/objs.list) -lm
  out/obj/mwtest/mwmemmove < "$EXE"
fi
