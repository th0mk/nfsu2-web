# syntax=docker/dockerfile:1
# NFS Underground 2 in the browser: the server (web/server-rs) as a container image (docs/web/VPS.md).
#
#   docker build -f web/server-rs/Dockerfile -t nfsu2-web-rs .
#
# The image holds the server (one static binary) and the page (web/dist, without the test pages and source maps).  It
# never holds the game or anything made from it (this Dockerfile's .dockerignore lets only the server's and the page's
# sources into the build); those are mounted:
#   /game     the NFS Underground 2 folder (the one with SPEED2.EXE), read-only, readable by everyone
#   /module   nfsu2.wasm and nfsu2.json, built with web/build.sh from the image's revision, read-only
#   /state    the server's keys (ETags, the switch's tokens), the Online mode's records and the bans: a volume, kept across
#             updates (a new ETag key would make every browser download the game data again)
# The server runs as uid 1000 (the user of the Node server's image before it, so a state volume that one wrote stays
# writable) and listens on 127.0.0.1:8080 of the network it is given: the VPS's own, with Caddy in front
# (web/deploy/compose.yml), or a tunnel's (compose.tunnel.yml).  Its command line is the server's options; by default
# --behind-proxy, which needs NFSU2_HOST, the name players open.
#
# Both stages that build run on the build machine's platform: the binary is cross-compiled (cargo-zigbuild, static
# musl) for the image's, so building it for arm64 on amd64 needs no emulation.
FROM --platform=$BUILDPLATFORM node:24-alpine AS page
ENV PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1
WORKDIR /src/web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY app/registry-defaults.txt /src/app/
COPY web/build.mjs web/index.html ./
COPY web/src src
RUN node build.mjs && mkdir /state

FROM --platform=$BUILDPLATFORM rust:1-bookworm AS server
ARG TARGETARCH
ARG ZIG=0.14.1
RUN case "$TARGETARCH" in \
      amd64) t=x86_64-unknown-linux-musl ;; \
      arm64) t=aarch64-unknown-linux-musl ;; \
      *) echo "no Rust target for $TARGETARCH" >&2; exit 1 ;; \
    esac \
 && echo "$t" > /target && rustup target add "$t" \
 && curl -sSfL "https://ziglang.org/download/$ZIG/zig-$(uname -m)-linux-$ZIG.tar.xz" | tar -xJ -C /opt \
 && ln -s /opt/zig-*/zig /usr/local/bin/zig \
 && cargo install --locked cargo-zigbuild@0.23.4
WORKDIR /src
COPY web/server-rs web/server-rs
# no line tables in the image's binary (a third of its size); a panic still names its functions
ENV CARGO_PROFILE_RELEASE_DEBUG=0
# (the build folder goes once the binary is out: the layer, which the CI's cache keeps, holds 6 MB instead of 500)
RUN cd web/server-rs && cargo zigbuild --release --locked -p nfsu2-server --target "$(cat /target)" \
 && cp "target/$(cat /target)/release/nfsu2-server" /nfsu2-server && rm -rf target

# the binary on its own, for a VPS without Docker (web/deploy/pack.sh):
#   docker build -f web/server-rs/Dockerfile --platform linux/amd64 --target binary --output <dir> .
FROM scratch AS binary
COPY --from=server /nfsu2-server /nfsu2-server

FROM alpine:3.22
LABEL org.opencontainers.image.title="nfsu2-web-rs" \
      org.opencontainers.image.description="NFS Underground 2 in the browser: the server, in Rust (bring your own game)" \
      org.opencontainers.image.licenses="MIT"
WORKDIR /app
COPY --from=page /src/web/dist web/dist
COPY --from=server /nfsu2-server /app/nfsu2-server
COPY --from=page --chown=1000:1000 --chmod=700 /state /state
# NFSU2_REPO: where the server finds the page (web/dist); its own default is the checkout it was built in
ENV NFSU2_REPO=/app \
    NFSU2_DATA=/game \
    NFSU2_WASM=/module/nfsu2.wasm \
    NFSU2_STATE=/state \
    NFSU2_MAX_PEERS=6000
# (a uid without a name: this stage runs nothing, so building it for arm64 needs no emulation)
USER 1000:1000
VOLUME /state
# the module is there and the server answers (over the HTTP port, which stays on 127.0.0.1): game.json at the game's
# base on this machine's names, UG2's root or Most Wanted's /mw (src/game.rs), whichever --game the image runs with;
# a server answers one of them only (tests/binary.rs runs this line against both games)
HEALTHCHECK --interval=30s --timeout=5s --start-period=60s --start-interval=2s \
  CMD for b in "" /mw; do wget -q -O /dev/null "http://127.0.0.1:${NFSU2_HTTP_PORT:-8080}$b/game.json" && exit 0; done; exit 1
ENTRYPOINT ["/app/nfsu2-server"]
CMD ["--behind-proxy"]
