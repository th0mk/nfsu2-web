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

## Meet races

Meet races are enabled automatically in every room when the free-roam hub runs. No feature flags are needed. The server needs the original `TRACKS/ROUTESL4RA/RoutesFreeRoam.bin` and `PathsFreeRoam.bin` to select reachable destinations within the meet's career region. Existing clients continue normal free roam; enrollment and the race UI require the updated browser build (including its two-player free-roam mode).

Enrollment for the next race opens immediately when the current race starts. The server runs a five-minute schedule per meet, a five-second countdown, a five-minute hard race deadline and a thirty-second closing window after the first finish. It derives places from fresh position samples and writes 300/200/100-credit receipts under `<state>/meet-rewards` on a bounded background worker. Back up that directory with server state. Restarting cancels active races; committed prizes remain recoverable by their authenticated owner and career profile. Guests recover within their current session only. Transition and payout logs start with `[meet-race]`.

The client journals a prize before applying it to career cash and acknowledges it after a normal career save. An unsaved restart reapplies the pending prize once. Both local players share the host's bank, with separate receipts. This preserves ordinary retry/crash recovery, but cannot prevent deliberate rollback of client-owned saves. Include `MEETRACE_A.DAT` and `MEETRACE_B.DAT` when backing up a profile (normal full backups already do).

For isolated development tests, `--dev` additionally permits `NFSU2_MEET_RACE_TEST_PERIOD_MS=30000` and `NFSU2_MEET_RACE_TEST_FINISH_OFFSET=0,-110,0`. The period override also caps race duration at that period, so the next enrollment opens while the current race runs and races end by the next start. Neither override is accepted without `--dev`; omit both for normal gameplay. Run `node web/tools/meet-races-test.mjs <base> <artifacts> --split` for enrollment, countdown and map/leave checks. With an owned-car save in `NFSU2_SAVE_SEED`, the matching finish fixture and `--finish` exercise driving, native cash and saving. `NFSU2_EXPECT_BANK=<amount>` checks a seeded fresh-session reload. Unit coverage lives in the roam crate and `web/test/meet-race-core.test.mjs` / `meet-races.test.mjs`.
