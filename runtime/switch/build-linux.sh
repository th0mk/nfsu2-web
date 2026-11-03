#!/bin/bash
# The Switch benchmark on Linux x86-64 (WSL): same sources and branches (-D__SWITCH__, switch_plat.h), host_linux.c in
# place of nx_main.c/nx_host.c.  Output goes to ~/nfsu2-linux-build (a Linux file system: /mnt is slow).  Needs gen/all and
# gen/server.   runtime/switch/build-linux.sh && ~/nfsu2-linux-build/nfsu2_bench $NFSU2_GAME_DIR/SPEED2.EXE tests/scripts/race_long.txt 180
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
OUT="${OUT:-$HOME/nfsu2-linux-build}"; mkdir -p "$OUT/obj" "$OUT/inc"
rm -rf "$OUT/inc/os" "$OUT/inc/pthread"; cp -r runtime/switch/compat/os runtime/switch/compat/pthread "$OUT/inc/"
# NXGLOCAL=1: local restrict guest base in LR_ENTER (see cpu.h). Pass through from the environment.
GC=""; [ "${NXGLOCAL:-0}" = 1 ] && GC="-DNFSU2_NX_GLOCAL"
CFLAGS="-O2 -fno-strict-aliasing -fno-delete-null-pointer-checks -fsigned-char -D__SWITCH__ -DNFSU2_LINUX_HOST=1 -DNFS_ENHANCED=0 $GC -w \
 -include sys/socket.h -include runtime/switch/switch_plat.h -I$OUT/inc -Iruntime/web/compat -Iruntime"
jobs=$(mktemp)
for f in gen/all/shard_*.c gen/all/fntab.c gen/all/impargs.c gen/all/stubs.c; do [ -f "$f" ] && echo "$f $OUT/obj/all_$(basename $f .c).o -Igen/all" >> $jobs; done
for f in gen/server/shard_*.c gen/server/fntab.c gen/server/impargs.c gen/server/modinfo.c gen/server/stubs.c; do [ -f "$f" ] && echo "$f $OUT/obj/srv_$(basename $f .c).o -Igen/server" >> $jobs; done
RT="mem dispatch genexec heap watchdog win32 win32_ext com debug res fileio overrides d3dx_hle vsemu oracle autotest lifecycle widescreen framerate dinput_pad dikeys keyboard dinput_kbd career intro drifttest roam roam_core roam_game mprace horn hornmenu optslider tonemenu powerunits radio drift carsfx racehorn racenet winsock online netrace module modheap enhance headlights language sms sponsors lobbyclick olfilter tunegraph menusize regional garage sndlead carshadow hiddenraces carspot movie photo"
for f in $RT; do s=runtime/$f.c; [ -f $s ] || continue; fx=""; case "$f" in roam_core|roam_game) fx="-ffp-contract=off" ;; esac
  echo "$s $OUT/obj/rt_$f.o -Igen/all${fx:+ $fx}" >> $jobs; done
for s in runtime/web/*.c; do f=$(basename $s .c); case "$f" in thread_exit|memcmp|tz|main_web|audio_web|input_web|texrep_web) continue ;; esac
  echo "$s $OUT/obj/web_$f.o -Igen/all" >> $jobs; done
echo "runtime/switch/game_main.c $OUT/obj/game_main.o -Igen/all" >> $jobs
echo "runtime/switch/gpu_native.c $OUT/obj/gpu_native.o -Igen/all" >> $jobs
echo "runtime/switch/gpu_gl.c $OUT/obj/gpu_gl.o -Igen/all" >> $jobs
echo "runtime/switch/wgsl2glsl.c $OUT/obj/wgsl2glsl.o -Igen/all" >> $jobs
echo "runtime/switch/host_linux.c $OUT/obj/host_linux.o -Igen/all" >> $jobs
export CFLAGS
cat $jobs | xargs -P "$(nproc)" -L1 sh -c '[ -s "$2" ] && [ ! "$1" -nt "$2" ] && [ ! runtime/cpu.h -nt "$2" ] && exit 0; gcc $CFLAGS $3 $4 -c -o "$2" "$1" 2> "$2.err" && rm -f "$2.err" || { echo "FAIL $1"; exit 255; }' sh
rm -f $jobs
gcc -o "$OUT/nfsu2_bench" "$OUT"/obj/*.o -lm -lpthread -ldl -lEGL -lGL 2> "$OUT/link.log" || { grep -E "undefined reference|multiple definition" "$OUT/link.log" | sed 's/.*: //' | sort | uniq -c | sort -rn | head -30; echo "link FAILED"; exit 1; }
echo "built $OUT/nfsu2_bench"
