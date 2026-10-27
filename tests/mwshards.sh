#!/bin/bash
# Compile Most Wanted's lifted exe (gen/mw/all, GAME=mw web/gen.sh) for the
# Mac, for the native tests that link it: tests/mwmemmove.sh,
# tests/mworacle.sh.  Objects in out/obj/mw (shard_*.o, fntab.o), the same
# content-digest incremental rule as build.sh (FULL=1 compiles all of them).
#
#   tests/mwshards.sh            exit 0 = every object is up to date
#   OPT="-O1" tests/mwshards.sh  another optimisation level (default build.sh's -O3)
#
# The lift is the web's (FLAGABI, X87OPT, JTSWITCH, LREGS, FTOL2INL: see
# gen/mw/all/web-lift.txt), compiled with clang for arm64 instead of wasm32:
# the same C, cpu.h's native guest accesses (g_mem).  Those are plain types,
# the browser's may_alias ones (L-042), so -fno-strict-aliasing, which every
# OPT here gets.  With strict aliasing clang moved a load past a store of
# another type to the same guest bytes: 0x7D9D20 patches a double's
# exponent with a 16-bit store and loads the double again, and got the old
# value back (tests/mworacle.sh; 0x68D6A0 and 0x82741D differed too, and are
# the same without it), which the browser's build cannot do.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
G=gen/mw/all
[ -f "$G/fntab.c" ] || { echo "no $G: run GAME=mw web/gen.sh <speed.exe>"; exit 1; }
[ -f "$G/gameinfo.h" ] || { echo "$G has no gameinfo.h: lift it again with GAME=mw web/gen.sh"; exit 1; }
OD="$ROOT/out/obj/mw"; mkdir -p "$OD"
OPT="${OPT:--O3 -mcpu=apple-m1} -fno-strict-aliasing"
J="${J:-$(sysctl -n hw.ncpu)}"
export OPT ROOT OD FULL HDRSUM="$(cat runtime/cpu.h "$G/gameinfo.h" | shasum | cut -c1-16)"
# a shard whose .c, decls.h, cpu.h, gameinfo.h and flags are unchanged is skipped
(cd "$G" && ls shard_*.c fntab.c $( [ -f stubs.c ] && echo stubs.c )) | xargs -P "$J" -n 1 sh -c '
  cd "$ROOT/gen/mw/all"
  obj="$OD/$(basename "$1" .c).o"
  sum=$( (cat "$1" decls.h; echo "$OPT $HDRSUM") | shasum | cut -c1-40)
  if [ -z "$FULL" ] && [ -f "$obj" ] && [ "$(cat "$obj.sum" 2>/dev/null)" = "$sum" ]; then exit 0; fi
  o=$(clang $OPT -c -I"$ROOT/runtime" -I. -DNFSU2_GAME_MW -Wno-unused-label -Wno-parentheses-equality \
        -o "$obj" "$1" 2>&1); r=$?
  [ -z "$o" ] || printf "%s\n" "$o" | head -5
  [ $r -eq 0 ] || { echo "DID NOT COMPILE: $1" >&2; exit $r; }
  echo "$sum" > "$obj.sum"' sh
# the objects of this lift, never one an older lift left behind
(cd "$G" && ls shard_*.c fntab.c $( [ -f stubs.c ] && echo stubs.c )) | sed -E "s|^(.*)\.c$|$OD/\1.o|" > "$OD/objs.list"
echo "mwshards: $(wc -l < "$OD/objs.list" | tr -d ' ') objects in out/obj/mw ($OPT)"
