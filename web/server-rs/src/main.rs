//! NFSU2 web: the server, in Rust.  It replaced the Node server, with the same options, environment, console lines,
//! files and answers.  What it serves, to Chrome on this machine, on the LAN, or (on a VPS) over the internet:
//!
//!   /                  the web app (web/dist)
//!   /game.wasm         the game module (out/web/nfsu2.wasm), brotli on the wire
//!   /game.json         what was built (size, memory layout, revision)
//!   /data/<path>       the NFSU2 install folder, read-only, with byte ranges: only what the game reads
//!                      (GAME_ENTRIES), never the rest of the folder (an installer's log, saves, mod settings)
//!   /data-manifest     those files: path, size, mtime
//!   /data-blocks/<path>?b=<first>&n=<count>
//!                      a run of 1 MiB blocks of one of them, compressed (zstd, br or gzip, whichever the client takes)
//!   /lan               WebSocket: the virtual LAN switch for multiplayer (nfsu2-lan), with the game's Online mode on
//!                      its 10.77.0.1 (nfsu2-online; --online off: none, the game then cannot connect) and free
//!                      roam's hub on 10.77.0.253 (nfsu2-roam: each player's peers are the players nearest to it;
//!                      --roam-hub off: none, players find each other by broadcast)
//!   /players           how many play on this server now: { players, online, roam, full, queue, roamBusy }, three
//!                      counts, whether a newcomer waits in line, how many do and whether free roam is busy, nothing
//!                      else (never cached);
//!                      ?room=<hash> adds room, how many of roam are in that free-roam room;
//!                      ?crews=<hash>,<hash>,... (at most 4) adds crews, how many of roam are in each of those crews
//!   (and for the operator, nothing on the network: control.sock in the state folder, which `nfsu2-server ctl` talks
//!   to: who is on, kick, ban, unban; src/ctl.rs, app/control.rs)
//!   test aids, only with --dev: the test pages (/test/) and source maps from web/dist-dev (web/build.mjs writes them
//!   there, never into web/dist), the repo's test scripts (/tests/), PUT /artifacts/<name>.png (with --artifacts)
//!
//! Its own names (the Host a request may carry, and the Origin of a /lan page): localhost, 127.0.0.1 and [::1],
//! always; with --tls-cert, the names in that certificate; the names given with --host (required with
//! --behind-proxy: the proxy forwards the player's Host, and this server cannot know the public name).  Anything
//! else gets 421.  HTTPS (Chrome allows WebGPU and SharedArrayBuffer only on secure origins; http://localhost is one)
//! comes from --tls-cert/--tls-key (a certificate from Let's Encrypt, or a Cloudflare Origin CA certificate; read
//! again when the file changes or on SIGHUP), or a reverse proxy or tunnel on this machine (Caddy, nginx,
//! cloudflared; --behind-proxy: plain HTTP on 127.0.0.1:<http-port> only), or not at all (only this machine can
//! play, over http://localhost:<http-port>); plus plain HTTP on 127.0.0.1:<http-port> for this machine (0 = none).
//!
//! The work runs on many cores: a tokio runtime with half the cores as workers, 2 to 8 (NFSU2_THREADS), the
//! compression of the game data on a pool of its own (half the cores), the Online host and the free-roam hub on a
//! thread each.  The repository the defaults are in (web/dist, out/web/nfsu2.wasm, out/web/server): the one this
//! binary was built from, or NFSU2_REPO.
//!
//! One game per server: NFS Underground 2, or with --game mw NFS Most Wanted (its own program, entries, page, module
//! and state folder: game.rs).  `nfsu2-server [--game mw] --print-entries` prints what of the game's folder it serves.

use mimalloc::MiMalloc;

#[global_allocator]
static GLOBAL: MiMalloc = MiMalloc;

mod app;
mod ctl;
mod game;
mod options;
mod paths;

/// the message on stderr, then exit code 1
pub fn die(msg: &str) -> ! {
    nfsu2_util::log::error(msg);
    std::process::exit(1)
}

/// the repository the defaults are in: NFSU2_REPO, else the one this binary was built from
fn repo(cwd: &str) -> String {
    match std::env::var_os("NFSU2_REPO") {
        Some(r) if !r.is_empty() => paths::resolve(cwd, &r.to_string_lossy()),
        _ => paths::normalize_abs(&format!("{}/../..", env!("CARGO_MANIFEST_DIR"))),
    }
}

fn main() {
    let argv: Vec<String> = std::env::args_os().skip(1).map(|a| a.to_string_lossy().into_owned()).collect();
    let cwd = std::env::current_dir().map(|p| p.to_string_lossy().into_owned()).unwrap_or_else(|_| "/".into());
    let env = |k: &str| std::env::var_os(k).map(|v| v.to_string_lossy().into_owned());
    // an operator's command to the running server (src/ctl.rs), not a server
    if argv.first().is_some_and(|a| a == "ctl") {
        std::process::exit(ctl::run(&argv[1..], &env, &repo(&cwd), &cwd));
    }
    // the game folder's entries it serves, one a line (web/deploy/pack.sh asks), not a server
    if argv.iter().any(|a| a == "--print-entries") {
        let g = options::game(&argv, &env).unwrap_or_else(|m| die(&m));
        for e in g.entries {
            println!("{e}");
        }
        return;
    }
    let o = options::parse(&argv, &env, &repo(&cwd), &cwd, &options::RealFs).unwrap_or_else(|m| die(&m));
    let cores = std::thread::available_parallelism().map_or(1, |n| n.get());
    // half the cores, 2 to 8: more workers do the same work for more CPU (parking and waking, stealing from each
    // other: 2000 players in free roam took 2.9 cores on 4 workers, 3.8 on 8, 4.3 on 12, at the same latency up to
    // 8), and the cores left over are the tunnel's in front (cloudflared pays for each message too)
    let env_threads = std::env::var("NFSU2_THREADS").ok().and_then(|v| v.trim().parse::<usize>().ok()).filter(|&n| n >= 1);
    let threads = env_threads.unwrap_or((cores / 2).clamp(2, 8));
    let rt = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(threads)
        .thread_name("nfsu2-worker")
        .enable_all()
        .build()
        .unwrap_or_else(|e| die(&format!("the runtime: {e}")));
    rt.block_on(app::run(o, cores));
}
