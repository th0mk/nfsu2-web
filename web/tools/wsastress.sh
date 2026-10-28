#!/bin/bash
# Most Wanted's overlapped Winsock under threads (web/test/wsaov_unit.c "stress") through the real browser host, as
# web/tools/waitstress.sh does for the Win32 waits (TRACKER M8-06, M8-07): a WSASetEvent ping-pong through
# WSAWaitForMultipleEvents (500 round trips), an event closed under an INFINITE wait, the guest lock free while a
# thread waits, a datagram waking a thread whose receive is pending, DirtySock's socket thread (speed.exe 0x84AC10)
# under 3,000 datagrams, a socket closed under a wait, and WSAGetOverlappedResult's fWait.  A lost wake-up shows as
# a timeout, a stuck thread as a deadline missed: both fail.
#   web/tools/wsastress.sh [http://localhost:<port>]   (a running server, started with --dev)
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
BASE="${1:-http://localhost:8611}"
[ "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/test/hostcheck.html")" = 404 ] && { echo "$BASE does not serve the test pages: start the server with --dev"; exit 2; }
mkdir -p web/dist-dev/test out/web
node web/tools/win32unit.mjs --wsaov web/dist-dev/test/wsaov_unit.wasm > /dev/null
node web/tools/run-page.mjs "$BASE/test/hostcheck.html?wasm=/test/wsaov_unit.wasm&pages=1024&save=core-wsastress&fresh&arg=stress" 120000 \
  | tee "$ROOT/out/web/wsastress.log" | grep -E 'FAIL|ping-pong|mid-wait|guest lock|ended the wait|socket thread|fWait|wsaov_unit:|EXIT|TIMEOUT|TRAP|PAGEERROR'
grep -q '^EXIT 0' "$ROOT/out/web/wsastress.log"
