# The server

The game's server, in Rust: it serves the page, the game module and the game's own files, and connects the players
(the virtual LAN switch, the free-roam hub and the game's Online mode).  Its options are in `src/options.rs`.

## Build and run

    web/server-rs/build.sh            # -> out/web/server-rs/nfsu2-server
    web/server-rs/build.sh --test     # and the workspace's own tests
    out/web/server-rs/nfsu2-server --data "<NFSU2 folder>" --http-port 8081 --state /tmp/state-rs

It needs Rust (https://rustup.rs) and a C compiler (zstd and mimalloc are C).

## The container image

`web/server-rs/Dockerfile` builds the image that `web/deploy/compose.yml` runs:

    docker build -f web/server-rs/Dockerfile -t nfsu2-web-rs .
