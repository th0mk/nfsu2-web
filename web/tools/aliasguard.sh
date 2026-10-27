#!/bin/bash
# Structural guard for guest memory type punning on WebAssembly: every guest
# access macro of runtime/cpu.h (MR8 ... MRF64), as the wasm build expands it,
# plain, with NFSU2_MEMCHECK and as the lifted shards see it (NFSU2_GUEST_OBJ),
# must go through a may_alias type (gm_u16,
# gm_u32, gm_u64, gm_f32, gm_f64; MR8's uint8_t is a character type and
# aliases anything already).  A plain uint32_t/float access lets clang assume
# an f32 store and an i32 load of the same guest address apart and reorder
# them: the missing car-light and street-lamp glows.
# tests/web/aliastest.c (web/tools/aliastest.sh) shows the hazard only where
# clang 23 happens to schedule it; this check does not depend on codegen.
#
#   web/tools/aliasguard.sh               exit 0 = every macro keeps its type
#   CPU_H=<dir> web/tools/aliasguard.sh   check <dir>/cpu.h instead (the guard's own test)
#   web/tools/aliasguard.sh --lift        and the game's lifted C (below)
#   LIFT_DIR=<dir> ... --lift             check the lifted C in <dir> instead (the guard's own test)
#   GAME=mw web/tools/aliasguard.sh ...   another game (games/<id>/game.conf): its
#                                         lift's include folder and define, as web/build.sh
# --lift: lifted code reaches guest memory through those macros, and through
# the guest base itself (g_mem, g_guest, GMEM_) only as an argument of a byte
# copy (memcpy, memmove, memset; cpu.h's f80_load/f80_store, which copy with
# memcpy).  Any other use of the base in the game's lifted folders (gen/all,
# gen/server for UG2; gen/mw/all, gen/mw/server, gen/mw/d3dx for MW) would be
# a typed access these types do not cover.
# Run by web/build.sh before it compiles anything, and by web/test/alias.test.mjs.
set -e -o pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
LIFT=""; [ "${1:-}" != --lift ] || LIFT=1
# the game's lift folder and define (web/build.sh's -I$G/all and -DNFSU2_GAME_<ID>)
. tools/gameconf.sh
GINC=""; [ ! -d "$GC_GEN_DIR/all" ] || GINC="-I$GC_GEN_DIR/all"
[ "$GAME" = ug2 ] || GINC="$GINC -DNFSU2_GAME_$(echo "$GAME" | tr a-z A-Z)"
WASI="${WASI:-$(ls -d "$ROOT"/deps/wasi-sdk-*-arm64-macos "$ROOT"/deps/wasi-sdk-*-x86_64-linux 2>/dev/null | tail -1 || true)}"
[ -x "$WASI/bin/clang" ] || { echo "aliasguard: wasi-sdk not found in deps/ (set WASI=...)"; exit 1; }
INC="${CPU_H:-runtime}"
[ -f "$INC/cpu.h" ] || { echo "aliasguard: no $INC/cpu.h"; exit 1; }
NAMES=$(grep -hoE '^[[:space:]]*#[[:space:]]*define[[:space:]]+MR[A-Z]*[0-9]+\(' "$INC/cpu.h" |
        sed -E 's/.*define[[:space:]]+(MR[A-Z]*[0-9]+)\(/\1/' | sort -u)
[ -n "$NAMES" ] || { echo "aliasguard: no MR* macros found in $INC/cpu.h"; exit 1; }
bad=0; n=0
for mc in "" "-DNFSU2_MEMCHECK" "-DNFSU2_GUEST_OBJ"; do
  # the wasm build's preprocessor (web/build.sh's target and forced include)
  OUT=$( { echo '#include "cpu.h"'; for m in $NAMES; do echo "@@ $m = $m(0x1000)"; done; } |
         "$WASI/bin/clang" --target=wasm32-wasip1-threads -pthread $mc -E -P -x c \
           -include runtime/web/compat/wasm_plat.h -Iruntime/web/compat -I"$INC" -Iruntime $GINC - )
  for t in u16 u32 u64 f32 f64; do
    case $t in u*) base="uint${t#u}_t" ;; f32) base=float ;; f64) base=double ;; esac
    if ! grep -qE "typedef[[:space:]]+$base[[:space:]]+__attribute__\(\(may_alias\)\)[[:space:]]+gm_$t;" <<< "$OUT"; then
      echo "aliasguard: gm_$t is not 'typedef $base __attribute__((may_alias)) gm_$t' in the wasm build${mc:+ ($mc)}"; bad=1; fi
  done
  for m in $NAMES; do
    case $m in
      MR8) want='uint8_t' ;;
      MR16|MR32|MR64) want="gm_u${m#MR}" ;;
      MRF32|MRF64) want="gm_f${m#MRF}" ;;
      *) echo "aliasguard: $m: a guest access macro this guard does not know: give it a may_alias type in cpu.h and add it here"
         bad=1; continue ;;
    esac
    line=$(grep -E "^@@ $m = " <<< "$OUT" || true)
    n=$((n + 1))
    if [ -z "$line" ]; then echo "aliasguard: $m did not expand${mc:+ ($mc)}"; bad=1; continue; fi
    if ! grep -qE "\*[[:space:]]*\([[:space:]]*$want[[:space:]]*\*[[:space:]]*\)" <<< "$line"; then
      echo "aliasguard: $m${mc:+ ($mc)} does not access guest memory as $want: ${line#@@ }"; bad=1; fi
  done
done
[ $bad -eq 0 ] || { echo "aliasguard: FAILED (runtime/cpu.h's wasm guest accesses must keep their may_alias types)"; exit 1; }
echo "aliasguard: $n guest access expansions keep their may_alias types${GAME:+ ($GAME)}"
[ -n "$LIFT" ] || exit 0
nf=0
for d in ${LIFT_DIR:-"$GC_GEN_DIR"/all "$GC_GEN_DIR"/server "$GC_GEN_DIR"/d3dx}; do
  [ -d "$d" ] || continue
  ls "$d"/*.c > /dev/null 2>&1 || { echo "aliasguard: $d has no lifted C"; exit 1; }
  # each use of the base: the call whose arguments it is in (none: a typed access or a pointer kept)
  r=$(perl -ne 'while (/\b(g_mem|g_guest|GMEM_)\b/g) {
        my $pre = substr($_, 0, $-[0]);
        if ($pre =~ /\b(memcpy|memmove|memset|f80_load|f80_store)\((?:[^;()]|\((?:[^;()]|\([^;()]*\))*\))*$/) { $ok++ }
        else { $bad++; print "aliasguard: $ARGV:$.: guest base outside a byte copy: ", substr($_, 0, 160) =~ s/^\s+//r if $bad <= 5 } }
        close ARGV if eof;
        END { print "#", $ok + 0, " ", $bad + 0, "\n" }' "$d"/*.c)
  ok=$(sed -n 's/^#\([0-9]*\) .*/\1/p' <<< "$r"); nb=$(sed -n 's/^#[0-9]* \([0-9]*\)$/\1/p' <<< "$r")
  grep -v '^#' <<< "$r" || true
  if [ "$nb" = 0 ]; then echo "aliasguard: $d: $(ls "$d"/*.c | wc -l | tr -d ' ') files, $ok uses of the guest base, all in byte copies"
  else echo "aliasguard: $d: $nb uses of the guest base outside a byte copy"; bad=1; fi
  nf=$((nf + 1))
done
[ $nf -gt 0 ] || { echo "aliasguard: --lift: no lifted folder under $GC_GEN_DIR (run ${GAME:+GAME=$GAME }web/gen.sh)"; exit 1; }
[ $bad -eq 0 ] || { echo "aliasguard: FAILED (a lifted access that bypasses runtime/cpu.h's guest access macros)"; exit 1; }
