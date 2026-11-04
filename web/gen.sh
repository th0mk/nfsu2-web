#!/bin/bash
# Generate the lifted C for the web build: ./gen.sh with the lifter's two
# code-size/speed options on:
#   FLAGABI  whole-program flag liveness across calls and returns
#            (tools/flagabi.py proves which functions return flags and which
#            take them at entry; every other call/ret has dead flags)
#   X87OPT   x87 stack slots as locals within a basic block
#   JTSWITCH MSVC switch jump tables as C switches inside the function, not
#            tail calls into a lifted copy of the rest of the body per target
#   LREGS    the guest registers and flags as locals of each lifted function,
#            stored to the CPU struct only around calls, returns and tail
#            jumps (tools/lregs.py)
#   FTOL2INL the CRT's _ftol2 in place at its call sites (cpu.h FTOL2_INL)
#   INLINE_LIST  the hot callees of tools/inline_hot.txt inlined at their
#            direct call sites (tools/inline_pick.py made the list from a
#            race's call counts; INLINE_LIST= turns it off)
# Measured in Chrome (race, NFSU2_FIXED_DT=120): -16 to -29 % instructions
# per frame and a 50 MB module instead of 65 MB (FLAGABI + X87OPT).  The Mac's
# gen.sh defaults are unchanged; this wrapper is the web build's generator.
#
#   web/gen.sh <path/to/SPEED2.EXE>      writes gen/all, gen/server (and out/flagabi.json)
#   WEB_LIFT=plain web/gen.sh <exe>      without the options (A/B)
#   JTSWITCH=0 web/gen.sh <exe>          the options without JTSWITCH (A/B); LREGS=0, FTOL2INL=0 and
#                                        INLINE_LIST= likewise
# gen.sh stamps the result (gen/all/stamp.txt, tools/genstamp.sh), which
# web/build.sh checks against the checkout.  In a worktree gen/all and
# gen/server are links to the main clone's, which every other worktree builds
# against: replace them with directories first (or GEN_SHARED=1 to regenerate
# the shared ones on purpose).  The default lift is made once per sources on a
# machine: web/tools/gencache.sh keeps it, this script copies a kept one in
# instead of lifting, and web/build.sh points a worktree's links at it.
set -e -o pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
EXE="${1:-${NFSU2_DATA:+$NFSU2_DATA/SPEED2.EXE}}"
[ -f "$EXE" ] || { echo "usage: web/gen.sh path/to/SPEED2.EXE"; exit 1; }
for d in gen/all gen/server; do
  if [ -L "$d" ] && { [ "${GEN_SHARED:-}" != 1 ] || case "$(readlink "$d")" in "$(web/tools/gencache.sh dir)"/*) true ;; *) false ;; esac; }; then
    echo "web/gen.sh: $d is a link to $(readlink "$d"), which other worktrees build against;"
    echo "  rm $d && mkdir $d to lift into this worktree, or GEN_SHARED=1 web/gen.sh to regenerate the shared one (not a kept lift)"
    exit 1
  fi
done
# the default lift of these sources made before on this machine (web/tools/gencache.sh): copied, not made again
DEFAULTS=$( [ -z "${WEB_LIFT+x}${JTSWITCH+x}${LREGS+x}${FTOL2INL+x}${INLINE_LIST+x}" ] && echo 1 || true)
if [ "$DEFAULTS" = 1 ] && [ ! -L gen/all ] && [ ! -L gen/server ] && web/tools/gencache.sh get "$EXE"; then exit 0; fi
PY="$ROOT/.venv/bin/python"
mkdir -p out gen/all
# never label a new lift with an old run's options, never reuse an old flag table
rm -f gen/all/web-lift.txt out/flagabi.json
if [ "${WEB_LIFT:-opt}" = plain ]; then
  ./gen.sh "$EXE"
  LIFT=plain
else
  # flagabi.py needs the relocations, which gen.sh would only make later
  "$PY" tools/reloc2.py "$EXE"
  echo "FLAGABI: interprocedural flag liveness -> out/flagabi.json"
  # flagabi.py sees each function's control flow as the lift will, switches included
  export JTSWITCH="${JTSWITCH:-1}"
  "$PY" tools/flagabi.py "$EXE" out/flagabi.json > out/flagabi.log 2>&1 || { tail -20 out/flagabi.log; echo "web/gen.sh: flagabi.py failed"; exit 1; }
  grep -E "^summary|^  round" out/flagabi.log || true
  [ -s out/flagabi.json ] || { echo "web/gen.sh: flagabi.py wrote no out/flagabi.json"; exit 1; }
  export LREGS="${LREGS:-1}" FTOL2INL="${FTOL2INL:-1}" INLINE_LIST="${INLINE_LIST-$ROOT/tools/inline_hot.txt}"
  FLAGABI="$ROOT/out/flagabi.json" X87OPT=1 ./gen.sh "$EXE"
  LIFT="FLAGABI X87OPT$( [ "$JTSWITCH" = 1 ] && echo " JTSWITCH" || true)$( [ "$LREGS" = 1 ] && echo " LREGS" || true)$( [ "$FTOL2INL" = 1 ] && echo " FTOL2INL" || true)$( [ -n "$INLINE_LIST" ] && echo " INLINE_LIST" || true)"
fi
# a lift that failed function by function still says "gen OK": every function
# became an unsupported_insn(..., "lift-error") stub.  Refuse that.  A good
# lift of v1.2 NA has 8 such stubs (the same 8 on the Mac: addresses that are
# discovered as functions but never run); a broken one has thousands.
BAD=$(cat gen/all/shard_*.c | grep -c '"lift-error"' || true)
[ "$BAD" -le 64 ] || { echo "web/gen.sh: $BAD functions failed to lift (lift-error stubs in gen/all): not a usable lift"; rm -f gen/all/fntab.c
                       echo "incomplete: web/gen.sh found $BAD functions that failed to lift" > gen/all/stamp.txt; exit 1; }
echo "$LIFT" > gen/all/web-lift.txt
echo "web lift options: $LIFT"
[ "$DEFAULTS" != 1 ] || web/tools/gencache.sh put "$EXE"
