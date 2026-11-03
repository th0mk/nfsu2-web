#!/bin/sh
# Builds n2n_cli with gcc in Docker (or with the local cc when it exists) and runs it against test/fake-switch.mjs.
#   homebrew/switch-net/test/run.sh
set -e
cd "$(dirname "$0")/.."
PORT=${PORT:-8099}
T=$(mktemp -d)
NAME='Zé "Q" \ ünï'
node test/fake-switch.mjs "$PORT" > "$T/fake.log" 2>&1 &
FAKE=$!
trap 'kill $FAKE 2>/dev/null || true; rm -rf "$T"' EXIT
sleep 1
if command -v cc >/dev/null 2>&1; then
  make n2n_cli && ./n2n_cli "ws://127.0.0.1:$PORT" "$NAME" 4 hub > "$T/cli.log"
else
  MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/src" -w /src --add-host=host.docker.internal:host-gateway gcc:14 \
    sh -c "make n2n_cli && ./n2n_cli ws://host.docker.internal:$PORT \"\$0\" 4 && rm -f n2n_cli" "$NAME" > "$T/cli.log"
fi
wait $FAKE || true
cat "$T/cli.log"
cat "$T/fake.log"
grep -q 'welcome: 10.77.1.5' "$T/cli.log"
grep -q '10.77.0.1 Online (server)' "$T/cli.log"
grep -q 'José 🏎' "$T/cli.log"
grep -q 'frame: kind 1 from 10.77.1.6 to port 47210, 4 bytes' "$T/cli.log"
grep -q 'to port 47211, 6 bytes' "$T/cli.log"
grep -q 'to port 47210, 2 bytes' "$T/cli.log"
grep -q '  10.77.1.7 New' "$T/cli.log"
grep -q 'pong: ok' "$T/cli.log"
grep -q 'kicked: bye "now"' "$T/cli.log"
grep -q 'closed: 4001' "$T/cli.log"
grep -q 'FAKE: OK' "$T/fake.log"
echo "n2n: all checks passed"
