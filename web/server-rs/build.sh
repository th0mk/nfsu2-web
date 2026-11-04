#!/bin/bash
# The Rust server: web/server-rs/build.sh [--test]  ->  out/web/server-rs/nfsu2-server
# (and the free-roam load generator, out/web/server-rs/nfsu2-roamload: web/tools/server-compare.mjs --load crowd)
#   --test  also its own tests (every crate's, in Cargo.toml's profile.ci: release code without its slow link)
# Needs Rust (rustup: https://rustup.rs) and a C compiler (zstd and mimalloc are C).  The build folder is
# out/web/server-rs/target ($CARGO_TARGET_DIR to put it elsewhere).
# Sources built before are not built again: ~/.cache/nsfu2-web/server keeps the last 12 builds' binaries under a
# hash of every file in web/server-rs and the toolchain, and a checkout whose files hash the same gets copies of
# them (the server finds its checkout from where it sits, not from where it was built).
set -e -o pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
[ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$REPO/out/web/server-rs/target}"
cd "$HERE"
BINS="nfsu2-server nfsu2-roamload"
CACHE="$HOME/.cache/nsfu2-web/server"
KEY="$( { find . -path ./target -prune -o -type f -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256
          rustc -vV; cargo -V; echo "${RUSTFLAGS:-}"; } | shasum -a 256 | cut -c1-16)"
mkdir -p "$REPO/out/web/server-rs"
# a rename, so that a server starting meanwhile never runs a half-written binary
put(){ cp "$1" "$REPO/out/web/server-rs/$2.tmp"; mv -f "$REPO/out/web/server-rs/$2.tmp" "$REPO/out/web/server-rs/$2"; echo "out/web/server-rs/$2"; }
if [ -f "$CACHE/$KEY/done" ]; then
  for b in $BINS; do put "$CACHE/$KEY/$b" "$b"; done
  touch "$CACHE/$KEY"
  echo "(built before from the same sources: $CACHE/$KEY)"
else
  cargo build --release -p nfsu2-server -p nfsu2-roamload
  for b in $BINS; do put "$CARGO_TARGET_DIR/release/$b" "$b"; done
  # into the cache: a folder of its own first, renamed when complete
  mkdir -p "$CACHE"; T="$(mktemp -d "$CACHE/.new-XXXXXX")"
  for b in $BINS; do cp "$CARGO_TARGET_DIR/release/$b" "$T/$b"; done
  touch "$T/done"
  if [ -e "$CACHE/$KEY" ]; then rm -rf "$T"; else mv "$T" "$CACHE/$KEY"; fi
  ls -1dt "$CACHE"/*/ | tail -n +13 | while read -r d; do rm -rf "$d"; done
  find "$CACHE" -maxdepth 1 -name '.new-*' -mmin +60 -exec rm -rf {} + 2>/dev/null || true
fi
if [ "${1:-}" = --test ]; then cargo test --profile ci --workspace; fi
