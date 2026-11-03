#!/bin/bash
# What a lift (gen/all, gen/server) depends on in the checkout, for gen.sh and
# the builds that use its output:
#   tools/genstamp.sh overrides   gen.sh's OVERRIDE list: every sub_X_orig the
#                                 runtime names, plus d3dx_hle.c's DX_WRAP table
#   tools/genstamp.sh hooks       the HOOK_SITE(0x...) instruction hooks of the runtime
#   tools/genstamp.sh sources     the stamp's source lines: hashes of both lists and
#                                 of the lifter's own code
# gen.sh writes the source lines, plus the lift options it ran with (recorded,
# not compared: a plain lift is a valid choice), into gen/all/stamp.txt and
# gen/server/stamp.txt when it has finished; web/build.sh recomputes the
# source lines and refuses a gen/all made from other ones (a stale gen links
# with duplicate or missing sub_X, or silently drops hooks).
set -e -o pipefail
cd "$(dirname "$0")/.."
overrides(){
  { { grep -ohE 'sub_00[0-9a-f]{6}_orig' runtime/*.c runtime/*.m || true; } | sed -E 's/sub_00([0-9a-f]{6})_orig/\1/';
    { grep -ohE '^DX_WRAP\([A-Z0-9_]+, *00[0-9a-f]{6}\)' runtime/d3dx_hle.c || true; } | sed -E 's/.*00([0-9a-f]{6})\)/\1/'; } | sort -u | paste -sd, -
}
hooks(){
  { { grep -ohE 'HOOK_SITE\(0x00[0-9a-f]{6}\)' runtime/*.c || true; } | sed -E 's/HOOK_SITE\((0x[0-9a-f]+)\)/\1/'; } | sort -u | paste -sd, -
}
sha(){ shasum -a 256 | cut -c1-16; }
# the lifter: everything that decides gen/all's and gen/server's C
LIFTER="tools/lift.py tools/split_reloc.py runtime/splitscreen_mem.h tools/lregs.py tools/genall.py tools/inline_hot.txt tools/retpast.txt tools/reloc2.py tools/argcount.py tools/flagabi.py tools/prepare_module.py"
case "${1:-}" in
  overrides) overrides ;;
  hooks) hooks ;;
  sources)
    ov=$(overrides); hk=$(hooks)
    echo "overrides $(printf '%s' "$ov" | sha) ($(printf '%s' "$ov" | tr ',' '\n' | grep -c .))"
    echo "hooks $(printf '%s' "$hk" | sha) ($(printf '%s' "$hk" | tr ',' '\n' | grep -c .))"
    echo "lifter $(cat $LIFTER | sha)"
    ;;
  *) echo "usage: tools/genstamp.sh overrides|hooks|sources"; exit 2 ;;
esac
