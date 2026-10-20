# The game server on a VPS

Status: **2026-10-19**, with Docker (the server's image and Caddy from one compose file). Without Docker (the
bundle and a systemd unit): [the last section](#without-docker).

Use a VPS when other PCs should play: on the Mac at home the server serves only that Mac (`http://localhost`). The
VPS runs the same server (`web/server/server.mjs`) in a container, with Caddy in front for HTTPS, so friends
anywhere open `https://<your name>/` in Chrome and play from there.

## Setup

**What you need:**

- **A Linux VPS** (Debian or Ubuntu, say) that you reach as root over SSH. 2 vCPU and 4 GB of memory are plenty for
  friends; see [Sizing](#sizing) for more players.
- **A domain name** (or a subdomain) with an `A` record pointing at the VPS, and an `AAAA` record if it has IPv6.
- **Your own NFS Underground 2 folder** (the one with `SPEED2.EXE`).
- **The game module**, built on the Mac with `web/build.sh` (`out/web/nfsu2.wasm` and `out/web/nfsu2.json`).
- **The image, public:** `ghcr.io/th0mk/nfsu2-web` is private, like the repository, so the VPS cannot pull it.
  Make it public once: on GitHub, the package's settings, "Change visibility". It holds the server and the page,
  never the game. To keep it private instead: [The image](#the-image).

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

The game takes about 3 GB. Why these lines: [Copying the game](#copying-the-game).

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

**When something is wrong:**

| What you see | Why |
|---|---|
| `docker compose up` says `set NFSU2_HOST in .env` | `/srv/nfsu2/.env` is missing, or not next to `compose.yml` |
| pulling the image says `unauthorized` | the image is private: make it public, or [log in](#the-image) |
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
  `speech`, `SUBTITLES`, `TRACKS`), compared without case, and everything in those folders except dot files.
  Nothing else at the top of the folder: never an installer's log (`unins000.dat` names the PC, the Windows account
  and the install time), a `SAVE` folder, mod plugins and their settings, or anything else you keep there.
- **What it never shows:** anything about the VPS or the other players (`web/test/deploy.test.mjs` checks what is
  and is not exposed).

## Sizing

For up to 2000 players at once (the image's limit): 8 vCPU, 8 GB of memory (the compose file's limit), 40 GB of
disk and a 1 Gbit/s port with a few TB of traffic a month.

- Measured on a live server behind a Cloudflare Tunnel (16 vCPU, 2026-10-20): 636 players, 303 of them in the
  game's Online mode, took the server 0.6 of a core and 200 MB, and cloudflared about 2 cores. 2000 players at once
  are **UNVERIFIED**.

- The server relays and serves files. It used about 110 MB of memory here, plus up to 10 MB queued towards each
  player who falls behind (8 MB on the connection, 2 MB more of LAN-race data in the switch's own queue).
- The game folder takes about 3 GB of disk.
- Each new player downloads the 7 MB module (again after each new build; with the Cache Rule below from Cloudflare,
  which takes it from the server once) and then the game data they play (about 70 MB for a quick race, up to
  the whole 3 GB over a career), once: the browser keeps it. The data is compressed on the way (zstd, or br through
  Cloudflare): all but the movies is 1.37 GB on the network for 1.98 GB, the tracks about 40 %; the movies' video
  does not compress and goes as it is. "Preload the game data" on the start page fetches all but the movies at
  once, 4 requests at a time, so a player on a slow line never waits on a loading screen. It asks for one block a
  request at the addresses the game's own reads use, so with the Cache Rule below Cloudflare serves both.
- Compressing costs the server about 5 s of one core per player for that whole download (zstd; br about 1.5 times
  that). At most 128 answers are compressed at once (8 per player, about 200 MB of memory); past that they go as
  they are, never held up. The image gives Node 8 helper threads for the compressing and the file reads
  (`UV_THREADPOOL_SIZE`, 4 by default).
- Free roam is at most about 25 KB/s each way per player: the server's free-roam hub pairs each player with the 7
  nearest, and snapshots go between those only. 2000 players roaming at once are about 400 Mbit/s each way through
  the VPS. Measured with 62 simulated players and 2 real pages on one server (`web/tools/roam-crowd.mjs`, a Ryzen 9
  5900X): about 9,500 datagrams a second each way through the switch, the server at 11 % of one core. More players
  than that, and a real VPS's CPU, are **UNVERIFIED**.

## The image

- **What it holds:** the server, its one dependency (`ws`) and the page: about 62 MB to download, on Node.js 24
  (Alpine). **Never the game**, or anything made from it: the build sees only the server's and the page's sources
  (`.dockerignore`, checked by `web/test/docker.test.mjs`), so a public image cannot carry the game. What it needs
  is mounted when it runs:

  | In the container | What | On the VPS (default) | |
  |---|---|---|---|
  | `/game` | your game folder | `/srv/nfsu2/game` | read-only |
  | `/module` | `nfsu2.wasm` and `nfsu2.json` | `/srv/nfsu2/module` | read-only |
  | `/state` | the server's keys and the Online mode's records | the volume `nfsu2-web_state` | |

- **From GitHub's registry:** `.github/workflows/image.yml` builds `ghcr.io/th0mk/nfsu2-web` for amd64 and arm64
  on every push: `latest` from `main`, `sha-<commit>` from every push, and `<version>` and `<major>.<minor>` from a
  `v*` tag. A package is private while its repository is, and then the VPS needs one of:
  - **public:** the package's settings on GitHub, "Change visibility";
  - **a login:** a GitHub token that may read packages (a classic personal access token with `read:packages`),
    once on the VPS: `docker login ghcr.io -u th0mk`, with the token as the password. Docker keeps it in
    `/root/.docker/config.json`;
  - **built yourself** (below).
- **Built yourself**, with no registry at all: on any machine with Docker, in the checkout, then into the VPS's
  Docker over SSH.

  ```sh
  docker build --platform linux/amd64 -t nfsu2-web .         # linux/arm64 for an Arm VPS
  docker save nfsu2-web | ssh root@<vps> docker load
  ssh root@<vps> 'echo NFSU2_IMAGE=nfsu2-web >> /srv/nfsu2/.env'
  ```

  It needs BuildKit (the buildx plugin; Docker Desktop has it). Building for the other architecture needs no
  emulation: the image's own stage only copies. To update, build and load again, then `docker compose up -d`
  (without `pull`: there is nothing to pull).
- **The module must come from the image's commit.** The page in the image and the module meet at the module's
  imports, and nothing checks that the two come from the same revision. `nfsu2.json`'s `rev` (the short commit,
  with `+` if the checkout had changes) and the image's tag `sha-<commit>` should name the same commit. On the VPS:

  ```sh
  grep -o '"rev":"[^"]*"' /srv/nfsu2/module/nfsu2.json
  docker image inspect --format '{{index .Config.Labels "org.opencontainers.image.revision"}}' ghcr.io/th0mk/nfsu2-web
  ```

  A self-built image has no revision label: build it and the module from the same checkout.

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
  - opens at most 15900 connections to the server (one per request in progress; with 2000 players the server takes
    16000);
  - has its admin API off: nothing reconfigures it while it runs, and a changed `.env` takes `docker compose up -d`.
- **NFSU2_HOST is one name.** For a second name, or a wildcard, edit the Caddyfile in `compose.yml` and the
  server's `NFSU2_HOST` (a comma list; `*.example.com` for one label under it). A wildcard certificate needs Let's
  Encrypt's DNS challenge, which the stock Caddy image cannot do.
- **The server's container** runs as the user `node`, with a read-only root, no capabilities, `no-new-privileges`,
  8 GB of memory without swap and 8192 open files. Those are the systemd unit's limits, as far as a container has
  them, but for the memory: the unit's 3 GB was too little for 110 players (2.6 GB). It writes only to `/state`.
  At the memory limit the kernel stops the server and Docker starts it again.
- **Keep the state volume.** It holds the ETag key (a new key makes every player's browser download the game data
  again), the switch's addresses (`lan-addresses.json`: a player who reconnects gets the same one back) and the
  Online mode's records (`online/`). `docker compose down` keeps it; `down -v` deletes it.
- **In every mode the server:**
  - answers only `GET` and `HEAD`, and only for its own names (any other `Host` gets 421);
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
starting (the page lets the place go 10 s after the last network feature). Counts only, no names or addresses.

- **No player's real IP address** is written, and the page sends the server nothing about the player but the
  nickname.
- **`--access-log`** adds a line per request with the client's IP address and the URL. It is off by default, for
  debugging only: add it to the server's `command` in `compose.yml` (`["--behind-proxy", "--access-log"]`), and
  the log then holds your friends' addresses for as long as Docker keeps it.
- **What one client can cause** is limited per minute: 2 lines about its refused requests, 2 about its refused
  `/lan` connections, 20 joins and leaves, 30 lines of Online mode and 3000 access-log lines; then one line saying
  how many were left out. Errors are written in full.
- **Docker keeps the console** in files it rotates (the `local` log driver: 5 files of 20 MB per container), so
  the log never fills the disk.

The console names paths and addresses: it is yours.

## Cloudflare

### Cloudflare's proxy (orange cloud), in front of Caddy

1. **DNS:** a proxied (orange) `A` record for your name, pointing at the VPS, and an `AAAA` record if the VPS has
   IPv6 (Cloudflare may connect to either).
2. **The client's address:** add `NFSU2_REAL_IP_HEADER=cf-connecting-ip` to `.env` and run `docker compose up -d`.
   Caddy believes `CF-Connecting-IP` only from Cloudflare's addresses (`compose.yml` lists them, from
   `web/server/cloudflare-ips.txt` as of 2026-10-18) and overwrites it otherwise, so a client that reaches the VPS
   directly cannot choose the address the server logs and limits by.
3. **Firewall:** let only Cloudflare's addresses reach 80 and 443, or anyone who finds the VPS's address bypasses
   Cloudflare. Instead of step 3 of the setup:

   ```sh
   ufw delete allow 80/tcp && ufw delete allow 443
   for r in $(curl -s https://www.cloudflare.com/ips-v4) $(curl -s https://www.cloudflare.com/ips-v6); do
     ufw allow proto tcp from "$r" to any port 80,443
   done
   ```

4. **SSL/TLS mode: Full (strict).** Cloudflare connects to the VPS over TLS and checks Caddy's certificate.
5. **The certificate.** Caddy gets it over port 80 (the HTTP challenge; the TLS challenge cannot pass Cloudflare's
   proxy), which Cloudflare passes on as long as it does not redirect it to https itself: leave "Always Use HTTPS"
   off (Caddy redirects already). The safe order is to start with the record DNS only (grey cloud) and turn the
   proxy on once Caddy has its certificate.
6. **Turn off what rewrites pages:**
   - **Rocket Loader** (Speed, Optimization);
   - **Email Obfuscation** (Scrape Shield);
   - anything else that injects scripts, such as Web Analytics' automatic setup or Zaraz.

   The page's Content-Security-Policy blocks injected scripts, and Rocket Loader would also rewrite the page's own
   module scripts.
7. **Caching:** one Cache Rule, which lets Cloudflare keep the game data and the module (Caching, Cache Rules): URI
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
8. **WebSockets** are on by default on every plan. The server pings each connection every 25 s, well inside
   Cloudflare's idle limit.

Through a real Cloudflare: **UNVERIFIED**. The client's address was checked with a stand-in Cloudflare address
([What was tested](#what-was-tested)).

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
4. Let Cloudflare keep the game data and the module: the Cache Rule of the proxy's step 7 above, and no "Bypass
   cache" rule for the hostname.

What follows from this setup:

- **cloudflared and the server share one network namespace**, on a Docker network, held by a small `pause`
  container (`net`), so either can restart on its own. cloudflared reaches the server on 127.0.0.1:8080; nothing
  else can.
- **The client's address** comes from Cloudflare's `CF-Connecting-IP`, and only from `cloudflared`. It opens a
  connection to the server for each request in progress and has no limit of its own per client: the server's own
  limits per client address hold.
- **Settings:** 6 and 8 of the proxy's list above apply too (7 is step 4 here).

A real tunnel was **not** opened here (**UNVERIFIED**). The layout was: `cloudflared` stood in by a container that
reaches the server on 127.0.0.1, crashes of either side, a reboot.

## A proxy you already have

With Caddy or nginx already on the VPS, run the server alone on the VPS's network and forward your name to
127.0.0.1:8080, as `web/deploy/Caddyfile` or the nginx block in [Without Docker](#without-docker) do:

```sh
docker run -d --name nfsu2-web --restart unless-stopped --network host \
  --read-only --cap-drop ALL --security-opt no-new-privileges \
  --memory 8g --memory-swap 8g --ulimit nofile=32768 --log-driver local \
  -e NFSU2_HOST=game.example.com \
  -v /srv/nfsu2/game:/game:ro -v /srv/nfsu2/module:/module:ro -v nfsu2-state:/state \
  ghcr.io/th0mk/nfsu2-web
```

The proxy must keep `Host`, pass WebSocket upgrades on, set `X-Forwarded-For` and `X-Forwarded-Proto`, and wait
longer than the 25 s ping interval for an answer. The image's command is the server's options, `--behind-proxy` by
default. Options with a value can also be set as `NFSU2_<OPTION>` (`web/server/server.mjs`).

## Multiplayer over the internet

The server's switch relays every packet: player → VPS → player. The delay between two players is the sum of both
paths to the VPS, so pick a VPS location between you.

- **Free roam's hub** (`web/server/roam-hub.mjs`, on 10.77.0.253 of the switch): every second it tells each player
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
    (`web/server/lan.mjs`, frame kind 32; a page or switch of before keeps one message a datagram, and both sides
    still work with it). The server's one thread pays per message, not per byte: on a live server with about 600
    players in free roam it was at 100 % of a core, and its switch took 0.1-0.7 s to answer a ping. Measured with
    120 `roam-crowd.mjs` players in one spot (a Ryzen 9 5900X): one message a datagram took the server past one core
    (20,000 datagrams a second in, pings answered after 11 s, links falling apart); bundled, 44,000 datagrams a
    second in took 74 % of a core, pings 2.7 ms (p50), 7.5 ms (p90). Only a page loaded after the server has them
    sends and takes bundles.
  - **Its own cost:** in a Room of more than 256 players the hub finds each player's nearest through a grid of the
    map instead of weighing every pair, with the same links. Measured on a Ryzen 9 5900X: 1000 players in one Room
    took about 30 ms a second (every pair took 0.5 s), 2000 about 70 ms, spread over the city or all in one spot.
  - **A busy server gives every player fewer links:** each second the hub looks at how busy the server's thread was.
    Over 85 %, the most links a player may have goes down by a quarter (32, 24, 18, 13, 9, 8 at the least), at most
    every 5 s; under 65 %, it goes back up by 2, at most every 15 s. The nearest links stay. The console says each
    change (`[roam] the server's thread was 93 % busy: at most 24 links a player (was 32)`).
- **Free roam** draws another player's car where it is now (solid cars, the default: the newest snapshot carried on
  by half the round trip), or with Solid cars off `roam_delay` milliseconds (100 by default) behind its newest
  snapshot. That delay hides the 50 ms between snapshots, jitter and the odd lost snapshot. Network delay comes on
  top of it.
  - **Measured with impairment on a local switch:** 100 ± 10 ms one way and 5 % loss.
    98.1 % of frames were interpolated, 1.9 % extrapolated, and none were held.
  - **For 50–150 ms round trips, the default is enough.** If friends' cars stutter (Wi-Fi or mobile jitter), set
    `roam_delay = 150` in Settings (settings.ini). It only adds display delay.
  - **There is no automatic raise from the measured ping.** The runtime reads `roam_delay` once at start; making it
    follow the ping is a runtime change for the net track.
  - **Over the real internet:** **UNVERIFIED**.
- **Traffic:** free roam sends 20 snapshots a second of about 95 bytes to each friend, about 1.9 KB/s per friend
  in each direction (7 friends: about 13 KB/s). A snapshot carries only the newest position, because the
  WebSocket loses none; on a real LAN (the Mac, Windows) it carries the last three, about 180 bytes.
- **Keepalive:** the switch pings every connection every 25 s. Proxies and Cloudflare then never cut a quiet player,
  and the switch finds players whose connection died.
- **Limits:**
  - 2000 players on the switch with the image (`NFSU2_MAX_PEERS=2000`; the server's own default is 128, and 2000 is
    the most its virtual network, 10.77.0.0/21, has addresses for). A game the switch refuses as full shows "The
    server is full" in the game and tries again every 5 s; it says so again when it gets a place, and the start
    page says the server is full;
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
    the ones kept across a restart do not count). When every address is held or kept, a newcomer gets "LAN full"
    until one frees.
- **LAN races** (the game's own LAN mode, from the LAN-play track) use the same switch. Over the internet they are
  **UNVERIFIED**.
- **The game's Online mode** is served by the server itself, at the switch's reserved address 10.77.0.1 (on by
  default; `--online off` in the server's `command` turns it off, and the game then cannot connect). Any name of 3
  to 15 letters, digits, `-` and `_` logs in; there are no accounts. One player per name, while online and while
  racing: a name in use is given with four digits in place of its end (on a busy server many players come as
  `NAME`), and the game plays under that.
  - **Kept in the state volume:** `online/stats.json` (race results and best times per persona) and
    `online/buddies.json` (EA Messenger friends lists, persona names only), each with the month a persona was last
    seen, written atomically. No passwords, CD keys, addresses or chat.
  - **The console** names online players by persona and virtual address (10.77.x.x) only, and shows at most 30
    Online lines a minute caused by one client address (then one line saying how many were left out).
  - **Who plays now:** `/players` answers anyone who reaches the server with two numbers for the start page and
    whether it is full: `{"players":3,"online":1,"full":false}`, the games on the switch, how many of them are in
    Online mode (logged in, or in an online race; `null` with `--online off`), and whether a game that connects
    now gets "LAN full". Nothing else: no names, addresses or times, never cached. The
    game's own lobby shows its players to anyone who logs in anyway.
  - **Limits:** 1000 lobby sessions, 3 per browser, 8 per client address, 32 per IPv6 /48, and the last quarter
    only for a client that holds none; 300 games of at most 6, 2 open and 2 started per browser (4 and 8 per client
    address), one browser one player of a race; 20 lobby messages a second per session (a flood ends the session);
    games created one every 2 s (a burst of 3), games joined, rooms entered and car descriptions published one a
    second (a burst of 4; a car description over that is not passed on); chat 1 a second with a burst of 5 (then
    10 s muted), 255 characters; a silent session goes after 120 s; a race whose players' browsers left the switch
    ends after 2 minutes. EA Messenger: 10 messages a second per connection (a flood closes it), the lists and the
    search 1 a second, a presence at most once a second; 50 friends each, 5000 friends lists (the least recently
    seen go), 20 new ones an hour per client address. Results: 100,000 personas (the least recently seen go), a
    race counts once two of its players in different browsers sent the same result.
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
- **Not tested:** a real VPS, real Let's Encrypt, a real domain, Cloudflare (proxy and tunnel), the game in Chrome
  through the current `compose.yml` (no module on the Windows PC), more than one player at once (**UNVERIFIED**).

## Without Docker

The same server from a bundle, as a systemd service, with Node.js and Caddy installed on the VPS. The behaviour,
the limits and the firewall are the same as above.

**You need** Node.js 20 or newer on the VPS (the current LTS is 24; tested on 24.22.0 and 23.9.0), and the bundle,
built on the Mac:

```sh
web/build.sh            # the game module, if out/web/nfsu2.wasm is not there yet
web/deploy/pack.sh      # -> out/web/deploy/nfsu2-web-<rev>.tar.gz (about 11 MB)
```

- **What is in it:** the server and its one dependency (`ws`), the page without its test pages, the game module,
  a systemd unit, example Caddy and cloudflared configurations, and a short README.
- **What is never in it: the game.** `pack.sh` refuses to build the bundle if any file in it has a game file's
  name, or the name and size of a file in your game folder. It also refuses if a file names this Mac (host name,
  home folder).

**Install:**

```sh
scp out/web/deploy/nfsu2-web-<rev>.tar.gz root@<vps>:
ssh root@<vps> 'mkdir -p /opt /srv/nfsu2 && tar -C /opt -xzf nfsu2-web-<rev>.tar.gz'   # -> /opt/nfsu2-web
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

**C: Cloudflare's proxy.** As in [Cloudflare](#cloudflare), and:

- with Caddy (A): the `Caddyfile`'s Cloudflare variant, with its global block at the top (Caddy then believes
  `CF-Connecting-IP` only from Cloudflare's addresses) and a Cloudflare Origin CA certificate; the server needs
  `--real-ip-header cf-connecting-ip`;
- with the server itself (B): the Origin CA certificate, plus `--trusted-proxies cloudflare`. Then only connections
  from Cloudflare's published ranges (`web/server/cloudflare-ips.txt`) may say who the client is.

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

The unit was run under systemd 257 (Debian trixie, Node 20) in a container, variants A, D and B: it starts, stays
up (no restarts) and serves. `LoadCredential` could not be exercised in a container (**UNVERIFIED**; B ran with the
certificate by path). Without `--tls-cert` or `--behind-proxy` the server listens on 127.0.0.1 only and says so on
start: nothing outside the VPS can reach it.

**Updating:** build a new bundle on the Mac, unpack it over `/opt/nfsu2-web`, and run
`systemctl restart nfsu2-web`.
