#!/bin/bash
# Build the game module for the browser: out/web/nfsu2.wasm (+ nfsu2.json).
#
#   web/build.sh            lifted shards (incremental) + runtime + link
#   FULL=1 web/build.sh     recompile everything
#   OPT=-O2 web/build.sh    another optimisation level (default -O3)
#   GAME=mw web/build.sh    another game (games/<id>/game.conf, tools/gameconf.sh):
#                           out/web/mw/nfsmw.wasm (+ .json)
#
# Needs the game's gen folders, gen/all and gen/server for UG2 (web/gen.sh
# <SPEED2.EXE>: ./gen.sh with the web's lift options; it lifts server.dll too),
# gen/mw/all and gen/mw/server for MW (GAME=mw web/gen.sh <speed.exe>), and
# wasi-sdk in deps/ (see docs/web).
# Toolchain: wasi-sdk (clang + wasm-ld), target wasm32-wasip1-threads, run by
# the page's own host (web/src/host): no Emscripten.
#
# The module json: pages, maxPages, guestEnd, rev, size, opt, lift (the lift
# options, web-lift.txt) and game (the game id).  Another game's json also has
# stamp (the lift's stamp.txt without its date line).  When the D3DX oracle is
# linked, any game's json has "oracle":1: a dev-only module that
# web/deploy/release.sh and pack.sh refuse (PLAN D-09).
# The lifted objects the link takes are listed in lift.objs next to the module
# (out/web/lift.objs for UG2), for the tools that link them again
# (web/tools/corelink.sh, web/tools/main_all.sh).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
WASI="${WASI:-$(ls -d "$ROOT"/deps/wasi-sdk-*-arm64-macos "$ROOT"/deps/wasi-sdk-*-x86_64-linux 2>/dev/null | tail -1)}"
[ -x "$WASI/bin/clang" ] || { echo "wasi-sdk not found in deps/ (set WASI=...)"; exit 1; }
# the game (GAME, default ug2): its gen folder, module name and output folder
. tools/gameconf.sh
G="$GC_GEN_DIR"; WASM_NAME="${GC_WASM_NAME:?$GAME_CONF: no WASM_NAME=}"
if [ "$GAME" = ug2 ]; then GENRUN="web/gen.sh"; OUT="$ROOT/out/web"; else GENRUN="GAME=$GAME web/gen.sh"; OUT="$ROOT/out/web/$GAME"; fi
# its lifted DLLs, each from its own gen folder into its own object folder:
# server.dll (gen.sh lifts it, or writes a stub module), and the D3DX oracle
# when the game has one and it was lifted (gen/<game>/d3dx: D3DX_DLL=<the
# DLL> GAME=<id> web/gen.sh; web/tools/gencheck.sh checks its stamp;
# D3DX_ORACLE=0 leaves it out)
MODS="server:srv"; LINK_ORACLE=""
if [ -n "$GC_D3DX_BASE" ] && [ -f "$G/d3dx/fntab.c" ] && [ "${D3DX_ORACLE:-1}" != 0 ]; then MODS="$MODS d3dx:d3dx"; LINK_ORACLE=1; fi
# guest memory accesses must keep their may_alias types (cpu.h)
WASI="$WASI" web/tools/aliasguard.sh > /dev/null || { WASI="$WASI" web/tools/aliasguard.sh; exit 1; }
[ -f "$G/all/fntab.c" ] || { echo "$G/all missing: run $GENRUN <path to $GC_EXE_NAME>"; exit 1; }
[ -f "$G/server/fntab.c" ] || { echo "$G/server missing: run $GENRUN <path to $GC_EXE_NAME> (it lifts server.dll too)"; exit 1; }
# The lift must be the one this runtime needs: its _orig declarations, and
# gen.sh's stamp (override list, hook list, lifter; web/tools/gencheck.sh) of
# the folder compiled here, whatever GEN (gencheck's test switch) says
GEN="$G" web/tools/gencheck.sh
LIFT="$(cat "$G/all/web-lift.txt" 2>/dev/null || echo unknown)"
[ "$LIFT" != unknown ] || echo "note: $G/all was not made by web/gen.sh (lift options unknown; web/gen.sh turns on FLAGABI and X87OPT)"
CC="$WASI/bin/clang"; OBJDUMP="$WASI/bin/llvm-objdump"
J="${J:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"
OPT="${OPT:--O3}"
mkdir -p "$OUT/obj" "$OUT/rt"; for m in $MODS; do mkdir -p "$OUT/obj/${m#*:}"; done
OBJCACHE="${OBJCACHE:-$HOME/.cache/nsfu2-web/obj}"; mkdir -p "$OBJCACHE"

# The guest owns [0, GUEST_END) of linear memory (runtime/cpu.h); the
# runtime's data starts there.  Initial memory covers the guest span plus room
# for the runtime heap; it can grow to 4 GB.
GUEST_END=671088640   # 0x28000000
INITIAL_MB="${INITIAL_MB:-1024}"
TARGET="--target=wasm32-wasip1-threads -pthread"
FEAT="-msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false"
DEFS="-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS"
# -fno-delete-null-pointer-checks: guest address 0 is real wasm memory (cpu.h).
# Another game's map (runtime/memmap.h): -DNFSU2_GAME_<ID>
GAMEDEF=""; [ "$GAME" = ug2 ] || GAMEDEF=" -DNFSU2_GAME_$(echo "$GAME" | tr a-z A-Z)"
CFLAGS="$TARGET $OPT $FEAT $DEFS -fno-delete-null-pointer-checks -include runtime/web/compat/wasm_plat.h -Iruntime/web/compat -Iruntime -I$G/all$GAMEDEF"
# the lifted shards only: guest memory as an object of its own (cpu.h, NFSU2_GUEST_OBJ)
SHARDFLAGS="-DNFSU2_GUEST_OBJ"
export CC OBJDUMP CFLAGS SHARDFLAGS OUT FULL MEMCHECK OBJCACHE
# cpu.h takes the exe's .text from gen/all/gameinfo.h, or from runtime/gameinfo_ug2.h without one
GAMEINFO="$( [ -f "$G/all/gameinfo.h" ] && echo "$G/all/gameinfo.h" || echo runtime/gameinfo_ug2.h )"
HDRSUM="$( (cat runtime/cpu.h runtime/web/compat/wasm_plat.h "$GAMEINFO"; echo "$CFLAGS $SHARDFLAGS") | shasum | cut -c1-16)"; export HDRSUM

# ---- lifted shards (content-hash incremental, as build.sh) ----
# gen/all (SPEED2.EXE) -> out/web/obj, gen/server (server.dll, the LAN server
# lifted as a second module, see gen.sh) -> out/web/obj/srv, any other lifted
# DLL of the game (MODS) -> out/web/obj/<its name>: one pool, the same rule for
# all.  A shard's key is its source, its own module's decls.h and the flags
# (a module's modinfo.c also runtime/module.h, its ModInfo), so the modules
# never share cache entries by accident.
# lifted: "<gen folder> <object folder> <source>" for each file of the lift;
# the link takes these objects only, never one an older lift left behind
lifted(){
  (cd "$G/all" && ls shard_*.c fntab.c impargs.c $( [ -f stubs.c ] && echo stubs.c ) | sed "s|^|$G/all obj |")
  for m in $MODS; do
    (cd "$G/${m%%:*}" && ls shard_*.c fntab.c impargs.c modinfo.c $( [ -f stubs.c ] && echo stubs.c ) | sed "s|^|$G/${m%%:*} obj/${m#*:} |"); done
}
lifted | xargs -P "$J" -n 3 sh -c '
  gd="$1"; od="$OUT/$2"
  src="$gd/$3"; obj="$od/$(basename "$3" .c).o"
  hdr=""; [ "$3" != modinfo.c ] || hdr=runtime/module.h
  sum=$( (cat "$src" "$gd/decls.h" $hdr; echo "$HDRSUM") | shasum | cut -c1-40)
  if [ -z "$FULL" ] && [ -f "$obj" ] && [ "$(cat "$obj.sum" 2>/dev/null)" = "$sum" ]; then exit 0; fi
  mc=""; case ",$MEMCHECK," in *",$(basename "$3" .c),"*) mc="-DNFSU2_MEMCHECK" ;; esac
  [ -n "$mc" ] && sum="$sum-memchk"
  if [ -z "$FULL" ] && [ -f "$obj" ] && [ "$(cat "$obj.sum" 2>/dev/null)" = "$sum" ]; then exit 0; fi
  # shared, content-addressed cache: every worktree with the same shard, cpu.h
  # and flags reuses one compile
  if [ -z "$FULL" ] && [ -f "$OBJCACHE/$sum.o" ]; then ln -f "$OBJCACHE/$sum.o" "$obj" 2>/dev/null || cp "$OBJCACHE/$sum.o" "$obj"
    echo "$sum" > "$obj.sum"; exit 0; fi
  o=$($CC $CFLAGS $SHARDFLAGS $mc -I"$gd" -Wno-unused-label -Wno-parentheses-equality -c -o "$obj" "$src" 2>&1); r=$?
  [ -z "$o" ] || printf "%s\n" "$o" | head -5
  [ $r -eq 0 ] || { echo "FAILED: $src" >&2; exit $r; }
  # a lookup table of guest addresses (g_guest + K in data) links as zeros
  # (cpu.h): compile the shard again without jump tables, which has none
  guestdata() { "$OBJDUMP" -r "$obj" | awk "/^RELOCATION RECORDS FOR \\[DATA\\]/ { d = 1; next } /^RELOCATION RECORDS/ { d = 0 }
    d && \$3 ~ /^g_guest[+-]/ && \$3 != \"g_guest+0\" { f = 1 } END { exit !f }"; }
  if guestdata; then
    $CC $CFLAGS $SHARDFLAGS $mc -fno-jump-tables -I"$gd" -Wno-unused-label -Wno-parentheses-equality -c -o "$obj" "$src" || { echo "FAILED: $src" >&2; exit 1; }
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
# load the lifted DLLs; enhance.c holds the graphics options (all off by
# default, as on the Mac).
# RT_CORE: the game-independent files (runtime/core.list); then the game's
# own: UG2's feature files in runtime/, another game's runtime/<id>/*.c
# (objects mw_<name>.o).  What core calls of a game that has no such file
# yet, runtime/web/null_game.c's weak stand-ins answer.
RT_CORE="$(sed 's/#.*//' runtime/core.list | awk 'NF{print $1}' | grep '\.c$' | sed 's/\.c$//' | tr '\n' ' ')"
RT_UG2="game_ug2 debug_ug2 overrides d3dx_hle oracle widescreen framerate dinput_pad career intro drifttest roam roam_game mprace hornmenu optslider tonemenu powerunits radio drift carsfx racehorn racenet online netrace headlights language sms sponsors lobbyclick olfilter tunegraph menusize regional garage sndlead carshadow movie photo"
if [ "$GAME" = ug2 ]; then RT="$RT_CORE $RT_UG2"; RT_GAME=""
else RT="$RT_CORE"; RT_GAME="$( (cd "runtime/$GAME" 2>/dev/null && ls *.c 2>/dev/null) | sed 's/\.c$//' | tr '\n' ' ')"; fi
# runtime files only: a build with the D3DX oracle linked
RTDEFS=""; [ -z "$LINK_ORACLE" ] || RTDEFS="-DNFSU2_D3DX_ORACLE=1"
WEB="$(cd runtime/web && ls *.c | sed 's/\.c$//')"
# every runtime object is compiled again below: none of a file that is gone
# (or moved into core) stays to be linked
rm -f "$OUT"/rt/*.o
pids=""
for f in $RT; do
  src="runtime/$f.c"; [ -f "$src" ] || src="runtime/web/$f.c"
  [ -f "$src" ] || continue
  fx=""; case "$f" in roam_core|roam_game) fx="-ffp-contract=off" ;; esac
  $CC $CFLAGS $fx -DNFS_ENHANCED=0 $RTDEFS -c -o "$OUT/rt/$f.o" "$src" & pids="$pids $!"
done
for f in $RT_GAME; do
  $CC $CFLAGS -DNFS_ENHANCED=0 $RTDEFS -c -o "$OUT/rt/${GAME}_$f.o" "runtime/$GAME/$f.c" & pids="$pids $!"
done
for f in $WEB; do
  case "$f" in thread_exit) fl="$TARGET $OPT $FEAT" ;; *) fl="$CFLAGS -DNFS_ENHANCED=0 $RTDEFS" ;; esac
  $CC $fl -c -o "$OUT/rt/web_$f.o" "runtime/web/$f.c" & pids="$pids $!"
done
for p in $pids; do wait "$p" || { echo "runtime compile failed"; exit 1; }; done

# ---- link ----
# lift.objs: the lifted objects (paths under $OUT), and the stamp.txt of each
# gen folder they come from (SHA-256 prefix; corelink.sh refuses objects older
# than the lift)
stampsum(){ if [ -f "$1/stamp.txt" ]; then shasum -a 256 < "$1/stamp.txt" | cut -c1-16; else echo none; fi; }
{ echo "# web/build.sh: the lifted objects its link takes, under ${OUT#$ROOT/}"
  for d in "$G/all" $(for m in $MODS; do echo "$G/${m%%:*}"; done); do echo "#stamp $d $(stampsum "$d")"; done
  lifted | while read -r gd od f; do echo "$od/${f%.c}.o"; done; } > "$OUT/lift.objs"
OBJS="$(grep -v '^#' "$OUT/lift.objs" | sed "s|^|$OUT/|") $(ls "$OUT"/rt/*.o)"
PAGES=$(( INITIAL_MB * 16 ))
W="$OUT/$WASM_NAME.wasm"; rm -f "$W"
$CC $TARGET $OPT $FEAT -o "$W" $OBJS \
  -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=4294967296 \
  -Wl,--initial-memory=$(( INITIAL_MB * 1048576 )),--global-base=$GUEST_END \
  -Wl,-z,stack-size=$(( 8 * 1048576 )) -Wl,--no-stack-first \
  -lsetjmp -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks 2>&1 | \
  { grep -v "^$" || true; } > "$OUT/link.log"
if [ ! -f "$W" ]; then
  echo "link FAILED ($(grep -c "undefined symbol" "$OUT/link.log") undefined symbols):"; grep -E "error" "$OUT/link.log" | head -80; exit 1; fi
REV="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)$(git diff --quiet 2>/dev/null || echo +)"
SIZE=$(wc -c < "$W" | tr -d ' ')
# UG2's json keeps the fields it always had and gains the game id; another
# game's also names its lift stamp; an oracle build of any game says so
MORE=""
if [ "$GAME" != ug2 ]; then
  STAMP="$(sed 1d "$G/all/stamp.txt" 2>/dev/null | tr -d '"\\' | awk 'NF' | paste -sd ';' - | sed 's/;/; /g')"
  MORE=",\"stamp\":\"${STAMP:-unknown}\""
fi
[ -z "$LINK_ORACLE" ] || MORE="$MORE,\"oracle\":1"
printf '{"pages":%d,"maxPages":65536,"guestEnd":%d,"rev":"%s","size":%d,"opt":"%s","lift":"%s","game":"%s"%s}\n' \
  "$PAGES" "$GUEST_END" "$REV" "$SIZE" "$OPT" "$LIFT" "$GAME" "$MORE" > "$OUT/$WASM_NAME.json"
echo "link OK: ${W#$ROOT/} ($(( SIZE / 1048576 )) MB, $PAGES initial pages, rev $REV, lift $LIFT)"
[ -z "$LINK_ORACLE" ] || echo "note: the D3DX oracle ($G/d3dx) is linked: a dev-only module, \"oracle\":1 in its json (web/deploy/release.sh and pack.sh refuse it)"
# a C declaration that disagrees with the definition links on wasm but traps
# when called ("signature_mismatch"); a long is 32 bits here, 64 on the Mac
if grep -q "signature mismatch" "$OUT/link.log"; then
  echo "WARNING: function signature mismatches (they trap when called):"; grep -A2 "signature mismatch" "$OUT/link.log"; fi
