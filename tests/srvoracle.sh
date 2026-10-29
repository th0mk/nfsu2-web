#!/bin/bash
# Differential test of the lifted server.dll (gen/server) against its original
# x86 code under Unicorn: see tests/srvoracle.c.
#   tests/srvoracle.sh [cases-per-function]
# Needs ./gen.sh and ./build.sh (out/obj/srv; on Linux OPT=-O3 J=<n> ./build.sh)
# and a native Unicorn 2.1.x (arm64 Mac or x86-64 Linux, x86 target enough):
# by default the project's work/deps/unicorn-2.1.4 (as link.sh), or
#   UNICORN_INC=<unicorn source>/include UNICORN_LIB=<build>/libunicorn.a
# Off macOS module.c gets CommonCrypto's SHA-256 from tests/compat, which is
# runtime/web/sha256.c (the wasm build's).
# SRVDLL defaults to $NFSU2_DATA/server.dll.  Test-only binary (Unicorn is
# GPLv2): out/srvoracle is never linked into the game or the app.  The lifted
# code is gen.sh's (server.dll is lifted with the exe's X87PC mode).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
UDEP=""
for d in "$ROOT/../../work/deps/unicorn-2.1.4" "$ROOT/../../deps/unicorn-2.1.4"; do
  [ -d "$d" ] && { UDEP="$(cd "$d" && pwd)"; break; }
done
UNICORN_INC="${UNICORN_INC:-${UDEP:+$UDEP/include}}"; UNICORN_LIB="${UNICORN_LIB:-${UDEP:+$UDEP/lib/libunicorn.a}}"
[ -d "$UNICORN_INC" ] && [ -f "$UNICORN_LIB" ] || { echo "needs Unicorn: work/deps/unicorn-2.1.4, or UNICORN_INC and UNICORN_LIB"; exit 1; }
SRVDLL="${SRVDLL:-${NFSU2_DATA:?set NFSU2_DATA or SRVDLL}/server.dll}"
ls gen/server/shard_*.c >/dev/null 2>&1 || { echo "gen/server has no lifted code (no server.dll at gen time)"; exit 1; }
OBJS="$(ls gen/server/shard_*.c | sed -E 's|gen/server/(.*)\.c|out/obj/srv/\1.o|') out/obj/srv/fntab.o"
[ -f gen/server/stubs.c ] && OBJS="$OBJS out/obj/srv/stubs.o"
mkdir -p out/obj/oracle
CCINC=""
if [ "$(uname -s)" != Darwin ]; then
  CCINC="-Itests/compat"
  clang -O2 -c -o out/obj/oracle/sha256.o runtime/web/sha256.c
fi
clang -O2 -Iruntime -I"$UNICORN_INC" -c -o out/obj/oracle/srvoracle.o tests/srvoracle.c
clang -O2 -Iruntime $CCINC -c -o out/obj/oracle/module.o runtime/module.c
clang -O2 -Iruntime -c -o out/obj/oracle/mem.o runtime/mem.c
clang -O2 -Iruntime -c -o out/obj/oracle/srv_impargs.o gen/server/impargs.c
clang -O2 -Iruntime -c -o out/obj/oracle/srv_modinfo.o gen/server/modinfo.c
clang -o out/srvoracle out/obj/oracle/*.o $OBJS "$UNICORN_LIB" -lpthread -lm
SRVDLL="$SRVDLL" out/srvoracle "${1:-2000}"
