#!/bin/bash
# What a lift (gen/all, gen/server) depends on in the checkout, for gen.sh and
# the builds that use its output:
#   tools/genstamp.sh overrides   gen.sh's OVERRIDE list: every sub_X_orig the
#                                 game's runtime files name, plus their DX_WRAP table
#   tools/genstamp.sh hooks       the HOOK_SITE(0x...) instruction hooks of those files
#   tools/genstamp.sh origs       the sub_X_orig names alone (web/tools/gencheck.sh)
#   tools/genstamp.sh sources     the stamp's source lines: hashes of both lists and
#                                 of the lifter's own code
#   tools/genstamp.sh check       nothing but the core check below
# GAME=<id> (default ug2, tools/gameconf.sh) picks the game's files:
#   ug2   sub_X_orig in runtime/*.c and runtime/*.m, ^DX_WRAP( in
#         runtime/d3dx_hle.c, HOOK_SITE in runtime/*.c, as always (runtime/<id>/
#         of another game is never seen, so UG2's stamp stays what it was)
#   <id>  all three in runtime/<id>/*.c; the source lines start with "game <id>",
#         and the lifter's hash covers the game's game.conf and inline list
# Every command first refuses a core file holding "_orig", "DX_WRAP" or
# "HOOK_SITE", comments included: runtime/core.list's files and every
# runtime/web/*.c are shared by every game, and a marker there would put one
# game's override or hook into another game's lift.
# gen.sh writes the source lines, plus the lift options it ran with (recorded,
# not compared: a plain lift is a valid choice), into gen/all/stamp.txt and
# gen/server/stamp.txt when it has finished; web/build.sh recomputes the
# source lines and refuses a gen/all made from other ones (a stale gen links
# with duplicate or missing sub_X, or silently drops hooks).
set -e -o pipefail
cd "$(dirname "$0")/.."
. tools/gameconf.sh
shopt -s nullglob
case "$GAME" in
  ug2) OVF=(runtime/*.c runtime/*.m); DXF=(runtime/d3dx_hle.c); HKF=(runtime/*.c) ;;
  *) OVF=(runtime/"$GAME"/*.c); DXF=("${OVF[@]}"); HKF=("${OVF[@]}") ;;
esac
shopt -u nullglob
# marks <regex> <file>...: every match in those files (none without files)
marks(){ local re="$1"; shift; [ $# -gt 0 ] || return 0; grep -ohE "$re" "$@" || true; }
overrides(){
  { marks 'sub_00[0-9a-f]{6}_orig' "${OVF[@]}" | sed -E 's/sub_00([0-9a-f]{6})_orig/\1/';
    marks '^DX_WRAP\([A-Z0-9_]+, *00[0-9a-f]{6}\)' "${DXF[@]}" | sed -E 's/.*00([0-9a-f]{6})\)/\1/'; } | sort -u | paste -sd, -
}
hooks(){
  { marks 'HOOK_SITE\(0x00[0-9a-f]{6}\)' "${HKF[@]}" | sed -E 's/HOOK_SITE\((0x[0-9a-f]+)\)/\1/'; } | sort -u | paste -sd, -
}
origs(){ marks 'sub_00[0-9a-f]{6}_orig' "${OVF[@]}" | sort -u; }
core_check(){
  [ -f runtime/core.list ] || { echo "genstamp: no runtime/core.list"; return 1; }
  local f files=() hits
  for f in $(sed 's/#.*//' runtime/core.list | awk 'NF { print $1 }'); do
    if [ -f "runtime/$f" ]; then files+=("runtime/$f"); fi   # a missing one: tools/gamelit.sh says so
  done
  for f in runtime/web/*.c; do if [ -f "$f" ]; then files+=("$f"); fi; done
  [ ${#files[@]} -gt 0 ] || return 0
  hits=$(grep -nHE '_orig|DX_WRAP|HOOK_SITE' "${files[@]}" || true)
  [ -n "$hits" ] || return 0
  echo "genstamp: a core file (runtime/core.list, runtime/web/*.c: every game's) holds a game's override or hook marker;"
  echo "  sub_X_orig, DX_WRAP and HOOK_SITE go in the game's own files, and a comment counts too:"
  sed 's/^/  /' <<< "$hits"
  return 1
}
sha(){ shasum -a 256 | cut -c1-16; }
# the lifter: everything that decides gen/all's and gen/server's C (UG2: the
# same files as always; its inline list is tools/inline_hot.txt)
LIFTER="tools/lift.py tools/lregs.py tools/genall.py $GC_INLINE_LIST tools/retpast.txt tools/reloc2.py tools/argcount.py tools/flagabi.py tools/prepare_module.py"
[ "$GAME" = ug2 ] || LIFTER="$LIFTER games/$GAME/game.conf tools/gameconf.sh"
case "${1:-}" in
  overrides|hooks|origs|sources|check) ;;
  *) echo "usage: tools/genstamp.sh overrides|hooks|origs|sources|check"; exit 2 ;;
esac
core_check >&2 || exit 1
case "$1" in
  overrides) overrides ;;
  hooks) hooks ;;
  origs) origs ;;
  sources)
    ov=$(overrides); hk=$(hooks)
    [ "$GAME" = ug2 ] || echo "game $GAME"
    echo "overrides $(printf '%s' "$ov" | sha) ($(printf '%s' "$ov" | tr ',' '\n' | grep -c .))"
    echo "hooks $(printf '%s' "$hk" | sha) ($(printf '%s' "$hk" | tr ',' '\n' | grep -c .))"
    echo "lifter $(cat $LIFTER | sha)"
    ;;
  check) ;;
esac
