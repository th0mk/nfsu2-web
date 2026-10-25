#!/bin/bash
# Compile the 23k lifted functions in parallel
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
# -O3 + -mcpu=native measured: median race frame 18.7 -> 15.7 ms
# (53.5 -> 63.9 fps).  Lifted code is made of very many small basic blocks,
# so more aggressive inlining/simplification pays off directly.
# DISTRIBUTION baseline:
#  -mcpu=apple-m1  : M1 is the common denominator of all Apple Silicon.  Built
#                    with "native", a binary made on a newer Mac crashes on an
#                    M1 with an illegal instruction.  Measured cost on M1: zero.
#  -mmacosx-version-min=11.0 : without the flag the binary is stamped with the
#                    building machine's OS version (minos 26.0) and on older
#                    macOS it never opens, saying "a newer version is required".
MCPU="${MCPU:-apple-m1}"
MINOS="${MINOS:-11.0}"
OPT="${OPT:--O3 -mcpu=$MCPU -mmacosx-version-min=$MINOS}"
J="${J:-$(sysctl -n hw.ncpu)}"
mkdir -p "$ROOT/out/obj" "$ROOT/out/obj/srv"
# Do NOT EMBED $OPT in the xargs command line: the length of a line built
# with -I{} is limited ("command line cannot be assembled, too long").  Pass
# it in through the environment and let the inner shell expand it.
#
# Do NOT PIPE compiler output DIRECTLY into "| head -5": once head closed
# after 5 lines, clang died of SIGPIPE writing the next warning and the object
# file was NEVER written (shard_0006/0019: 3 times in 3 tries), and build.sh
# still returned 0 -> link then broke with "symbol(s) not found".
# The output goes into a variable first and is truncated after; clang's exit
# code is kept (if any shard fails to compile, xargs, and so build.sh, fails).
export OPT ROOT
# v12na: incremental build keyed on a content digest.  genall.py rewrites
# every shard, so mtime is useless; a shard whose .c + cpu.h + OPT digest is
# unchanged is skipped (FULL=1 compiles all of them).  cpu.h takes the exe's
# .text from gen/all/gameinfo.h, or from runtime/gameinfo_ug2.h without one.
export FULL HDRSUM="$(cat "$ROOT/runtime/cpu.h" "$( [ -f "$ROOT/gen/all/gameinfo.h" ] && echo "$ROOT/gen/all/gameinfo.h" || echo "$ROOT/runtime/gameinfo_ug2.h" )" | shasum | cut -c1-16)"
# gen/all (SPEED2.EXE) -> out/obj, gen/server (server.dll, see gen.sh) ->
# out/obj/srv: the same incremental rule for both, one xargs pool.
{ (cd "$ROOT/gen/all" && ls shard_*.c fntab.c stubs.c 2>/dev/null | sed "s|^|all |")
  [ -d "$ROOT/gen/server" ] && (cd "$ROOT/gen/server" && ls shard_*.c fntab.c stubs.c 2>/dev/null | sed "s|^|server |"); } | \
  xargs -P "$J" -n 2 sh -c '
    case "$1" in all) od="$ROOT/out/obj" ;; *) od="$ROOT/out/obj/srv" ;; esac
    cd "$ROOT/gen/$1"; shift
    obj="$od/$(basename "$1" .c).o"
    sum=$( (cat "$1" decls.h; echo "$OPT $HDRSUM") | shasum | cut -c1-40)
    if [ -z "$FULL" ] && [ -f "$obj" ] && [ "$(cat "$obj.sum" 2>/dev/null)" = "$sum" ]; then exit 0; fi
    o=$(clang $OPT -c -I"$ROOT/runtime" -I. -Wno-unused-label -Wno-parentheses-equality \
          -o "$obj" "$1" 2>&1); r=$?
    [ -z "$o" ] || printf "%s\n" "$o" | head -5
    [ $r -eq 0 ] || echo "DID NOT COMPILE: $1" >&2
    [ $r -eq 0 ] && echo "$sum" > "$obj.sum" && echo "  compiled: $1"
    exit $r' sh
