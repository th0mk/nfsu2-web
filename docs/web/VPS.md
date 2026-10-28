# The game server on a VPS

Status: **2026-10-19**, with Docker (the server's image and Caddy from one compose file). Without Docker (the
bundle and a systemd unit): [the last section](#without-docker).

Use a VPS when other PCs should play: on the Mac at home the server serves only that Mac (`http://localhost`). The
VPS runs the same server (`web/server-rs`) in a container, with Caddy in front for HTTPS, so friends
anywhere open `https://<your name>/` in Chrome and play from there.

## Setup

**What you need:**

- **A Linux VPS** (Debian or Ubuntu, say) that you reach as root over SSH, with a key ([SSH](#ssh)). 2 vCPU and
  4 GB of memory are plenty for friends; see [Sizing](#sizing) for more players.
- **A domain name** (or a subdomain) with an `A` record pointing at the VPS, and an `AAAA` record if it has IPv6.
- **Your own NFS Underground 2 folder** (the one with `SPEED2.EXE`).
- **The game module**, built on the Mac with `web/build.sh` (`out/web/nfsu2.wasm` and `out/web/nfsu2.json`).
- **The image:** `ghcr.io/th0mk/nfsu2-web-rs`, public, so the VPS pulls it without logging in. It holds the server
  and the page, never the game. Of a fork, or to build it yourself: [The image](#the-image).

**1. On the VPS**, as root: Docker, the ports, two folders and your name.

```sh
docker compose version || curl -fsSL https://get.docker.com | sh    # Docker and its compose plugin, if not there yet
ss -ltn | grep -E ':(80|443|8080) '                                  # nothing: the ports are free
mkdir -p /srv/nfsu2/game /srv/nfsu2/module
echo NFSU2_HOST=game.example.com > /srv/nfsu2/.env                  # your name
```

- **Docker there already** (other containers on the VPS): keep it. Installing it again would restart Docker, and
  with it every container. The game's containers and volumes are its own (`nfsu2-web-…`), and `docker compose` in
  `/srv/nfsu2` touches only those.
- **8080 in use** (on any address): pick a free port for the server, `echo NFSU2_HTTP_PORT=8090 >> /srv/nfsu2/.env`.
  It listens on 127.0.0.1 only; Caddy follows it.
- **80 in use, 443 free** (an API or site that Docker publishes on port 80, say): Caddy does without 80. Copy
  `compose.443.yml` too (step 2) and `echo COMPOSE_FILE=compose.yml:compose.443.yml >> /srv/nfsu2/.env`. What
  that changes: [Port 80 taken](#port-80-taken).
- **443 in use:** this Caddy cannot start next to it. Use [the Cloudflare Tunnel](#a-cloudflare-tunnel-instead-of-caddy),
  which needs no port at all, or put the game behind the proxy you have
  ([A proxy you already have](#a-proxy-you-already-have)).

**2. From the Mac**, in the checkout: the compose file and the module, then the game's own files from inside the
game folder.

```sh
scp web/deploy/compose.yml root@<vps>:/srv/nfsu2/
scp web/deploy/compose.443.yml root@<vps>:/srv/nfsu2/              # only when port 80 is taken (step 1)
scp out/web/nfsu2.wasm out/web/nfsu2.json root@<vps>:/srv/nfsu2/module/
cd "<your game folder>"
rsync -rlt SPEED2.EXE server.dll server.cfg CARS CREDITS FRONTEND GLOBAL LANGUAGES memcard MOVIES NIS pfdata \
  SOUND speech SUBTITLES TRACKS root@<vps>:/srv/nfsu2/game/
ssh root@<vps> chmod -R u=rwX,go=rX /srv/nfsu2/game /srv/nfsu2/module
```

The game takes about 3 GB. Why these lines: [Copying the game](#copying-the-game). The voices and movies of other
languages are an optional extra 1.6 GB: [Languages](#languages).

**3. The firewall** must let in HTTP and HTTPS (443 for TCP and for HTTP/3's UDP; with `compose.443.yml` only
443). On the VPS, `ufw status`:

- `active`: `ufw allow 80/tcp && ufw allow 443` (with `compose.443.yml`: `ufw allow 443`);
- `inactive` or no ufw: nothing to do on the VPS. Do not turn a firewall on just for the game: one that is new on
  a VPS with other services blocks those that listen without Docker publishing their ports (on the host's network,
  say) until you allow them too;
- a firewall in your provider's web panel: open 80/TCP and 443/TCP+UDP there.

**4. Start it:**

```sh
cd /srv/nfsu2 && docker compose up -d && docker compose logs -f server
```

- Caddy gets a Let's Encrypt certificate for your name within seconds and renews it by itself
  (`docker compose logs caddy` says `certificate obtained successfully`).
- `docker compose ps` shows the server `healthy` once it serves the module. Docker starts both containers again
  after a reboot.

**5. Play:** open `https://game.example.com/` in Chrome. The start page says it is ready, and Play starts the game.

**Updating:** copy the new module (step 2, the `nfsu2.wasm` line and the `chmod` line), then on the VPS
`cd /srv/nfsu2 && docker compose pull && docker compose up -d`. The players' browsers notice the new module by its
ETag and download it once. The module and the image must come from the same commit: [The image](#the-image).

Or all of it in one command from your own machine, with the folder that holds the new `nfsu2.wasm` and `nfsu2.json`:

```sh
web/deploy/release.sh root@<vps> <folder>              # the module, and the image of its commit (sha-<rev>)
web/deploy/release.sh root@<vps> <folder> <commit>     # the image of a later commit that changed only the page or server
web/deploy/release.sh root@<vps> --undo                # back to the module and image before
```

It checks that the registry has the image (CI cancels a commit's image when a newer push comes), pulls it on the VPS
before anything changes there (a pull that fails leaves the site as it was), copies the module into place by
renames, sets `NFSU2_IMAGE` in `.env`, restarts only the server and waits until it is healthy; `-n` first only says
what it would do. Run again after a failure, it keeps the module and image of before for `--undo`. Last it writes
`module/release.json`, the module's and the image's commits, for the forum bot (`discord-bot/`): after a release of
only the page or the server, the image's commit is what is live. `NFSU2_DIR` names another folder than `/srv/nfsu2`.

**When something is wrong:**

| What you see | Why |
|---|---|
| `docker compose up` says `set NFSU2_HOST in .env` | `/srv/nfsu2/.env` is missing, or not next to `compose.yml` |
| pulling the image says `unauthorized` | the image (a fork's) is private: make it public, or [log in](#the-image) |
| the server keeps `Restarting`, its log says `usage: ... (got: /game)` | `/srv/nfsu2/game` has no `SPEED2.EXE` |
| the server stays `health: starting`, then `unhealthy` (after about 2½ minutes) | `/srv/nfsu2/module` has no `nfsu2.wasm` and `nfsu2.json` |
| Caddy keeps `Restarting`, its log says `listen tcp :80: bind: address already in use` | something else has port 80: `compose.443.yml` (step 1) |
| the same for `:443` | something else has port 443: [the tunnel](#a-cloudflare-tunnel-instead-of-caddy) |
| Caddy's log says `challenge failed` | the DNS record does not point at the VPS yet, or 80 and 443 are closed (with `compose.443.yml`: 443) |
| the server keeps `Restarting`, its log says `EACCES: permission denied, scandir '/game/…'`; or a game file gets a 502 | the game folder is not readable by everyone: run the `chmod` line again |

> **Who can play.** The server has no accounts, passwords or allowlist. Anyone who can reach it can play and
> download the game files from it, so your firewall decides who that is. Behind Cloudflare's proxy the VPS only
> sees Cloudflare's addresses.
>
> **What players see of each other:** the names they chose and, with free roam on, their status, car and position;
> never another player's real address. The server answers only for its own name (`NFSU2_HOST`): a page of another
> site whose name points at the VPS gets nothing, and a scanner that connects to the VPS's bare address gets no
> certificate and no page.

The rest of this guide is reference.

## What it serves

- **Nothing changes in how it behaves.** Only TLS differs: Caddy (or Cloudflare) provides HTTPS.
- **What it serves:** the page, the game module, the game's own files from your game folder (read-only) and the
  multiplayer switch. Of the game folder it serves only `SPEED2.EXE`, `server.dll`, `server.cfg` and the game's
  data folders (`CARS`, `CREDITS`, `FRONTEND`, `GLOBAL`, `LANGUAGES`, `memcard`, `MOVIES`, `NIS`, `pfdata`, `SOUND`,
  `speech`, `SUBTITLES`, `TRACKS`), the language packs (`LANGPACKS`, [Languages](#languages)) and the texture packs
  (`TEXPACKS`, [Texture packs](#texture-packs)), compared without case, and everything in those folders except dot
  files.
  Nothing else at the top of the folder: never an installer's log (`unins000.dat` names the PC, the Windows account
  and the install time), a `SAVE` folder, mod plugins and their settings, or anything else you keep there.
- **What it never shows:** anything about the VPS or the other players (`web/test/deploy.test.mjs` checks what is
  and is not exposed).
- **What it keeps for players:** their keyphrase saves, locked ([Keyphrase saves](#keyphrase-saves)), and the Online
  mode's race results and EA Messenger lists, with the messages that wait for a friend
  ([Multiplayer over the internet](#multiplayer-over-the-internet)).

## Keyphrase saves

A player can keep their saves on the server under a keyphrase (`nfsu2-xyz-` and 8 words), and load them in any
browser by typing it (the page's Cloud saves: PLAYING.md, Saves). The page locks them before they leave the browser (AES-GCM, with a key made
from the keyphrase); the server keeps those bytes under the SHA-256 of a token also made from the keyphrase. It never
sees a keyphrase, a key or a save, and its files name no player.

- **Where:** `vault/` in the state folder (the state volume): one file per keyphrase, `vault/<3 hex>/<64 hex>`,
  with the current version and the copy of an earlier day. A player's saves without screenshots are 6 to 14 KB:
  100,000 players take about 1.5 to 3 GB on the disk, both copies counted.
- **How much room:** `NFSU2_VAULT_GB` in `.env` (`--vault-gb`; 50 when it is not set). New keyphrases stop at 90 % of
  it, and any save that would grow a file stops at all of it; what is kept stays readable. Give it what the disk can
  spare, leaving room for the rest (150 GB free: `NFSU2_VAULT_GB=140`). It counts 4 KB blocks, as ext4 takes them;
  each keyphrase also takes one inode (`df -i`). `0` turns keyphrase saves off, and the page then does not offer
  them.
- **How long:** a keyphrase nobody used (no save, no load) for two years is deleted at the daily sweep. The console
  says so then, counts only: `[vault] 81234 keyphrases, 1.20 GB of 140.00 GB (counted in 0.4 s); 3 removed after two
  years unused`. A flag in a file's head keeps one without end, for an operator to set (no tool sets it yet).
- **What one client may do** (an IPv4 address, an IPv6 /56; twice that for its /48): 20 new keyphrases, then 20 a
  day; 60 saves, then one every 2 s; 60 loads, then one a second; and 30 loads of keyphrases there are not, then 30
  an hour, so nobody learns anything by guessing. A version is at most 256 KB. More gets 429 (or 413, or 507 when the
  room is full).
- **What all clients may do:** the saves add at most as many bytes a day to the disk as fill the room for new
  keyphrases in 30 days (50 GB: 1.5 GB a day; 140 GB: 4.2 GB; the start-up line says), a quarter of that at once. A
  new keyphrase counts as 32 KB at least (a player's saves pack into 6-14 KB, two versions of them), so 50 GB takes
  about 45,000 new keyphrases a day; a save that grows its keyphrase past 32 KB counts what it adds. More gets 503 and
  the console a line `[vault] refused a save: the 1.50 GB a day the saves may add are taken for now`. Without it,
  anyone with enough addresses could fill the room within days, and keep every new player out for the two years the
  files stay. A save that keeps its size, or stays within 32 KB, never waits for it.
- **The disk:** any save that would grow a file stops (507, `[vault] refused a save: the disk has less than ...`)
  while the disk under the state folder has less than 2 GB or 100,000 inodes free, whatever `NFSU2_VAULT_GB` says.
- **Backups:** copy `vault/` while the server runs: each file is replaced whole, by a rename. Without the folder the
  players' copies on the server are gone; their browsers keep their own.

## Sizing

For up to 6000 players at once (the image's limit; the Node server's was 2000, [From the Node
server](#from-the-node-server)): 8 vCPU for 2000 and 16 for 4000 to 6000 (4000 in free roam took the Rust server 6-8 cores of a Ryzen 9 5900X,
and a Cloudflare Tunnel's cloudflared pays for each message again, `web/server-rs/README.md`), 8 GB of memory (the
compose file's limit), 40 GB of disk and a 1 Gbit/s port with a few TB of traffic a month.

- Measured on a live server behind a Cloudflare Tunnel (16 vCPU, 2026-10-21): 1209 players, 165 of them in the
  game's Online mode and most of the rest in free roam, took the server 0.7 of a core over its 8 workers and 340 MB,
  and cloudflared 1.7 cores. 2000 players at once are **UNVERIFIED** there; on a Ryzen 9 5900X the server held 2000
  simulated players in free roam, 32 friends each, on 4.5 of 6 cores (`web/server-rs/README.md`).

- The server relays and serves files. It used about 110 MB of memory here, plus up to 10 MB queued towards each
  player who falls behind (8 MB on the connection, 2 MB more of LAN-race data in the switch's own queue).
- The game folder takes about 3 GB of disk.
- Each new player downloads the 7 MB module (again after each new build; with the Cache Rule below from Cloudflare,
  which takes it from the server once) and then the game data they play (about 70 MB for a quick race, up to
  the whole 3 GB over a career), once: the browser keeps it. The data is compressed on the way (br, kept at its best,
  below): all but the movies is 1.28 GB on the network for 2.09 GB, the tracks about a third; the movies' video
  does not compress and goes as it is. "Preload the game data" on the start page fetches all but the movies at
  once, 4 requests at a time, so a player on a slow line never waits on a loading screen. It asks for one block a
  request at the addresses the game's own reads use, so with the Cache Rule below Cloudflare serves both.
- Compressing costs the server about 5 s of one core per player for that whole download (zstd; br about 1.5 times
  that). At most 128 answers are compressed at once (8 per player, about 200 MB of memory); past that they go as
  they are, never held up. The compressing runs on threads of its own (half the cores, at least two) at a lower
  priority than the switch, so a burst of downloads takes only what the players' traffic leaves.
- The server also compresses each 1 MiB block of the game data once at brotli's best and keeps it in the state
  volume (`blocks/`, 1.5 GB for a game folder with every language pack). All but the movies then crosses the network
  in 1.28 GB instead of the 1.39 GB of compressing as it is sent (the tracks 41 -> 33 % of their size, SPEED2.EXE
  50 -> 41 %, the voices 84 -> 79 %, the music 86 -> 84 %; the movies not at all), at no CPU per player. Two threads
  at the lowest priority make them after each start and each change of the game files (a manifest `?refresh`), the
  blocks players ask for first: about half an hour for the whole folder (5600 blocks, 3100 that compress). Their two
  cores count against `nfsu2.slice`'s quota all the same, so they wait while the slice is 70 % used and go on below
  55 % (`kept blocks: waiting while /nfsu2.slice uses ...`, then `going on`): on a busy evening the folder takes
  longer, and free roam's busy cap never sees them. Without room (16 GB in `blocks/`, or less than 2 GB free on its
  disk) they wait too, the room looked at again every minute. Until a block is kept it goes out as before, compressed
  as it is sent, and
  Cloudflare keeps that answer 10 minutes only. The page asks at `/data-blocks/v2/` since, so Cloudflare's copies of
  the answers from before are not served for them (the Cache Rule covers both). `NFSU2_COMPRESS_THREADS` in the
  server's environment sets the threads (0: none, every block as before). The `[blocks]` line every 5 minutes counts
  the answers: kept blocks, compressed as sent (zstd, br, gzip), as they are. Through Cloudflare nearly all should be
  kept ones once the blocks are made; none at all would mean Cloudflare asks the server for gzip only.
- Free roam is at most about 25 KB/s each way per player: the server's free-roam hub pairs each player with the 7
  nearest, and snapshots go between those only. 2000 players roaming at once are about 400 Mbit/s each way through
  the VPS, 4000 about 800 Mbit/s. Measured with 62 simulated players and 2 real pages on one server
  (`web/tools/roam-crowd.mjs`, a Ryzen 9 5900X): about 9,500 datagrams a second each way through the switch, the
  server at 11 % of one core. More players than that, and a real VPS's CPU, are **UNVERIFIED**.

### A share of the VPS

The compose files put every container of theirs (the server, Caddy, or cloudflared and its namespace) into the systemd
slice `nfsu2.slice` (`cgroup_parent`, `NFSU2_SLICE` to name another), and `web/deploy/nfsu2.slice` gives that slice at
most 14 of a 16-vCPU VPS (`CPUQuota=1400%`; for another size, its vCPUs less what the rest needs, times 100), so the
rest of the VPS keeps at least 2 cores whatever the players do. The server and cloudflared share the 14 as their load
goes. The server sees what the whole slice uses (`cgroup: host` in `compose.yml`: the VPS's cgroup tree, read only),
and over 85 % of the quota the free-roam hub's busy cap gives players fewer friends, as it does when the server's own
workers are busy: fewer car states to send costs cloudflared less too (bytes, not messages: each player still gets a
bundle every 50 ms). At the limit they are slowed down, never the rest. The server's console says the quota it found
at the start (`CPU quota : 14 cores for /nfsu2.slice`; "none seen" in a container without `cgroup: host`), and every
5 minutes what the slice used (`[players] CPU in the last 5 minutes: the server 1.09 cores, its switch's workers 13 %
busy; /nfsu2.slice 4.51 of its 14 cores (32 %)`).

    cp web/deploy/nfsu2.slice /etc/systemd/system/ && systemctl daemon-reload
    docker compose up -d --force-recreate
    systemctl status nfsu2.slice                               # the containers in it
    systemctl show nfsu2.slice -p CPUQuotaPerSecUSec           # 14s

A new quota for a slice that runs already needs no recreate: after the `cp` and `daemon-reload`,
`systemctl set-property --runtime nfsu2.slice CPUQuota=1400%` sets it at once (`cat
/sys/fs/cgroup/nfsu2.slice/cpu.max`: `1400000 100000`), and the file keeps it after a reboot.

It needs Docker's systemd cgroup driver (`docker info`: `Cgroup Driver: systemd`, the default with cgroup v2).
Without the file the containers still start, in a slice systemd makes for them with no limit; with the cgroupfs
driver (Docker Desktop) `nfsu2.slice` is a plain cgroup, also with no limit.

How far the cores go: 1209 players took the server 0.7 of a core and cloudflared 1.7 (2026-10-21); 4,294 took the
server 3.4 and cloudflared 8.9, 11.8 in all, when the slice was 12.8 (2026-10-23). 4000 simulated
players in free roam took the server 8.2 cores on 12 workers at 32 friends each (`NFSU2_THREADS=12`), 6.5 on 8
workers at the busy cap's 8 (`web/server-rs/README.md`); cloudflared pays for each message again, so 4000 players
are about where the slice's limit is reached, and past it free roam gives fewer friends instead of taking more of the
VPS.

## The image

- **What it holds:** the server (one static binary) and the page: about 7 MB to download, on Alpine. **Never the
  game**, or anything made from it: the build sees only the server's and the page's sources
  (`web/server-rs/Dockerfile.dockerignore`, checked by `web/test/docker.test.mjs`), so a public image cannot carry
  the game. What it needs is mounted when it runs:

  | In the container | What | On the VPS (default) | |
  |---|---|---|---|
  | `/game` | your game folder | `/srv/nfsu2/game` | read-only |
  | `/module` | `nfsu2.wasm` and `nfsu2.json` | `/srv/nfsu2/module` | read-only |
  | `/state` | the server's keys, the Online mode's records, the keyphrase saves, the kept blocks | the volume `nfsu2-web_state` | |

- **From GitHub's registry:** `.github/workflows/image-rs.yml` builds `ghcr.io/th0mk/nfsu2-web-rs` for amd64 and
  arm64 on every push: `latest` from `main`, `sha-<commit>` from every push, and `<version>` and `<major>.<minor>`
  from a `v*` tag. It is public. A fork's package is private while its repository is, and then the VPS needs one of:
  - **public:** the package's settings on GitHub, "Change visibility";
  - **a login:** a GitHub token that may read packages (a classic personal access token with `read:packages`),
    once on the VPS: `docker login ghcr.io -u th0mk`, with the token as the password. Docker keeps it in
    `/root/.docker/config.json`;
  - **built yourself** (below).
- **Built yourself**, with no registry at all: on any machine with Docker, in the checkout, then into the VPS's
  Docker over SSH.

  ```sh
  docker build -f web/server-rs/Dockerfile --platform linux/amd64 -t nfsu2-web .   # linux/arm64 for an Arm VPS
  docker save nfsu2-web | ssh root@<vps> docker load
  ssh root@<vps> 'echo NFSU2_IMAGE=nfsu2-web >> /srv/nfsu2/.env'
  ```

  It needs BuildKit (the buildx plugin; Docker Desktop has it). Building for the other architecture needs no
  emulation: the server is cross-compiled (cargo-zigbuild, static musl), and the image's own stage only copies. The
  first build takes a few minutes. To update, build and load again, then `docker compose up -d` (without `pull`:
  there is nothing to pull).
- **The module must come from the image's commit.** The page in the image and the module meet at the module's
  imports, and nothing checks that the two come from the same revision. `nfsu2.json`'s `rev` (the short commit,
  with `+` if the checkout had changes) and the image's tag `sha-<commit>` should name the same commit. On the VPS:

  ```sh
  grep -o '"rev":"[^"]*"' /srv/nfsu2/module/nfsu2.json
  docker image inspect --format '{{index .Config.Labels "org.opencontainers.image.revision"}}' ghcr.io/th0mk/nfsu2-web-rs
  ```

  A self-built image has no revision label: build it and the module from the same checkout.

## From the Node server

Until 6076514 (2026-10-21) the server was a Node.js program (`web/server`, the image `ghcr.io/th0mk/nfsu2-web`),
which did everything on one thread: about 600 players in free roam filled it
([Multiplayer over the internet](#multiplayer-over-the-internet)). The Rust server took its place with the same
options and `NFSU2_*` environment, mounts, health check and user (uid 1000), and the same files in the state volume.
A VPS that still runs the Node image switches with one line in `/srv/nfsu2/.env`, then
`cd /srv/nfsu2 && docker compose pull && docker compose up -d`:

```sh
NFSU2_IMAGE=ghcr.io/th0mk/nfsu2-web-rs:sha-<commit>
```

- The compose files (and your `COMPOSE_FILE`: `compose.443.yml`, `compose.tunnel.yml`), the game folder, the module
  and the state volume `nfsu2-web_state` stay as they are. The ETag key, the switch's addresses and the Online
  records carry over: no browser downloads the game data again, and the pages reconnect as after an update.
- **Back to Node:** `NFSU2_IMAGE=ghcr.io/th0mk/nfsu2-web:sha-<commit>` of 6076514 or before, with the module of that
  commit, then `docker compose up -d`. The state volume serves either.
- `UV_THREADPOOL_SIZE` means nothing to the Rust server; `NFSU2_THREADS` sets its worker threads (by default half
  the cores, 2 to 8). Its console has the Node server's lines.
- **6000 players** where the Node server took 2000 (`--max-peers` 1 to 6000, the image's `NFSU2_MAX_PEERS=6000`): the
  virtual network is 10.77.0.0/19 (addresses in 10.77.0-23.x), and the game module's peer bits cover a /21, so for a player on 10.77.8-23.x the
  game asks that host itself (a LAN race's connect to one who has just gone ends after the same 3 s as "timed out"
  instead of "unreachable"; nothing else differs, and pages and modules of before need no change). Online mode takes
  3000 lobby sessions and 900 games (Node: 1000 and 300), the free-roam hub 6144 players. 6000 players need 65536
  open files (`ulimits: { nofile: 65536 }` in `compose.yml`), and past about 3000 in free roam more workers than 8
  (`NFSU2_THREADS=12` in `.env`; `compose.yml` passes it on) keep them at 32 friends each instead of the busy cap's 8.

Tested 2026-10-21 on Docker Desktop (WSL), the images built from the checkout: amd64 and arm64 built; the amd64 one
run as `compose.yml` runs the server (a read-only root, no capabilities, `no-new-privileges`) was `healthy` within
6 s; the Node image, then the Rust one, then the Node image again on one state volume kept the ETag key (the module's
and the data manifest's ETags stayed the same).

On a real VPS, 2026-10-21: nfsu2.xyz (16 vCPU, `compose.yml:compose.tunnel.yml`) went from the Node image to
`nfsu2-web-rs:sha-6076514` with the `.env` line and `docker compose up -d server`, with 970 players on. It was
`healthy` within 4 s; the pages came back on their old switch addresses (`is back`), Online read its records from
the volume, and the module stayed (the commit changed no page or module file). Six minutes later, 1209 players (165
in Online): the server 0.7 of a core over its 8 workers (about 8 % each; the free-roam hub 1 %), 340 MB, free roam
not busy; cloudflared 1.7 cores; the VPS 80 % idle. The Node server's one thread had been at 100 % with 820-960
players the evening before.

## Copying the game

- **Only the game.** The `rsync` line lists what the server serves; nothing else of the folder needs to be on the
  VPS (an installer's log, saves, mod settings). `web/deploy/pack.sh --data "<your game folder>"` lists what your
  folder holds besides these. On a folder whose names differ in case (`Sound`), use the names it has.
- **The times are kept** (`-t`). A file's time is part of its ETag, so a copy that changes the times makes every
  player's browser download the game data again.
- **Owned by root, readable by everyone.** `-rlt` copies the files, folders and their times, not their owner,
  group or modes: on the VPS they belong to root (`rsync -a` as root would keep your Mac's uid 501). The server
  runs as the image's user `node` (uid 1000), so the game folder and the module must be world-readable: the
  `chmod` does that (folders 755, files 644). Run it again after every copy: new files arrive with the Mac's modes.
- **Not `--chmod`.** The Mac's own `rsync` (openrsync) applies `--chmod` only to the files named on its command line,
  not to what is inside the folders (`TRACKS/` stayed 700, its files 600), and it refuses `D755,F644`. Checked:
  the lines above from the Mac's openrsync to rsync 3.5.0 on Debian, and with rsync 3.5.0 on both ends.
- **From Windows:** the same lines in WSL, with the game folder under `/mnt/<drive>/`.
- **Elsewhere:** set `NFSU2_DATA` (the game folder) and `NFSU2_MODULE` (the module's folder) in `.env`. By default
  they are `./game` and `./module` next to `compose.yml`.
- **Copyright.** The game folder is EA's copyrighted content, and the server hands it to anyone who can reach it.
  That is one more reason to keep the firewall tight.

## Languages

Every player picks the game's language in Settings. The North American game folder has the text of thirteen,
but English voices and movies only, and short early versions of the Chinese, Japanese and Thai text. The rest comes
from KyoriAsh's "Multi 13 languages" mod ([nfsmods.xyz/mod/5749](https://nfsmods.xyz/mod/5749)), for Brazilian
Portuguese TraduMix's and MixMods' translation, for Polish PLProjekt's, for Turkish muntazam's and for the English
subtitles VileStuff's "English Subtitles Fixed" ([nfsmods.xyz/mod/5286](https://nfsmods.xyz/mod/5286)), as language
packs: a `LANGPACKS` folder in
the game folder, one folder per language, whose files a game in that language reads in place of the game folder's
(`runtime/language.c`). The game folder itself stays as it is.

1. Download the mod's archives (Core and the dubs) and extract them all into one empty folder, not the game folder.
   The dubs need RAR 7's `unrar` (`unrar x`): libarchive (`bsdtar`, Windows' `tar`) cannot read them.
2. Download the Portuguese translation ("Tradução PT-BR, V1.1 (TraduMix e MixMods)", from
   [joao13traducoes.com](https://joao13traducoes.com/2018/08/pc-need-for-speed-underground-2-traduzido-e-dublado-varios-autores/))
   and extract it into another empty folder (`unrar x -p'João13'`). To include the Brazilian Portuguese dubbing by
   ZANIT, also extract its archive into a folder (`speech/UG2_Speech.big`, `MOVIES/*.vp6`).
3. Download the Polish translation (PLProjekt's v1.1, from
   [portal24h.pl](https://www.portal24h.pl/spolszczenia-gier/spolszczenia-gier/mo/Need-For-Speed-Underground-2-(plprojekt)-1456/))
   and extract it into a third empty folder (`unrar x`). It holds two Windows installers; the tool reads either one's
   files and never runs it.
4. Download the English subtitles (`English Subtitles Fixed (v1.0).7z`) and extract it into a fourth empty folder
   (`7z x`; `bsdtar` and Windows' `tar` read it too). The tool reads its Binary script; neither Binary nor the
   Extra Options mod it asks for is needed.
5. Download the Turkish translation (`need_for_speed_underground_2_turkish_translation_patch_v1.1.zip`, muntazam's
   "Need for Speed Underground 2 Turkish Translation Patch" v1.1 at
   [moddb.com](https://www.moddb.com/mods/need-for-speed-underground-2-turkish-translation-patch)) and extract it into
   a fifth empty folder. It holds a Windows installer, `nfsu2tr11.exe` (Inno Setup): unpack it there with
   [innoextract](https://constexpr.org/innoextract/) (`innoextract nfsu2tr11.exe`, into `app/`), never run. The
   Extra Options mod it asks for is not needed either.
6. `tools/langpacks.py "<your game folder>" --multi13 "<the mod's folder>" --ptbr "<the translation's folder>" [--ptbr-dub "<the dub's folder>"]
   --pl "<the Polish folder>" --subs "<the subtitles' folder>" --tr "<the Turkish folder>"` builds `LANGPACKS`
   (1.6 GB) and prints what each language got: the dubbed voices and movies of French, German, Italian, Spanish,
   Japanese and Russian, the full Chinese, Japanese and Thai text with their fonts, Russian, Portuguese, Polish and
   Turkish in English's place (Polish and Turkish with their fonts), the Danish, Dutch and Swedish EA logos, and
   English's text with the movies' lines whole (0.3 MB). Korean needs nothing. Any source can be left out: the
   others' packs stay as they are.
7. Copy it like the rest, from inside the game folder:
   `rsync -rlt LANGPACKS root@<vps>:/srv/nfsu2/game/`, then the same `chmod`.

A player's browser fetches only their own language's pack: "Preload the game data" adds about 50 MB of dubbed
voices (Russian 94 MB), and the dubbed movies (206 MB in all) stream in place of the English ones (725 MB). Without
`LANGPACKS` every language but Russian, Portuguese, Polish and Turkish still plays its text, with English voices and
movies; those four play in English. All but French, German, Italian and Spanish (whose texts leave the lines their dubs
speak empty) show the movies' subtitles (Settings, Game, Subtitles, on by default), English only with its pack: the
game folder's English lines stop at their first apostrophe ("But if you"). The North American game shows none.

## Texture packs

Settings, Display, More, Menus and HUD, Upscaled (Original is the default) shows LunicAura106's "Upscaled HUD" (UHUD,
[nfsmods.xyz/mod/4319](https://nfsmods.xyz/mod/4319)): the menus, the race HUD, the fonts and the car and brand logos
redrawn at four times their size. It comes from a texture pack, `TEXPACKS/uhud.pak` in the game folder, which the
port reads in place of the game's textures on the GPU only (`runtime/web/texrep_web.c`): the game keeps reading its
own files and keeps its layout. The mod's own installer and plugin are not used.

1. Download the mod's archive (`NFSU2UHUD11192022a.7z`) and extract it into an empty folder, not the game folder
   (`7z x`; `bsdtar` and Windows' `tar` read it too).
2. `tools/texpacks.py "<your game folder>" --uhud "<the mod's folder>"` builds `TEXPACKS/uhud.pak` (101 MB, a few
   seconds) and prints what it holds.
3. Copy it like the rest, from inside the game folder:
   `rsync -rlt TEXPACKS root@<vps>:/srv/nfsu2/game/`, then the same `chmod`.

A player's browser fetches the pack only with Upscaled; its blocks compress to about 5 MB ("Preload the game data"
counts its 101 MB). Without `TEXPACKS` Upscaled plays with the game's own textures.

## Smaller music

The game's music, `pfdata/MusicSFx.mus`, is 510 MB: 27 of its 43 songs are 4-bit ADPCM at 382 kbit/s, and 30 of its
73 streams are never played (copies that the game's song table does not name). `tools/musicpack.py` rebuilds it with
those 27 songs in EALayer3, the MP3 variant the game already plays its other 16 songs in, and without the unused
streams: 235 MB, with a `MusicSFx.mpf` that points at the songs' new places. The game plays it with its own decoder
and no change to the port. Every song keeps its length to the sample, its first 47 and last 100 samples are the
original's (as in EA's own EALayer3 songs), and the 16 EALayer3 songs are the same bytes. The new songs are LAME VBR
`-V 1` (231 kbit/s on average), which the PEAQ measure rates as imperceptible against the ADPCM (-0.17 to -0.02,
0 being no difference). Over the network the file takes 231 MB instead of 425 MB, and a song while it plays 24 %
fewer bytes (206 instead of 271 kbit/s on average).

1. `apt install lame` (LAME 3.100: the same game folder and LAME make the same bytes).
2. `tools/musicpack.py "<your game folder>" <an empty folder>` (about a minute on 12 cores) writes `MusicSFx.mus`
   and `MusicSFx.mpf` there. It reads the game folder and never writes to it.
3. Copy both to the VPS next to the game folder, not into it: `rsync -t MusicSFx.mus MusicSFx.mpf
   root@<vps>:/srv/nfsu2/music-new/`.
4. On the VPS, keep the originals and move the new pair in with one command, so no page sees one new file and one
   old: `cd /srv/nfsu2 && mkdir -p music-orig && cp -a game/pfdata/MusicSFx.* music-orig/ && chmod 644
   music-new/* && mv music-new/MusicSFx.mus music-new/MusicSFx.mpf game/pfdata/`, then open
   `https://<your site>/data-manifest?refresh`. Going back is the same `mv` from `music-orig`.

A player who is in a game at that moment keeps the old song table: when their browser asks for a part of the music it
has not kept and Cloudflare has not either, the music stops or turns to noise until they reload the page (tested: the
game itself carries on). A quiet hour keeps that to a few players. Browsers keep the old file's blocks they already
have (the cache names a file by its size and time) until the player deletes the game data.

## Movies in AV1

The game's movies are VP6 at 5 to 12 Mbit/s, the most a player downloads after the game data. Browsers that decode
AV1 (Chrome, Edge and Firefox, on computers and phones; Safari on the Macs and iPhones that decode it in hardware, M3
and iPhone 15 Pro on) can play them as AV1 instead, at about a fifth of the size and looking the same: a copy of each
movie in an `AV1` folder next to it (`MOVIES/AV1`, `LANGPACKS/<language>/MOVIES/AV1`), which the page opens in its
place (`runtime/movie.c`). Safari on older Macs and iPhones, and any browser without AV1, keeps playing the VP6
movies, so the game folder keeps them too.

1. `sudo apt install ffmpeg` (the encoder is its libaom), then from the repository:
   `tools/movies_av1.py "<your game folder>" <out>`. The AV1 files land in `<out>`, at their places in the game
   folder: the game folder's 965 MB of movies become 179 MB, each dubbed language's 216 MB 153 MB. It took about
   50 minutes on 12 cores; a second run makes only what is new, and the dubbed languages share their pictures, so
   those are encoded once.
2. Optional: `tools/movies_av1.py --check "<your game folder>" <out> --ffmpeg <an ffmpeg with libvmaf>` measures
   each AV1 movie against its VP6 one (VMAF; 95 or more looks the same).
3. Copy them over like the rest, from inside `<out>`:
   `rsync -rlt MOVIES LANGPACKS root@<vps>:/srv/nfsu2/game/`, then the same `chmod`, and open
   `https://<your site>/data-manifest?refresh`.

The order of this and a new version does not matter: a page that does not know the AV1 files never opens them, and
one that does opens a movie's AV1 copy only where there is one. `NFSU2_MOVIE_CODEC = vp6` in a player's settings.ini
plays the VP6 movies anyway.

## How it runs

- **The VPS's own network.** Caddy and the server use the VPS's network (`network_mode: host`), as they would
  without Docker: Caddy on 80 and 443, the server on 127.0.0.1:8080, which nothing outside the VPS can reach.
  Ports published from a Docker network would have two problems:
  - they pass by the VPS's firewall (ufw, nftables), which is what decides who can play here;
  - IPv6 players come in through Docker's proxy and all get the same address. The switch allows 8 connections per
    address, so only 8 of them could play.

  If 8080 is taken on the VPS, set `NFSU2_HTTP_PORT` in `.env`: Caddy follows it.
- **Caddy** runs with `compose.yml`'s own Caddyfile, the directives of `web/deploy/Caddyfile` for `NFSU2_HOST`. It:
  - gets and renews the certificate (Let's Encrypt's HTTP or TLS challenge, on 80 or 443);
  - redirects http to https;
  - passes WebSockets (the multiplayer switch) and the client's address (`X-Forwarded-For`) on; the server trusts
    those headers only from this machine;
  - names no software: no `Server` or `Via` header, also not on its redirects and errors (a 502 while the server
    restarts);
  - closes, without an answer, a connection that asks for another name (the bare address, a scanner's made-up
    name), on 80 and on 443; a TLS handshake without your name fails, so the certificate is not shown;
  - opens at most 47900 connections to the server (one per request in progress; with 6000 players the Rust server
    takes 48000, 8 per player; the Node image, at most 2000 players, takes 16000);
  - has its admin API off: nothing reconfigures it while it runs, and a changed `.env` takes `docker compose up -d`.
- **NFSU2_HOST is one name.** For a second name, or a wildcard, edit the Caddyfile in `compose.yml` and the
  server's `NFSU2_HOST` (a comma list; `*.example.com` for one label under it). A wildcard certificate needs Let's
  Encrypt's DNS challenge, which the stock Caddy image cannot do.
- **The server's container** runs as uid 1000, with a read-only root, no capabilities, `no-new-privileges`,
  8 GB of memory without swap and 32768 open files. Those are the systemd unit's limits, as far as a container has
  them, but for the memory: the unit's 3 GB was too little for 110 players (2.6 GB). It writes only to `/state`.
  At the memory limit the kernel stops the server and Docker starts it again.
- **Keep the state volume.** It holds the ETag key (a new key makes every player's browser download the game data
  again), the switch's addresses (`lan-addresses.json`: a player who reconnects gets the same one back), the
  Online mode's records (`online/`), the players' keyphrase saves (`vault/`), your [bans](#kick-and-ban)
  (`bans.json`, `bans.key`) and the kept blocks (`blocks/`: made again when lost, half an hour of two threads).
  `docker compose down` keeps it; `down -v` deletes it.
- **In every mode the server:**
  - answers only `GET` and `HEAD` (and `PUT` and `DELETE` at `/vault`, the keyphrase saves, from its own pages), and
    only for its own names (any other `Host` gets 421);
  - sends HSTS when the page arrived over https for its name (not for `localhost` or an IP address);
  - keeps proxies and CDNs from storing anything (`Cache-Control: private`, `CDN-Cache-Control: no-store`), but
    the game data's runs (`/data-blocks/`) and the module (`/game.wasm`), which a CDN may keep for good when their
    address names the file's version or the module's hash (the Cache Rule in [Cloudflare](#cloudflare));
  - limits what one client can make it hold (`web/test/deploy.test.mjs`, `web/test/audit.test.mjs`). A client is
    an IPv4 address or an IPv6 /56; all clients of one IPv6 /48 (a site) together get twice one client's share:
    - 64 connections at once per client and 128 per site, for TCP peers that are not a trusted proxy (a proxy
      carries many clients);
    - 64 requests in progress per client and 128 per site, also when they come through a proxy or tunnel (more get
      429; the page waits and tries again);
    - the last quarter of its connections and requests only for clients that hold none yet, so the others can never
      lock a newcomer out;
    - a response must move at least 64 KB every 20 s (a client that stopped reading, or reads a trickle, is let go)
      and may take 15 minutes at most;
    - 15 s for a TLS handshake and for the request headers, at most 4 requests queued on one connection;
    - WebSocket pings at 10 a second (a burst of 20); a connection that sends more is closed.

## Port 80 taken

With `compose.443.yml` (`COMPOSE_FILE=compose.yml:compose.443.yml` in `.env`), Caddy uses only port 443, for TCP
and HTTP/3's UDP, and port 80 stays with whatever has it (another API, say), which it never sees.

- **The certificate** comes with Let's Encrypt's TLS challenge, on 443. The HTTP challenge, on 80, is off.
- **No redirect from http.** `http://<your name>` reaches what has port 80, which answers its own way (an API with
  a 404, say). Share the `https://` link: Chrome tries https first for an address typed without it, and once a
  browser has been there, HSTS keeps it on https.
- **Not behind Cloudflare's proxy** (orange cloud): Cloudflare lets neither challenge through to the VPS then. For
  a name on Cloudflare, the [tunnel](#a-cloudflare-tunnel-instead-of-caddy) needs no port at all.
- **Everything else** is as in [How it runs](#how-it-runs): the names, the headers, the closed connections for
  other names (on 443), the server.

## What the log keeps

`docker compose logs server` is the server's console: start-up, players joining (by virtual address, `10.77.x.x`)
and leaving (by virtual address and nickname), refused requests and connections, and errors (method, path and the
server's own stack, never the client's details). Every 5 minutes while anyone is on, a `[players]` line says what
the players on the switch do, each counted once: in Online mode, on the LAN or Online screens, in free roam, on an
older page (one loaded before the page let its place go; it holds the place until its tab closes), and leaving or
starting (the page lets the place go 10 s after the last network feature). It ends with how many players are more
than a second behind on their connection (the server times each one's round trip every second, queues past the
server included: cloudflared's, the tunnel's) and how many of them get fewer of the other cars' updates meanwhile,
with the updates left out since the last line. Many players behind while the server is idle: look at the tunnel
(the cloudflared copies' sites, below). Counts only, no names or addresses.

After each multiplayer race from free roam, every player's game reports it once back in free roam, and the log has a
`[race]` line for each: the race (its lobby, which every player's line of that race shares, the race type, track,
laps and cars), the player's nickname, place (or "did not finish", "quit") and the car as the game rated it (its
type, the game's class and rating, the race's average rating, power, weight, top speed, and the factor its engine
power had in the race). For evening out the cars: `docker compose logs -t server --since 24h | grep '\[race\]'`.

- **No player's real IP address** is written, and the page sends the server nothing about the player but the
  nickname, a random id the browser keeps for [bans](#kick-and-ban), and after a multiplayer race its car and place.
- **`--access-log`** adds a line per request with the client's IP address and the URL. It is off by default, for
  debugging only: add it to the server's `command` in `compose.yml` (`["--behind-proxy", "--access-log"]`), and
  the log then holds your friends' addresses for as long as Docker keeps it.
- **What one client can cause** is limited per minute: 2 lines about its refused requests, 2 about its refused
  `/lan` connections, 20 joins and leaves, 30 lines of Online mode and 3000 access-log lines; then one line saying
  how many were left out. Errors are written in full.
- **Docker keeps the console** in files it rotates (the `local` log driver: 5 files of 20 MB per container), so
  the log never fills the disk.

The console names paths and addresses: it is yours.

## Kick and ban

You can take a player out of playing with others (free roam, Online mode and LAN races) for now, for a while or for
good, with a message their game shows them. Their game goes on, on their own. `web/deploy/nfsu2` runs the commands
in the server's container: copy it next to `compose.yml` once (`scp web/deploy/nfsu2 root@<vps>:/srv/nfsu2/`, then
`chmod +x /srv/nfsu2/nfsu2` on the VPS), and on the VPS:

```sh
cd /srv/nfsu2
./nfsu2 who                              # who is on: address, name, what they do, for how long; and who left in the last hour
./nfsu2 who bob                          # only names with "bob" in them
./nfsu2 kick Bob Stop ramming people     # off now, with that message; reloading the page brings them back
./nfsu2 ban Bob 3d Ramming, 3 days       # off, and turned away for 3 days: 30m, 12h, 3d, 2w, or perm for good
./nfsu2 ban 10.77.4.17 perm              # a player by address, when two have the same name
./nfsu2 bans                             # the bans: number, name, until when, the message
./nfsu2 unban Bob                        # or unban '#4'
```

From your own computer it is one line: `ssh root@<vps> /srv/nfsu2/nfsu2 kick Bob Stop ramming`. The message needs
no quotes, but over ssh an apostrophe needs them twice (`"'Don't ram'"`), or log in first.

- **What the player sees**: a box in the middle of the game, "You were removed from playing with others: Stop
  ramming people." or "You can't play with others until 27 Oct 2026, 15:05: Ramming, 3 days." (in their own time
  and language), and that they can keep playing on their own. Nothing connects them again until they reload (a
  kick) or the ban ends. A page loaded before this version does not know the message: it shows the server as gone
  for a while and keeps trying.
- **What a ban names**: there are no accounts, so a ban names the player two ways, the random id their browser
  keeps for the site and their address (an IPv4 address or an IPv6 /56). A page with either is turned away. The
  address only for a week after it was last the player's: addresses change hands (a phone network gives many people
  one address in turn), so a ban for good would one day hit strangers. The browser's id comes from the page, and
  when it shows up at a new address (a VPN, the phone), that address is banned too. A ban also takes off whoever is on
  the switch from that address at the moment (a household): `ban` lists who it removed. A player who clears the
  site's data and changes networks comes back; no ban can stop that without accounts.
- **Who left**: `ban` takes a player who left in the last hour too (`who` lists them), so you can ban someone who
  is gone already. `kick` needs them on.
- **Banning again** replaces the earlier ban of the same player: `ban Bob perm` after `ban Bob 1d` leaves one ban,
  for good.
- **What is kept**: `bans.json` in the state volume, with keyed hashes of the addresses and ids only (the key,
  `bans.key`, beside it): no address or id can be read back from them. The log has a `[ban]` line for each kick, ban
  and lift, and when a banned player is turned away (2 a minute per client at most).
- **How it works**: the commands go to `control.sock` in the state volume, which only the server's own user and
  root can open; nothing on the network reaches it. Without Docker: `sudo /opt/nfsu2-web/nfsu2-server ctl --state
  /var/lib/nfsu2-web who` (and the same for the others).

## SSH

Port 22 answers on the VPS whatever the game's setup (the tunnel's too), and a password there can be guessed: log
in with a key only.

1. From your computer, with a key (`ssh-keygen -t ed25519` makes one): `ssh-copy-id root@<vps>`, then check that
   `ssh root@<vps>` logs in without asking for a password.
2. In that session on the VPS (keep it open until step 3 works):

   ```sh
   printf 'PasswordAuthentication no\nKbdInteractiveAuthentication no\nPermitRootLogin prohibit-password\n' \
     > /etc/ssh/sshd_config.d/00-keys-only.conf
   sshd -t && systemctl restart ssh        # sessions already open stay
   ```

   The file's `00-` puts it first: sshd takes the first value it reads, and a provider's image often sets
   `PasswordAuthentication yes` in a file read later (`50-cloud-init.conf`).
3. From a second terminal: `ssh root@<vps>` still logs in, and
   `ssh -o PreferredAuthentications=none -o PubkeyAuthentication=no root@<vps>` says `Permission denied
   (publickey)`, no longer `(publickey,password)`.

Locked out anyway: the provider's web console (or its rescue system) still logs in.

## Cloudflare

### Cloudflare's proxy (orange cloud), in front of Caddy

1. **DNS:** a proxied (orange) `A` record for your name, pointing at the VPS, and an `AAAA` record if the VPS has
   IPv6 (Cloudflare may connect to either).
2. **Authenticated Origin Pulls**, before step 3: in Cloudflare's dashboard (SSL/TLS, Origin Server), turn it on.
   Cloudflare's proxy then shows its origin-pull certificate to the VPS. Cloudflare's addresses are not its proxy
   alone: any Cloudflare customer connects from them too (a Worker's TCP socket) and could send any
   `CF-Connecting-IP`, and so pass as thousands of players and lock real ones out of the server's per-address limits
   (the second security audit, 2026-10-22). Turning it on first is safe: Cloudflare shows the certificate only to
   an origin that asks for it.
3. **The client's address:** copy `web/deploy/compose.cloudflare.yml` next to `compose.yml`, add
   `COMPOSE_FILE=compose.yml:compose.cloudflare.yml` to `.env` and run `docker compose up -d`. Caddy then asks every
   connection to your name for Cloudflare's origin-pull certificate (without it the TLS handshake fails, so nothing
   but Cloudflare's proxy reaches the game), believes `CF-Connecting-IP` only from Cloudflare's addresses
   (`compose.cloudflare.yml` lists them, from `web/server-rs/crates/headers/cloudflare-ips.txt` as of 2026-10-18),
   and the server reads it. Anyone else's `CF-Connecting-IP` is replaced by the address that connected. Since
   2026-10-22 `compose.yml` alone believes no proxy: a setup of before (`NFSU2_REAL_IP_HEADER=cf-connecting-ip` in
   `.env`, which can go now) sees every player at Cloudflare's addresses until it takes steps 2 and 3.
4. **Firewall:** let only Cloudflare's addresses reach 80 and 443, or anyone who finds the VPS's address bypasses
   Cloudflare. Instead of step 3 of the setup:

   ```sh
   ufw delete allow 80/tcp && ufw delete allow 443
   for r in $(curl -s https://www.cloudflare.com/ips-v4) $(curl -s https://www.cloudflare.com/ips-v6); do
     ufw allow proto tcp from "$r" to any port 80,443
   done
   ```

5. **SSL/TLS mode: Full (strict).** Cloudflare connects to the VPS over TLS and checks Caddy's certificate.
6. **The certificate.** Caddy gets it over port 80 (the HTTP challenge; the TLS challenge cannot pass Cloudflare's
   proxy), which Cloudflare passes on as long as it does not redirect it to https itself: leave "Always Use HTTPS"
   off (Caddy redirects already). The safe order is to start with the record DNS only (grey cloud) and
   `compose.yml` alone (`compose.cloudflare.yml` lets no browser in without Cloudflare), and turn the proxy on, then
   take step 3, once Caddy has its certificate.
7. **Turn off what rewrites pages:**
   - **Rocket Loader** (Speed, Optimization);
   - **Email Obfuscation** (Scrape Shield);
   - anything else that injects scripts, such as Web Analytics' automatic setup or Zaraz.

   The page's Content-Security-Policy blocks injected scripts, and Rocket Loader would also rewrite the page's own
   module scripts.
8. **Caching:** one Cache Rule, which lets Cloudflare keep the game data and the module (Caching, Cache Rules): URI
   Path wildcard `/data-blocks/*` (the `*` matters: without it only `/data-blocks/` itself matches) or URI Path
   equals `/game.wasm`, Eligible for cache, Edge TTL "Use cache-control header if present, bypass cache if not",
   Browser TTL "Respect origin TTL", the cache key as it is (it must keep the query string). Turn on Smart Tiered
   Caching too. The server then says what Cloudflare may keep, for good: the runs whose address names the file's
   version, compressed or not compressible (the movies too), and the module at the address that names its hash
   (`/game.wasm?v=`, as the page asks for it). A run or the module asked twice answers `cf-cache-status: MISS`,
   then `HIT`. At about 110 players the 1 Gbit uplink was full without it; without the module in the rule, each new
   build is 7 MB from the server for every player. Everything else already says `no-store` to Cloudflare
   (`Cloudflare-CDN-Cache-Control`), so no
   "Bypass cache" rule is needed; delete one from before: of the rules that match, the last one's settings win, and
   a bypass for the whole hostname after this rule turns the game data's cache off.
9. **WebSockets** are on by default on every plan. The server pings each connection every 25 s, well inside
   Cloudflare's idle limit.

Through a real Cloudflare: **UNVERIFIED**. The client's address was checked with a stand-in Cloudflare address, and
the origin-pull certificate with a stand-in CA ([What was tested](#what-was-tested)).

### A Cloudflare Tunnel, instead of Caddy

`cloudflared` on the VPS connects out to Cloudflare, so the VPS opens no port for the game at all (only SSH).
`compose.tunnel.yml` puts `cloudflared` where Caddy was.

1. In Cloudflare's dashboard (Zero Trust, Networks, Tunnels), create a tunnel with the Docker connector and copy
   its token. Give it a public hostname: your name, service `http://127.0.0.1:8080`.
2. Copy `web/deploy/compose.tunnel.yml` next to `compose.yml`, and add to `.env`:

   ```sh
   TUNNEL_TOKEN=<the token>
   COMPOSE_FILE=compose.yml:compose.tunnel.yml
   ```

3. Run `docker compose up -d`. Skip the setup's firewall step for 80 and 443 (`ufw allow 22/tcp && ufw --force
   enable`).
4. Let Cloudflare keep the game data and the module: the Cache Rule of the proxy's step 8 above, and no "Bypass
   cache" rule for the hostname.

What follows from this setup:

- **cloudflared and the server share one network namespace**, on a Docker network, held by a small `pause`
  container (`net`), so either can restart on its own. cloudflared reaches the server on 127.0.0.1:8080; nothing
  else can.
- **The client's address** comes from Cloudflare's `CF-Connecting-IP`, and only from `cloudflared`. It opens a
  connection to the server for each request in progress and has no limit of its own per client: the server's own
  limits per client address hold.
- **Settings:** 7 and 9 of the proxy's list above apply too (8 is step 4 here). Authenticated Origin Pulls is not
  needed: the server is reached through cloudflared alone.
- **Four copies of cloudflared** (`nfsu2-web-cloudflared-1` to `-4`), each with 4 connections to Cloudflare.
  Each player's connection goes over one of them. A connection that went to a far or lossy Cloudflare site slows
  every player on it: on nfsu2.xyz (2026-10-22, 2,950 players) two connections had gone to Amsterdam, and about 600
  players got the other cars seconds late while the server was idle. A copy's sites are in its log:
  `docker logs nfsu2-web-cloudflared-1 2>&1 | grep 'Registered tunnel connection'`. Restart only the copy with the
  bad site (`docker restart nfsu2-web-cloudflared-1`); `docker compose restart cloudflared` drops every player.
- **QUIC, not HTTP/2.** A fifth copy over HTTP/2 (`--protocol http2`, TCP) next to the four, on nfsu2.xyz
  (2026-10-23, ~4,300 players), took 2.58 cores per 1000 of its players against the QUIC copies' 2.20-2.38: no
  cheaper (its players had all come within the hour, still loading, which costs more). Over TCP a lost packet also
  holds up every player on that connection, not only the one it was for.
- **Small receive buffers** in the namespace (`sysctls` on `net`: 128 KiB at most, not Linux's 32 MiB): what a
  player's connection does not take waits at the server, which then sends that player fewer of free roam's
  updates, instead of piling up unread at cloudflared. A change to it recreates `net`, and with it cloudflared and
  the server: every player reconnects.

A real tunnel was **not** opened here (**UNVERIFIED**). The layout was: `cloudflared` stood in by a container that
reaches the server on 127.0.0.1, crashes of either side, a reboot.

### Cloudflare's proxy, straight to the server

No Caddy and no `cloudflared`: Cloudflare's proxy connects to the server itself, which does TLS with a Cloudflare
Origin CA certificate (TLS choice B with C, [Without Docker](#tls-pick-one)). An Origin Rule sends
`https://<your name>` to a high port (28443 here), so the VPS needs nothing on 80 or 443. On nfsu2.xyz the tunnel
took about 3/4 of the CPU (1.72 of 2.25 cores per 1000 players, 2026-10-24); without it the port (0.114 Mbit/s a
player) is the limit, about 7,000-8,000 players on 1 Gbit/s.

1. **Cloudflare, before anything on the VPS:**
   - SSL/TLS, Origin Server, **Create Certificate**: your name and `*.<your name>`, 15 years. Save the certificate
     as `/srv/nfsu2/tls/origin.pem` and the key as `origin.key`, then `chown -R 1000:1000 /srv/nfsu2/tls && chmod
     700 /srv/nfsu2/tls && chmod 600 /srv/nfsu2/tls/origin.key` (the container runs as uid 1000).
   - SSL/TLS, Origin Server, **Authenticated Origin Pulls** on.
   - SSL/TLS, Overview: **Full (strict)**. SSL/TLS, Edge Certificates: **Always Use HTTPS** on (http:// never reaches
     the VPS: the Origin Rule sends every request for the name to the TLS port).
   - Rules, Origin Rules: hostname equals your name, **Destination Port** 28443. Deploy it at step 4 (with the
     tunnel still in front it is not needed).
   - The proxy's settings 7-9 above apply as they are (the Cache Rule stays).
2. **Firewall:** the port from Cloudflare's addresses only (`ufw status` first; with ufw off, `ufw allow 22/tcp`
   before `ufw enable`):

   ```sh
   for r in $(curl -s https://www.cloudflare.com/ips-v4) $(curl -s https://www.cloudflare.com/ips-v6); do
     ufw allow proto tcp from "$r" to any port 28443 comment cloudflare
   done
   ```

3. **Compose:** copy `web/deploy/compose.origin.yml` next to `compose.yml`; in `.env`
   `COMPOSE_FILE=compose.yml:compose.origin.yml` (and `NFSU2_TLS_DIR=/srv/nfsu2/tls`). Keep `TUNNEL_TOKEN`: it is the
   way back.
4. **The switch** (everyone reconnects once, as at a release): `docker compose up -d --remove-orphans` (removes
   `cloudflared` and `net`), then in Cloudflare's DNS delete the tunnel's record for your name (CNAME to
   `<id>.cfargotunnel.com`) and add a proxied (orange) `A` record to the VPS's IPv4, and deploy the Origin Rule. In
   Zero Trust, delete the tunnel's public hostname (the tunnel itself can stay). Leave out `AAAA` unless the firewall
   step covered IPv6 and you want it.
5. **Check:** the log says `HTTPS : port 28443` and `cloudflare: its addresses must show its origin-pull
   certificate`; the page loads; `curl -kv --max-time 5 https://<vps>:28443/` from outside Cloudflare gets no
   answer (the firewall); players' `[roam]` rtt as before or lower.

**Back to the tunnel:** `COMPOSE_FILE=compose.yml:compose.tunnel.yml`, `docker compose up -d --remove-orphans`,
turn the Origin Rule off, delete the `A` record and add the tunnel's public hostname again (that makes its CNAME).

What the handshake lets through: an address outside Cloudflare's ranges never gets past the firewall; one inside
must show the origin-pull certificate, and every handshake must ask for a name in the certificate (none, or another
name: no certificate, no answer). Authenticated Origin Pulls' Cloudflare certificate is the same for every
Cloudflare account, so it proves Cloudflare's proxy, not your zone: another account's proxy would also need to send
your name as its SNI, which only Enterprise plans can set, and Cloudflare sets `CF-Connecting-IP` there itself.
A certificate of your own (zone-level Authenticated Origin Pulls) would close that too, but the server trusts
Cloudflare's CA only (built in).

Checked on a Mac (2026-10-24, the arguments of `compose.origin.yml`, a self-signed certificate): the health
check's 127.0.0.1 port and HTTPS for the name answer, the bare address and another name get no handshake. Through a
real Cloudflare: **UNVERIFIED**.

## A proxy you already have

With Caddy or nginx already on the VPS, run the server alone on the VPS's network and forward your name to
127.0.0.1:8080, as `web/deploy/Caddyfile` or the nginx block in [Without Docker](#without-docker) do:

```sh
docker run -d --name nfsu2-web --restart unless-stopped --network host \
  --read-only --cap-drop ALL --security-opt no-new-privileges \
  --memory 8g --memory-swap 8g --ulimit nofile=65536 --log-driver local \
  -e NFSU2_HOST=game.example.com \
  -v /srv/nfsu2/game:/game:ro -v /srv/nfsu2/module:/module:ro -v nfsu2-state:/state \
  ghcr.io/th0mk/nfsu2-web-rs
```

The proxy must keep `Host`, pass WebSocket upgrades on, set `X-Forwarded-For` and `X-Forwarded-Proto`, and wait
longer than the 25 s ping interval for an answer. The image's command is the server's options, `--behind-proxy` by
default. Options with a value can also be set as `NFSU2_<OPTION>` (`web/server-rs/src/options.rs`).

## Multiplayer over the internet

The server's switch relays every packet: player → VPS → player. The delay between two players is the sum of both
paths to the VPS, so pick a VPS location between you.

- **Free roam's hub** (`web/server-rs/crates/roam`, on 10.77.0.253 of the switch): every second it tells each player
  in free roam which players are nearest (32 at most, in the same Room; 7 for a game from before this change); the
  players then send their cars only to those, straight through the switch, and the game shows all of them. For that
  the hub reads where each player's car is; it keeps nothing on disk. `--roam-hub off` in the server's `command`:
  no hub, players find each other by broadcast in groups of 33, the first to meet.
  - **Traffic:** a player sends each linked player 20 snapshots a second (about 1.9 KB/s each way), 5 a second past
    300 m and 2 past 1 km. 32 players in one spot: about 60 KB/s each way per player, and the switch relays about
    2 MB/s (measured with 32 `web/tools/roam-crowd.mjs` players: 22,000 datagrams a second each way, 10 % of one core
    for the server).
  - **Bundles:** what the page sends at once (a snapshot to each friend, the same bytes) goes as one WebSocket
    message with the friends' addresses, and the switch sends each page what came for it in one turn as one message
    (`web/server-rs/crates/lan`, frame kind 32; a page or switch of before keeps one message a datagram, and both
    sides still work with it). The server and the tunnel pay per message, not per byte: on a live server with about
    600 players in free roam the Node server it replaced was at 100 % of its one thread, and its switch took 0.1-0.7 s
    to answer a ping. Measured with
    120 `roam-crowd.mjs` players in one spot (a Ryzen 9 5900X): one message a datagram took the server past one core
    (20,000 datagrams a second in, pings answered after 11 s, links falling apart); bundled, 44,000 datagrams a
    second in took 74 % of a core, pings 2.7 ms (p50), 7.5 ms (p90). Only a page loaded after the server has them
    sends and takes bundles.
  - **Its own cost:** in a Room of more than 256 players the hub finds each player's nearest through a grid of the
    map instead of weighing every pair, with the same links. Measured on a Ryzen 9 5900X: 1000 players in one Room
    took about 30 ms a second (every pair took 0.5 s), 2000 about 70 ms, spread over the city or all in one spot.
  - **A busy server gives every player fewer links:** at each tick the hub looks at how busy the server's workers
    were, and at the share of the CPU quota the game's containers used together (nfsu2.slice, where the server sees
    it), the higher of the two. Over 85 %, the most links a player may have goes down by a quarter (32, 24, 18, 13, 9, 8 at the least), at most
    every 5 s; under 65 %, it goes back up by 2, at most every 15 s. The nearest links stay. The console says each
    change (`[roam] the server's thread was 93 % busy: at most 24 links a player (was 32)`).
- **Free roam** draws another player's car where it is now (ghost mode off, the default: the newest snapshot carried
  on by half the round trip), or with Ghost mode on `roam_delay` milliseconds (100 by default) behind its newest
  snapshot. That delay hides the 50 ms between snapshots, jitter and the odd lost snapshot. Network delay comes on
  top of it.
  - **Measured with impairment on a local switch:** 100 ± 10 ms one way and 5 % loss.
    98.1 % of frames were interpolated, 1.9 % extrapolated, and none were held.
  - **For 50–150 ms round trips, the default is enough.** If friends' cars stutter (Wi-Fi or mobile jitter), set
    `roam_delay = 150` in settings.ini (the text editor at the bottom of Settings; the page has no row for it). It
    only adds display delay.
  - **There is no automatic raise from the measured ping.** The runtime reads `roam_delay` once at start; making it
    follow the ping is a runtime change for the net track.
  - **Over the real internet:** **UNVERIFIED**.
- **Traffic:** free roam sends 20 snapshots a second of about 95 bytes to each friend, about 1.9 KB/s per friend
  in each direction (7 friends: about 13 KB/s). A snapshot carries only the newest position, because the
  WebSocket loses none; on a real LAN (the Mac, Windows) it carries the last three, about 180 bytes.
- **Keepalive:** the switch pings every connection every 25 s. Proxies and Cloudflare then never cut a quiet player,
  and the switch finds players whose connection died.
- **Limits:**
  - 6000 players on the switch with the Rust image (`NFSU2_MAX_PEERS=6000`; the server's own default is 128, and
    6000 is the most addresses it hands out on its virtual network, 10.77.0.0/19: 250 in each of 10.77.0-23.x), 2000 with
    the Node image (its network is 10.77.0.0/21);
  - when every place is taken a game waits in line for one, first come first served (nfsu2-lan `queue.rs`): the
    line's first takes the next place that frees up, and while anyone waits a newcomer waits too. The game shows
    its place in line and, once a few went in, about how long it takes at the pace of the last ten minutes; the
    start page shows how many wait, and at Play offers to wait or play offline. A waiting page whose connection
    drops keeps its place for a minute (a ticket only it has), as a dropped player keeps its address; one that
    lets go (no network feature in use, away, "Stop waiting") leaves the line. At most 5000 wait. A page from
    before the line gets "LAN full" instead, tries again every 5 s and gets in only once nobody waits;
  - a player holds a place only while a network feature needs it: free roam while it is on, LAN play and Online
    from the game's LAN or Online screens until it leaves them. 10 s after the last one ends the page closes its
    connection and the switch frees the address at once; going back into those screens connects again (a new
    address). Single-player alone never connects;
  - LAN play's broadcasts (the game's search for LAN games) reach only the players who have the LAN or Online
    screens open, not everyone on the switch;
  - 8 connections at once per client address and 16 per IPv6 /48; opening them is rate limited per client
    address (a burst of 20, then one a second), so a client that opens and closes connections as fast as it can
    gets 429;
  - frames up to 8 KB; 2500 frames and 256 KB a second per connection, 5000 and 512 KB for all of one client
    address's connections together, 10000 and 1 MB for all of one IPv6 /48 (a 3-player LAN race's host sent at
    most 100 frames and 7.7 KB a second; free roam is 1.9 KB a second per friend, and 32 friends at 60 snapshots
    a second are about 2000 frames). A broadcast counts once for every player it reaches, and a frame in a bundle
    (64 KB at most) once for every address it is for;
  - datagrams to a player whose connection is 256 KB behind are dropped, as a full link would; free roam's already
    at 64 KB (about a second of them), so a player whose link falls behind gets the newest positions as soon as it
    catches up, not seconds of old ones first. LAN-race stream data is never dropped. A player with more than 2 MB of it waiting, or more than 1 MB of it from one client, is
    disconnected if it stopped reading (nothing went out for a second while data waited); otherwise the client
    that queued the most for it is, all its connections (so a flood does not get a player who reads cut; only many
    clients that together outrun that player's link for a long time could). Only a player that stops reading
    altogether (8 MB behind) is disconnected for that;
  - a player who drops (a lost connection, not the page letting go) gets the same address back when it reconnects:
    the switch keeps the address a minute for that player alone (a token only that browser has), also across a restart of the server, and never gives it to
    anyone else. At most 8 such addresses are kept per client address (16 per /48; past that its own oldest goes;
    the ones kept across a restart do not count). A kept address holds its place, so when every place is held or
    kept a newcomer waits in line until one frees.
- **LAN races** (the game's own LAN mode, from the LAN-play track) use the same switch. Over the internet they are
  **UNVERIFIED**.
- **The game's Online mode** is served by the server itself, at the switch's reserved address 10.77.0.1 (on by
  default; `--online off` in the server's `command` turns it off, and the game then cannot connect). Any name of 3
  to 15 letters, digits, `-` and `_` logs in; there are no accounts. One player per name, while online and while
  racing: a name in use is given with four digits in place of its end (on a busy server many players come as
  `NAME`), and the game plays under that.
  - **Kept in the state volume:** `online/stats.json` (race results, best times and records per persona) and
    `online/buddies.json` (EA Messenger: each persona's friends and blocked by name, its friend requests waiting for
    an answer, and the messages from friends waiting for it while it is away, text included, 7 days at most), each
    with the month a persona was last seen, written atomically (0600). When the server stops, `online/races.json`
    keeps the races going on (each one's players by persona and virtual address, and the reports so far), read back
    at the next start and removed once it is up: the players race on through a restart (the switch gives their
    addresses back) and their reports afterwards still count, as long as their pages are back within about 10 s (the
    game ends a race whose network was gone that long). No passwords, CD keys, addresses, and no chat
    but those waiting messages. The server never reads an account's
    password; a game's password it checks against an HMAC kept in memory with that game, under a key of the game's
    own.
  - **The console** names online players by persona and virtual address (10.77.x.x) only, and shows at most 30
    Online lines a minute caused by one client address (then one line saying how many were left out).
  - **Who plays now:** `/players` answers anyone who reaches the server with three numbers for the start page,
    whether it is full, how many wait and whether free roam is busy:
    `{"players":3,"online":1,"roam":2,"full":false,"queue":0,"roamBusy":false}`, the games on the switch, how many
    of them are in Online mode (logged in, or in an online race; `null` with `--online off`), how many drive in free
    roam or are paused there (`null` with `--roam-hub off`), whether a game that connects now waits in line, how
    many wait in it, and
    whether the free-roam hub gives fewer than 24 links a player (its thread is busy; the start page doesn't ask
    its free-roam question then). Nothing else: no names, addresses or times, never cached. The game's own lobby
    shows its players to anyone who logs in anyway.
  - **Limits:** 2000 lobby sessions (the Node server: 1000), 3 per browser, 8 per client address, 32 per IPv6 /48,
    and the last quarter only for a client that holds none; 600 games (Node: 300) of at most 6, 2 open and 2
    started per browser (4 and 8 per client address), one browser one player of a race; 20 lobby messages a second per session (a flood ends the session);
    games created one every 2 s (a burst of 3), games joined, rooms entered and car descriptions published one a
    second (a burst of 4; a car description over that is not passed on); chat 1 a second with a burst of 5 (then
    10 s muted), 255 characters; a silent session goes after 120 s; a race whose players' browsers left the switch
    ends after 2 minutes. EA Messenger: 10 messages a second per connection (a flood closes it), the lists and the
    search 1 a second, a presence at most once a second; 50 friends and 50 blocked each, 20 friend requests waiting
    from one persona (30 days), 20 messages waiting for one (the oldest goes) and 20,000 in all (7 days), 5000
    persona lists (the least recently seen go), 20 new ones an hour per client address. Results: 100,000 personas
    (the least recently seen go), a
    race counts once two of its players who finished it, in different browsers, sent the same result.
  - **What one client costs the others:** what the online host sends because of a client's message (its answer,
    and what the others hear of it: room chat, a game's players) comes out of that client's switch budget; over
    it, the connection that message came on closes, and the client must pay the overspend back before its next
    message gets through. A game's record goes only to players looking at the game list, at most once a second; a
    room's population at most once a second. A player too far behind (1 MB queued) loses its online connection,
    not its place on the switch.
  - **Measured on the Mac** (`web/tools/online-load.mjs`): 200 simulated players logging in, chatting and racing
    in groups of four for 90 s took the server to 230 MB and 14.7 s of CPU; chat arrived in 1.1 ms (p50).
  - Race traffic goes from browser to browser through the switch, as LAN races do (**UNVERIFIED** over the
    internet).

## What was tested

- **2026-10-19, Docker 29.9 (a Docker-in-Docker container on Docker Desktop for Windows as the VPS), compose 5.6,
  Caddy 2.11.7, the image built from the checkout (amd64), the real game folder** and a stand-in module:
  - the setup's steps as written, with `scp` and rsync 3.5.0 on both ends: the game copied in 25 s, owned by root
    and readable by everyone;
  - the certificate, with Pebble (Let's Encrypt's test server) instead of Let's Encrypt: obtained over the HTTP
    challenge, and in a second run over the TLS challenge alone;
  - through Caddy: the headers (HSTS, isolation, CSP, `no-store`, no `Server` or `Via`), the redirect to https (no
    `Server`), no answer for the bare address or another name on 80 and 443, no TLS handshake without the name,
    a 502 without `Server` while the server is stopped, the `/lan` WebSocket (101), `/players`, a 404 for a file
    outside the game's folders;
  - the client's address the server sees, with `X-Forwarded-For` and `CF-Connecting-IP` faked by the client: its
    own address by default and with `NFSU2_REAL_IP_HEADER=cf-connecting-ip`, and Cloudflare's `CF-Connecting-IP`
    only from a stand-in address in Cloudflare's range;
  - the ETag key kept when the server's container is recreated; a read-only root, no capabilities, 3 GB, 8192 open
    files, user `node`; `healthy`, `health: starting` then `unhealthy` without a module, the restart loop without a
    game folder; both containers back after a reboot of the VPS;
  - the tunnel's layout, `cloudflared` stood in by a container on 127.0.0.1: crashes of either side, a reboot; the
    real `cloudflared` image with an invalid token (the server keeps running);
  - port 80 taken by a stand-in API that Docker publishes on 80 (`0.0.0.0:80->5282`): `compose.yml` alone keeps
    Caddy restarting (`address already in use`); with `compose.443.yml`, Caddy on 443 only, the certificate over
    the TLS challenge, the page, the WebSocket and a 502 as above, the API still answering on 80, and all three
    back after a reboot;
  - the `docker run` line of [A proxy you already have](#a-proxy-you-already-have); the `ufw` lines in Ubuntu 24.04.
- **2026-10-19 on the Mac**, Docker 29.5 (Colima, an arm64 Linux VM), compose 5.6, Caddy 2.11.7, with the earlier
  `compose.yml` (its Caddyfile had only the proxy and `header -Server`):
  `node web/tools/deploy-e2e.mjs --mode docker --menu`, Caddy with a test certificate. In Chrome: the headers, a
  secure and cross-origin isolated page, the start page, Play, then the main menu with the renderer, and no CSP
  violation. Two runs of three reached the main menu, in 71 and 81 s; the third was still at the profile prompt when
  the test's two minutes ran out (the same test without Docker reached it in 42 s: every file went through Colima's
  VM). The amd64 image built on the arm64 Mac without emulation, and ran.
- **2026-10-22, Docker 29.4 on Windows, Caddy 2.11:** Authenticated Origin Pulls with a stand-in CA in place of
  Cloudflare's. `compose.cloudflare.yml`'s Caddyfile (as `docker compose config` renders it, for `localhost`): no
  answer for a connection without a client certificate or with one another CA signed, an answer for one the CA
  signed; a `CF-Connecting-IP` the client sent replaced by its own address, there and with `compose.yml` alone; the
  `Caddyfile`'s Cloudflare variant uncommented passes `caddy validate`. The server's own (`--trusted-proxies
  cloudflare`): the same three cases in `crates/http/tests/tls.rs`, and Cloudflare's CA taken by rustls.
- **Not tested:** a real VPS, real Let's Encrypt, a real domain, Cloudflare (proxy and tunnel), the game in Chrome
  through the current `compose.yml` (no module on the Windows PC), more than one player at once (**UNVERIFIED**).

## Without Docker

The same server from a bundle, as a systemd service, with Caddy installed on the VPS. The behaviour, the limits and
the firewall are the same as above.

**You need** nothing installed on the VPS for the server itself (it is one static binary), and the bundle, built on
the Mac or any machine with Docker:

```sh
web/build.sh                      # the game module, if out/web/nfsu2.wasm is not there yet
web/deploy/pack.sh                # -> out/web/deploy/nfsu2-web-<rev>-amd64.tar.gz (about 12 MB)
web/deploy/pack.sh --arch arm64   # for an Arm VPS
```

- **What is in it:** the server (`nfsu2-server`, a static binary for Linux on that processor, built in Docker by
  `web/server-rs/Dockerfile`'s `binary` stage, as the image's: the first build takes a few minutes; `--server <file>`
  takes one you have), the page without its test pages, the game module, a systemd unit, example Caddy and
  cloudflared configurations, and a short README.
- **What is never in it: the game.** `pack.sh` refuses to build the bundle if any file in it has a game file's
  name, or the name and size of a file in your game folder. It also refuses if a file names this Mac (host name,
  home folder).

**Install:**

```sh
scp out/web/deploy/nfsu2-web-<rev>-amd64.tar.gz root@<vps>:
ssh root@<vps> 'mkdir -p /opt /srv/nfsu2 && tar -C /opt -xzf nfsu2-web-<rev>-amd64.tar.gz'   # -> /opt/nfsu2-web
```

Then copy the game to `/srv/nfsu2/game/` as in step 2 of the setup (the module is in the bundle).

### TLS: pick one

Chrome runs the game only on a secure page, so TLS is required. Each choice sets the server's command line in
`/opt/nfsu2-web/deploy/nfsu2-web.service`.

| | TLS done by | Server command line | Open on the VPS |
|---|---|---|---|
| **A** | Caddy, with Let's Encrypt (recommended) | `--behind-proxy --http-port 8080 --host game.example.com` (plus `--real-ip-header cf-connecting-ip` with Cloudflare's proxy in front, C) | 80, 443 |
| **B** | the server itself, with Let's Encrypt or a Cloudflare Origin CA certificate | `--port 443 --http-port 0 --tls-cert … --tls-key …` | 443 (and 80 while certbot renews) |
| **C** | Cloudflare's proxy (orange cloud), then A or B on the VPS | as A or B, plus a Cloudflare option (below) | 80 and 443, from Cloudflare's addresses only |
| **D** | a Cloudflare Tunnel (`cloudflared` on the VPS) | `--behind-proxy --http-port 8080 --host game.example.com --real-ip-header cf-connecting-ip` | none |

`--host` is the name players open (a comma list for more; `*.example.com` for one label under it). Behind a proxy
the server cannot know it and refuses to start without it. With a certificate (B) the names come from the
certificate.

**A: Caddy.** Install Caddy (caddyserver.com, "Install"), copy `deploy/Caddyfile` to `/etc/caddy/Caddyfile`,
replace `game.example.com` with your name everywhere in it (and in the unit's `--host`), and run
`systemctl reload caddy`. It does what Caddy does in Docker ([How it runs](#how-it-runs)).

**B: the server does TLS itself.**

- **Let's Encrypt:** get the certificate with certbot, and let it restart the server after each renewal:
  `certbot certonly --standalone -d game.example.com --deploy-hook "systemctl restart nfsu2-web"`. In the unit, use
  variant B: systemd hands the certificate files to the service (`LoadCredential`), so they stay readable by root
  only, and `AmbientCapabilities=CAP_NET_BIND_SERVICE` lets the server bind port 443.
- **A Cloudflare Origin CA certificate** (only with the orange cloud, C): create one under SSL/TLS, Origin Server,
  save the certificate and the key on the VPS and point `LoadCredential` at them.
- Outside systemd credentials, the server reloads its certificate on `SIGHUP`, and by itself within a minute of the
  file changing.
- A TLS handshake succeeds only for a name in the certificate: a connection to the bare address without a name (an
  internet-wide scanner) or with another name gets no certificate (a certificate for an IP address is shown
  without a name too).
- **IPv4 and IPv6:** the server listens on `::`, so an `A` and an `AAAA` record both work; on a VPS without IPv6
  it falls back to IPv4 by itself, and then only an `A` record belongs in DNS. `--bind 0.0.0.0` keeps it to IPv4.
  With Caddy (A) or a tunnel (D), the server listens on 127.0.0.1 only.

**C: Cloudflare's proxy.** As in [Cloudflare](#cloudflare), Authenticated Origin Pulls first (its step 2), and:

- with Caddy (A): the `Caddyfile`'s Cloudflare variant, with its global block at the top (Caddy then believes
  `CF-Connecting-IP` only from Cloudflare's addresses), a Cloudflare Origin CA certificate, and
  `deploy/cloudflare-origin-pull-ca.pem` copied to `/etc/caddy/` (the variant asks every connection for
  Cloudflare's origin-pull certificate); the server needs `--real-ip-header cf-connecting-ip`;
- with the server itself (B): the Origin CA certificate, plus `--trusted-proxies cloudflare`. Then only connections
  from Cloudflare's published ranges (`web/server-rs/crates/headers/cloudflare-ips.txt`) may say who the client is,
  and only with Cloudflare's origin-pull certificate: the server asks those addresses for it (its CA is built in,
  `web/server-rs/crates/headers/cloudflare-origin-pull-ca.pem`), and their handshakes fail without it. It says so
  on start (`cloudflare: its addresses must show its origin-pull certificate`).

**D: a Cloudflare Tunnel.** `cloudflared tunnel login`, `cloudflared tunnel create nfsu2`,
`cloudflared tunnel route dns nfsu2 game.example.com`; copy `deploy/cloudflared.yml` to
`/etc/cloudflared/config.yml` and fill in the tunnel id; `cloudflared service install`. The unit's variant D.
`cloudflared tunnel ingress validate` accepts the example (cloudflared 2026.9.3); a real tunnel is **UNVERIFIED**.

**nginx instead of Caddy** (with `--host game.example.com`; this configuration was not run here, **UNVERIFIED**):

```nginx
map $http_upgrade $connection_upgrade { default upgrade; '' close; }
server {
    listen 443 ssl; server_name game.example.com;   # + ssl_certificate / ssl_certificate_key
    server_tokens off;
    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection $connection_upgrade;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_read_timeout 120s;
        proxy_buffering off;
    }
}
```

### The service

```sh
cp /opt/nfsu2-web/deploy/nfsu2-web.service /etc/systemd/system/
# pick the ExecStart for your TLS choice (A as shipped; D or B: the commented lines), and put your name in --host
systemctl daemon-reload && systemctl enable --now nfsu2-web
journalctl -u nfsu2-web -f
```

The unit runs the server as a fresh unprivileged user (`DynamicUser`):

- the system, `/opt/nfsu2-web` and the game folder are read-only to it;
- it may write only in its state folder `/var/lib/nfsu2-web` and its own `/tmp`;
- `NoNewPrivileges` leaves it no capabilities, except `CAP_NET_BIND_SERVICE` in variant B, for port 443;
- it may use at most 3 GB of memory (`MemoryMax`, reclaimed from above 2 GB), so whatever a client does, the rest
  of the VPS keeps its memory: at the limit the kernel stops the server and systemd starts it again;
- the journal takes at most 2000 of its lines per 30 s (`LogRateLimitBurst`), so it keeps the rest of the VPS's
  logs whatever a client does.

The unit was run under systemd 257 (Debian trixie, with the Node server of then) in a container, variants A, D and
B: it starts, stays up (no restarts) and serves. With the Rust server's bundle (2026-10-21): `pack.sh` built it in
Docker, and the unpacked server, run as the unit runs it (`NFSU2_REPO=/opt/nfsu2-web`), served the page, the module
and the game's files, and no test page; the unit itself under systemd with it: **UNVERIFIED**. `LoadCredential` could not be exercised in a container (**UNVERIFIED**; B ran with the
certificate by path). Without `--tls-cert` or `--behind-proxy` the server listens on 127.0.0.1 only and says so on
start: nothing outside the VPS can reach it.

**Updating:** build a new bundle on the Mac, unpack it over `/opt/nfsu2-web`, and run
`systemctl restart nfsu2-web`.
