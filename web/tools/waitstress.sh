#!/bin/bash
# The Win32 waits under threads (web/test/win32_unit.c "stress") through the
# real browser host, as web/tools/iostress.sh does for the file RPC: 8 threads
# x 1,000 ticks of a 1 ms waitable timer re-armed with absolute due times (Most
# Wanted's 100 Hz thread, faster), 4 waiters on one periodic auto-reset timer,
# a WaitForMultipleObjects ping-pong through sets that hold a timer, a timer
# closed under an INFINITE wait, and semaphores: 8 threads x 20,000
# release/wait pairs (four pairs handing two semaphores back and forth, then
# four releasers and four takers on one).  A lost wake-up shows as a timeout,
# a stuck thread as a deadline missed: both fail.
#   web/tools/waitstress.sh [http://localhost:<port>]   (a running server, started with --dev)
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
BASE="${1:-http://localhost:8611}"
[ "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/test/hostcheck.html")" = 404 ] && { echo "$BASE does not serve the test pages: start the server with --dev"; exit 2; }
mkdir -p web/dist-dev/test out/web
node web/tools/win32unit.mjs web/dist-dev/test/win32_unit.wasm > /dev/null
node web/tools/run-page.mjs "$BASE/test/hostcheck.html?wasm=/test/win32_unit.wasm&pages=1024&save=core-waitstress&fresh&arg=stress" 300000 \
  | tee "$ROOT/out/web/waitstress.log" | grep -E 'FAIL|tick threads|shared timer|ping-pong|semaphore|win32_unit:|EXIT|TIMEOUT|TRAP|PAGEERROR'
grep -q '^EXIT 0' "$ROOT/out/web/waitstress.log"
