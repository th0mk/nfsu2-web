#!/bin/bash
# The function checks of tests/main_all.c (the Mac's out/nfsu2_all) on
# WebAssembly: direct and indirect calls into the full lifted image, dispatch
# table integrity, x87 conversions, the guest heap (TLSF + VirtualAlloc
# semantics) and the virtual clock's stall valve.  Linked from the objects
# web/build.sh made (the lifted ones it links, out/web/lift.objs, + the
# runtime, with main_all.c in place of runtime/web/main_web.c) and run
# through the real browser host (headless Chrome, /data from the local
# server) by the host self-test page.
#
#   web/tools/main_all.sh [http://localhost:<port>]   (default: a server on 8611)
#
# Needs a built tree (web/build.sh, cd web && node build.mjs) and a running
# server (out/web/server-rs/nfsu2-server) started with --dev.  Exit 0 = all
# checks passed.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
BASE="${1:-http://localhost:8611}"
[ "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/test/hostcheck.html")" = 404 ] && { echo "$BASE does not serve the test pages: start the server with --dev"; exit 2; }
WASI="${WASI:-$(ls -d "$ROOT"/deps/wasi-sdk-*-arm64-macos 2>/dev/null | tail -1)}"
[ -x "$WASI/bin/clang" ] || { echo "wasi-sdk not found in deps/ (set WASI=...)"; exit 1; }
OUT="$ROOT/out/web"
[ -f "$OUT/rt/web_main_web.o" ] && [ -f "$OUT/lift.objs" ] || { echo "run web/build.sh first"; exit 1; }
# Same target, features and defines as web/build.sh (keep in step with it).
OPT="${OPT:--O3}"
TARGET="--target=wasm32-wasip1-threads -pthread"
FEAT="-msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call -mexception-handling -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false"
DEFS="-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS"
CFLAGS="$TARGET $OPT $FEAT $DEFS -fno-delete-null-pointer-checks -include runtime/web/compat/wasm_plat.h -Iruntime/web/compat -Iruntime -Igen/all"
mkdir -p "$OUT/test" web/dist-dev/test
# a FLAGABI lift leaves flags dead at returns that no caller reads (tests/main_all.c section 6)
LIFTDEF="$(grep -q FLAGABI gen/all/web-lift.txt 2>/dev/null && echo -DLIFT_FLAGABI)"
"$WASI/bin/clang" $CFLAGS $LIFTDEF -c -o "$OUT/test/main_all.o" tests/main_all.c
RT="$(ls "$OUT"/rt/*.o | grep -v '/web_main_web\.o$')"
# the lifted objects web/build.sh linked (out/web/lift.objs), never one an older lift left in out/web/obj
LIFTED="$(grep -v '^#' "$OUT/lift.objs" | sed "s|^|$OUT/|")"
"$WASI/bin/clang" $TARGET $OPT -o web/dist-dev/test/nfsu2_all.wasm $LIFTED $RT "$OUT/test/main_all.o" \
  -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=4294967296 \
  -Wl,--initial-memory=1073741824,--global-base=671088640 \
  -Wl,-z,stack-size=$(( 8 * 1048576 )) -Wl,--no-stack-first \
  -lsetjmp -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks
echo "built web/dist-dev/test/nfsu2_all.wasm"
LOG="$OUT/test/main_all.log"
node web/tools/run-page.mjs "$BASE/test/hostcheck.html?wasm=/test/nfsu2_all.wasm&pages=16384&arg=/data/SPEED2.EXE&save=core-main-all&fresh" 300000 \
  | sed -E 's/^\[[0-9]+!?\] //' > "$LOG"
grep -aE 'FAIL|PASSED:|EXIT|TRAP|TIMEOUT|PAGEERROR' "$LOG" | grep -av '^  .\[32mPASS' || true
grep -aqx 'EXIT 0' "$LOG"
