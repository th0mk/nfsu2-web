#!/bin/bash
# Compile the runtime and link the binaries (the shards must already be compiled)
#
# TWO SEPARATE BUILDS are made; the lifted shards are SHARED, only the runtime differs:
#   out/nfsu2_boot     the port: the game's own look by default; the optional
#                      graphics (msaa, mirror, texture_filtering, graphics
#                      preset) are runtime options, all off by default
#   out/nfsu2_hd_boot  research build (NFS_ENHANCED=1): the larger road
#                      reflection, upstream's SSR and ray-traced reflections,
#                      HD_* defaults.  Not packaged (measured and
#                      rejected)
#   out/nfsu2_all      test suite (with the original runtime)
#
#   ./link.sh          both
#   ./link.sh orig     original only
#   ./link.sh hd       HD only
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"
MCPU="${MCPU:-apple-m1}"; MINOS="${MINOS:-11.0}"   # see build.sh
OPT="${OPT:--O3 -mcpu=$MCPU -mmacosx-version-min=$MINOS}"
LDOPT="${LDOPT} -mmacosx-version-min=$MINOS"
WHICH="${1:-both}"
case "$WHICH" in both|orig|hd) ;; *) echo "usage: ./link.sh [orig|hd]"; exit 1 ;; esac

CSRC="mem memmap dispatch genexec heap watchdog win32 win32_ext win32_misc com debug game_ug2 debug_ug2 res fileio overrides d3dx_hle vsemu enhance perflog autotest lifecycle widescreen framerate dinput_pad pad_ug2 dikeys keyboard dinput_kbd career intro oracle drifttest discord roam roam_core roam_bsd roam_game mprace horn hornmenu optslider tonemenu radio powerunits drift carsfx racehorn racenet winsock online netrace net_vlan module modheap headlights language sms sponsors lobbyclick olfilter tunegraph menusize regional garage sndlead carshadow hiddenraces movie photo"
MSRC="metal audio bundle gamepad keyboard_mac fpscounter texrep"
# Unicorn 2.1.4 (native arm64, x86 target only) for the two test builds,
# ORACLE=1 and ASIPEER=1: by default the project's work/deps/unicorn-2.1.4
# (its README.txt says where it came from and how it was built), found from
# two folders above the checkout (../../work/deps or ../../deps).
# UNICORN_INC=<src>/include and UNICORN_LIB=<build>/libunicorn.a override it.
UDEP=""
for d in "$ROOT/../../work/deps/unicorn-2.1.4" "$ROOT/../../deps/unicorn-2.1.4"; do
  [ -d "$d" ] && { UDEP="$(cd "$d" && pwd)"; break; }
done
UNICORN_INC="${UNICORN_INC:-${UDEP:+$UDEP/include}}"; UNICORN_LIB="${UNICORN_LIB:-${UDEP:+$UDEP/lib/libunicorn.a}}"
# ORACLE=1: test binary with the differential x87 oracle (runtime/oracle.c).
ORACLE_CFLAGS=""; ORACLE_LIBS=""
if [ "${ORACLE:-0}" = 1 ]; then
  [ -f "$UNICORN_LIB" ] && [ -d "$UNICORN_INC" ] || { echo "ORACLE=1 needs Unicorn: work/deps/unicorn-2.1.4, or UNICORN_INC and UNICORN_LIB"; exit 1; }
  ORACLE_CFLAGS="-DNFSU2_ORACLE -I$UNICORN_INC"; ORACLE_LIBS="$UNICORN_LIB -lpthread"
fi
FW="-lm -framework Cocoa -framework Metal -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreServices -framework AudioToolbox -framework CoreAudio -framework GameController -framework Carbon"
SHARDS="$(ls gen/all/shard_*.c | sed -E "s|gen/all/(.*)\.c|out/obj/\1.o|" | tr "\n" " ") out/obj/fntab.o $([ -f gen/all/stubs.c ] && echo out/obj/stubs.o || true)"
# server.dll (gen.sh -> gen/server, build.sh -> out/obj/srv); a stub module
# when the game folder had no server.dll at gen time
[ -f gen/server/fntab.c ] || { echo "gen/server is missing: run ./gen.sh"; exit 1; }
SHARDS="$SHARDS $(ls gen/server/shard_*.c 2>/dev/null | sed -E "s|gen/server/(.*)\.c|out/obj/srv/\1.o|" | tr "\n" " ") out/obj/srv/fntab.o $([ -f gen/server/stubs.c ] && echo out/obj/srv/stubs.o || true)"

# $1 = object directory, $2 = NFS_ENHANCED value.  Files compile in parallel.
build_rt(){
  local od="$1" enh="$2" pids="" f
  mkdir -p "$od"
  for f in $CSRC; do
    [ -f "runtime/$f.c" ] || continue
    # roam_core, roam_game: no fused multiply-add, so their floats are
    # bit-identical to the x86 build of the same files (the Windows companion,
    # win/roam_asi)
    local fx=""; case "$f" in roam_core|roam_game) fx="-ffp-contract=off" ;; esac
    clang $OPT $fx -DNFS_ENHANCED=$enh $ORACLE_CFLAGS -Iruntime -Igen/all -c -o "$od/$f.o" "runtime/$f.c" & pids="$pids $!"
  done
  for f in $MSRC; do
    clang $OPT -DNFS_ENHANCED=$enh -fobjc-arc -Iruntime -Igen/all -c -o "$od/$f.o" "runtime/$f.m" & pids="$pids $!"
  done
  clang $OPT -Iruntime -Igen/all -c -o "$od/impargs.o" gen/all/impargs.c & pids="$pids $!"
  clang $OPT -Iruntime -c -o "$od/srv_impargs.o" gen/server/impargs.c & pids="$pids $!"
  clang $OPT -Iruntime -c -o "$od/srv_modinfo.o" gen/server/modinfo.c & pids="$pids $!"
  clang $OPT -DNFS_ENHANCED=$enh -Iruntime -Igen/all -c -o "$od/boot.o" tests/boot.c & pids="$pids $!"
  for p in $pids; do wait "$p" || { echo "compile error ($od)"; return 1; }; done
}
rt_objs(){ local od="$1" f o=""
  for f in $CSRC $MSRC impargs srv_impargs srv_modinfo; do [ -f "$od/$f.o" ] && o="$o $od/$f.o"; done
  echo "$o"; }

if [ "${ORACLE:-0}" = 1 ]; then
  # oracle test binary only: own objects, own name, the normal build untouched
  build_rt out/obj_oracle 0                  # enhance.o too: the options are in every build
  RT="$(rt_objs out/obj_oracle)"
  clang ${LDOPT} -o out/nfsu2_oracle $SHARDS $RT out/obj_oracle/boot.o $FW $ORACLE_LIBS
  echo "link OK: oracle (out/nfsu2_oracle)"
  exit 0
fi
if [ "$WHICH" != hd ]; then
  build_rt out/obj 0
  RT="$(rt_objs out/obj)"
  clang $OPT -Iruntime -Igen/all -c -o out/obj/main.o tests/main_all.c
  clang ${LDOPT} -o out/nfsu2_boot $SHARDS $RT out/obj/boot.o $FW
  clang ${LDOPT} -o out/nfsu2_all  $SHARDS $RT out/obj/main.o $FW
  # free-roam multiplayer core: unit tests (./run.sh test runs them)
  clang -O1 -ffp-contract=off -mmacosx-version-min=$MINOS -Iruntime -o out/roam_unit tests/roam_unit.c runtime/roam_core.c runtime/roam_bsd.c
  clang -O2 -ffp-contract=off -mmacosx-version-min=$MINOS -Iruntime -o out/roam_fakepeer tests/roam_fakepeer.c runtime/roam_core.c runtime/roam_bsd.c
  clang -O2 -ffp-contract=off -mmacosx-version-min=$MINOS -Iruntime -o out/roam_recv tests/roam_recv.c runtime/roam_core.c runtime/roam_bsd.c
  # ASIPEER=1: also out/nfsu2_asipeer, a "virtual Windows friend" (the Windows
  # plugin out/win/dbg/NFSU2FreeRoam.asi under Unicorn, runtime/roam_asipeer.c;
  # test tooling) and the plugin's tests (tests/asi_harness.c,
  # tests/roam_pe_selftest.c).  Unicorn: as for ORACLE=1 above.
  if [ "${ASIPEER:-0}" = 1 ]; then
    UINC="$UNICORN_INC"; ULIB="$UNICORN_LIB"
    [ -f "$ULIB" ] && [ -d "$UINC" ] || { echo "ASIPEER=1 needs Unicorn: work/deps/unicorn-2.1.4, or UNICORN_INC and UNICORN_LIB (now: '$UINC', '$ULIB')"; exit 1; }
    for f in tests/asi_vm tests/asi_win32 runtime/roam_asipeer; do
      clang $OPT -Iruntime -Itests -I"$UINC" -c -o "out/obj/$(basename $f).o" "$f.c"
    done
    clang ${LDOPT} -o out/nfsu2_asipeer $SHARDS $RT out/obj/asi_vm.o out/obj/asi_win32.o out/obj/roam_asipeer.o out/obj/boot.o "$ULIB" $FW -lpthread
    clang -O2 -g -ffp-contract=off -mmacosx-version-min=$MINOS -Iruntime -Itests -I"$UINC" -o out/asi_harness tests/asi_harness.c \
      tests/asi_vm.c tests/asi_win32.c runtime/roam_game.c runtime/roam_core.c "$ULIB" -lpthread -lm
    clang -O2 -ffp-contract=off -mmacosx-version-min=$MINOS -Iruntime -Itests -I"$UINC" -o out/roam_pe_selftest tests/roam_pe_selftest.c \
      tests/asi_vm.c tests/roam_selftest.c "$ULIB" -lpthread -lm
    echo "link OK: out/nfsu2_asipeer, out/asi_harness, out/roam_pe_selftest (Unicorn)"
  fi
  # Winsock / Netbios HLE unit harness (./run.sh test runs it)
  clang -O1 -Iruntime -o out/net_harness tests/net_harness.c runtime/winsock.c runtime/net_vlan.c runtime/mem.c
  echo "link OK: original (out/nfsu2_boot)"
fi
if [ "$WHICH" != orig ]; then
  build_rt out/obj_hd 1
  RT="$(rt_objs out/obj_hd)"
  clang ${LDOPT} -o out/nfsu2_hd_boot $SHARDS $RT out/obj_hd/boot.o $FW
  echo "link OK: HD (out/nfsu2_hd_boot)"
fi

# ISA semantics tests
# Do NOT PASS SILENTLY if the generator fails.  The error used to be swallowed:
# while isatest.py crashed for weeks, the old gen/isatest.c was compiled and
# said "439 passed", so the tests never exercised the current lifter at all.
# Now the stale binary is deleted, so ./run.sh test fails visibly.
if "$ROOT/.venv/bin/python" "$ROOT/tools/isatest.py" >/dev/null \
   && clang -O1 -Wno-unused-label -Iruntime -o out/isatest gen/isatest.c runtime/mem.c -lm; then
  :
else
  rm -f out/isatest
  echo "WARNING: the ISA tests could not be generated/compiled -- out/isatest deleted" >&2
fi
