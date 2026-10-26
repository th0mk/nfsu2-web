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
#   GAME=mw web/gen.sh <speed.exe>       another game (tools/gameconf.sh): games/<id>/game.conf names
#                                        the exe, the folders (GEN_DIR/all, GEN_DIR/server, OUT_DIR)
#                                        and the defaults of FTOL2INL and INLINE_LIST; a list that
#                                        names no function inlines nothing and is left off
# gen.sh stamps the result (gen/all/stamp.txt, tools/genstamp.sh), which
# web/build.sh checks against the checkout.  In a worktree gen/all and
# gen/server are links to the main clone's, which every other worktree builds
# against: replace them with directories first (or GEN_SHARED=1 to regenerate
# the shared ones on purpose).  The same holds for another game's GEN_DIR and
# every folder on its way; this script and gen.sh refuse a link there
# (tools/gameconf.sh).  The paths below are UG2's (GEN_DIR gen, OUT_DIR out).
set -e -o pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
. tools/gameconf.sh
G="$GEN_DIR"; O="$OUT_DIR"
EXE="${1:-${NFSU2_DATA:+$NFSU2_DATA/$GC_EXE_NAME}}"
[ -f "$EXE" ] || { echo "usage: web/gen.sh path/to/$GC_EXE_NAME"; exit 1; }
gameconf_nolink web/gen.sh
# before anything is written: a refused exe, or a core file holding a game's
# override or hook marker (tools/genstamp.sh refuses it), leaves the last lift as it was
gameconf_files "$EXE"
tools/genstamp.sh check
PY="$ROOT/.venv/bin/python"
mkdir -p "$O" "$G/all"
# never label a new lift with an old run's options, never reuse an old flag table
rm -f "$G/all/web-lift.txt" "$O/flagabi.json"
if [ "${WEB_LIFT:-opt}" = plain ]; then
  ./gen.sh "$EXE"
  LIFT=plain
else
  # flagabi.py needs the relocations, which gen.sh would only make later
  "$PY" tools/reloc2.py "$EXE"
  echo "FLAGABI: interprocedural flag liveness -> $O/flagabi.json"
  # flagabi.py sees each function's control flow as the lift will, switches included
  export JTSWITCH="${JTSWITCH:-1}"
  "$PY" tools/flagabi.py "$EXE" "$O/flagabi.json" > "$O/flagabi.log" 2>&1 || { tail -20 "$O/flagabi.log"; echo "web/gen.sh: flagabi.py failed"; exit 1; }
  grep -E "^summary|^  round" "$O/flagabi.log" || true
  [ -s "$O/flagabi.json" ] || { echo "web/gen.sh: flagabi.py wrote no $O/flagabi.json"; exit 1; }
  # the game's defaults (UG2: both on, the list tools/inline_hot.txt); given here, they win
  export LREGS="${LREGS:-1}" FTOL2INL="${FTOL2INL:-$GC_FTOL2INL}" INLINE_LIST="${INLINE_LIST-${GC_INLINE_LIST:+$ROOT/$GC_INLINE_LIST}}"
  if [ -n "$INLINE_LIST" ]; then
    [ -f "$INLINE_LIST" ] || { echo "web/gen.sh: no $INLINE_LIST (INLINE_LIST)"; exit 1; }
    N=$(sed 's/#.*//' "$INLINE_LIST" | grep -c '[^[:space:],]' || true)
    [ "$N" -gt 0 ] || { echo "INLINE_LIST: $INLINE_LIST names no function: off"; INLINE_LIST=; }
  fi
  FLAGABI="$ROOT/$O/flagabi.json" X87OPT=1 ./gen.sh "$EXE"
  LIFT="FLAGABI X87OPT$( [ "$JTSWITCH" = 1 ] && echo " JTSWITCH" || true)$( [ "$LREGS" = 1 ] && echo " LREGS" || true)$( [ "$FTOL2INL" = 1 ] && echo " FTOL2INL" || true)$( [ -n "$INLINE_LIST" ] && echo " INLINE_LIST" || true)"
fi
# a lift that failed function by function still says "gen OK": every function
# became an unsupported_insn(..., "lift-error") stub.  Refuse that.  A good
# lift of v1.2 NA has 8 such stubs (the same 8 on the Mac: addresses that are
# discovered as functions but never run); a broken one has thousands.
BAD=$(cat "$G"/all/shard_*.c | grep -c '"lift-error"' || true)
[ "$BAD" -le 64 ] || { echo "web/gen.sh: $BAD functions failed to lift (lift-error stubs in $G/all): not a usable lift"; rm -f "$G/all/fntab.c"
                       echo "incomplete: web/gen.sh found $BAD functions that failed to lift" > "$G/all/stamp.txt"; exit 1; }
echo "$LIFT" > "$G/all/web-lift.txt"
echo "web lift options: $LIFT"
