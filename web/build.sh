#!/bin/bash
# Build the game module for the browser: out/web/nfsu2.wasm (+ nfsu2.json).
#
#   web/build.sh            lifted shards (incremental) + runtime + link
#   FULL=1 web/build.sh     recompile everything
#   OPT=-O2 web/build.sh    another optimisation level (default -O3)
#
# Needs gen/all and gen/server (web/gen.sh <SPEED2.EXE>: ./gen.sh with the web's lift
# options; it lifts server.dll too) and wasi-sdk in deps/ (see docs/web).
# Toolchain: wasi-sdk (clang + wasm-ld), target wasm32-wasip1-threads, run by
# the page's own host (web/src/host): no Emscripten.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
WASI="${WASI:-$(ls -d "$ROOT"/deps/wasi-sdk-*-arm64-macos "$ROOT"/deps/wasi-sdk-*-x86_64-linux 2>/dev/null | tail -1)}"
[ -x "$WASI/bin/clang" ] || { echo "wasi-sdk not found in deps/ (set WASI=...)"; exit 1; }
# guest memory accesses must keep their may_alias types (cpu.h)
WASI="$WASI" web/tools/aliasguard.sh > /dev/null || { WASI="$WASI" web/tools/aliasguard.sh; exit 1; }
[ -f gen/all/fntab.c ] || { echo "gen/all missing: run web/gen.sh <path to SPEED2.EXE>"; exit 1; }
[ -f gen/server/fntab.c ] || { echo "gen/server missing: run web/gen.sh <path to SPEED2.EXE> (it lifts server.dll too)"; exit 1; }
# The lift must be the one this runtime needs: its _orig declarations, and
# gen.sh's stamp (override list, hook list, lifter; web/tools/gencheck.sh)
web/tools/gencheck.sh
LIFT="$(cat gen/all/web-lift.txt 2>/dev/null || echo unknown)"
[ "$LIFT" != unknown ] || echo "note: gen/all was not made by web/gen.sh (lift options unknown; web/gen.sh turns on FLAGABI and X87OPT)"
CC="$WASI/bin/clang"; OBJDUMP="$WASI/bin/llvm-objdump"
J="${J:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"
OPT="${OPT:--O3}"
OUT="$ROOT/out/web"; mkdir -p "$OUT/obj" "$OUT/obj/srv" "$OUT/rt"
OBJCACHE="${OBJCACHE:-$HOME/.cache/nsfu2-web/obj}"; mkdir -p "$OBJCACHE"

# The guest owns [0, GUEST_END) of linear memory (runtime/cpu.h); the
# runtime's data starts there.  Initial memory covers the guest span plus room
# for the runtime heap; it can grow to 4 GB.
GUEST_END=671088640   # 0x28000000
INITIAL_MB="${INITIAL_MB:-1024}"
TARGET="--target=wasm32-wasip1-threads -pthread"
FEAT="-msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false"
DEFS="-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS"
# -fno-delete-null-pointer-checks: guest address 0 is real wasm memory (cpu.h)
CFLAGS="$TARGET $OPT $FEAT $DEFS -fno-delete-null-pointer-checks -include runtime/web/compat/wasm_plat.h -Iruntime/web/compat -Iruntime -Igen/all"
# the lifted shards only: guest memory as an object of its own (cpu.h, NFSU2_GUEST_OBJ)
SHARDFLAGS="-DNFSU2_GUEST_OBJ"
export CC OBJDUMP CFLAGS SHARDFLAGS OUT FULL MEMCHECK OBJCACHE
HDRSUM="$( (cat runtime/cpu.h runtime/web/compat/wasm_plat.h; echo "$CFLAGS $SHARDFLAGS") | shasum | cut -c1-16)"; export HDRSUM

# ---- lifted shards (content-hash incremental, as build.sh) ----
# gen/all (SPEED2.EXE) -> out/web/obj, gen/server (server.dll, the LAN server
# lifted as a second module, see gen.sh) -> out/web/obj/srv: one pool, the same
# rule for both.  A shard's key is its source, its own module's decls.h and the
# flags, so the two modules never share cache entries by accident.
{ (cd gen/all && ls shard_*.c fntab.c impargs.c $( [ -f stubs.c ] && echo stubs.c ) | sed "s|^|all |")
  (cd gen/server && ls shard_*.c fntab.c impargs.c modinfo.c $( [ -f stubs.c ] && echo stubs.c ) | sed "s|^|server |"); } |
  xargs -P "$J" -n 2 sh -c '
  case "$1" in all) od="$OUT/obj" ;; *) od="$OUT/obj/srv" ;; esac
  src="gen/$1/$2"; obj="$od/$(basename "$2" .c).o"
  sum=$( (cat "$src" "gen/$1/decls.h"; echo "$HDRSUM") | shasum | cut -c1-40)
  if [ -z "$FULL" ] && [ -f "$obj" ] && [ "$(cat "$obj.sum" 2>/dev/null)" = "$sum" ]; then exit 0; fi
  mc=""; case ",$MEMCHECK," in *",$(basename "$2" .c),"*) mc="-DNFSU2_MEMCHECK" ;; esac
  [ -n "$mc" ] && sum="$sum-memchk"
  if [ -z "$FULL" ] && [ -f "$obj" ] && [ "$(cat "$obj.sum" 2>/dev/null)" = "$sum" ]; then exit 0; fi
  # shared, content-addressed cache: every worktree with the same shard, cpu.h
  # and flags reuses one compile
  if [ -z "$FULL" ] && [ -f "$OBJCACHE/$sum.o" ]; then ln -f "$OBJCACHE/$sum.o" "$obj" 2>/dev/null || cp "$OBJCACHE/$sum.o" "$obj"
    echo "$sum" > "$obj.sum"; exit 0; fi
  o=$($CC $CFLAGS $SHARDFLAGS $mc -I"gen/$1" -Wno-unused-label -Wno-parentheses-equality -c -o "$obj" "$src" 2>&1); r=$?
  [ -z "$o" ] || printf "%s\n" "$o" | head -5
  [ $r -eq 0 ] || { echo "FAILED: $src" >&2; exit $r; }
  # a lookup table of guest addresses (g_guest + K in data) links as zeros
  # (cpu.h): compile the shard again without jump tables, which has none
  guestdata() { "$OBJDUMP" -r "$obj" | awk "/^RELOCATION RECORDS FOR \\[DATA\\]/ { d = 1; next } /^RELOCATION RECORDS/ { d = 0 }
    d && \$3 ~ /^g_guest[+-]/ && \$3 != \"g_guest+0\" { f = 1 } END { exit !f }"; }
  if guestdata; then
    $CC $CFLAGS $SHARDFLAGS $mc -fno-jump-tables -I"gen/$1" -Wno-unused-label -Wno-parentheses-equality -c -o "$obj" "$src" || { echo "FAILED: $src" >&2; exit 1; }
    if guestdata; then echo "FAILED: $src has guest addresses in data, without jump tables too (cpu.h, g_guest)" >&2; exit 1; fi
    echo "  $src: without jump tables (it had a lookup table of guest addresses)"
  fi
  echo "$sum" > "$obj.sum"; ln -f "$obj" "$OBJCACHE/$sum.o" 2>/dev/null || cp "$obj" "$OBJCACHE/$sum.o"
  echo "  compiled $src"' sh

# ---- runtime ----
# The shared runtime minus the Mac-only parts (Cocoa/Metal/CoreAudio .m files:
# metal, audio, bundle, gamepad, keyboard_mac, fpscounter, texrep; Discord IPC;
# the Mach profiler; BSD and AF_UNIX sockets: roam_bsd, net_vlan);
# runtime/web/*.c stands in.  winsock.c is the Winsock front end with the
# browser's virtual-LAN backend (runtime/web/net_lan.c); module.c + modheap.c
# load the lifted server.dll; enhance.c holds the graphics options (all off by
# default, as on the Mac).
RT="mem dispatch genexec heap watchdog win32 win32_ext com debug res fileio overrides d3dx_hle vsemu oracle autotest lifecycle widescreen framerate dinput_pad dikeys keyboard dinput_kbd career intro drifttest roam roam_core roam_game mprace horn hornmenu radio carsfx racehorn racenet winsock online netrace module modheap enhance headlights language sponsors lobbyclick olfilter regional carshadow movie"
WEB="$(cd runtime/web && ls *.c | sed 's/\.c$//')"
pids=""
for f in $RT; do
  src="runtime/$f.c"; [ -f "$src" ] || src="runtime/web/$f.c"
  [ -f "$src" ] || continue
  fx=""; case "$f" in roam_core|roam_game) fx="-ffp-contract=off" ;; esac
  $CC $CFLAGS $fx -DNFS_ENHANCED=0 -c -o "$OUT/rt/$f.o" "$src" & pids="$pids $!"
done
for f in $WEB; do
  case "$f" in thread_exit) fl="$TARGET $OPT $FEAT" ;; *) fl="$CFLAGS -DNFS_ENHANCED=0" ;; esac
  $CC $fl -c -o "$OUT/rt/web_$f.o" "runtime/web/$f.c" & pids="$pids $!"
done
for p in $pids; do wait "$p" || { echo "runtime compile failed"; exit 1; }; done

# ---- link ----
OBJS="$(ls "$OUT"/obj/*.o) $(ls "$OUT"/obj/srv/*.o) $(ls "$OUT"/rt/*.o)"
PAGES=$(( INITIAL_MB * 16 ))
rm -f "$OUT/nfsu2.wasm"
$CC $TARGET $OPT $FEAT -o "$OUT/nfsu2.wasm" $OBJS \
  -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=4294967296 \
  -Wl,--initial-memory=$(( INITIAL_MB * 1048576 )),--global-base=$GUEST_END \
  -Wl,-z,stack-size=$(( 8 * 1048576 )) -Wl,--no-stack-first \
  -lsetjmp -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks 2>&1 | \
  { grep -v "^$" || true; } > "$OUT/link.log"
if [ ! -f "$OUT/nfsu2.wasm" ]; then
  echo "link FAILED ($(grep -c "undefined symbol" "$OUT/link.log") undefined symbols):"; grep -E "error" "$OUT/link.log" | head -80; exit 1; fi
REV="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)$(git diff --quiet 2>/dev/null || echo +)"
SIZE=$(wc -c < "$OUT/nfsu2.wasm" | tr -d ' ')
printf '{"pages":%d,"maxPages":65536,"guestEnd":%d,"rev":"%s","size":%d,"opt":"%s","lift":"%s"}\n' \
  "$PAGES" "$GUEST_END" "$REV" "$SIZE" "$OPT" "$LIFT" > "$OUT/nfsu2.json"
echo "link OK: out/web/nfsu2.wasm ($(( SIZE / 1048576 )) MB, $PAGES initial pages, rev $REV, lift $LIFT)"
# a C declaration that disagrees with the definition links on wasm but traps
# when called ("signature_mismatch"); a long is 32 bits here, 64 on the Mac
if grep -q "signature mismatch" "$OUT/link.log"; then
  echo "WARNING: function signature mismatches (they trap when called):"; grep -A2 "signature mismatch" "$OUT/link.log"; fi
