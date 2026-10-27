# NFSU2 in the browser: design

Status: **2026-10-18** (phase 2, integration). The player's page (`web/index.html`) runs the game with the WebGPU
renderer, AudioWorklet sound, keyboard, mouse and pad input, and free roam over the server's virtual LAN.
- **Driven to a race:** from the start page by the real keyboard or by an autotest script.
- **Frame rate** in a headed window on this Mac's 120 Hz panel: the menus run at 118.6–120 fps. Races reach 120 only
  when the GPU is not shared with other programs: then one race missed 5 of its 120 Hz slots, with the game thread's
  p99 at 8.6 ms against the 8.33 ms slot. With the GPU shared, races missed ~1,300–1,500 slots. Before the perf
  work they ran at 100–118 fps.
- **Start:** `web/start.sh`.
- **More:** `docs/web/PLAYING.md` (the player's guide) and `docs/web/VPS.md` (hosting for friends).
- **Test pages:** they keep the null stand-ins (`runtime/web/null_platform.c`) where they leave a subsystem out.

## 1. Goals and constraints

- **Goal:** NFS Underground 2 natively playable in the browser: whole career, save games, 120 fps, the original look by
  default. Bonuses: LAN and free-roam multiplayer between PCs, the upscaled-texture pack.
- **Browser:** the latest **Chrome only** (the user's call). Chrome 155 on the dev Mac (M4 Pro, macOS 27, 3024×1964
  ProMotion) has WebGPU on Metal 3 with `texture-compression-bc`, `texture-component-swizzle`,
  `texture-compression-unaligned`, `clip-distances`, `dual-source-blending`, `timestamp-query`, `subgroups`; JSPI;
  `Atomics.waitAsync`; 4 GB shared wasm memory; tail calls; exnref exceptions.
- **Hosting:** the player's own server, no public site.
  - A Mac (or a VPS) runs the server (`web/server-rs`, built into `out/web/server-rs/nfsu2-server`). It serves the
    app, the game module built on the Mac (`out/web/nfsu2.wasm`) and the game's own files of the NFSU2 install folder
    (`SPEED2.EXE`, `server.dll`, `server.cfg`, the data folders), read-only: never the rest of the folder (an
    installer's log, saves, mods).
  - This Mac uses `http://localhost:8080/`. Other PCs need HTTPS in front of the server (`--tls-cert`, or a reverse
    proxy or tunnel), because WebGPU and SharedArrayBuffer need a secure context; the server has no certificate of its own.
  - No file picking. Game data streams over the LAN on demand and each browser keeps an OPFS block cache.
  - The server is also the multiplayer switch (the virtual LAN).
  - The same server also runs on a VPS, for friends who are not on the same LAN (`docs/web/VPS.md`): a real
    certificate or a reverse proxy/tunnel (Caddy, Cloudflare) in front, nothing else changes.
- **Graphics enhancements are opt-in.** The original look is the default, and they ship only when measured to be a
  real improvement. No FSR (the newest generation cannot run in a browser).

## 2. Architecture

```
 Chrome page (main thread)                         workers (all share one 4 GB-max wasm32 memory)
 ─────────────────────────                         ─────────────────────────────────────────────
 UI shell, settings, input capture  ──SAB rings──▶ game threads: one Worker per guest/host pthread
 WebGPU renderer (TS) ◀─ frame packets / resource   (wasi-threads; the GIL serialises guest code)
   queue in wasm memory, Atomics.waitAsync          I/O worker: WASI files (/data over HTTP + OPFS
 rAF: vsync clock ─────────────────────────────▶     cache, /save in OPFS, /dev)
 AudioWorklet ◀── PCM ring (audio pump pthread)     LAN socket to the server's switch (net track)
```

- **Toolchain:** wasi-sdk 34 (clang 23.1), `wasm32-wasip1-threads`.
  - Flags: `-O3 -mtail-call -msimd128 -mbulk-memory -mnontrapping-fptoint -mexception-handling` (setjmp/longjmp via
    wasm EH), `-fno-delete-null-pointer-checks`.
  - The lifted shards also get `-DNFSU2_GUEST_OBJ`: their guest accesses go through `g_guest`, a weak undefined
    symbol that wasm-ld puts at address 0, instead of `(uint8_t *)0`. Same addresses, but clang then knows a guest
    store cannot touch the `restrict` CPU struct, and keeps the registers in wasm locals until the next call (before,
    it reloaded them after every guest store). With the fused x87 rounding (`X87R2`) and an inline indirect-call
    fast path (`DISPATCH_CALL`, `g_dtab`): race game-thread time p50 −25 %, p95 and p99 about −20 % (Ryzen 5900X,
    Chrome 155, `web/tools/bench.mjs --env NFSU2_FIXED_DT=120`).
  - A few hot routines run natively on wasm (`runtime/overrides.c`) where the result is provably the lifted body's,
    and take the lifted body otherwise: the CRT's `_ftol2`, the view-frustum box test and its box transform (f32 at
    PC=24; a NaN, an infinity or a float subnormal goes to the lifted body) and D3DXMatrixMultiply's SSE version
    (f32x4; NaNs go to the lifted body). The last two were 8.5 % of a race frame, 2.9 % now. `tests/main_all.c`
    sections 6 to 8 compare each with its lifted body. Tried and dropped: ThinLTO of the shards (slower). `_ftol2`
    in place at the lifted call sites gained nothing on its own; with LREGS it does a little (FTOL2INL below).
  - The compat headers (`runtime/web/compat/`) are force-included: Mach time, `os_unfair_lock`, kqueue, Mach task info,
    `pthread_exit` through a trampoline.
  - No Emscripten: the host is our own TypeScript (`web/src/host`).
  - `web/gen.sh <SPEED2.EXE>` lifts the game and `server.dll` into `gen/all` and `gen/server` (`./gen.sh` with the
    web's lift options FLAGABI, X87OPT, JTSWITCH, LREGS, FTOL2INL and INLINE_LIST). `gen.sh` stamps both
    (`stamp.txt`, `tools/genstamp.sh`: the override list, the hook list, the lifter with its inline and retpast lists,
    the exe, the options).
  - JTSWITCH: an MSVC switch (`cmp r, n; ja`, then `jmp [r*4+table]`, also through a byte table) becomes a C
    `switch` inside its function. Each case checks the table entry in guest memory and anything else still takes the
    dispatcher, so the semantics stay the same. Without it every table target was entered as a function of its own,
    a lifted copy of the rest of the parent's body (3,821 of them from 477 tables; `sub_00620ae0` alone had 20
    copies of ~660 KB of wasm). `genall.py` no longer makes functions of pointers that only such tables hold.
  - LREGS (`tools/lregs.py`, run by `lift.py` on every function): the guest registers and the six arithmetic flags
    are C locals of each lifted function (`_r_eax` ... `_r_of`; `cpu.h`'s `RG_()` names them in the shared macros).
    The CPU struct outlives the function, so before, clang had to make every write to it: a flag only the next
    branch reads was materialised and stored, a register written in a loop was stored on every pass. Now the
    struct is brought up to date only where other code can read it: before a call (registers; flags when the callee
    reads them at entry, FLAGABI's `live_in`), a tail jump, a return (flags in the functions FLAGABI says return
    them) and the run-time helpers that take the CPU, and it is reloaded after calls. A forward dataflow over the
    function's blocks stores only what may have changed since the struct was last in step.
  - INLINE_LIST (`genall.py`): the functions in `tools/inline_hot.txt` are inlined at their direct call sites. A race
    frame makes ~94,000 guest calls. `tools/inline_pick.py` chose the list (110 functions, 36 % of the calls) from a
    call-count profile (a `COVERAGE=1` lift run through `tests/web/scripts/perf_cov.txt`), by calls per byte of
    copies up to a 1 MB budget; 3 MB did the same, 6 MB was 3 % slower.
  - FTOL2INL: the CRT's `_ftol2` (`cpu.h` `FTOL2_BODY`, which the runtime override runs too) in place at its 1,769
    call sites (~11,000 calls a race frame), where its ecx and flags are dead.
  - Measured (Chrome 155, M4 Pro; `web/tools/bench.mjs`'s lockstep recipe: the same race every run, 3 runs per
    build): the game thread's P-core cycles per racing frame 16.45 M -> 15.48 M (−5.9 %), instructions 56.7 M ->
    52.7 M (−7.1 %), its busy time −4 %. Of that LREGS −3.2 %, runtime fixes about −1 % (the vertex/index mirror
    table's hash used the low bits of the product, so the 16-byte-aligned keys filled 256 of its 4,096 slots; an
    effect texture's ~27 reference counts per SetTexture went through three calls each), the inlining −1.3 %,
    FTOL2INL a little. The module is 1.2 MB smaller (35 MB). Uncapped the game thread's time per frame went from
    2.73 to 2.25 ms, but there each build plays a different race. Tried and dropped: guest displacements folded into
    the wasm load offset (+9 % instructions: V8 then zero-extends and adds in 64 bits per access), leaf-only
    inlining (INLINE=8: nothing), a fast path or a split of `com_call` (nothing), and the x87 rounding at PC=24 is
    not a bottleneck (`NFSU2_FPU_PRESERVE=1` keeps PC=53: the same time).
  - A lifted call continues after the call. The CRT's x87 2^x core (`0x7653de`, under exp, sinh, cosh and tanh) does
    not come back there when its argument is out of range (|x·log2 e| ≥ 32767.5): it pops its own return address and
    its ret goes to its caller's caller. After a direct call to a function in `tools/retpast.txt` (each game's list is
    its `game.conf` RETPAST_LIST) the caller compares `c->eip` with its own return address and returns as well when
    they differ. A static scan of the lifted functions' returns found no other such function with direct callers, but
    it could not follow the stack in every function.
  - `web/build.sh` refuses a `gen/all` made from another override list, hook list or lifter than the checkout's,
    then does an incremental shard build through a shared content-addressed object cache
    (`~/.cache/nsfu2-web/obj`), links `out/web/nfsu2.wasm` (35 MB) and writes `nfsu2.json`.
- **Memory:** guest address == linear address (`cpu.h`).
  - The guest owns `[0, 0x28000000)` (640 MB):
    - image at 0x400000
    - server.dll (LAN hosting, lifted second module) 0x00A00000–0x00A58000; Winsock's per-thread hostent buffers
      0x00BE0000
    - TEBs at 0x00C00000
    - server.dll's heaps (`modheap.c`) 0x01000000–0x05000000 (2 × 32 MB; the Mac's 0x01000000–0x0F000000 would
      overlap the stacks)
    - main stack 0x05000000–0x05FF0000
    - thread stacks from 0x06000000
    - heap VA 0x08000000–0x28000000
  - The runtime's data, C stacks and malloc heap sit above (`--global-base=0x28000000 --no-stack-first`).
  - Initial memory 1 GB, maximum 4 GB.
- **Threads:** the page starts the I/O worker and a pool of game workers (`web/src/host/process.ts`): 12 at first,
  then more as threads take them so that 2 stay idle, up to 24. A race or free roam starts 15 (at most 13 threads at
  once), hosting a LAN game 18; all 24 at once held about 55 MB more of WebKit's page process (a phone's memory).
  - `wasi thread-spawn` claims an idle pool slot synchronously. One that finds none asks the page for a worker and
    waits for it (up to 10 s, as a slow `CreateThread`; never more than 15 ms in the suite's 19 scenarios). Workers block with `Atomics.wait`; the
    page's main thread never runs wasm and never blocks.
  - Guest threads still hold the GIL (`win32_ext.c`): a FIFO ticket lock (`runtime/web/ticketlock.h`). A woken
    worker sometimes runs only 3 to 4.6 ms after its notify (Chrome 155 on Windows 11: about 1 in 1,800 wakes,
    measured between two plain workers), so every hand-over of the lock to a sleeping thread can cost a 120 Hz slot.
    The frame pacer therefore takes the lock back at its slot boundary ahead of the queue when nobody holds it
    (`gil_acquire_frame`), instead of waiting behind a thread still waking up. And `WaitForSingleObject` takes a free
    mutex without handing the GIL over unless another thread queues and this one has held it for 2 ms (the game
    thread handed it over at every take of the sound library's mutex). Taking every free mutex outright helped the
    race a little more, but in a heavy free-roam scene (32 friends' cars, 13.5 ms of game work a frame) the other
    threads then waited 4 to 10 times longer (the loader 0.7 → 8.7 ms on average, p99 33 → 262 ms).
- **Stacks:** a Chrome worker's engine stack is about 1 MB and cannot be raised (measured: `--js-flags=--stack-size`
  changes nothing).
  - Lifted frames are small (48–400 B) and indirect tail jumps are real tail calls (`DISPATCH_TAIL`/`dispatch_tail`,
    `musttail`).
  - Fallback if a deep chain ever overflows: hop to a fresh stack segment through a JSPI `WebAssembly.promising` call.
    This is measured to work: a 100,000-deep recursion passed through 25 segments in 5.9 ms.
- **Files:** WASI preview1 calls go to the I/O worker over per-thread RPC slots (SAB + `Atomics.wait`). Results are
  written straight into wasm memory. Mounts:
  - `/data`: the install folder. Manifest from the server, case-insensitive lookup, 1 MiB blocks, memory LRU,
    persistent OPFS block cache (`web/src/host/datacache.ts`). The memory holds 64 MB while OPFS keeps the blocks (a
    block reads from OPFS in 0.14 ms in WebKit), 768 MB without it. At most 64 cache files are open at once: each
    holds two sync access handles, and WebKit gives a page 251 in all (then "Invalid platform file handle", for the
    save folder's files too); kept open for good, a long free roam used them up in 13 min. Blocks come from the
    server's `/data-blocks/` (named by the manifest: `/data-blocks/v2/`): runs of up to 8 blocks as one answer,
    compressed with zstd, br or gzip as Content-Encoding, which the browser undoes itself; a run whose first 64 KB
    does not compress goes as it is. One block (all the page asks for) goes to a client that takes br as the server
    keeps it: compressed once at brotli's
    best in its state folder (`web/server-rs/src/app/kept.rs`, threads of its own at the lowest priority that wait
    while the CPU quota is 70 % used, the blocks asked for first, then the manifest's), or as it is where that found
    it does not compress; until it is kept, as
    above, and Cloudflare keeps that answer 10 minutes only (`EDGE_WHILE`). `v2` since: Cloudflare's copies of the
    answers from before are at the old addresses, which still answer. The address carries the file's
    mtime and size (`m`, `s`), and the server lets a shared cache (Cloudflare) keep a run for good when they are the
    file's (read from the one open file it answers from), the run is compressed or does not compress, the movies too
    (`EDGE_KEEP`). A run whose probe read fails is compressed, never kept as it is. An older server: byte ranges
    of `/data/`. A request that fails on the way (no connection, 408, 429, 5xx) or stalls (no byte for 20 s) is
    asked again after a growing pause, for as long as it takes: a failed read would give the game 0 bytes. After two
    tries with no answer at all, every other try asks for the block as a byte range of `/data/` (another address, not
    in Cloudflare's cache, uncompressed). A block the OPFS cache has no room for (a full disk) ends the keeping for
    that game, not the read. The log's `[data]` lines name each block that waited 5 s or needed another try, and a
    cache that filled up (`NFSU2_DATA_FAULT` tests it, the web suite's `datafault`). At most 8 blocks are fetched at
    once, and a block waiting for a turn goes by priority: the track's stream file (`TRACKS/STREAM*.BUN`) first, as a
    section the world waits for is a "Loading" screen in the middle of a drive, then the game's other reads, then
    the reading ahead of the track. Once the game reads a track's stream file, the blocks of it this PC lacks come in
    at that lowest priority, the nearest to the game's latest read first (two at once, and only while the game fetches
    nothing: on a slow line a block read ahead would share it with the game's), until the cache has all of it (the city's 113 MB,
    about 40 MB on the network); its sections then read from this PC. Only while OPFS keeps the blocks;
    `NFSU2_TRACK_AHEAD=0` turns it off. The data but the movies crosses
    the network in 61 % of its size (1.28 of 2.09 GB, kept; 1.39 GB compressed as it is sent; tracks about a third).
    "Preload the game data" (Game data on this PC, at the bottom of the start page) fills the cache with all but the
    movies ahead of play from its own worker (`predownload-worker.ts`, 4 blocks at once, each at the address the game's
    own reads use, so one copy in Cloudflare's cache serves both). It runs only while no game
    runs in the tab: Play pauses it, and the game's end resumes it.
  - The module: `/game.json` (no cache keeps it) names it by the hash of its content (`module`, the server's own),
    and the page asks for `/game.wasm?v=<hash>` (`web/src/app/loader.ts`). That address the browser and Cloudflare
    keep for good (`immutable`): a start reads it from the disk cache without asking, Cloudflare serves every player
    after the first, and a new build is a new address, so no copy anywhere is an older module. The server says so
    only when the hash is the one of the module it sends; another hash (a build that arrived after the page read
    `/game.json`) gets 409, and the page asks `/game.json` again. The bare `/game.wasm`, and the module as it is (no
    br, a byte range), are revalidated and kept by no shared cache.
  - Offline mode: a service worker (`web/src/sw.ts`, registered once the module has loaded) keeps the start page,
    its scripts, the module (at its own address, one at a time: offline, a start gets the module its `game.json`
    names or none), `game.json` and the manifest in Cache Storage, network first: the server's answer when
    there is one (the copy replaced when its ETag changes; after each start page from the server the workers and the
    manifest are brought up to date too, so the copies are one version), the copies when it fails, answers 5xx or
    keeps the start page waiting 4 s, and then for 30 s first, whatever the server answers meanwhile (one start, one
    set: a script from a copy beside the server's `game.json` cannot draw the server's module). Nothing else passes
    through it. At Play the page asks
    the server (`/players`, 2 s); without an answer the movies are off for that game unless all of them are cached.
  - A newer version: the module stays loaded while the page is open, through every game it plays, and a page can
    start from the offline copies of the version before (a deploy restarts the server while its players reload). The
    page asks for `/game.json?latest` (the query keeps it past the offline copies) once the module has loaded, when
    the server answers again, when the tab is shown, after each game, at Play, every 5 minutes while the tab is
    visible (in a game too) and within 30 s of the game's link to the server coming back. On another revision than
    the loaded one: with nothing on the start page a reload would lose (no game ran in the page, no dialog, no field
    being typed in, no game data downloading) the page reloads by itself, after telling the service worker the
    server answers (so the reload is not the copies'); in a game it says so over the game once; otherwise it offers
    Reload, and Play reloads too. sessionStorage names the revision a reload was for, so one that came back without
    it only offers it. A page with test settings in its address only offers it.
  - A page of another build than its module (the renderer finds the game's GPU control block of another layout,
    `OtherBuild`: the page's script a copy and the module the server's, or a page loaded while a deploy swapped the
    module): the game is stopped and the page reloaded with the service worker unregistered, so no copy answers it
    (the page registers it again, and its install takes new copies). Once a tab and revision, while the server
    answers; after that the renderer did not start (`GPU-RENDERER`), as for any other reason.
  - `/save`: OPFS save overlay. Holds saves, `registry.txt`, `settings.ini` and logs. Reads look there first, but
    for the game folder's own folders (`TRACKS`, `CARS`, ...) once until the next write (`runtime/fileio.c`): each
    look is OPFS calls in the I/O worker, 1.5 to 2 ms an open with the GIL held, and a race streams its track by
    opening `TRACKS\STREAM*.BUN` again and again, from the game thread too. Closing a file the game wrote
    there posts `{ t: 'saved', path }` to the page (the "Progress saved" note, F9's last save, the end screen, and
    keyphrase saves: `web/src/app/vault.ts` seals the overlay but its screenshots with a key made from the player's
    keyphrase and keeps it in step with the server's `/vault`, which holds only the sealed bytes). Before
    each Play the page sets every profile still on the game's Save = Prompt to Autosave, once per profile
    (`web/src/app/profile.ts`, settings.ini `autosave`).
  - `/dev`: `null`, `zero`, `urandom`.

  The runtime runs as on the Mac with `NFSU2_SAVEDIR=/save` and `NFSU2_READONLY_DATA=1`.
- **Movies in AV1:** where the browser decodes AV1 (WebCodecs `VideoDecoder.isConfigSupported`, software decoder
  preferred: `web/src/host/movie.ts` `movieCodec`, before Play and in `startProcess`), the I/O worker opens
  `MOVIES/AV1/<name>.vp6` (and `LANGPACKS/<language>/MOVIES/AV1/<name>.vp6`) in place of `MOVIES/<name>.vp6` when
  the manifest has it (`io-worker.ts` `dataFile`, `datacache.ts` `av1Path`; the offline "every movie kept" check and
  the preload's movie exclusion count the same files). `tools/movies_av1.py` makes them: EA's movie file with every
  chunk kept in place (the sound byte for byte) and each video frame's payload swapped for `AV01`, a key flag and one
  AV1 temporal unit, a key frame at every `MV0K` (the player jumps to the next one when it falls 16 frames behind),
  pictures stored bottom-up as the game's VP6 decoder leaves them. The game's own movie player still reads the file,
  keeps the time, plays the sound, skips and ends it; `runtime/movie.c` takes over the VP6 decoder's three calls
  (decode `0x702e47`, picture `0x70288e`, free `0x702e5e`) for tagged frames: each goes to the I/O worker
  (`Op.MovieDecode`), whose `VideoDecoder` (`optimizeForLatency`: otherwise Chrome and Firefox hold 2–3 frames back)
  writes the picture straight into a block of the VP6 decoder's layout in the game's memory (Y with a 48-pixel
  border, then U and V), and the player gets that block. Chrome gives I420 (copied in place); Firefox gives BGRX,
  turned back into the YUV it came from by the frame's matrix; Safari, whose AV1 decoder is the Mac's or iPhone's
  hardware (it says yes to the software preference too, and gives every picture at once), gives NV12, its UV split
  into U and V. A frame takes 2 to 4 ms in Chrome, about 8 in Firefox (the conversion) and about 1 in Safari 27 on
  an M4 Mac, less than the lifted VP6 decoder took; one that brings no picture within 250 ms keeps the last one on screen. One log line a movie:
  `[movie] AV1 768x512: N frames, 0 without a picture, decode … ms a frame (max …), 0 catch-ups`.
  `NFSU2_MOVIE_CODEC = vp6` (settings.ini, `&env=`) plays the VP6 files anyway; the page's session line says which.
  At crf 26 (cpu-used 4) every movie keeps a VMAF of 95 or more against its VP6 pictures, and the pictures the game
  gets are within 42 to 55 dB PSNR of its own VP6 decoder's (`NFSU2_MOVIE_DUMP`, `runtime/movie.c`): the game
  folder's 965 MB of movies become 179 MB, a dubbed language's 216 MB 153 MB.
- **Clock:** `clock_gettime(MONOTONIC)` is `performance.timeOrigin + performance.now()`, which is one clock across
  workers.
- **Display rate:** the page measures the refresh rate from rAF, as the median of spans of 4 frames: Safari on an
  iPhone or iPad gives rAF times in whole milliseconds, and single intervals had a median of 62.5 Hz at times at 60
  Hz, always 125 at 120. `fps = auto` follows the rate as it changes (`NFSU2_DISPLAY_HZ` is only the page's
  measurement at Play): a 30 Hz phase (Safari in Low Power Mode; Playwright's WebKit on the Mac after a minute
  without input, or with another window in front) runs the game at 30, with the 30 fps fixes
  (`runtime/framerate.c`), instead of making every other frame for nothing: in WebKit's free roam 1 of 896 frames
  was then drawn over unseen, and the web content process took 27 % of a core instead of 42 %. The log's `[gpu]`
  line a minute counts the frames drawn over before the browser showed them (more than one frame between two rAFs;
  at fps = 120 on a 60 Hz rAF half of them, on a 30 Hz one three quarters).
- **The log for a player to send** (`web/src/app/logs.ts`, Download log): besides the game's own lines (its `[status]`
  line a minute: fps, CPU, frame times, heap), the page writes the browser and system by name (an iPhone's Safari is
  "Safari 26 on iOS", an iPad's "on iPadOS" although it says Macintosh), a `[app] device:` line (screen, pixel ratio,
  cores, touch, Home Screen app or tab, a 30 Hz display before Play), and a line a minute each from the renderer
  (`[gpu] 60 s to t=…`: frames, rAF rate, frames drawn over, the GPU's time to finish a frame p50/p90/max, frames
  that waited for the GPU, the textures' memory on the GPU) and, while blocks come from the server, from the I/O worker
  (`[data] 60 s: …`: blocks, MB, average and slowest fetch, tries again, reads from the cache, cache files open). How
  long the page was hidden and an AudioContext interruption lasted are in the log too. Safari stops a page whose memory
  it needs without any event, and the log went with it: `web/src/app/lastgame.ts` keeps a copy of the running game's log
  (`LogRing.snapshot`, 160 KB at most: the opening lines, the game's session lines, the newest lines) in localStorage
  every 10 s while it grows, when the page is hidden or shown, and at the end. A page that finds a game that did not get
  to its end (or one stopped for a reason, GPU-LOST say, before a reload) says so on the start page and puts that log
  first in Download log, with a line on how it ended (the page on screen or hidden at its last copy) and what the game
  held at that copy: the textures' MB (and those unpacked from DXT or DXT1 as ASTC) and, in free roam, the friends' cars in the game
  and the friends connected (the status table's `roam_cars` and `roam_friends`, `runtime/web/status_web.c`). Safari
  gives the page no memory figure, and the first kept iPhone kill came between two renderer lines. A game the player
  left by reloading or closing the page (`pagehide`) is offered as well (a frozen game, reloaded), with a quiet line and
  no call for a bug report (`lastGameEnd`). Tested by killing WebKit's web content process during a game (`kill -9`,
  as Safari's memory limit does).
- **Frame pacing on Windows:** a worker's `Atomics.wait` timeout wakes on the OS timer, which Chrome on Windows runs
  at 1 ms or at 15.6 ms (Chrome 155: a 1 ms wait took 15.5 ms at the median). The pacer's sleep
  (`runtime/win32_ext.c`) therefore waits until a learned margin before its deadline and spins on the clock for the
  rest, while the page draws (not in a hidden tab). A race at 4K on a 160 Hz screen (content rate 120, Ryzen 5900X)
  went from ~105 to ~119.5 fps in the warm drive; before, one frame in five was presented late.
  Whether the pacer tries a wait at all comes from a probe thread that waits 1 ms every ~10 ms and keeps the
  lateness (up at once, down in about a second). Before, on the 15.6 ms timer the pacer tried a wait of its own every
  1.5 to 5 s, and that wait mostly cost one or two slots: a hitch every few seconds. `NFSU2_TEST_TIMER_MS=15.625`
  (`web/src/host/game-worker.ts`) ends every sleep on that grid, as on Windows. With it (M4 Pro, `bench.mjs --fps
  120`, 2 runs each) late frames in the warm drive went from 10 and 10 of 4,224 to 0 and 2, missed slots from 6 and 3
  to 2 and 1. With the Mac's fine timer the pacer still waits on 97 % of its sleeps, as before. The exit report
  has a "pacer sleeps" and a "timer probe" line, and a coarse timer logs a `[timer]` line once.

## 3. The renderer contract (C recorder ↔ TypeScript WebGPU encoder)

The game thread keeps `com.c` (D3D9 state, shader translation) and records through the `metal_*` API (the boundary
`com.c` already has).

- `runtime/web/gpu_web.c` implements that API. It writes into **shared memory**:
  1. **Frame packets.** Port the essentials of `metal.m`'s record side:
     - double/triple-buffered `MDraw`-like records;
     - the raw vertex arena;
     - the VS constant arena;
     - the PS constant pool;
     - clears and blits;
     - render-target switches.
  2. **An ordered resource queue.** Texture create/upload/free, mirrors (persistent vertex/index pool uploads) and
     shader registration (WGSL text). It replaces the immediate Metal calls the guest thread made on the Mac.
  3. **A present handshake.** A frame-state word: the game publishes with `Atomics.notify`, and the page wakes with
     `Atomics.waitAsync`. Pacing follows a vsync clock the page fills from rAF (timestamp, period). The game thread
     never waits on JS or WebGPU except for frames in flight (≤ 2) and explicit readbacks.
- The TypeScript side (`web/src/gpu/`) reads those structures **zero-copy** from the wasm memory and uploads arenas with
  one `queue.writeBuffer` each per frame. It keeps pipeline and bind-group caches and encodes the passes. It presents to
  the canvas at native resolution, aspect-fit; at a render scale below 1 (a phone's 75 %) with the picture in the
  view's shape, the canvas takes the picture's size, the frame draws straight into it and CSS stretches it, so there
  is no composite pass over every screen pixel (Playwright WebKit, iPhone 13 geometry: GPU 0.62 -> 0.60 ms a frame on
  an M4 Pro, the screen within 2 levels where it holds still; three canvas buffers of 1898x876 instead of 2532x1170).
  It reads frames back for the tests' dumps and F12 screenshots (the
  PNGs are made in a worker, `png-worker.ts`, off the thread that draws). The one readback the game waits for: a
  render target it locks to read, the picture of a DVD or magazine cover. The frame drawn so far goes to the page
  early with the request, and the page writes the texture's rows into the game's lock buffer
  (`gpu_web.c` `metal_read_texture`). When the game's device size is
  the canvas's (the usual case) the frame draws its screen straight into the canvas texture: no back buffer and no
  composite pass. A frame that copies from the screen before drawing to it keeps the back buffer (it reads the
  previous frame); none of the checked scenes did. Firefox keeps the back buffer too: it shows the canvas texture
  while the next frame draws into it, so drawn straight in, frames showed half drawn and partly black (Firefox 158 on
  a Mac: 3 of 25 race screenshots, none from the back buffer). `?direct=0` / `?direct=1` on the test page turns the
  direct path off or on.
- DXT textures stay BC on the GPU. A GPU without `texture-compression-bc` (PowerVR and Mali phones) gets them unpacked
  to BGRA8 as they upload (`web/src/gpu/dxt.ts`). Measured on a Quick Race (Resort Loop) with a desktop CPU: about
  7× the texture memory (30 MB packed, 213 MB unpacked) and about 0.4 s of page-thread work, all of it in the menus
  and on the loading screen (pauses up to ~0.2 s there), none during the race. `?bc=0` on the test page takes this
  path on any GPU.
- Where such a GPU has `texture-compression-astc` (Apple's: every iPhone and iPad), DXT1 goes up as ASTC 4x4 instead
  (`web/src/gpu/astc.ts`): block for block, the two RGB565 colours kept exactly (endpoint mode 8, 8 bits) and the codes
  as weights (2-bit 0/21/43/64 for four colours, trits 0/32/64 for three). On an Apple GPU (`web/tools/astc-check.mjs`,
  Chrome and WebKit) a texel is at most 2 of 255 from the CPU's unpacking, as close as that GPU's own BC1 (mean 0.469
  against 0.471); in game frames at most 4, 87-91 % of the pixels identical. Textures 228 → 112 MB in free roam
  (Quick Race 229 → 113): DXT1 is 4x smaller than unpacked, and rewriting a level is quicker than unpacking it
  (1.2 against 1.7 ms for 1024x1024). A DXT1 texture with transparent texels (a three-colour block's fourth code) in
  the first level that goes up is unpacked as before: one plane cannot give transparent black. One met only in a later
  level gets two-plane blocks (alpha exact, the colour under a transparent texel the darker endpoint, not black).
  DXT3 and DXT5 stay unpacked. `?astc=0` on the test page unpacks DXT1 too.
- C and TS layouts come from one source: a C header with `_Static_assert` offsets, plus a generated or hand-mirrored
  `proto.ts` checked by a test.

### Shader ABI (WGSL), shared by the translator and the encoder

The translator (`vsemu.c`, a WGSL backend next to `vs_to_msl`/`ps_to_msl`) and the encoder both follow this ABI. The
authoritative version is `docs/web/gpu-abi.md`.

```wgsl
// group 0: shared by runs of draws, dynamic offsets
@group(0) @binding(0) var<storage, read> VB : array<u32>;   // vertex pulling: the frame arena or the mirror pool
@group(0) @binding(1) var<uniform> K : VSWin;               // 64 KB window; the draw's {stride, tsel, base, pad; c[NK]} at row firstInstance
@group(0) @binding(2) var<uniform> P : PSPar;               // {aref, usec, pad, pad; proj; c[32]; fog}
// group 1: textures and samplers, cached per tuple
@group(1) @binding(0..3) t0..t3 : texture_2d<f32>           // texture_cube<f32> for t0 in the cube variant
@group(1) @binding(4..7) s0..s3 : sampler
struct VOut { @builtin(position) pos: vec4f; @location(0) col0: vec4f; @location(1) col1: vec4f;
              @location(2) uv0: vec4f; ... @location(5) uv3: vec4f; @location(6) fog: f32 }
```

- **Vertex pulling** as on Metal: no vertex attributes, so a pipeline does not depend on the vertex format. The vertex
  address is `base + vertex_index * stride` (the draw's K block header); `drawIndexed`'s `baseVertex` handles D3D9's `BaseVertexIndex`.
- The fixed-function / pretransformed (`TVert`) path is one hand-written WGSL module over the same ABI.
- Alpha test is an `override` constant, so there is a no-discard variant when `aref <= 0`, as on Metal for HSR.
- Everything is triangle lists, because `metal_draw_raw` already unrolls strips and fans on the CPU.
- D3D9 gaps that WebGPU does not have:
  - 16-bit formats: `com.c` already converts to A8R8G8B8.
  - Border clamp: not used.
  - LOD bias: `textureSampleBias`, if used.
  - D3DCOLOR: unpack bytes in the shader.

## 4. Testing

- Without a browser:
  - `web/tools/isatest.sh` (`WEB_LIFT=opt` for the web's lift options): the x86 ISA suite (`tools/isatest.py`, 610
    tests, 646 with `WEB_LIFT=opt`) lifted and compiled like the game's shards, run in V8 under node:wasi. The first
    gate after a lifter or `cpu.h` change.
  - `web/tools/aliastest.sh`: guest memory type punning (`tests/web/aliastest.c`). It runs
    `web/tools/aliasguard.sh` first, a structural check that every guest access macro of `runtime/cpu.h`'s wasm
    branches keeps its `may_alias` type; `web/build.sh` runs the guard before it compiles anything.
  - `cd web && node --test test/*.test.mjs`: the unit tests (the LAN backend `lan_unit.c`, free roam, the C ns* key
    driver against the page's key path, `settings.ts` against `bundle.m`, the alias guard and test, the server,
    deployment, the security and privacy audit's regressions: `audit.test.mjs`, `page-audit.test.mjs`). They build their wasm programs with wasi-sdk from `deps/`.
- `node web/tools/predownload-test.mjs` checks "Preload the game data" in Chrome against its own server and a
  made-up game folder: byte-exact cache, no movies, zstd where the bytes compress, stop and resume, retries, a refused
  file, Play pausing it, a second tab. `web/test/data-blocks.test.mjs` checks the server's `/data-blocks/` (and its kept blocks),
  `web/test/module.test.mjs` the module's address (`/game.wasm?v=`) and what caches may keep.
- `node web/tools/offline-test.mjs` checks offline mode with the real game folder and a server it starts and stops:
  the copies kept, Play to the title screen with the server gone, behind a proxy answering 502, and with a server
  that never answers, then online again, and a deploy (the page from the copies reloads by itself into the new
  version once the server answers).
- `node web/tools/run-page.mjs <url> <ms>` drives Chrome 155 (`channel: 'chrome'`, headless, WebGPU works) and streams
  the console.
- Test page: `/test/hostcheck.html?wasm=/game.wasm&pages=16384&arg=/data/SPEED2.EXE&env=K=V&...&save=<opfs dir>&fresh`
  - `&reg=/tests/web/seed/registry_perf4_native.txt` seeds the registry, as `run_suite.sh` does.
  - `&script=/tests/scripts/quickrace.txt` adds an input script.
  - Suite env: `NOMOVIES=1 SNDSYNC=1 GPUVS=1 NFSU2_FNG_LOG=1 NFSU2_STATUS_EVERY=600 NFSU2_DISCORD=0`.
- The server: `out/web/server-rs/nfsu2-server --data "<game folder>" --http-port <http> --dev` (built by
  `web/server-rs/build.sh`).
  `--dev` serves the test pages (`/test/`), the repo's test scripts (`/tests/`), source maps and, with `--artifacts`,
  frame dumps; without it the server serves only what a player needs.
- **Logs are English:** the runtime's log lines, the lifter's trap reasons in the lifted C, the generator's and the
  tools' console output and the test harnesses.
  - Among their other checks the suite runners fail a run on these markers: `FAULT IN GUEST`, `GUEST MEMORY FAULT`
    or `HOST SEGV` (a fault), `TIME LIMIT REACHED` (boot.c's alarm or the runner's limit), `UNRESOLVED INDIRECT` (an
    indirect call into no lifted function), a line starting with `[trap]`.
  - `web/test/log-english.test.mjs` checks that every runner and report still matches the markers the runtime
    prints, and that the runtime's string literals stay English.
