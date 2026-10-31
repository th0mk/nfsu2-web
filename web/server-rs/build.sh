#!/bin/bash
# The Rust server: web/server-rs/build.sh [--test]  ->  out/web/server-rs/nfsu2-server
# (and the free-roam load generator, out/web/server-rs/nfsu2-roamload: web/tools/server-compare.mjs --load crowd)
#   --test  also its own tests (every crate's, in Cargo.toml's profile.ci: release code without its slow link)
# Needs Rust (rustup: https://rustup.rs) and a C compiler (zstd and mimalloc are C).  The build folder is
# out/web/server-rs/target ($CARGO_TARGET_DIR to put it elsewhere).
set -e -o pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
[ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$REPO/out/web/server-rs/target}"
cd "$HERE"
cargo build --release -p nfsu2-server -p nfsu2-roamload
mkdir -p "$REPO/out/web/server-rs"
for b in nfsu2-server nfsu2-roamload; do
  # a rename, so that a server starting meanwhile never runs a half-written binary
  cp "$CARGO_TARGET_DIR/release/$b" "$REPO/out/web/server-rs/$b.tmp"
  mv -f "$REPO/out/web/server-rs/$b.tmp" "$REPO/out/web/server-rs/$b"
  echo "out/web/server-rs/$b"
done
if [ "${1:-}" = --test ]; then cargo test --profile ci --workspace; fi
