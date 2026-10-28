# The server

The game's server, in Rust: it serves the page, the game module and the game's own files, and connects the players
(the virtual LAN switch, the free-roam hub and the game's Online mode).  Its options are in `src/options.rs`, and the
crates' docs say why each rule is what it is, with the audits they name.  The state folder keeps `etag.key`,
`lan-addresses.json`, `online/stats.json`, `online/buddies.json`, `online/races.json` (the races going on, from a stop
to the next start), `vault/` and the operator's bans (`bans.json`, `bans.key`: nfsu2-bans), and has the control
socket, `control.sock`, which `nfsu2-server ctl` talks to (who is on, kick, ban: src/app/control.rs, docs/web/VPS.md
"Kick and ban").

It replaced the Node.js server (`web/server/*.mjs`, removed after 6076514) with the same options and environment
(`NFSU2_*`), the same console lines and the same state files, byte for byte, so a VPS switched with one line
(docs/web/VPS.md, "From the Node server").  Why: Node ran everything on one thread.  On nfsu2.xyz (2026-10-20/21:
850-1075 players, 690-880 of them in free roam) that thread was at 85-100 % while the 16-core VPS was mostly idle,
and the hub had to give free-roam players 12-24 links instead of 32.  This server spreads the switch over the cores:
on 2026-10-21 it carried 1209 players on 0.7 of a core.

## Build and run

    web/server-rs/build.sh            # -> out/web/server-rs/nfsu2-server (and nfsu2-roamload, the load generator)
    web/server-rs/build.sh --test     # and the workspace's own tests
    out/web/server-rs/nfsu2-server --data "<NFSU2 folder>" --http-port 8081 --state /tmp/state-rs

It needs Rust (https://rustup.rs) and a C compiler (zstd and mimalloc are C).  Servers can run at once on different
ports; give each its own `--state`.  One game per server: NFS Underground 2, or with `--game mw` (`NFSU2_GAME`) NFS
Most Wanted, with that game's defaults (`src/game.rs`: the program the folder holds, what of it is served, the page,
the module, the state folder; no Online mode for it yet).  `nfsu2-server [--game mw] --print-entries` prints what of
the game's folder it serves (web/deploy/pack.sh asks).  Knobs for measurements, from the environment:

| | |
|---|---|
| `NFSU2_THREADS` | the runtime's workers (default half the cores, 2 to 8: more did the same work for more CPU; past about 3000 players in free roam more keep them at 32 friends, "Measured" below) |
| `NFSU2_BUNDLE_MS` | the bundle clock's tick (default 50 ms, below; 0: no clock, each datagram wakes its writer at once) |
| `NFSU2_FPRED` | 0: friends' car states go to the pages as byte deltas even when a page asks for them by their fields (nfsu2-lan fpred.rs), for measurements |
| `NFSU2_BUNDLE_JITTER` | each tick is the tick give or take this many percent of it, at random (default 40; nfsu2-lan conn.rs `Clock` says why) |
| `NFSU2_BUNDLE_MAX_MS`, `NFSU2_BUNDLE_BUDGET` | longer than `NFSU2_BUNDLE_MS`: the tick follows the crowd up to it, aiming at the budget's bundles a second in all (125,000) |
| `NFSU2_LAN_SNDBUF` | SO_SNDBUF of /lan connections (default 32 KiB, which Linux doubles; 0: the kernel's own, which grows to 4 MB) |
| `NFSU2_COMPRESS_THREADS` | the threads that compress the game data's blocks once at brotli's best into the state folder's `blocks/` (src/app/kept.rs; default 2, at the lowest priority, waiting while the CPU quota the server shares is 70 % used; 0: none, every block compressed as it is sent) |
| `NFSU2_REPO` | where the defaults are (web/dist, out/web/nfsu2.wasm, the state in out/web/server): the repo it was built from |

## The container image

`web/server-rs/Dockerfile` (its own `.dockerignore` next to it): the page, the mounts (`/game`, `/module`,
`/state`), uid 1000 (the Node image's user, so its state volume stays writable), the health check, and one static
binary (musl, cross-compiled with cargo-zigbuild, so arm64 needs no emulation; 22 MB in all; it took no more CPU than
the native build for 500 players in free roam).  `.github/workflows/image-rs.yml` publishes it as
`ghcr.io/<owner>/<repo>-rs` (`latest`, `sha-<commit>`, versions); a VPS names it in its `.env`
(web/deploy/compose.yml, docs/web/VPS.md):

    NFSU2_IMAGE=ghcr.io/th0mk/nfsu2-web-rs:sha-<commit>

Its `binary` stage is the binary alone (6 MB), for a VPS without Docker (web/deploy/pack.sh):

    docker build -f web/server-rs/Dockerfile --platform linux/amd64 --target binary --output <dir> .

## The JS tests and tools

The tests and tools that start a server start this binary (`web/tools/server-cmd.mjs`: out/web/server-rs/nfsu2-server,
or `NFSU2_SERVER_BIN` for another build; on Windows it runs in WSL), as UG2's server unless the tool passes `--game`: a
`NFSU2_GAME` in the shell is not passed on:

    node --test --test-concurrency=1 web/test/deploy.test.mjs
    node --test web/test/audit.test.mjs web/test/players.test.mjs web/test/module.test.mjs web/test/data-blocks.test.mjs
    node --test web/test/lan.test.mjs
    node web/tools/online-pair.mjs ...      # and lan-pair, roam-pair, online-load, offline-test, deploy-e2e

(data-blocks' "past the 128 compressors" fails on a machine whose loopback buffers hold a whole answer.)  Real games:
online-pair (two games log in, race, the result recorded) and roam-pair (a friend's car shown at 0.10 m from where it
is) pass against it.  Keyphrase saves between two browsers, a real game's save among them:
`node web/tools/vault-pair.mjs --base <a running --dev server> --backup <zip> --play`.

`web/tools/server-compare.mjs` runs the same load against builds of the server (`--servers a=<bin>,b=<bin>`: a
change against main's, say), one after the other, and compares them.  Its `crowd`
load is `nfsu2-roamload`: up to 4000 simulated free-roam players (each a /lan WebSocket that takes bundles, a HELLO
and SNAPs to the hub, 20 snapshots a second to every friend the hub gives it), light enough to leave the cores to the
server, which measures what players feel: friends per player, snapshots lost, a snapshot's delay from player to
player, their pings, the messages each way.  `mix` is free roam and Online clients at once, as nfsu2.xyz has them.

    unshare -rn bash -c 'ip link set lo up; node web/tools/server-compare.mjs --load crowd,mix --n 2000 --seconds 60 \
        --server-cpus 0-11 --gen-cpus 12-23 --gen-threads 12'

On WSL run it inside a network namespace of its own (`unshare -rn`): with mirrored networking 127.0.0.1 goes through
Hyper-V's virtual NIC and netfilter, which cost more than the server (the VPS's tunnel reaches the server over a plain
loopback).  `--server-cpus`/`--gen-cpus` keep the server and the load on cores of their own, as two machines would.

`web/tools/mix-load.mjs` is nfsu2.xyz near its limit, on any machine (ps, not /proc): players driving, idle ones (free
roam on, in a race or the menus), Online clients (online-load.mjs) and more pages than places, so that a line forms,
with players leaving and coming back all the while (`--churn`), some dropping their link while they wait
(`--drop-wait`), and `--blip 150:0.4` as when a tunnel connection goes: 40 % of them at once, back as the page comes
back.  nfsu2-roamload's bots take compact bundles and deltas as the page does (`--plain`, `--compact` to compare) and
check every snapshot they get byte for byte against what its sender sent (its state is a function of the sender's
number and its clock), and the line's order, waits and tickets.

## Measured against the Node server (2026-10-21, 91e270c, Ryzen 9 5900X, WSL, the server on 6 cores, the load on the other 6)

| 2000 players in free roam | Node | Rust |
|---|---|---|
| players on the switch | 1339 (still joining) | 1999 (the ping probe is the 2000th: full) |
| friends per player | 8 (the hub's busy cap) | 32 |
| snapshot deliveries a second | 286,000 | 1,342,000 (all of them) |
| snapshots lost | 2.7 % | 0 |
| a snapshot's delay, player to player, p50 / p99 | 190 ms / 1.9 s | 11 / 22 ms |
| the switch's ping p50 / p99 | 40 / 526 ms | 0.7 / 6.8 ms |
| messages to the players a second | 14,000 | 131,000 |
| server CPU | 1.1 cores (its one thread full) | 4.0 cores |
| memory | 457 MB | 220 MB |

| 1800 in free roam and 200 in Online mode | Node | Rust |
|---|---|---|
| friends per player | 8 | 32 |
| a snapshot's delay p50 / p99 | 210 ms / 1.2 s | 10 / 22 ms |
| Online login p50 / p95 | 225 / 534 ms | 12 / 30 ms |
| Online chat p50 / p95 | 50 / 258 ms | 2.5 / 8.2 ms |
| races started in 60 s | 375 | 627 |

What a crowd costs is its messages, not its bytes: three quarters of the server's CPU is the kernel's TCP for each
WebSocket message (and the tunnel in front, cloudflared, pays for each again).  The bundle clock's tick decided it
(2000 players, 32 friends each): 4 ms sent 374,000 messages a second for 8.5 cores and did not keep up; 16 ms,
132,000 messages for 4.2 cores, everything.  So the tick followed the crowd: 4 ms up to 500 players taking bundles,
then as long as it took to send about 125,000 bundles a second in all, 25 ms at most.  Since 2026-10-24 it is 50 ms
for any crowd: at most 20 bundles a second to a page, as many as each friend sends snapshots (free roam draws friends'
cars 100 ms behind the soonest their snapshots came).  LAN play's datagrams (Online and LAN races) never wait for the tick: the game draws the other
racers from each update as it comes, and a wait of up to 25 ms at random made them wobble.  And the worker count: 2.9 cores on 4 workers, 3.8 on 8, 4.3 on 12, the same latency up to 8.

4000 players (2026-10-21, the server on CPUs 0-11, nfsu2-roamload on 12-23, 3999 bots and the probe, spread over
3 km; other sessions' loads on the machine the same day put 2000 players on 6 workers at the busy cap's edge, 5.0
cores and 81 % busy, on main as on this):

| 4000 players in free roam | 6 workers (the default for 12 CPUs) | 8 (a 16-core VPS's default) | 12 (`NFSU2_THREADS=12`) |
|---|---|---|---|
| refused / closed / snapshots lost | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| friends per player | 8 (the busy cap, workers 97 % busy) | 8 (79 % busy: the cap fell while they joined) | 32 (66 % busy) |
| snapshot deliveries a second | 515,000 | 453,000 | 1,920,000 |
| a snapshot's delay p50 / p99 | 39-46 / 120-160 ms | 34 / 89 ms | 35 / 73 ms |
| the players' own ping p50 / p99 (the load generator's 12 CPUs full too) | 27-33 / 100-140 ms | 25 / 75 ms | 27 / 62 ms |
| server CPU | 6.0 cores | 6.5 cores | 8.2 cores |
| memory (peak) | 370-420 MB | 370 MB | 420 MB |

The busy cap cuts bytes but hardly messages (each player still gets a bundle most ticks), so at 4000 on 6 workers it
falls to 8 friends and stays there; 12 workers held all 32.  A longer bundle tick (`NFSU2_BUNDLE_MAX_MS=40`) saved
nothing measurable.  Three quarters of the CPU is the kernel's TCP (`perf`: 47 % in sendto, of it 21 % the loopback
delivering to the receiver, 11 % recvfrom).

5000 places, mixed as nfsu2.xyz is (2026-10-23, `mix-load.mjs` on one 12-core M-series Mac, server and bots on the same
cores, the server on 8 workers; 1750 driving, 2750 idle, 500 Online clients and 600 pages more than places, each player
about 15 minutes, half of them leaving by a dropped link):

| 5 minutes at 5000 places | |
|---|---|
| players on the switch, waiting in line | 4839 (4960 at most), 759 (789) |
| server CPU, memory | 1.9 cores (2.1), 405 MB |
| snapshots checked byte for byte through deltas, bad | 330 million, 0 |
| a snapshot's delay p50 / p99 / max, lost | 13 / 42 / 160 ms, 0.1 % (to players who had just left) |
| let in from the line, let in after one who came later, a ticket's page back at its place | 1579, 0, 440 of 440 |
| the wait a page was told, against the wait | 1.4 to 1.7 times (the 5-minute pace over the line's first minutes) |
| Online: logins, p50 / p95 login, races | 42,081, 5 / 26 ms, 10,394 |

The same 1750 driving (2.5 min, every snapshot checked): bytes a driving player gets a second, plain bundles 72,900,
compact 55,500, compact with deltas 33,300 (40 % under compact, the page's before), at 2.3-2.4 cores each.  The server
of before (2933f54) against this one, 1750 driving, 1750 idle and 300 Online at 4000 places, compact bundles: 1.93
against 1.82 cores, 372 against 307 MB, delay p50 / p99 16 / 40 against 14 / 35 ms, an idle player 634 against 47
bytes a second (the peer list only for LAN play), 21 against 0 refused.  A tunnel's blip (40 % of 4700 at once) at
5000 with a line: all 1932 back at their addresses, p50 0.6 s and p90 1.4-1.9 s (the bots try again as the page does;
the Mac's backlog of 128 dropped the rest of the burst's handshakes).  Under 4500 of that load (17 min, 286 million
snapshots, 0 bad, 127,000 Online logins), real games: roam-pair (a friend's car 15 ms behind, 0.10 m from where it
is), lan-pair and online-pair pass.  3000 driving at once was more than the bots could take on the same Mac (they fell
behind, the switch dropped their snapshots; the server stayed at 1.5 cores): 4000 driving is the table above.

## How the work is shared out

- **HTTP** (nfsu2-http): the server's own HTTP/1.1 and TLS (rustls), a task per connection on a tokio runtime of
  several workers.  Not hyper: the audits' rules need answers of the server's own to requests a library refuses by
  itself, the pipelining cut, a keep-alive time per connection, and the connection budget before any TLS.
- **The switch** (nfsu2-lan): each connection's reader routes the frames it reads itself, on whatever worker it runs.
  Who is on the switch changes under one lock; frames find their receivers without it (a table of the 6000
  addresses, a copy-on-write list of everyone).  Each receiver's writer takes everything queued into one write, and
  makes bundles of a page's datagrams on the bundle clock.  Counters every frame touches are sharded.
- **Compact bundles** (nfsu2-lan `compact.rs`, kind 33): to a page whose hello says "compact":1 the datagrams go
  without what the page puts back exactly (its own address, free roam's port, a free-roam datagram's magic,
  version, length and CRC, which the switch checks first): the same bytes reach the game, a snapshot costs 84 bytes
  on the wire instead of 110.  The port was the limit on nfsu2.xyz (486 Mbit/s of 1000 at 2960 players).  When the
  hello says "delta":1 too, a snapshot goes as what changed since its friend's last one (a bitmap and the new
  bytes; the page keeps the same last bodies, and TCP loses nothing): about 40 % fewer bytes for a moving car and
  75 % for a standing one (estimated from the snapshot's fields, not measured on the live server), 50 ns an entry
  for the writer on an M-series Mac (`compact::tests::what_an_entry_costs`).
- **A player behind on its path** (nfsu2-lan `lag.rs`): a WebSocket ping every second times each player's round trip,
  queues past the server included (the kernel's, cloudflared's, the tunnel's).  While its data waits there, the
  player gets only a share of free roam's snapshots (a half for each late round trip, a sixteenth at once past
  1.5 s, an eighth back for each round trip without a wait), never fewer of its other messages.  On 2026-10-22 two
  of the Cloudflare Tunnel's connections on a lossy route left about 600 of 2950 players seconds behind while the
  server was idle; the 5-minute `[players]` line now counts the players more than a second behind and the snapshots
  left out.
- **The Online host and the free-roam hub** (nfsu2-online, nfsu2-roam): single-threaded, each on a thread of its
  own; the switch posts them their frames in order.  Past 4096 frames waiting at the Online host, a reader that feeds
  it waits (TCP backpressure); the hub's busy cap reads the runtime's workers' busy time, or the share of a CPU quota
  the server shares (nfsu2.slice: cloudflared's CPU too) where that is higher.
- **Compression** of the game data's runs on a pool of its own at a lower priority, so a burst of downloads never
  queues in front of the switch (Cloudflare serves the runs on nfsu2.xyz: this is the origin's cold start).

## The crates

| crate | |
|---|---|
| nfsu2-util | JS-compatible text and numbers (`\s`, `trim`, `Number#toString`, `toFixed`, `JSON.stringify`), token buckets and budgets, sharded counters, clocks, the busy measures, the switch's host interface |
| nfsu2-names | a name that reads as a slur (the rules of the page's web/src/app/names.mjs) |
| nfsu2-headers | the headers every answer carries, who the client is, the server's own names |
| nfsu2-http | the HTTP/1.1 engine and TLS, with the server's limits |
| nfsu2-ws | the WebSocket handshake and frames |
| nfsu2-lan | the switch |
| nfsu2-roam | the free-roam hub |
| nfsu2-online | the game's Online mode |
| nfsu2-vault | keyphrase saves (`/vault`): each player's saves as the page sealed them, one file per keyphrase, the copy of an earlier day, the caps, the sweep of the unused |
| nfsu2-server (src/) | the routes, the rules, the wiring, the start-up |
| nfsu2-roamload | thousands of simulated players: driving or idle, in line, leaving and coming back (as web/tools/roam-crowd.mjs, lighter) |

Each crate has its own tests (`build.sh --test`: 330 on 2026-10-21).  Where the Node server's answers had to be
matched exactly, they were checked against its JS while both existed, and the tests keep them as expected values
(the switch's name cleaning, stats.json and buddies.json, Math.hypot for the hub's links).  What still has a reference
of its own is checked against it, under node: names against web/src/app/names.mjs (the page's, 1.1 million names),
number printing against JS's, the HTTP engine and TLS against Node's http and tls (crates/http/tests/node), the
WebSocket frames against the ws library (and tungstenite).  `web/server-rs/tests/` puts the switch, the hub and the
Online host together as the server wires them, fuzzes the binary, and runs it on one scenario: start-up lines,
/players, console and files, and a state folder carried over a restart.

## Rules for changes

The same bytes on the wire and on disk as before: pages already open, the game and the state folders of running
servers depend on them (`nfsu2_util::json` for what JSON.stringify wrote, `nfsu2_util::js` for JS's text and number
semantics, Unicode tables held at Node's ICU version, Unicode 16).  Nothing a client sends may panic or hold
resources past their end, nothing it sends may cost the server work its budgets did not pay for (a broadcast walks
the peers only once paid for), and nothing is shared by every frame (no lock or atomic all frames touch, no
allocation a `Bytes` slice spares, but for the one copy of a frame that waits in a queue: a slice of what a reader
read keeps its whole block, nfsu2-lan's crate doc).  Judge a change by server-compare's crowd and mix at 2000
players.

## Where it differs from the Node server

- 6000 players (`--max-peers` 1 to 6000; the image's `NFSU2_MAX_PEERS=6000`), Node's 2000: the switch's network is
  10.77.0.0/19 (the welcome's mask 255.255.224.0; addresses round twenty-four of its /24s, 10.77.0-23.x), the free-roam
  hub takes 6144 players, Online mode 3000 lobby sessions, 900 games and 12000 streams.  The game module's peer bits
  (mailbox v4) cover a /21: for a player on 10.77.8-23.x the game asks that host itself (`runtime/web/net_lan.c`): a LAN race's
  connect to one who has just left sends its SYN and ends after the same 3 s with WSAETIMEDOUT instead of
  WSAEHOSTUNREACH.  Nothing else differs: the page and the module need no change (`crates/lan/tests/page.mjs`).
- A full switch keeps a line (nfsu2-lan `queue.rs`) for the pages that say they can wait (/lan?queue): Node closed
  every newcomer with "LAN full", and the page asked again every 5 s, so whoever asked first after a place freed up
  got it.  A page of before still gets "LAN full".
- The peer list goes only to the pages with LAN play open (the others get the server's own hosts): Node sent every
  join and leave to every page.
- Compressed bytes (gzip and br) differ from Node's zlib; the decoded content is the same.
- Bundles are made on the bundle clock (above), not per turn of an event loop; between different kinds of message
  only the order differs.
- The Online host's sends are charged to a sender's budgets on the host's thread, a moment after the frame was routed
  (the Node server did it at once).
- The hub's busy cap reads the workers' busy time, or a CPU quota's share (nfsu2_util::cpu::QuotaBusy) where that is
  higher; `[roam] the server's thread was N % busy` keeps its words.
- A ping every second besides the keepalive's (lag.rs), and a pong to any of the switch's pings queued since the
  keepalive's last tick says the client is there; `[players]` has a clause more.
- JSON a client sends: a lone surrogate escape reads as U+FFFD; a number beyond the doubles or nesting deeper than
  128 makes the switch ignore that message, where JS took it.
- Pipelined requests are answered one after the other (Node ran their handlers at once and wrote the answers in
  order); what a client sees is the same.  The engine's other edge cases are in `crates/http/src/lib.rs`.
