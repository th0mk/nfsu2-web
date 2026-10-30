//! The start: the state folder, the switch and its hosts, listening (HTTPS with a certificate, HTTP on 127.0.0.1),
//! the manifest and the module, the timers, the signals, and the start-up lines (the tests wait for "  access    :"
//! and read others).

use std::os::unix::fs::{DirBuilderExt, MetadataExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use tokio::signal::unix::{SignalKind, signal};
use tokio::time::{Instant, interval_at};

use nfsu2_http::tls::TlsState;
use nfsu2_http::{Limits, bind, serve};
use nfsu2_lan::{LanOptions, LanSwitch};
use nfsu2_online::{LobbyTrace, OnlineHost, OnlineOptions};
use nfsu2_roam::{RoamHub, RoamOptions};
use nfsu2_util::cpu::{QuotaBusy, WorkersBusy, process_cpu_seconds};
use nfsu2_util::{Host, log};
use nfsu2_vault::{Sweep, Vault, VaultOptions};

use super::{App, etag, limits};
use crate::die;
use crate::game::ModuleJson;
use crate::options::{Options, Tls};
use crate::paths;

/// what the players on the switch do, every 5 minutes while any are on (counts only)
const BREAKDOWN_EVERY: Duration = Duration::from_secs(5 * 60);
/// SO_SNDBUF of a /lan connection (NFSU2_LAN_SNDBUF; 0: the kernel's own), which Linux doubles: what a player's path
/// does not take waits in the switch's own queue then, where its rules see it (roam_queue), not in the kernel's,
/// which Linux grows to 4 MB (nfsu2.xyz, 2026-10-22: up to 4 MB waiting there for one player, seconds of free roam)
const LAN_SNDBUF: u64 = 32 << 10;
/// the threads that compress the game data's blocks at brotli's best, once, at the lowest priority
/// (NFSU2_COMPRESS_THREADS; kept.rs): a game folder with every language pack (5600 blocks, 3100 that compress) in
/// about half an hour
const COMPRESS_THREADS: u64 = 2;
/// keyphrase saves: counted at the start, then swept (the unused removed) and counted again every day
const VAULT_SWEEP_EVERY: Duration = Duration::from_secs(24 * 3600);

/// "1.2 GB", "340.5 MB"
pub(super) fn size(bytes: u64) -> String {
    if bytes >= 1_000_000_000 { format!("{:.2} GB", bytes as f64 / 1e9) } else { format!("{:.1} MB", bytes as f64 / 1e6) }
}

/// the console's line for a sweep: counts only, never a keyphrase
fn vault_line(s: &Sweep, max: u64, seconds: f64) -> String {
    let mut l = format!("[vault] {} keyphrases, {} of {} (counted in {seconds:.1} s)", s.keyphrases, size(s.bytes), size(max));
    if s.removed > 0 {
        l.push_str(&format!("; {} removed after two years unused", s.removed));
    }
    if s.temps > 0 {
        l.push_str(&format!("; {} unfinished writes cleared", s.temps));
    }
    l
}

/// where the routes are on the server's names, when any of them has them under a base (none: everything at the root,
/// as ever; a "*.zone" name has the base of names --base-path does not give)
fn paths_line(o: &Options, names: &[String]) -> Option<String> {
    let mut by: Vec<(&str, Vec<&str>)> = Vec::new();
    for n in names {
        let b = o.bases.of(n);
        match by.iter_mut().find(|(x, _)| *x == b) {
            Some((_, ns)) => ns.push(n),
            None => by.push((b, vec![n])),
        }
    }
    if by.iter().all(|(b, _)| b.is_empty()) {
        return None;
    }
    let rest = if o.game.root_files { "any other path 404 but /robots.txt, which names each game's" } else { "any other path 404" };
    let parts: Vec<String> = by
        .iter()
        .map(|(b, ns)| {
            if b.is_empty() {
                format!("at the root for {}", ns.join(", "))
            } else {
                format!("under {b}/ for {} ({b} redirects there; {rest})", ns.join(", "))
            }
        })
        .collect();
    Some(format!("  paths     : {}", parts.join("; ")))
}

/// a measurement knob from the environment (a whole number), else the default
fn env_num(name: &str, def: u64) -> u64 {
    std::env::var(name).ok().and_then(|v| v.trim().parse().ok()).unwrap_or(def)
}

/// the state folder: made private to this server's user (the files inside are 0600 too)
fn state_folder(o: &Options) -> String {
    if let Err(e) = std::fs::DirBuilder::new().recursive(true).mode(0o700).create(&o.state) {
        die(&format!("{}: {e}", o.state));
    }
    // (only a folder of this user's own is made 0700: not ours to change otherwise; /proc/self is owned by the
    // process's user)
    if let (Ok(st), Ok(me)) = (std::fs::metadata(&o.state), std::fs::metadata("/proc/self"))
        && st.uid() == me.uid()
    {
        let _ = std::fs::set_permissions(&o.state, std::fs::Permissions::from_mode(0o700));
    }
    match std::fs::canonicalize(&o.state) {
        Ok(p) => p.to_string_lossy().into_owned(),
        Err(e) => die(&format!("{}: {e}", o.state)),
    }
}

/// the certificate file's time, for the check once a minute (0 when it cannot be read, as fs.watchFile has it)
async fn cert_mtime(p: &str) -> f64 {
    tokio::fs::metadata(p).await.map(|m| etag::mtime_ms(&m)).unwrap_or(0.0)
}

/// a renewed certificate (certbot, a new Origin CA certificate) is picked up without a restart
async fn reload_cert(app: &Arc<App>, tls: &Arc<TlsState>) {
    let t = tls.clone();
    match tokio::task::spawn_blocking(move || t.reload()).await {
        Ok(Ok(names)) => {
            app.host_check.set_cert_names(&[names.dns, names.ips].concat());
            log::line("certificate reloaded");
        }
        Ok(Err(m)) => log::error(&format!("certificate not reloaded: {m}")),
        Err(e) => log::error(&format!("certificate not reloaded: {e}")),
    }
}

pub async fn run(o: Options, cores: usize) {
    let real_state = state_folder(&o);
    // the console kept in the state folder too, from its first line (a game whose box keeps it: game.rs keeps_log)
    if o.game.keeps_log {
        let dir = paths::join(&o.state, "log");
        if let Err(e) = log::keep(Path::new(&dir)) {
            die(&format!("{dir}: {e}"));
        }
    }
    if !o.artifacts.is_empty()
        && let Err(e) = std::fs::create_dir_all(&o.artifacts)
    {
        die(&format!("{}: {e}", o.artifacts));
    }
    // ETags without file-system detail: a keyed hash (the key never leaves this machine)
    let etag_key = etag::load_key(&o.state).unwrap_or_else(|e| die(&format!("{}/etag.key: {e}", o.state)));

    // ---- /lan: the switch, the game's Online mode at 10.77.0.1 and free roam's hub at 10.77.0.253 ----
    let trace = if o.online_trace.is_empty() {
        None
    } else {
        Some(LobbyTrace::open(Path::new(&o.online_trace)).unwrap_or_else(|e| die(&format!("{}: {e}", o.online_trace))))
    };
    let tap = trace.as_ref().map(|t| t.tap());
    // the lines a client causes in Online mode (logins, games, races, friends) count against that client's share of
    // the console, as its joins and refusals do (the Online audit's ONL-8)
    // claimed names (nfsu2-claims): with keyphrase saves only (a name is claimed by a keyphrase)
    let claims =
        (o.vault_gb > 0.0 && o.game.own_vault).then(|| Arc::new(nfsu2_claims::Claims::open(Path::new(&o.state), nfsu2_util::now_ms())));
    // the operator's bans, and the control socket that makes them (control.rs); the hosts' [cheat] lines of races get
    // a handle that `ban` takes for a week (control.rs Marker)
    let bans = Arc::new(nfsu2_bans::Bans::open(Path::new(&o.state)).unwrap_or_else(|e| die(&format!("{}/bans.json: {e}", o.state))));
    let marker = super::control::Marker::new(bans.clone());
    let online = o.game.online.filter(|_| o.online).map(|title| {
        let m = marker.clone();
        OnlineHost::spawn_for(
            title,
            OnlineOptions {
                state_dir: Some(PathBuf::from(paths::join(&o.state, "online"))),
                log: Arc::new(move |line: &str, key: Option<&str>| {
                    let tagged = m.tag(line);
                    let line = tagged.as_deref().unwrap_or(line);
                    match key {
                        None => log::line(line),
                        Some(k) => log::limited("online", "Online lines", k, line, 30),
                    }
                }),
                tap: tap.clone(),
                claims: claims.clone(),
            },
        )
    });
    // (its lines: the cap on the players' links going down while the server is busy and back up, at most one every 5 s)
    let m = marker.clone();
    let roam_log = Arc::new(move |line: &str| log::line(m.tag(line).as_deref().unwrap_or(line)));
    // (the players of this server's game only: PLAN D-18)
    let roam_o = RoamOptions { game: o.game.roam_game, ..RoamOptions::default() };
    let roam = o.roam_hub.then(|| RoamHub::spawn_with(roam_o, roam_log, claims.clone()));
    // keyphrase saves: a folder in the state folder (none with --vault-gb 0, and none for a game whose saves are the U2
    // box's: its pages' /vault goes on there, vault_forward.rs)
    let vault = (o.vault_gb > 0.0 && o.game.own_vault).then(|| {
        let dir = paths::join(&o.state, "vault");
        Arc::new(Vault::open(VaultOptions::sized(PathBuf::from(&dir), o.vault_gb)).unwrap_or_else(|e| die(&format!("{dir}: {e}"))))
    });
    // the hand-over store: a folder in the state folder, where a name has its routes under a base (covers.rs)
    let covers = super::covers::open(&o).unwrap_or_else(|e| die(&format!("{}: {e}", paths::join(&o.state, "covers"))));
    let lan_sndbuf = env_num("NFSU2_LAN_SNDBUF", LAN_SNDBUF) as usize;
    // the bundle clock's tick, 50 ms (nfsu2-lan); NFSU2_BUNDLE_MS another (0: none), for measurements
    let d = LanOptions::default();
    let fixed = std::env::var_os("NFSU2_BUNDLE_MS").is_some().then(|| env_num("NFSU2_BUNDLE_MS", d.bundle_ms));
    let lan_options = LanOptions {
        max_peers: o.max_peers,
        max_payload: limits::WS_MAX_PAYLOAD,
        bundle_ms: fixed.unwrap_or(d.bundle_ms),
        bundle_max_ms: fixed.unwrap_or_else(|| env_num("NFSU2_BUNDLE_MAX_MS", d.bundle_max_ms)),
        bundle_budget: env_num("NFSU2_BUNDLE_BUDGET", d.bundle_budget),
        bundle_jitter: env_num("NFSU2_BUNDLE_JITTER", (d.bundle_jitter * 100.0) as u64) as f64 / 100.0,
        fpred: env_num("NFSU2_FPRED", 1) != 0,
        send_buffer_os: lan_sndbuf,
        ..d
    };
    // the switch's addresses outlive a restart (by the hashes of their tokens): the players come back to them
    let lan = LanSwitch::new(
        lan_options,
        if o.dev { &o.lan_impair } else { "" },
        online.clone().map(|h| h as Arc<dyn Host>),
        roam.clone().map(|h| h as Arc<dyn Host>),
        tap,
        Some(PathBuf::from(paths::join(&o.state, "lan-addresses.json"))),
    );
    if let Some(h) = &online {
        h.attach_switch(lan.switch_send());
    }
    if let Some(h) = &roam {
        h.attach_switch(lan.switch_send());
    }
    marker.set_switch(lan.clone());

    // Cloudflare's check for a claim (turnstile.rs: off without its two keys)
    let turnstile = super::turnstile::Turnstile::from_env();
    if let Some(c) = &claims {
        log::line(&format!(
            "[names] {} names claimed; Cloudflare's check {}",
            c.count(nfsu2_util::now_ms()),
            if turnstile.is_some() { "on" } else { "off (no NFSU2_TURNSTILE_SITE and NFSU2_TURNSTILE_SECRET)" }
        ));
    }
    let names = claims.clone().map(|c| super::names::NamesRoute::new(c, turnstile));
    let mut app = App::new(
        o,
        real_state,
        etag_key,
        lan.clone(),
        online,
        roam.clone(),
        vault.clone(),
        covers.clone(),
        names,
        bans,
        (cores / 2).max(2),
        lan_sndbuf,
        // the threads that keep the game data's blocks at brotli's best (kept.rs; 0: none)
        env_num("NFSU2_COMPRESS_THREADS", COMPRESS_THREADS) as usize,
    );
    // the box-to-box keyphrase saves (vault_forward.rs): a game without its own forwards to the U2 box's; the U2 box's
    // takes the forwards that carry the credential.  The keys from the environment (never a log line)
    if app.o.game.own_vault {
        app.vault_box_key = std::env::var("NFSU2_VAULT_BOX_KEY").ok().map(|k| k.trim().to_string()).filter(|k| !k.is_empty());
        if app.vault_box_key.as_ref().is_some_and(|k| k.len() < super::vault_forward::MIN_KEY) {
            die(&format!("NFSU2_VAULT_BOX_KEY: at least {} characters", super::vault_forward::MIN_KEY));
        }
    } else {
        app.vault_forward = super::vault_forward::VaultForward::from_env().unwrap_or_else(|e| die(&e));
    }
    let app = Arc::new(app);
    let o = &app.o;
    let control = super::control::listen(app.clone());

    // the switch's players, every 5 minutes while any are on
    {
        let lan = lan.clone();
        tokio::spawn(async move {
            let mut shown = false;
            let mut thinned = 0;
            let mut was = nfsu2_lan::Breakdown::default();
            let (mut cpu_was, mut workers, mut quota) =
                (process_cpu_seconds(), WorkersBusy::new(&tokio::runtime::Handle::current()), QuotaBusy::find());
            let mut every = interval_at(Instant::now() + BREAKDOWN_EVERY, BREAKDOWN_EVERY);
            loop {
                every.tick().await;
                // the CPU of the last 5 minutes: the server's, its workers' busy time, and the fullest quota's (the
                // game's containers' together: nfsu2.slice)
                let cpu = process_cpu_seconds();
                let server = (cpu - cpu_was) / BREAKDOWN_EVERY.as_secs_f64();
                let (busy, slice) = (workers.sample(), quota.as_mut().and_then(QuotaBusy::sample_use));
                cpu_was = cpu;
                let b = lan.breakdown().await;
                if b.players == 0 && !shown {
                    continue;
                }
                shown = b.players > 0;
                // how many are behind on their connection (its queues past the server: the kernel's, cloudflared's,
                // the tunnel's) and get fewer of the other cars' updates meanwhile (nfsu2-lan lag.rs)
                log::line(&format!(
                    "[players] {} on the switch: {} in Online mode, {} on the LAN or Online screens, {} in free roam, {} on an older page \
                     (until its tab closes), {} leaving or starting; {} of all of them bundling (a page loaded since the server has \
                     bundles); {} more than a second behind on their connection, {} getting fewer car updates ({} left out since the last count)",
                    b.players,
                    b.online,
                    b.lan,
                    b.roam,
                    b.older,
                    b.other,
                    b.bundles,
                    b.behind,
                    b.thinning,
                    b.thinned - thinned
                ));
                // the line (nfsu2-lan queue.rs): pages waiting for a place, and how long the longest of those let in
                // waited
                if b.waiting > 0 || b.waited > 0 {
                    log::line(&format!(
                        "[players] {} waiting in line for a place; the longest wait of those let in since the line began {} min",
                        b.waiting,
                        b.waited.div_ceil(60)
                    ));
                }
                // what free roam sent the pages in compact bundles since the last count: friends' car states (by their
                // fields, nfsu2-lan fpred.rs, or as a byte delta) and the rest
                let (sb, se, sf) = (b.snap_bytes - was.snap_bytes, b.snap_entries - was.snap_entries, b.fpred_entries - was.fpred_entries);
                let (ob, oe) = (b.other_bytes - was.other_bytes, b.other_entries - was.other_entries);
                if se + oe > 0 {
                    log::line(&format!(
                        "[players] to the pages in compact bundles since the last count: {:.1} MB in {se} car states ({:.1} bytes each, \
                         {sf} of them by their fields), {:.1} MB in {oe} other datagrams",
                        sb as f64 / 1e6,
                        sb as f64 / se.max(1) as f64,
                        ob as f64 / 1e6
                    ));
                }
                let slice = slice.map_or(String::new(), |q| {
                    format!("; {} {:.2} of its {} cores ({:.0} %)", q.group, q.used, q.cores, q.share() * 100.0)
                });
                log::line(&format!(
                    "[players] CPU in the last 5 minutes: the server {server:.2} cores, its switch's workers {:.0} % busy{slice}",
                    busy * 100.0
                ));
                thinned = b.thinned;
                was = b;
            }
        });
    }
    // the game data's answers, every 5 minutes while there were any (blocks.rs Sent)
    {
        let app = app.clone();
        tokio::spawn(async move {
            let mut every = interval_at(Instant::now() + BREAKDOWN_EVERY, BREAKDOWN_EVERY);
            loop {
                every.tick().await;
                let sent = app.sent.take();
                if !sent.is_empty() {
                    log::line(&format!("[blocks] in the last 5 minutes: {sent}"));
                }
                if let Some(l) = app.vault_forward.as_ref().and_then(|f| f.line()) {
                    log::line(&l);
                }
            }
        });
    }
    // the /lan openers' buckets that are full again hold nothing worth keeping
    {
        let app = app.clone();
        tokio::spawn(async move {
            let mut every = interval_at(Instant::now() + Duration::from_secs(60), Duration::from_secs(60));
            loop {
                every.tick().await;
                app.lan_opens.prune();
                if let Some(v) = &app.vault {
                    v.prune();
                }
                if let Some(c) = &app.covers {
                    c.prune();
                }
                // the claimed names: the ended ones go, names.json written when anything changed
                if let Some(n) = &app.names {
                    n.prune();
                    let now = nfsu2_util::now_ms();
                    n.claims.prune(now);
                    let c = n.claims.clone();
                    if let Ok(Err(e)) = tokio::task::spawn_blocking(move || c.save(now)).await {
                        log::error(&format!("names.json: {e}"));
                    }
                }
            }
        });
    }

    // ---- listening ----
    let limits = Arc::new(Limits { max_connections: limits::max_connections(o.max_peers), ..Limits::default() });
    let mut servers = Vec::new();
    let mut bound_to = o.bind.clone();
    if o.tls == Tls::Cert {
        // A --tls-cert certificate: its names (SAN), for the Host check and the handshake, which goes through only for a
        // server name (SNI) the certificate covers, so a scanner that connects to the bare address, or asks for any
        // other name, never sees the certificate or the domain in it (the audit's F4).  With Cloudflare's ranges trusted,
        // a peer there must show Cloudflare's origin-pull certificate (App::client_certificate).
        let (cert, key) = (Path::new(&o.tls_cert), Path::new(&o.tls_key));
        let loaded = if o.trust.cloudflare {
            TlsState::load_with_client_ca(cert, key, nfsu2_headers::CLOUDFLARE_ORIGIN_PULL_CA)
        } else {
            TlsState::load(cert, key)
        };
        let (tls, names) = match loaded {
            Ok(t) => t,
            Err(m) => die(&format!("the certificate files cannot be used: {m}")),
        };
        app.host_check.set_cert_names(&[names.dns, names.ips].concat());
        let (listener, host) = bind(&o.bind, o.https_port, if o.bind_set { None } else { Some("0.0.0.0") })
            .await
            .unwrap_or_else(|m| die(&format!("cannot listen: {m}")));
        bound_to = host;
        servers.push(serve(listener, Some(tls.clone()), limits.clone(), app.clone()));
        // the certificate file, checked once a minute, and SIGHUP
        {
            let (app, tls, cert) = (app.clone(), tls.clone(), o.tls_cert.clone());
            tokio::spawn(async move {
                let mut last = cert_mtime(&cert).await;
                let mut every = interval_at(Instant::now() + Duration::from_secs(60), Duration::from_secs(60));
                loop {
                    every.tick().await;
                    let cur = cert_mtime(&cert).await;
                    if cur != last {
                        last = cur;
                        reload_cert(&app, &tls).await;
                    }
                }
            });
        }
        {
            let (app, tls) = (app.clone(), tls.clone());
            let mut hup = signal(SignalKind::hangup()).unwrap_or_else(|e| die(&format!("SIGHUP: {e}")));
            tokio::spawn(async move {
                while hup.recv().await.is_some() {
                    reload_cert(&app, &tls).await;
                }
            });
        }
    }
    if o.http_port != 0 {
        let (listener, _) = bind("127.0.0.1", o.http_port, None).await.unwrap_or_else(|m| die(&format!("cannot listen: {m}")));
        servers.push(serve(listener, None, limits.clone(), app.clone()));
    }
    if let Err(e) = app.refresh_manifest().await {
        die(&format!("the game folder {}: {e}", o.data));
    }
    // ready before the first player asks
    {
        let app = app.clone();
        tokio::spawn(async move {
            app.compressed_wasm().await;
        });
    }

    // SIGTERM, SIGINT: the switch saves its addresses, the hosts stop, the claimed names are written, the control
    // socket's file goes, then the process ends
    let control_up = control.is_some();
    {
        let (lan, trace, claims) = (lan.clone(), trace.clone(), claims.clone());
        let mut term = signal(SignalKind::terminate()).unwrap_or_else(|e| die(&format!("SIGTERM: {e}")));
        let mut int = signal(SignalKind::interrupt()).unwrap_or_else(|e| die(&format!("SIGINT: {e}")));
        tokio::spawn(async move {
            loop {
                tokio::select! {
                    _ = term.recv() => {}
                    _ = int.recv() => {}
                }
                lan.stop();
                if let Some(t) = &trace {
                    t.close();
                }
                if let Some(c) = &claims
                    && let Err(e) = c.save(nfsu2_util::now_ms())
                {
                    log::error(&format!("names.json: {e}"));
                }
                if let Some(c) = &control {
                    c.remove();
                }
                for s in &servers {
                    s.abort();
                }
                tokio::spawn(async {
                    tokio::time::sleep(Duration::from_millis(200)).await;
                    std::process::exit(0);
                });
            }
        });
    }

    let here = if cfg!(target_os = "macos") { "this Mac" } else { "this host" };
    let mut names = vec!["localhost".to_string()];
    for n in app.host_check.names() {
        if !names.contains(&n) {
            names.push(n);
        }
    }
    let mut lines = vec![
        o.game.title.to_string(),
        format!("  game data : {} ({} entries of the game's own)", o.data, app.manifest_count()),
        format!(
            "  game.wasm : {}",
            if !Path::new(&o.wasm).exists() {
                format!("{}  (missing: run {})", o.wasm, o.game.build)
            } else {
                // a module of another game, a dev build without --dev, or one without a json that is a JSON object
                // (Underground 2's aside): not served (game.rs module_refusal)
                match o.game.module_refusal(&ModuleJson::read(std::fs::read(&o.game_json)), o.dev) {
                    Some(r) => format!("{}  (not served: {})", o.wasm, r.why),
                    None => o.wasm.clone(),
                }
            }
        ),
        format!(
            "  {here:<10}: {}{}",
            if o.http_port != 0 { format!("http://localhost:{}/", o.http_port) } else { "(no local HTTP port)".into() },
            if o.tls == Tls::Proxy { "  (the proxy forwards here)" } else { "" }
        ),
    ];
    match o.tls {
        Tls::None => lines
            .push("  other PCs : none: they need HTTPS, from --tls-cert/--tls-key or a proxy or tunnel in front (docs/web/VPS.md)".into()),
        Tls::Cert => lines.push(format!(
            "  HTTPS     : port {} on {}, certificate {}",
            o.https_port,
            if bound_to == "::" { ":: (IPv6 and IPv4)".to_string() } else { bound_to.clone() },
            o.tls_cert
        )),
        Tls::Proxy => {
            lines.push("  TLS       : done by the proxy or tunnel in front (it forwards to the address above, WebSockets included)".into())
        }
    }
    lines.push(format!(
        "  proxies   : this machine{} (client address from {})",
        if o.trust.entries.is_empty() { String::new() } else { format!(", {}", o.trust.entries.join(", ")) },
        o.real_ip
    ));
    if o.trust.cloudflare && o.tls == Tls::Cert {
        lines.push(
            "  cloudflare: its addresses must show its origin-pull certificate (turn on Authenticated Origin Pulls), or their handshakes fail"
                .into(),
        );
    }
    lines.push(format!("  names     : {} (other Host names get 421)", names.join(", ")));
    if let Some(l) = paths_line(o, &names) {
        lines.push(l);
    }
    lines.push(format!("  switch    : at most {} players", o.max_peers));
    lines.push(format!(
        "  free roam : {}",
        if o.roam_hub {
            "the hub at 10.77.0.253 on the switch links each player to the 32 nearest (--roam-hub off: groups of 33 by broadcast)"
        } else {
            "no hub (--roam-hub off): players find each other by broadcast, groups of 33"
        }
    ));
    if o.roam_hub {
        // the quota the hub's busy cap reads besides the workers (nfsu2_util::cpu::QuotaBusy)
        let quotas = QuotaBusy::find().map(|q| q.quotas()).unwrap_or_default();
        lines.push(match quotas.iter().min_by(|a, b| a.1.total_cmp(&b.1)) {
            Some((group, cores)) => {
                format!("  CPU quota : {cores} cores for {group}; over 85 % of them in use, free roam gives fewer friends")
            }
            None => {
                "  CPU quota : none seen (a container sees its slice's with cgroup: host); free roam's busy cap reads the workers".into()
            }
        });
    }
    lines.push(format!(
        "  online    : {}",
        if o.online {
            format!(
                "the game's Online mode at 10.77.0.1 on the switch (any name logs in; race results per persona in {})",
                paths::join(&o.state, "online")
            )
        } else if o.game.online.is_none() {
            format!("off (this server has no Online mode for {} yet)", o.game.name)
        } else {
            "off (--online off)".into()
        }
    ));
    lines.push(format!(
        "  saves     : {}",
        match &app.vault {
            Some(v) => {
                let vo = v.store.options();
                format!(
                    "keyphrase saves (/vault) in {}, up to {} (new keyphrases until {}, growing {} a day at most); one \
                     unused for two years goes",
                    vo.dir.display(),
                    size(vo.max_bytes),
                    size(vo.new_until),
                    size(vo.per_day)
                )
            }
            None => match &app.vault_forward {
                Some(f) => format!("keyphrase saves forwarded to {}", f.target()),
                None if !app.o.game.own_vault => "no keyphrase saves (none forwarded: NFSU2_VAULT_FORWARD)".into(),
                None => "no keyphrase saves (--vault-gb 0)".into(),
            },
        }
    ));
    if let Some(c) = &app.covers {
        let co = c.store.options();
        lines.push(format!(
            "  covers    : the hand-over store (/covers under {}) in {}, {} an entry, up to {} in all; one nobody takes \
             goes after 30 days",
            o.bases.all(|_| true).into_iter().filter(|b| !b.is_empty()).collect::<Vec<_>>().join(", "),
            co.dir.display(),
            size(co.max_entry as u64),
            size(co.max_bytes)
        ));
    }
    lines.push(format!(
        "  blocks    : {}",
        app.kept.as_ref().map_or_else(|| "compressed as they are sent (NFSU2_COMPRESS_THREADS=0)".into(), |k| k.summary())
    ));
    let bans = app.bans.list(nfsu2_util::now_ms()).len();
    // (ctl goes to UG2's state folder unless told the game: ctl.rs)
    let g = if o.game.id == "ug2" { String::new() } else { format!("--game {} ", o.game.id) };
    lines.push(if control_up {
        format!(
            "  bans      : {bans}; nfsu2-server ctl {g}who, kick, ban, unban, bans (through {})",
            paths::join(&o.state, super::control::SOCKET)
        )
    } else {
        format!("  bans      : {bans}; no kick or ban (the control socket could not be made: above)")
    });
    if o.game.keeps_log {
        lines.push(format!(
            "  log       : these lines also in {} (each after its time; past {} MB it turns over, {} files at most), \
             kept past restarts and releases",
            paths::join(&paths::join(&o.state, "log"), &log::kept_name(0)),
            log::KEEP_BYTES >> 20,
            log::KEEP_FILES
        ));
    }
    if !o.online_trace.is_empty() {
        lines.push(format!("  trace     : lobby messages (secrets redacted) into {}", o.online_trace));
    }
    if o.dev {
        lines.push(format!(
            "  dev       : test pages (/test/), test scripts (/tests/), source maps{}",
            if o.artifacts.is_empty() { String::new() } else { format!(", PUT /artifacts/<name>.png into {}", o.artifacts) }
        ));
    }
    lines.push("  access    : none: whoever reaches this server may play (the firewall decides who that is)".into());
    log::line(&lines.join("\n"));
    // keyphrase saves: counted (the caps wait for it) and swept now (its line after the start-up lines), then daily; a
    // count that stopped is tried again in ten minutes, not a day (until one ends, the caps wait)
    if let Some(v) = vault {
        tokio::spawn(async move {
            loop {
                let (t0, st) = (std::time::Instant::now(), v.clone());
                let failed = match tokio::task::spawn_blocking(move || st.sweep(nfsu2_util::now_ms())).await {
                    Ok(Ok(s)) => {
                        log::line(&vault_line(&s, v.options().max_bytes, t0.elapsed().as_secs_f64()));
                        false
                    }
                    Ok(Err(e)) => {
                        log::error(&format!("[vault] the count of the keyphrase saves stopped: {e}"));
                        true
                    }
                    Err(e) => {
                        log::error(&format!("[vault] the count of the keyphrase saves stopped: {e}"));
                        true
                    }
                };
                tokio::time::sleep(if failed { Duration::from_secs(600) } else { VAULT_SWEEP_EVERY }).await;
            }
        });
    }

    // the hand-over store: counted and swept now, then every hour (entries nobody took go after 30 days); its line
    // every 5 minutes (what it holds, and what it did since it was first opened: kept across restarts)
    if let Some(c) = covers {
        let sweeper = c.clone();
        tokio::spawn(async move {
            loop {
                let st = sweeper.clone();
                match tokio::task::spawn_blocking(move || st.sweep(nfsu2_util::now_ms())).await {
                    Ok(Ok(s)) if s.expired > 0 || s.temps > 0 => {
                        log::line(&format!("[move] {} covers entries nobody took went after 30 days", s.expired))
                    }
                    Ok(Ok(_)) => {}
                    Ok(Err(e)) => log::error(&format!("[move] the sweep of the hand-over store stopped: {e}")),
                    Err(e) => log::error(&format!("[move] the sweep of the hand-over store stopped: {e}")),
                }
                tokio::time::sleep(Duration::from_secs(3600)).await;
            }
        });
        tokio::spawn(async move {
            let mut every = interval_at(Instant::now() + Duration::from_secs(300), Duration::from_secs(300));
            loop {
                every.tick().await;
                log::line(&move_line(&c));
            }
        });
    }

    std::future::pending::<()>().await;
}

/// the hand-over store's line: what it holds, and what it did since it was first opened
pub fn move_line(c: &nfsu2_vault::covers::CoverStore) -> String {
    let (n, bytes) = c.usage();
    let k = c.counts();
    format!(
        "[move] covers: {n} entries, {} held; {} stored, {} imported, {} expired, {} refused",
        size(bytes),
        k.stored,
        k.imported,
        k.expired,
        k.refused
    )
}
