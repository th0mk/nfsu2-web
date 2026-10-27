#!/bin/bash
# Is the lifted C (gen/all, gen/server) the one this checkout's runtime needs?
# web/build.sh runs this before it compiles anything; exit 1 with what to do.
#
#   web/tools/gencheck.sh                 check gen/all and gen/server
#   GAME=mw web/tools/gencheck.sh         check another game's (games/<id>/game.conf
#                                         GEN_DIR: gen/mw/all and gen/mw/server)
#   GEN=<dir> web/tools/gencheck.sh       check <dir>/all and <dir>/server (the check's own test)
#   GEN_STALE_OK=1 ...                    report a stale gen but pass (experiments)
#
# 0. No core file holds a game's override or hook marker (tools/genstamp.sh
#    check: core.list's files and runtime/web/*.c are every game's).
# 1. The stamp (gen/all/stamp.txt) is this game's: its game line names the
#    game (UG2's stamps have none), and its exe line holds the SHA-256
#    prefixes games/<id>/game.conf gives for the exe and server.dll.  Another
#    game's or another exe's gen is refused, GEN_STALE_OK or not.  A lift of
#    another game than UG2 has gen/all/gameinfo.h (tools/genall.py: the exe's
#    values for runtime/cpu.h, which takes UG2's without one).
# 2. Every sub_X_orig the game's runtime files name is declared in
#    gen/all/decls.h (gen.sh renames the lifted function the runtime
#    overrides; a gen/all older than the runtime fails the link with duplicate
#    or missing sub_X).
# 3. gen.sh's stamp (tools/genstamp.sh): the override list, the hook list (a
#    HOOK_SITE change does not break the link: the hook silently goes) and the
#    lifter of this checkout are the ones gen/all was made from, the lift
#    finished, and gen/server comes from the same run.  A UG2 gen/all without
#    a stamp (made before gen.sh wrote one) gets a note; another game's is
#    refused.
# 4. The D3DX oracle (gen/<game>/d3dx, linked when the game has a D3DX_BASE,
#    the folder has fntab.c and D3DX_ORACLE is not 0, as web/build.sh decides)
#    has a stamp of its own finished lift: this game's line and this
#    checkout's override list, hook list and lifter.  One without a stamp is
#    refused; a stale one passes with GEN_STALE_OK=1.
set -e -o pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
. tools/gameconf.sh
G="${GEN:-$GC_GEN_DIR}"
LIFT="$( [ "$GAME" = ug2 ] || echo "GAME=$GAME ")web/gen.sh"; X="$GC_EXE_NAME"
R=$(tools/genstamp.sh check 2>&1) || { echo "$R"; exit 1; }
[ -f "$G/all/decls.h" ] || { echo "$G/all missing: run $LIFT <path to $X>"; exit 1; }
S="$G/all/stamp.txt"
if [ -f "$S" ]; then
  if grep -q '^incomplete' "$S"; then
    echo "$G/all is not a finished lift ($(head -1 "$S")): run $LIFT <$X>"; exit 1; fi
  HAVE_GAME="$(sed -n 's/^game //p' "$S" | head -1)"; HAVE_GAME="${HAVE_GAME:-ug2}"
  if [ "$HAVE_GAME" != "$GAME" ]; then
    echo "$G/all is a lift of $HAVE_GAME, not of $GAME (its stamp): GAME=$HAVE_GAME web/tools/gencheck.sh checks it; run $LIFT <path to $X> for $GAME"; exit 1; fi
  h16(){ echo "$1" | tr 'A-F' 'a-f' | cut -c1-16; }
  EXE_LINE="$(grep -m1 '^exe ' "$S" || true)"
  set -- $EXE_LINE   # exe <sha16> [server.dll <sha16>]
  if [ "${2:-}" != "$(h16 "$GC_EXE_SHA256")" ] || { [ -n "${4:-}" ] && [ "$4" != "$(h16 "$GC_SRV_SHA256")" ]; }; then
    echo "$G/all was not lifted from $GAME's files (its stamp: ${EXE_LINE:-no exe line}; $GAME_CONF: $X $(h16 "$GC_EXE_SHA256"), server.dll $(h16 "$GC_SRV_SHA256")): run $LIFT <path to $X>"
    exit 1
  fi
fi
# the exe's values for runtime/cpu.h (tools/genall.py); without them the runtime builds with UG2's
if [ "$GAME" != ug2 ] && [ ! -f "$G/all/gameinfo.h" ]; then
  echo "$G/all has no gameinfo.h (made before tools/genall.py wrote one): run $LIFT <path to $X>"; exit 1; fi
STALE=$(tools/genstamp.sh origs | while read -r f; do
  grep -q "void $f(" "$G/all/decls.h" || echo "$f"; done)
[ -z "$STALE" ] || { echo "$G/all is older than the runtime (not lifted as _orig: $(echo $STALE | tr '\n' ' ')): run $LIFT <$X>"; exit 1; }
if [ ! -f "$S" ]; then
  if [ "$GAME" = ug2 ]; then
    echo "note: $G/all has no stamp (made before gen.sh wrote one): only its _orig declarations were checked; $LIFT <$X> makes a stamped one"
    exit 0
  fi
  echo "$G/all has no stamp, so no finished gen.sh lift: run $LIFT <path to $X>"; exit 1
fi
WANT="$(tools/genstamp.sh sources | grep -E '^(overrides|hooks|lifter) ')"
HAVE="$(grep -E '^(overrides|hooks|lifter) ' "$S" || true)"
if [ "$WANT" != "$HAVE" ]; then
  echo "$G/all is stale: it was made from another"
  for k in overrides hooks lifter; do
    [ "$(grep "^$k " <<< "$WANT" || true)" = "$(grep "^$k " <<< "$HAVE" || true)" ] && continue
    case $GAME.$k in
      ug2.overrides) echo "  override list (sub_X_orig in runtime/, DX_WRAP in d3dx_hle.c: tools/genstamp.sh overrides)" ;;
      ug2.hooks) echo "  hook list (HOOK_SITE in runtime/: tools/genstamp.sh hooks)" ;;
      *.overrides) echo "  override list (sub_X_orig and DX_WRAP in runtime/$GAME/*.c: GAME=$GAME tools/genstamp.sh overrides)" ;;
      *.hooks) echo "  hook list (HOOK_SITE in runtime/$GAME/*.c: GAME=$GAME tools/genstamp.sh hooks)" ;;
      *.lifter) echo "  lifter (tools/lift.py, lregs.py, genall.py, reloc2.py, argcount.py, flagabi.py, prepare_module.py, gameconf.sh, $GAME_CONF${GC_INLINE_LIST:+, $GC_INLINE_LIST}, $GC_RETPAST_LIST)" ;;
    esac
  done
  if [ "${GEN_STALE_OK:-}" = 1 ]; then echo "  GEN_STALE_OK=1: building anyway"
  else echo "  run $LIFT <path to $X> (GEN_STALE_OK=1 builds anyway)"; exit 1; fi
fi
cmp -s "$S" "$G/server/stamp.txt" || {
  echo "$G/server is not from the gen.sh run that made $G/all ($G/server/stamp.txt differs or is missing): run $LIFT <$X>"; exit 1; }
if [ -n "${GC_D3DX_BASE:-}" ] && [ -f "$G/d3dx/fntab.c" ] && [ "${D3DX_ORACLE:-1}" != 0 ]; then
  D="$G/d3dx/stamp.txt"
  if [ ! -f "$D" ] || grep -q '^incomplete' "$D"; then
    echo "$G/d3dx (the D3DX oracle, linked unless D3DX_ORACLE=0) has no stamp of a finished lift: D3DX_DLL=<d3dx9_26.dll> $LIFT <$X> lifts it again"; exit 1; fi
  if [ "$(grep -E '^(game|overrides|hooks|lifter) ' "$D" || true)" != "$(tools/genstamp.sh sources | grep -E '^(game|overrides|hooks|lifter) ')" ]; then
    echo "$G/d3dx (the D3DX oracle) is stale: its stamp names another game, override list, hook list or lifter than this checkout's (tools/genstamp.sh sources)"
    if [ "${GEN_STALE_OK:-}" = 1 ]; then echo "  GEN_STALE_OK=1: building anyway"
    else echo "  D3DX_DLL=<d3dx9_26.dll> $LIFT <$X> lifts it again (GEN_STALE_OK=1 builds anyway, D3DX_ORACLE=0 builds without it)"; exit 1; fi
  fi
fi
