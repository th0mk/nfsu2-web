#!/bin/bash
# Compiles the runtime, the C side of the page contract (runtime/web) and the lifted shards for the Nintendo Switch
# (devkitA64, libnx), into out/switch/{obj,srv,rt}.  Run from anywhere; it re-runs itself in the devkitpro/devkita64
# Docker image unless it is already inside one.
#   runtime/switch/build.sh            everything (incremental: an object is rebuilt when its source is newer)
#   runtime/switch/build.sh rt         the runtime only
#   runtime/switch/build.sh nro        compile what changed, link, and make out/switch/nfsu2.nro (the benchmark)
# Needs gen/all and gen/server (web/gen.sh <SPEED2.EXE>).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
if [ ! -d /opt/devkitpro/devkitA64 ]; then
  MSYS_NO_PATHCONV=1 exec docker run --rm -e NXGCONST -e NXGLOCAL -e NXO3 -v "$(pwd -W 2>/dev/null || pwd):/src" -w //src devkitpro/devkita64 bash runtime/switch/build.sh "$@"
fi
export PATH=/opt/devkitpro/devkitA64/bin:$PATH
# the build number: runtime/switch/BUILD, +1 on every nro build; it is in the NACP version (the homebrew menu shows it) and in the log
if [ "${1:-}" = nro ]; then N=$(( $(cat runtime/switch/BUILD 2>/dev/null || echo 0) + 1 )); echo $N > runtime/switch/BUILD; else N=$(cat runtime/switch/BUILD 2>/dev/null || echo 0); fi
[ -f gen/all/fntab.c ] || { echo "gen/all missing: run web/gen.sh <path to SPEED2.EXE>"; exit 1; }
CC=aarch64-none-elf-gcc
CFLAGS="-O2 -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections -fno-strict-aliasing -fsigned-char \
 -fno-delete-null-pointer-checks -specs=/opt/devkitpro/libnx/switch.specs -D__SWITCH__ -DNX_BUILD=$N -DNFS_ENHANCED=0 \
 -include runtime/switch/switch_plat.h -Iruntime/switch/compat -Iruntime/web/compat -Iruntime -I/opt/devkitpro/libnx/include \
 -I/opt/devkitpro/portlibs/switch/include -w"
mkdir -p out/switch/obj out/switch/srv out/switch/rt
[ "${1:-}" = nro ] && rm -f out/switch/rt/nx_nx_main.o    # its __DATE__ __TIME__ is the build id the log prints: rebuilt every time
J=$(nproc)
# NXGLOCAL: local restrict guest base in LR_ENTER (preferred). NXGCONST: older const-pointer alias (0% on yuzu).
GC=""; [ "${NXGCONST:-0}" = 1 ] && GC="-DNFSU2_NX_GCONST"; [ "${NXGLOCAL:-0}" = 1 ] && GC="-DNFSU2_NX_GLOCAL"
# NXO3=1: compile the lifted shards at -O3 (runtime stays -O2). Opt-in until yuzu shows a real gain.
SO3=""; [ "${NXO3:-0}" = 1 ] && SO3="-O3" && rm -f out/switch/obj/shard_*.o out/switch/srv/shard_*.o
# Force shard rebuild when cpu.h / this flag set changes (GLOCAL etc. live in cpu.h; the rule below already watches it).
# one job per line: "<source> <object> <extra flags>"
jobs=$(mktemp)
if [ "${1:-all}" = all ] || [ "${1:-}" = nro ]; then
  for f in gen/all/shard_*.c gen/all/fntab.c gen/all/impargs.c gen/all/stubs.c; do
    [ -f "$f" ] && echo "$f out/switch/obj/$(basename $f .c).o -Igen/all ${GC} ${SO3}" >> $jobs
  done
  for f in gen/server/shard_*.c gen/server/fntab.c gen/server/impargs.c gen/server/modinfo.c gen/server/stubs.c; do
    [ -f "$f" ] && echo "$f out/switch/srv/$(basename $f .c).o -Igen/server ${GC} ${SO3}" >> $jobs
  done
fi
RT="mem dispatch genexec heap watchdog win32 win32_ext com debug res fileio overrides d3dx_hle vsemu oracle autotest lifecycle widescreen framerate dinput_pad dikeys keyboard dinput_kbd career intro drifttest roam roam_core roam_game mprace horn hornmenu optslider tonemenu powerunits radio drift carsfx racehorn racenet winsock online netrace module modheap enhance headlights language sms sponsors lobbyclick olfilter tunegraph menusize regional garage sndlead carshadow hiddenraces carspot movie photo"
for f in $RT; do
  s=runtime/$f.c; [ -f $s ] || continue
  fx=""; case "$f" in roam_core|roam_game) fx="-ffp-contract=off" ;; esac
  echo "$s out/switch/rt/$f.o -Igen/all${fx:+ $fx}" >> $jobs
done
for s in runtime/web/*.c; do
  f=$(basename $s .c); case "$f" in thread_exit|memcmp|tz) continue ;; esac   # wasm-only: its pthread_exit trampoline, SIMD memcmp, the page's time zone
  echo "$s out/switch/rt/web_$f.o -Igen/all" >> $jobs
done
for s in runtime/switch/*.c; do case "$s" in */host_linux.c|*/gltest.c) continue ;; esac
  fx="-Igen/all"; case "$s" in */lan_nx.c) fx="-Ihomebrew/switch-net -DN2N_TLS" ;; esac      # the network consumer: the n2n client's header, with TLS
  [ -f "$s" ] && echo "$s out/switch/rt/nx_$(basename $s .c).o $fx" >> $jobs; done
# the n2n client (homebrew/switch-net): the WebSocket to the server's virtual LAN, over TCP and mbedTLS
for f in n2n n2n_sock n2n_tls_mbedtls; do echo "homebrew/switch-net/$f.c out/switch/rt/n2n_$f.o -Ihomebrew/switch-net -DN2N_TLS -DN2N_BUILD_EPOCH=$(date +%s)LL" >> $jobs; done
export CC CFLAGS
cat $jobs | xargs -P $J -L1 sh -c '[ "$1" -nt "$2" ] || [ ! -s "$2" ] || [ runtime/cpu.h -nt "$2" ] || exit 0; $CC $CFLAGS $3 $4 -c -o "$2" "$1" 2> "$2.err" && rm -f "$2.err" || { echo "FAIL $1"; exit 255; }' sh
rm -f $jobs
echo "build OK: $(ls out/switch/obj/*.o out/switch/srv/*.o out/switch/rt/*.o 2>/dev/null | wc -l) objects"

if [ "${1:-}" = nro ]; then
  # headless: runtime/web/null_platform.c stands in for the renderer, the sound and the pad; the browser's
  # entry point and the mailboxes' page-side consumers are not linked
  OBJS="$(ls out/switch/obj/*.o out/switch/srv/*.o out/switch/rt/*.o | grep -vE 'rt/web_(main_web|input_web|texrep_web).o')"
  $CC -specs=/opt/devkitpro/libnx/switch.specs -march=armv8-a+crc+crypto -mtp=soft -fPIE -Wl,--gc-sections -Wl,--wrap=open,--wrap=close,--wrap=read,--wrap=lseek,--wrap=fstat,--wrap=stat,--wrap=opendir,--wrap=readdir,--wrap=closedir,--wrap=rewinddir,--wrap=fopen,--wrap=pthread_create -o out/switch/nfsu2.elf       $OBJS -L/opt/devkitpro/libnx/lib -L/opt/devkitpro/portlibs/switch/lib -lglad -lEGL -lglapi -ldrm_nouveau -lmbedtls -lmbedx509 -lmbedcrypto -lnx -lstdc++ -lm -lpthread 2> out/switch/link.log       || { grep -E "undefined reference|multiple definition|error" out/switch/link.log | sed 's/.*: //' | sort | uniq -c | sort -rn | head -40; echo "link FAILED"; exit 1; }
  # a NACP (name, author, version) and an icon: homebrew menus read them, and a menu (sphaira) crashed on an NRO without
  /opt/devkitpro/tools/bin/nacptool --create "Need for Speed: Underground 2" "Electronic Arts" "b$N" out/switch/nfsu2.nacp
  /opt/devkitpro/tools/bin/elf2nro out/switch/nfsu2.elf out/switch/nfsu2.nro --nacp=out/switch/nfsu2.nacp --icon=runtime/switch/icon.jpg
  ls -l out/switch/nfsu2.nro
fi
