#!/bin/bash
# Differential test of Most Wanted's lifted exe (gen/mw/all) against its
# original x86 code under Unicorn: see tests/mworacle.c.
#   tests/mworacle.sh [cases-per-function] [functions] [seed]   (150, 1200, 1)
# tests/mworacle_pick.py picks the functions (a seeded sample of those that
# need no import and no HLE) into out/mw/oracle/sample.txt; the lift is
# compiled by tests/mwshards.sh.  Needs gen/mw/all (GAME=mw web/gen.sh), its
# flag table (out/mw/flagabi.json), speed.exe (NFSMW_DATA=<MW's folder>, else
# the ignored mw/ folder of this checkout or of the main clone) and a native
# arm64 Unicorn 2.1.x: by default the project's work/deps/unicorn-2.1.4 (as
# link.sh; or the native port's, ../nsfu2/work/deps next to the main clone),
# or UNICORN_INC=<unicorn source>/include UNICORN_LIB=<build>/libunicorn.a.
# Test-only binary (Unicorn is GPLv2): out/obj/mwtest/mworacle is never linked
# into the game or the app.  Exit 0 = no differing case.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
GAME=mw; . tools/gameconf.sh
MAIN="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$ROOT/.git")")"
UDEP=""
for d in "$ROOT/../../work/deps/unicorn-2.1.4" "$ROOT/../../deps/unicorn-2.1.4" "$MAIN/../nsfu2/work/deps/unicorn-2.1.4"; do
  [ -d "$d" ] && { UDEP="$(cd "$d" && pwd)"; break; }
done
UNICORN_INC="${UNICORN_INC:-${UDEP:+$UDEP/include}}"; UNICORN_LIB="${UNICORN_LIB:-${UDEP:+$UDEP/lib/libunicorn.a}}"
[ -d "$UNICORN_INC" ] && [ -f "$UNICORN_LIB" ] || { echo "needs Unicorn: work/deps/unicorn-2.1.4, or UNICORN_INC and UNICORN_LIB"; exit 1; }
MWDIR="${NFSMW_DATA:-}"
if [ -z "$MWDIR" ]; then
  for d in "$ROOT/mw/Need For Speed Most Wanted Black Edition" "$MAIN/mw/Need For Speed Most Wanted Black Edition"; do
    [ -f "$d/$GC_EXE_NAME" ] && { MWDIR="$d"; break; }
  done
fi
EXE="$MWDIR/$GC_EXE_NAME"
[ -f "$EXE" ] || { echo "no $GC_EXE_NAME (NFSMW_DATA=<MW's folder>)"; exit 1; }
gameconf_sha "$EXE" "$GC_EXE_SHA256" "$GC_EXE_NAME"
FLAGABI="$GC_OUT_DIR/flagabi.json"
[ -f "$FLAGABI" ] || { echo "no $FLAGABI (GAME=mw web/gen.sh writes it)"; exit 1; }
PY="$ROOT/.venv/bin/python"; [ -x "$PY" ] || PY=python3
mkdir -p out/obj/mwtest "$GC_OUT_DIR/oracle"
SAMPLE="$GC_OUT_DIR/oracle/sample.txt"
"$PY" -I tests/mworacle_pick.py "$EXE" gen/mw/all "$FLAGABI" "$SAMPLE" "${2:-1200}" "${3:-1}"
tests/mwshards.sh
clang -O2 -Iruntime -Igen/mw/all -DNFSU2_GAME_MW -I"$UNICORN_INC" -c -o out/obj/mwtest/mworacle.o tests/mworacle.c
clang -O2 -Iruntime -c -o out/obj/mwtest/mem.o runtime/mem.c
clang -o out/obj/mwtest/mworacle out/obj/mwtest/mworacle.o out/obj/mwtest/mem.o $(cat out/obj/mw/objs.list) "$UNICORN_LIB" -lpthread -lm
out/obj/mwtest/mworacle "$SAMPLE" "$EXE" "${1:-150}"
